// L2 microcompact(第三期)的单测:冷区挑选(热区不碰/触发线/迟滞/每趟
// 上限/大者先/已收拾过的不重做)、摘要 JSON 解析(好/坏/残次)、假 backend
// 的完整请求路(输入来自 blob 原文、失败退 L1 不删原文)、应用趟(视图换
// 摘要、原文照旧、前缀决策改写)。
#include <doctest/doctest.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
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
    std::string fail_message;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
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

// 造一场历史:两条用户轮,各带一枚超长 read_file 结果(都会在 memo 里定形
// 成 Artifact)。冷区 = 第一轮;热区 = 第二轮(最后一条用户文本之后)。
// 默认冷区 2200 行(约 40KB,过 32KiB 触发线)。
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

        // 现场定形:两枚都判成 Artifact 并落盘。注意 CompressWorkingView 返回
        // 的是请求视图(结果正文已换预览);挑候选取**原史**(事件账的
        // result_content 是原文,冷区字节按它算),视图只用来断言渲染。
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

TEST_CASE("挑候选:只挑冷区,热区绝不碰;触发线与每趟上限") {
    TempStoreDir dir("lubancode-micro-pick");
    Fixture fx(dir.path());
    agent::MicrocompactOptions options;   // cold_trigger_bytes 默认 32KiB
    options.max_per_pass = 3;
    agent::MicrocompactHysteresis fresh;

    const auto candidates = agent::PickMicrocompactCandidates(fx.history, fx.memo, options, fresh);
    REQUIRE(candidates.size() == 1);  // 冷区只有第一轮那枚;热区那枚不进
    CHECK(candidates[0].tool_use_id == "toolu-cold");
    CHECK(candidates[0].event_id.find("e") == 0);
    CHECK(!candidates[0].artifact_id.empty());

    SUBCASE("没过触发线:一条不挑") {
        Fixture small(dir.path(), /*cold_lines=*/50);  // 约 1KB,不过 32KiB 线
        const auto none = agent::PickMicrocompactCandidates(small.history, small.memo, options, fresh);
        CHECK(none.empty());
    }
    SUBCASE("迟滞:上趟压过后,冷区没再涨五成不压") {
        agent::MicrocompactHysteresis cooldown;
        cooldown.pass_attempted = true;
        cooldown.last_pass_cold_bytes = agent::ColdArtifactBytes(fx.history, fx.memo);
        const auto blocked = agent::PickMicrocompactCandidates(fx.history, fx.memo, options, cooldown);
        CHECK(blocked.empty());
        // 冷区再涨一截(新的一场,约 66KB > 41KB × 1.5)过了迟滞线。
        Fixture bigger(dir.path(), /*cold_lines=*/3600);
        agent::MicrocompactHysteresis after_growth;
        after_growth.pass_attempted = true;
        after_growth.last_pass_cold_bytes = agent::ColdArtifactBytes(fx.history, fx.memo);
        const auto again = agent::PickMicrocompactCandidates(bigger.history, bigger.memo, options, after_growth);
        CHECK(!again.empty());
    }
    SUBCASE("已是 L2 摘要的不重做(摘要不套娃)") {
        agent::ResultViewMemo summarized = fx.memo;
        summarized.decisions["toolu-cold"].kind = agent::ResultViewKind::MicrocompactSummary;
        summarized.decisions["toolu-cold"].summary_text = "已收拾过";
        const auto none = agent::PickMicrocompactCandidates(fx.history, summarized, options, fresh);
        CHECK(none.empty());
    }
}

TEST_CASE("解析摘要 JSON:严格、容错围栏、拒残次") {
    using agent::ParseMicrocompactSummary;
    const auto good = ParseMicrocompactSummary(
        "```json\n{\"summary\": \"这次构建跑了 700 步全部成功\", \"key_facts\": [\"退出码 0\", \"无错误行\"]}\n```",
        "a0001", "e1");
    REQUIRE(good.has_value());
    CHECK(good->summary.find("700") != std::string::npos);
    REQUIRE(good->key_facts.size() == 2);
    CHECK(good->key_facts[0] == "退出码 0");
    CHECK(good->source_artifact_id == "a0001");
    CHECK(good->source_event_id == "e1");
    CHECK(!good->derived_from_summary);

    CHECK(!ParseMicrocompactSummary("不是 JSON", "a", "e").has_value());
    CHECK(!ParseMicrocompactSummary("{\"key_facts\": []}", "a", "e").has_value());          // 缺 summary
    CHECK(!ParseMicrocompactSummary("{\"summary\": \"太短\"}", "a", "e").has_value());      // 残次
    CHECK(!ParseMicrocompactSummary("{\"summary\": 42}", "a", "e").has_value());            // 类型不对
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
        const auto summary = agent::RunMicrocompact(backend, "cheap-m", "low", *fx.store, *ref, "e1",
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
        const auto summary = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref, "e1", options);
        REQUIRE(summary.has_value());
        const std::string sent = std::get_if<api::TextBlock>(&backend.captured_requests[0].messages.back().content[0])->text;
        CHECK(sent.find("context_read") != std::string::npos);  // 中段省略注明可追
        CHECK(sent.size() < fx.cold_content.size());
    }
    SUBCASE("请求失败:返回错误,调用方退 L1(决策与原文不动)") {
        backend.fail = true;
        backend.fail_message = "provider 超时";
        const auto failed = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref, "e1",
                                                   agent::MicrocompactOptions{});
        CHECK(!failed.has_value());
        CHECK(fx.memo.decisions["toolu-cold"].kind == agent::ResultViewKind::Artifact);  // 还是 L1
        // 原文仍在仓里,一字未删。
        const auto blob = fx.store->ReadBlobVerified(*ref);
        REQUIRE(blob.has_value());
        CHECK(*blob == fx.cold_content);
    }
    SUBCASE("坏 JSON 输出:退 L1") {
        backend.script = ReplyScript("瞎写的,不是 JSON");
        const auto bad = agent::RunMicrocompact(backend, "cheap-m", "", *fx.store, *ref, "e1",
                                                agent::MicrocompactOptions{});
        CHECK(!bad.has_value());
    }
}

TEST_CASE("应用趟:视图换摘要,决策只改 L1 形态的") {
    TempStoreDir dir("lubancode-micro-apply");
    Fixture fx(dir.path());

    std::map<std::string, agent::MicrocompactSummary> summaries;
    agent::MicrocompactSummary summary;
    summary.summary = "构建 700 步全部通过";
    summary.key_facts = {"退出码 0", "无错误行"};
    summary.model = "cheap-m";
    summaries.emplace("toolu-cold", std::move(summary));
    summaries.emplace("toolu-ghost", agent::MicrocompactSummary{});  // 不存在的事件

    const int applied = agent::ApplyMicrocompactSummaries(fx.memo, summaries);
    CHECK(applied == 1);  // ghost 不在账上,自然跳过
    const auto& decision = fx.memo.decisions["toolu-cold"];
    CHECK(decision.kind == agent::ResultViewKind::MicrocompactSummary);
    CHECK(decision.summary_model == "cheap-m");
    CHECK(decision.summary_text.find("退出码 0") != std::string::npos);
    CHECK(decision.artifact_id == "a0001");  // source ref 保住

    // 热区那枚不受影响(还是 L1 artifact)。
    CHECK(fx.memo.decisions["toolu-hot"].kind == agent::ResultViewKind::Artifact);

    // 渲染:视图里带追回指引(摘要≠全文)。
    agent::StructuralCompressionOptions options;
    options.long_result_bytes = 4096;
    agent::StructuralCompressionStats stats;
    agent::ResultViewMemo replay = fx.memo;
    const auto view = agent::CompressWorkingView(fx.history, options, stats, replay, fx.store.get());
    std::string cold_view;
    for (const auto& block : view[2].content) {
        if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
            result != nullptr && result->tool_use_id == "toolu-cold") {
            cold_view = result->content;
        }
    }
    CHECK(cold_view.find("microcompact 摘要") != std::string::npos);
    CHECK(cold_view.find("context_read") != std::string::npos);
    CHECK(cold_view.find("a0001") != std::string::npos);
    CHECK(cold_view.find("cheap:cheap-m") != std::string::npos);

    // 已是 L2 的再应用一趟:不动(摘要不套娃)。
    std::map<std::string, agent::MicrocompactSummary> again;
    again.emplace("toolu-cold", agent::MicrocompactSummary{});
    CHECK(agent::ApplyMicrocompactSummaries(fx.memo, again) == 0);
}
