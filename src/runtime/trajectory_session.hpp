// 轨迹会话账(P0-2 运行时单一写口):SessionRuntime 持有的 Trajectory
// 侧句柄与轮次边界桥。
//
// 分层(§七/§十五):
//   trajectory::SessionManager/Recorder   纯库,只管账与文件
//   TrajectorySessionLedger(本件)        一场 session 的账本持有者:
//                                         main recorder 所有权 + 子代理
//                                         scoped recorder 注册表
//   TrajectoryTurnBridge(本件)           一轮的边界翻译:loop 的模型
//                                         请求/输出边界 + hub 的工具栅栏
//                                         -> trajectory 事件
//   ToolTraceHub(改造)                  持久账从 SessionStore 改接本件
//                                         的桥(旧指针路 P0-6 删)
//
// P0-2(Trajectory 升为唯一 Session):feature/env 开关已删,Session 即
// Trajectory——开不出账就会话明败,不回退旧 SessionStore(旧件退役待
// P0-5/P0-6,本批不再消费)。
#pragma once

#include "approval_mode.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/tool_trajectory_sink.hpp"
#include "telemetry/wake.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/environment.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/usage_gc.hpp"
#include "workspace/identity.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 轮次边界桥:AgentLoop 的模型边界 + hub 的工具栅栏 -> trajectory 事件
//    (hub 侧的抽象口在 tool_trajectory_sink.hpp;本类实现它)
// ---------------------------------------------------------------------------

class TrajectoryTurnBridge : public agent::LoopBoundaryRecorder, public ToolTrajectorySink {
public:
    // recorder:落账的 stream(main 或 subagent 各一只)。identity:身份
    // 与渠道(provider/wire 名、channel 名),进 run/turn 事件的 payload。
    struct Identity {
        std::string provider;
        std::string wire;
        std::string channel = "terminal";  // terminal | app_server | subagent
    };

    TrajectoryTurnBridge(trajectory::TrajectoryRecorder& recorder, trajectory::EventScope base_scope,
                         Identity identity);
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
    void OnRequestSent(const std::string& request_id) override;
    // 任务级 turn 账(turn 预算单 §11.1,P1-1):permit 提交后的 sent 边界带
    // task_turn_index/turn_limit/input_round_index;随后同 request_id 的
    // output 三态收口也带上 task_turn_index——started/completed/failed 三处
    // 边界数字与台账同一本账,不靠数 assistant message 猜。
    void OnRequestSentWithTurn(const std::string& request_id, int task_turn_index, int turn_limit,
                               int input_round_index) override;
    void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id,
                         int cache_epoch = 0, bool prefix_append_only = true,
                         bool cache_reported_by_provider = false) override;
    bool OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                           const std::string& stop_reason,
                           const std::string& provider_response_id) override;
    void OnOutputFailed(const std::string& request_id, const std::string& reason) override;
    void OnOutputCancelled(const std::string& request_id) override;

    // ---- ToolTrajectorySink(hub 在工具栅栏调) ----
    void OnToolTrace(const agent::ToolTraceEvent& event) override;
    void OnToolResultsCommitted(const std::string& batch_id, const api::Message& results) override;
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

private:
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
    // turn 收口的证据裁断(§5.5 outcome.assessed):有 fresh 证据才落,
    // 引用未失效的 verification.recorded 事件 id。
    void AssessOutcome(bool ok, bool cancelled);
    // 消息 content -> 规范 blocks 数组(模型中立;大正文交 blob,由
    // recorder 的 offload 上限管)。
    static nlohmann::json MessageToBlocks(const api::Message& message);

    trajectory::TrajectoryRecorder& recorder_;
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

// ---------------------------------------------------------------------------
// 旁路模型请求桥(Token 账本单 A1):compact 的 map/reduce、记忆抽取、
// 会话起名、doctor 探针这类"回合外的宿主小请求"的模型边界 -> trajectory
// 事件。§11.2"旁路请求也走公共 ModelRequestRecorder"落在它身上:接口
// 与主桥同一只(agent::LoopBoundaryRecorder),SampleModel/探针只认接口。
//
// 与主桥的差异:没有工具栅栏簿记,每次 OnRequestPrepared 自开一只
// scheduled_host 小 turn 并把请求的首条 user 消息记作 input(recorder
// 状态机约束:sent/output 须落 turn 内,首 sent 前须有 input),output
// 三态收口时把 turn 一并收掉。一只桥一次采样用完即弃,不跨线程共享
// ——每处调用自己造(recorder 提交全程持锁,多桥并发在盘上仍串行)。
// ---------------------------------------------------------------------------
class TrajectoryBypassBridge : public agent::LoopBoundaryRecorder {
public:
    TrajectoryBypassBridge(trajectory::TrajectoryRecorder& recorder, trajectory::EventScope base_scope,
                           TrajectoryTurnBridge::Identity identity);
    ~TrajectoryBypassBridge() override;

    TrajectoryBypassBridge(const TrajectoryBypassBridge&) = delete;
    TrajectoryBypassBridge& operator=(const TrajectoryBypassBridge&) = delete;

    // ---- agent::LoopBoundaryRecorder(采样/探针在模型边界调) ----
    std::string OnRequestPrepared(const api::Request& request,
                                  const agent::RequestPreparedContext& ctx) override;
    void OnRequestSent(const std::string& request_id) override;
    void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id,
                         int cache_epoch = 0, bool prefix_append_only = true,
                         bool cache_reported_by_provider = false) override;
    bool OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                           const std::string& stop_reason,
                           const std::string& provider_response_id) override;
    void OnOutputFailed(const std::string& request_id, const std::string& reason) override;
    void OnOutputCancelled(const std::string& request_id) override;

    // 诊断:最近一枚提交失败 receipts 的稳定码。
    std::vector<std::string> recent_errors() const { return recent_errors_; }
    // T1 committed wake(与主桥同款;默认空 = 零行为)。
    void SetCommitWake(telemetry::CommitObserver* wake, std::string stream_id) {
        commit_wake_ = wake;
        wake_stream_id_ = std::move(stream_id);
    }

private:
    trajectory::RecordReceipt Put(trajectory::EventKind kind, std::optional<std::string> request_id,
                                  trajectory::Actor actor, trajectory::Origin origin, nlohmann::json payload,
                                  trajectory::Durability durability = trajectory::Durability::ProcessCrash);
    void NoteError(const trajectory::RecordReceipt& receipt, const char* where);
    void OpenTurn();
    void CloseTurn(bool ok, bool cancelled, const std::string& reason);
    std::string NextRequestId();
    std::string NextTurnId();
    std::string NextInputId();
    std::string NextOutputId();

    trajectory::TrajectoryRecorder& recorder_;
    trajectory::EventScope base_scope_;
    TrajectoryTurnBridge::Identity identity_;
    std::string turn_id_;
    bool turn_open_ = false;
    bool dead_ = false;  // 开不了小 turn(主 turn 在开着)后哑火,不再连发
    std::map<std::string, std::string> request_prepared_;  // request_id -> prepared event id
    std::string last_input_event_id_;
    std::uint64_t request_counter_ = 0;
    std::uint64_t turn_counter_ = 0;
    std::uint64_t input_counter_ = 0;
    std::uint64_t output_counter_ = 0;
    std::vector<std::string> recent_errors_;
    telemetry::CommitObserver* commit_wake_ = nullptr;  // T1 committed wake(默认空)
    std::string wake_stream_id_;
};

// ---------------------------------------------------------------------------
// 子代理轨迹桥:AgentTool 派工时申请,子 loop 的边界与工具事件落子账
// ---------------------------------------------------------------------------

class TrajectorySubagentBridge {
public:
    virtual ~TrajectorySubagentBridge() = default;
    virtual const std::string& run_id() const = 0;
    virtual TrajectoryTurnBridge& turn_bridge() = 0;
    // 收口:run terminal + 关柄(§8.3 journal_sha256)。返回子账终态事件
    // 的 event_hash(父账 finished 边界引用它);账已坏给空串,父账如实
    // 标注。
    virtual std::string Finish(bool ok, const std::string& reason) = 0;
};

// ---------------------------------------------------------------------------
// /record 选段器(§14.3:从"第二只录音笔"改成"轨迹选段器")
// ---------------------------------------------------------------------------

class TrajectorySessionLedger;

// 只圈 canonical refs,不旁听、不复制事实。start/pause/resume/note/stop
// 各落 record.selection.* 事件进 main Journal;canonical Journal 照常全录,
// 暂停不制造事实缺口。一场 session 至多一份活动 selection。
class RecordSelectionController {
public:
    explicit RecordSelectionController(TrajectorySessionLedger& ledger);

    bool active() const { return !record_id_.empty(); }
    bool paused() const { return paused_; }
    const std::string& record_id() const { return record_id_; }

    // 空 error = 成功。name 只作 annotation 记录。
    std::string Start(const std::string& name, const std::string& goal,
                      const std::vector<std::string>& variables, const std::string& acceptance);
    std::string Pause();
    std::string Resume();
    std::string Note(const std::string& text);
    // stop:completed(included_spans 圈本 session 起点至今的事件段,
    // source_terminal_hashes 记当前末 hash)。选段事实在 Journal 里,
    // 草稿由 P0-5 的 SkillDraftCompiler 从同一 selection 确定性重编。
    std::string Stop(const std::string& verification);
    std::string Cancel();

private:
    std::string Put_(trajectory::EventKind kind, nlohmann::json payload);

    TrajectorySessionLedger& ledger_;
    std::string record_id_;
    bool paused_ = false;
    std::string start_event_hash_;
    std::uint64_t selection_counter_ = 0;
};

// ---------------------------------------------------------------------------
// 一场 session 的轨迹账本(flag 开的会话由装配层挂进 SessionRuntime)
// ---------------------------------------------------------------------------

// ReplayState -> api::Message 投影(P0-3 §15.4:durable history 是 replay
// projection 的内存缓存,每项带 source event id)。runtime 适配层做翻译,
// trajectory 纯库不认 api。
std::vector<api::Message> ProjectHistoryFromReplay(const trajectory::ReplayState& state);

// resume 七步的 runtime 摘要(/resume 与 --continue 的接货单)。
struct TrajectoryResumeSummary {
    trajectory::ResumeOutcome outcome;  // 空 error_code = 成功
    std::vector<api::Message> history;  // 折叠出的有效对话(投影)
};

// ---------------------------------------------------------------------------
// 子代理空轨迹单 P0-A/P0-B:SpawnSubagent 的结构化失败(替代裸字符串——
// 装配层吞 error() 是这次第一因查不出的根,失败必须带阶段与稳定码过境)。
// ---------------------------------------------------------------------------
struct SubagentSpawnFailure {
    // 失败阶段:reserve_stream | recorder_start | run_started。
    std::string stage;
    // 稳定码(trajectory.subagent_stream / trajectory.subagent_recorder /
    // trajectory.subagent_run_started 前缀 + 底层码)。
    std::string error_code;
    // 字段级人话(schema 缺哪个字段、io 细节);不含子 prompt 正文与
    // 敏感绝对路径。
    std::string detail;
    std::string reserved_run_id;  // 已铸出的子 run id(失败前铸了就带上)
    bool retryable = false;      // I/O 类失败可重试;schema/状态机类不可
};

class TrajectorySessionLedger {
public:
    struct Options {
        // P0-1:冻结身份由装配层裁决后整份递进(终端/app-server/子代理同一
        // 把钥匙)。空身份且 workspace_root 也空时,才按启动 cwd 现场裁决
        // ——这条兜底只服务旧测试;子代理/Gateway 恢复路必须显式递身份,
        // 不得让各进程按临时 cwd 另算 key(§4.5)。
        workspace::WorkspaceIdentity workspace_identity;
        std::filesystem::path workspace_root;  // 兜底裁决起点与环境快照根
        // P0-2:唯一项目持久化根。空 = <home>/.lubancode/workspaces(生产
        // 默认);测试注入临时根。旧 trajectories/ 根零读零写。
        std::filesystem::path workspaces_root;
        std::string launch_cwd;      // UTF-8 文本,进 manifest
        std::string lubancode_version;
        // run.started 的 v2 usage owner 账(Token 账本单 §6.1.1):
        // main 与 subagent 各自的 stream 统一 v2。
        int event_schema_version = 2;
        // --continue 启动路(§10.4):不先造空 session,直接开
        // start_reason=resume 的新场。resume_source_session_id 空 = 取本
        // workspace 最近一场可恢复的;没有任何可恢复场时回落普通开张
        // (quiet_if_none 语义,与旧路 --continue 一致)。
        bool resume_at_launch = false;
        std::string resume_source_session_id;
        // 单发轨迹断档单:one_shot 一场置 true——main run 写 run_kind=
        // one_shot(manifest/信封/run.started 三处同源),resume 候选排除。
        bool one_shot = false;
        // 轮桥与子代理账的默认 training_policy:交互会话吃缺省 Metadata;
        // 单发账本配 Exclude(实战派活含内部路径,不进训练集),配置
        // oneshot_training_policy 可改。session 边界事件恒 Exclude,不在此列。
        trajectory::TrainingPolicy training_policy = trajectory::TrainingPolicy::Metadata;
        // 故障注入(测试专用;生产恒空 = 零行为):子账首枚 run.started
        // 提交前问一次,返回稳定码即按该码注入一次失败(子代理空轨迹单
        // 5.1 的 fault injection)。只作用于子账,不影响 main。
        std::function<std::optional<std::string>()> subagent_start_fault;
        ApprovalMode approval_mode = ApprovalMode::Default;
    };

    // 进程一场:LaunchSession(建 workspace/session 目录、独占锁、
    // main recorder、run.started、session.json running)。flag 开了却开
    // 不出账,给错误——调用方须让会话启动失败,不许回退旧写口(§十七)。
    // resume_at_launch:先按 §10.4 七步 resume-as-new;源场验不过按
    // options 的回落策略(见上)。
    static std::expected<TrajectorySessionLedger, std::string> Open(Options options);

    TrajectorySessionLedger(TrajectorySessionLedger&&) noexcept;
    TrajectorySessionLedger& operator=(TrajectorySessionLedger&&) = delete;
    ~TrajectorySessionLedger();

    // main stream(轮次桥从这只造)。
    trajectory::TrajectoryRecorder* main();

    // 主会话一轮的桥(turn_runner 每轮 New 一只,绑 main recorder)。
    std::unique_ptr<TrajectoryTurnBridge> NewTurnBridge(TrajectoryTurnBridge::Identity identity);

    // 子代理 scoped JSONL(§3.5 subagents/<agent_run_id>.jsonl):目录占位
    // + 独立 recorder + run.started(run_kind=subagent,relations 带
    // parent_run_id/parent_call_id)。子 Agent 只拿自己的桥;父拿不到子
    // recorder 的写权限。parent_run_id(递归派工单 P1-2 嵌套轨迹边):空串
    // = main 直派(落回本场 main_run_id,行为与从前一致);非空 = 嵌套
    // 派工——派工者自己的 agent_run_id,relations.parent_run_id 记它,不
    // 冒充 main("嵌套 headless 路的父亲是父任务的 run,不是 main")。
    // 失败给结构化 SubagentSpawnFailure(阶段 + 稳定码 + 字段级人话):
    // 正式 .jsonl 由首枚 run.started 提交事务独占创建(P0-C),失败不
    // 留 0 字节残留。
    std::expected<std::unique_ptr<TrajectorySubagentBridge>, SubagentSpawnFailure> SpawnSubagent(
        const std::string& parent_call_id, const std::string& task_label,
        const std::string& parent_run_id = std::string());

    // 子代理空轨迹单 P0-B:子账开张失败的父侧 typed 事件
    //(subagent.run.start_failed,由父 run 持有,main stream 落账)。
    // parent_call_id/turn_id 按在场与否如实带;stream_ref 是 session 相对
    // 引用(subagents/<file>),不写绝对路径。失败事实同时进 recent_io_errors
    // 与日志——诊断要能跨进程留证,不靠终端滚屏。
    void NoteSubagentStartFailed(const SubagentSpawnFailure& failure, const std::string& parent_run_id,
                                 const std::string& parent_call_id, const std::string& turn_id);

    // 旁路模型请求桥(Token 账本单 A1):compact/起名/抽取/doctor 等回合外
    // 小请求落 main stream。每次采样现造一只,用完即弃;账开不出(main
    // recorder 不在)给 nullptr,调用方按"没接轨迹"走旧路。identity 的
    // provider/wire 照实填该次请求真用的端(compact 的 cheap 路由可能跨
    // provider,与主会话端不是一家);channel 建议 "host"。
    std::unique_ptr<TrajectoryBypassBridge> NewBypassBridge(TrajectoryTurnBridge::Identity identity);

    // 父账边界:子代理 finished 时补的边界引用(child run id + 子账终态
    // hash),由主桥的 OnToolTrace 落——这里只给查口。
    std::optional<std::string> ChildTerminalHash(const std::string& agent_run_id) const;

    // 正常封口(/exit 与 EOF):turn 收齐后 run terminal + session.ended +
    // session.json closed。恢复器/replay 是 P0-3 的活,这里只留封口。
    trajectory::CloseOutcome CloseSession(const std::string& reason);

    // ---- P0-1(§4.5):cwd 变化对账 ----
    // 同 workspace(common git dir 未变,如 /worktree 进出 linked worktree):
    // 落 control.cwd.changed + checkout 登记,账不换房。跨 workspace:
    // 不写旧账,same_workspace=false 带新 key——调用方封当前 session 后在
    // 新 workspace 开新场,不许偷偷往旧房搬账。
    struct CwdChangeResult {
        bool same_workspace = false;
        std::string workspace_key;  // 新裁决的 key
        std::string error;          // 落账/登记失败说明(空 = 顺)
    };
    CwdChangeResult HandleCwdChange(const workspace::WorkspaceIdentity& new_identity);

    // ---- P0-3:clear 八步换账 / resume-as-new / replay 读口 ----

    // /clear 的换账事务(§3.3.1 八步,SessionManager 串行掌管)。flag 关
    // 的会话不走这里(P0-2 遗留#3 的收口)。成功后账本自动指到新场,
    // 选段器重置。
    trajectory::ClearOutcome ClearSession(const trajectory::ClearRequest& request,
                                          trajectory::ClearParticipant* participant);

    // 交互 /resume(§10.4):旧场写 requested → 封口(end_reason=
    // switch_to_resume)→ 新场七步开张(start_reason=resume)。source
    // Journal 永不 reopen append;已完成 child 只核 terminal hash,不内联
    // 正文。回的 history 是折叠投影,调用方 ReplaceHistory 接上。
    TrajectoryResumeSummary ResumeInteractive(const std::string& source_session_id,
                                              const std::string& command_name = "resume");

    // 本 workspace 最近一场可恢复的 session(空 = 没有)。
    std::string LatestResumableSessionId() const;

    // 运行中切档即写(单子 §七):把 active 场 session.json 的 approval_mode
    // 原子换成新档。终端空闲/流式两处用户 Shift+Tab 切档都经装配层挂的
    // 钩子走到这里;clear 继承内存份、resume 继承盘上份,从此同拍。回空
    // 串 = 成功,否则稳定码人话(账没开张/写盘失败——切档本身不失败,只是
    // 持久化没跟上,调用方按可见性决定声张)。
    std::string UpdateApprovalMode(ApprovalMode mode);

    // --continue 启动路开出来的场是不是 resume(装配层据此把折叠投影灌进
    // loop 的 history 并打"已恢复 N 条"一行)。
    bool resumed_at_launch() const;
    // 启动路 resume 折叠出的有效对话投影(没 resume 给空)。
    std::vector<api::Message> LaunchResumeHistory() const;

    // 折叠本场 main.jsonl(纯读,writer 持句柄照读——journal 以共享读开)。
    // /export、/copy、session view 的数据源(§14.5:一律读 ReplayState)。
    trajectory::ReplayReport FoldMainReplay() const;

    // session verifier 引擎口(trajectory verify)。
    trajectory::SessionVerifyReport VerifySession() const;

    // exact replay 引擎口:回本场折叠账与规范状态 hash。
    struct ExactReplay {
        bool ok = false;
        std::string state_hash;
        std::string error_code;
        trajectory::ReplayState state;
    };
    ExactReplay ExactReplayMain() const;

    // /record 选段器(一场 session 一只)。
    RecordSelectionController& record_selection();

    // ---- P0-4:环境快照(§9.1/§9.2) ----
    // 会话侧身份与材料由装配层采好递进;git/cwd/os 由账本现取。落
    // run.environment.captured(snapshot blob + replay_level + gaps)。
    // 一场 run 只落一次,重复调用是幂等 no-op。回空串 = 成功,否则稳定码。
    struct EnvironmentFacts {
        std::string provider;
        std::string wire;
        std::string model;
        nlohmann::json model_parameters = nlohmann::json::object();
        std::string system_prompt;  // 非空则先落 blob 得 system_prompt_ref
        trajectory::ToolsetSummary toolset;
        std::vector<std::string> project_instruction_refs;
        std::vector<std::string> loaded_skill_refs;
        nlohmann::json plugin_refs = nlohmann::json::array();
        nlohmann::json config_snapshot_redacted = nlohmann::json::object();
        std::vector<std::pair<std::string, std::string>> allowlisted_env;
    };
    std::string CaptureEnvironment(const EnvironmentFacts& facts);

    // ---- P0-4:排队账(§5.5 control.queue.item.*) ----
    // steering queue 的状态可见变化经这四枚口进 Journal。item_id 用队列
    // 的稳定 id("q-<n>"),input_id 同 item_id——排队消息本身就是输入身份,
    // 真正触发 turn 时 input.received 另发新 id,两账以 item_id 关联。
    void NoteQueueEnqueued(const std::string& item_id, const std::string& target_label,
                           const std::string& reason = {});
    void NoteQueueDequeued(const std::string& item_id, const std::string& reason = {});
    void NoteQueueCancelled(const std::string& item_id, const std::string& reason);
    void NoteQueueExpired(const std::string& item_id, const std::string& reason = {});

    // ---- P0-4:容量与存储(§12.2) ----
    // 磁盘 reserve 门(storage_exhausted 的判据)。reserve 字节数用 §13
    // 首版起始值 16 MiB(journal_emergency_reserve_bytes 的本批缺省)。
    bool StorageAvailable() const;
    // workspace 容量账(lubancode trajectory usage 的引擎体)。
    trajectory::WorkspaceUsageReport WorkspaceUsage() const;
    // /doctor trajectory 的引擎体(§13.1 只读聚合):active session 各
    // stream 验链 + 容量四笔 + 磁盘余量 + 最近 I/O 错误。
    trajectory::WorkspaceDoctorReport BuildDoctorReport() const;
    // 最近落账错误(桥逐轮推进来;doctor 的"最近 I/O 错误"从这取)。
    std::vector<std::string> recent_io_errors() const;

    // 命令生命周期(§14.1/§15.7 TrajectoryCommandExecutor 的落账半场):
    // BeginCommand 在 handler 前 durable 落 control.command.requested
    //(真人敲 slash:actor=user/origin=external_user),回 command_id;
    // EndCommand 在 handler 后落 completed/failed。P0-2 最小版:effect
    // class 按命令名粗分表,动作级细分随 P0-4 的注册表元数据落。
    std::string BeginCommand(const std::string& command_name, const std::string& action_name,
                             const std::string& effect_class);
    void EndCommand(const std::string& command_id, bool ok, const std::string& reason);

    // /compact 的 typed 状态机(P0-2 最小版:requested + applied/failed;
    // prepared/generated/validation 的细分账随 P0-4 落)。trigger:
    // manual | auto | midturn(§14.4 三路同一状态机,只差 trigger)。
    void RecordCompactRequested(const std::string& trigger, int old_epoch,
                                const std::string& input_state_hash);
    void RecordCompactApplied(const std::string& old_state_hash, const std::string& new_state_hash,
                              std::uint64_t pre_tokens, std::uint64_t post_tokens, int new_epoch);
    void RecordCompactFailed(const std::string& reason);

    const std::string& session_id() const;
    std::filesystem::path session_dir() const;
    // T1 遥测注册用:本场 workspace 的假名 key(与 wake/cursor 同一口径)。
    std::string workspace_key() const;

    // ---- P0-2:会话读面与 workspace 管理面(命令/app-server 共用) ----
    // 唯一持久化根(<home>/.lubancode/workspaces);空 = 开账时没递。
    std::filesystem::path workspaces_root() const;
    // /sessions、/resume 选择器、thread/list 的数据源:可重建索引查询
    // (session_index.hpp;不为每次列表重放所有 Journal)。
    trajectory::SessionIndexPage ListWorkspaceSessions(
        const trajectory::SessionIndexQuery& query) const;
    // Ctrl+R 提问历史(当前 workspace;旧→新,max_lines 截尾)。
    std::vector<trajectory::PromptHistoryLine> ReadPromptHistory(std::size_t max_lines) const;
    // /resume 选择器的 Ctrl+T 转录浮层。
    std::vector<std::string> MakeTranscriptExcerpt(const std::string& session_id,
                                                   std::size_t max_half) const;
    // workspace 管理操作(任意场次;本进程 active 的那场仍走成员语义,
    // 先 close 再动)。回空 error_code = 成功。
    std::string ArchiveSessionInWorkspace(const std::string& session_id) const;
    std::string UnarchiveSessionInWorkspace(const std::string& session_id) const;
    std::string DeleteSessionInWorkspace(const std::string& session_id,
                                         const std::string& reason) const;

    // ---- P0-2:标题与协作档的会话控制账(/title、Plan 档;resume 折叠
    // 回 ReplayControlState.title/mode)----
    void RecordTitleChanged(const std::string& title, const std::string& old_title);
    void RecordModeChanged(const std::string& mode, const std::string& reason,
                           const std::string& old_mode);

    // 端云协同可观测单 T1(§25.4 路 1):装配层把 TelemetryService 挂上,
    // 之后本账本铸的每只桥与每笔会话级控制事件提交后都投 committed wake。
    // 空(默认)= 零行为,老路一字不变;Notify 端承诺非阻塞(§25.3)。
    void SetTelemetryWake(telemetry::CommitObserver* wake);

private:
    TrajectorySessionLedger() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<RecordSelectionController> record_selection_;
    std::uint64_t command_counter_ = 0;
    // P0-4:落账错误共享环(桥逐轮推进;doctor 从这读,见 recent_io_errors)。
    std::vector<std::string> io_errors_;
    bool environment_captured_ = false;

    // committed wake 的账本侧漏斗:main stream 上的提交经这投。
    void NotifyCommitted_() const;

    // 会话级控制事件(compact 一族)的公共落账口(Host/CompactRuntime)。
    void PutControl_(trajectory::EventKind kind, nlohmann::json payload);
    // 真人命令事件的落账口(actor=user/external_user,§5.5)。
    void PutUserCommand_(trajectory::EventKind kind, nlohmann::json payload);
    // 当前已发到几(seq;source_event_span 的终点)。
    std::uint64_t SpanEndSeq();
};

}  // namespace lubancode::runtime
