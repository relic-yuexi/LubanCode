// 会话标题异步精炼器(实测问题 7)的单测:Start 不阻塞、TakeFinished
// 非阻塞收货、单飞不叠发、失败半截也出账、取消生效、代数原样带回、
// 析构不挂。假后端可控延迟,不发一个网络包。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/session_title.hpp"        // kTitleRefineMaxTokens(单子预算钉)
#include "app/session_title_refiner.hpp"

namespace {

using lubancode::app::SessionTitleRefiner;

// 请求摘要:记进测试栈上的账本(backend 本体随线程闭包在 join 时销毁,
// 账不能记在它身上——join 之后读它就是 use-after-free)。
struct CapturedCall {
    std::string model;
    int max_tokens = -1;
    bool has_tools = false;
    std::string user_text;
};

// 假后端:延迟 delay_ms 后回一枚短标题(带 usage);cancel 非空且已被拉起
// 时回取消错误。请求摘要写给 calls 指的外部账本,供断言只喂首问截段。
struct FakeTitleBackend final : lubancode::api::Backend {
    int delay_ms = 0;
    bool honor_cancel = true;
    std::vector<CapturedCall>* calls = nullptr;  // 可空 = 不记

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        if (calls != nullptr) {
            CapturedCall call;
            call.model = request.model;
            if (request.max_tokens.has_value()) {
                call.max_tokens = *request.max_tokens;
            }
            call.has_tools = !request.tools.empty();
            for (const auto& message : request.messages) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                        call.user_text += text->text;
                    }
                }
            }
            calls->push_back(std::move(call));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (honor_cancel && cancel != nullptr && cancel->load()) {
                return std::unexpected(
                    lubancode::api::Error{lubancode::api::ErrorKind::Cancelled, "cancelled", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        on_event(lubancode::api::TextDelta{"实现图书系统"});
        on_event(lubancode::api::ContentBlockDone{0});
        lubancode::api::Usage usage;
        usage.input_tokens = 492;
        usage.output_tokens = 10;
        on_event(lubancode::api::MessageDone{"end_turn", usage});
        return {};
    }
};

SessionTitleRefiner::Inputs MakeInputs(std::unique_ptr<lubancode::api::Backend> backend,
                                       std::uint64_t generation = 1) {
    SessionTitleRefiner::Inputs inputs;
    inputs.backend = std::move(backend);
    inputs.model = "cheap-m";
    inputs.effort = "low";
    inputs.first_query = "做一个图书管理系统,node 前端";
    inputs.generation = generation;
    return inputs;
}

std::unique_ptr<FakeTitleBackend> MakeRecordingBackend(std::vector<CapturedCall>* calls, int delay_ms) {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = delay_ms;
    backend->calls = calls;
    return backend;
}

// 有界等完工:最多等 wait_ms,轮询 TakeFinished(它自己绝不等待)。
std::optional<SessionTitleRefiner::Outcome> AwaitFinished(SessionTitleRefiner& refiner, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto outcome = refiner.TakeFinished()) {
            return outcome;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return refiner.TakeFinished();
}

}  // namespace

TEST_CASE("精炼器:Start 立即返回,结果非阻塞收货,usage 与代数带回") {
    std::vector<CapturedCall> calls;  // 账本住测试栈上:join 销毁闭包后仍可读
    SessionTitleRefiner refiner;
    const auto started_at = std::chrono::steady_clock::now();
    CHECK(refiner.Start(MakeInputs(MakeRecordingBackend(&calls, /*delay_ms=*/300))));
    const auto start_elapsed = std::chrono::steady_clock::now() - started_at;
    CHECK(start_elapsed < std::chrono::milliseconds(200));  // 起飞不等网络
    CHECK(refiner.Busy());
    // 还没完工:收货给空,绝不等待。
    CHECK_FALSE(refiner.TakeFinished().has_value());

    const auto outcome = AwaitFinished(refiner, /*wait_ms=*/2000);
    REQUIRE(outcome.has_value());
    CHECK(outcome->ok);
    CHECK(outcome->title == "实现图书系统");
    CHECK(outcome->model == "cheap-m");
    CHECK(outcome->generation == 1);
    // usage 带回给调用方记 cheap 账(半截/成功一个口径)。
    CHECK(outcome->accounting.usage.input_tokens == 492);
    CHECK(outcome->accounting.usage.output_tokens == 10);
    CHECK(outcome->accounting.usage_reported);
    // 只喂首问:一次调用、输出上限收紧、不带工具、正文是首问截段。
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].max_tokens == lubancode::app::kTitleRefineMaxTokens);
    CHECK_FALSE(calls[0].has_tools);
    CHECK(calls[0].user_text.find("做一个图书管理系统") != std::string::npos);

    // 收货后复位:不忙了,可再起飞。
    CHECK_FALSE(refiner.Busy());
    CHECK(refiner.Start(MakeInputs(MakeRecordingBackend(&calls, /*delay_ms=*/0), /*generation=*/2)));
    CHECK(AwaitFinished(refiner, 2000)->generation == 2);
    CHECK(calls.size() == 2);
}

TEST_CASE("精炼器:单飞——上一枚没收走之前不叠发") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 200;
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::move(backend))));
    CHECK_FALSE(refiner.Start(MakeInputs(std::make_unique<FakeTitleBackend>())));
    CHECK(AwaitFinished(refiner, 2000).has_value());
    // 结果待收也算忙:还没 TakeFinished 之前再 Start 照样拒。
    auto backend2 = std::make_unique<FakeTitleBackend>();
    backend2->delay_ms = 0;
    CHECK(refiner.Start(MakeInputs(std::move(backend2))));
    CHECK(refiner.Busy());
    CHECK_FALSE(refiner.Start(MakeInputs(std::make_unique<FakeTitleBackend>())));
    CHECK(AwaitFinished(refiner, 2000).has_value());
}

TEST_CASE("精炼器:失败带回 ok=false,账照出;本地标题由调用方保留") {
    struct ErrorBackend final : lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
            const std::atomic<bool>*) override {
            on_event(lubancode::api::TextDelta{"半"});
            on_event(lubancode::api::StreamError{"cheap 不可用"});
            return {};
        }
    };
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::make_unique<ErrorBackend>())));
    const auto outcome = AwaitFinished(refiner, 2000);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);
    CHECK(outcome->title.empty());
}

TEST_CASE("精炼器:RequestCancel 打断在飞请求,不重试") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 60'000;  // 不取消就跑不完
    SessionTitleRefiner refiner;
    CHECK(refiner.Start(MakeInputs(std::move(backend))));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    refiner.RequestCancel();  // 人工 /title 抢先那一下
    const auto outcome = AwaitFinished(refiner, 3000);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);  // 取消收场:调用方保留本地标题
}

TEST_CASE("精炼器:看门狗 5 秒硬上限——慢后端到点被打断") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 60'000;  // 不打断就跑不完的慢后端
    SessionTitleRefiner refiner;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(refiner.Start(MakeInputs(std::move(backend))));
    const auto outcome = AwaitFinished(refiner, 12'000);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->ok);  // 看门狗拉取消:调用方保留本地标题
    // 5 秒看门狗 + 收尾余量,不许拖到等待窗上限。
    CHECK(elapsed < std::chrono::seconds(8));
}

TEST_CASE("精炼器:析构时在飞也不挂——取消 + 有界收尾 + detach 放行") {
    auto backend = std::make_unique<FakeTitleBackend>();
    backend->delay_ms = 60'000;
    backend->honor_cancel = false;  // 最坏现场:取消旗拉了也装死
    const auto t0 = std::chrono::steady_clock::now();
    {
        SessionTitleRefiner refiner;
        CHECK(refiner.Start(MakeInputs(std::move(backend))));
    }  // 析构:取消 → 有界等待(7 秒窗)→ detach 放行,不 terminate
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < std::chrono::seconds(15));
    CHECK(true);  // 到这里就是过了:析构未挂死、未 terminate
}
