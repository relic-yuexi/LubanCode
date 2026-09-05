// 记忆写入调度单 P0:调度账的册——文本统计、稳定枚举、写路回执、
// 漏斗计数与两枚 typed event(memory.extraction.assessed /
// memory.write.receipted)的端到端。P1 批添:turn_mutated(同轮去重的
// 判定位)、必跳层与 shadow 在 ExtractTurnMemory 真路径上的端到端。
// 全部离线:假收件口 + 假后端 + 临时目录,零网络零真模型。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/memory_extract.hpp"
#include "app/model_router.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lmb-memdispatch-" + std::to_string(run_id % 100000) + "-" + name + "-" +
                     std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

// 假收件口:只攒回执,不落盘。
class CaptureSink final : public memory::MemoryWriteReceiptSink {
public:
    void OnMemoryWriteReceipt(const memory::MemoryWriteReceipt& receipt) override {
        receipts.push_back(receipt);
    }
    std::vector<memory::MemoryWriteReceipt> receipts;
};

memory::Options EnabledOptions() {
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;  // learn 默认 review
    return options;
}

memory::SaveRequest ValidFactRequest() {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.deploy";
    request.title = "部署命令";
    request.summary = "部署命令";
    request.content = "deploy 走 build.sh。";
    request.keywords = {"deploy"};
    request.paths = {"build.sh"};
    request.confidence = "verified";
    return request;
}

std::vector<nlohmann::json> ReadEvents(const fs::path& stream) {
    std::vector<nlohmann::json> events;
    const auto lines = trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (!parsed.is_discarded()) events.push_back(parsed);
    }
    return events;
}

std::vector<nlohmann::json> EventsOfKind(const fs::path& stream, const std::string& kind) {
    std::vector<nlohmann::json> found;
    for (const auto& event : ReadEvents(stream)) {
        if (event.value("kind", std::string()) == kind) found.push_back(event);
    }
    return found;
}

// P1 e2e 的假后端:每次调用都吐同一份合法抽取 JSON(当正文回复用也无
// 妨),关键是数调用次数——cheap 未配回落 normal,主 turn 与抽取共用它。
class CountingBackend final : public api::Backend {
public:
    int calls = 0;
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        ++calls;
        on_event(api::MessageStart{"msg", "test-model"});
        on_event(api::TextDelta{
            R"({"task_type":"other","summary":"无事可记","retrieval_terms":[],"candidates":[]})"});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"end_turn", api::Usage{}});
        return {};
    }
};

// 最小回合接线(同 test_agent_recovery 的形状):文本回合够用。
struct FakeTurn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    FakeTurn() : adapter("test", ids) { adapter.Start(); }
};

lubancode::config::ConfigResult RouterConfig() {
    const auto parsed = lubancode::config::ParseFileConfigJson(R"({
        "providers": [{"name": "local", "base_url": "http://localhost:1", "wire": "anthropic",
                       "model": "n1"}],
        "active_provider": "local"
    })",
                                                               "test.json");
    REQUIRE(parsed.has_value());
    const auto merged = lubancode::config::MergeConfig(lubancode::config::LubancodeEnvValues{},
                                                       std::optional<lubancode::config::FileConfig>{*parsed});
    REQUIRE(merged.has_value());
    return *merged;
}

// 环境变量临时改写:出了作用域还原。
class EnvGuard final {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old != nullptr) old_ = old;
        had_value_ = old != nullptr;
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, /*replace=*/1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv_s(name_, had_value_ ? old_.c_str() : "");
#else
        if (had_value_) {
            setenv(name_, old_.c_str(), /*replace=*/1);
        } else {
            unsetenv(name_);
        }
#endif
    }

private:
    const char* name_;
    std::string old_;
    bool had_value_ = false;
};

std::shared_ptr<memory::ProjectMemory> MakeMemory(const fs::path& root,
                                                  memory::MemoryWriteReceiptSink* sink) {
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    auto store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home, EnabledOptions());
    store->set_write_receipt_sink(sink);
    return store;
}

nlohmann::json SaveToolInput() {
    nlohmann::json input;
    input["kind"] = "fact";
    input["title"] = "部署命令";
    input["summary"] = "部署命令";
    input["content"] = "部署走 build.sh,先 bump 版本号。";
    input["paths"] = nlohmann::json::array({"build.sh"});
    input["confidence"] = "verified";
    return input;
}

}  // namespace

// ---------------------------------------------------------------------------
// 文本统计(§3.2 首折):中文没有天然空格,不能照搬英文词数口径。
// ---------------------------------------------------------------------------
TEST_CASE("ComputeMeaningfulTextStats: 中英混排与边界") {
    const auto empty = app::ComputeMeaningfulTextStats("");
    CHECK(empty.unicode_scalar_count == 0);
    CHECK(empty.cjk_char_count == 0);
    CHECK(empty.latin_word_count == 0);

    // "记住，以后用 pnpm":5 个 CJK + 全角逗号(不计 CJK)+ 空格 + 4 字母。
    const auto mixed = app::ComputeMeaningfulTextStats("记住，以后用 pnpm");
    CHECK(mixed.unicode_scalar_count == 11);
    CHECK(mixed.cjk_char_count == 5);
    CHECK(mixed.latin_word_count == 1);

    const auto english = app::ComputeMeaningfulTextStats("hello world foo");
    CHECK(english.unicode_scalar_count == 15);
    CHECK(english.cjk_char_count == 0);
    CHECK(english.latin_word_count == 3);

    // 纯标点/空白:标量照数,词数与 CJK 为 0。
    const auto punct = app::ComputeMeaningfulTextStats("，。 !?");
    CHECK(punct.unicode_scalar_count == 5);
    CHECK(punct.cjk_char_count == 0);
    CHECK(punct.latin_word_count == 0);

    // 四字节 emoji:算一个标量,不进 CJK。
    const auto emoji = app::ComputeMeaningfulTextStats("\xF0\x9F\x98\x80");
    CHECK(emoji.unicode_scalar_count == 1);
    CHECK(emoji.cjk_char_count == 0);

    // 坏 UTF-8(孤续字节/超界首字节):不炸,按标量摊开。
    const auto broken = app::ComputeMeaningfulTextStats("\xFF\x80");
    CHECK(broken.unicode_scalar_count == 2);

    // 半截多字节序列结尾:丢弃,不计数。
    const auto truncated = app::ComputeMeaningfulTextStats("\xE4\xB8");
    CHECK(truncated.unicode_scalar_count == 0);
}

// ---------------------------------------------------------------------------
// 稳定枚举名(§15:同一冻结输入跨平台一致)。改名即改合同,这里钉死。
// ---------------------------------------------------------------------------
TEST_CASE("调度枚举名:全部钉死,P1 的名字先冻结") {
    CHECK(std::string(app::ExtractionTriggerName(app::ExtractionTrigger::EveryTurn)) == "every_turn");
    CHECK(std::string(app::ExtractionTriggerName(app::ExtractionTrigger::BatchWatermark)) ==
          "batch_watermark");
    CHECK(std::string(app::ExtractionTriggerName(app::ExtractionTrigger::IdleTimeout)) == "idle_timeout");
    CHECK(std::string(app::ExtractionTriggerName(app::ExtractionTrigger::BeforeCompact)) ==
          "before_compact");
    CHECK(std::string(app::ExtractionTriggerName(app::ExtractionTrigger::SessionEnd)) == "session_end");

    CHECK(std::string(app::ExtractionDecisionName(app::ExtractionDecision::Skipped)) == "skipped");
    CHECK(std::string(app::ExtractionDecisionName(app::ExtractionDecision::Called)) == "called");

    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::Disabled)) == "disabled");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::NoNewHistory)) ==
          "no_new_history");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::EmptyTranscript)) ==
          "empty_transcript");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::PromptMissing)) ==
          "prompt_missing");
    // P1 起的五枚:名字现在冻结,判定 P1 接。
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::ExtractModeOff)) ==
          "extract_mode_off");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::AlreadyMutated)) ==
          "already_mutated");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::ShortText)) == "short_text");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::AcknowledgementOnly)) ==
          "acknowledgement_only");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::SlashCommandOnly)) ==
          "slash_command_only");
    CHECK(std::string(app::ExtractionSkipReasonName(app::ExtractionSkipReason::NoDurableSignal)) ==
          "no_durable_signal");

    CHECK(memory::MemoryWriteSourceName(memory::MemoryWriteSource::ExplicitCommandSave) ==
          "explicit_command_save");
    CHECK(memory::MemoryWriteSourceName(memory::MemoryWriteSource::ModelToolSave) == "model_tool_save");
    CHECK(memory::MemoryWriteSourceName(memory::MemoryWriteSource::ExplicitForget) == "explicit_forget");
    CHECK(memory::MemoryWriteSourceName(memory::MemoryWriteSource::CandidateAccept) == "candidate_accept");
    CHECK(memory::MemoryWriteSourceName(memory::MemoryWriteSource::AutoExtraction) == "auto_extraction");
    CHECK(memory::MemoryWriteReceiptOutcomeName(memory::MemoryWriteReceiptOutcome::Queued) == "queued");
    CHECK(memory::MemoryWriteReceiptOutcomeName(memory::MemoryWriteReceiptOutcome::Rejected) == "rejected");
}

TEST_CASE("稳定错误码:固定文案折枚举,认不出落 other") {
    CHECK(memory::StableWriteErrorCode("本场记忆写入未开启") == "write_disabled");
    CHECK(memory::StableWriteErrorCode("本场记忆学习未开启") == "write_disabled");
    CHECK(memory::StableWriteErrorCode("本场记忆未开启") == "memory_disabled");
    CHECK(memory::StableWriteErrorCode(
              "memory.global_unauthorized: 全局记忆只认用户主动命令(/memory remember global ...)") ==
          "global_unauthorized");
    CHECK(memory::StableWriteErrorCode("用户级记忆未在全局配置授权(memory.user_enabled),本场命令开不了") ==
          "user_layer_unauthorized");
    CHECK(memory::StableWriteErrorCode("找不到候选: cand-1") == "candidate_not_found");
    CHECK(memory::StableWriteErrorCode("记忆 id 不合法") == "invalid_id");
    CHECK(memory::StableWriteErrorCode("记忆标题不能为空，且不能超过 200 字节") == "invalid_request");
    CHECK(memory::StableWriteErrorCode("feedback 只收用户明说的纠正(confidence 须为 user-stated),模型推断不得直写") ==
          "feedback_requires_user_stated");
    CHECK(memory::StableWriteErrorCode("从没见过的错") == "other");

    CHECK(app::StableExtractErrorCode("cheap 路由找不到 provider \"kimi\"") == "route_miss");
    CHECK(app::StableExtractErrorCode("抽取输出为空") == "empty_output");
    CHECK(app::StableExtractErrorCode("抽取输出不是合法 JSON: ...") == "parse_failed");
    CHECK(app::StableExtractErrorCode("抽取输出里找不到 JSON object") == "parse_failed");
    CHECK(app::StableExtractErrorCode("网络炸了") == "other");
}

// ---------------------------------------------------------------------------
// 漏斗与回执账(无轨迹:只记内存账,零落盘)。
// ---------------------------------------------------------------------------
TEST_CASE("MemoryTurnLedger: 漏斗计数与回执分账(无轨迹)") {
    app::MemoryTurnLedger ledger(nullptr);

    ledger.BeginTurn("sess-1", "turn-1", "记住，以后用 pnpm");
    ledger.NoteExtractionSkipped(app::ExtractionSkipReason::Disabled);
    ledger.FinishTurn(3);
    CHECK(ledger.funnel().outer_user_turns == 1);
    CHECK(ledger.funnel().skipped_disabled == 1);
    CHECK(ledger.funnel().extract_batches == 0);

    ledger.BeginTurn("sess-1", "turn-2", "帮我修这个崩溃");
    ledger.NoteExtractionSkipped(app::ExtractionSkipReason::NoNewHistory);
    ledger.FinishTurn(1);
    CHECK(ledger.funnel().skipped_no_new_history == 1);

    ledger.BeginTurn("sess-1", "turn-3", "跑一下测试");
    ledger.NoteHistoryGrew();
    ledger.NoteExtractionSkipped(app::ExtractionSkipReason::EmptyTranscript);
    ledger.FinishTurn(2);
    CHECK(ledger.funnel().history_grew_turns == 1);
    CHECK(ledger.funnel().skipped_empty_transcript == 1);

    app::MemoryTurnLedger ledger2(nullptr);
    ledger2.BeginTurn("sess-1", "turn-4", "看看构建");
    ledger2.NoteHistoryGrew();
    ledger2.NoteExtractionCalled();
    app::MemoryTurnLedger::ExtractOutcome outcome;
    outcome.ok = true;
    outcome.usage_reported = true;
    outcome.input_tokens = 1584;
    outcome.output_tokens = 173;
    outcome.cached_tokens = 512;
    outcome.extract_wall_ms = 812;
    outcome.review_candidates = 1;
    outcome.auto_written = 0;
    ledger2.NoteExtractionOutcome(outcome);
    ledger2.FinishTurn(930);
    CHECK(ledger2.funnel().extract_batches == 1);
    CHECK(ledger2.funnel().eligible_turns == 1);
    CHECK(ledger2.funnel().extract_failures == 0);

    app::MemoryTurnLedger::ExtractOutcome failure;
    failure.ok = false;
    failure.extract_wall_ms = 45000;
    failure.error_code = "route_miss";
    ledger2.BeginTurn("sess-1", "turn-5", "再来");
    ledger2.NoteExtractionCalled();
    ledger2.NoteExtractionOutcome(failure);
    ledger2.FinishTurn(45012);
    CHECK(ledger2.funnel().extract_batches == 2);
    CHECK(ledger2.funnel().extract_failures == 1);
}

TEST_CASE("MemoryTurnLedger: 四路回执进本轮账,回合间不带回合号") {
    app::MemoryTurnLedger ledger(nullptr);
    ledger.BeginTurn("sess-1", "turn-9", "正文");

    memory::MemoryWriteReceipt save;
    save.source = memory::MemoryWriteSource::ModelToolSave;
    save.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    save.operation = "upsert";
    save.job_id = "job-a.json";
    save.layer = "project";
    save.kind = "fact";
    ledger.OnMemoryWriteReceipt(save);

    memory::MemoryWriteReceipt forget;
    forget.source = memory::MemoryWriteSource::ExplicitForget;
    forget.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    forget.operation = "forget";
    forget.job_id = "job-b.json";
    forget.layer = "project";
    ledger.OnMemoryWriteReceipt(forget);

    memory::MemoryWriteReceipt accept;
    accept.source = memory::MemoryWriteSource::CandidateAccept;
    accept.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    accept.operation = "upsert";
    accept.job_id = "job-c.json";
    accept.layer = "project";
    accept.kind = "fact";
    ledger.OnMemoryWriteReceipt(accept);

    memory::MemoryWriteReceipt rejected;
    rejected.source = memory::MemoryWriteSource::ExplicitCommandSave;
    rejected.outcome = memory::MemoryWriteReceiptOutcome::Rejected;
    rejected.operation = "upsert";
    rejected.error_code = "write_disabled";
    rejected.layer = "project";
    ledger.OnMemoryWriteReceipt(rejected);

    ledger.FinishTurn(7);

    // 回合间的回执(slash 命令)照收,不带回合号、不炸。
    memory::MemoryWriteReceipt between;
    between.source = memory::MemoryWriteSource::ExplicitCommandSave;
    between.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    between.operation = "upsert";
    between.job_id = "job-d.json";
    between.layer = "project";
    between.kind = "preference";
    ledger.OnMemoryWriteReceipt(between);
}

// ---------------------------------------------------------------------------
// ProjectMemory 四路写路的回执(假收件口,临时目录)。
// ---------------------------------------------------------------------------
TEST_CASE("ProjectMemory: remember/forget/accept/memory_save 四路回执") {
    const fs::path root = TempRoot("receipts");
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);

    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(std::move(*identity), home, EnabledOptions());
    CaptureSink sink;
    store.set_write_receipt_sink(&sink);

    SUBCASE("显式命令保存:queued + 全字段") {
        const auto queued = store.EnqueueSave(ValidFactRequest(), /*user_initiated=*/true,
                                              memory::MemoryWriteSource::ExplicitCommandSave);
        REQUIRE(queued.has_value());
        REQUIRE(sink.receipts.size() == 1);
        const auto& receipt = sink.receipts[0];
        CHECK(receipt.source == memory::MemoryWriteSource::ExplicitCommandSave);
        CHECK(receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued);
        CHECK(receipt.operation == "upsert");
        CHECK(receipt.job_id == *queued);
        CHECK_FALSE(receipt.job_id.empty());
        CHECK(receipt.layer == "project");
        CHECK(receipt.kind == "fact");
    }

    SUBCASE("learn off:rejected + 稳定码") {
        CHECK(store.set_learn(memory::LearnMode::Off).has_value());
        const auto rejected = store.EnqueueSave(ValidFactRequest(), /*user_initiated=*/true,
                                                memory::MemoryWriteSource::ExplicitCommandSave);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(sink.receipts.size() == 1);
        CHECK(sink.receipts[0].outcome == memory::MemoryWriteReceiptOutcome::Rejected);
        CHECK(sink.receipts[0].error_code == "write_disabled");
        CHECK(sink.receipts[0].job_id.empty());
    }

    SUBCASE("forget:非法 id rejected,合法 id queued") {
        const auto rejected = store.EnqueueForget("bogus id!");
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(sink.receipts.size() == 1);
        CHECK(sink.receipts[0].source == memory::MemoryWriteSource::ExplicitForget);
        CHECK(sink.receipts[0].error_code == "invalid_id");

        const auto queued = store.EnqueueForget("fact.deploy");
        REQUIRE(queued.has_value());
        REQUIRE(sink.receipts.size() == 2);
        CHECK(sink.receipts[1].outcome == memory::MemoryWriteReceiptOutcome::Queued);
        CHECK(sink.receipts[1].operation == "forget");
        CHECK(sink.receipts[1].job_id == *queued);
        CHECK(sink.receipts[1].kind.empty());
    }

    SUBCASE("候选接受:成功走 candidate_accept,早失败也投回执") {
        // 早失败:不存在的候选。
        CHECK_FALSE(store.AcceptCandidate("cand-nope").has_value());
        REQUIRE(sink.receipts.size() == 1);
        CHECK(sink.receipts[0].source == memory::MemoryWriteSource::CandidateAccept);
        CHECK(sink.receipts[0].outcome == memory::MemoryWriteReceiptOutcome::Rejected);
        CHECK(sink.receipts[0].error_code == "candidate_not_found");

        // 种一条可接受的候选(verified fact 带证据)。
        memory::MemoryCandidate candidate;
        candidate.kind = memory::MemoryKind::Fact;
        candidate.title = "构建命令";
        candidate.summary = "构建命令";
        candidate.content = "构建走 cmake。";
        candidate.paths = {"CMakeLists.txt"};
        candidate.confidence = "verified";
        const auto planted = store.AddCandidate(candidate);
        REQUIRE(planted.has_value());

        const auto queued = store.AcceptCandidate(*planted);
        REQUIRE(queued.has_value());
        REQUIRE(sink.receipts.size() == 2);
        CHECK(sink.receipts[1].source == memory::MemoryWriteSource::CandidateAccept);
        CHECK(sink.receipts[1].outcome == memory::MemoryWriteReceiptOutcome::Queued);
        CHECK(sink.receipts[1].job_id == *queued);
    }

    SUBCASE("memory_save 工具:model_tool_save 回执") {
        auto shared = std::make_shared<memory::ProjectMemory>(std::move(store));
        memory::MemorySaveTool tool(shared);
        nlohmann::json input;
        input["kind"] = "fact";
        input["title"] = "部署命令";
        input["summary"] = "部署命令";
        input["content"] = "deploy 走 build.sh。";
        input["paths"] = nlohmann::json::array({"build.sh"});
        input["confidence"] = "verified";
        const auto result = tool.execute(input);
        CHECK_FALSE(result.is_error);
        REQUIRE(sink.receipts.size() == 1);
        CHECK(sink.receipts[0].source == memory::MemoryWriteSource::ModelToolSave);
        CHECK(sink.receipts[0].outcome == memory::MemoryWriteReceiptOutcome::Queued);
        CHECK_FALSE(sink.receipts[0].job_id.empty());
        CHECK(sink.receipts[0].kind == "fact");
    }
}

// ---------------------------------------------------------------------------
// typed event 端到端:真 TrajectorySessionLedger,读回 main.jsonl 逐条
// 过 schema(ParseAndValidateEventLine)。
// ---------------------------------------------------------------------------
TEST_CASE("typed event: assessed 与 receipted 落 main.jsonl 且过 schema") {
    const fs::path root = TempRoot("typed");
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);

    runtime::TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = home / "workspaces";
    ledger_options.workspace_root = repo;
    ledger_options.workspace_identity = workspace::MakeFallbackIdentity(repo);
    ledger_options.lubancode_version = "test";
    auto session = runtime::TrajectorySessionLedger::Open(ledger_options);
    REQUIRE(session.has_value());

    memory::Options options = EnabledOptions();
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(std::move(*identity), home, options);

    app::MemoryTurnLedger ledger(&*session);
    store.set_write_receipt_sink(&ledger);

    // 回合 1:显式保存排队 + 抽取被拦(learn off 场景的 disabled 分支)。
    store.set_learn(memory::LearnMode::Off);
    ledger.BeginTurn(session->session_id(), "turn-100", "记住，以后用 pnpm");
    CHECK_FALSE(store.EnqueueSave(ValidFactRequest(), /*user_initiated=*/true,
                                  memory::MemoryWriteSource::ExplicitCommandSave)
                    .has_value());
    ledger.NoteExtractionSkipped(app::ExtractionSkipReason::Disabled);
    ledger.FinishTurn(4);

    // 回合 2:抽取真发了一枪(记账路径直驱,不发网络)。
    store.set_learn(memory::LearnMode::Review);
    ledger.BeginTurn(session->session_id(), "turn-101", "帮我修这个崩溃");
    ledger.NoteHistoryGrew();
    ledger.NoteExtractionCalled();
    app::MemoryTurnLedger::ExtractOutcome outcome;
    outcome.ok = true;
    outcome.usage_reported = true;
    outcome.input_tokens = 1584;
    outcome.output_tokens = 173;
    outcome.cached_tokens = 512;
    outcome.extract_wall_ms = 812;
    outcome.review_candidates = 1;
    outcome.auto_written = 0;
    ledger.NoteExtractionOutcome(outcome);
    // 同轮的 auto 直写回执。
    memory::MemoryWriteReceipt auto_write;
    auto_write.source = memory::MemoryWriteSource::AutoExtraction;
    auto_write.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    auto_write.operation = "upsert";
    auto_write.job_id = "job-x.json";
    auto_write.layer = "project";
    auto_write.kind = "fact";
    ledger.OnMemoryWriteReceipt(auto_write);
    ledger.FinishTurn(930);

    const fs::path main_stream = session->session_dir() / "main.jsonl";

    // 全部行逐条过 schema(含既有事件,防新事件破坏整链)。
    for (const auto& event : ReadEvents(main_stream)) {
        trajectory::EventEnvelope envelope;
        const auto error = trajectory::ParseAndValidateEventLine(event, &envelope);
        if (error.has_value()) {
            const std::string detail = "schema 拒收: " + error->error_code + " " + error->message +
                                       " kind=" + event.value("kind", std::string());
            FAIL(detail.c_str());
        }
    }

    const auto assessed = EventsOfKind(main_stream, "memory.extraction.assessed");
    REQUIRE(assessed.size() == 2);

    const auto& first = assessed[0]["payload"];
    CHECK(first.value("trigger", std::string()) == "every_turn");
    CHECK(first.value("turn_id", std::string()) == "turn-100");
    CHECK(first.value("decision", std::string()) == "skipped");
    CHECK(first.value("skip_reason", std::string()) == "disabled");
    CHECK(first.value("foreground_tail_ms", std::int64_t{0}) == 4);
    CHECK(first.contains("input_tokens") == false);
    CHECK(first["user_text_stats"].value("cjk_char_count", std::uint64_t{0}) == 5);

    const auto& second = assessed[1]["payload"];
    CHECK(second.value("turn_id", std::string()) == "turn-101");
    CHECK(second.value("decision", std::string()) == "called");
    CHECK(second.contains("skip_reason") == false);
    CHECK(second.value("extract_outcome", std::string()) == "completed");
    CHECK(second.value("input_tokens", std::int64_t{0}) == 1584);
    CHECK(second.value("output_tokens", std::int64_t{0}) == 173);
    CHECK(second.value("cached_tokens", std::int64_t{0}) == 512);
    CHECK(second.value("extract_wall_ms", std::int64_t{0}) == 812);
    CHECK(second.value("review_candidates", std::uint64_t{0}) == 1);
    CHECK(second.value("foreground_tail_ms", std::int64_t{0}) == 930);

    const auto receipted = EventsOfKind(main_stream, "memory.write.receipted");
    REQUIRE(receipted.size() == 2);
    CHECK(receipted[0]["payload"].value("source", std::string()) == "explicit_command_save");
    CHECK(receipted[0]["payload"].value("outcome", std::string()) == "rejected");
    CHECK(receipted[0]["payload"].value("error_code", std::string()) == "write_disabled");
    CHECK(receipted[0].value("actor", std::string()) == "user");
    CHECK(receipted[0]["payload"].value("turn_id", std::string()) == "turn-100");
    CHECK(receipted[1]["payload"].value("source", std::string()) == "auto_extraction");
    CHECK(receipted[1]["payload"].value("outcome", std::string()) == "queued");
    CHECK(receipted[1]["payload"].value("job_id", std::string()) == "job-x.json");
    CHECK(receipted[1]["payload"].value("turn_id", std::string()) == "turn-101");
    CHECK(receipted[1].value("origin", std::string()) == "scheduled_host");
}

// ---------------------------------------------------------------------------
// schema 条件裁:互斥字段越界要拒。
// ---------------------------------------------------------------------------
TEST_CASE("schema: assessed/receipted 的互斥约束拒越界") {
    nlohmann::json assessed;
    assessed["trigger"] = "every_turn";
    assessed["turn_id"] = "t-1";
    assessed["decision"] = "skipped";
    assessed["skip_reason"] = "disabled";
    assessed["user_text_stats"] = nlohmann::json::object();
    assessed["foreground_tail_ms"] = 1;
    // skipped 却带调用侧字段:拒。
    auto bad = assessed;
    bad["input_tokens"] = 10;
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed, bad)
              .has_value());
    // skipped 缺 skip_reason:拒。
    auto no_reason = assessed;
    no_reason.erase("skip_reason");
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed,
                                                 no_reason)
              .has_value());
    // called 缺 extract_outcome:拒。
    auto called_missing = assessed;
    called_missing["decision"] = "called";
    called_missing.erase("skip_reason");
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed,
                                                 called_missing)
              .has_value());
    // 合法 called + 报账:过。
    nlohmann::json called;
    called["trigger"] = "every_turn";
    called["turn_id"] = "t-1";
    called["decision"] = "called";
    called["user_text_stats"] = nlohmann::json::object();
    called["foreground_tail_ms"] = 1;
    called["extract_outcome"] = "completed";
    called["extract_wall_ms"] = 5;
    called["review_candidates"] = std::uint64_t{0};
    called["auto_written"] = std::uint64_t{0};
    called["usage_reported"] = true;
    called["input_tokens"] = 1;
    called["output_tokens"] = 1;
    called["cached_tokens"] = 0;
    CHECK_FALSE(trajectory::ValidatePayloadWithVersion(
                    2, trajectory::EventKind::MemoryExtractionAssessed, called)
                    .has_value());
    // provider 没报却带 token:拒。
    auto unreported = called;
    unreported["usage_reported"] = false;
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed,
                                                 unreported)
              .has_value());

    nlohmann::json receipt;
    receipt["source"] = "model_tool_save";
    receipt["operation"] = "upsert";
    receipt["outcome"] = "queued";
    receipt["layer"] = "project";
    // queued 缺 job_id:拒。
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryWriteReceipted, receipt)
              .has_value());
    receipt["job_id"] = "job.json";
    CHECK_FALSE(trajectory::ValidatePayloadWithVersion(
                    2, trajectory::EventKind::MemoryWriteReceipted, receipt)
                    .has_value());
    // rejected 带 job_id:拒。
    auto rejected = receipt;
    rejected["outcome"] = "rejected";
    rejected["error_code"] = "write_disabled";
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryWriteReceipted, rejected)
              .has_value());
}

// ---------------------------------------------------------------------------
// P1:turn_mutated(§7.1 案二的判定位)——回执账翻真,拒绝不算变更。
// ---------------------------------------------------------------------------
TEST_CASE("MemoryTurnLedger P1: turn_mutated 随成功回执翻真") {
    app::MemoryTurnLedger ledger(nullptr);
    ledger.BeginTurn("sess-1", "turn-m1", "正文");
    CHECK_FALSE(ledger.turn_mutated());

    // 被拒的写路不算变更(没写进去就没有"已走一条写路")。
    memory::MemoryWriteReceipt rejected;
    rejected.source = memory::MemoryWriteSource::ModelToolSave;
    rejected.outcome = memory::MemoryWriteReceiptOutcome::Rejected;
    rejected.operation = "upsert";
    rejected.error_code = "feedback_requires_user_stated";
    rejected.layer = "project";
    ledger.OnMemoryWriteReceipt(rejected);
    CHECK_FALSE(ledger.turn_mutated());

    // 成功的 save 翻位;forget/accept 同账(见 P0 册)。
    memory::MemoryWriteReceipt save;
    save.source = memory::MemoryWriteSource::ModelToolSave;
    save.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    save.operation = "upsert";
    save.job_id = "job-m1.json";
    save.layer = "project";
    save.kind = "fact";
    ledger.OnMemoryWriteReceipt(save);
    CHECK(ledger.turn_mutated());

    // 回合收口后落回 false:回合间的回执(slash 命令)不归下一轮。
    ledger.FinishTurn(2);
    CHECK_FALSE(ledger.turn_mutated());
}

// ---------------------------------------------------------------------------
// P1 e2e:同轮去重与必跳层在 ExtractTurnMemory 真路径上拦得住——假后端
// 数调用,主回合真跑,memory_save 工具真排队,漏斗对得上账。
// ---------------------------------------------------------------------------
TEST_CASE("P1 e2e: ExtractTurnMemory 的同轮去重与必跳层") {
    const fs::path root = TempRoot("e2e-gate");

    CountingBackend backend;
    auto current_model = std::make_shared<std::string>("test-model");
    std::string active_provider = "local";
    const auto config = RouterConfig();
    app::ModelRouterService router(config, backend, current_model, active_provider);
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    FakeTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) {
        return true;
    };
    const cli::Theme theme;
    std::string prompts_dir;

    SUBCASE("主回合 memory_save 成功:回合尾 already_mutated,一发不多发") {
        app::MemoryTurnLedger ledger(nullptr);
        auto store = MakeMemory(root / "dedup", &ledger);

        // 回合开张,主回合里模型调了 memory_save(工具真排队,回执进账)。
        const std::string text = "记住，这个项目以后部署都走 build.sh，发布前先把版本号 bump 一遍";
        ledger.BeginTurn(std::string(), "turn-e2e-dedup", text);
        memory::MemorySaveTool save_tool(store);
        CHECK_FALSE(save_tool.execute(SaveToolInput()).is_error);
        CHECK(ledger.turn_mutated());

        // 主回合正文跑完(纯文本回复,history 增长)。
        const auto run = loop.Run(text, wiring);
        REQUIRE(run.has_value());
        const int main_calls = backend.calls;
        REQUIRE(main_calls >= 1);

        app::SessionTailContext tail;
        tail.project_memory = store.get();
        tail.agent = &loop;
        tail.model_router = &router;
        tail.prompts_dir = &prompts_dir;
        tail.theme = &theme;
        tail.memory_turns = &ledger;
        app::ExtractTurnMemory(tail, text, /*history_before=*/0);

        // 同轮去重收手:抽取一发没发,漏斗对账。
        CHECK(backend.calls == main_calls);
        CHECK(ledger.funnel().outer_user_turns == 1);
        CHECK(ledger.funnel().history_grew_turns == 1);
        CHECK(ledger.funnel().skipped_already_mutated == 1);
        CHECK(ledger.funnel().extract_batches == 0);
        // 正文本身过得了文本门(22 个 CJK),拦它的必是同轮去重,不是短文本。
        CHECK(ledger.funnel().skipped_short == 0);
        CHECK(ledger.funnel().skipped_ack == 0);
        ledger.FinishTurn(3);
    }

    SUBCASE("纯确认回合:acknowledgement_only,模型一次不叫") {
        app::MemoryTurnLedger ledger(nullptr);
        auto store = MakeMemory(root / "ack", &ledger);

        ledger.BeginTurn(std::string(), "turn-e2e-ack", "好");
        const auto run = loop.Run("好", wiring);
        REQUIRE(run.has_value());
        const int main_calls = backend.calls;
        REQUIRE(main_calls == 1);

        app::SessionTailContext tail;
        tail.project_memory = store.get();
        tail.agent = &loop;
        tail.model_router = &router;
        tail.prompts_dir = &prompts_dir;
        tail.theme = &theme;
        tail.memory_turns = &ledger;
        app::ExtractTurnMemory(tail, "好", /*history_before=*/0);

        CHECK(backend.calls == main_calls);  // "好"不再触发抽取
        CHECK(ledger.funnel().skipped_ack == 1);
        CHECK(ledger.funnel().extract_batches == 0);
        ledger.FinishTurn(1);
    }

    SUBCASE("过门回合照发 + shadow 开着:判断逐回合落账可复算") {
        EnvGuard shadow_on("LUBANCODE_MEMORY_GATE_SHADOW", "1");

        runtime::TrajectorySessionLedger::Options ledger_options;
        ledger_options.workspaces_root = root / "workspaces";
        ledger_options.workspace_root = root / "repo";
        ledger_options.workspace_identity = workspace::MakeFallbackIdentity(root / "repo");
        ledger_options.lubancode_version = "test";
        auto session = runtime::TrajectorySessionLedger::Open(ledger_options);
        REQUIRE(session.has_value());

        app::MemoryTurnLedger ledger(&*session);
        auto store = MakeMemory(root / "shadow", &ledger);

        const std::string text = "以后统一用 pnpm 装依赖，别再用 npm 了，把 README 的安装段也修一下";
        ledger.BeginTurn(session->session_id(), "turn-e2e-shadow", text);
        const auto run = loop.Run(text, wiring);
        REQUIRE(run.has_value());
        const int main_calls = backend.calls;
        REQUIRE(main_calls == 1);

        app::SessionTailContext tail;
        tail.project_memory = store.get();
        tail.agent = &loop;
        tail.model_router = &router;
        tail.prompts_dir = &prompts_dir;
        tail.theme = &theme;
        tail.memory_turns = &ledger;
        app::ExtractTurnMemory(tail, text, /*history_before=*/0);

        // 过了门:真发一枪(现行路不因 shadow 改一字)。
        CHECK(backend.calls == main_calls + 1);
        CHECK(ledger.funnel().extract_batches == 1);
        ledger.FinishTurn(9);

        const fs::path main_stream = session->session_dir() / "main.jsonl";
        // 全链逐行过 schema(P1 新键在内,新事件不许破坏整链)。
        for (const auto& event : ReadEvents(main_stream)) {
            trajectory::EventEnvelope envelope;
            const auto error = trajectory::ParseAndValidateEventLine(event, &envelope);
            if (error.has_value()) {
                const std::string detail = "schema 拒收: " + error->error_code + " " + error->message +
                                           " kind=" + event.value("kind", std::string());
                FAIL(detail.c_str());
            }
        }

        const auto assessed = EventsOfKind(main_stream, "memory.extraction.assessed");
        REQUIRE(assessed.size() == 1);
        const auto& payload = assessed[0]["payload"];
        CHECK(payload.value("decision", std::string()) == "called");
        CHECK(payload.value("has_tool_evidence", true) == false);
        // 六键统计齐(§3.2 补全的三项在内),门槛判定离线可复算。
        CHECK(payload["user_text_stats"].contains("unicode_scalar_count"));
        CHECK(payload["user_text_stats"].contains("cjk_char_count"));
        CHECK(payload["user_text_stats"].contains("latin_word_count"));
        CHECK(payload["user_text_stats"].contains("code_token_count"));
        CHECK(payload["user_text_stats"].contains("only_acknowledgement"));
        CHECK(payload["user_text_stats"].contains("only_slash_command"));
        // shadow 判断落账:偏好案命中(词法上"以后"必中)。
        REQUIRE(payload.contains("shadow_gate"));
        const auto& shadow = payload.at("shadow_gate");
        CHECK(shadow.value("durable_signal", std::string()) == "hit");
        bool has_preference = false;
        for (const auto& signal : shadow.at("signals")) {
            if (signal == "preference_or_correction") has_preference = true;
        }
        CHECK(has_preference);
    }
}

// ---------------------------------------------------------------------------
// P1 schema:shadow_gate 的内洽裁——自相矛盾的行不许过(漏判账靠它复算)。
// ---------------------------------------------------------------------------
TEST_CASE("schema: shadow_gate 内洽裁与互斥约束") {
    const auto base = [] {
        nlohmann::json called;
        called["trigger"] = "every_turn";
        called["turn_id"] = "t-1";
        called["decision"] = "called";
        called["user_text_stats"] = nlohmann::json::object();
        called["foreground_tail_ms"] = 1;
        called["extract_outcome"] = "completed";
        called["extract_wall_ms"] = 5;
        called["review_candidates"] = std::uint64_t{0};
        called["auto_written"] = std::uint64_t{0};
        return called;
    };
    // 合法 hit:verdict 与名单同进。
    auto hit = base();
    hit["shadow_gate"] = nlohmann::json{{"durable_signal", "hit"},
                                        {"signals", nlohmann::json::array({"preference_or_correction"})}};
    CHECK_FALSE(
        trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed, hit)
            .has_value());
    // 合法 none:评过、一条没中。
    auto none = base();
    none["shadow_gate"] = nlohmann::json{{"durable_signal", "none"}, {"signals", nlohmann::json::array()}};
    CHECK_FALSE(
        trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed, none)
            .has_value());
    // hit 却空名单:拒。
    auto hollow = hit;
    hollow["shadow_gate"]["signals"] = nlohmann::json::array();
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed, hollow)
              .has_value());
    // none 却带名单:拒。
    auto stuffed = none;
    stuffed["shadow_gate"]["signals"] = nlohmann::json::array({"test_conclusion"});
    CHECK(
        trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed, stuffed)
            .has_value());
    // 野 verdict:拒。
    auto wild = hit;
    wild["shadow_gate"]["durable_signal"] = "maybe";
    CHECK(trajectory::ValidatePayloadWithVersion(2, trajectory::EventKind::MemoryExtractionAssessed, wild)
              .has_value());
    // skipped 却带 shadow_gate:拒(shadow 只在真叫模型那步评)。
    nlohmann::json skipped;
    skipped["trigger"] = "every_turn";
    skipped["turn_id"] = "t-1";
    skipped["decision"] = "skipped";
    skipped["skip_reason"] = "short_text";
    skipped["user_text_stats"] = nlohmann::json::object();
    skipped["foreground_tail_ms"] = 1;
    skipped["has_tool_evidence"] = false;
    CHECK_FALSE(trajectory::ValidatePayloadWithVersion(
                    2, trajectory::EventKind::MemoryExtractionAssessed, skipped)
                    .has_value());
    auto skipped_shadowed = skipped;
    skipped_shadowed["shadow_gate"] =
        nlohmann::json{{"durable_signal", "none"}, {"signals", nlohmann::json::array()}};
    CHECK(trajectory::ValidatePayloadWithVersion(
              2, trajectory::EventKind::MemoryExtractionAssessed, skipped_shadowed)
              .has_value());
}
