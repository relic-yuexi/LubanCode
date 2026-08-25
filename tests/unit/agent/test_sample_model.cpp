// SampleModel 原语(骨架拆解单批一·病四)的单测:usage 统一口径、错误
// 兜底语义、看门狗、半截流文本不丢、output_schema 复检。假 backend,
// 不出网。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "agent/sample_model.hpp"

namespace {

using lubancode::agent::AddSampleAccounting;
using lubancode::agent::BackgroundCallAccounting;
using lubancode::agent::SampleModel;
using lubancode::agent::SampleOptions;
using lubancode::agent::SampleRequest;
using lubancode::agent::SampleResult;

// 全要素假 backend:正文、usage、收尾事件齐发。
class FullBackend final : public lubancode::api::Backend {
public:
    std::string reply = "正文一枚";
    std::int64_t input_tokens = 100;
    std::int64_t output_tokens = 50;
    std::int64_t cache_read_tokens = 7;
    std::int64_t cache_creation_tokens = 3;
    int calls = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        (void)request;
        ++calls;
        on_event(lubancode::api::TextDelta{reply});
        on_event(lubancode::api::ContentBlockDone{0});
        lubancode::api::MessageDone done;
        done.usage.input_tokens = input_tokens;
        done.usage.output_tokens = output_tokens;
        done.usage.cache_read_tokens = cache_read_tokens;
        done.usage.cache_creation_tokens = cache_creation_tokens;
        on_event(done);
        return {};
    }
};

// 发送即败的 backend:半截 usage 与正文先吐,再回错(kind 可配)。
class FailingBackend final : public lubancode::api::Backend {
public:
    lubancode::api::ErrorKind kind = lubancode::api::ErrorKind::Cancelled;
    std::string message = "被取消";

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        on_event(lubancode::api::TextDelta{"半截"});
        lubancode::api::MessageDone done;
        done.usage.input_tokens = 11;
        done.usage.output_tokens = 4;
        on_event(done);
        return std::unexpected(lubancode::api::Error{kind, message, 0});
    }
};

// 流内报错的 backend:send_stream 本身成功,流里夹 StreamError。
class StreamErrorBackend final : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        on_event(lubancode::api::StreamError{"服务端业务错"});
        return {};
    }
};

// 看门狗靶子:吃取消旗,旗不拉就自旋等。
class BlockingBackend final : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&,
        const std::atomic<bool>* cancel) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel != nullptr && cancel->load()) {
                return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Cancelled,
                                                             "看门狗拉旗", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return {};
    }
};

SampleRequest OneShot(const std::string& system, const std::string& user_text) {
    SampleRequest request;
    request.model = "test-model";
    request.system = system;
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{user_text});
    request.messages.push_back(std::move(message));
    request.max_tokens = 64;
    return request;
}

}  // namespace

TEST_SUITE("agent-sample-model") {

TEST_CASE("usage 统一报:五项口径齐、半截也出账、AddSampleAccounting 累加首报") {
    FullBackend backend;
    const SampleResult result = SampleModel(backend, OneShot("指令", "材料"));
    REQUIRE(result.ok);
    CHECK(result.text == "正文一枚");
    CHECK(result.usage.input_tokens == 100);
    CHECK(result.usage.output_tokens == 50);
    CHECK(result.usage.cache_read_tokens == 7);
    CHECK(result.usage.cache_creation_tokens == 3);
    CHECK(result.usage_reported);

    // 两次采样累进一本账:token 相加,reported 只置不撤;没报过的一笔
    // 不把已报的冲回去。
    BackgroundCallAccounting accounting;
    AddSampleAccounting(&accounting, result);
    FullBackend zero_backend;  // 不吐 usage 的第二笔
    zero_backend.input_tokens = 0;
    zero_backend.output_tokens = 0;
    zero_backend.cache_read_tokens = 0;
    zero_backend.cache_creation_tokens = 0;
    const SampleResult second = SampleModel(zero_backend, OneShot("指令", "材料"));
    REQUIRE(second.ok);
    CHECK_FALSE(second.usage_reported);
    AddSampleAccounting(&accounting, second);
    CHECK(accounting.usage.input_tokens == 100);
    CHECK(accounting.usage.output_tokens == 50);
    CHECK(accounting.usage.cache_read_tokens == 7);
    CHECK(accounting.usage.cache_creation_tokens == 3);
    CHECK(accounting.usage_reported);
    // 空指针直接空操作。
    AddSampleAccounting(nullptr, second);
}

TEST_CASE("发送失败:kind 原样保留、半截 usage/正文照带回") {
    FailingBackend backend;
    const SampleResult result = SampleModel(backend, OneShot("指令", "材料"));
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == lubancode::api::ErrorKind::Cancelled);
    CHECK(result.error.message == "被取消");
    // 旧口径:六处都是先记账再判错——半截流的账不许丢。
    CHECK(result.usage.input_tokens == 11);
    CHECK(result.usage.output_tokens == 4);
    CHECK(result.usage_reported);
    CHECK(result.text == "半截");
}

TEST_CASE("流内错:折成 Api + 原消息") {
    StreamErrorBackend backend;
    const SampleResult result = SampleModel(backend, OneShot("指令", "材料"));
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == lubancode::api::ErrorKind::Api);
    CHECK(result.error.message == "服务端业务错");
}

TEST_CASE("看门狗:到点拉旗,发送被取消打断") {
    BlockingBackend backend;
    SampleOptions options;
    options.timeout_secs = 1;
    const auto started = std::chrono::steady_clock::now();
    const SampleResult result = SampleModel(backend, OneShot("指令", "材料"), options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == lubancode::api::ErrorKind::Cancelled);
    // 一秒看门狗 + 100ms 轮询粒度:不许秒回(没等就报)也不许挂死。
    CHECK(elapsed >= std::chrono::milliseconds(850));
    CHECK(elapsed < std::chrono::milliseconds(5000));
}

TEST_CASE("外部取消链在场:直接吃外部旗,不看门狗也不误伤") {
    BlockingBackend backend;
    std::atomic<bool> cancel{false};
    SampleOptions options;
    options.cancel = &cancel;
    std::thread flip([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        cancel = true;
    });
    const SampleResult result = SampleModel(backend, OneShot("指令", "材料"), options);
    flip.join();
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == lubancode::api::ErrorKind::Cancelled);
}

TEST_CASE("output_schema 复检:过/不过两态,不影响 ok") {
    FullBackend ok_backend;
    ok_backend.reply = "{\"decision\": \"achieved\"}";
    SampleRequest request = OneShot("指令", "材料");
    request.output_schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"decision", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"decision"})}};
    const SampleResult good = SampleModel(ok_backend, request);
    REQUIRE(good.ok);
    CHECK(good.schema_ok);
    CHECK(good.schema_error.empty());

    FullBackend bad_backend;  // 正文不是 JSON
    bad_backend.reply = "不是 JSON 的白话";
    const SampleResult ugly = SampleModel(bad_backend, request);
    REQUIRE(ugly.ok);  // 采样成了,复检只标记不翻案
    CHECK_FALSE(ugly.schema_ok);
    CHECK_FALSE(ugly.schema_error.empty());

    // 缺必填字段也算不过。
    FullBackend missing_backend;
    missing_backend.reply = "{\"other\": 1}";
    const SampleResult missing = SampleModel(missing_backend, request);
    REQUIRE(missing.ok);
    CHECK_FALSE(missing.schema_ok);

    // 不带 schema:复检恒过。
    FullBackend plain_backend;
    plain_backend.reply = "白话也行";
    const SampleResult plain = SampleModel(plain_backend, OneShot("指令", "材料"));
    REQUIRE(plain.ok);
    CHECK(plain.schema_ok);
    CHECK(plain.schema_error.empty());
}

}  // TEST_SUITE(agent-sample-model)
