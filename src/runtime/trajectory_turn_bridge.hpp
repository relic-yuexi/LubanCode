// 轨迹主轮桥(AR-12 机械拆分:自 trajectory_session.hpp 按桥类边界拆出,
// 类体与合同一字未动)。一轮的边界翻译:loop 的模型请求/输出边界 + hub
// 的工具栅栏 -> trajectory 事件;hub 侧的抽象口在 tool_trajectory_sink.hpp。
// V3SessionBooks 是 v3 写侧会话共享账,由 TrajectorySessionLedger 的 Impl
// 持有,主会话/子代理/旁路各桥借指针共用——随主桥头走,供各桥与账本同取。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/tool_trajectory_sink.hpp"
#include "telemetry/wake.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// v3 写侧会话共享账(session_switch.hpp 接线点 1):v3 场由 Trajectory
// SessionLedger 的 Impl 持有一份,v3 模式的轮桥(主会话/子代理)借指针
// 共用——system 正文/设置版本跨轮延续,结果仓按 session 目录开一次。
// V3Writer 自带 mutex 管单写者发号;这里的可变字段只在回合串行推进中
// 改(与 v2"一只桥一轮"的纪律同门)。
// ---------------------------------------------------------------------------

struct V3SessionBooks {
    trajectory::v3::V3Writer* writer = nullptr;          // 主账写者(ActiveSession::v3_main)
    std::string system_content;              // 当前根 system 正文(§4.3 切换后更新)
    std::uint64_t settings_version = 1;      // systemMeta.settingsVersion 序列
    std::optional<trajectory::v3::ResultStore> captures;
    std::shared_ptr<std::recursive_mutex> tool_results_mutex = std::make_shared<std::recursive_mutex>();
    std::optional<trajectory::v3::ResultStore> results;  // 惰性开:session 目录 artifacts/
    // ---- T12-A(V3-GAP-07 P0,SessionV3 旧设计清理单):执行阻断 ------------
    // compact applied 已落稳、但内存换账(链投影 / ReplaceHistory)失败时
    // 置位。此后本场所有主会话轮桥(CLI/AppServer/Goal/Loop 同一条路)的
    // 请求最终准入一律拒绝:不发新模型请求、不派新工具(请求拒了就没有
    // 新 assistant 的 tool_use)、自动续跑同门;在飞请求按真实状态收尾。
    // 保留已提交链,不回写不重压。解除只有一条路:换场时 books 重建
    // (resume/clear 重开即天然解除);本进程内不静默放行——恢复须沿已
    // 提交 v3 上下文核验重建,那发生在新场的新 books 上。
    // 注:阻断只住内存,不落账——schema 尚无对应 kind(T11 按合同发行),
    // 不拿旧 payload 换名伪造。
    bool execution_blocked = false;
    std::string execution_block_reason;         // 稳定原因(compact.swap.*)
    std::uint64_t execution_block_revision = 0;  // 阻断时账面 revision(准入对表/诊断)
    // 绑定场次(session_id):换场判据用。manager 的 active 是 std::optional,
    // clear 同址换值时新写者地址与旧写者相同(地址复用),单比指针认不出
    // 换场——旧 books(含执行阻断)会原样带进新场。
    std::string bound_session_id;
    // 最近一只主会话轮桥 BeginTurn 的回合号(取消误报 ESC 单 Bug 2):
    // v3 旁路桥拿它挂 parentTurnId——记忆抽取在回合尾巴跑,主回合多半已
    // 收口,"最近一只"就是触发它的那只。只在回合串行推进中写(与本书
    // 其余可变字段同一纪律);子代理桥绑自己的 books,不碰这只。
    std::string active_main_turn_id;
    // T12-C(V3-GAP-07,SessionV3 旧设计清理单):主回合此刻是否真在跑
    //(BeginTurn 置 / EndTurn 清)。active_main_turn_id 是"最近一只"
    // 的粘账,分不清在跑与收口;idle 手动压缩要按"实际无活动主轮"表达
    //(parentTurnId 落 null),turn 中途压缩要递真号——判据就是这只旗。
    bool main_turn_open = false;
    // provider 调用号 -> v3 调用身份:轮桥声明 tool call 时登记(§4.15),
    // 子代理五步的 parentActionRef 从这查(actionId/声明消息/turn/step)。
    struct DeclaredAction {
        std::string action_id;
        std::string message_id;  // 声明它的 assistant 消息
        std::string turn_id;
        std::string step_id;
    };
    std::map<std::string, DeclaredAction> declared_actions;
};

// ---------------------------------------------------------------------------
// 轮次边界桥:AgentLoop 的模型边界 + hub 的工具栅栏 -> trajectory 事件
//    (hub 侧的抽象口在 tool_trajectory_sink.hpp;本类实现它)
// ---------------------------------------------------------------------------

// v3 模式轮桥的回合簿(定义在 trajectory_session.cpp;此处只占位)。
struct V3TurnBooks;

class TrajectoryTurnBridge : public agent::LoopBoundaryRecorder, public ToolTrajectorySink {
public:
    // recorder:落账的 stream(main 或 subagent 各一只)。identity:身份
    // 与渠道(provider/wire 名、channel 名),进 run/turn 事件的 payload。
    struct Identity {
        std::string provider;
        std::string wire;
        std::string channel = "terminal";  // terminal | app_server | subagent
        // 应用Worker接入单 §八(本单切片):连接快照的冻结件
        // (connection_snapshot.hpp 的形状;wire/model/provider 已在 prepared
        // 载荷顶层,这里补的是 endpoint/secretRef/configVersion 三件,只落
        // v3 的 model.request.prepared connection 块——v2 载荷键表封闭,不
        // 写,老账形状零变化)。非 object(终端/子代理桥不带)= 载荷零变化。
        // 快照本身是脱敏合同:不含密钥正文,也不含密钥的内容哈希。
        nlohmann::json connection;
    };

    TrajectoryTurnBridge(trajectory::TrajectoryRecorder& recorder, trajectory::EventScope base_scope,
                         Identity identity);
    // v3 写模式(接线点 1):v2 recorder 不在,主账是 V3Writer;v3_books 是
    // 会话级共享账(system 正文/结果仓),由账本 Impl 持有。base_scope 只借
    // workspace/session/run 三枚身份(wake 投递用),不进事件信封——v3 行
    // 的身份在信封自己的 sessionId/runId。
    TrajectoryTurnBridge(trajectory::v3::V3Writer* v3_writer, V3SessionBooks* v3_books,
                         trajectory::EventScope identity_scope, Identity identity);
    ~TrajectoryTurnBridge() override;

    TrajectoryTurnBridge(const TrajectoryTurnBridge&) = delete;
    TrajectoryTurnBridge& operator=(const TrajectoryTurnBridge&) = delete;
    // 移动构造允许(轮次桥按值造好带出;recorder 是引用成员,move 不断
    // 链)。move 赋值随引用成员隐式删除。
    TrajectoryTurnBridge(TrajectoryTurnBridge&&) = default;

    // ---- 轮次生命周期(turn_runner 在 DriveTurn 前后调) ----
    // trigger:external_user | queued_user | peer_agent | scheduled_host |
    // goal_continuation(§5.1:起因写明,不拿第一条 user 消息猜)。
    void BeginTurn(const std::string& turn_id, const std::string& trigger);
    // 主输入( durable 的那份;动态上下文注入不进来)。input_id 自增。
    void RecordInput(const api::Message& user_message);
    // 收口:ok && !cancelled -> turn.completed;cancelled -> turn.cancelled;
    // 其余 turn.failed(reason)。悬空的已声明调用补 tool.cancelled
    // (闸前未执行/收不回的尾巴),不冒充执行过。
    void EndTurn(bool ok, bool cancelled, const std::string& reason);

    // ---- agent::LoopBoundaryRecorder(loop 在模型边界调) ----
    void OnContextPressure(const agent::ContextPressure& pressure) override;
    std::string OnRequestPrepared(const api::Request& request, const agent::RequestPreparedContext& ctx) override;
    // false = sent 这笔本地账没写稳(失败与恢复单 P1-C/FA-03):调用方不得
    // 把请求交给 backend。事件语义只到"本地交给 transport",不暗示远端
    // 收据。
    bool OnRequestSent(const std::string& request_id) override;
    // 任务级 turn 账(turn 预算单 §11.1,P1-1):permit 提交后的 sent 边界带
    // task_turn_index/turn_limit/input_round_index;随后同 request_id 的
    // output 三态收口也带上 task_turn_index——started/completed/failed 三处
    // 边界数字与台账同一本账,不靠数 assistant message 猜。
    bool OnRequestSentWithTurn(const std::string& request_id, int task_turn_index, int turn_limit,
                               int input_round_index) override;
    // v3 流式边(轨迹 v3 §4.43):loop 在 SSE 消费点调(MessageStart 到 =
    // 响应开始;文本/思考增量为片段)。v2 模式 no-op——v2 无流式事件账。
    void OnResponseStarted(const std::string& request_id) override;
    void OnStreamDelta(const std::string& request_id, const std::string& delta_type,
                       const std::string& text) override;
    void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id,
                         int cache_epoch = 0, bool prefix_append_only = true,
                         bool cache_read_reported_by_provider = false,
                         bool cache_creation_reported_by_provider = false,
                         const std::string& usage_anomaly = std::string()) override;
    bool OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                           const std::string& stop_reason,
                           const std::string& provider_response_id) override;
    void OnOutputFailed(const std::string& request_id, const std::string& reason) override;
    void OnOutputCancelled(const std::string& request_id, agent::OutputCancelSource source) override;

    // ---- ToolTrajectorySink(hub 在工具栅栏调) ----
    void OnToolTrace(const agent::ToolTraceEvent& event) override;
    void ConfigureActionSummary(api::Backend* backend, const ActionSummaryProfile& profile) override {
        std::unique_lock<std::recursive_mutex> lock;
        if (v3_books_) lock = std::unique_lock(*v3_books_->tool_results_mutex);
        action_summary_backend_ = backend;
        action_summary_profile_ = profile;
        action_summary_calls_remaining_ = profile.max_calls;
        ++action_summary_generation_;
    }
    // 批次尾结果提交回执(失败与恢复单 P1-A/FA-01):Failed = 有结果的
    // "模型可见 tool 消息"没写稳,调用方须停止后续模型发送;Degraded =
    // 主账正文已保住的约定降级(metadata 落盘失败一类),放行另查链。
    // v2 桥回执默认 Committed 且不改正文;v3 桥在管预览与整批预算。
    bool ManagesToolResultPreviews() const override { return V3Mode(); }
    ToolResultsCommitReceipt RewriteToolResultsForHistory(api::Message& results) override;
    ToolResultsCommitReceipt CaptureToolResult(const api::ToolResultBlock& result) override;
    ToolResultsCommitReceipt OnToolResultsCommitted(const std::string& batch_id,
                                                    const api::Message& results) override;
    bool ShouldBlockExecution(const agent::ToolTraceEvent& started) override;

    // ---- 子代理边界(§3.5:父子文件只传边界引用与 terminal hash) ----
    // agent 工具派工时挂子 run 引用:该 call 的 started/终态事件带
    // relations.child_run_id。
    void AttachChildRun(const std::string& call_id, const std::string& agent_run_id);
    // 子账收口后报终态 hash:该 call 的执行终态 payload 带
    // child_run_id 与 child_terminal_event_hash(双向对账的父侧)。
    void NoteChildTerminal(const std::string& agent_run_id, const std::string& terminal_event_hash);

    // ---- P0-4:verification 与 outcome(§5.5/§五 5.5) ----
    // 验证点落账:started+recorded 两枚,observed_after_seq 钉在当前账尾。
    // 回 verification_id(空 = 落账失败,§7.4"verification 记不住,不得
    // 判 verified success"——调用方不得据此宣称已验)。
    std::string BeginVerification(const std::string& kind, const std::string& subject,
                                  const std::string& producer);
    void FinishVerification(const std::string& verification_id, bool passed,
                            const nlohmann::json& facts, const nlohmann::json& command_ref = nlohmann::json(),
                            const std::vector<std::string>& artifact_paths = {});

    // ---- P0-4:存储门(§12.2 storage_exhausted) ----
    // 磁盘余量低于 journal emergency reserve 时给 false。副作用栅栏
    //(OnToolTrace 的 started 路)与装配层据此拒绝新的副作用;大模型请求
    // 的拒绝门属 P0-6 回退门,本批明留缺口。
    bool StorageAvailable() const;

    // 诊断:最近一枚提交失败 receipts 的稳定码(测试与 /doctor 用)。
    std::vector<std::string> recent_errors() const { return recent_errors_; }
    // P0-D:无主 tool trace 的有界诊断投影(run_id/turn_id/execution_id/
    // call_id/tool_name/parent_execution_id 各一行)。只进这里,不进父
    // canonical call 状态机(calls_ 一个字节不动)。
    std::vector<std::string> unowned_trace_notes() const { return unowned_trace_notes_; }
    // P0-B/turn_runner 用:当前轮 id(空 = 轮没开),子账开张失败的父侧
    // typed 事件按它带 turn_id。
    const std::string& current_turn_id() const { return turn_id_; }
    // 落账错误的共享汇(账本持有,/doctor trajectory 的"最近 I/O 错误"
    // 从这取;桥按轮把错误推进来)。
    void SetErrorSink(std::vector<std::string>* sink) { error_sink_ = sink; }

    // 端云协同可观测单 T1(§25.3/§25.4):committed wake 窄口。账本侧在
    // receipt committed 后通知;空(默认)= 零行为,trajectory 老路一字
    // 不变。wake 只投"哪条 stream 有新账",不携正文,不阻塞落账路径。
    void SetCommitWake(telemetry::CommitObserver* wake, std::string stream_id) {
        commit_wake_ = wake;
        wake_stream_id_ = std::move(stream_id);
    }

    // ---- 异步工具 P2:闸门/规划器的账面查询口(v3 模式;v2 返回空) ----
    // provider call id -> 声明上下文(assistant 已落账后可查;会话共享
    // 声明册,批次闸门 TakeJobOrder 的 originRef 三件从这取)。
    struct V3CallOrigin {
        std::string action_id;
        std::string message_id;  // 声明消息(assistantMessageRef 锚)
        std::string turn_id;
        std::string step_id;
    };
    std::optional<V3CallOrigin> V3DeclaredCallOrigin(const std::string& provider_call_id) const;
    // request_id -> 流式预留的 assistant messageId(提前档调用证据锚;
    // 流没起账/请求簿没有 → nullopt)。
    std::optional<std::string> V3ReservedAssistantMessageId(const std::string& request_id) const;
    // request_id -> 响应证据事件 id(投递 acknowledged 的 evidenceRef 用;
    // 完整 assistant 成行的请求给 model.response.completed 的事件 id,
    // 其余 nullopt——没证据不宣称接纳)。
    std::optional<std::string> V3ResponseEvidenceId(const std::string& request_id) const;

    // 会话共享写者与锁(子代理/旁路装配挂异步运行时用;v2 场 nullptr)。
    trajectory::v3::V3Writer* v3_writer() const { return v3_writer_; }
    std::shared_ptr<std::recursive_mutex> v3_shared_mutex() const {
        return v3_books_ != nullptr ? v3_books_->tool_results_mutex : nullptr;
    }

private:
    api::Backend* action_summary_backend_ = nullptr;
    ActionSummaryProfile action_summary_profile_;
    int action_summary_calls_remaining_ = 0;
    std::uint64_t action_summary_generation_ = 0;
    bool action_summary_running_ = false;
    struct CallBook {
        std::string request_id;         // 声明它的 model output 所属请求
        // P0-E:只由已提交的 model.output.completed 置真。dangling 收口
        //(CancelDanglingCalls)只认 declared=true 的账项——无主调用不是
        // 悬空调用,不补 cancelled,不造明知过不了 schema 的事件。
        bool declared = false;
        bool planned = false;           // tool.execution.planned 已落
        bool effective = false;         // tool.input.effective 已落
        bool started = false;           // tool.execution.started 已提交
        bool terminal = false;          // 执行终态已落
        std::string terminal_event_id;  // 终态事件 id(result committed 引它)
        bool result_committed = false;
        bool terminal_cancelled = false;  // 终态是 cancelled(免 result)
        std::string child_run_id;         // agent 工具派出的子 run(§3.5)
        // P0-4 细账料:started 时留下的实际入参与来源(命令的 argv/shell、
        // MCP 的 server 身份从这翻,§9.3)。
        nlohmann::json effective_arguments = nlohmann::json::object();
        std::string source_instance;
    };

    // 验证点在账(§5.5):本轮 recorded 的验证,文件被改动时逐枚判 stale。
    struct VerificationBook {
        std::string verification_id;
        std::string kind;
        std::string subject;
        std::string producer;
        bool passed = false;
        bool recorded = false;   // recorded 事件已落(started 之后)
        bool invalidated = false;
        std::string recorded_event_id;
    };

    trajectory::RecordReceipt Put(trajectory::EventKind kind, std::optional<std::string> request_id,
                                  std::optional<std::string> call_id, trajectory::Actor actor,
                                  trajectory::Origin origin, nlohmann::json payload,
                                  trajectory::Durability durability = trajectory::Durability::ProcessCrash,
                                  trajectory::EventLinks links = {});
    void NoteError(const trajectory::RecordReceipt& receipt, const char* where);
    // P0-D:陌生 tool trace 的有界诊断。只记投影,不改 calls_,不落 canonical。
    void NoteUnownedToolTrace(const agent::ToolTraceEvent& event);

    // ---- v3 写模式(接线点 1;V3Mode() 为假时一只方法都不进) ----
    // v3 提交失败 → recent_errors/error_sink + 日志(v2 NoteError 的同款
    // 漏斗,receipt 形状换成 v3::WriteReceipt)。
    void NoteV3Error(const trajectory::v3::WriteReceipt& receipt, const char* where);
    // v3 提交成功后的 committed wake(只投身份,不投正文)。
    void V3NotifyCommitted(const trajectory::v3::WriteReceipt& receipt);
    // system 对表(§4.3):请求携带的 system 与当前根不同 → 三步切换,
    // 正文相同不动(不造假版本)。成功/无需切换回 true。
    bool V3EnsureSystem(const std::string& system_content);
    void V3RecordInput(const api::Message& user_message);
    std::string V3RequestPrepared(const api::Request& request,
                                  const agent::RequestPreparedContext& ctx);
    // false = model.request.sent 落不住(P1-C/FA-03):请求不得上 wire。
    bool V3RequestSent(const std::string& request_id);
    // ---- v3 流式三件套(§4.43/§4.63;接线点 1 的 D1 修复) ----
    // 响应开始:预留 messageId + 发 streamId,落 model.response.started。
    // 幂等;prepared 没落稳的请求不伪造流。
    void V3ResponseStarted(const std::string& request_id);
    // 片段进账:按类型攒批(4 KiB 窗口一批),批满即落 model.response.delta;
    // 不足一批的尾巴由终态前统一放行。
    void V3StreamDelta(const std::string& request_id, const std::string& delta_type,
                       const std::string& text);
    // 懒起流(非流式后端零片段路同样保 started 形状):返回 false = started
    // 记不住,定稿不得继续(§4.4 同款耐久栅栏)。
    bool V3EnsureStreamStarted(const std::string& request_id);
    // 放行一只攒批尾巴(空批 no-op)。
    void V3FlushStreamBatch(const std::string& request_id, bool reasoning);
    // text + reasoning 两条攒批尾巴一并放行(终态前调)。
    void V3FlushStreamBatches(const std::string& request_id);
    void V3UsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id);
    bool V3OutputCompleted(const std::string& request_id, const api::Message& assistant,
                           const std::string& stop_reason, const std::string& provider_response_id);
    void V3OutputFailed(const std::string& request_id, const std::string& reason);
    void V3OutputCancelled(const std::string& request_id, agent::OutputCancelSource source);
    void V3ToolTrace(const agent::ToolTraceEvent& event);
    // 批次结果提交回执(P1-A):结果链各档折算(见 ToolResultsCommitReceipt)。
    ToolResultsCommitReceipt V3ToolResultsCommitted(api::Message& results);
    // turn 收口:已声明未终态的 Action 补 cancelled(配对完整,不悬空)。
    void V3CancelDanglingActions(const std::string& reason);
    // v3 模式判定(空 = v2 原路)。
    bool V3Mode() const { return v3_writer_ != nullptr; }
    std::string NextRequestId();
    std::string NextInputId();
    std::string NextOutputId();
    std::string NextVerificationId();
    // 悬空收口:turn 终态前把已声明未收口的调用补 tool.cancelled。
    void CancelDanglingCalls(const std::string& reason);
    // §9.3 side-effect 细账:file(undo token)/command(有效入参 + exit
    // code)/mcp(server 身份 + jsonrpc id)三形,按事件里实际有的料拼。
    nlohmann::json BuildSideEffects(const agent::ToolTraceEvent& event, const CallBook& book,
                                    bool* has_exit_code, std::int64_t* exit_code) const;
    // §5.5 stale invalidation:本次改动的文件路径逐枚对账,recorded 未
    // invalidated 且 subject 命中的落 verification.invalidated。
    void InvalidateStaleVerifications(const std::string& mutated_path,
                                      const std::string& invalidated_by_event);
    // T11-D:stale invalidation 的 v3 实现(tool.verification.invalidated)。
    void InvalidateStaleVerificationsV3(const std::string& mutated_path,
                                        const std::string& invalidated_by_action);
    // turn 收口的证据裁断(§5.5 outcome.assessed):有 fresh 证据才落,
    // 引用未失效的 verification.recorded 事件 id。
    void AssessOutcome(bool ok, bool cancelled);
    // 消息 content -> 规范 blocks 数组(模型中立;大正文交 blob,由
    // recorder 的 offload 上限管)。
    static nlohmann::json MessageToBlocks(const api::Message& message);

    trajectory::TrajectoryRecorder* recorder_ = nullptr;  // v2 模式的主账(引用改指针:类要装得下 v3 模式)
    trajectory::v3::V3Writer* v3_writer_ = nullptr;                   // v3 模式的主账
    V3SessionBooks* v3_books_ = nullptr;                  // v3 会话共享账(system/结果仓)
    std::unique_ptr<V3TurnBooks> v3_turn_;                // v3 回合簿(请求/调用),v2 模式为空
    trajectory::EventScope base_scope_;  // 身份四件 + 默认 actor/origin
    Identity identity_;
    std::string turn_id_;
    bool turn_open_ = false;
    std::map<std::string, CallBook> calls_;  // call_id(模型 tool_use id)
    // P0-D:无主 tool trace 的有界诊断投影(上限 32 条,溢出只计数)。
    std::vector<std::string> unowned_trace_notes_;
    std::size_t unowned_trace_dropped_ = 0;
    std::map<std::string, std::string> request_prepared_;  // request_id -> prepared event id
    // 任务 turn 账的请求簿(§11.1,P1-1):sent 时记下这枚请求的 turn 坐标,
    // output 三态收口按 request_id 对回。只住本轮内存,不落盘。
    struct RequestTurnBook {
        int task_turn_index = 0;
        int turn_limit = 0;
        int input_round_index = 0;
    };
    std::map<std::string, RequestTurnBook> request_turns_;
    std::map<std::string, std::string> child_terminal_hashes_;  // agent_run_id -> hash
    std::set<std::string> started_io_failed_;  // started 落不住被拦的 execution
    std::set<std::string> storage_blocked_;    // 磁盘 reserve 不足被拦的 execution
    std::vector<VerificationBook> verifications_;
    std::string last_input_event_id_;
    std::uint64_t request_counter_ = 0;
    std::uint64_t input_counter_ = 0;
    std::uint64_t output_counter_ = 0;
    std::uint64_t verification_counter_ = 0;
    std::vector<std::string> recent_errors_;
    std::vector<std::string>* error_sink_ = nullptr;  // 账本持有的共享汇
    telemetry::CommitObserver* commit_wake_ = nullptr;  // T1 committed wake(默认空)
    std::string wake_stream_id_;                        // session 相对 stream 路径
};

}  // namespace lubancode::runtime
