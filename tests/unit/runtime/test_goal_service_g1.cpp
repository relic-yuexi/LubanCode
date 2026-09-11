// GoalService 测试(轨迹 v3 §4.67 Goal 模式 G1:命令接线与恢复去重):
//   - pendingIntent 定型:GoalPendingIntent roundtrip/合同校验/去重键;
//   - 意图提交(SetPendingIntent):CAS、前一枚未认领不覆盖、认领后可换、
//     对着旧合同拒;
//   - 认领(ClaimPendingIntent):claimed+writerEpoch+phase=queued 落 applied;
//     同写者幂等;他写者按相位分路(G2 定案:queued=确认未发送,接管沿用
//     原工作项;running/evaluating=在途,goal.intent_already_claimed 不盲
//     重放);缺意图/停态拒;
//   - 开轮/收工(BeginIteration/EndIteration):iterationId 发号单调落快照
//     (不再恒 null)、preparing→active、收口销 intent 不假装评过;
//   - EvaluateGoalWork 可用性矩阵(未认领/本写者待开轮/他写者已认领/
//     旧合同/停态);
//   - 崩溃窗口幂等:快照落稳、applied 未落,同字节重试复用候选文件补账;
//   - 跨卷接管:adoptedFrom 凭据 + 单卷投影认半路续接,伪造无凭据/凭据
//     不衔接拒;
//   - ProjectGoalLineage:穿 resume 边、clear 边不穿、多跳、回环护栏;
//   - 恢复去重:claim 落账后重复接管不再补队列(同一 workItemId 恰好
//     消费一次)。
// G0 的提交事务/投影缺口矩阵在 test_goal_service.cpp;本册只钉 G1 面。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "runtime/goal_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalLifecycle;
using goalns::GoalPendingIntent;
using goalns::GoalPhase;
using goalns::GoalService;
using goalns::GoalStateSnapshot;
using goalns::GoalTransitionCandidate;
using lubancode::trajectory::v3::Durability;
using lubancode::trajectory::v3::EventDraft;
using lubancode::trajectory::v3::EventKindV3;
using lubancode::trajectory::v3::V3Writer;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(同款见
// test_goal_service.cpp)。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshDir(const std::string& tag) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("lubancode-goal-g1-" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::int64_t FixedClock() { return 1700000000000; }

// 一只 v3 写者(新卷 Start)。session_id 须与目录名一致(lineage 走
// FindV3SessionStream 的 <dir>/<id>.jsonl 约定)。
struct Volume {
    std::filesystem::path dir;
    std::string session_id;
    std::optional<V3Writer> writer;

    Volume(const std::string& tag, const std::string& sid, bool fail_applied = false)
        : dir(FreshDir(tag)), session_id(sid) {
        lubancode::trajectory::v3::V3WriterOptions options;
        if (fail_applied) {
            options.inject_io_failure = []() -> std::optional<std::string> {
                return std::optional<std::string>("inject.applied_failure");
            };
        }
        auto started = V3Writer::Start(dir / (sid + ".jsonl"), sid, "run-" + sid, "system prompt",
                                       nlohmann::json::object(), std::move(options));
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }

    std::filesystem::path jsonl() const { return dir / (session_id + ".jsonl"); }

    Volume(const Volume&) = delete;
    Volume& operator=(const Volume&) = delete;
};

GoalService::Options ServiceOptionsFor(const std::filesystem::path& session_dir) {
    GoalService::Options options;
    options.session_dir = session_dir;
    options.clock = FixedClock;
    return options;
}

GoalService::Options ServiceOptions(const Volume& volume) {
    return ServiceOptionsFor(volume.dir);
}

GoalStateSnapshot DraftObjective(std::string objective) {
    GoalStateSnapshot draft;
    draft.objective = std::move(objective);
    draft.workspace_root = "/repo";
    draft.workspace_identity = "repo@main";
    return draft;
}

GoalPendingIntent FirstIntent() {
    GoalPendingIntent intent;
    intent.work_item_id = "wi-1";
    intent.contract_revision = 1;
    intent.predecessor_iteration_id = "";  // 首轮
    intent.continuation_ordinal = 1;
    return intent;
}

GoalTransitionCandidate Transition(GoalLifecycle to, std::uint64_t expected_state_revision) {
    GoalTransitionCandidate candidate;
    candidate.to_lifecycle = to;
    candidate.expected_state_revision = expected_state_revision;
    return candidate;
}

// 写一枚 session.json(lineage 走 ReadSessionJson;必选键按 schema)。
void WriteSessionJson(const std::filesystem::path& session_dir, const std::string& session_id,
                      const std::string& start_reason, const std::string& previous_session_id) {
    nlohmann::json manifest = nlohmann::json::object();
    manifest["schema_version"] = 2;
    manifest["workspace_key"] = "wk-test";
    manifest["session_id"] = session_id;
    manifest["main_run_id"] = "run-" + session_id;
    manifest["status"] = "running";
    manifest["start_reason"] = start_reason;
    if (!previous_session_id.empty()) {
        manifest["previous_session_id"] = previous_session_id;
    }
    std::ofstream file(session_dir / "session.json", std::ios::binary | std::ios::trunc);
    file << manifest.dump();
}

}  // namespace

TEST_CASE("pendingIntent 定型:roundtrip、合同校验、去重键") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");

    GoalPendingIntent intent = FirstIntent();
    const nlohmann::json json = intent.ToJson();
    CHECK(json.at("workItemId") == "wi-1");
    CHECK(json.at("claimed") == false);
    std::string error;
    const auto parsed = GoalPendingIntent::FromJson(json, &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->contract_revision == 1);
    CHECK(parsed->continuation_ordinal == 1);
    CHECK(parsed->predecessor_iteration_id.empty());
    CHECK(parsed->DedupeKey("goal-1") == "goal-1|1||1");

    // 合同:缺字段 / ordinal 0 / contractRevision 0 拒。
    nlohmann::json bad = json;
    bad.erase("workItemId");
    CHECK_FALSE(goalns::ValidatePendingIntent(bad).empty());
    bad = json;
    bad["continuationOrdinal"] = 0;
    CHECK_FALSE(goalns::ValidatePendingIntent(bad).empty());
    bad = json;
    bad["contractRevision"] = 0;
    CHECK_FALSE(goalns::ValidatePendingIntent(bad).empty());
    // claim 面自洽:claimed=true 须带 epoch+时点;false 时不许带。
    bad = json;
    bad["claimed"] = true;
    CHECK_FALSE(goalns::ValidatePendingIntent(bad).empty());
    GoalPendingIntent claimed = FirstIntent();
    claimed.claimed = true;
    claimed.writer_epoch = "run-x";
    claimed.claimed_at_ms = 1700;
    CHECK(goalns::ValidatePendingIntent(claimed.ToJson()).empty());
}

TEST_CASE("CreateGoal 带首轮意图:快照与投影可见,坏草稿拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("create-intent", "s-create");
    GoalService service(&*volume.writer, ServiceOptions(volume));

    GoalStateSnapshot draft = DraftObjective("修好 auth;ctest -R auth 全过");
    draft.pending_intent = FirstIntent().ToJson();
    const auto created = service.CreateGoal(std::move(draft), nlohmann::json{{"command", "/goal"}});
    REQUIRE(created.ok);
    REQUIRE(service.current() != nullptr);
    std::string error;
    const auto intent = GoalPendingIntent::FromJson(service.current()->pending_intent, &error);
    REQUIRE(intent.has_value());
    CHECK(intent->work_item_id == "wi-1");
    CHECK_FALSE(intent->claimed);

    // 投影侧同款可见(单一读面)。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger.has_value());
    const auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
    CHECK(projection.gap == goalns::GoalProjectionGap::None);
    CHECK(projection.has_goal);
    CHECK(projection.snapshot.pending_intent.at("workItemId") == "wi-1");

    // 坏草稿(缺字段的意图)不落盘。
    Volume volume2("create-bad", "s-create2");
    GoalService service2(&*volume2.writer, ServiceOptions(volume2));
    GoalStateSnapshot bad_draft = DraftObjective("坏意图");
    bad_draft.pending_intent = nlohmann::json{{"workItemId", "wi-1"}};  // 缺字段
    const auto rejected = service2.CreateGoal(std::move(bad_draft), {});
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.error_code == goalns::kErrGoalCandidateInvalid);
    // 预置 claimed 的首轮意图也拒(claim 只走 ClaimPendingIntent)。
    GoalStateSnapshot pre_claimed = DraftObjective("预置认领");
    GoalPendingIntent claimed_intent = FirstIntent();
    claimed_intent.claimed = true;
    claimed_intent.writer_epoch = "run-x";
    claimed_intent.claimed_at_ms = 1700;
    pre_claimed.pending_intent = claimed_intent.ToJson();
    const auto rejected2 = service2.CreateGoal(std::move(pre_claimed), {});
    CHECK_FALSE(rejected2.ok);
}

TEST_CASE("SetPendingIntent:CAS、未认领不覆盖、认领后可换、旧合同拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("set-intent", "s-set");
    GoalService service(&*volume.writer, ServiceOptions(volume));
    GoalStateSnapshot draft = DraftObjective("目标 A");
    draft.pending_intent = FirstIntent().ToJson();  // 首轮意图在账
    REQUIRE(service.CreateGoal(std::move(draft), {}).ok);
    const std::uint64_t rev = service.current()->state_revision;

    GoalPendingIntent next = FirstIntent();
    next.work_item_id = "wi-2";
    next.predecessor_iteration_id = "goal-1/iter-1";
    next.continuation_ordinal = 2;
    // 前一枚(wi-1,CreateGoal 带的首轮意图)未认领:不许覆盖。
    const auto conflict = service.SetPendingIntent(next, rev, {});
    CHECK_FALSE(conflict.ok);
    CHECK(conflict.error_code == goalns::kErrGoalIntentConflict);

    // 同键未认领的重试(同字节幂等路)不算覆盖:同 dedupe key 放行。
    const auto same_key = service.SetPendingIntent(FirstIntent(), rev, {});
    CHECK(same_key.ok);

    // CAS:旧 revision 拒。
    const auto stale = service.SetPendingIntent(FirstIntent(), rev, {});
    CHECK_FALSE(stale.ok);  // 上一笔已把 revision 抬到 rev+1

    // 对着旧合同拒:改合同后(c2)再交带 r1 的意图。
    const std::uint64_t rev2 = service.current()->state_revision;
    goalns::GoalContract contract = service.current()->contract;
    contract.objective = "目标 A(改)";
    REQUIRE(service.AmendContract(contract, rev2, service.current()->contract_revision, {}).ok);
    GoalPendingIntent stale_contract = FirstIntent();
    stale_contract.contract_revision = 1;  // 在账已是 r2
    const auto old_contract = service.SetPendingIntent(
        stale_contract, service.current()->state_revision, {});
    CHECK_FALSE(old_contract.ok);
    CHECK(old_contract.error_code == goalns::kErrGoalIntentConflict);

    // 认领销账后可交下一枚:claim wi-1(同键覆盖的那枚),再交 wi-2。
    REQUIRE(service.ClaimPendingIntent("run-set", service.current()->state_revision, {}).ok);
    GoalPendingIntent wi2 = FirstIntent();
    wi2.work_item_id = "wi-2";
    wi2.contract_revision = service.current()->contract_revision;
    wi2.predecessor_iteration_id = "goal-1/iter-1";
    wi2.continuation_ordinal = 2;
    const auto replaced = service.SetPendingIntent(wi2, service.current()->state_revision, {});
    CHECK(replaced.ok);
    CHECK(service.current()->pending_intent.at("workItemId") == "wi-2");
}

TEST_CASE("ClaimPendingIntent:claim 落账+queued,同写者幂等,他写者拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("claim", "s-claim");
    GoalService service(&*volume.writer, ServiceOptions(volume));
    GoalStateSnapshot draft = DraftObjective("认领语义");
    draft.pending_intent = FirstIntent().ToJson();
    REQUIRE(service.CreateGoal(std::move(draft), {}).ok);

    const std::uint64_t rev1 = service.current()->state_revision;
    const auto claim = service.ClaimPendingIntent("run-1", rev1, {});
    REQUIRE(claim.ok);
    CHECK(claim.payload.at("workItemId") == "wi-1");
    CHECK(service.current()->phase == GoalPhase::Queued);
    std::string error;
    auto intent = GoalPendingIntent::FromJson(service.current()->pending_intent, &error);
    REQUIRE(intent.has_value());
    CHECK(intent->claimed);
    CHECK(intent->writer_epoch == "run-1");
    CHECK(intent->claimed_at_ms > 0);

    // 同写者幂等(回执丢了的重试):ok 且 revision 不动。
    const auto again = service.ClaimPendingIntent("run-1", service.current()->state_revision, {});
    CHECK(again.ok);
    CHECK(again.payload.value("idempotent", false) == true);
    CHECK(service.current()->state_revision == rev1 + 1);

    // 他写者接管(G2 收口):phase=queued = claim 落账、开轮没落(开轮
    // 先于模型发送)= 账面证据"确认未发送"——接管沿用原工作项,续原请求
    // 不重放副作用;接管本身落 applied。
    const auto other = service.ClaimPendingIntent("run-2", service.current()->state_revision, {});
    CHECK(other.ok);
    CHECK(other.payload.at("workItemId") == "wi-1");
    CHECK(other.payload.at("adoptedFromEpoch") == "run-1");
    CHECK(service.current()->state_revision == rev1 + 2);
    {
        std::string intent_error;
        const auto taken = GoalPendingIntent::FromJson(service.current()->pending_intent,
                                                       &intent_error);
        REQUIRE(taken.has_value());
        CHECK(taken->writer_epoch == "run-2");
    }
    // 他写者已在途(phase=running):真拒,不盲重放(§4.67.8)。
    REQUIRE(service.BeginIteration(service.current()->state_revision, {}).ok);
    const auto in_flight = service.ClaimPendingIntent("run-3", service.current()->state_revision, {});
    CHECK_FALSE(in_flight.ok);
    CHECK(in_flight.error_code == goalns::kErrGoalIntentAlreadyClaimed);

    // 停态不认领。
    Volume volume2("claim-stopped", "s-claim2");
    GoalService service2(&*volume2.writer, ServiceOptions(volume2));
    GoalStateSnapshot draft2 = DraftObjective("停态认领");
    draft2.pending_intent = FirstIntent().ToJson();
    REQUIRE(service2.CreateGoal(std::move(draft2), {}).ok);
    GoalTransitionCandidate pause = Transition(GoalLifecycle::Paused,
                                               service2.current()->state_revision);
    pause.stop_reason = "user_pause";
    REQUIRE(service2.ApplyTransition(pause).ok);
    const auto stopped = service2.ClaimPendingIntent("run-1", service2.current()->state_revision, {});
    CHECK_FALSE(stopped.ok);
    CHECK(stopped.error_code == goalns::kErrGoalCandidateInvalid);

    // 没有意图:goal.intent_missing。
    Volume volume3("claim-missing", "s-claim3");
    GoalService service3(&*volume3.writer, ServiceOptions(volume3));
    REQUIRE(service3.CreateGoal(DraftObjective("无意图"), {}).ok);
    const auto missing = service3.ClaimPendingIntent("run-1", service3.current()->state_revision, {});
    CHECK_FALSE(missing.ok);
    CHECK(missing.error_code == goalns::kErrGoalIntentMissing);
}

TEST_CASE("BeginIteration/EndIteration:iterationId 发号单调,收口销 intent") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("iteration", "s-iter");
    GoalService service(&*volume.writer, ServiceOptions(volume));
    GoalStateSnapshot draft = DraftObjective("轮次归属");
    draft.pending_intent = FirstIntent().ToJson();
    REQUIRE(service.CreateGoal(std::move(draft), {}).ok);
    REQUIRE(service.ClaimPendingIntent("run-iter", service.current()->state_revision, {}).ok);

    // 开轮:preparing → active,iterationId 落快照(不再恒 null)。
    const auto began = service.BeginIteration(service.current()->state_revision, {});
    REQUIRE(began.ok);
    CHECK(began.payload.at("iterationId") == "goal-1/iter-1");
    CHECK(service.current()->lifecycle == GoalLifecycle::Active);
    CHECK(service.current()->phase == GoalPhase::Running);
    CHECK(service.current()->iteration_id.has_value());
    CHECK(*service.current()->iteration_id == "goal-1/iter-1");
    CHECK(service.current()->counters.iterations_started == 1);

    // 已有轮在跑:再开拒。
    CHECK_FALSE(service.BeginIteration(service.current()->state_revision, {}).ok);

    // 收工:回 idle、pendingIntent 销账(不自动续排,判词归 G2)。
    const auto ended = service.EndIteration(service.current()->state_revision, {});
    REQUIRE(ended.ok);
    CHECK(service.current()->phase == GoalPhase::Idle);
    CHECK(service.current()->pending_intent.empty());
    CHECK(service.current()->iteration_id.has_value());  // 记录保留(status 可见)

    // 第二轮(新意图→认领→开轮):iter-2,单调。
    GoalPendingIntent second = FirstIntent();
    second.work_item_id = "wi-2";
    second.predecessor_iteration_id = "goal-1/iter-1";
    second.continuation_ordinal = 2;
    REQUIRE(service.SetPendingIntent(second, service.current()->state_revision, {}).ok);
    REQUIRE(service.ClaimPendingIntent("run-iter", service.current()->state_revision, {}).ok);
    const auto began2 = service.BeginIteration(service.current()->state_revision, {});
    REQUIRE(began2.ok);
    CHECK(began2.payload.at("iterationId") == "goal-1/iter-2");
    CHECK(service.current()->counters.iterations_started == 2);

    // 停态不开轮。
    Volume volume2("iteration-stopped", "s-iter2");
    GoalService service2(&*volume2.writer, ServiceOptions(volume2));
    GoalStateSnapshot draft2 = DraftObjective("停态开轮");
    draft2.pending_intent = FirstIntent().ToJson();
    REQUIRE(service2.CreateGoal(std::move(draft2), {}).ok);
    GoalTransitionCandidate pause = Transition(GoalLifecycle::Paused,
                                               service2.current()->state_revision);
    pause.stop_reason = "user_pause";
    REQUIRE(service2.ApplyTransition(pause).ok);
    CHECK_FALSE(service2.BeginIteration(service2.current()->state_revision, {}).ok);
    // 不在执行轮:收工拒。
    CHECK_FALSE(service2.EndIteration(service2.current()->state_revision, {}).ok);
}

TEST_CASE("EvaluateGoalWork 可用性矩阵") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalStateSnapshot snapshot;
    snapshot.goal_id = "goal-1";
    snapshot.session_id = "s";
    snapshot.run_id = "r";
    snapshot.objective = "o";
    snapshot.contract_revision = 1;
    snapshot.lifecycle = GoalLifecycle::Active;
    snapshot.phase = GoalPhase::Idle;
    snapshot.pending_intent = FirstIntent().ToJson();

    // 未认领 + 活动:可补(恢复按原 workItemId)。
    auto view = goalns::EvaluateGoalWork(snapshot, "run-now");
    CHECK(view.claimable);
    CHECK(view.intent.work_item_id == "wi-1");

    // 停态:不排。
    snapshot.lifecycle = GoalLifecycle::Paused;
    snapshot.phase = GoalPhase::Idle;
    snapshot.stop_reason = "user_pause";
    view = goalns::EvaluateGoalWork(snapshot, "run-now");
    CHECK_FALSE(view.claimable);

    // 他写者已认领:claimed_by_other(恢复核验,不重放)。
    snapshot.lifecycle = GoalLifecycle::Active;
    GoalPendingIntent claimed = FirstIntent();
    claimed.claimed = true;
    claimed.writer_epoch = "run-old";
    claimed.claimed_at_ms = 1700;
    snapshot.pending_intent = claimed.ToJson();
    view = goalns::EvaluateGoalWork(snapshot, "run-now");
    CHECK_FALSE(view.claimable);
    CHECK(view.claimed_by_other);

    // 本写者已认领且待开轮(queued):沿用原项。
    snapshot.phase = GoalPhase::Queued;
    view = goalns::EvaluateGoalWork(snapshot, "run-old");
    CHECK(view.claimable);
    // 本写者已认领但轮已在跑:不重复。
    snapshot.phase = GoalPhase::Running;
    view = goalns::EvaluateGoalWork(snapshot, "run-old");
    CHECK_FALSE(view.claimable);

    // 意图对着旧合同(edit 后):不排(旧工作项不挤过边界命令)。
    GoalPendingIntent stale = FirstIntent();
    stale.claimed = false;
    stale.writer_epoch.clear();
    stale.claimed_at_ms = 0;
    stale.contract_revision = 1;
    snapshot.contract_revision = 2;
    snapshot.phase = GoalPhase::Idle;
    snapshot.pending_intent = stale.ToJson();
    view = goalns::EvaluateGoalWork(snapshot, "run-now");
    CHECK_FALSE(view.claimable);

    // 没有意图。
    snapshot.pending_intent = nlohmann::json::object();
    view = goalns::EvaluateGoalWork(snapshot, "run-now");
    CHECK_FALSE(view.claimable);
    CHECK_FALSE(view.has_intent);
}

TEST_CASE("崩溃窗口幂等:快照落稳、applied 未落,同字节重试复用候选文件") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const std::filesystem::path dir = FreshDir("crash-retry");
    const std::string sid = "s-crash";
    {
        lubancode::trajectory::v3::V3WriterOptions options;
        bool fail_now = false;
        options.inject_io_failure = [&fail_now]() -> std::optional<std::string> {
            return fail_now ? std::optional<std::string>("inject.applied_failure")
                            : std::nullopt;
        };
        auto started = V3Writer::Start(dir / (sid + ".jsonl"), sid, "run-" + sid, "system prompt",
                                       nlohmann::json::object(), std::move(options));
        REQUIRE(started.has_value());
        V3Writer writer = std::move(*started);
        GoalService service(&writer, ServiceOptionsFor(dir));
        GoalStateSnapshot draft = DraftObjective("崩溃重试");
        draft.pending_intent = FirstIntent().ToJson();
        fail_now = true;
        const auto failed = service.CreateGoal(std::move(draft), {});
        CHECK_FALSE(failed.ok);
        CHECK(service.broken());
        // 候选快照在盘上(rev-000001),applied 没落。
        CHECK(std::filesystem::exists(dir / "state/goals/goal-1/rev-000001.json"));
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(dir / (sid + ".jsonl"));
        REQUIRE(ledger.has_value());
        CHECK(goalns::ProjectGoalState(*ledger, dir).gap == goalns::GoalProjectionGap::NoGoal);
    }
    // 重来:干净写者续卷 + 新服务,同字节重试 → 复用候选文件,applied 补账。
    auto continued = V3Writer::Continue(dir / (sid + ".jsonl"));
    REQUIRE(continued.has_value());
    GoalService service(&*continued, ServiceOptionsFor(dir));
    GoalStateSnapshot retry = DraftObjective("崩溃重试");
    retry.pending_intent = FirstIntent().ToJson();
    const auto created = service.CreateGoal(std::move(retry), {});
    REQUIRE(created.ok);
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(dir / (sid + ".jsonl"));
    REQUIRE(ledger.has_value());
    const auto projection = goalns::ProjectGoalState(*ledger, dir);
    CHECK(projection.gap == goalns::GoalProjectionGap::None);
    CHECK(projection.has_goal);
    CHECK(projection.snapshot.pending_intent.at("workItemId") == "wi-1");
}

TEST_CASE("跨卷接管:adoptedFrom 凭据,单卷投影认半路续接,伪造拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume source("cross-src", "s-src");
    GoalService src_service(&*source.writer, ServiceOptions(source));
    GoalStateSnapshot draft = DraftObjective("跨卷目标");
    draft.pending_intent = FirstIntent().ToJson();
    REQUIRE(src_service.CreateGoal(std::move(draft), {}).ok);
    const auto src_ledger = lubancode::trajectory::v3::ReadV3Ledger(source.jsonl());
    REQUIRE(src_ledger.has_value());
    const auto src_projection = goalns::ProjectGoalState(*src_ledger, source.dir);
    REQUIRE(src_projection.gap == goalns::GoalProjectionGap::None);

    // 新卷新写者:AdoptFromProjection 接管(§4.67.8 单写者接管)。
    Volume target("cross-dst", "s-dst");
    GoalService dst_service(&*target.writer, ServiceOptions(target));
    const auto adopted = dst_service.AdoptFromProjection(src_projection);
    REQUIRE(adopted.ok);
    CHECK(dst_service.current()->goal_id == "goal-1");
    CHECK(dst_service.current()->state_revision == src_projection.snapshot.state_revision);

    // 接管后的首笔提交:applied 带 adoptedFrom{sessionId,stateRevision}。
    const auto claim = dst_service.ClaimPendingIntent("run-dst",
                                                      dst_service.current()->state_revision, {});
    REQUIRE(claim.ok);
    const auto dst_ledger = lubancode::trajectory::v3::ReadV3Ledger(target.jsonl());
    REQUIRE(dst_ledger.has_value());
    bool found = false;
    for (const auto& event : dst_ledger->events) {
        if (event.kind == EventKindV3::StateGoalApplied) {
            found = true;
            REQUIRE(event.payload.contains("adoptedFrom"));
            CHECK(event.payload.at("adoptedFrom").at("sessionId") == "s-src");
            CHECK(event.payload.at("adoptedFrom").at("stateRevision") ==
                  event.payload.at("fromStateRevision"));
        }
    }
    CHECK(found);
    // 单卷投影认半路续接:无缺口。
    const auto dst_projection = goalns::ProjectGoalState(*dst_ledger, target.dir);
    CHECK(dst_projection.gap == goalns::GoalProjectionGap::None);
    CHECK(dst_projection.has_goal);
    CHECK(dst_projection.session_id == "s-dst");
    // 续接后的快照落在目标卷(接管写面搬家)。
    CHECK(std::filesystem::exists(
        target.dir / goalns::SnapshotRefPath("goal-1", dst_service.current()->state_revision)));

    // 伪造:无凭据的半路首条 → 单行合同过(缺 adoptedFrom 不违法),跨行
    // 序列校验在投影报 illegal;凭据不衔接 → schema3 单行合同就拒,落不了账。
    {
        Volume fake("cross-fake0", "s-fake0");
        EventDraft draft_event;
        draft_event.kind = EventKindV3::StateGoalApplied;
        draft_event.payload["goalId"] = "goal-9";
        draft_event.payload["fromStateRevision"] = 3;
        draft_event.payload["toStateRevision"] = 4;
        draft_event.payload["contractRevision"] = 1;
        draft_event.payload["snapshotRef"] = "state/goals/goal-9/rev-000004.json";
        draft_event.payload["snapshotSha256"] = std::string(64, 'a');
        draft_event.payload["lifecycle"] = "active";
        const auto receipt = fake.writer->AppendEvent(std::move(draft_event), Durability::PowerLoss);
        REQUIRE(receipt.status == lubancode::trajectory::v3::WriteReceipt::Status::Committed);
        const auto fake_ledger = lubancode::trajectory::v3::ReadV3Ledger(fake.jsonl());
        REQUIRE(fake_ledger.has_value());
        const auto fake_projection = goalns::ProjectGoalState(*fake_ledger, fake.dir);
        CHECK(fake_projection.gap == goalns::GoalProjectionGap::IllegalTransition);
    }
    {
        Volume fake("cross-fake1", "s-fake1");
        EventDraft draft_event;
        draft_event.kind = EventKindV3::StateGoalApplied;
        draft_event.payload["goalId"] = "goal-9";
        draft_event.payload["fromStateRevision"] = 3;
        draft_event.payload["toStateRevision"] = 4;
        draft_event.payload["contractRevision"] = 1;
        draft_event.payload["snapshotRef"] = "state/goals/goal-9/rev-000004.json";
        draft_event.payload["snapshotSha256"] = std::string(64, 'a');
        draft_event.payload["lifecycle"] = "active";
        draft_event.payload["adoptedFrom"] =
            nlohmann::json{{"sessionId", "s-other"}, {"stateRevision", 2}};  // != from
        const auto receipt = fake.writer->AppendEvent(std::move(draft_event), Durability::PowerLoss);
        CHECK(receipt.status == lubancode::trajectory::v3::WriteReceipt::Status::Rejected);
    }
}

TEST_CASE("ProjectGoalLineage:穿 resume 边,clear 边不穿,多跳与回环") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    // 三卷链:C(resume←B(resume←A(launch))),goal 只在 A。
    Volume a("lineage-a", "s-a");
    Volume b("lineage-b", "s-b");
    Volume c("lineage-c", "s-c");
    WriteSessionJson(a.dir, "s-a", "process_launch", "");
    WriteSessionJson(b.dir, "s-b", "resume", "s-a");
    WriteSessionJson(c.dir, "s-c", "resume", "s-b");
    GoalService service_a(&*a.writer, ServiceOptions(a));
    GoalStateSnapshot draft = DraftObjective("链上目标");
    draft.pending_intent = FirstIntent().ToJson();
    REQUIRE(service_a.CreateGoal(std::move(draft), {}).ok);

    // 从 C 起:穿两跳 resume 到 A。
    auto lineage = goalns::ProjectGoalLineage(c.dir);
    CHECK(lineage.found);
    CHECK(lineage.projection.gap == goalns::GoalProjectionGap::None);
    CHECK(lineage.projection.has_goal);
    CHECK(lineage.projection.snapshot.goal_id == "goal-1");
    CHECK(lineage.walked.size() == 3);

    // B 也写了 goal:从 C2(resume←B)起取最近一份,不再往 A 走到底。
    Volume c2("lineage-c2", "s-c2");
    WriteSessionJson(c2.dir, "s-c2", "resume", "s-b");
    GoalService service_b(&*b.writer, ServiceOptions(b));
    GoalStateSnapshot draft_b = DraftObjective("B 卷目标");
    GoalPendingIntent b_intent = FirstIntent();
    b_intent.work_item_id = "wi-9";
    draft_b.pending_intent = b_intent.ToJson();
    // B 卷还没有 goal;直接建(goalId 从 1 起,各卷独立)。
    REQUIRE(service_b.CreateGoal(std::move(draft_b), {}).ok);
    lineage = goalns::ProjectGoalLineage(c2.dir);
    CHECK(lineage.found);
    CHECK(lineage.projection.snapshot.pending_intent.at("workItemId") == "wi-9");

    // clear 边不穿:clear 开的新场不带旧 goal(§4.67.2 clear 才撤 goal)。
    Volume cleared("lineage-clear", "s-cl");
    WriteSessionJson(cleared.dir, "s-cl", "clear", "s-a");
    lineage = goalns::ProjectGoalLineage(cleared.dir);
    CHECK_FALSE(lineage.found);
    CHECK(lineage.projection.gap == goalns::GoalProjectionGap::NoGoal);

    // 回环护栏:C←B 互相指,链停不死循环。
    Volume loop_c("lineage-loopc", "s-lc");
    Volume loop_b("lineage-loopb", "s-lb");
    WriteSessionJson(loop_c.dir, "s-lc", "resume", "s-lb");
    WriteSessionJson(loop_b.dir, "s-lb", "resume", "s-lc");
    lineage = goalns::ProjectGoalLineage(loop_c.dir);
    CHECK_FALSE(lineage.found);
    CHECK_FALSE(lineage.detail.empty());
}

TEST_CASE("恢复去重:claim 落账后,重复接管不再补队列") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("dedup", "s-dedup");
    {
        GoalService service(&*volume.writer, ServiceOptions(volume));
        GoalStateSnapshot draft = DraftObjective("恢复去重");
        draft.pending_intent = FirstIntent().ToJson();
        REQUIRE(service.CreateGoal(std::move(draft), {}).ok);
    }
    // 第一次"resume":干净写者续卷 + 接管 → wi-1 未认领,可按原 id 补。
    auto continued1 = V3Writer::Continue(volume.jsonl());
    REQUIRE(continued1.has_value());
    const auto ledger1 = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger1.has_value());
    auto projection1 = goalns::ProjectGoalState(*ledger1, volume.dir);
    REQUIRE(projection1.gap == goalns::GoalProjectionGap::None);
    GoalService service1(&*continued1, ServiceOptions(volume));
    REQUIRE(service1.AdoptFromProjection(projection1).ok);
    auto view1 = goalns::EvaluateGoalWork(*service1.current(), "run-new");
    CHECK(view1.claimable);
    CHECK(view1.intent.work_item_id == "wi-1");
    // 消费恰好一次:claim 落账 + 开轮收工(工作项销账)。
    REQUIRE(service1.ClaimPendingIntent("run-new", service1.current()->state_revision, {}).ok);
    REQUIRE(service1.BeginIteration(service1.current()->state_revision, {}).ok);
    CHECK(service1.EndIteration(service1.current()->state_revision, {}).ok);

    // 第二次"resume"(重复):同卷接管后,意图已销账 → 没得补。
    auto continued2 = V3Writer::Continue(volume.jsonl());
    REQUIRE(continued2.has_value());
    const auto ledger2 = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger2.has_value());
    auto projection2 = goalns::ProjectGoalState(*ledger2, volume.dir);
    REQUIRE(projection2.gap == goalns::GoalProjectionGap::None);
    GoalService service2(&*continued2, ServiceOptions(volume));
    REQUIRE(service2.AdoptFromProjection(projection2).ok);
    auto view2 = goalns::EvaluateGoalWork(*service2.current(), "run-newer");
    CHECK_FALSE(view2.claimable);
    CHECK_FALSE(view2.has_intent);

    // claim 落账但没收工(claim 与开轮之间崩):phase=queued = 确认未发送
    //(G2 定案:开轮先于模型发送)——新写者接管沿用原工作项,不另发新工作。
    Volume volume2("dedup-claim", "s-dedup2");
    {
        GoalService service(&*volume2.writer, ServiceOptions(volume2));
        GoalStateSnapshot draft = DraftObjective("认领后崩");
        draft.pending_intent = FirstIntent().ToJson();
        REQUIRE(service.CreateGoal(std::move(draft), {}).ok);
        REQUIRE(service.ClaimPendingIntent("run-crashed", service.current()->state_revision, {})
                    .ok);
    }
    auto continued3 = V3Writer::Continue(volume2.jsonl());
    REQUIRE(continued3.has_value());
    const auto ledger3 = lubancode::trajectory::v3::ReadV3Ledger(volume2.jsonl());
    REQUIRE(ledger3.has_value());
    auto projection3 = goalns::ProjectGoalState(*ledger3, volume2.dir);
    GoalService service3(&*continued3, ServiceOptions(volume2));
    REQUIRE(service3.AdoptFromProjection(projection3).ok);
    auto view3 = goalns::EvaluateGoalWork(*service3.current(), "run-restart");
    CHECK(view3.claimable);  // 未发送:接管续原请求(§4.67.4 恢复核验)
    CHECK(view3.intent.work_item_id == "wi-1");  // 同一 id,不另发新工作
    // 接管成:writerEpoch 换手落 applied,原写者迟到的开轮被 CAS 拒。
    REQUIRE(service3.ClaimPendingIntent("run-restart", service3.current()->state_revision, {}).ok);
    CHECK(service3.current()->state_revision == projection3.snapshot.state_revision + 1);
}

TEST_CASE("命令面状态投影:FormatGoalV3Status 不在 runtime 册钉(占位防漏)") {
    // 本册钉 runtime 面;命令排版(goal_commands.cpp 的 v3 路线)在
    // tests/unit/app/test_goal_v3_commands.cpp 钉。此处只确认 EvaluateGoalWork
    // 对空 epoch(显示路径不带写者身份)不误报他写者认领。
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalStateSnapshot snapshot;
    snapshot.goal_id = "goal-1";
    snapshot.session_id = "s";
    snapshot.run_id = "r";
    snapshot.objective = "o";
    snapshot.lifecycle = GoalLifecycle::Active;
    snapshot.phase = GoalPhase::Idle;
    GoalPendingIntent claimed = FirstIntent();
    claimed.claimed = true;
    claimed.writer_epoch = "run-x";
    claimed.claimed_at_ms = 1700;
    snapshot.pending_intent = claimed.ToJson();
    const auto view = goalns::EvaluateGoalWork(snapshot, /*writer_epoch=*/"");
    CHECK_FALSE(view.claimable);
    CHECK(view.claimed_by_other);  // 显示口径:已认领即"已认领",不猜是谁
}
