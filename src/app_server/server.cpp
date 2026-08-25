// server.hpp 的实现:装配方法表、thread 账、turn/start 的整回合驱动、
// 审批/ask_user 反向请求与 turn/interrupt 的接线(阶段 2)。
//
// 线程模型(阶段 2 起):
//   - 读线程(Run 所在线程):逐行读入站,请求法子大部分在读线程跑;
//   - 回合工作线程:turn/start 受理后把整回合挪进工作线程跑(handler
//     立即回 turnId,不堵读线程——审批悬停期间事件泵照常活,前端可以
//     随时发 interrupt/答复审批);
//   - 写线程:从 outbox 逐条 PopWait、写 stdout。
// 同一 thread 同拍两轮仍协议明拒(turn_running 的 CAS)。
#include "app_server/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "agent/session_catalog.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/session_command_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/turn_item.hpp"
#include "tools/ask_user.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "workflow/frontend.hpp"
#include "workflow/journal.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:入站用户文本的编码关口

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

// usage 报告 -> 事件字段(五项原样,缺失字段前端自己看)。
nlohmann::json UsageToJson(const api::Usage& usage) {
    return nlohmann::json{{"inputTokens", usage.input_tokens},
                          {"outputTokens", usage.output_tokens},
                          {"cacheReadTokens", usage.cache_read_tokens},
                          {"cacheCreationTokens", usage.cache_creation_tokens},
                          {"outputReasoningTokens", usage.output_reasoning_tokens}};
}

// runtime::DiffTable -> 协议 JSON(阶段 3:diff 行表直转,零翻译——
// 行就是行,渲染归前端)。字段名 camelCase,kind 是 "context"/"del"/
// "add"(与 runtime::DiffRowKind 一一对应)。
nlohmann::json DiffTableToJson(const runtime::DiffTable& table) {
    nlohmann::json rows = nlohmann::json::array();
    for (const runtime::DiffRow& row : table.rows) {
        rows.push_back(nlohmann::json{
            {"kind", row.kind == runtime::DiffRowKind::Context
                         ? "context"
                         : (row.kind == runtime::DiffRowKind::Del ? "del" : "add")},
            {"text", row.text},
            {"oldNo", row.old_no},
            {"newNo", row.new_no},
        });
    }
    nlohmann::json diff;
    diff["path"] = table.path;
    diff["located"] = table.located;
    diff["replacedCount"] = table.replaced_count;
    diff["oldExists"] = table.old_exists;
    diff["addedLines"] = table.added_lines();
    diff["removedLines"] = table.removed_lines();
    diff["rows"] = std::move(rows);
    return diff;
}

// workflow 的 journal 事件 -> 协议事件形状(阶段 4:wf 线的事件出口)。
// wf 事件没有 turn/item 语义,不硬塞三层账;method 用专属的
// workflow/event。两枚序号各归各:
//   - eventSeq:journal 的 run 内单调号(快照的 lastSeq 对的就是它,
//     前端增量补账用它筛);
//   - seq:连接层统一盖的协议事件序号(所有事件都有,见 connection.cpp)。
nlohmann::json WorkflowEventToProtocol(const workflow::JournalEvent& event, const std::string& run_id) {
    nlohmann::json params;
    params["runId"] = run_id;
    params["eventSeq"] = event.seq;
    params["type"] = event.type;
    params["workflowId"] = event.workflow_id;
    if (!event.node_id.empty()) {
        params["nodeId"] = event.node_id;
    }
    if (event.attempt != 0) {
        params["attempt"] = event.attempt;
    }
    if (!event.data.is_null() && !event.data.empty()) {
        params["data"] = event.data;
    }
    return nlohmann::json{{"method", "workflow/event"}, {"params", std::move(params)}};
}

// 一回合聚合的 usage(各请求求和)。
api::Usage SumUsage(const std::vector<api::UsageReport>& reports) {
    api::Usage total;
    for (const api::UsageReport& report : reports) {
        total.input_tokens += report.usage.input_tokens;
        total.output_tokens += report.usage.output_tokens;
        total.cache_read_tokens += report.usage.cache_read_tokens;
        total.cache_creation_tokens += report.usage.cache_creation_tokens;
        total.output_reasoning_tokens += report.usage.output_reasoning_tokens;
    }
    return total;
}

}  // namespace

std::string PlatformId() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

Server::Server(ServerOptions options, BackendFactory backend_factory, RegistryFactory registry_factory)
    : options_(std::move(options)), backend_factory_(std::move(backend_factory)),
      registry_factory_(std::move(registry_factory)), sessions_dir_(options_.sessions_dir) {
    dispatcher_ = std::make_shared<Dispatcher>();
    dispatcher_->SetInitializeResultFactory(
        [this]() { return MakeInitializeResult(options_.lubancode_version, PlatformId()); });
    // P9 收尾:会话查询/搬删的执行体(sessions_dir 空 = 没建,list 给空表)。
    if (!sessions_dir_.empty()) {
        session_commands_ = std::make_unique<runtime::SessionCommandService>(sessions_dir_);
    }
    RegisterMethods();
}

std::shared_ptr<ThreadRecord> Server::FindThread(const std::string& thread_id) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const auto it = threads_.find(thread_id);
    return it == threads_.end() ? nullptr : it->second;
}

Server::~Server() {
    // 析构兜底:在跑的回合收口(与 Shutdown 同路),不能让 joinable 的
    // 工作线程把进程掀了(std::thread 析构时 joinable = terminate)。
    Shutdown();
}

void Server::RegisterMethods() {
    // thread/start
    dispatcher_->RegisterMethod(
        kMethodThreadStart, [this](const IncomingRequest& request, DispatchContext& context)
                           -> std::optional<nlohmann::json> {
            std::string error_code;
            // 参数表在 handler 内查(纯函数,错误码稳定)。
            const ParamsCheck base = CheckThreadStartParams(request.params);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            const nlohmann::json result = HandleThreadStart(request.params, error_code);
            if (!error_code.empty()) {
                return MakeError(request.id, kErrInternalError, "thread/start 失败: " + error_code);
            }
            // thread/started 事件在响应之前发:前端先见事件后见响应,顺眼
            // 也顺逻辑(threadId 是事件给出来的身份)。
            context.emit_event(kEventThreadStarted,
                               MakeThreadStartedParams(result.value("threadId", std::string()),
                                                        result.value("cwd", std::string())),
                               false);
            return MakeResult(request.id, result);
        });

    // thread/list(P9:查询参数透传 SessionCommandService,server 不另写
    // 扫盘路。旧响应形状 {threads:[...]} 不变,新加 total)。
    dispatcher_->RegisterMethod(
        kMethodThreadList, [this](const IncomingRequest& request, DispatchContext&)
                             -> std::optional<nlohmann::json> {
            const ParamsCheck base = CheckParamsIsObject(request.params, kMethodThreadList);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            // 协议缺省与旧口径对齐:全量 scope + active + updated;不截
            // (SessionCommandService 缺省 limit=20 是终端 picker 的口径,
            // 协议侧默认给全量,前端自己分页)。
            nlohmann::json query = request.params;
            if (!query.contains("scope")) {
                query["scope"] = "all";
            }
            if (!query.contains("limit")) {
                query["limit"] = 1000000; // 不截
            }
            return MakeResult(request.id, HandleThreadList(query));
        });

    // thread/archive|unarchive|delete(P9 收尾:SessionCommandService 执行,
    // 按命令分家——server 只折协议错误码)。
    const auto lifecycle_handler =
        [this](std::string_view method) -> MethodHandler {
        return [this, method](const IncomingRequest& request, DispatchContext& context)
            -> std::optional<nlohmann::json> {
            std::string thread_id;
            const ParamsCheck base = CheckThreadLifecycleParams(request.params, thread_id);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            std::string error_message;
            std::string state;
            const nlohmann::json result =
                HandleThreadLifecycle(std::string(method), thread_id, request.params, error_code,
                                      error_message, state);
            if (!error_code.empty()) {
                // delete 没带 confirm 是"要确认"不是失败:kErrInvalidParams
                // 的口径太重,给参数错 + data 带 required=confirm,前端好
                // 画确认框。其余(not_found/io_error)按 invalid params 折
                // ——协议 v1 的错误码段还没有会话搬删专属码,拿 data 带
                // 稳定串补。
                return MakeError(request.id, kErrInvalidParams, error_message.empty()
                                                                   ? std::string(method) + " 失败: " + error_code
                                                                   : error_message,
                                 nlohmann::json{{"reason", error_code}});
            }
            if (method == kMethodThreadDelete) {
                context.emit_event("thread/deleted",
                                   nlohmann::json{{"threadId", thread_id}}, true);
            } else {
                context.emit_event("thread/updated",
                                   nlohmann::json{{"threadId", thread_id}, {"state", state}}, true);
            }
            return MakeResult(request.id, result);
        };
    };
    dispatcher_->RegisterMethod(kMethodThreadArchive, lifecycle_handler(kMethodThreadArchive));
    dispatcher_->RegisterMethod(kMethodThreadUnarchive, lifecycle_handler(kMethodThreadUnarchive));
    dispatcher_->RegisterMethod(kMethodThreadDelete, lifecycle_handler(kMethodThreadDelete));

    // trace/query(逐枚追踪单第 5 期):断线补账与冷回放。事件从 session
    // 存档的 tool_trace_v1 行折叠——线程重启、app-server 重启后都有账可
    // 查(单子:"app-server 断线按 seq 补事件,必要时从 session trace
    // 冷回放")。lastSeq 是"客户端已见到的最大 seq",返回大于它的;缺省
    // 0 = 全量。脱敏是默认:正文只回 preview 摘要,不回 inline 原文
    // (单子"/trace 默认遮敏;--raw 须本机交互确认"——远端通道没有本机
    // 交互,一律走遮敏档)。
    dispatcher_->RegisterMethod(
        kMethodTraceQuery, [this](const IncomingRequest& request, DispatchContext&)
                             -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::uint64_t last_seq = 0;
            const ParamsCheck base = CheckTraceQueryParams(request.params, thread_id, last_seq);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            // 会话档路径:ThreadRecord 活着取 file_path,没活着的 thread
            // 按 sessions_dir 拼(存档还在盘上,冷回放照给)。
            std::string file_path;
            {
                std::lock_guard<std::mutex> lock(threads_mutex_);
                const auto it = threads_.find(thread_id);
                if (it != threads_.end() && it->second->store != nullptr) {
                    file_path = it->second->store->file_path();
                }
            }
            if (file_path.empty() && !sessions_dir_.empty()) {
                file_path = sessions_dir_ + "/" + thread_id + ".jsonl";
            }
            if (file_path.empty()) {
                return MakeError(request.id, kErrInvalidParams,
                                 "trace/query: 没有会话存档(纯内存 thread 或未配置 sessionsDir)");
            }
            const auto bytes = agent::ReadSessionFileBytes(file_path);
            if (!bytes.has_value()) {
                return MakeError(request.id, kErrInvalidParams, "trace/query: 会话档读不到: " + file_path);
            }
            const auto loaded = agent::ParseSessionFile(*bytes);
            if (!loaded.has_value()) {
                return MakeError(request.id, kErrInvalidParams, "trace/query: 会话档解析失败: " + file_path);
            }
            const auto ledger = runtime::ToolTraceHub::BuildLedger(loaded->tool_trace_events);

            // 可选过滤。
            const std::string filter_execution =
                request.params.value("executionId", std::string());
            const std::string filter_tool_use = request.params.value("toolUseId", std::string());
            const std::string filter_turn = request.params.value("turnId", std::string());
            const bool errors_only =
                request.params.value("errorsOnly", false);

            nlohmann::json executions = nlohmann::json::array();
            std::uint64_t max_seq = last_seq;
            for (const auto& record : ledger.executions()) {
                // seq 过滤:lastSeq 之后的才回(断线补账的口径)。
                if (record.seq_scheduled <= last_seq) {
                    continue;
                }
                if (!filter_execution.empty() && record.execution_id != filter_execution) {
                    continue;
                }
                if (!filter_tool_use.empty() && record.tool_use_id != filter_tool_use) {
                    continue;
                }
                if (!filter_turn.empty() && record.turn_id != filter_turn) {
                    continue;
                }
                if (errors_only && record.outcome == agent::ToolOutcome::Succeeded) {
                    continue;
                }
                nlohmann::json item;
                item["executionId"] = record.execution_id;
                item["toolUseId"] = record.tool_use_id;
                item["toolName"] = record.tool_name;
                item["batchId"] = record.batch_id;
                item["sequenceInBatch"] = record.sequence_in_batch;
                item["turnId"] = record.turn_id;
                item["source"] = agent::ToString(record.source_kind);
                if (!record.source_instance.empty()) {
                    item["sourceInstance"] = record.source_instance;
                }
                if (!record.parent_execution_id.empty()) {
                    item["parentExecutionId"] = record.parent_execution_id;
                }
                if (!record.retry_of.empty()) {
                    item["retryOf"] = record.retry_of;
                }
                if (!record.blocked_by.empty()) {
                    item["blockedBy"] = record.blocked_by;
                }
                if (!record.compensates.empty()) {
                    item["compensates"] = record.compensates;
                }
                item["outcome"] = agent::ToString(record.outcome);
                if (!record.error_code.empty()) {
                    item["errorCode"] = record.error_code;
                }
                item["durationMs"] = record.duration_ms;
                item["recovery"] = agent::ToString(record.Classify());
                item["corrupt"] = record.corrupt;
                // 遮敏默认:preview 与字节/hash 只给摘要,不给 inline 原文。
                item["resultBytes"] = record.result_ref.bytes;
                item["resultSha256"] = record.result_ref.sha256;
                if (!record.result_ref.preview.empty()) {
                    item["resultPreview"] = record.result_ref.preview;
                }
                if (!record.result_ref.artifact_id.empty()) {
                    item["resultArtifactId"] = record.result_ref.artifact_id;
                }
                executions.push_back(std::move(item));
                max_seq = std::max(max_seq, record.seq_scheduled);
            }

            return MakeResult(request.id,
                              nlohmann::json{{"threadId", thread_id},
                                             {"lastSeq", max_seq},
                                             {"count", executions.size()},
                                             {"executions", std::move(executions)}});
        });

    // workflow/query(wf 线的事件出口:LoadSnapshotFromDisk +
    // BuildIncrementalEvents,server 只折协议形状)。
    dispatcher_->RegisterMethod(
        kMethodWorkflowQuery, [this](const IncomingRequest& request, DispatchContext& context)
                              -> std::optional<nlohmann::json> {
            std::string run_id;
            std::uint64_t last_seq = 0;
            const ParamsCheck base = CheckWorkflowQueryParams(request.params, run_id, last_seq);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            std::string error_message;
            WorkflowQueryResult result =
                HandleWorkflowQuery(run_id, last_seq, error_code, error_message);
            if (!error_code.empty()) {
                return MakeError(request.id, kErrInvalidParams,
                                 error_message.empty() ? "workflow/query 失败: " + error_code
                                                       : error_message,
                                 nlohmann::json{{"reason", error_code}});
            }
            // 增量事件先出(emit_event),响应(快照)后出:前端先接事件
            // 后拿快照对账,与 thread/started 先于响应的口径一致。
            for (const nlohmann::json& event : result.events) {
                context.emit_event(event.value("method", std::string()),
                                   event.value("params", nlohmann::json::object()), false);
            }
            return MakeResult(request.id, result.snapshot);
        });

    // thread/stop
    dispatcher_->RegisterMethod(
        kMethodThreadStop, [this](const IncomingRequest& request, DispatchContext& context)
                           -> std::optional<nlohmann::json> {
            std::string thread_id;
            const ParamsCheck base = CheckThreadStopParams(request.params, thread_id);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json result = HandleThreadStop(thread_id, error_code);
            if (!error_code.empty()) {
                return MakeError(request.id, kErrInvalidParams, "thread/stop 失败: " + error_code);
            }
            context.emit_event(kEventThreadStopped, MakeThreadStoppedParams(thread_id), false);
            return MakeResult(request.id, result);
        });

    // turn/start
    dispatcher_->RegisterMethod(
        kMethodTurnStart, [this](const IncomingRequest& request, DispatchContext&)
                          -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::string text;
            std::vector<nlohmann::json> images;
            const ParamsCheck base =
                CheckTurnStartParams(request.params, thread_id, text, images);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json accepted = AcceptTurnStart(thread_id, text, images, error_code);
            if (!error_code.empty()) {
                if (error_code == "already_running") {
                    return MakeError(request.id, kErrTurnAlreadyRunning,
                                     "该 thread 已有回合在跑: " + thread_id);
                }
                return MakeError(request.id, kErrInvalidParams, "turn/start 失败: " + error_code);
            }
            // 立即回 {threadId, turnId}:整回合在工作线程跑,终态走
            // turn/completed 事件(handler 不等回合结束——审批悬停期间
            // 读线程还得收前端的答复与 interrupt)。
            return MakeResult(request.id, accepted);
        });

    // turn/interrupt
    dispatcher_->RegisterMethod(
        kMethodTurnInterrupt, [this](const IncomingRequest& request, DispatchContext&)
                              -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::string turn_id;
            const ParamsCheck base = CheckTurnInterruptParams(request.params, thread_id, turn_id);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json result = HandleTurnInterrupt(thread_id, turn_id, error_code);
            if (!error_code.empty()) {
                if (error_code == "stale") {
                    // 迟到的打断:回合不在跑(收口了/没这回合),不追旧账。
                    return MakeError(request.id, kErrStaleRequestId,
                                     "回合不在跑,打断不受理: " + thread_id + " " + turn_id);
                }
                return MakeError(request.id, kErrInvalidParams, "turn/interrupt 失败: " + error_code);
            }
            return MakeResult(request.id, result);
        });

    // goal/loop/plan 的 typed 命令面(goal 单合流批):一个 handler 吃
    // 全部十六枚方法——参数表与命令折算都在 HandleTypedDomainCommand,
    // 错误码走 data.reason 带稳定串(goal.*/loop.*/plan.*/confirmation_
    // required/stale_request_id),协议 v1 的错误码段没有这些专属号。
    const auto domain_handler = [this](const IncomingRequest& request, DispatchContext&)
                                     -> std::optional<nlohmann::json> {
        bool error = false;
        std::string error_code;
        std::string error_message;
        const nlohmann::json result =
            HandleTypedDomainCommand(request, error, error_code, error_message);
        if (error) {
            return MakeError(request.id, kErrInvalidParams,
                             error_message.empty() ? std::string(request.method) + " 失败: " + error_code
                                                   : error_message,
                             nlohmann::json{{"reason", error_code}});
        }
        return MakeResult(request.id, result);
    };
    dispatcher_->RegisterMethod(kMethodGoalCreate, domain_handler);
    dispatcher_->RegisterMethod(kMethodGoalGet, domain_handler);
    dispatcher_->RegisterMethod(kMethodGoalEdit, domain_handler);
    dispatcher_->RegisterMethod(kMethodGoalPause, domain_handler);
    dispatcher_->RegisterMethod(kMethodGoalResume, domain_handler);
    dispatcher_->RegisterMethod(kMethodGoalClear, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopCreate, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopList, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopRead, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopPause, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopResume, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopCancel, domain_handler);
    dispatcher_->RegisterMethod(kMethodLoopRunNow, domain_handler);
    dispatcher_->RegisterMethod(kMethodPlanSetMode, domain_handler);
    dispatcher_->RegisterMethod(kMethodPlanReview, domain_handler);
    dispatcher_->RegisterMethod(kMethodPlanReopen, domain_handler);
}

std::size_t Server::active_thread_count() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    return threads_.size();
}

nlohmann::json Server::HandleThreadStart(const nlohmann::json& params, std::string& out_error_code) {
    out_error_code.clear();
    auto record = std::make_shared<ThreadRecord>(std::string());
    record->cwd = params.value("cwd", std::string());
    if (record->cwd.empty()) {
        record->cwd = options_.cwd;
    }

    // 会话账:复用 SessionStore,不另立第二本账。首句摘要用一句占位的
    // 协议话——thread/start 阶段还没有用户文本(单子的存档恢复线会把
    // 真首句补进来);MakeSessionId 的 slug 拿它生成文件名。
    record->thread_id = agent::MakeSessionId(agent::NowIdTimestamp(), "app-server-thread");
    record->interactions = std::make_unique<InteractionLedger>(record->thread_id);
    if (!sessions_dir_.empty()) {
        record->store = std::make_unique<agent::SessionStore>(sessions_dir_);
        agent::SessionMeta meta;
        // 阶段 3 冻结项:meta 写真值。wire 是协议名(anthropic/responses/
        // chat,配置四级合并的结果),model 是配置里的模型名;测试/纯内存
        // 跑没有配置,空串照写(与 CLI 会话档同一张 meta 表)。
        meta.wire = options_.session_wire;
        meta.model = options_.session_model;
        meta.cwd = record->cwd;
        meta.started_at = agent::NowTimestamp();
        if (!record->store->Begin(meta, record->thread_id)) {
            // 落盘失败不拦协议:thread 照开,只打 stderr。
            Diagnose("会话建档失败,本场不落盘: " + sessions_dir_);
            record->store.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads_[record->thread_id] = record;
    }
    // goal 单合流批:typed 命令面的会话级状态按 thread 各起一本。goal/
    // loop 的 feature 门从 options 折(关着也建实例——命令面回稳定禁用
    // 码,存档侧不碰);Plan 的 SessionRuntime 纯内存跑(sessions_dir 空,
    // 存档走 record->store 自己那条,不掺和)。
    {
        runtime::goal::GoalCoordinator::Options goal_options;
        goal_options.goals_enabled = options_.features_goal;
        record->goal_coordinator = std::make_unique<runtime::goal::GoalCoordinator>(goal_options);
        runtime::loop::LoopScheduler::Options loop_options;
        loop_options.enabled = options_.features_loop;
        record->loop_scheduler = std::make_unique<runtime::loop::LoopScheduler>(loop_options);
        runtime::SessionRuntime::Options runtime_options;  // sessions_dir 空 = 纯内存
        record->session_runtime = std::make_unique<runtime::SessionRuntime>(runtime_options);
    }
    Diagnose("thread 已建: " + record->thread_id);
    return nlohmann::json{{"threadId", record->thread_id}, {"cwd", record->cwd}};
}

nlohmann::json Server::HandleThreadList(const nlohmann::json& params) {
    // P9 收尾:列举走 SessionCommandService(会话管理器单第六步)——与
    // 终端 picker 共吃一碗饭,同一查询给同一份 id/顺序/状态,server 不
    // 另写第二条扫盘路。sessions_dir 空(纯内存跑)给空表。
    if (session_commands_ == nullptr) {
        return MakeThreadListResult({});
    }
    const runtime::SessionCommandOutcome outcome = session_commands_->ListThreads(params);
    if (!outcome.accepted) {
        // ListThreads 现下不会拒(query 解析容错);防御给空表。
        Diagnose("thread/list 拒绝: " + outcome.error_code);
        return MakeThreadListResult({});
    }
    // 旧字段 startedAt 继续给(createdAt 同源),老前端不断。
    nlohmann::json result = outcome.payload;
    if (result.contains("threads") && result["threads"].is_array()) {
        for (nlohmann::json& thread : result["threads"]) {
            if (thread.contains("createdAt")) {
                thread["startedAt"] = thread["createdAt"];
            }
        }
    }
    return result;
}

nlohmann::json Server::HandleThreadLifecycle(const std::string& method, const std::string& thread_id,
                                             const nlohmann::json& params, std::string& out_error_code,
                                             std::string& out_error_message, std::string& out_state) {
    out_error_code.clear();
    out_error_message.clear();
    out_state.clear();
    if (session_commands_ == nullptr) {
        out_error_code = "not_found";
        out_error_message = "服务没有会话档目录,搬删一律不可用";
        return nlohmann::json();
    }
    // 在跑的 thread 不许搬删(句柄在 SessionStore 手里,Windows 上 rename
    // 会吃 sharing violation;协议侧明拒,不留给 IO 层炸)。
    if (FindThread(thread_id) != nullptr) {
        out_error_code = "active_thread";
        out_error_message = "thread 还开着,先 thread/stop 再搬删";
        return nlohmann::json();
    }
    runtime::SessionCommandOutcome outcome;
    if (method == std::string(kMethodThreadArchive)) {
        outcome = session_commands_->ArchiveThread(thread_id);
        out_state = "archived";
    } else if (method == std::string(kMethodThreadUnarchive)) {
        outcome = session_commands_->UnarchiveThread(thread_id);
        out_state = "active";
    } else if (method == std::string(kMethodThreadDelete)) {
        outcome = session_commands_->DeleteThread(thread_id, params);
    } else {
        out_error_code = "invalid_request";
        out_error_message = "不认识的会话命令: " + method;
        return nlohmann::json();
    }
    if (!outcome.accepted) {
        out_error_code = outcome.error_code;
        out_error_message = outcome.error_message;
        out_state.clear();
        return nlohmann::json();
    }
    return MakeThreadLifecycleResult(thread_id, out_state);
}

Server::WorkflowQueryResult Server::HandleWorkflowQuery(const std::string& run_id,
                                                         std::uint64_t last_seq,
                                                         std::string& out_error_code,
                                                         std::string& out_error_message) {
    out_error_code.clear();
    out_error_message.clear();
    WorkflowQueryResult result;
    if (options_.workflow_runs_dir.empty()) {
        out_error_code = "no_workflow_dir";
        out_error_message = "服务没配 workflow run 账目录";
        return result;
    }
    const std::filesystem::path runs_root =
        tools::Utf8ToPath(options_.workflow_runs_dir);
    const std::filesystem::path run_dir = runs_root / tools::Utf8ToPath(run_id);
    std::error_code ec;
    if (!std::filesystem::exists(run_dir, ec)) {
        out_error_code = "not_found";
        out_error_message = "run 不存在: " + run_id;
        return result;
    }
    const std::optional<workflow::WorkflowRunSnapshot> snapshot =
        workflow::LoadSnapshotFromDisk(run_dir);
    if (!snapshot.has_value()) {
        out_error_code = "not_found";
        out_error_message = "run 账读不出(无事件账或损坏): " + run_id;
        return result;
    }
    // 快照折 camelCase(协议惯例:threadId/turnId/lastSeq 一路 camel;
    // snapshot 的 snake_case 是 wf 层存档口径,进协议转一层,字段一一
    // 对应不丢)。
    nlohmann::json raw = snapshot->ToJson();
    nlohmann::json shaped;
    shaped["runId"] = raw.value("run_id", std::string());
    shaped["workflowId"] = raw.value("workflow_id", std::string());
    shaped["workflowVersion"] = raw.value("workflow_version", std::string());
    shaped["contentHash"] = raw.value("content_hash", std::string());
    shaped["state"] = raw.value("state", std::string());
    shaped["errorCode"] = raw.value("error_code", std::string());
    shaped["errorMessage"] = raw.value("error_message", std::string());
    shaped["result"] = raw.value("result", nlohmann::json::object());
    shaped["durationMs"] = raw.value("duration_ms", std::int64_t{0});
    shaped["tokensUsed"] = raw.value("tokens_used", std::uint64_t{0});
    shaped["lastSeq"] = raw.value("last_seq", std::uint64_t{0});
    if (raw.contains("nodes")) {
        shaped["nodes"] = raw["nodes"];
    }
    result.snapshot = std::move(shaped);
    // 增量事件:journal 事件从 last_seq+1 起(0 = 全量),直接折协议形状。
    // 与 BuildIncrementalEvents 同一个筛法(单子口径:前端重连先取快照,
    // 再从 last_seq+1 接事件);这里不走 ServerEvent 中转——wf 事件没有
    // turn/item 语义,硬套三层账只会丢信息。
    for (const workflow::JournalEvent& event : workflow::ReadJournalEvents(run_dir)) {
        if (event.seq > last_seq) {
            result.events.push_back(WorkflowEventToProtocol(event, run_id));
        }
    }
    return result;
}

nlohmann::json Server::HandleThreadStop(const std::string& thread_id, std::string& out_error_code) {
    out_error_code.clear();
    std::shared_ptr<ThreadRecord> record;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        const auto it = threads_.find(thread_id);
        if (it == threads_.end()) {
            out_error_code = "不认识的 threadId: " + thread_id;
            return nlohmann::json();
        }
        record = it->second;
        threads_.erase(it);
    }
    if (record->turn_running.load()) {
        // 在跑回合按打断收口:置旗、悬起件全清(审批悬停立即醒,按
        // "thread 关闭"的悬空收口处理),等回合工作线程收尾(硬时限内),
        // 等不到就分离——store 的柄不能在它还写着时收。
        record->interrupt_requested.store(true);
        record->interactions->CancelPending();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options_.interrupt_hard_deadline_ms);
        while (!record->turn_finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (record->turn_finished.load() && record->turn_worker.joinable()) {
            record->turn_worker.join();
        } else if (record->turn_worker.joinable()) {
            Diagnose("thread 停场时回合未在硬时限内收口,分离工作线程: " + thread_id);
            record->turn_worker.detach();
        }
    }
    if (record->store != nullptr) {
        record->store->Reset(); // 文件留在磁盘上,只是句柄收掉
    }
    Diagnose("thread 已停: " + thread_id);
    return MakeThreadStoppedResult();
}

nlohmann::json Server::AcceptTurnStart(const std::string& thread_id, const std::string& text,
                                       const std::vector<nlohmann::json>& images,
                                       std::string& out_error_code) {
    out_error_code.clear();

    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr) {
        out_error_code = "不认识的 threadId: " + thread_id;
        return nlohmann::json();
    }

    // 同一 thread 同拍两轮:协议明拒(单子验收:规矩写死并有测试)。
    bool expected = false;
    if (!record->turn_running.compare_exchange_strong(expected, true)) {
        out_error_code = "already_running";
        return nlohmann::json();
    }

    // P9(显示系统剥离单):统一发号换 runtime::ProcessIdAuthority——
    // id_authority.hpp 定过的规矩:只此一家,不许各处再造第二套。
    const std::string turn_id = runtime::ProcessIdAuthority().NextTurnId();
    record->turn_id = turn_id;
    record->interrupted_turn.clear();
    record->interrupt_requested.store(false);
    record->turn_finished.store(false);
    if (record->turn_worker.joinable()) {
        record->turn_worker.join(); // 上一轮的尾巴(正常已收,防御)
    }
    record->turn_worker = std::thread([this, record, thread_id, turn_id, text, images] {
        RunTurnToCompletion(record, thread_id, turn_id, text, images);
    });
    return nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}};
}

nlohmann::json Server::HandleTurnStart(const std::string& thread_id, const std::string& text,
                                       const std::vector<nlohmann::json>& images,
                                       std::string& out_error_code) {
    // 兼容直驱(单测与阶段 1 的口径):受理 + 等工作线程收尾,返回
    // turn/completed 的 params。协议路径(读线程)只调 AcceptTurnStart,
    // 立即回 turnId——这里给同步消费方留一条等完的路。
    const nlohmann::json accepted = AcceptTurnStart(thread_id, text, images, out_error_code);
    if (!out_error_code.empty()) {
        return nlohmann::json();
    }
    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr || !record->turn_worker.joinable()) {
        out_error_code = "回合工作线程没能立起";
        return nlohmann::json();
    }
    // interrupt 的硬时限:置旗后 loop.Run 最多在流式/工具边界处收口;真有
    // 卡死不退的(长命令/卡死的外部进程),等满 interrupt_hard_deadline_ms
    // 就不再等——回合标记收口失败,join 分离(detach),读线程不被拖死。
    // 这里是同步口径的等法;协议路径的兜底在连接收线(断管/exit)时由
    // 析构路径兜。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.interrupt_hard_deadline_ms);
    while (!record->turn_finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (record->turn_finished.load()) {
        record->turn_worker.join();
        return record->last_completed;
    }
    // 硬时限已到回合还没收口:不 join(卡死工作线程不该拖死调用方),
    // detach 让它自生自灭,终态事件由它(假如还活着)自己补发。
    Diagnose("回合未在硬时限内收口,分离工作线程: " + thread_id + " " + record->turn_id);
    record->turn_worker.detach();
    record->turn_running.store(false);
    out_error_code = "hard_deadline";
    return nlohmann::json();
}

// 整回合驱动:在工作线程跑。审批/ask_user 悬停期间读线程照常收前端的
// 答复(HandleInteractionResponse)与打断(HandleTurnInterrupt);悬停的
// future 靠 promise 被读线程唤醒,事件泵不堵。
void Server::RunTurnToCompletion(const std::shared_ptr<ThreadRecord>& record, const std::string& thread_id,
                                 const std::string& turn_id, const std::string& text,
                                 const std::vector<nlohmann::json>& images) {
    connection_->EmitEvent(kEventTurnStarted, MakeTurnStartedParams(thread_id, turn_id));

    // 用户消息:text + 图片(images 字段名与 api::ImageBlock 对齐,阶段 3
    // 冻结)。图片原样入 history——下一轮、重放、会话恢复都带得上。
    api::Message user_message;
    user_message.role = api::Role::User;
    // 入站用户文本:JSON parse 不校验 UTF-8,前端/代理可能带进坏串,
    // 进 history 前洗掉。
    user_message.content.push_back(api::TextBlock{platform::SanitizeExternalText(text)});
    for (const nlohmann::json& image : images) {
        api::ImageBlock block;
        block.media_type = image.value("mediaType", std::string());
        block.data = image.value("data", std::string());
        block.filename = image.value("filename", std::string());
        block.width = image.value("width", 0);
        block.height = image.value("height", 0);
        user_message.content.push_back(std::move(block));
    }
    if (record->store != nullptr) {
        record->store->AppendMessage(user_message);
    }

    // 事件账:P9 起条目 id 与事件序号都从 runtime::ProcessIdAuthority 发
    // (id_authority.hpp 的"只此一家"),旧 next_item_seq 回合内计数拆掉。
    nlohmann::json completed_params;
    {
        // 装配:假 backend + 注册表(骨架期工具链由测试注入假工具;生产
        // 装配走 cli_app 的 registry_factory)。
        // TODO(plugins 第 7 步的 app-server 侧收尾,另一线接手):插件工具
        // 挂进这条 registry 后,ESC/取消链与 PluginLogSink(事件流日志)也
        // 要照 ToolRuntime::SetPluginCancel/SetPluginLogSink 的口子接进来;
        // 本线只做了 Terminal 侧,app-server 深度挂载不在这里展开。
        std::unique_ptr<api::Backend> backend = backend_factory_();
        std::unique_ptr<tools::ToolRegistry> registry =
            registry_factory_ ? registry_factory_() : std::make_unique<tools::ToolRegistry>();

        agent::AgentRuntimeProfile profile;
        profile.max_steps_per_turn = 32; // 骨架期防跑飞;真配置线接进来再换
        agent::Agent loop(*backend, *registry, std::move(profile), std::string("lubancode app-server"));

        agent::Callbacks callbacks;
        runtime::IdAuthority& ids = runtime::ProcessIdAuthority();
        std::string text_item_id;
        callbacks.on_text_delta = [&](const std::string& delta) {
            if (text_item_id.empty()) {
                text_item_id = ids.NextItemId();
                connection_->EmitEvent(kEventItemStarted,
                                       MakeItemStartedParams(thread_id, turn_id, text_item_id, kItemTypeText,
                                                             nlohmann::json::object()));
            }
            connection_->EmitEvent(kEventItemDelta,
                                   MakeItemDeltaParams(thread_id, turn_id, text_item_id, delta));
        };
        std::string thinking_item_id;
        callbacks.on_thinking_delta = [&](const std::string& delta) {
            if (thinking_item_id.empty()) {
                thinking_item_id = ids.NextItemId();
                connection_->EmitEvent(kEventItemStarted,
                                       MakeItemStartedParams(thread_id, turn_id, thinking_item_id,
                                                             kItemTypeThinking, nlohmann::json::object()));
            }
            connection_->EmitEvent(kEventItemDelta,
                                   MakeItemDeltaParams(thread_id, turn_id, thinking_item_id, delta));
        };

        // ---- 工具条目(阶段 3:diff 行表直转) ----
        // write_file/edit_file 的 item/started 带 "diff":runtime::DiffTable
        // 中立行表直转(path/located/replacedCount/oldExists/addedLines/
        // removedLines/rows[{kind,text,oldNo,newNo}])。别的工具不带该字段。
        // 行表在 on_tool_start 算(预览语义:改动还没落盘,终端确认块同款);
        // run_command 的条目类型用 command(输出整段落在 item/completed)。
        const auto emit_tool_started = [&](const std::string& tool_use_id, const std::string& name,
                                           const nlohmann::json& input) {
            const std::string item_id = ids.NextItemId();
            const std::string_view item_type =
                name == "run_command" ? kItemTypeCommand : kItemTypeTool;
            nlohmann::json payload;
            payload["tool"] = name;
            if (!tool_use_id.empty()) {
                payload["toolUseId"] = tool_use_id;
            }
            if (input.is_object() && !input.empty()) {
                payload["input"] = input;
            }
            if (const std::optional<runtime::DiffTable> table =
                    runtime::BuildDiffTable(name, input)) {
                payload["diff"] = DiffTableToJson(*table);
            }
            connection_->EmitEvent(kEventItemStarted,
                                   MakeItemStartedParams(thread_id, turn_id, item_id, item_type,
                                                         std::move(payload)));
            return item_id;
        };
        std::map<std::string, std::string> open_tools; // tool_use_id -> item_id
        callbacks.on_tool_start = [&](const std::string& tool_use_id, const std::string& name,
                                      const nlohmann::json& input) {
            if (!text_item_id.empty()) {
                connection_->EmitEvent(
                    kEventItemCompleted,
                    MakeItemCompletedParams(thread_id, turn_id, text_item_id, nlohmann::json::object()));
                text_item_id.clear();
            }
            if (!thinking_item_id.empty()) {
                connection_->EmitEvent(
                    kEventItemCompleted,
                    MakeItemCompletedParams(thread_id, turn_id, thinking_item_id, nlohmann::json::object()));
                thinking_item_id.clear();
            }
            open_tools[tool_use_id] = emit_tool_started(tool_use_id, name, input);
        };
        const auto finish_tool = [&](const std::string& tool_use_id, const nlohmann::json& payload) {
            // 空兜底"最早一个进行中的"(与 ToolDisplay 同款);查不到 =
            // 迟到/陌生,丢弃不误伤。
            std::string item_id;
            if (!tool_use_id.empty()) {
                const auto it = open_tools.find(tool_use_id);
                if (it == open_tools.end()) {
                    return;
                }
                item_id = it->second;
                open_tools.erase(it);
            } else if (!open_tools.empty()) {
                item_id = open_tools.begin()->second;
                open_tools.erase(open_tools.begin());
            } else {
                return;
            }
            connection_->EmitEvent(kEventItemCompleted,
                                   MakeItemCompletedParams(thread_id, turn_id, item_id, payload));
        };
        callbacks.on_tool_done = [&](const std::string& tool_use_id, const std::string& /*name*/,
                                     const tools::Tool::Result& result) {
            finish_tool(tool_use_id,
                        nlohmann::json{{"result", result.content}, {"isError", result.is_error}});
        };
        callbacks.on_builtin_tool_start = [&](const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input) {
            if (!text_item_id.empty()) {
                connection_->EmitEvent(
                    kEventItemCompleted,
                    MakeItemCompletedParams(thread_id, turn_id, text_item_id, nlohmann::json::object()));
                text_item_id.clear();
            }
            open_tools[tool_use_id] = emit_tool_started(tool_use_id, name, input);
        };
        callbacks.on_builtin_tool_done = [&](const std::string& tool_use_id, const std::string& /*name*/,
                                             const nlohmann::json& /*input*/, const std::string& summary,
                                             bool is_error) {
            finish_tool(tool_use_id, nlohmann::json{{"result", summary}, {"isError", is_error}});
        };

        // ---- usage / context 进度事件(阶段 3) ----
        std::vector<api::UsageReport> usage_reports;
        callbacks.on_usage = [&](const api::UsageReport& report) {
            usage_reports.push_back(report);
            // 每次到模型的请求收尾发一枚 turn/usage(前端画 token 账)。
            connection_->EmitEvent(kEventTurnUsage, MakeTurnUsageParams(thread_id, turn_id,
                                                                        UsageToJson(report.usage),
                                                                        report.model));
        };
        loop.SetOnContextPressure([this, &thread_id, &turn_id](
                                      const agent::ContextPressure& pressure) {
            // 上下文压力通报:PreRequest 评估与 hard trim 之后各来一次。
            nlohmann::json context;
            context["phase"] = pressure.phase == agent::ContextPressure::Phase::PreRequest
                                   ? "pre_request"
                                   : "after_hard_trim";
            context["projectedTokens"] = pressure.projected_tokens;
            context["windowTokens"] = pressure.window_tokens;
            context["projectedOverflow"] = pressure.projected_overflow;
            context["hardTrimmedTurns"] = pressure.hard_trimmed_turns;
            context["hardDroppedMessages"] = pressure.hard_dropped_messages;
            context["hardTruncatedResults"] = pressure.hard_truncated_results;
            connection_->EmitEvent(kEventTurnContext,
                                   MakeTurnContextParams(thread_id, turn_id, std::move(context)));
        });

        // ---- 审批接线(阶段 2 核心) ----
        // needs_confirm 的工具:先查会话级放行账(acceptForSession 记过
        // 的免问),没放行就发 permission/request 反向请求,悬停等前端
        // 答复。悬停期间:
        //   - 读线程 HandleInteractionResponse 把答复 resolve 进 promise;
        //   - turn/interrupt 置旗 + CancelPending,future 按 cancel 醒;
        //   - 超时(选项给了时限)按"没人可答"悬空收口,不冒充用户拒绝。
        callbacks.on_tool_confirm_async =
            [this, record, turn_id](const agent::ApprovalRequest& request)
            -> std::shared_ptr<agent::InteractionFuture> {
                // 会话级放行:免问直接放。
                if (record->interactions->IsSessionAllowed(request.tool_name)) {
                    agent::ApprovalResponse accepted;
                    accepted.decision = agent::ApprovalDecision::Accept;
                    return std::make_shared<ReadyApprovalFuture>(std::move(accepted));
                }
                const runtime::ApprovalRequest mirrored = MirrorApprovalRequest(request);
                auto future = std::static_pointer_cast<PendingFuture>(record->interactions->AskApproval(
                    mirrored, turn_id,
                    [this](std::string_view method, const nlohmann::json& params) {
                        // must_keep:审批丢了客户端不知道要答。EmitEvent
                        // 统一盖 seq + 兜溢出通报。
                        connection_->EmitEvent(method, params);
                    }));
                if (options_.approval_timeout_ms > 0) {
                    // 限时悬停:超时按悬空收口,悬停不偷跑。
                    future->SetTimeout(std::chrono::milliseconds(options_.approval_timeout_ms));
                }
                // 打断旗轮询贴在 future 上:CancelPending 收 promise 唤醒
                // Wait;旗先置位而 promise 未收(窗口极窄)由 future 的
                // 轮询兜底。
                future->WatchInterrupt(&record->interrupt_requested);
                return future;
            };
        // 悬空收口的拒绝文案:写明真因,不冒充用户拒绝(P2 工人接线
        // 注意 3)。interrupt 旗置位 = 打断;否则是超时/断线。
        callbacks.on_tool_denial_text = [&record](const std::string& /*tool_use_id*/, const std::string& name) {
            if (record->interrupt_requested.load()) {
                return "回合被 turn/interrupt 打断," + name + " 的审批按取消收口,未执行。";
            }
            return "审批悬停超时(前端未在时限内答复)," + name + " 按取消收口,未执行。";
        };
        // 同步回落路不设:app-server 一律走 async(没有终端可问)。

        // ---- ask_user 接线(user/ask 反向请求,同一套悬起机制) ----
        // 工具表的 ask_user 工具(装配时注入的)经 SetHandler 换成悬起
        // 版:每题发一枚 user/ask,等前端 answers。
        if (tools::Tool* raw_ask = registry->Find("ask_user"); raw_ask != nullptr) {
            if (auto* ask_tool = dynamic_cast<tools::AskUserTool*>(raw_ask); ask_tool != nullptr) {
                ask_tool->SetHandler([this, record, turn_id](const tools::AskUserQuestion& question)
                                         -> std::expected<std::vector<std::string>, std::string> {
                    runtime::QuestionRequest request;
                    request.header = question.header;
                    request.question = question.question;
                    request.multi_select = question.multi_select;
                    for (const tools::AskUserOption& option : question.options) {
                        request.options.push_back(runtime::QuestionOption{option.label, option.description});
                    }
                    auto future = record->interactions->AskQuestion(
                        request, turn_id,
                        [this](std::string_view method, const nlohmann::json& params) {
                            connection_->EmitEvent(method, params);
                        });
                    if (options_.approval_timeout_ms > 0) {
                        future->SetTimeout(std::chrono::milliseconds(options_.approval_timeout_ms));
                    }
                    future->WatchInterrupt(&record->interrupt_requested);
                    const std::optional<runtime::QuestionResponse> answer = future->WaitQuestion();
                    if (!answer.has_value()) {
                        // 悬空收口:打断/超时/断线。真因文案。
                        if (record->interrupt_requested.load()) {
                            return std::unexpected("回合被 turn/interrupt 打断,提问按取消收口");
                        }
                        return std::unexpected("提问悬停超时(前端未在时限内答复),按取消收口");
                    }
                    if (!answer->error.empty()) {
                        return std::unexpected(answer->error);
                    }
                    if (answer->answers.empty()) {
                        return std::unexpected("用户没有选择答案");
                    }
                    return answer->answers;
                });
            }
        }

        // ---- 跑 ----
        // 图片走 Message 入口(与字符串入口同义,图片原样入 history)。
        const std::expected<agent::RunOutcome, std::string> outcome =
            loop.Run(user_message, callbacks, &record->interrupt_requested);
        if (!text_item_id.empty()) {
            connection_->EmitEvent(
                kEventItemCompleted,
                MakeItemCompletedParams(thread_id, turn_id, text_item_id, nlohmann::json::object()));
        }
        if (!thinking_item_id.empty()) {
            connection_->EmitEvent(
                kEventItemCompleted,
                MakeItemCompletedParams(thread_id, turn_id, thinking_item_id, nlohmann::json::object()));
        }
        // 没自然终态的工具条目(打断/异常路径不会再有 on_tool_done):
        // 统一按 cancelled 收口——条目不悬空,前端好对账。
        for (const auto& [tool_use_id, item_id] : open_tools) {
            (void)tool_use_id;
            connection_->EmitEvent(kEventItemCompleted,
                                   MakeItemCompletedParams(thread_id, turn_id, item_id,
                                                           nlohmann::json{{"isError", false},
                                                                          {"cancelled", true}}));
        }
        open_tools.clear();

        // acceptForSession 的放行记账在答复侧(HandleInteractionResponse)
        // 落,这里只收终态。
        std::string status;
        std::string error_message;
        if (!outcome.has_value()) {
            status = std::string(kTurnStatusError);
            error_message = outcome.error();
        } else if (record->interrupt_requested.load() && outcome->cancelled) {
            // 打断收场:interrupted 终态(单子口径:turn/interrupt 发
            // interrupted,ESC 语义的 cancelled 留给内部收线)。
            status = std::string(kTurnStatusInterrupted);
        } else if (outcome->cancelled) {
            status = std::string(kTurnStatusCancelled);
        } else {
            status = std::string(kTurnStatusSuccess);
        }
        const int steps_used = outcome.has_value() ? outcome->steps_used : 0;
        completed_params = MakeTurnCompletedParams(thread_id, turn_id, status, error_message,
                                                   UsageToJson(SumUsage(usage_reports)), steps_used);

        // 存档:助手回合整段落一条(事件流是协议的,存档是会话账的)。
        if (record->store != nullptr && outcome.has_value()) {
            for (const api::Message& message : loop.history()) {
                // 历史里首条 user(本轮输入)已写过;只补 assistant 的尾巴。
                if (message.role == api::Role::Assistant) {
                    record->store->AppendMessage(message);
                }
            }
        }
    }

    // 回合收口:清掉这一轮残留的悬起请求(理论到不了这——审批都是同步
    // Wait 的;防御:中断路径上 future 撤了 promise 没收的,这里兜底)。
    record->interactions->CancelPending();
    connection_->EmitEvent(kEventTurnCompleted, completed_params);
    record->last_completed = completed_params;
    record->turn_finished.store(true);
    record->turn_running.store(false);
}

nlohmann::json Server::HandleTurnInterrupt(const std::string& thread_id, const std::string& turn_id,
                                           std::string& out_error_code) {
    out_error_code.clear();
    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr) {
        out_error_code = "不认识的 threadId: " + thread_id;
        return nlohmann::json();
    }
    if (!record->turn_running.load()) {
        out_error_code = "stale"; // 回合不在跑:迟到打断,不受理
        return nlohmann::json();
    }
    if (!turn_id.empty() && turn_id != record->turn_id) {
        out_error_code = "stale"; // 点名了别的回合:旧账不追
        return nlohmann::json();
    }
    record->interrupt_requested.store(true);
    record->interrupted_turn = record->turn_id;
    // 审批悬停立即醒:pending 全按 cancel 收口(文案由 denial_text 写
    // "被 turn/interrupt 打断")。回合驱动在流式/工具边界看旗收场,终态
    // turn/completed(status=interrupted)照发。
    record->interactions->CancelPending();
    Diagnose("turn/interrupt 受理: " + thread_id + " " + record->turn_id);
    return nlohmann::json{{"threadId", thread_id}, {"turnId", record->turn_id}, {"accepted", true}};
}

// goal/loop/plan 的 typed 命令执行体(goal 单合流批)。方法名 ->
// ClientCommandKind 的映射在这里;参数已由 schema 层查过,这里折
// ClientCommand 交 CommandService(执行体只有一份,终端与远端同吃)。
// 线程模型:读线程跑(与 turn 工作线程不并发碰 goal/loop 状态——turn
// 收口的续跑泵在终端那条线,app-server 的 goal 续跑属于会话泵线,本批
// 只接命令面,泵线另立)。
nlohmann::json Server::HandleTypedDomainCommand(const IncomingRequest& request, bool& out_error,
                                                std::string& out_error_code,
                                                std::string& out_error_message) {
    out_error = false;
    out_error_code.clear();
    out_error_message.clear();
    const std::string method = std::string(request.method);

    // 参数表分家:goal/loop/plan 三组各查各的。
    std::string thread_id;
    std::string text;
    std::string task_id;
    ParamsCheck base = CheckParamsIsObject(request.params, request.method);
    if (!base.ok) {
        out_error = true;
        out_error_code = "invalid_params";
        out_error_message = base.message;
        return nlohmann::json();
    }
    const bool is_goal = method.rfind("goal/", 0) == 0;
    const bool is_loop = method.rfind("loop/", 0) == 0;
    const bool is_plan = method.rfind("plan/", 0) == 0;
    if (is_goal) {
        base = CheckGoalMutationParams(request.params, request.method, thread_id, text);
    } else if (is_loop) {
        base = CheckLoopMutationParams(request.params, request.method, thread_id, task_id, text);
    } else if (is_plan) {
        base = CheckPlanMutationParams(request.params, request.method, thread_id);
    } else {
        out_error = true;
        out_error_code = "invalid_params";
        out_error_message = "不认识的域命令: " + method;
        return nlohmann::json();
    }
    if (!base.ok) {
        out_error = true;
        out_error_code = "invalid_params";
        out_error_message = base.message;
        return nlohmann::json();
    }

    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr) {
        out_error = true;
        out_error_code = "not_found";
        out_error_message = "不认识的 threadId: " + thread_id;
        return nlohmann::json();
    }

    // 协议方法名 -> ClientCommandKind(camelCase 参数 -> payload 字段)。
    runtime::ClientCommand command;
    command.thread_id = thread_id;
    command.text = text;
    command.value = task_id;
    if (method == std::string(kMethodGoalCreate)) {
        command.kind = runtime::ClientCommandKind::CreateGoal;
    } else if (method == std::string(kMethodGoalGet)) {
        command.kind = runtime::ClientCommandKind::GetGoal;
    } else if (method == std::string(kMethodGoalEdit)) {
        command.kind = runtime::ClientCommandKind::EditGoal;
        command.payload["expected_revision"] = request.params.value("expectedRevision", 0);
    } else if (method == std::string(kMethodGoalPause)) {
        command.kind = runtime::ClientCommandKind::PauseGoal;
    } else if (method == std::string(kMethodGoalResume)) {
        command.kind = runtime::ClientCommandKind::ResumeGoal;
        command.payload["expected_revision"] = request.params.value("expectedRevision", 0);
    } else if (method == std::string(kMethodGoalClear)) {
        command.kind = runtime::ClientCommandKind::ClearGoal;
        command.payload["confirm"] = request.params.value("confirm", false);
    } else if (method == std::string(kMethodLoopCreate)) {
        command.kind = runtime::ClientCommandKind::CreateLoopTask;
        command.payload["interval_ms"] = request.params.value("intervalMs", 0);
    } else if (method == std::string(kMethodLoopList)) {
        command.kind = runtime::ClientCommandKind::ListLoopTasks;
    } else if (method == std::string(kMethodLoopRead)) {
        command.kind = runtime::ClientCommandKind::ReadLoopTask;
    } else if (method == std::string(kMethodLoopPause)) {
        command.kind = runtime::ClientCommandKind::PauseLoopTask;
    } else if (method == std::string(kMethodLoopResume)) {
        command.kind = runtime::ClientCommandKind::ResumeLoopTask;
    } else if (method == std::string(kMethodLoopCancel)) {
        command.kind = runtime::ClientCommandKind::CancelLoopTask;
    } else if (method == std::string(kMethodLoopRunNow)) {
        command.kind = runtime::ClientCommandKind::RunLoopTaskNow;
    } else if (method == std::string(kMethodPlanSetMode)) {
        command.kind = runtime::ClientCommandKind::SetCollaborationMode;
        command.value = request.params.value("mode", std::string());
        command.payload["reason"] = request.params.value("reason", std::string("remote"));
        command.payload["permission_before_plan"] =
            request.params.value("permissionBeforePlan", std::string());
    } else if (method == std::string(kMethodPlanReview)) {
        command.kind = runtime::ClientCommandKind::ReviewPlan;
        command.payload["plan_id"] = request.params.value("planId", std::string());
        command.payload["plan_revision"] = request.params.value("planRevision", 0);
        command.payload["sha256"] = request.params.value("sha256", std::string());
        command.payload["decision"] = request.params.value("decision", std::string());
    } else if (method == std::string(kMethodPlanReopen)) {
        command.kind = runtime::ClientCommandKind::ReopenPlanReview;
    } else {
        out_error = true;
        out_error_code = "invalid_params";
        out_error_message = "不认识的域命令: " + method;
        return nlohmann::json();
    }

    // 执行:goal/loop/plan 各交各的状态机(实例按 thread 起,读线程单碰)。
    static runtime::CommandService kDomainService(runtime::CommandService::Options{});
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    runtime::ClientReceipt receipt;
    if (is_goal) {
        receipt = kDomainService.HandleGoalCommand(command, record->goal_coordinator.get(),
                                                   record->cwd, now_ms);
    } else if (is_loop) {
        receipt = kDomainService.HandleLoopCommand(command, record->loop_scheduler.get(), record->cwd,
                                                   record->thread_id, now_ms);
    } else {
        receipt = kDomainService.HandlePlanCommand(command, record->session_runtime.get());
    }
    if (!receipt.accepted) {
        out_error = true;
        out_error_code = receipt.error_code;
        out_error_message = receipt.error_message;
        return nlohmann::json();
    }
    return receipt.payload;
}

InteractionResolution Server::HandleInteractionResponse(const IncomingResponse& response) {    // requestId -> 哪场 thread:先扫各 thread 的悬起件(骨架期 thread 数
    // 少,线性可忍;TODO(seq/runtime P4)统一 id 后按 id 前缀直路由)。
    std::vector<std::shared_ptr<ThreadRecord>> records;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (const auto& [id, record] : threads_) {
            records.push_back(record);
        }
    }
    InteractionResolution result;
    result.error_code = runtime::kStaleRequestId;
    result.error_message = "没有在飞的请求配得上这条响应";
    for (const std::shared_ptr<ThreadRecord>& record : records) {
        if (record->interactions->pending_count() == 0) {
            continue;
        }
        // 只有一场 thread 认领这条响应(其它场报 stale);认领成功即收。
        const InteractionResolution one = record->interactions->HandleIncomingResponse(response);
        if (one.ok) {
            return one;
        }
    }
    return result;
}

int Server::Run() {
    connection_ = std::make_unique<StdioConnection>(
        dispatcher_,
        [](const std::string& line) {
            if (!WriteProtocolLine(line)) {
                Diagnose("stdout 写失败(断管),收线");
            }
        },
        []() { return ReadStdinChunk(); }, options_.outbox_capacity);
    connection_->SetInteractionResolver([this](const IncomingResponse& response) -> std::string {
        const InteractionResolution result = HandleInteractionResponse(response);
        if (result.ok) {
            return std::string();
        }
        return result.error_code.empty() ? std::string(runtime::kStaleRequestId) : result.error_code;
    });
    const int code = connection_->Run();
    Shutdown();
    return code;
}

void Server::Shutdown() {
    // 在跑的回合一律按打断收口:置旗、悬起全清(审批悬停立即醒),等
    // 收尾(硬时限内),等不到就分离——绝不许 join 卡死把退场路堵死。
    std::vector<std::shared_ptr<ThreadRecord>> records;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (const auto& [id, record] : threads_) {
            records.push_back(record);
        }
    }
    for (const std::shared_ptr<ThreadRecord>& record : records) {
        if (record->turn_worker.joinable()) {
            record->interrupt_requested.store(true);
            record->interactions->CancelPending();
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(options_.interrupt_hard_deadline_ms);
            while (!record->turn_finished.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (record->turn_finished.load()) {
                record->turn_worker.join();
            } else {
                Diagnose("收线时回合未在硬时限内收口,分离工作线程: " + record->thread_id);
                record->turn_worker.detach();
            }
        }
        if (record->store != nullptr) {
            record->store->Reset();
        }
    }
}

// 单测直驱用的连接装配:HandleTurnStart 要经 connection_ 发事件,测试里
// 先用假读写器把连接立起来,再直调 handler。resolver 一并接上,整线
// 直驱(Feed 响应信封)与生产同路。
void Server::AttachForTest(std::unique_ptr<StdioConnection> connection) {
    connection->SetInteractionResolver([this](const IncomingResponse& response) -> std::string {
        const InteractionResolution result = HandleInteractionResponse(response);
        if (result.ok) {
            return std::string();
        }
        return result.error_code.empty() ? std::string(runtime::kStaleRequestId) : result.error_code;
    });
    connection_ = std::move(connection);
}

}  // namespace lubancode::app_server
