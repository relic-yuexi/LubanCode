// 异步工具单 P1 册:ToolJobCoordinator(持久 job 宿主侧任务服务)——
// start/get/wait/cancel 四接口、单写者完成信封(ownerEpoch 校验/唯一终态)、
// 权鉴 fail-closed 与派发前复查、资源键串行与并发上限、取消竞态、
// 32 KiB 预览与输出配额,以及单 §6 恢复表的账态注入(手造账到崩溃边界,
// 新协调器 AdoptRecovery,CI 可重复——不起真进程也不睡拍子竞态)。
//
// P1 是宿主侧服务,不是模型工具:不动 AgentLoop、不改工具注册表(单 §8
// 模型可见性归后续批次)。
//
// 夹具纪律:遍历账面先落局部 V3Ledger,不许 range-for 直接吃
// ReadV3Ledger(...).value().events / h.Read().events ——range 表达式里的
// expected(或按值返回的账)临时随完整表达式析构,auto&& 绑到的是其成员
// 左值,不延长生命周期,循环遍历已释放内存(UB)。libstdc++(gcc)腿上
// 确定性显形(glibc free 写 tcache 指针污染 EventLine 数组,垃圾 json 的
// type 字节读作 null,get<string> 抛 type_error.302);libc++/MSVC 侥幸不
// 显。CI 先例:run 34764809961 linux-manylinux 双红,g++-14 对该写法发
// [-Wdangling-pointer=]。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/tool_job_coordinator.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/schema3.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::tools;
namespace v3 = lubancode::trajectory::v3;

namespace {

// ctest 注册循环默认注入 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;本册写
// v3 账,显式开回 1(同 P0 两册口径)。
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

// 受控执行闸:executor 挂在 future 上,测试精确放行(无 sleep 竞态)。
// Open 幂等——测试显式放行后 Harness 析构的兜底放行不二次 set_value。
struct Gate {
    std::promise<void> release;
    std::future<void> released;
    std::atomic<bool> opened{false};
    Gate() : released(release.get_future()) {}
    void Open() {
        bool expected = false;
        if (opened.compare_exchange_strong(expected, true)) {
            release.set_value();
        }
    }
};

JobAuthDecision AllowAll(const std::string&, const nlohmann::json&) {
    return JobAuthDecision{true, false, ""};
}

// 六键 artifactRef(手造账用,P0 ledger 册同款形状)。
nlohmann::json MakeArtifactRef(const char* id) {
    return nlohmann::json::object({
        {"artifactId", id},
        {"kind", "result_metadata"},
        {"path", std::string("artifacts/") + id + ".json"},
        {"sha256", std::string(64, 'a')},
        {"bytes", 128},
        {"mediaType", "application/json"},
    });
}

// 按 seq 输出事件 kind 序列(账序断言用)。
std::vector<std::string> KindSequence(const v3::V3Ledger& ledger) {
    std::vector<std::string> kinds;
    for (const auto& event : ledger.events) {
        kinds.push_back(v3::EventKindV3Name(event.kind));
    }
    return kinds;
}

bool HasKind(const v3::V3Ledger& ledger, const char* kind_name) {
    for (const auto& event : ledger.events) {
        if (std::string(v3::EventKindV3Name(event.kind)) == kind_name) {
            return true;
        }
    }
    return false;
}

struct Harness {
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<v3::V3Writer> writer;
    std::unique_ptr<ToolJobCoordinator> coord;
    std::vector<std::shared_ptr<Gate>> gates;  // 收尾放行,不悬挂 worker

    explicit Harness(const char* tag, ToolJobCoordinator::Options options = {},
                     JobAuthorizationGate gate = AllowAll, JobExecutor executor = nullptr) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-job-coord-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "s1.jsonl";
        auto started = v3::V3Writer::Start(jsonl, "20260913-120000-JOBCO1", "run-000001",
                                           "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
        coord = std::make_unique<ToolJobCoordinator>(*writer, std::move(gate),
                                                     std::move(executor), options);
    }

    ~Harness() {
        for (auto& gate : gates) {
            gate->Open();  // 放行挂住的 executor,收尾干净
        }
        coord.reset();
        writer.reset();
    }

    // 恢复场景:丢掉旧协调器(等价进程重启,worker 已收场),续卷重建。
    void RebootCoordinator(JobAuthorizationGate gate = AllowAll, JobExecutor executor = nullptr,
                           ToolJobCoordinator::Options options = {}) {
        for (auto& gate_ptr : gates) {
            gate_ptr->Open();
        }
        coord.reset();
        auto continued = v3::V3Writer::Continue(jsonl);
        REQUIRE_MESSAGE(continued.has_value(), continued.error_or(""));
        writer = std::move(*continued);
        coord = std::make_unique<ToolJobCoordinator>(*writer, std::move(gate),
                                                     std::move(executor), options);
    }

    v3::V3Ledger Read() {
        auto ledger = v3::ReadV3Ledger(jsonl);
        REQUIRE_MESSAGE(ledger.has_value(), ledger.error_or(""));
        return *ledger;
    }

    // 声明一枚工具调用的 assistant 消息并接纳(调用证据的声明侧)。
    std::string AppendAssistantWithCall(const char* call_id) {
        v3::MessageDraft draft;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.request_id = "request-000001";
        draft.origin = v3::MessageOrigin::SessionRuntime;
        draft.provider = "openai";
        draft.wire = "responses";
        draft.model = "gpt-6";
        draft.response_model = nlohmann::json("gpt-6");
        draft.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
        draft.message = nlohmann::json::object({
            {"role", "assistant"},
            {"content", "先查"},
            {"tool_calls", nlohmann::json::array({nlohmann::json::object({
                {"id", call_id},
                {"type", "function"},
                {"function", nlohmann::json::object({
                    {"name", "search"},
                    {"arguments", "{\"query\":\"资料\"}"},
                })},
            })})},
        });
        auto receipt = writer->AppendMessage(draft, v3::Durability::PowerLoss);
        REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed,
                        receipt.error_message);
        auto admitted = writer->AdmitMessages({receipt.id});
        REQUIRE_MESSAGE(admitted.status == v3::WriteReceipt::Status::Committed,
                        admitted.error_message);
        return receipt.id;
    }

    JobStartRequest MakeRequest(const std::string& assistant_ref,
                                JobExecutionPolicy policy = {}) {
        JobStartRequest request;
        request.tool_name = "search";
        request.tool_input = nlohmann::json::object({{"query", "资料"}});
        request.turn_id = "turn-000001";
        request.step_id = "step-000001";
        request.assistant_message_ref = assistant_ref;
        request.policy = policy;
        return request;
    }

    std::shared_ptr<Gate> NewGate() {
        auto gate = std::make_shared<Gate>();
        gates.push_back(gate);
        return gate;
    }
};

// ---- 手造账注入(单 §6 恢复表;崩溃边界用账面钉死,CI 可重复) ----------

v3::WriteReceipt Emit(v3::V3Writer& writer, v3::EventKindV3 kind,
                      std::optional<v3::OpStatus> status, const char* action,
                      nlohmann::json payload, std::optional<std::string> request_id = std::nullopt) {
    v3::EventDraft draft;
    draft.kind = kind;
    draft.status = status;
    draft.turn_id = "turn-000001";
    draft.step_id = "step-000001";
    draft.action_id = action;
    draft.request_id = request_id;
    draft.payload = std::move(payload);
    auto receipt = writer.AppendEvent(std::move(draft), v3::Durability::ProcessCrash);
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    return receipt;
}

const char* kAction = "action-000001";

// pending(1) -> registered:调用证据 + 注册落稳(未派发)。
v3::WriteReceipt EmitPendingAndRegistered(
    v3::V3Writer& writer, const std::string& assistant_ref, const char* job_id,
    const nlohmann::json& policy_extra = nlohmann::json::object(), bool approval = false) {
    Emit(writer, v3::EventKindV3::ToolExecutionPending, v3::OpStatus::Pending, kAction,
         nlohmann::json{{"tool_call_id", kAction},
                        {"attempt", 1},
                        {"reason", "queued"},
                        {"assistantMessageRef", assistant_ref},
                        // 声明块配对键:缺了它 FoldToolActions 按 provider 号
                        // 过滤声明块时会把 tool_name 洗空(P0 fixture 同款)。
                        {"provider_tool_call_id", "call_A1"}});
    nlohmann::json payload{{"tool_call_id", kAction},
                           {"attempt", 1},
                           {"jobId", job_id},
                           {"mode", "job_handle"},
                           {"assistantMessageRef", assistant_ref},
                           {"executionPolicy", policy_extra}};
    if (approval) {
        payload["approvalRequired"] = true;
    }
    return Emit(writer, v3::EventKindV3::ToolJobRegistered, std::nullopt, kAction,
                std::move(payload));
}

// 接单链:started(1) -> finished(1) -> persisted(1) -> selected(1) -> tool 消息。
void EmitAdmissionChain(v3::V3Writer& writer, const char* job_id) {
    Emit(writer, v3::EventKindV3::ToolExecutionStarted, v3::OpStatus::Running, kAction,
         nlohmann::json{{"tool_call_id", kAction},
                        {"attempt", 1},
                        {"effectiveArgsRef", "args-000001"}});
    auto finished =
        Emit(writer, v3::EventKindV3::ToolExecutionFinished, v3::OpStatus::Done, kAction,
             nlohmann::json{{"tool_call_id", kAction}, {"attempt", 1}, {"exit_code", nullptr}});
    auto persisted = Emit(writer, v3::EventKindV3::ToolResultPersisted, std::nullopt, kAction,
                          nlohmann::json{{"tool_call_id", kAction},
                                         {"attempt", 1},
                                         {"result_ref", nlohmann::json::array({MakeArtifactRef("res-000001")})},
                                         {"executionEventRef", finished.id}});
    auto selected = Emit(writer, v3::EventKindV3::ToolResultSelected, std::nullopt, kAction,
                         nlohmann::json{{"tool_call_id", kAction},
                                        {"attempt", 1},
                                        {"sourceResultEventRefs", nlohmann::json::array({persisted.id})},
                                        {"hookEffectEventRefs", nlohmann::json::array()},
                                        {"effectiveOutcome", "done"}});
    v3::MessageDraft tool_message;
    tool_message.turn_id = "turn-000001";
    tool_message.step_id = "step-000001";
    tool_message.action_id = kAction;
    tool_message.origin = v3::MessageOrigin::SessionRuntime;
    tool_message.result_selection_ref = selected.id;
    tool_message.message = nlohmann::json::object({
        {"role", "tool"},
        {"tool_call_id", kAction},
        {"content", std::string("{\"jobId\":\"") + job_id + "\",\"status\":\"queued\"}"},
    });
    auto receipt = writer.AppendMessage(tool_message, v3::Durability::PowerLoss);
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    auto admitted = writer.AdmitMessages({receipt.id});
    REQUIRE_MESSAGE(admitted.status == v3::WriteReceipt::Status::Committed, admitted.error_message);
}

// 派发:pending(2) -> dispatched(epoch) -> started(2)。
void EmitDispatch(v3::V3Writer& writer, const char* job_id, const char* epoch) {
    Emit(writer, v3::EventKindV3::ToolExecutionPending, v3::OpStatus::Pending, kAction,
         nlohmann::json{{"tool_call_id", kAction}, {"attempt", 2}, {"reason", "job_dispatch"}});
    Emit(writer, v3::EventKindV3::ToolJobDispatched, std::nullopt, kAction,
         nlohmann::json{{"tool_call_id", kAction},
                        {"attempt", 2},
                        {"jobId", job_id},
                        {"ownerEpoch", epoch}});
    Emit(writer, v3::EventKindV3::ToolExecutionStarted, v3::OpStatus::Running, kAction,
         nlohmann::json{{"tool_call_id", kAction},
                        {"attempt", 2},
                        {"effectiveArgsRef", "args-000002"}});
}

// 业务终态:finished(2) -> persisted(2)(原文已落仓)。
std::string EmitBusinessTerminal(v3::V3Writer& writer, const char* job_id) {
    (void)job_id;  // 业务终态挂发起 action,jobId 不进载荷
    auto finished =
        Emit(writer, v3::EventKindV3::ToolExecutionFinished, v3::OpStatus::Done, kAction,
             nlohmann::json{{"tool_call_id", kAction}, {"attempt", 2}, {"exit_code", nullptr}});
    auto persisted = Emit(writer, v3::EventKindV3::ToolResultPersisted, std::nullopt, kAction,
                          nlohmann::json{{"tool_call_id", kAction},
                                         {"attempt", 2},
                                         {"result_ref", nlohmann::json::array({MakeArtifactRef("res-000002")})},
                                         {"executionEventRef", finished.id}});
    return persisted.id;
}

// 终态观测。
void EmitObserved(v3::V3Writer& writer, const char* job_id, const char* status,
                  const std::string& result_ref = "") {
    nlohmann::json payload{{"tool_call_id", kAction},
                           {"jobId", job_id},
                           {"observedStatus", status}};
    if (!result_ref.empty()) {
        payload["resultRef"] = result_ref;
        payload["resultVersion"] = 1;
    }
    Emit(writer, v3::EventKindV3::ToolJobObserved, std::nullopt, kAction, std::move(payload));
}

}  // namespace

// ---------------------------------------------------------------------------
// start 全链:调用证据 -> 注册落稳 -> 接单配齐 -> 派发 -> worker -> 单写者
// ---------------------------------------------------------------------------

TEST_CASE("start/get/wait 全链:账序、单写者信封、投零错") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::atomic<int> executor_calls{0};
    auto gate = std::make_shared<Gate>();
    Harness h("full",
              ToolJobCoordinator::Options{},
              AllowAll,
              [&executor_calls, gate](const JobExecutionContext& ctx) {
                  executor_calls.fetch_add(1);
                  REQUIRE(ctx.input.contains("query"));
                  gate->released.wait();
                  return Tool::Result::Text("资料结果:完整正文");
              });
    h.gates = {gate};
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
    REQUIRE(start.ok);
    CHECK(start.status == "running");  // 立即派发(配额空闲),worker 挂在闸上
    CHECK(start.job_id == "job-000001");
    gate->Open();

    JobWaitResult wait = h.coord->WaitJobs({start.job_id}, 5000, /*wait_all=*/true);
    REQUIRE(wait.satisfied);
    REQUIRE(wait.statuses.size() == 1);
    CHECK(wait.statuses[0].state == "succeeded");
    CHECK_FALSE(wait.statuses[0].result_ref.empty());
    CHECK(wait.statuses[0].result_version == 1);
    CHECK(wait.statuses[0].preview.find("资料结果") != std::string::npos);

    // get:终态带 resultRef 与有界预览;不自动重跑。
    JobStatusView view = h.coord->GetJob(start.job_id);
    CHECK(view.state == "succeeded");
    CHECK_FALSE(view.result_ref.empty());
    CHECK(view.result_version == 1);
    CHECK(view.preview.size() <= 32768);
    JobStatusView again = h.coord->GetJob(start.job_id);
    CHECK(again.result_ref == view.result_ref);
    CHECK(executor_calls.load() == 1);

    // 账序(单 §5 持久顺序):调用证据 -> 注册 -> 接单链 -> 派发 -> 业务。
    v3::V3Ledger ledger = h.Read();
    const auto kinds = KindSequence(ledger);
    auto at = [&](const char* kind) {
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i] == kind) {
                return i;
            }
        }
        return static_cast<std::size_t>(std::string::npos);
    };
    REQUIRE(at("tool.execution.pending") != std::string::npos);
    CHECK(at("tool.execution.pending") < at("tool.job.registered"));
    CHECK(at("tool.job.registered") < at("tool.execution.finished"));   // 接单执行收口
    CHECK(at("tool.execution.finished") < at("tool.result.selected"));
    CHECK(at("tool.result.selected") < at("tool.job.dispatched"));      // 接单配齐后才派发
    CHECK(at("tool.job.dispatched") < at("tool.job.observed"));
    // registered 落了 executionPolicy(执行策略持久档案)。
    bool policy_seen = false;
    for (const auto& event : ledger.events) {
        if (event.kind == v3::EventKindV3::ToolJobRegistered) {
            REQUIRE(event.payload.contains("executionPolicy"));
            REQUIRE(event.payload["executionPolicy"].is_object());
            CHECK(event.payload["executionPolicy"].contains("side_effect_class"));
            policy_seen = true;
        }
    }
    CHECK(policy_seen);
    // dispatched 带租约代号;业务 observed 带 resultRef/resultVersion。
    auto jobs = v3::FoldJobExecutions(ledger);
    const auto* job = v3::FindJobExecution(jobs, "job-000001");
    REQUIRE(job != nullptr);
    CHECK(job->state == "succeeded");
    CHECK(job->owner_epoch == "epoch-1");
    CHECK(job->observed_result_ref.has_value());
    // 协议欠账:start 的接单 tool 消息已配齐发起调用。
    auto obligations = v3::ProjectProtocolObligations(ledger);
    const auto* obligation = v3::FindProtocolObligation(obligations, "action-job-000001");
    REQUIRE(obligation != nullptr);
    CHECK(obligation->mode == "job_handle");
    CHECK(obligation->paired);
    CHECK(obligation->on_current_context);
    // 整账跨行合同零错。
    CHECK(v3::ValidateAsyncToolSequence(ledger).empty());
}

// ---------------------------------------------------------------------------
// 注册落稳前不派发(单 §5)
// ---------------------------------------------------------------------------

TEST_CASE("注册落账失败:不派发不接单,executor 零调用") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::atomic<int> executor_calls{0};
    // 用注入 IO 失败的 writer:registered(AppendEvent)写失败。
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-job-coord-regfail";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    auto jsonl = dir / "s1.jsonl";
    v3::V3WriterOptions options;
    // 只坏 registered 那一笔:提交序 system(1)/session.started(2)/
    // assistant(3)/pending(4)/registered(5)。注入后 writer 句柄 broken,
    // 正是"注册没落稳"的崩溃边界。
    std::atomic<int> commit_ordinal{0};
    options.inject_io_failure = [&commit_ordinal]() -> std::optional<std::string> {
        const int ordinal = commit_ordinal.fetch_add(1) + 1;
        if (ordinal == 5) {
            return std::string("injected_io_failure");
        }
        return std::nullopt;
    };
    auto started = v3::V3Writer::Start(jsonl, "20260913-130000-REGFAIL", "run-000001",
                                       "system prompt", nlohmann::json::object(), options);
    REQUIRE(started.has_value());
    auto writer = std::move(*started);
    ToolJobCoordinator coord(writer, AllowAll,
                             [&executor_calls](const JobExecutionContext&) {
                                 executor_calls.fetch_add(1);
                                 return Tool::Result::Text("never");
                             });
    v3::MessageDraft draft;
    draft.turn_id = "turn-000001";
    draft.step_id = "step-000001";
    draft.request_id = "request-000001";
    draft.origin = v3::MessageOrigin::SessionRuntime;
    draft.provider = "openai";
    draft.wire = "responses";
    draft.model = "gpt-6";
    draft.response_model = nlohmann::json("gpt-6");
    draft.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
    draft.message = nlohmann::json::object({{"role", "assistant"}, {"content", "查"}});
    auto assistant = writer.AppendMessage(draft, v3::Durability::PowerLoss);
    REQUIRE_MESSAGE(assistant.status == v3::WriteReceipt::Status::Committed,
                    assistant.error_message);

    JobStartRequest request;
    request.tool_name = "search";
    request.tool_input = nlohmann::json::object({{"query", "资料"}});
    request.turn_id = "turn-000001";
    request.step_id = "step-000001";
    request.assistant_message_ref = assistant.id;
    JobStartResult result = coord.StartJob(request);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "job.start.register_write_failed");
    CHECK(executor_calls.load() == 0);  // 注册没落稳,一个字都没执行
    // 账上无 registered、无 dispatched。
    auto ledger = v3::ReadV3Ledger(jsonl);
    REQUIRE(ledger.has_value());
    CHECK_FALSE(HasKind(*ledger, "tool.job.registered"));
    CHECK_FALSE(HasKind(*ledger, "tool.job.dispatched"));
}

// ---------------------------------------------------------------------------
// 权鉴(单 §8:jobId 不是访问凭证;fail-closed)
// ---------------------------------------------------------------------------

TEST_CASE("权鉴 fail-closed:无闸门全拒;闸门拒落 rejected;审批挂起不派发") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SUBCASE("没挂闸门:一律拒,不注册") {
        Harness h("gate-missing", ToolJobCoordinator::Options{}, /*gate=*/nullptr,
                  [](const JobExecutionContext&) { return Tool::Result::Text("x"); });
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        JobStartResult result = h.coord->StartJob(h.MakeRequest(assistant));
        CHECK_FALSE(result.ok);
        CHECK(result.error_code == "job.start.denied");
        v3::V3Ledger ledger = h.Read();
        CHECK(HasKind(ledger, "tool.execution.rejected"));
        CHECK_FALSE(HasKind(ledger, "tool.job.registered"));
    }
    SUBCASE("闸门拒:reason 落账") {
        Harness h("gate-denied", ToolJobCoordinator::Options{},
                  [](const std::string&, const nlohmann::json&) {
                      return JobAuthDecision{false, false, "scope_violation"};
                  },
                  [](const JobExecutionContext&) { return Tool::Result::Text("x"); });
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        JobStartResult result = h.coord->StartJob(h.MakeRequest(assistant));
        CHECK(result.error_code == "job.start.denied");
        v3::V3Ledger ledger = h.Read();
        bool reason_seen = false;
        for (const auto& event : ledger.events) {
            if (event.kind == v3::EventKindV3::ToolExecutionRejected &&
                event.payload.contains("reason") && event.payload["reason"].is_string() &&
                event.payload["reason"].get<std::string>() == "scope_violation") {
                reason_seen = true;
            }
        }
        CHECK(reason_seen);
    }
    SUBCASE("审批未过:awaiting_approval 不派发;放行后派发") {
        std::atomic<int> executor_calls{0};
        std::atomic<int> gate_calls{0};
        Harness h("gate-approval", ToolJobCoordinator::Options{},
                  [&gate_calls](const std::string&, const nlohmann::json&) {
                      // 第 1 次(Start 权鉴)要审批;审批放行后的派发复查
                      //(GrantApproval 路径)恢复放行。
                      const int call = gate_calls.fetch_add(1);
                      return call == 0 ? JobAuthDecision{false, true, ""}
                                       : JobAuthDecision{true, false, ""};
                  },
                  [&executor_calls](const JobExecutionContext&) {
                      executor_calls.fetch_add(1);
                      return Tool::Result::Text("ok");
                  });
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
        REQUIRE(start.ok);
        CHECK(start.status == "awaiting_approval");
        CHECK(executor_calls.load() == 0);  // 审批未过不派发(单 §6)
        v3::V3Ledger ledger = h.Read();
        bool approval_flag = false;
        for (const auto& event : ledger.events) {
            if (event.kind == v3::EventKindV3::ToolJobRegistered &&
                event.payload.contains("approvalRequired") &&
                event.payload["approvalRequired"].get<bool>()) {
                approval_flag = true;
            }
        }
        CHECK(approval_flag);
        // 放行:入队派发,跑到终态。
        JobStartResult granted = h.coord->GrantApproval(start.job_id);
        REQUIRE(granted.ok);
        JobWaitResult wait = h.coord->WaitJobs({start.job_id}, 5000, true);
        REQUIRE(wait.satisfied);
        CHECK(wait.statuses[0].state == "succeeded");
        CHECK(executor_calls.load() == 1);
        CHECK(v3::ValidateAsyncToolSequence(h.Read()).empty());
    }
    SUBCASE("四接口都过闸门:拒读的视角拿不到结果") {
        Harness h("gate-read", ToolJobCoordinator::Options{}, AllowAll,
                  [](const JobExecutionContext&) { return Tool::Result::Text("secret"); });
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
        REQUIRE(start.ok);
        REQUIRE(h.coord->WaitJobs({start.job_id}, 5000, true).satisfied);
        JobStatusView ok_view = h.coord->GetJob(start.job_id);
        CHECK(ok_view.state == "succeeded");
        CHECK_FALSE(ok_view.access_denied);
        CHECK(ok_view.preview.find("secret") != std::string::npos);
        // 换拒读闸门的协调器接管(等价新宿主视角):GetJob 被拒,不泄结果。
        h.RebootCoordinator(
            [](const std::string&, const nlohmann::json&) {
                return JobAuthDecision{false, false, "read_not_allowed"};
            },
            [](const JobExecutionContext&) { return Tool::Result::Text("x"); });
        JobRecoveryPlan plan =
            ToolJobCoordinator::PlanRecovery(v3::ReadV3Ledger(h.jsonl).value());
        REQUIRE(plan.items.size() == 1);
        CHECK(plan.items[0].disposition == "already_terminal");
        h.coord->AdoptRecovery(plan);
        JobStatusView denied = h.coord->GetJob(start.job_id);
        CHECK(denied.access_denied);
        CHECK(denied.access_reason == "read_not_allowed");
        CHECK(denied.state.empty());
        CHECK(denied.preview.empty());
    }
}

// ---------------------------------------------------------------------------
// 派发前复查授权有效期(单 §7)
// ---------------------------------------------------------------------------

TEST_CASE("派发前复查:入队时获准不永久放行,复查拒按 failed 收口") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::atomic<int> gate_calls{0};
    std::atomic<int> executor_calls{0};
    Harness h("recheck", ToolJobCoordinator::Options{},
              [&gate_calls](const std::string&, const nlohmann::json&) {
                  const int call = gate_calls.fetch_add(1);
                  // 第 1 次(Start 权鉴)过;第 2 次(派发复查)拒——授权
                  // 过期;第 3 次起(Get 读取)恢复放行,便于断言落账。
                  return call == 1 ? JobAuthDecision{false, false, "authorization_expired"}
                                   : JobAuthDecision{true, false, ""};
              },
              [&executor_calls](const JobExecutionContext&) {
                  executor_calls.fetch_add(1);
                  return Tool::Result::Text("should not run");
              });
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
    REQUIRE(start.ok);
    JobStatusView view = h.coord->GetJob(start.job_id);
    CHECK(view.state == "failed");  // 复查拒:job 观测 failed(执行投影无 rejected)
    CHECK(view.failure == "authorization_expired");
    CHECK(executor_calls.load() == 0);  // 复查拒在派发前:没有执行
    v3::V3Ledger ledger = h.Read();
    CHECK(HasKind(ledger, "tool.execution.rejected"));  // attempt 2 无执行
}

// ---------------------------------------------------------------------------
// 资源键串行与并发上限(单 §7)
// ---------------------------------------------------------------------------

TEST_CASE("资源键串行:同 resource_keys 同时只跑一个") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    auto gate1 = std::make_shared<Gate>();
    Harness h("resource", ToolJobCoordinator::Options{}, AllowAll,
              [gate1](const JobExecutionContext& ctx) {
                  // 只挂第一只(占住资源);第二只获得资源后立即跑完。
                  if (ctx.input.contains("n") && ctx.input["n"] == 2) {
                      return Tool::Result::Text("second");
                  }
                  gate1->released.wait();
                  return Tool::Result::Text("first");
              });
    h.gates = {gate1};
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobExecutionPolicy policy;
    policy.side_effect_class = "local_write";
    policy.resource_keys = {"repo-root"};
    JobStartRequest first = h.MakeRequest(assistant, policy);
    first.tool_input = nlohmann::json::object({{"n", 1}});
    JobStartRequest second = h.MakeRequest(assistant, policy);
    second.tool_input = nlohmann::json::object({{"n", 2}});
    JobStartResult j1 = h.coord->StartJob(first);
    JobStartResult j2 = h.coord->StartJob(second);
    REQUIRE(j1.ok);
    REQUIRE(j2.ok);
    CHECK(h.coord->GetJob(j1.job_id).state == "running");
    // 同键互斥:第二只留在队里,不派发。
    CHECK(h.coord->GetJob(j2.job_id).state == "queued");
    CHECK(h.coord->queued_count() == 1);
    // 放行第一只:终态后资源让位,第二只自动派发。
    gate1->Open();
    JobWaitResult wait1 = h.coord->WaitJobs({j1.job_id}, 5000, true);
    REQUIRE(wait1.satisfied);
    CHECK(wait1.statuses[0].state == "succeeded");
    JobWaitResult wait2 = h.coord->WaitJobs({j2.job_id}, 5000, true);
    REQUIRE(wait2.satisfied);
    CHECK(wait2.statuses[0].state == "succeeded");
    CHECK(v3::ValidateAsyncToolSequence(h.Read()).empty());
}

TEST_CASE("并发上限:session_running=1 挡住第二只(read_only 不占资源键)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    auto gate1 = std::make_shared<Gate>();
    Harness h("limit", [&] {
                  ToolJobCoordinator::Options options;
                  options.limits.session_running = 1;
                  return options;
              }(),
              AllowAll,
              [gate1](const JobExecutionContext& ctx) {
                  // 只挂第一只;第二只获配额后立即跑完。
                  if (ctx.input.contains("n") && ctx.input["n"] == 2) {
                      return Tool::Result::Text("second");
                  }
                  gate1->released.wait();
                  return Tool::Result::Text("first");
              });
    h.gates = {gate1};
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobStartRequest first = h.MakeRequest(assistant);  // 默认 read_only
    first.tool_input = nlohmann::json::object({{"n", 1}});
    JobStartRequest second = h.MakeRequest(assistant);
    second.tool_input = nlohmann::json::object({{"n", 2}});
    JobStartResult j1 = h.coord->StartJob(first);
    JobStartResult j2 = h.coord->StartJob(second);
    REQUIRE(j1.ok);
    REQUIRE(j2.ok);
    CHECK(h.coord->GetJob(j1.job_id).state == "running");
    CHECK(h.coord->GetJob(j2.job_id).state == "queued");  // 配额挡住
    gate1->Open();
    REQUIRE(h.coord->WaitJobs({j1.job_id}, 5000, true).satisfied);
    REQUIRE(h.coord->WaitJobs({j2.job_id}, 5000, true).satisfied);
}

// ---------------------------------------------------------------------------
// 取消(单 §8:请求不是终态;不保证终止)
// ---------------------------------------------------------------------------

TEST_CASE("取消三径:未派发收口/worker 响应/跑完不改写") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SUBCASE("未派发取消:唯一终态 cancelled,无执行") {
        auto gate1 = std::make_shared<Gate>();
        std::atomic<int> executor_calls{0};
        Harness h("cancel-queued", ToolJobCoordinator::Options{}, AllowAll,
                  [gate1, &executor_calls](const JobExecutionContext& ctx) {
                      executor_calls.fetch_add(1);
                      gate1->released.wait();
                      (void)ctx;
                      return Tool::Result::Text("first");
                  });
        h.gates = {gate1};
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        JobExecutionPolicy policy;
        policy.side_effect_class = "git";
        policy.resource_keys = {"repo"};
        JobStartRequest first = h.MakeRequest(assistant, policy);
        first.tool_input = nlohmann::json::object({{"n", 1}});
        JobStartRequest second = h.MakeRequest(assistant, policy);
        second.tool_input = nlohmann::json::object({{"n", 2}});
        JobStartResult j1 = h.coord->StartJob(first);
        JobStartResult j2 = h.coord->StartJob(second);
        REQUIRE(j1.ok);
        REQUIRE(j2.ok);
        REQUIRE(h.coord->GetJob(j2.job_id).state == "queued");
        // 取消还在队里的第二只:请求即收口。
        JobCancelResult cancel = h.coord->CancelJob(j2.job_id, "user_escape");
        REQUIRE(cancel.ok);
        CHECK(cancel.status == "cancel_requested");
        CHECK(cancel.terminal == "cancelled");
        CHECK(h.coord->GetJob(j2.job_id).state == "cancelled");
        // 账:cancel_requested + observed(cancelled);attempt 2 没开。
        v3::V3Ledger ledger = h.Read();
        CHECK(HasKind(ledger, "tool.job.cancel_requested"));
        int attempt2_pending = 0;
        for (const auto& event : ledger.events) {
            if (event.kind == v3::EventKindV3::ToolExecutionPending &&
                event.payload.contains("attempt") && event.payload["attempt"].is_number_unsigned() &&
                event.payload["attempt"].get<std::uint64_t>() == 2 &&
                event.action_id.has_value() && *event.action_id == "action-job-000002") {
                attempt2_pending += 1;
            }
        }
        CHECK(attempt2_pending == 0);
        gate1->Open();
        REQUIRE(h.coord->WaitJobs({j1.job_id}, 5000, true).satisfied);
        CHECK(v3::ValidateAsyncToolSequence(h.Read()).empty());
    }
    SUBCASE("在跑取消:worker 响应旗子,终态 cancelled") {
        Harness h("cancel-running", ToolJobCoordinator::Options{}, AllowAll,
                  [](const JobExecutionContext& ctx) {
                      while (ctx.cancel != nullptr && !ctx.cancel->load()) {
                          std::this_thread::yield();
                      }
                      return Tool::Result::Error("cancelled_by_flag");
                  });
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
        REQUIRE(start.ok);
        JobCancelResult cancel = h.coord->CancelJob(start.job_id, "user_escape");
        REQUIRE(cancel.ok);
        CHECK(cancel.status == "cancel_requested");  // 请求状态,不保证终止
        JobWaitResult wait = h.coord->WaitJobs({start.job_id}, 5000, true);
        REQUIRE(wait.satisfied);
        CHECK(wait.statuses[0].state == "cancelled");
        CHECK(wait.statuses[0].cancel_requested);
        auto jobs = v3::FoldJobExecutions(h.Read());
        const auto* job = v3::FindJobExecution(jobs, start.job_id);
        REQUIRE(job != nullptr);
        CHECK(job->state == "cancelled");
        CHECK(job->cancel_requested);
    }
    SUBCASE("竞态:取消请求后 worker 跑完,真实完成不改写") {
        // 受控闸(P2 排障补):worker 挂 future,取消意图先落账、再放行完
        // 成——"取消在先、完成在后"的次序由测试钉死。即回型 worker 在慢
        // 机器上可能抢在 StartJob 尾泵前完事,CancelJob 变 already_terminal,
        // cancel_requested 永不落账(潜伏竞态,CI 高负载显形)。
        auto gate = std::make_shared<Gate>();
        Harness h2("cancel-race", ToolJobCoordinator::Options{}, AllowAll,
                   [gate](const JobExecutionContext&) {
                       gate->released.get();
                       // 不看取消旗,把活干完(竞态:取消与完成同时到)。
                       return Tool::Result::Text("done anyway");
                   });
        h2.gates.push_back(gate);  // 收尾兜底放行(Harness dtor)
        std::string assistant = h2.AppendAssistantWithCall("call_A1");
        JobStartResult start = h2.coord->StartJob(h2.MakeRequest(assistant));
        REQUIRE(start.ok);
        JobCancelResult cancel = h2.coord->CancelJob(start.job_id, "user_escape");
        REQUIRE(cancel.ok);
        CHECK(cancel.status == "cancel_requested");  // 取消先落账
        gate->Open();
        JobWaitResult wait = h2.coord->WaitJobs({start.job_id}, 5000, true);
        REQUIRE(wait.satisfied);
        CHECK(wait.statuses[0].state == "succeeded");  // 唯一终态按证据收
        auto jobs = v3::FoldJobExecutions(h2.Read());
        const auto* job = v3::FindJobExecution(jobs, start.job_id);
        REQUIRE(job != nullptr);
        CHECK(job->state == "succeeded");
        CHECK(job->cancel_requested);  // 取消意图保留在账(单 §6)
        CHECK(v3::ValidateAsyncToolSequence(h2.Read()).empty());
    }
}

// ---------------------------------------------------------------------------
// wait 超时(单 §8:超时回 pending+游标,不宣告失败)
// ---------------------------------------------------------------------------

TEST_CASE("wait 超时:回 pending 与快照,放行后拿到终态") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    auto gate = std::make_shared<Gate>();
    Harness h("wait-timeout", ToolJobCoordinator::Options{}, AllowAll,
              [gate](const JobExecutionContext&) {
                  gate->released.wait();
                  return Tool::Result::Text("late result");
              });
    h.gates = {gate};
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
    REQUIRE(start.ok);
    JobWaitResult timeout = h.coord->WaitJobs({start.job_id}, 50, true);
    CHECK_FALSE(timeout.satisfied);
    CHECK(timeout.timed_out);  // 不宣告失败
    REQUIRE(timeout.statuses.size() == 1);
    CHECK(timeout.statuses[0].state == "running");
    CHECK(timeout.statuses[0].preview.empty());
    gate->Open();
    JobWaitResult done = h.coord->WaitJobs({start.job_id}, 5000, true);
    REQUIRE(done.satisfied);
    CHECK(done.statuses[0].state == "succeeded");
    CHECK(done.statuses[0].preview.find("late result") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 单写者信封:旧租约拒收、终态后迟到拒收(单 §5 定案 1/§6 唯一终态)
// ---------------------------------------------------------------------------

TEST_CASE("完成信封:旧租约拒收;终态后第二枚拒收") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    auto gate = std::make_shared<Gate>();
    Harness h("envelope", ToolJobCoordinator::Options{}, AllowAll,
              [gate](const JobExecutionContext&) {
                  gate->released.wait();
                  return Tool::Result::Text("real worker");
              });
    h.gates = {gate};
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant));
    REQUIRE(start.ok);
    CHECK(h.coord->GetJob(start.job_id).state == "running");
    // 旧租约信封(epoch-9):拒收,不落终态不落观测。
    CHECK_FALSE(h.coord->DebugSubmitEnvelope(start.job_id, "epoch-9",
                                             Tool::Result::Text("stale worker")));
    CHECK(h.coord->stale_envelopes_rejected() == 1);
    CHECK(h.coord->GetJob(start.job_id).state == "running");
    // 真 worker 完成:收口 succeeded。
    gate->Open();
    REQUIRE(h.coord->WaitJobs({start.job_id}, 5000, true).satisfied);
    CHECK(h.coord->GetJob(start.job_id).state == "succeeded");
    // 终态后迟到的第二枚(同租约):拒收,唯一终态。
    CHECK_FALSE(h.coord->DebugSubmitEnvelope(start.job_id, "epoch-1",
                                             Tool::Result::Text("late duplicate")));
    CHECK(h.coord->duplicate_terminal_envelopes_rejected() == 1);
    CHECK(h.coord->GetJob(start.job_id).state == "succeeded");
    // 账上只有一枚终态观测。(账先落局部再遍历:range-for 的 range
    // 表达式里函数按值返回的临时随完整表达式析构,直接绑它的成员是
    // 悬垂遍历 UB——gcc/libstdc++ 腿确定性踩雷,见册头夹具纪律。)
    int terminal_observations = 0;
    v3::V3Ledger envelope_ledger = h.Read();
    for (const auto& event : envelope_ledger.events) {
        if (event.kind == v3::EventKindV3::ToolJobObserved &&
            event.payload.contains("observedStatus") &&
            event.payload["observedStatus"].get<std::string>() == "succeeded") {
            terminal_observations += 1;
        }
    }
    CHECK(terminal_observations == 1);
}

// ---------------------------------------------------------------------------
// 输出配额与 32 KiB 预览(单 §7/§4.18)
// ---------------------------------------------------------------------------

TEST_CASE("输出配额:max_output_bytes 截断,预览如实报 quota") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Harness h("quota", ToolJobCoordinator::Options{}, AllowAll,
              [](const JobExecutionContext&) {
                  return Tool::Result::Text(std::string(500, 'x'));  // 500 字节正文
              });
    std::string assistant = h.AppendAssistantWithCall("call_A1");
    JobExecutionPolicy policy;
    policy.max_output_bytes = 100;  // 配额 100:截断
    JobStartResult start = h.coord->StartJob(h.MakeRequest(assistant, policy));
    REQUIRE(start.ok);
    JobWaitResult wait = h.coord->WaitJobs({start.job_id}, 5000, true);
    REQUIRE(wait.satisfied);
    CHECK(wait.statuses[0].state == "succeeded");
    const std::string& preview = wait.statuses[0].preview;
    CHECK(preview.find("capture_complete: false") != std::string::npos);
    CHECK(preview.find("quota") != std::string::npos);
    CHECK(preview.size() <= 32768);
    // persisted 描述落仓:capture_complete=false/quota 如实记。
    bool quota_recorded = false;
    for (const auto& entry : std::filesystem::directory_iterator(h.dir / "artifacts")) {
        if (entry.path().extension().string() != ".json") {
            continue;
        }
        std::ifstream file(entry.path());
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.find("\"capture_reason\": \"quota\"") != std::string::npos) {
            quota_recorded = true;
        }
    }
    CHECK(quota_recorded);
}

// ---------------------------------------------------------------------------
// 恢复:账态注入(单 §6 表;崩溃边界用账面钉死,CI 可重复)
// ---------------------------------------------------------------------------

TEST_CASE("恢复:registered 未派发 -> requeue,epoch 从账上接续") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::filesystem::path jsonl;
    {
        Harness h("recover-requeue");
        jsonl = h.jsonl;
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        // 账造到:pending -> registered -> 接单链(未派发)。崩溃在
        //"注册落稳、尚未派发"边界。
        EmitPendingAndRegistered(*h.writer, assistant, "job-000001");
        EmitAdmissionChain(*h.writer, "job-000001");
    }
    std::atomic<int> executor_calls{0};
    auto writer = v3::V3Writer::Continue(jsonl);
    REQUIRE(writer.has_value());
    ToolJobCoordinator coord(*writer, AllowAll,
                             [&executor_calls](const JobExecutionContext&) {
                                 executor_calls.fetch_add(1);
                                 return Tool::Result::Text("requeued result");
                             });
    v3::V3Ledger ledger = v3::ReadV3Ledger(jsonl).value();
    JobRecoveryPlan plan = ToolJobCoordinator::PlanRecovery(ledger);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].disposition == "requeue");
    CHECK(plan.items[0].tool_name == "search");  // 执行材料从声明块恢复
    REQUIRE(coord.AdoptRecovery(plan) == 1);
    JobWaitResult wait = coord.WaitJobs({"job-000001"}, 5000, true);
    REQUIRE(wait.satisfied);
    CHECK(wait.statuses[0].state == "succeeded");
    CHECK(executor_calls.load() == 1);
    // 派发是新租约:账上首枚 dispatched,epoch-1(此前无派发)。
    auto jobs = v3::FoldJobExecutions(v3::ReadV3Ledger(jsonl).value());
    const auto* job = v3::FindJobExecution(jobs, "job-000001");
    REQUIRE(job != nullptr);
    CHECK(job->owner_epoch == "epoch-1");
    CHECK(v3::ValidateAsyncToolSequence(v3::ReadV3Ledger(jsonl).value()).empty());
}

TEST_CASE("恢复:dispatched 无终态 -> unknown_hold,不盲跑") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::filesystem::path jsonl;
    {
        Harness h("recover-unknown");
        jsonl = h.jsonl;
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        // 账造到:派发与 started(2) 已落,无终态(崩溃在执行中)。
        EmitPendingAndRegistered(*h.writer, assistant, "job-000001");
        EmitAdmissionChain(*h.writer, "job-000001");
        EmitDispatch(*h.writer, "job-000001", "epoch-1");
    }
    std::atomic<int> executor_calls{0};
    auto writer = v3::V3Writer::Continue(jsonl);
    REQUIRE(writer.has_value());
    ToolJobCoordinator coord(*writer, AllowAll,
                             [&executor_calls](const JobExecutionContext&) {
                                 executor_calls.fetch_add(1);
                                 return Tool::Result::Text("never");
                             });
    v3::V3Ledger ledger = v3::ReadV3Ledger(jsonl).value();
    JobRecoveryPlan plan = ToolJobCoordinator::PlanRecovery(ledger);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].disposition == "unknown_hold");
    REQUIRE(coord.AdoptRecovery(plan) == 1);
    CHECK(coord.GetJob("job-000001").state == "unknown");  // 不合成假终态
    CHECK(executor_calls.load() == 0);                     // 不盲跑(单 §6)
    // observed(unknown) 在账。(账先落局部再遍历:range 表达式里的
    // expected 临时随完整表达式析构,直接绑 .value().events 是悬垂遍历
    // UB——gcc/libstdc++ 腿确定性踩雷,见册头夹具纪律。)
    bool unknown_observed = false;
    v3::V3Ledger after_adopt = v3::ReadV3Ledger(jsonl).value();
    for (const auto& event : after_adopt.events) {
        if (event.kind == v3::EventKindV3::ToolJobObserved &&
            event.payload.contains("observedStatus") &&
            event.payload["observedStatus"].get<std::string>() == "unknown") {
            unknown_observed = true;
        }
    }
    CHECK(unknown_observed);
    auto jobs = v3::FoldJobExecutions(v3::ReadV3Ledger(jsonl).value());
    const auto* job = v3::FindJobExecution(jobs, "job-000001");
    REQUIRE(job != nullptr);
    CHECK(job->state == "unknown");
}

TEST_CASE("恢复:终态与原文在账、观测缺 -> 补投递不重跑") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::filesystem::path jsonl;
    std::string business_persisted_id;
    {
        Harness h("recover-complete");
        jsonl = h.jsonl;
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        // 账造到:业务 finished(2)+persisted(2) 已落,observed 终态没落
        //(崩溃在"原文落仓、终态观测未写"边界)。
        EmitPendingAndRegistered(*h.writer, assistant, "job-000001");
        EmitAdmissionChain(*h.writer, "job-000001");
        EmitDispatch(*h.writer, "job-000001", "epoch-1");
        business_persisted_id = EmitBusinessTerminal(*h.writer, "job-000001");
    }
    std::atomic<int> executor_calls{0};
    auto writer = v3::V3Writer::Continue(jsonl);
    REQUIRE(writer.has_value());
    ToolJobCoordinator coord(*writer, AllowAll,
                             [&executor_calls](const JobExecutionContext&) {
                                 executor_calls.fetch_add(1);
                                 return Tool::Result::Text("never");
                             });
    v3::V3Ledger ledger = v3::ReadV3Ledger(jsonl).value();
    JobRecoveryPlan plan = ToolJobCoordinator::PlanRecovery(ledger);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].disposition == "complete_delivery");
    CHECK(plan.items[0].business_result_ref == business_persisted_id);
    REQUIRE(coord.AdoptRecovery(plan) == 1);
    // 补的是观测,不是执行。
    CHECK(executor_calls.load() == 0);
    JobStatusView view = coord.GetJob("job-000001");
    CHECK(view.state == "succeeded");
    CHECK(view.result_ref == business_persisted_id);  // resultRef 指账上原文
    CHECK(view.result_version == 1);
    auto jobs = v3::FoldJobExecutions(v3::ReadV3Ledger(jsonl).value());
    const auto* job = v3::FindJobExecution(jobs, "job-000001");
    REQUIRE(job != nullptr);
    CHECK(job->state == "succeeded");
    CHECK(job->observed_result_ref.has_value());
    CHECK(*job->observed_result_ref == business_persisted_id);
    CHECK(v3::ValidateAsyncToolSequence(v3::ReadV3Ledger(jsonl).value()).empty());
}

TEST_CASE("恢复:接单 tool 消息缺 -> 补链入队,不重跑") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::filesystem::path jsonl;
    {
        Harness h("recover-admission");
        jsonl = h.jsonl;
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        // 账造到:registered 落稳、接单链断在 selected 之后(消息没写)。
        EmitPendingAndRegistered(*h.writer, assistant, "job-000001");
        Emit(*h.writer, v3::EventKindV3::ToolExecutionStarted, v3::OpStatus::Running, kAction,
             nlohmann::json{{"tool_call_id", kAction},
                            {"attempt", 1},
                            {"effectiveArgsRef", "args-000001"}});
        auto finished =
            Emit(*h.writer, v3::EventKindV3::ToolExecutionFinished, v3::OpStatus::Done, kAction,
                 nlohmann::json{{"tool_call_id", kAction}, {"attempt", 1}, {"exit_code", nullptr}});
        Emit(*h.writer, v3::EventKindV3::ToolResultPersisted, std::nullopt, kAction,
             nlohmann::json{{"tool_call_id", kAction},
                            {"attempt", 1},
                            {"result_ref", nlohmann::json::array({MakeArtifactRef("res-000001")})},
                            {"executionEventRef", finished.id}});
        Emit(*h.writer, v3::EventKindV3::ToolResultSelected, std::nullopt, kAction,
             nlohmann::json{{"tool_call_id", kAction},
                            {"attempt", 1},
                            {"sourceResultEventRefs", nlohmann::json::array({finished.id})},
                            {"hookEffectEventRefs", nlohmann::json::array()},
                            {"effectiveOutcome", "done"}});
        // 没有 tool 消息(崩溃在"结果选用后、tool 消息未提交"边界)。
    }
    std::atomic<int> executor_calls{0};
    auto writer = v3::V3Writer::Continue(jsonl);
    REQUIRE(writer.has_value());
    ToolJobCoordinator coord(*writer, AllowAll,
                             [&executor_calls](const JobExecutionContext&) {
                                 executor_calls.fetch_add(1);
                                 return Tool::Result::Text("requeued after repair");
                             });
    v3::V3Ledger ledger = v3::ReadV3Ledger(jsonl).value();
    JobRecoveryPlan plan = ToolJobCoordinator::PlanRecovery(ledger);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].disposition == "complete_delivery");
    CHECK(plan.items[0].detail == "admission_chain_missing");
    REQUIRE(coord.AdoptRecovery(plan) == 1);
    // 接单消息补上后重新入队,执行一轮到终态。
    JobWaitResult wait = coord.WaitJobs({"job-000001"}, 5000, true);
    REQUIRE(wait.satisfied);
    CHECK(wait.statuses[0].state == "succeeded");
    // 补链不重跑"接单执行"(attempt 1 只有一条 finished)。(账先落局部
    // 再遍历:range 表达式里的 expected 临时随完整表达式析构,直接绑
    // .value().events 是悬垂遍历 UB——gcc/libstdc++ 腿确定性踩雷,见册头
    // 夹具纪律。)
    int attempt1_finished = 0;
    v3::V3Ledger settled = v3::ReadV3Ledger(jsonl).value();
    for (const auto& event : settled.events) {
        if (event.kind == v3::EventKindV3::ToolExecutionFinished && event.action_id.has_value() &&
            *event.action_id == kAction && event.payload.contains("attempt") &&
            event.payload["attempt"].get<std::uint64_t>() == 1) {
            attempt1_finished += 1;
        }
    }
    CHECK(attempt1_finished == 1);
    // 接单 tool 消息(配对)在链上。
    auto obligations = v3::ProjectProtocolObligations(v3::ReadV3Ledger(jsonl).value());
    const auto* obligation = v3::FindProtocolObligation(obligations, kAction);
    REQUIRE(obligation != nullptr);
    CHECK(obligation->paired);
    CHECK(obligation->on_current_context);
    CHECK(executor_calls.load() == 1);
    CHECK(v3::ValidateAsyncToolSequence(v3::ReadV3Ledger(jsonl).value()).empty());
}

TEST_CASE("恢复:审批挂起与已终态;取消竞态唯一终态") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SUBCASE("awaiting_approval:Adopt 不派发,GrantApproval 后跑完") {
        std::filesystem::path jsonl;
        {
            Harness h("recover-approval");
            jsonl = h.jsonl;
            std::string assistant = h.AppendAssistantWithCall("call_A1");
            EmitPendingAndRegistered(*h.writer, assistant, "job-000001",
                                     nlohmann::json::object(), /*approval=*/true);
            EmitAdmissionChain(*h.writer, "job-000001");
        }
        std::atomic<int> executor_calls{0};
        auto writer = v3::V3Writer::Continue(jsonl);
        REQUIRE(writer.has_value());
        ToolJobCoordinator coord(*writer, AllowAll,
                                 [&executor_calls](const JobExecutionContext&) {
                                     executor_calls.fetch_add(1);
                                     return Tool::Result::Text("after approval");
                                 });
        JobRecoveryPlan plan = ToolJobCoordinator::PlanRecovery(v3::ReadV3Ledger(jsonl).value());
        REQUIRE(plan.items.size() == 1);
        CHECK(plan.items[0].disposition == "awaiting_approval");
        coord.AdoptRecovery(plan);
        CHECK(coord.GetJob("job-000001").state == "awaiting_approval");
        CHECK(executor_calls.load() == 0);
        REQUIRE(coord.GrantApproval("job-000001").ok);
        JobWaitResult wait = coord.WaitJobs({"job-000001"}, 5000, true);
        REQUIRE(wait.satisfied);
        CHECK(wait.statuses[0].state == "succeeded");
    }
    SUBCASE("已终态(取消与完成竞态落账后):Adopt 只登记,Cancel 回 already_terminal") {
        std::filesystem::path jsonl;
        {
            Harness h("recover-terminal");
            jsonl = h.jsonl;
            std::string assistant = h.AppendAssistantWithCall("call_A1");
            EmitPendingAndRegistered(*h.writer, assistant, "job-000001");
            EmitAdmissionChain(*h.writer, "job-000001");
            EmitDispatch(*h.writer, "job-000001", "epoch-1");
            // 取消请求与完成观测都在账(竞态已由先到者收口为 succeeded)。
            Emit(*h.writer, v3::EventKindV3::ToolJobCancelRequested, std::nullopt, kAction,
                 nlohmann::json{{"tool_call_id", kAction},
                                {"jobId", "job-000001"},
                                {"reason", "user_escape"}});
            const std::string persisted_id = EmitBusinessTerminal(*h.writer, "job-000001");
            EmitObserved(*h.writer, "job-000001", "succeeded", persisted_id);
        }
        auto writer = v3::V3Writer::Continue(jsonl);
        REQUIRE(writer.has_value());
        ToolJobCoordinator coord(*writer, AllowAll,
                                 [](const JobExecutionContext&) {
                                     return Tool::Result::Text("never");
                                 });
        JobRecoveryPlan plan = ToolJobCoordinator::PlanRecovery(v3::ReadV3Ledger(jsonl).value());
        REQUIRE(plan.items.size() == 1);
        CHECK(plan.items[0].disposition == "already_terminal");
        coord.AdoptRecovery(plan);
        JobStatusView view = coord.GetJob("job-000001");
        CHECK(view.state == "succeeded");  // 真实完成不改写成"未执行"
        CHECK(view.cancel_requested);      // 取消意图从账恢复(≠已终止)
        JobCancelResult cancel = coord.CancelJob("job-000001", "again");
        REQUIRE(cancel.ok);
        CHECK(cancel.status == "already_terminal");
        CHECK(cancel.terminal == "succeeded");
        // 账没被 Adopt/Cancel 追加任何 job 事件。(账先落局部再遍历:
        // range 表达式里的 expected 临时随完整表达式析构,直接绑
        // .value().events 是悬垂遍历 UB——gcc/libstdc++ 腿确定性踩雷,
        // 见册头夹具纪律。)
        int job_events = 0;
        v3::V3Ledger terminal_ledger = v3::ReadV3Ledger(jsonl).value();
        for (const auto& event : terminal_ledger.events) {
            const std::string name = v3::EventKindV3Name(event.kind);
            if (name.rfind("tool.job.", 0) == 0) {
                job_events += 1;
            }
        }
        // registered+dispatched+cancel_requested+observed = 4,原样。
        CHECK(job_events == 4);
    }
}

TEST_CASE("恢复:号池对齐,新单不与账上 jobId 撞号") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::filesystem::path jsonl;
    {
        Harness h("recover-numbering");
        jsonl = h.jsonl;
        std::string assistant = h.AppendAssistantWithCall("call_A1");
        EmitPendingAndRegistered(*h.writer, assistant, "job-000007");
        EmitAdmissionChain(*h.writer, "job-000007");
    }
    std::atomic<int> executor_calls{0};
    auto writer = v3::V3Writer::Continue(jsonl);
    REQUIRE(writer.has_value());
    ToolJobCoordinator coord(*writer, AllowAll,
                             [&executor_calls](const JobExecutionContext&) {
                                 executor_calls.fetch_add(1);
                                 return Tool::Result::Text("ok");
                             });
    coord.AdoptRecovery(ToolJobCoordinator::PlanRecovery(v3::ReadV3Ledger(jsonl).value()));
    coord.WaitJobs({"job-000007"}, 5000, true);
    // 新单从 job-000008 起。
    v3::MessageDraft draft;
    draft.turn_id = "turn-000001";
    draft.step_id = "step-000001";
    draft.request_id = "request-000001";
    draft.origin = v3::MessageOrigin::SessionRuntime;
    draft.provider = "openai";
    draft.wire = "responses";
    draft.model = "gpt-6";
    draft.response_model = nlohmann::json("gpt-6");
    draft.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
    draft.message = nlohmann::json::object({{"role", "assistant"}, {"content", "再查"}});
    auto assistant = writer->AppendMessage(draft, v3::Durability::PowerLoss);
    REQUIRE_MESSAGE(assistant.status == v3::WriteReceipt::Status::Committed,
                    assistant.error_message);
    ToolJobCoordinator* coord_ptr = &coord;
    JobStartRequest request;
    request.tool_name = "search";
    request.tool_input = nlohmann::json::object({{"query", "二"}});
    request.turn_id = "turn-000001";
    request.step_id = "step-000001";
    request.assistant_message_ref = assistant.id;
    JobStartResult fresh = coord_ptr->StartJob(request);
    REQUIRE(fresh.ok);
    CHECK(fresh.job_id == "job-000008");
}
