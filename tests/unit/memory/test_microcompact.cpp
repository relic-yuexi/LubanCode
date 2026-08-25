// artifact 按需摘要的单测:摘要 JSON 解析(好/坏/残次),以及假 backend
// 的完整请求路(输入来自 blob 原文、失败不删原文)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agent/artifact_store.hpp"
#include "agent/context_events.hpp"
#include "agent/microcompact.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

class TempStoreDir {
public:
    explicit TempStoreDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~TempStoreDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class FakeBackend : public api::Backend {
public:
    std::vector<api::StreamEvent> script;
    bool fail = false;
    bool wait_for_cancel = false;
    std::string fail_message;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        captured_requests.push_back(request);
        if (wait_for_cancel) {
            while (cancel != nullptr && !cancel->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::unexpected(api::Error{api::ErrorKind::Network, "cancelled", 0});
        }
        if (fail) {
            return std::unexpected(api::Error{api::ErrorKind::Network, fail_message, 0});
        }
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> ReplyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "cheap-m"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::string Log(std::size_t lines, const std::string& mark = "ok") {
    std::string out;
    for (std::size_t i = 1; i <= lines; ++i) {
        out += "build step " + std::to_string(i) + " " + mark + "\n";
    }
    return out;
}

// 造一场历史,把两枚超长 read_file 结果落成 Artifact。RunMicrocompact
// 只收调用方点名的 ArtifactRef,不会自行扫描冷区。
struct Fixture {
    std::vector<api::Message> history;
    agent::ResultViewMemo memo;
    std::unique_ptr<agent::ContextArtifactStore> store;
    std::string cold_content;
    std::string hot_content;

    Fixture(const std::filesystem::path& root, std::size_t cold_lines = 2200, std::size_t hot_lines = 700)
        : cold_content(Log(cold_lines)), hot_content(Log(hot_lines, "HOT")) {
        store = std::make_unique<agent::ContextArtifactStore>();
        REQUIRE(store->Open((root / "ctx").string(), "sess-1"));

        api::Message turn1 = UserText("第一轮");
        api::Message use1 = ToolUse("toolu-cold", "read_file", "a.log");
        api::Message res1 = ToolResult("toolu-cold", cold_content);
        api::Message turn2 = UserText("第二轮");
        api::Message use2 = ToolUse("toolu-hot", "read_file", "b.log");
        api::Message res2 = ToolResult("toolu-hot", hot_content);
        history = {turn1, use1, res1, turn2, use2, res2};

        // 现场定形:两枚都判成 Artifact 并落盘。按需摘要随后拿稳定 id
        // 找回 blob 真本,不从这份预览取输入。
        agent::StructuralCompressionOptions options;
        options.long_result_bytes = 4096;
        agent::StructuralCompressionStats stats;
        (void)agent::CompressWorkingView(history, options, stats, memo, store.get());
    }

    static api::Message UserText(const std::string& text) {
        api::Message m;
        m.role = api::Role::User;
        m.content.push_back(api::TextBlock{text});
        return m;
    }
    static api::Message ToolUse(const std::string& id, const std::string& name, const std::string& path) {
        api::Message m;
        m.role = api::Role::Assistant;
        m.content.push_back(api::ToolUseBlock{id, name, nlohmann::json{{"path", path}}});
        return m;
    }
    static api::Message ToolResult(const std::string& tool_use_id, const std::string& content) {
        api::Message m;
        m.role = api::Role::User;
        m.content.push_back(api::ToolResultBlock{tool_use_id, content, false});
        return m;
    }
};

}  // namespace

TEST_CASE("解析摘要 JSON:严格、容错围栏、拒残次") {
    using agent::ParseMicrocompactSummary;
    const auto good = ParseMicrocompactSummary(
        "```json\n{\"summary\": \"这次构建跑了 700 步全部成功\", \"key_facts\": [\"退出码 0\", \"无错误行\"]}\n```",
        "a0001");
    REQUIRE(good.has_value());
    CHECK(good->summary.find("700") != std::string::npos);
    REQUIRE(good->key_facts.size() == 2);
    CHECK(good->key_facts[0] == "退出码 0");
    CHECK(good->source_artifact_id == "a0001");
    CHECK(!good->derived_from_summary);

    CHECK(!ParseMicrocompactSummary("不是 JSON", "a").has_value());
    CHECK(!ParseMicrocompactSummary("{\"key_facts\": []}", "a").has_value());          // 缺 summary
    CHECK(!ParseMicrocompactSummary("{\"summary\": \"太短\"}", "a").has_value());      // 残次
    CHECK(!ParseMicrocompactSummary("{\"summary\": 42}", "a").has_value());            // 类型不对
}

TEST_CASE("RunMicrocompact:输入来自 blob 原文;失败只报错不删原文") {
    TempStoreDir dir("lubancode-micro-run");
    Fixture fx(dir.path());
    const auto* ref = fx.store->Find(fx.memo.decisions["toolu-cold"].artifact_id);
    REQUIRE(ref != nullptr);

    FakeBackend backend;
    backend.script = ReplyScript(
        "{\"summary\": \"构建 700 步全部通过,产物正常\", \"key_facts\": [\"退出码 0\", \"无 FAILED 行\"]}");

    SUBCASE("成功:model/effort 进请求,摘要带来源") {
        agent::BackgroundCallAccounting accounting;
        const auto summary = agent::RunMicrocompact(backend, "cheap-m", "low", *fx.store, *ref,
                                                    agent::MicrocompactOptions{}, &accounting);
        REQUIRE(summary.has_value());
        CHECK(summary->model == "cheap-m");
        CHECK(summary->source_artifact_id == ref->artifact_id);
        REQUIRE(backend.captured_requests.size() == 1);
        CHECK(backend.captured_requests[0].model == "cheap-m");
        CHECK(backend.captured_requests[0].reasoning_effort == "low");
        // 请求里带的是原文(不是预览、不是旧摘要):头部原文行在场。
        CHECK(backend.captured_requests[0].messages.back().content.size() > 0);
        bool has_original = false;
        for (const auto& block : backend.captured_requests[0].messages.back().content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block);
                text != nullptr && text->text.find("build step 100 ") != std::string::npos) {
                has_original = true;
            }
        }
        CHECK(has_original);
    }
    SUBCASE("超长输入头尾各半截断:中段标注可追回") {
        agent::MicrocompactOptions options;
        options.input_cap_bytes = 8 * 1024;  // 原文约 11KB,必截
        const auto summary = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref, options);
        REQUIRE(summary.has_value());
        const std::string sent = std::get_if<api::TextBlock>(&backend.captured_requests[0].messages.back().content[0])->text;
        CHECK(sent.find("context_read") != std::string::npos);  // 中段省略注明可追
        CHECK(sent.size() < fx.cold_content.size());
    }
    SUBCASE("请求失败:返回错误,旧决策与原文不动") {
        backend.fail = true;
        backend.fail_message = "provider 超时";
        const auto failed = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref,
                                                   agent::MicrocompactOptions{});
        CHECK(!failed.has_value());
        CHECK(fx.memo.decisions["toolu-cold"].kind == agent::ResultViewKind::Artifact);  // 还是 L1
        // 原文仍在仓里,一字未删。
        const auto blob = fx.store->ReadBlobVerified(*ref);
        REQUIRE(blob.has_value());
        CHECK(*blob == fx.cold_content);
    }
    SUBCASE("调用前已取消:不碰 backend") {
        std::atomic<bool> cancel{true};
        const auto failed = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref,
                                                   agent::MicrocompactOptions{}, nullptr, &cancel);
        CHECK(!failed.has_value());
        CHECK(failed.error().find("取消") != std::string::npos);
        CHECK(backend.captured_requests.empty());
    }
    SUBCASE("调用跑到半截收到取消:外部信号传进 backend") {
        backend.wait_for_cancel = true;
        std::atomic<bool> cancel{false};
        std::thread canceller([&cancel]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            cancel = true;
        });
        const auto failed = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref,
                                                   agent::MicrocompactOptions{}, nullptr, &cancel);
        canceller.join();
        CHECK(!failed.has_value());
        CHECK(backend.captured_requests.size() == 1);
    }
    SUBCASE("坏 JSON 输出:返回错误") {
        backend.script = ReplyScript("瞎写的,不是 JSON");
        const auto bad = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref,
                                                agent::MicrocompactOptions{});
        CHECK(!bad.has_value());
    }
}
