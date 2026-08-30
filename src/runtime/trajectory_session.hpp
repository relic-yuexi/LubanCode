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
//                                         的桥(flag 关的老路一字不变)
//
// flag 门控(§十七"内部预览"):ResolveTrajectoryEnabled 合成配置
// features.trajectory(默认 false)与环境变量 LUBANCODE_TRAJECTORY。flag
// 开的会话只写 Trajectory,不写旧 SessionStore(禁 dual-write);flag 关
// 的会话照旧路走,trajectory 目录一个字节不产。
#pragma once

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
#include "trajectory/directory.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/session_manager.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// flag(§十七:开发期 test-only feature flag,产品切换按 session 原子选路)
// ---------------------------------------------------------------------------

// 合成开关:config_flag 来自配置 features.trajectory(默认 false);环境
// 变量 LUBANCODE_TRAJECTORY 显式压一头——"1"/"true" 开,"0"/"false" 关,
// 没设或空串听配置。默认整条路是关的,旧路行为零变。
bool ResolveTrajectoryEnabled(bool config_flag);

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
    std::string OnRequestPrepared(const api::Request& request) override;
    void OnRequestSent(const std::string& request_id) override;
    void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id) override;
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

    // 诊断:最近一枚提交失败 receipts 的稳定码(测试与 /doctor 用)。
    std::vector<std::string> recent_errors() const { return recent_errors_; }

private:
    struct CallBook {
        std::string request_id;         // 声明它的 model output 所属请求
        bool planned = false;           // tool.execution.planned 已落
        bool effective = false;         // tool.input.effective 已落
        bool started = false;           // tool.execution.started 已提交
        bool terminal = false;          // 执行终态已落
        std::string terminal_event_id;  // 终态事件 id(result committed 引它)
        bool result_committed = false;
        bool terminal_cancelled = false;  // 终态是 cancelled(免 result)
        std::string child_run_id;         // agent 工具派出的子 run(§3.5)
    };

    trajectory::RecordReceipt Put(trajectory::EventKind kind, std::optional<std::string> request_id,
                                  std::optional<std::string> call_id, trajectory::Actor actor,
                                  trajectory::Origin origin, nlohmann::json payload,
                                  trajectory::Durability durability = trajectory::Durability::ProcessCrash,
                                  trajectory::EventLinks links = {});
    void NoteError(const trajectory::RecordReceipt& receipt, const char* where);
    std::string NextRequestId();
    std::string NextInputId();
    std::string NextOutputId();
    // 悬空收口:turn 终态前把已声明未收口的调用补 tool.cancelled。
    void CancelDanglingCalls(const std::string& reason);
    // 消息 content -> 规范 blocks 数组(模型中立;大正文交 blob,由
    // recorder 的 offload 上限管)。
    static nlohmann::json MessageToBlocks(const api::Message& message);

    trajectory::TrajectoryRecorder& recorder_;
    trajectory::EventScope base_scope_;  // 身份四件 + 默认 actor/origin
    Identity identity_;
    std::string turn_id_;
    bool turn_open_ = false;
    std::map<std::string, CallBook> calls_;  // call_id(模型 tool_use id)
    std::map<std::string, std::string> request_prepared_;  // request_id -> prepared event id
    std::map<std::string, std::string> child_terminal_hashes_;  // agent_run_id -> hash
    std::set<std::string> started_io_failed_;  // started 落不住被拦的 execution
    std::string last_input_event_id_;
    std::uint64_t request_counter_ = 0;
    std::uint64_t input_counter_ = 0;
    std::uint64_t output_counter_ = 0;
    std::vector<std::string> recent_errors_;
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

class TrajectorySessionLedger {
public:
    struct Options {
        std::filesystem::path workspace_root;  // 仓库根(算 workspace_key;空 = 启动 cwd)
        std::filesystem::path trajectories_root;  // 空 = <home>/.lubancode/trajectories
        std::string readable_workspace_name;
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
    // recorder 的写权限。
    std::expected<std::unique_ptr<TrajectorySubagentBridge>, std::string> SpawnSubagent(
        const std::string& parent_call_id, const std::string& task_label);

    // 父账边界:子代理 finished 时补的边界引用(child run id + 子账终态
    // hash),由主桥的 OnToolTrace 落——这里只给查口。
    std::optional<std::string> ChildTerminalHash(const std::string& agent_run_id) const;

    // 正常封口(/exit 与 EOF):turn 收齐后 run terminal + session.ended +
    // session.json closed。恢复器/replay 是 P0-3 的活,这里只留封口。
    trajectory::CloseOutcome CloseSession(const std::string& reason);

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

private:
    TrajectorySessionLedger() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<RecordSelectionController> record_selection_;
    std::uint64_t command_counter_ = 0;

    // 会话级控制事件(compact 一族)的公共落账口(Host/CompactRuntime)。
    void PutControl_(trajectory::EventKind kind, nlohmann::json payload);
    // 真人命令事件的落账口(actor=user/external_user,§5.5)。
    void PutUserCommand_(trajectory::EventKind kind, nlohmann::json payload);
    // 当前已发到几(seq;source_event_span 的终点)。
    std::uint64_t SpanEndSeq();
};

}  // namespace lubancode::runtime
