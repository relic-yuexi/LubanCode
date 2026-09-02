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

#include "agent/agent.hpp"  // Agent/AgentProfile/AgentWiring(批四自立门户)
#include "agent/context.hpp"  // ContextPressure:压力通报的形状
#include "config/config.hpp"  // HomeLubancodeDir:P0-1 身份裁决的全局件止步
#include "runtime/id_authority.hpp"
#include "runtime/session_command_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_session.hpp"  // P0-2:app-server 同一口接 Trajectory
#include "trajectory/session_index.hpp"    // P0-2:trace/query 冷回放的索引定位
#include "runtime/turn_event_adapter.hpp"
#include "runtime/turn_item.hpp"
#include "tools/ask_user.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "workspace/identity.hpp"  // P0-1:thread 面 workspace 身份裁决
#include "workflow/frontend.hpp"
#include "workflow/journal.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:入站用户文本的编码关口

namespace lubancode::app_server {

namespace {

// 病十(骨架拆解批三):app-server 回合的步数硬闸,从前散在装配处的魔数
// 32,现在立名进皮(AgentProfile.runtime.max_steps_per_turn 的取值源)。
// 协议前端没有 ESC 可打断,不设闸会真跑飞;真配置线接进来再换。
constexpr int kAppServerMaxStepsPerTurn = 32;

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

// ServerEvent -> app-server 协议事件的桥(骨架拆解批二:回合驱动的显示
// 回调整装切到 TurnEventAdapter,这只 sink 负责把 ServerEvent 翻成冻结的
// item/started、item/delta、item/completed、turn/usage 形状)。事件本体
// 不改写——id 从事件里原样取,次序照投递序,载荷按下表的口径对译:
//   ItemStarted   -> item/started(item.type 按 ItemKind,工具名 run_command
//                    折 command;diff 行表照旧在 started 时算)
//   ItemDelta     -> item/delta
//   ItemCompleted -> item/completed(正文/思考不带载荷;Cancelled 的工具
//                    带 isError=false+cancelled=true;其余带 result+isError)
//   UsageUpdated  -> turn/usage(五项 camelCase + model;顺带累进整轮
//                    usage 汇总,turn/completed 的账与事件同源)
// TurnStarted/TurnCompleted 不在桥上发——回合驱动自己按终态分型发,带
// status/usage 汇总/steps,口径比事件层全。
class ProtocolBridgeSink final : public runtime::EventSink {
public:
    using Emitter = std::function<void(std::string_view, const nlohmann::json&)>;

    ProtocolBridgeSink(Emitter emit, std::vector<api::UsageReport>& usage_out)
        : emit_(std::move(emit)), usage_out_(&usage_out) {}

    void Emit(const runtime::ServerEvent& event) override {
        switch (event.kind) {
            case runtime::ServerEventKind::ItemStarted: {
                const std::string tool_name = event.payload.value("tool_name", std::string());
                std::string_view item_type = kItemTypeTool;
                switch (event.item_kind) {
                    case runtime::ItemKind::Text:
                        item_type = kItemTypeText;
                        break;
                    case runtime::ItemKind::Thinking:
                        item_type = kItemTypeThinking;
                        break;
                    default:
                        item_type = tool_name == "run_command" ? kItemTypeCommand : kItemTypeTool;
                        break;
                }
                nlohmann::json payload = nlohmann::json::object();
                if (!tool_name.empty()) {
                    payload["tool"] = tool_name;
                    const std::string tool_use_id = event.payload.value("tool_use_id", std::string());
                    if (!tool_use_id.empty()) {
                        payload["toolUseId"] = tool_use_id;
                    }
                    const nlohmann::json input =
                        event.payload.contains("input") ? event.payload["input"] : nlohmann::json::object();
                    if (input.is_object() && !input.empty()) {
                        payload["input"] = input;
                    }
                    if (const std::optional<runtime::DiffTable> table =
                            runtime::BuildDiffTable(tool_name, input)) {
                        payload["diff"] = DiffTableToJson(*table);
                    }
                }
                emit_(kEventItemStarted,
                      MakeItemStartedParams(event.envelope.thread_id, event.turn_id, event.item_id, item_type,
                                            std::move(payload)));
                break;
            }
            case runtime::ServerEventKind::ItemDelta:
                emit_(kEventItemDelta,
                      MakeItemDeltaParams(event.envelope.thread_id, event.turn_id, event.item_id, event.text));
                break;
            case runtime::ServerEventKind::ItemCompleted: {
                nlohmann::json payload = nlohmann::json::object();
                if (event.item_kind == runtime::ItemKind::Text ||
                    event.item_kind == runtime::ItemKind::Thinking) {
                    // 正文/思考条目的收尾不带载荷(旧口径)。
                } else if (event.outcome == runtime::Outcome::Cancelled) {
                    payload["isError"] = false;
                    payload["cancelled"] = true;
                } else {
                    payload["result"] = event.payload.value("result", std::string());
                    payload["isError"] = event.payload.value("is_error", false);
                    // 截图等富图片的 artifact 引用(可见调试单阶段 2):只递
                    // 元数据与 ArtifactRef,不带 base64——正文在会话 artifact
                    // 落盘处,前端凭 path/id 取。模型看的图与前端点开的图
                    // 指向同一 artifact,不许各说各话。
                    if (event.payload.contains("images") && event.payload["images"].is_array() &&
                        !event.payload["images"].empty()) {
                        payload["images"] = event.payload["images"];
                    }
                }
                emit_(kEventItemCompleted,
                      MakeItemCompletedParams(event.envelope.thread_id, event.turn_id, event.item_id,
                                              std::move(payload)));
                break;
            }
            case runtime::ServerEventKind::UsageUpdated: {
                api::Usage usage;
                usage.input_tokens = event.payload.value("input_tokens", std::int64_t{0});
                usage.output_tokens = event.payload.value("output_tokens", std::int64_t{0});
                usage.cache_read_tokens = event.payload.value("cache_read_tokens", std::int64_t{0});
                usage.cache_creation_tokens = event.payload.value("cache_creation_tokens", std::int64_t{0});
                usage.output_reasoning_tokens = event.payload.value("reasoning_tokens", std::int64_t{0});
                const std::string model = event.payload.value("model", std::string());
                if (usage_out_ != nullptr) {
                    api::UsageReport report;
                    report.usage = usage;
                    report.model = model;
                    usage_out_->push_back(std::move(report));
                }
                emit_(kEventTurnUsage, MakeTurnUsageParams(event.envelope.thread_id, event.turn_id,
                                                           UsageToJson(usage), model));
                break;
            }
            default:
                break;  // thread/turn 层事件归回合驱动,不在桥上翻
        }
    }

private:
    Emitter emit_;
    std::vector<api::UsageReport>* usage_out_;
};

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
      registry_factory_(std::move(registry_factory)),
      workspaces_dir_(options_.workspaces_dir) {
    dispatcher_ = std::make_shared<Dispatcher>();
    dispatcher_->SetInitializeResultFactory(
        [this]() { return MakeInitializeResult(options_.lubancode_version, PlatformId()); });
    // P9 收尾 + P0-2 换账:会话查询/搬删的执行体吃 workspace 新账
    //(索引投影 + 管理自由函数)。workspaces_dir 空(测试纯内存跑)= 没建,
    // list 给空表。scope=cwd 的默认 workspace 按服务 cwd 裁决。
    if (!workspaces_dir_.empty()) {
        runtime::SessionCommandService::Options service_options;
        service_options.workspaces_root = tools::Utf8ToPath(workspaces_dir_);
        service_options.workspace_key = DefaultWorkspaceKey();
        session_commands_ = std::make_unique<runtime::SessionCommandService>(service_options);
    }
    // 浏览器面(阶段 3):事件出水走连接层的统一出口(seq + 有界 + 溢出
    // 通报);审批口挂 thread 的悬起件(取消旗贯通到动作)。
    {
        BrowserServiceOptions browser_options;
        browser_options.sidecar_command = options_.browser_sidecar_command;
        browser_options.sidecar_args = options_.browser_sidecar_args;
        browser_options.artifact_dir = options_.browser_artifact_dir;
        if (options_.browser_screencast_queue_capacity > 0) {
            browser_options.screencast_queue_capacity = options_.browser_screencast_queue_capacity;
        }
        browser_ = std::make_unique<BrowserService>(
            std::move(browser_options),
            [this](std::string_view method, const nlohmann::json& params) {
                EmitEventSafe(method, params);
            });
        browser_->SetApprovalAsk(
            [this](const std::string& thread_id, const runtime::ApprovalRequest& request,
                   const std::atomic<bool>* cancel) -> std::optional<BrowserService::ApprovalTicket> {
                return HandleBrowserApproval(thread_id, request, cancel);
            });
    }
    RegisterMethods(*dispatcher_);
}

// 服务进程默认 workspace 的 key:按 options_.cwd 四级裁决(P0-1 同一颗
// resolver);裁决不出给空(scope=cwd 的 list 就按空 key 拒,如实不冒充)。
std::string Server::DefaultWorkspaceKey() const {
    if (workspaces_dir_.empty()) {
        return std::string();
    }
    const std::filesystem::path identity_cwd = tools::Utf8ToPath(options_.cwd);
    const auto identity_home = lubancode::config::HomeLubancodeDir();
    auto identity = lubancode::workspace::ResolveWorkspaceIdentity(
        identity_cwd, identity_home.has_value() ? lubancode::tools::Utf8ToPath(*identity_home)
                                                : std::filesystem::path());
    if (!identity.has_value()) {
        return std::string();
    }
    return identity->workspace_key;
}

// browser 动作的审批:与工具审批同一套悬起件(permission/request 反向
// 请求 + 会话级放行账 + 打断/取消/超时的悬空收口)。turn_id 空——浏览器
// 动作不在回合里,悬起件按 thread 配对。
std::optional<BrowserService::ApprovalTicket> Server::HandleBrowserApproval(
    const std::string& thread_id, const runtime::ApprovalRequest& request, const std::atomic<bool>* cancel) {
    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr) {
        return std::nullopt;
    }
    if (record->interactions->IsSessionAllowed(request.tool_name)) {
        BrowserService::ApprovalTicket ticket;
        runtime::ApprovalResponse accepted;
        accepted.decision = runtime::InteractionDecision::Accept;
        ticket.future = std::make_shared<ReadyApprovalFuture>(std::move(accepted));
        return ticket;
    }
    auto future = std::static_pointer_cast<PendingFuture>(record->interactions->AskApproval(
        request, std::string(),
        [this](std::string_view method, const nlohmann::json& params) {
            // must_keep:审批丢了客户端不知道要答。EmitEvent 统一盖 seq
            // + 兜溢出通报。
            EmitEventSafe(method, params);
        }));
    if (options_.approval_timeout_ms > 0) {
        future->SetTimeout(std::chrono::milliseconds(options_.approval_timeout_ms));
    }
    // 动作自己的取消旗挂进打断路:browser/action/cancel、thread/stop、
    // turn/interrupt(CancelPending)都能把悬着的审批叫醒。
    if (cancel != nullptr) {
        future->WatchInterrupt(cancel);
    }
    BrowserService::ApprovalTicket ticket;
    ticket.future = future;
    // 悬空收口的擦账口:把这枚悬起件从 thread 的 pending 表里摘掉,
    // 迟到的答复按 stale 报(审批人没答的不许吞)。
    ticket.cancel = [record, tool_use_id = request.tool_use_id] {
        record->interactions->CancelPendingForToolUse(tool_use_id);
    };
    return ticket;
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

void Server::RegisterMethods(Dispatcher& dispatcher) {
    // thread/start
    dispatcher.RegisterMethod(
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
    dispatcher.RegisterMethod(
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
    dispatcher.RegisterMethod(kMethodThreadArchive, lifecycle_handler(kMethodThreadArchive));
    dispatcher.RegisterMethod(kMethodThreadUnarchive, lifecycle_handler(kMethodThreadUnarchive));
    dispatcher.RegisterMethod(kMethodThreadDelete, lifecycle_handler(kMethodThreadDelete));

    // trace/query(逐枚追踪单第 5 期):断线补账与冷回放。事件从 session
    // 存档的 tool_trace_v1 行折叠——线程重启、app-server 重启后都有账可
    // 查(单子:"app-server 断线按 seq 补事件,必要时从 session trace
    // 冷回放")。lastSeq 是"客户端已见到的最大 seq",返回大于它的;缺省
    // 0 = 全量。脱敏是默认:正文只回 preview 摘要,不回 inline 原文
    // (单子"/trace 默认遮敏;--raw 须本机交互确认"——远端通道没有本机
    // 交互,一律走遮敏档)。
    dispatcher.RegisterMethod(
        kMethodTraceQuery, [this](const IncomingRequest& request, DispatchContext&)
                             -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::uint64_t last_seq = 0;
            const ParamsCheck base = CheckTraceQueryParams(request.params, thread_id, last_seq);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            // P0-2:会话账在 workspace Journal——活 thread 从账本拿
            // main.jsonl;冷 thread 经索引跨 workspace 定位。折叠直接吃
            // tool.* 事件(唯一真账),不再读旧 tool_trace_v1 行。
            std::filesystem::path session_dir;
            {
                std::lock_guard<std::mutex> lock(threads_mutex_);
                const auto it = threads_.find(thread_id);
                if (it != threads_.end() && it->second->session_runtime != nullptr &&
                    it->second->session_runtime->trajectory() != nullptr) {
                    session_dir = it->second->session_runtime->trajectory()->session_dir();
                }
            }
            if (session_dir.empty() && !workspaces_dir_.empty()) {
                // 冷 thread:索引跨 workspace 定位(thread 的 cwd 各归各的
                // workspace,不能只按服务默认 key 找)。
                trajectory::SessionIndexQuery index_query;
                index_query.all_workspaces = true;
                const auto page = trajectory::QueryWorkspaceSessions(tools::Utf8ToPath(workspaces_dir_),
                                                                    index_query);
                for (const auto& summary : page.entries) {
                    if (summary.session_id == thread_id) {
                        session_dir = tools::Utf8ToPath(summary.session_dir);
                        break;
                    }
                }
            }
            if (session_dir.empty()) {
                return MakeError(request.id, kErrInvalidParams,
                                 "trace/query: 没有会话账(纯内存 thread 或未配置 workspaces 根)");
            }
            const auto folded = trajectory::FoldSessionToolExecutions(session_dir);

            // 可选过滤。
            const std::string filter_execution =
                request.params.value("executionId", std::string());
            const std::string filter_tool_use = request.params.value("toolUseId", std::string());
            const std::string filter_turn = request.params.value("turnId", std::string());
            const bool errors_only =
                request.params.value("errorsOnly", false);

            nlohmann::json executions = nlohmann::json::array();
            std::uint64_t max_seq = last_seq;
            for (const nlohmann::json& item : folded.executions) {
                // seq 过滤:lastSeq 之后的才回(断线补账的口径)。行的
                // seqScheduled 是该调用首枚事件(planned)的 seq,与旧
                // tool_trace_v1 的 seq_scheduled 同语义。
                if (item.value("seqScheduled", std::uint64_t{0}) <= last_seq) {
                    continue;
                }
                if (!filter_execution.empty() &&
                    item.value("executionId", std::string()) != filter_execution) {
                    continue;
                }
                if (!filter_tool_use.empty() && item.value("toolUseId", std::string()) != filter_tool_use) {
                    continue;
                }
                if (!filter_turn.empty() && item.value("turnId", std::string()) != filter_turn) {
                    continue;
                }
                if (errors_only && item.value("outcome", std::string()) == "succeeded") {
                    continue;
                }
                executions.push_back(item);
            }
            max_seq = std::max(max_seq, folded.max_seq);

            return MakeResult(request.id,
                              nlohmann::json{{"threadId", thread_id},
                                             {"lastSeq", max_seq},
                                             {"count", executions.size()},
                                             {"executions", std::move(executions)}});
        });

    // workflow/query(wf 线的事件出口:LoadSnapshotFromDisk +
    // BuildIncrementalEvents,server 只折协议形状)。
    dispatcher.RegisterMethod(
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
    dispatcher.RegisterMethod(
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
    dispatcher.RegisterMethod(
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
    dispatcher.RegisterMethod(
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
    dispatcher.RegisterMethod(kMethodGoalCreate, domain_handler);
    dispatcher.RegisterMethod(kMethodGoalGet, domain_handler);
    dispatcher.RegisterMethod(kMethodGoalEdit, domain_handler);
    dispatcher.RegisterMethod(kMethodGoalPause, domain_handler);
    dispatcher.RegisterMethod(kMethodGoalResume, domain_handler);
    dispatcher.RegisterMethod(kMethodGoalClear, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopCreate, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopList, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopRead, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopPause, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopResume, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopCancel, domain_handler);
    dispatcher.RegisterMethod(kMethodLoopRunNow, domain_handler);
    dispatcher.RegisterMethod(kMethodPlanSetMode, domain_handler);
    dispatcher.RegisterMethod(kMethodPlanReview, domain_handler);
    dispatcher.RegisterMethod(kMethodPlanReopen, domain_handler);

    // browser 面(阶段 3):方法表、事件转发、审批与取消都在
    // BrowserService 里,这里只递 dispatcher 与 thread 查找口。
    browser_->RegisterMethods(dispatcher, [this](const std::string& thread_id) {
        return FindThread(thread_id);
    });
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

    // P0-2(Trajectory 升为唯一 Session):thread 的会话账走
    // SessionRuntime 的 TrajectorySessionLedger——thread_id 直接用 workspace
    // session id(与 CLI 同一命名空间,迁移器原样带入 legacy_import 场)。
    // 旧 SessionStore 不再开档(禁 dual-write)。
    //
    // goal 单合流批:typed 命令面的会话级状态按 thread 各起一本(goal/
    // loop 关着也建实例——命令面回稳定禁用码)。
    record->goal_coordinator = [this] {
        runtime::goal::GoalCoordinator::Options goal_options;
        goal_options.goals_enabled = options_.features_goal;
        return std::make_unique<runtime::goal::GoalCoordinator>(goal_options);
    }();
    {
        runtime::loop::LoopScheduler::Options loop_options;
        loop_options.enabled = options_.features_loop;
        record->loop_scheduler = std::make_unique<runtime::loop::LoopScheduler>(loop_options);
    }
    {
        runtime::SessionRuntime::Options runtime_options;
        // P0-1:thread 身份按前端指定的 cwd(record->cwd;空则 options_.cwd)
        // 四级裁决,不按 server 进程的 current_path——前端外壳在各项目里
        // 起 thread,server 进程 cwd 跟项目无关。home 递进去做全局件止步。
        const std::filesystem::path identity_cwd = lubancode::tools::Utf8ToPath(record->cwd);
        const auto identity_home = lubancode::config::HomeLubancodeDir();
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(
            identity_cwd, identity_home.has_value()
                             ? lubancode::tools::Utf8ToPath(*identity_home)
                             : std::filesystem::path());
        if (identity.has_value()) {
            runtime_options.trajectory_workspace_identity = std::move(*identity);
        }
        runtime_options.lubancode_version = options_.lubancode_version;
        if (!workspaces_dir_.empty()) {
            runtime_options.trajectory_workspaces_root = tools::Utf8ToPath(workspaces_dir_);
        }
        record->session_runtime = std::make_unique<runtime::SessionRuntime>(std::move(runtime_options));
    }
    const runtime::TrajectorySessionLedger* ledger = record->session_runtime->trajectory();
    if (ledger == nullptr) {
        // 开不出账 thread 明败,不回退旧写口(§十七失败合同)。
        Diagnose("会话账开张失败,thread 不开: " + record->session_runtime->trajectory_open_error());
        out_error_code = "trajectory.open_failed";
        return nlohmann::json();
    }
    record->thread_id = ledger->session_id();
    record->session_main_path = (ledger->session_dir() / "main.jsonl").generic_string();
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        // ledger 的 session_id 自带随机尾,理论不撞;真撞了(同秒同尾)按
        // 旧行为追加序号,审批/放行账不丢。
        if (threads_.count(record->thread_id) > 0) {
            std::string candidate = record->thread_id;
            const std::string base = candidate;
            for (int n = 2; threads_.count(candidate) > 0; ++n) {
                candidate = base + "-" + std::to_string(n);
            }
            record->thread_id = candidate;
        }
        threads_[record->thread_id] = record;
    }
    record->interactions = std::make_unique<InteractionLedger>(record->thread_id);
    Diagnose("thread 已建: " + record->thread_id);
    return nlohmann::json{{"threadId", record->thread_id}, {"cwd", record->cwd}};
}

nlohmann::json Server::HandleThreadList(const nlohmann::json& params) {
    // P9 收尾:列举走 SessionCommandService(会话管理器单第六步)——与
    // 终端 picker 共吃一碗饭,同一查询给同一份 id/顺序/状态,server 不
    // 另写第二条扫盘路。
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
        out_error_message = "服务没有会话账根,搬删一律不可用";
        return nlohmann::json();
    }
    // 在跑的 thread 不许搬删(账本还持着独占锁,running 场也不进状态图
    // 的归档边;协议侧明拒,不留给 IO 层炸)。
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
    // thread 名下在飞的浏览器动作先取消(审批悬着的也叫醒)。
    if (browser_ != nullptr) {
        browser_->CancelActionsForThread(thread_id, "thread/stop");
    }
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
    // P0-2:thread 停场即 session 封口(session.ended + session.json closed;
    // 收不回的执行记 unknown,不冒充 clean)。封不了只记账,不拦停场——
    // 半开的场由恢复器按 Journal 事实收口。
    if (record->session_runtime != nullptr && record->session_runtime->trajectory() != nullptr) {
        const auto closed = record->session_runtime->trajectory()->CloseSession("thread_stop");
        if (!closed.error_code.empty()) {
            Diagnose("thread 停场时会话封口失败(" + closed.error_code + "): " + thread_id);
        }
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
    EmitEventSafe(kEventTurnStarted, MakeTurnStartedParams(thread_id, turn_id));

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

        // 病十(骨架拆解批三):差别全部进皮。app-server 这张皮从前走兼容
        // 门旁参 + 字面量,现在正门构造,两处差别显式写在皮上:
        //   max_steps——骨架期防跑飞的硬闸(协议前端没有 ESC 可打断,不设
        //   闸会真跑飞;真配置线接进来再换);
        //   system_prompt——协议服务器的最小人格(不吃终端人格/法文件:
        //   这里没有交互提示词栈,装了反而是没想清的差别)。
        agent::AgentProfile profile;
        profile.runtime.max_steps_per_turn = kAppServerMaxStepsPerTurn;
        profile.system_prompt = "lubancode app-server";
        agent::Agent loop(*backend, *registry, std::move(profile));

        // ---- 事件流(骨架拆解批二:整装切到 TurnEventAdapter) ----
        // 旧路在本地手拼 text/thinking 懒起条、open_tools 对账、收口补账,
        // 与适配器各养一份;现在显示回调全从适配器出,协议形状由
        // ProtocolBridgeSink 对译(id 照旧 ProcessIdAuthority 发,turn_id 用
        // AcceptTurnStart 发的那枚,行为与旧路逐事件对得上)。
        std::vector<api::UsageReport> usage_reports;
        ProtocolBridgeSink bridge(
            [this](std::string_view method, const nlohmann::json& params) {
                EmitEventSafe(method, params);
            },
            usage_reports);
        runtime::TurnEventAdapter turn_events(thread_id, runtime::ProcessIdAuthority());
        turn_events.Attach([&bridge](const runtime::ServerEvent& event) { bridge.Emit(event); });
        turn_events.Start(turn_id);
        // 批二余款:显示出水直连适配器(唯一出水口);控制口(审批)收在
        // TurnWiring,协议形状与旧手拼回调逐事件对得上。
        agent::TurnWiring wiring;
        wiring.events = &turn_events;
        // 接线(批四·病十二):压力钩进 AgentWiring。
        agent::AgentWiring loop_wiring;
        loop_wiring.on_context_pressure = [this, &thread_id, &turn_id](
                                              const agent::ContextPressure& pressure) {
            // 上下文压力通报三相明确投影；最终预检不得冒充 hard trim。
            nlohmann::json context;
            if (pressure.phase == agent::ContextPressure::Phase::PreRequest) {
                context["phase"] = "pre_request";
            } else if (pressure.phase == agent::ContextPressure::Phase::AfterHardTrim) {
                context["phase"] = "after_hard_trim";
            } else {
                context["phase"] = "preflight_exceeded";
            }
            context["projectedTokens"] = pressure.projected_tokens;
            context["windowTokens"] = pressure.window_tokens;
            context["projectedOverflow"] = pressure.projected_overflow;
            context["hardTrimmedTurns"] = pressure.hard_trimmed_turns;
            context["hardDroppedMessages"] = pressure.hard_dropped_messages;
            context["hardTruncatedResults"] = pressure.hard_truncated_results;
            if (pressure.phase == agent::ContextPressure::Phase::PreflightExceeded) {
                context["estimatedInputTokens"] = pressure.estimated_input_tokens;
                context["reservedOutputTokens"] = pressure.reserved_output_tokens;
                context["protocolHeadroomTokens"] = pressure.protocol_headroom_tokens;
                context["reserveClamped"] = pressure.reserve_clamped;
            }
            EmitEventSafe(kEventTurnContext,
                                   MakeTurnContextParams(thread_id, turn_id, std::move(context)));
        };
        loop.SetWiring(std::move(loop_wiring));

        // ---- 审批接线(阶段 2 核心) ----
        // needs_confirm 的工具:先查会话级放行账(acceptForSession 记过
        // 的免问),没放行就发 permission/request 反向请求,悬停等前端
        // 答复。悬停期间:
        //   - 读线程 HandleInteractionResponse 把答复 resolve 进 promise;
        //   - turn/interrupt 置旗 + CancelPending,future 按 cancel 醒;
        //   - 超时(选项给了时限)按"没人可答"悬空收口,不冒充用户拒绝。
        wiring.on_permission_evaluate =
            [this, record](const std::string&, const std::string& name, const nlohmann::json& input,
                           const runtime::ToolHookDecision& pre) {
                runtime::PermissionContext context;
                context.mode = options_.permission_mode;
                std::set<std::string> session_allowed;
                if (record->interactions->IsSessionAllowed(name)) {
                    session_allowed.insert(name);
                }
                context.always_allowed = &session_allowed;
                return runtime::EvaluatePermission(context, pre, name, input);
            };
        wiring.on_tool_confirm_async =
            [this, record, turn_id](const runtime::ApprovalRequest& request)
            -> std::shared_ptr<runtime::InteractionFuture> {
                // 会话级放行:免问直接放。
                if (record->interactions->IsSessionAllowed(request.tool_name)) {
                    runtime::ApprovalResponse accepted;
                    accepted.decision = runtime::InteractionDecision::Accept;
                    return std::make_shared<ReadyApprovalFuture>(std::move(accepted));
                }
                auto future = std::static_pointer_cast<PendingFuture>(record->interactions->AskApproval(
                    request, turn_id,
                    [this](std::string_view method, const nlohmann::json& params) {
                        // must_keep:审批丢了客户端不知道要答。EmitEvent
                        // 统一盖 seq + 兜溢出通报。
                        EmitEventSafe(method, params);
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
        wiring.on_tool_denial_text = [&record](const std::string& /*tool_use_id*/, const std::string& name) {
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
                                         -> std::expected<tools::AskUserResponse, std::string> {
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
                            EmitEventSafe(method, params);
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
                    return tools::AskUserResponse::Answered(answer->answers);
                });
            }
        }

        // ---- 跑 ----
        // P0-2 轨迹:flag 开的 thread 接同一口——hub(工具栅栏 + 落盘关口)
        // 与轮次边界桥都挂上,与终端 RunTurn 同一形状(§15.5 app-server
        // 走同一 TrajectorySink)。flag 关不建 hub,app-server 行为零变。
        std::optional<runtime::ToolTraceHub> trajectory_hub;
        std::unique_ptr<runtime::TrajectoryTurnBridge> trajectory_bridge;
        runtime::TrajectorySessionLedger* trajectory_ledger =
            record->session_runtime != nullptr ? record->session_runtime->trajectory() : nullptr;
        if (trajectory_ledger != nullptr) {
            runtime::TrajectoryTurnBridge::Identity identity{std::string(), options_.session_wire,
                                                             "app_server"};
            trajectory_bridge = trajectory_ledger->NewTurnBridge(std::move(identity));
            if (trajectory_bridge != nullptr) {
                trajectory_hub.emplace(record->session_runtime->ids());
                trajectory_hub->Install(loop, wiring, thread_id, turn_id);
                trajectory_hub->AttachTrajectory(trajectory_bridge.get());
                wiring.boundary_recorder = trajectory_bridge.get();
                trajectory_bridge->BeginTurn(turn_id, "external_user");
                trajectory_bridge->RecordInput(user_message);
            }
        }
        // 图片走 Message 入口(与字符串入口同义,图片原样入 history)。
        const std::expected<agent::RunOutcome, std::string> outcome =
            loop.Run(user_message, wiring, &record->interrupt_requested);
        if (trajectory_bridge != nullptr) {
            trajectory_bridge->EndTurn(outcome.has_value(), outcome.has_value() && outcome->cancelled,
                                       outcome.has_value() ? std::string() : outcome.error());
            if (trajectory_hub.has_value()) {
                trajectory_hub->DetachTrajectory();
            }
        }
        // 事件流收口:没收尾的条目(正文/思考/没终态的工具)由适配器统一
        // 按 Cancelled 补账——条目不悬空,前端好对账;终态分型随本地账。
        turn_events.Finish(!outcome.has_value() ? runtime::Outcome::Failed
                          : outcome->cancelled  ? runtime::Outcome::Cancelled
                                                : runtime::Outcome::Succeeded,
                           !outcome.has_value() ? outcome.error() : std::string());

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

    }

    // 回合收口:清掉这一轮残留的悬起请求(理论到不了这——审批都是同步
    // Wait 的;防御:中断路径上 future 撤了 promise 没收的,这里兜底)。
    record->interactions->CancelPending();
    EmitEventSafe(kEventTurnCompleted, completed_params);
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
    // P0-2 轨迹:TrajectoryCommandExecutor 包住 app-server 命令入口
    //(§15.7)——flag 开的 thread 落 command lifecycle。
    runtime::TrajectorySessionLedger* trajectory_ledger =
        record->session_runtime != nullptr ? record->session_runtime->trajectory() : nullptr;
    const std::string command_trajectory_id =
        trajectory_ledger != nullptr ? trajectory_ledger->BeginCommand(method, method, "session_state")
                                     : std::string();
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
    if (trajectory_ledger != nullptr) {
        trajectory_ledger->EndCommand(command_trajectory_id, receipt.accepted,
                                      receipt.accepted ? std::string() : receipt.error_code);
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

std::function<std::string(const IncomingResponse&)> Server::MakeInteractionResolver() {
    return [this](const IncomingResponse& response) -> std::string {
        const InteractionResolution result = HandleInteractionResponse(response);
        if (result.ok) {
            return std::string();
        }
        return result.error_code.empty() ? std::string(runtime::kStaleRequestId) : result.error_code;
    };
}

int Server::Run() {
    // 两承载按需起一种(多前端外壳单阶段 A):配了 ws 走监听循环;stdio
    // 的"EOF 即进程收线"与 WS 的"断线只收连接、进程等重连"语义不同,
    // 并跑两头都拧巴,不并。
    if (options_.ws.has_value()) {
        return RunWsLoop();
    }
    connection_ = std::make_shared<StdioConnection>(
        dispatcher_,
        [](const std::string& line) {
            if (!WriteProtocolLine(line)) {
                Diagnose("stdout 写失败(断管),收线");
            }
        },
        []() { return ReadStdinChunk(); }, options_.outbox_capacity);
    // 身份章(阶段 B):stdio 宿主是拉起本进程的操作者本人——principal
    // 裁定 "user"(browser 面 owner 仲裁的内核真值,外壳报什么不算数)。
    connection_->SetPrincipal("user");
    connection_->SetInteractionResolver(MakeInteractionResolver());
    const int code = connection_->Run();
    Shutdown();
    return code;
}

// WS 承载主循环(阶段 D 起):专职 accept 线程一条一条接连接——升级的
// Session 进队等伺候(会话服务仍串行,阶段 A 语义不变),artifact GET 在
// accept 线程上就地应答。改动的由头:参考前端开着 WS 会话的同时要经
// HTTP 取截图/镜像帧的字节,accept 被在服务的会话堵死就两头死锁(写
// 参考前端时暴露的缝,单子 §五)。纯断线(Disconnected)接着等重连;
// 对端 exit/shutdown(ExitRequested)整场收线;监听收摊(ListenerStopped)
// 同样收线——监听层错没有自愈路,赖活着只会空转。
int Server::RunWsLoop() {
    // 阶段 D:artifact 字节口子与 WS 同端口——协议事件里截图/镜像帧只有
    // 引用(base64 永不进协议),Web 外壳凭 GET /artifact/<名> 取字节。
    // 目录与浏览器面同源(BrowserServiceOptions.artifact_dir 一处配置)。
    WsOptions ws_options = *options_.ws;
    ws_options.artifact_dir = options_.browser_artifact_dir;
    WsTransport transport(ws_options);
    if (!transport.Start()) {
        return 1;
    }
    std::mutex sessions_mutex;
    std::condition_variable sessions_cv;
    std::deque<std::unique_ptr<WsTransport::Session>> pending_sessions;
    bool acceptor_done = false;
    std::thread accept_thread([&] {
        while (true) {
            // artifact GET 在 Accept 里就地应答继续等;升级的 Session 交出去。
            std::unique_ptr<WsTransport::Session> session = transport.Accept();
            if (session == nullptr) {
                break; // 监听叫停/监听层错
            }
            {
                std::lock_guard<std::mutex> lock(sessions_mutex);
                pending_sessions.push_back(std::move(session));
            }
            sessions_cv.notify_one();
        }
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            acceptor_done = true;
        }
        sessions_cv.notify_all();
    });
    WsServeOutcome outcome = WsServeOutcome::Disconnected;
    while (true) {
        std::unique_ptr<WsTransport::Session> session;
        {
            std::unique_lock<std::mutex> lock(sessions_mutex);
            sessions_cv.wait(lock, [&] { return acceptor_done || !pending_sessions.empty(); });
            if (pending_sessions.empty()) {
                break; // 收摊:没有后续连接了
            }
            session = std::move(pending_sessions.front());
            pending_sessions.pop_front();
        }
        outcome = ServeWsSession(std::move(session));
        if (outcome != WsServeOutcome::Disconnected) {
            break;
        }
    }
    transport.Stop(); // 叫停 accept 线程(等连接的那一趟 select 超时内回来)
    accept_thread.join();
    Shutdown();
    return 0;
}

Server::WsServeOutcome Server::ServeWsConnection(WsTransport& transport) {
    std::unique_ptr<WsTransport::Session> session = transport.Accept();
    if (session == nullptr) {
        return WsServeOutcome::ListenerStopped; // 监听叫停/监听层错:没有连接可服务
    }
    return ServeWsSession(std::move(session));
}

Server::WsServeOutcome Server::ServeWsSession(std::unique_ptr<WsTransport::Session> session) {
    // 每条连接新铸 dispatcher:握手状态机(先 initialize 才放业务)是
    // 连接级的,跨连接复用会把第二条连接卡死在 kErrNotInitialized 之外
    // 的所有岔路上。
    const std::shared_ptr<Dispatcher> dispatcher = MakeWsDispatcher();
    // Session 的生命周期:lambda 拿裸指针,连接对象(conn)在本函数栈上
    // 先于 session 析构,不悬空。writer 是写线程调,reader 是读线程调,
    // Session 自己线程安全(写锁 + 收件箱只归读线程)。
    WsTransport::Session* session_ptr = session.get();
    auto connection = std::make_shared<StdioConnection>(
        dispatcher,
        [session_ptr](const std::string& line) {
            if (!session_ptr->SendMessage(line)) {
                Diagnose("WS 写失败(断线),收线");
            }
        },
        [session_ptr]() -> std::string {
            const std::optional<std::string> message = session_ptr->ReadMessage();
            if (!message.has_value()) {
                return std::string(); // EOF:连接收线
            }
            // 一条 WS 文本帧 = 一行协议消息:补上换行喂给 LineFramer
            // (分帧由 WS 层扛,这里只借它的行纪律)。
            return *message + "\n";
        },
        options_.outbox_capacity);
    // 身份章(阶段 B):走到这儿的 WS 连接要么回环免鉴权(本机操作者)、
    // 要么过了 token 门(token 是操作者的秘密)——都裁定 "user"。没过
    // 门的连接在 Accept 里就断了,到不了方法面。内核内部发放的 agent
    // principal(回合驱动的浏览器工具、§4.4 的 agent 连接)不走这儿。
    connection->SetPrincipal("user");
    connection->SetInteractionResolver(MakeInteractionResolver());
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        connection_ = connection;
    }
    connection->Run();
    const bool exit_requested = connection->close_requested();
    // 这条连接收线:打断还挂在它身上的回合(分离出去的僵尸线程经
    // EmitEventSafe 快照,扑不空),thread 账与浏览器会话不动——重连的
    // 外壳凭 cursor(query 类方法)补账,老 threadId 还能继续用。
    InterruptRunningTurns();
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        connection_.reset();
    }
    return exit_requested ? WsServeOutcome::ExitRequested : WsServeOutcome::Disconnected;
}

std::shared_ptr<Dispatcher> Server::MakeWsDispatcher() {
    const std::shared_ptr<Dispatcher> dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory(
        [this]() { return MakeInitializeResult(options_.lubancode_version, PlatformId()); });
    RegisterMethods(*dispatcher);
    return dispatcher;
}

void Server::EmitEventSafe(std::string_view method, const nlohmann::json& params) {
    std::shared_ptr<StdioConnection> connection;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        connection = connection_;
    }
    if (connection != nullptr) {
        connection->EmitEvent(method, params);
    }
}

void Server::InterruptRunningTurns() {
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
    }
}

void Server::Shutdown() {
    // 浏览器面先收:取消在飞动作(审批悬着的也醒)、杀 sidecar 进程树
    // (收尸;profile 锁由 sidecar 退出钩子释放)。
    if (browser_ != nullptr) {
        browser_->Shutdown();
    }
    // 在跑的回合一律按打断收口(与 WS 连接收线同一段),随后会话档句柄
    // 全收。
    InterruptRunningTurns();
    std::vector<std::shared_ptr<ThreadRecord>> records;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (const auto& [id, record] : threads_) {
            records.push_back(record);
        }
    }
    for (const std::shared_ptr<ThreadRecord>& record : records) {
    }
}

// 单测直驱用的连接装配:HandleTurnStart 要经 connection_ 发事件,测试里
// 先用假读写器把连接立起来,再直调 handler。resolver 一并接上,整线
// 直驱(Feed 响应信封)与生产同路。
void Server::AttachForTest(std::unique_ptr<StdioConnection> connection) {
    connection->SetInteractionResolver(MakeInteractionResolver());
    connection_ = std::move(connection);
}

}  // namespace lubancode::app_server
