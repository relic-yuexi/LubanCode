// 回合记忆抽取异步执行器(记忆回合总结异步化单)的单测:Start 不阻塞、
// 单飞不叠发、迟到收账账落对档(Suspend/Settle/Abandon 三态)、换代弃
// 迟到、退出有界收口、Ready 真值表与唤醒源形制。假后端可控延迟,不发
// 一个网络包;SettleTurnMemory 的全链(候选入队/台账落袋/assessed 事件)
// 走真 ProjectMemory + 真 TrajectorySessionLedger。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/memory_extract.hpp"
#include "app/model_router.hpp"
#include "app/turn_memory_extractor.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "memory/project_memory.hpp"
#include "runtime/idle_wake.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;
using app::TurnMemoryExtractor;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lmb-turnmem-" + std::to_string(run_id % 100000) + "-" + name + "-" +
                     std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

memory::Options EnabledOptions() {
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;  // learn 默认 review:候选进待审区
    return options;
}

std::shared_ptr<memory::ProjectMemory> MakeMemory(const fs::path& root,
                                                  memory::MemoryWriteReceiptSink* sink = nullptr) {
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

// 假后端:延迟 delay_ms 后回一段抽取 JSON(带一条 user-stated 的偏好候选)。
// honor_cancel=false 时取消旗拉了也装死(退出兜底的最坏现场)。
struct FakeExtractBackend final : lubancode::api::Backend {
    int delay_ms = 0;
    bool honor_cancel = true;
    int calls = 0;
    std::string reply =
        R"({"task_type":"research","summary":"用户定了包管理器的规矩","retrieval_terms":["pnpm"],)"
        R"("candidates":[{"kind":"preference","title":"包管理器用 pnpm","summary":"装依赖统一走 pnpm",)"
        R"("content":"装依赖统一走 pnpm,不再用 npm。","keywords":["pnpm"],"paths":[],"confidence":"user-stated"}]})";
    std::string stop_reason = "end_turn";
    bool stream_error = false;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        ++calls;
        if (stream_error) {
            on_event(lubancode::api::StreamError{"抽取后端流内错"});
            return {};
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (honor_cancel && cancel != nullptr && cancel->load()) {
                return std::unexpected(
                    lubancode::api::Error{lubancode::api::ErrorKind::Cancelled, "cancelled", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        on_event(lubancode::api::MessageStart{"msg", "cheap-m"});
        on_event(lubancode::api::TextDelta{reply});
        on_event(lubancode::api::ContentBlockDone{0});
        lubancode::api::MessageDone done;
        done.stop_reason = stop_reason;
        done.usage.input_tokens = 21;
        done.usage.output_tokens = 9;
        on_event(done);
        return {};
    }
};

TurnMemoryExtractor::Inputs MakeInputs(std::unique_ptr<lubancode::api::Backend> backend,
                                       std::uint64_t generation = 1, const std::string& turn_id = "turn-1") {
    TurnMemoryExtractor::Inputs inputs;
    inputs.backend = std::move(backend);
    inputs.model = "cheap-m";
    inputs.system_prompt = "SYSTEM";
    inputs.transcript = "TRANSCRIPT";
    inputs.task_type = "research";
    inputs.session_generation = generation;
    inputs.turn_id = turn_id;
    return inputs;
}

std::unique_ptr<FakeExtractBackend> MakeBackend(int delay_ms) {
    auto backend = std::make_unique<FakeExtractBackend>();
    backend->delay_ms = delay_ms;
    return backend;
}

bool AwaitReady(TurnMemoryExtractor& extractor, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (extractor.Ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return extractor.Ready();
}

std::optional<TurnMemoryExtractor::Outcome> AwaitFinished(TurnMemoryExtractor& extractor, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto outcome = extractor.TakeFinished()) {
            return outcome;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return extractor.TakeFinished();
}

std::vector<nlohmann::json> EventsOfKind(const fs::path& stream, const std::string& kind) {
    std::vector<nlohmann::json> found;
    const auto lines = lubancode::trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            continue;
        }
        if (parsed.value("kind", std::string()) == kind) found.push_back(parsed);
    }
    return found;
}

// V3-LEGACY-01 后新建唯一 v3:assessed 事实行落 <id>.jsonl,键名随 v3
// 合同走 camelCase(memory_extract.cpp RecordAssessedV3Locked)。
std::vector<nlohmann::json> V3EventsOfKind(const fs::path& session_dir, const std::string& kind) {
    std::vector<nlohmann::json> found;
    const fs::path stream = session_dir / fs::path(session_dir.filename().string() + ".jsonl");
    std::ifstream in(stream, std::ios::binary);
    REQUIRE(in.is_open());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) continue;
        if (parsed.value("type", std::string()) == "event" &&
            parsed.value("kind", std::string()) == kind) {
            found.push_back(parsed);
        }
    }
    return found;
}

// 门拦真路径用的最小路由(ExtractTurnMemory 顶部就解引用 model_router,
// learn off 的门虽在最前排,材料包还是得配真件)。
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

// 门拦真路径的挂名主后端:门在最前排拦下,一次也不该被叫到。
struct CountingStubBackend final : lubancode::api::Backend {
    int calls = 0;
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&,
        const std::atomic<bool>*) override {
        ++calls;
        return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "不应被叫", 0});
    }
};

}  // namespace

TEST_CASE("抽取器:Start 立即返回,结果非阻塞收货,usage/世代/轮号带回") {
    TurnMemoryExtractor extractor;
    const auto started_at = std::chrono::steady_clock::now();
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/300))));
    const auto start_elapsed = std::chrono::steady_clock::now() - started_at;
    CHECK(start_elapsed < std::chrono::milliseconds(200));  // 起飞不等网络
    CHECK(extractor.Busy());
    CHECK_FALSE(extractor.TakeFinished().has_value());  // 没完工绝不等待

    const auto outcome = AwaitFinished(extractor, /*wait_ms=*/2000);
    REQUIRE(outcome.has_value());
    CHECK(outcome->ok);
    REQUIRE(outcome->extraction.candidates.size() == 1);
    CHECK(outcome->extraction.candidates[0].title == "包管理器用 pnpm");
    CHECK(outcome->model == "cheap-m");
    CHECK(outcome->task_type == "research");
    CHECK(outcome->session_generation == 1);
    CHECK(outcome->turn_id == "turn-1");
    CHECK(outcome->accounting.usage.input_tokens == 21);
    CHECK(outcome->accounting.usage.output_tokens == 9);
    CHECK(outcome->accounting.usage_reported);
    CHECK(outcome->extract_wall_ms >= 0);

    // 收货后复位:可再起飞。
    CHECK_FALSE(extractor.Busy());
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/0), /*generation=*/2, "turn-2")));
    const auto second = AwaitFinished(extractor, 2000);
    REQUIRE(second.has_value());
    CHECK(second->session_generation == 2);
    CHECK(second->turn_id == "turn-2");
}

TEST_CASE("抽取器:单飞——上一枚没收走之前不叠发") {
    TurnMemoryExtractor extractor;
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/200))));
    CHECK_FALSE(extractor.Start(MakeInputs(std::make_unique<FakeExtractBackend>())));
    CHECK(AwaitFinished(extractor, 2000).has_value());
    // 结果待收也算忙:还没 TakeFinished 之前再 Start 照样拒。
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/0))));
    CHECK(extractor.Busy());
    CHECK_FALSE(extractor.Start(MakeInputs(std::make_unique<FakeExtractBackend>())));
    CHECK(AwaitFinished(extractor, 2000).has_value());
}

TEST_CASE("抽取器:失败半截也出账;RequestCancel 打断在飞请求") {
    TurnMemoryExtractor extractor;
    {
        auto backend = std::make_unique<FakeExtractBackend>();
        backend->stream_error = true;  // 流内错:transport_failed
        CHECK(extractor.Start(MakeInputs(std::move(backend), 3, "turn-err")));
        const auto outcome = AwaitFinished(extractor, 2000);
        REQUIRE(outcome.has_value());
        CHECK_FALSE(outcome->ok);
        CHECK(outcome->error.code == lubancode::app::ExtractionErrorCode::TransportFailed);
    }
    {
        auto backend = std::make_unique<FakeExtractBackend>();
        backend->delay_ms = 60'000;  // 不取消就跑不完
        CHECK(extractor.Start(MakeInputs(std::move(backend))));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        extractor.RequestCancel();  // 换代/退出那一下
        const auto outcome = AwaitFinished(extractor, 3000);
        REQUIRE(outcome.has_value());
        CHECK_FALSE(outcome->ok);  // 取消收场:调用方按世代门处置
    }
}

TEST_CASE("抽取器:析构时在飞也不挂——取消 + 有界收尾 + detach 放行") {
    auto backend = std::make_unique<FakeExtractBackend>();
    backend->delay_ms = 60'000;
    backend->honor_cancel = false;  // 最坏现场:取消旗拉了也装死
    const auto t0 = std::chrono::steady_clock::now();
    {
        TurnMemoryExtractor extractor;
        CHECK(extractor.Start(MakeInputs(std::move(backend))));
    }  // 析构:取消 → 有界等待(5 秒窗)→ detach 放行,不 terminate
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < std::chrono::seconds(15));
    CHECK(true);  // 到这里就是过了:析构未挂死、未 terminate
}

TEST_CASE("Ready 真值表与唤醒源形制:running 不醒、finished 醒、收完不再醒") {
    lubancode::runtime::IdleWakeCoordinator wakes;
    TurnMemoryExtractor extractor;
    // 与 interactive_session_assembly 的挂法同款:ready 只问 Ready()。
    const auto token = wakes.AddSource("memory_extract", [&extractor]() { return extractor.Ready(); });
    CHECK(token.valid());
    CHECK_FALSE(wakes.AnyReady());  // 未启动:不醒
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/150))));
    CHECK(extractor.Busy());
    CHECK_FALSE(wakes.AnyReady());  // 运行中:不空醒(Ready 才是唤醒条件)
    REQUIRE(AwaitReady(extractor, 2000));
    CHECK(wakes.AnyReady());  // 完工:叫醒主循环收账
    REQUIRE(extractor.TakeFinished().has_value());
    CHECK_FALSE(wakes.AnyReady());  // 收完:不再醒,不空转
}

// ---------------------------------------------------------------------------
// 迟到收账:SettleTurnMemory 全链 + MemoryTurnLedger 悬账三态
// (Suspend/Settle 对档/Abandon 弃账)。
// ---------------------------------------------------------------------------
TEST_CASE("迟到收账:门过的回合悬账,完工后对档落袋——候选入队 + assessed 事件") {
    const fs::path root = TempRoot("settle-ok");

    lubancode::runtime::TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = root / "workspaces";
    ledger_options.workspace_root = root / "repo";
    ledger_options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    ledger_options.lubancode_version = "test";
    auto session = lubancode::runtime::TrajectorySessionLedger::Open(ledger_options);
    REQUIRE(session.has_value());

    lubancode::app::MemoryTurnLedger ledger(&*session);
    auto store = MakeMemory(root / "mem");
    const lubancode::cli::Theme theme;

    TurnMemoryExtractor extractor;
    const std::string text = "以后统一用 pnpm 装依赖，别再用 npm 了，把 README 的安装段也修一下";
    ledger.BeginTurn(session->session_id(), "turn-settle", text);
    ledger.NoteExtractionCalled();  // 门过的等价记账:decision=called 进悬账
    // 门过起飞(材料在真实链路由前台拼好,这里直连执行器等价覆盖收账合同)。
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/80), /*generation=*/1, "turn-settle")));
    ledger.SuspendTurn();  // 收口悬账(控制器的 DispatchTurnMemory 同款次序)
    const auto outcome = AwaitFinished(extractor, 2000);
    REQUIRE(outcome.has_value());
    CHECK(outcome->ok);

    lubancode::app::SessionTailContext tail;
    tail.project_memory = store.get();
    tail.theme = &theme;
    tail.memory_turns = &ledger;
    lubancode::app::SettleTurnMemory(tail, *outcome, /*tail_wall_ms=*/17);

    // 候选落待审区(review 档):一条 preference。
    const auto candidates = store->ListCandidates();
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].title == "包管理器用 pnpm");
    CHECK(candidates[0].task_type == "research");
    // 漏斗:门过记过 called,收账补 outcome——failed 零。
    CHECK(ledger.funnel().extract_batches == 1);
    CHECK(ledger.funnel().extract_failures == 0);
    // assessed 事件:completed、候选数、usage、墙钟全落。
    const auto assessed =
        V3EventsOfKind(session->session_dir(), "memory.extraction.assessed");
    REQUIRE(assessed.size() == 1);
    const auto& payload = assessed[0].at("payload");
    CHECK(payload.value("decision", std::string()) == "called");
    CHECK(payload.value("extractOutcome", std::string()) == "completed");
    CHECK(payload.value("reviewCandidates", 0) == 1);
    CHECK(payload.value("usageReported", false));
    if (payload.contains("inputTokens")) {  // nlohmann 缺键 UB 铁律:contains 先行
        CHECK(payload.value("inputTokens", std::int64_t{0}) == 21);
        CHECK(payload.value("outputTokens", std::int64_t{0}) == 9);
    }
}

TEST_CASE("账落对档:悬账被新轮冲掉记 aborted,迟到 outcome 对不上不落第二枚") {
    lubancode::app::MemoryTurnLedger ledger(nullptr);
    ledger.BeginTurn("sess", "turn-a", "正文");
    ledger.NoteExtractionCalled();
    ledger.SuspendTurn();  // turn-a 悬账

    // 新轮先开张:悬账以 aborted 口径落袋(decision=called、outcome 缺席)。
    ledger.BeginTurn("sess", "turn-b", "下一问");
    CHECK_FALSE(ledger.SettleSuspendedTurn("turn-a", /*settle_wall_ms=*/9, nullptr));

    // 迟到的失败也数进漏斗(一场会话的聚合,不因回合翻篇丢数)。
    lubancode::app::MemoryTurnLedger::ExtractOutcome late;
    late.ok = false;
    late.error_code = "transport_failed";
    CHECK_FALSE(ledger.SettleSuspendedTurn("turn-a", 9, &late));
    CHECK(ledger.funnel().extract_failures == 1);
    ledger.FinishTurn(1);
}

TEST_CASE("换代弃迟到:AbandonSuspendedTurn 清悬账不落盘,世代原样带回") {
    const fs::path root = TempRoot("abandon");
    lubancode::app::MemoryTurnLedger ledger(nullptr);
    auto store = MakeMemory(root / "mem");
    const lubancode::cli::Theme theme;

    TurnMemoryExtractor extractor;
    ledger.BeginTurn("sess-old", "turn-old", "正文");
    ledger.NoteExtractionCalled();  // 门过的等价记账:真起飞过,漏斗不抹
    CHECK(extractor.Start(MakeInputs(MakeBackend(/*delay_ms=*/60), /*generation=*/1, "turn-old")));
    ledger.SuspendTurn();
    const auto outcome = AwaitFinished(extractor, 2000);
    REQUIRE(outcome.has_value());
    CHECK(outcome->session_generation == 1);  // 世代原样带回:收货点的世代门靠它

    // /clear 的换代块(DispatchSlashCommand 同款):取消 + 悬账弃。
    extractor.RequestCancel();
    ledger.AbandonSuspendedTurn();
    // 收货点过世代门(1 != 当前 2):一票不落新场——SettleTurnMemory 不调,
    // 候选区空、悬账已清(迟到的 Settle 对不上档,不落盘)。
    CHECK_FALSE(ledger.SettleSuspendedTurn("turn-old", 5, nullptr));
    CHECK(store->ListCandidates().empty());
    CHECK(ledger.funnel().extract_batches == 1);  // 起飞事实不抹(真发过)
}

TEST_CASE("门拦不发:learn off 的回合前台即收,执行器零起飞") {
    auto store = MakeMemory(TempRoot("gate-off") / "mem");
    CHECK(store->set_learn(memory::LearnMode::Off).has_value());  // learn off:门在最前排拦下
    const lubancode::cli::Theme theme;
    lubancode::app::MemoryTurnLedger ledger(nullptr);
    TurnMemoryExtractor extractor;

    CountingStubBackend backend;
    auto current_model = std::make_shared<std::string>("test-model");
    std::string active_provider = "local";
    const auto config = RouterConfig();
    lubancode::app::ModelRouterService router(config, backend, current_model, active_provider);

    lubancode::app::SessionTailContext tail;
    tail.project_memory = store.get();
    tail.model_router = &router;
    tail.theme = &theme;
    tail.memory_turns = &ledger;
    tail.extractor = &extractor;
    tail.session_generation = 1;
    tail.turn_id = "turn-off";

    ledger.BeginTurn("sess", "turn-off", "正文");
    CHECK(lubancode::app::ExtractTurnMemory(tail, "正文", /*history_before=*/0) ==
          lubancode::app::TurnMemoryDispatch::Skipped);
    CHECK_FALSE(extractor.Busy());  // 没起飞
    CHECK(backend.calls == 0);      // 门拦下真不发
    CHECK(ledger.funnel().skipped_disabled == 1);
    CHECK(ledger.funnel().extract_batches == 0);
    ledger.FinishTurn(0);
}
