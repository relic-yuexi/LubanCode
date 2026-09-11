// GoalEvaluator 的 G2 加固(轨迹 v3 §4.67.5;单测钉):
//   - 严格判词校验:缺/重复 criterion、跨 goal 证据引用、合同外 criterion、
//     continue 无 next_action——全部拒;
//   - 完成门槛审计:required 缺证据/stale/truncated、required_artifacts
//     缺口、checkpoint remaining 非空、wait_task_refs 非空——不 eligible;
//   - 内部请求服务:真 V3Writer 进账(requested/system/user/prepared/
//     assistant/completed;purpose=goal_evaluation;parentTurnId 回指工作轮;
//     inputMessageRefs 指 user);repair 轮带原合同与原回复(不只错误);
//   - usage 逐次各记:两轮采样(初判坏 + repair 成)的 token 累加,
//     不只取末次;
//   - 组合取消:外部取消令牌在场时,内部超时也断(旧行为"外部在场选
//     外部、超时失效"已修);外部拉旗立刻断;
//   - 两坏落 rejected + evaluator_failed(不默认 achieved)。
// 旧口径回归(schema/prompt/无 tools)在 test_goal_evaluator.cpp 不动。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/assembler.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/goal_types.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalCheckpoint;
using goalns::GoalContract;
using goalns::GoalDecision;
using goalns::GoalEvaluation;
using goalns::GoalEvaluationInput;
using goalns::GoalEvaluationLedgerScope;
using goalns::GoalEvaluatorOptions;
using goalns::GoalEvidence;
using goalns::RunGoalEvaluation;
using goalns::ValidateEvaluationAgainstMaterial;
using goalns::AuditAchievedDecision;
using lubancode::trajectory::v3::EventKindV3;
using lubancode::trajectory::v3::MessagePurpose;
using lubancode::trajectory::v3::V3Writer;

namespace {

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

// 脚本 backend:按序吐预设回文与 usage;记收到的请求。
class ScriptBackend : public lubancode::api::Backend {
public:
    struct Reply {
        std::string text;
        std::int64_t input_tokens = 10;
        std::int64_t output_tokens = 5;
    };
    std::vector<Reply> replies;
    std::vector<lubancode::api::Request> seen;
    std::size_t call = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        seen.push_back(request);
        const Reply reply = call < replies.size() ? replies[call++] : Reply{"{}", 1, 1};
        on_event(lubancode::api::MessageStart{});
        on_event(lubancode::api::TextDelta{reply.text});
        // usage 各不同:两轮累加是否只取末次,看这笔账。
        lubancode::api::MessageDone done;
        done.usage = lubancode::api::Usage{reply.input_tokens, reply.output_tokens, 0, 0, 0};
        done.usage_reported = true;
        on_event(done);
        return {};
    }
};

// 挂起 backend:sleep 循环查取消旗,拉了就回 Cancelled(测组合取消)。
class HangingBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&,
        const std::atomic<bool>* cancel) override {
        for (int i = 0; i < 500; ++i) {  // 最多 5 秒
            if (cancel != nullptr && cancel->load()) {
                return std::unexpected(lubancode::api::Error{
                    lubancode::api::ErrorKind::Cancelled, "取消", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "超时", 0});
    }
};

GoalEvaluationInput SampleInput() {
    GoalEvaluationInput in;
    in.task.id = "goal-1";
    in.task.revision = 1;
    in.task.objective = "迁移认证层";
    in.task.contract.criteria.push_back({"c-1", "契约测试全绿", true});
    in.task.contract.required_artifacts = {"auth-report.txt"};
    in.checkpoint.summary = "契约测试跑通";
    in.checkpoint.remaining = {};
    GoalEvidence ev;
    ev.id = "ev-1";
    ev.goal_id = "goal-1";
    ev.iteration_id = "goal-1/iter-1";
    ev.producer = "run_command";
    ev.facts["command"] = "ctest -R auth";
    ev.facts["exit_code"] = 0;
    ev.facts["artifact"] = "auth-report.txt";
    ev.content_sha256 = std::string(64, 'b');
    ev.fresh = true;
    in.evidence = {ev};
    in.now_ms = 1000;
    return in;
}

const char* kGoodAchieved = R"({
  "decision": "achieved",
  "summary": "criterion 全 pass",
  "progress": true,
  "criteria": [{"id": "c-1", "status": "pass", "evidence_ids": ["ev-1"], "reason": "ctest 绿"}],
  "next_action": "无",
  "confidence": 0.9
})";

const char* kGoodContinue = R"({
  "decision": "continue",
  "summary": "还差一条",
  "progress": true,
  "criteria": [{"id": "c-1", "status": "fail", "evidence_ids": ["ev-1"], "reason": "还红"}],
  "next_action": "补契约测试",
  "confidence": 0.4
})";

GoalEvaluation ParseOrDie(const char* text) {
    GoalEvaluation evaluation;
    std::string error;
    if (!goalns::ParseGoalEvaluationReply(text, evaluation, &error)) {
        FAIL("判词解析失败: ", error);
    }
    return evaluation;
}

struct WriterHarness {
    std::filesystem::path dir;
    std::optional<V3Writer> writer;

    explicit WriterHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-goal-eval-v3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        auto started = V3Writer::Start(dir / "s1.jsonl", "20260911-120000-BBBBBB",
                                       "run-000001", "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }

    // ReadV3Ledger 自 P2(edceca68) 起返回 expected;auto 接,has_value/
    // operator-> 用法与 optional 同形。原 G2 册按 optional 写,一直被
    // 前序 TU 的编译错挡着没轮到编。
    auto Ledger() {
        return lubancode::trajectory::v3::ReadV3Ledger(dir / "s1.jsonl");
    }
};

GoalEvaluatorOptions LedgeredOptions(V3Writer& writer) {
    GoalEvaluatorOptions options;
    options.timeout_secs = 5;
    options.provider = "test-provider";
    options.wire = "anthropic";
    options.model = "eval-model";
    options.ledger.writer = &writer;
    options.ledger.goal_id = "goal-1";
    options.ledger.iteration_id = "goal-1/iter-1";
    options.ledger.evaluation_id = "eval-goal-1/iter-1";
    options.ledger.parent_turn_id = "turn-000001";
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// 严格判词校验
// ---------------------------------------------------------------------------

TEST_CASE("判词校验:恰好覆盖合同,缺/多/重全拒") {
    const GoalEvaluationInput in = SampleInput();

    SUBCASE("合同内的合法判词过") {
        const std::string error = ValidateEvaluationAgainstMaterial(in, ParseOrDie(kGoodContinue));
        CHECK(error.empty());
    }
    SUBCASE("缺 criterion:criteria 空拒") {
        GoalEvaluation e = ParseOrDie(kGoodContinue);
        e.criteria.clear();
        CHECK_FALSE(ValidateEvaluationAgainstMaterial(in, e).empty());
    }
    SUBCASE("重复 criterionId 拒") {
        GoalEvaluation e = ParseOrDie(kGoodContinue);
        e.criteria.push_back(e.criteria.front());
        const std::string error = ValidateEvaluationAgainstMaterial(in, e);
        REQUIRE_FALSE(error.empty());
        CHECK(error.find("重复") != std::string::npos);
    }
    SUBCASE("合同外 criterion(跨 goal)拒") {
        GoalEvaluation e = ParseOrDie(kGoodContinue);
        e.criteria[0].id = "c-99";
        const std::string error = ValidateEvaluationAgainstMaterial(in, e);
        REQUIRE_FALSE(error.empty());
        CHECK(error.find("不在冻结合同") != std::string::npos);
    }
    SUBCASE("跨 goal 证据引用拒") {
        GoalEvaluation e = ParseOrDie(kGoodContinue);
        e.criteria[0].evidence_ids = {"ev-OTHER-GOAL"};
        const std::string error = ValidateEvaluationAgainstMaterial(in, e);
        REQUIRE_FALSE(error.empty());
        CHECK(error.find("材料外") != std::string::npos);
    }
    SUBCASE("continue 无 next_action 拒") {
        GoalEvaluation e = ParseOrDie(kGoodContinue);
        e.next_action.clear();
        CHECK_FALSE(ValidateEvaluationAgainstMaterial(in, e).empty());
    }
}

// ---------------------------------------------------------------------------
// 完成门槛审计
// ---------------------------------------------------------------------------

TEST_CASE("审计门槛:achieved 缺口逐项报,不 eligible") {
    SUBCASE("材料全齐过") {
        GoalEvaluationInput in = SampleInput();
        in.task.contract.required_artifacts.clear();  // ev-1 facts 带 artifact 键
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK(audit.eligible);
        CHECK(audit.failures.empty());
    }
    SUBCASE("required_artifacts 缺证据") {
        GoalEvaluationInput in = SampleInput();
        in.evidence.front().facts.erase("artifact");
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK_FALSE(audit.eligible);
        REQUIRE(audit.failures.size() == 1);
        CHECK(audit.failures[0].find("auth-report.txt") != std::string::npos);
    }
    SUBCASE("stale 证据不算有效") {
        GoalEvaluationInput in = SampleInput();
        in.evidence.front().fresh = false;
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK_FALSE(audit.eligible);
    }
    SUBCASE("truncated 证据不算有效") {
        GoalEvaluationInput in = SampleInput();
        in.evidence.front().truncated = true;
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK_FALSE(audit.eligible);
    }
    SUBCASE("他 goal 的证据不算") {
        GoalEvaluationInput in = SampleInput();
        in.evidence.front().goal_id = "goal-OTHER";
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK_FALSE(audit.eligible);
    }
    SUBCASE("checkpoint remaining 非空:自称完成不顶用") {
        GoalEvaluationInput in = SampleInput();
        in.checkpoint.remaining.push_back("还有一条没验");
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK_FALSE(audit.eligible);
        REQUIRE(!audit.failures.empty());
        CHECK(audit.failures.back().find("未完成项") != std::string::npos);
    }
    SUBCASE("相关任务未收口不封账(G3 预留语义)") {
        GoalEvaluationInput in = SampleInput();
        in.wait_task_refs.push_back("task-9");
        const auto audit = AuditAchievedDecision(in, ParseOrDie(kGoodAchieved));
        CHECK_FALSE(audit.eligible);
    }
    SUBCASE("非 achieved 判词不适用(failures 空 = 不是缺口)") {
        const auto audit = AuditAchievedDecision(SampleInput(), ParseOrDie(kGoodContinue));
        CHECK_FALSE(audit.eligible);
        CHECK(audit.failures.empty());
    }
}

// ---------------------------------------------------------------------------
// 内部请求服务进账
// ---------------------------------------------------------------------------

TEST_CASE("进账:requested/system/user/prepared/assistant/completed 全落链") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("full-ledger");
    ScriptBackend backend;
    backend.replies = {{kGoodContinue}};
    const auto result = RunGoalEvaluation(backend, LedgeredOptions(*harness.writer),
                                          SampleInput());
    REQUIRE(result.has_value());
    CHECK(result->evaluation.decision == GoalDecision::Continue);
    REQUIRE(backend.seen.size() == 1);
    CHECK(backend.seen[0].tools.empty());  // 无工具铁律不破

    const auto report = lubancode::trajectory::v3::VerifyV3File(harness.dir / "s1.jsonl");
    REQUIRE(report.ok);
    const auto ledger = harness.Ledger();
    REQUIRE(ledger.has_value());

    bool saw_requested = false, saw_prepared = false, saw_completed = false;
    std::string requested_hash;
    for (const auto& event : ledger->events) {
        if (event.kind == EventKindV3::GoalEvaluationRequested) {
            saw_requested = true;
            requested_hash = event.payload.at("evidenceSetHash");
            CHECK(event.payload.at("evaluationId") == "eval-goal-1/iter-1");
            CHECK(event.payload.at("contractRevision") == 1);
        }
        if (event.kind == EventKindV3::ModelRequestPrepared) {
            saw_prepared = true;
            CHECK(event.payload.at("purpose") == "goal_evaluation");
            CHECK(event.payload.at("provider") == "test-provider");
        }
        if (event.kind == EventKindV3::GoalEvaluationCompleted) {
            saw_completed = true;
            CHECK(event.payload.at("decision") == "continue");
            CHECK(event.payload.at("evaluationMessageRef") == result->evaluation_message_id);
        }
    }
    CHECK(saw_requested);
    CHECK(saw_prepared);
    CHECK(saw_completed);
    CHECK_FALSE(requested_hash.empty());
    CHECK(result->evidence_set_hash == requested_hash);
    CHECK(result->evaluation_turn_id.rfind("goaleval-turn-", 0) == 0);
    CHECK(result->request_ids.size() == 1);
    CHECK(result->message_ids.size() == 3);  // system + user + assistant

    // 消息面:验收三枚消息 purpose=goal_evaluation;system turnId=null,
    // user/assistant 挂内部回合、parentTurnId 回指工作轮;不进 main 链
    //(链上没有它们 = 没被 AdmitMessages 接纳)。
    int purpose_messages = 0;
    for (const auto& message : ledger->messages) {
        if (message.purpose != MessagePurpose::GoalEvaluation) continue;
        ++purpose_messages;
        if (message.message.at("role") == "system") {
            CHECK_FALSE(message.turn_id.has_value());
        } else {
            REQUIRE(message.turn_id.has_value());
            CHECK(*message.turn_id == result->evaluation_turn_id);
            REQUIRE(message.parent_turn_id.has_value());
            CHECK(*message.parent_turn_id == "turn-000001");
        }
        if (message.message.at("role") == "assistant") {
            CHECK(message.provider.has_value());
            CHECK(message.wire.has_value());
            CHECK(message.model.has_value());
            REQUIRE(message.usage.has_value());
            CHECK(message.usage->at("inputTokens") == 10);
        }
    }
    CHECK(purpose_messages == 3);
    for (const auto& node : ledger->context.chain) {
        const std::string ref = node.message_ref;
        for (const auto& id : result->message_ids) {
            CHECK(ref != id);  // 验收消息不进 main contextChain
        }
    }
}

TEST_CASE("进账 + repair:两轮各记 usage,累计不只取末次;repair 带原回复") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("repair-ledger");
    ScriptBackend backend;
    backend.replies = {{"这不是 JSON", 7, 3}, {kGoodContinue, 11, 6}};
    const auto result = RunGoalEvaluation(backend, LedgeredOptions(*harness.writer),
                                          SampleInput());
    REQUIRE(result.has_value());
    CHECK(result->schema_repaired);
    REQUIRE(backend.seen.size() == 2);

    // usage 累计:7+11=18、3+6=9(旧口径只取末次的缺口已修)。
    CHECK(result->usage.input_tokens == 18);
    CHECK(result->usage.output_tokens == 9);
    CHECK(result->usage.request_count == 2);
    CHECK(result->request_ids.size() == 2);

    // repair 轮 user message:原合同、证据与原回复都在,不只一句错误。
    const auto& repair_message = backend.seen[1].messages.front();
    std::string repair_text;
    for (const auto& block : repair_message.content) {
        if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
            repair_text = text->text;
        }
    }
    CHECK(repair_text.find("迁移认证层") != std::string::npos);      // 原合同
    CHECK(repair_text.find("ev-1") != std::string::npos);            // 原证据
    CHECK(repair_text.find("这不是 JSON") != std::string::npos);     // 原回复
    CHECK(repair_text.find("上一次错误") != std::string::npos);      // 错误说明

    // 两轮请求各自成账:两条 prepared、两条 assistant(各带自己的 usage)。
    const auto ledger = harness.Ledger();
    REQUIRE(ledger.has_value());
    int assistant_count = 0;
    std::int64_t input_sum = 0;
    for (const auto& message : ledger->messages) {
        if (message.purpose != MessagePurpose::GoalEvaluation) continue;
        if (message.message.at("role") != "assistant") continue;
        ++assistant_count;
        REQUIRE(message.usage.has_value());
        input_sum += message.usage->at("inputTokens").get<std::int64_t>();
    }
    CHECK(assistant_count == 2);
    CHECK(input_sum == 18);
}

TEST_CASE("进账 + 两坏:rejected 落链,evaluator_failed 不默认 achieved") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("rejected-ledger");
    ScriptBackend backend;
    backend.replies = {{"坏的一次", 7, 3}, {"坏的两次", 11, 6}};
    goalns::GoalUsage failed_usage;
    const auto result = RunGoalEvaluation(backend, LedgeredOptions(*harness.writer),
                                          SampleInput(), /*cancel=*/nullptr, &failed_usage);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("evaluator_failed") != std::string::npos);
    // 失败路出参带回已花费用(两轮各记,§4.67.10):7+11=18、3+6=9。
    CHECK(failed_usage.input_tokens == 18);
    CHECK(failed_usage.output_tokens == 9);
    CHECK(failed_usage.request_count == 2);

    const auto ledger = harness.Ledger();
    REQUIRE(ledger.has_value());
    bool saw_rejected = false;
    for (const auto& event : ledger->events) {
        CHECK(event.kind != EventKindV3::GoalEvaluationCompleted);
        if (event.kind == EventKindV3::GoalEvaluationRejected) {
            saw_rejected = true;
            CHECK(event.payload.at("reason").get<std::string>().find("判词两坏") !=
                  std::string::npos);
        }
    }
    CHECK(saw_rejected);
}

TEST_CASE("进账 + 判词不合合同:视同解析失败,一次修复机会") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("material-reject");
    ScriptBackend backend;
    // 初判:缺 criterion(schema 过、材料校验拒);repair:补齐。
    const char* missing_criterion = R"({"decision":"continue","summary":"s","progress":true,
        "criteria":[],"next_action":"补"})";
    backend.replies = {{"```json\n" + std::string(missing_criterion) + "\n```", 5, 2},
                       {kGoodContinue, 5, 2}};
    const auto result = RunGoalEvaluation(backend, LedgeredOptions(*harness.writer),
                                          SampleInput());
    REQUIRE(result.has_value());
    CHECK(result->schema_repaired);  // 材料校验的修复也走 repair 轮
}

// ---------------------------------------------------------------------------
// 组合取消
// ---------------------------------------------------------------------------

TEST_CASE("组合取消:外部令牌在场时,内部超时也断") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    HangingBackend backend;
    std::atomic<bool> external{false};  // 在场但不拉——旧口径会让超时失效
    GoalEvaluatorOptions options;
    options.timeout_secs = 1;
    options.model = "m";
    const auto started = std::chrono::steady_clock::now();
    const auto result = RunGoalEvaluation(backend, options, SampleInput(), &external);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    REQUIRE(!result.has_value());
    CHECK(elapsed.count() < 4);  // 1 秒超时拉组合旗断掉(旧行为会挂满 5 秒)
}

TEST_CASE("组合取消:外部拉旗立刻断") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    HangingBackend backend;
    std::atomic<bool> external{true};  // 已拉
    GoalEvaluatorOptions options;
    options.timeout_secs = 30;  // 超时远在后面,断的必须是外部旗
    const auto started = std::chrono::steady_clock::now();
    const auto result = RunGoalEvaluation(backend, options, SampleInput(), &external);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    REQUIRE(!result.has_value());
    CHECK(elapsed.count() < 5);
}
