// 轨迹会话账(P0-2 运行时单一写口):SessionRuntime 持有的 Trajectory
// 侧句柄与轮次边界桥。AR-12 起各桥按现有类边界拆出独立文件,本件保留
// TrajectorySessionLedger(会话 owner 与窄工厂)与 /record 选段器;桥类经
// 下方 include 聚合可见(既有引用与测试零改动)。
//
// 分层(§七/§十五):
//   trajectory::SessionManager/Recorder   纯库,只管账与文件
//   TrajectorySessionLedger(本件)        一场 session 的账本持有者:
//                                         main recorder 所有权 + 子代理
//                                         scoped recorder 注册表
//   TrajectoryTurnBridge                  一轮的边界翻译:loop 的模型
//                                         请求/输出边界 + hub 的工具栅栏
//                                         -> trajectory 事件
//                                         (trajectory_turn_bridge.hpp)
//   TrajectoryBypassBridge                回合外宿主小请求的旁路桥
//                                         (trajectory_bypass_bridge.hpp)
//   TrajectorySubagentBridge 及 workflow  子代理桥与编排账桥
//   桥                                     (trajectory_subagent_bridge.hpp /
//                                          trajectory_workflow_bridge.hpp)
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
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/session_soul.hpp"  // SessionSoulSnapshot(Soul 会话冻结单 P0)
#include "runtime/tool_trajectory_sink.hpp"
#include "runtime/trajectory_bypass_bridge.hpp"  // AR-12:旁路桥(原住本件)
#include "runtime/trajectory_history_view.hpp"  // P3:RestoredHistoryView(v3 旧史显示投影)
#include "runtime/trajectory_subagent_bridge.hpp"  // AR-12:子代理桥(原住本件)
#include "runtime/trajectory_turn_bridge.hpp"      // AR-12:主轮桥(原住本件)
#include "runtime/trajectory_workflow_bridge.hpp"  // AR-12:workflow 桥(原住本件)
#include "telemetry/wake.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/environment.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/usage_gc.hpp"
#include "trajectory/v3/result_store.hpp"  // V3SessionBooks 的结果仓(接线点 1)
#include "trajectory/v3/writer.hpp"  // v3_main_writer:v3 compact 运行时分派门
#include "workspace/identity.hpp"

namespace lubancode::runtime {

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

// 宿主目录通知的正文(前缀缓存守恒单 §五 B):落账(v3 链)与内存注入
// 共用同一只纯函数,两边一字不差——内存史与链投影才对得上账。开头带
// "[宿主通知]"来源标识(与"[用户排队消息]"同款先例),仓库文件/工具
// 正文冒充不了这个格式之外的来源分离(origin 字段才是真凭据,这只是
// 模型可见的自明)。
std::string FormatHostDirectoryNoticeText(const std::string& old_cwd_utf8, const std::string& new_cwd_utf8,
                                          const std::string& reason);

// resume 七步的 runtime 摘要(/resume 与 --continue 的接货单)。
struct TrajectoryResumeSummary {
    trajectory::ResumeOutcome outcome;  // 空 error_code = 成功
    std::vector<api::Message> history;  // 折叠出的有效对话(投影)
    // v3 源(P3 显示侧)的旧史显示投影:含被压缩原文/hidden 标志/压缩
    // 标记(持久 token 字段)。v2 源为 nullopt——v2 照旧走 history 渲染,
    // 数据结构喂不进就不强求(单子 P3 第一棒口径)。
    std::optional<RestoredHistoryView> restored_view;
    // Soul 会话冻结单 P0(§5.3):源场已提交的 soul 快照(锁定那一刻落
    // 进源场目录的 blob)。nullopt = 源场从未锁定过魂,恢复后按未锁定
    // 草稿起步;材料坏在 outcome.error_code 里报错(resume.soul_snapshot_
    // corrupt),不静默换魂。
    std::optional<SessionSoulSnapshot> soul_snapshot;
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
        // --continue 启动路(§10.4):不先造空 session,直接走 resume——
        // v3 源续接源场(2026-09-19 拍板),v2 源开 start_reason=resume 的
        // 迁移新场。resume_source_session_id 空 = 取本 workspace 最近一场
        // 可恢复的;没有任何可恢复场时回落普通开张
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
        // v3 场(接线点 1 开关开)的首行基础 system 正文。空串合法:建场时
        // 宿主还不知道最终 system,第一次模型请求带上真 system 时走 §4.3
        // 三步切换(旧 system -> change 事件 -> 新 system)。
        std::string v3_system_content;
        // 故障注入(测试专用;生产恒空 = 零行为):子账首枚 run.started
        // 提交前问一次,返回稳定码即按该码注入一次失败(子代理空轨迹单
        // 5.1 的 fault injection)。只作用于子账,不影响 main。
        std::function<std::optional<std::string>()> subagent_start_fault;
        // v3 主账写者的提交故障注入(测试专用;生产恒空 = 零行为):非空
        // 稳定码即该枚提交按 IoFailed 收,写者随后 broken——T08(V3-GAP-03)
        // 召回快照 fail-closed 的测试缝,经 SessionManager 递进 writer。
        std::function<std::optional<std::string>()> v3_main_io_fault;
        // workflow 编排单同款:编排账(workflow run)与 node 账(node
        // attempt)各自的首枚 run.started 提交前问一次,fail closed 测试用。
        std::function<std::optional<std::string>()> workflow_start_fault;
        std::function<std::optional<std::string>()> workflow_node_start_fault;
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
    // v3 主账写者(v2 场 nullptr)。异步工具 P2 的会话级运行时从这取
    // 共享写者;互斥锁见 v3_tool_results_mutex。
    trajectory::v3::V3Writer* v3_main_writer();
    // 会话共享账的写者互斥锁(异步 P2 共享写者用;v2 场 nullptr)。
    std::shared_ptr<std::recursive_mutex> v3_tool_results_mutex();

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

    // workflow 编排账开张(workflow 会话归属统一单):ReserveWorkflowRun
    // 建 workflows/<workflow_run_id>/{checkpoints,nodes}/ + definition.json
    // 快照 + 独立 recorder(run_kind=workflow,延迟开卷),首枚 run.started
    // 带 parent_run_id=本场 main_run_id(后台派工语义:main 不落派发边,
    // verifier 核 owner 即可)。失败给结构化 WorkflowSpawnFailure——调用方
    // (workflow runtime)须停在明确失败态,不回退旧写口。
    std::expected<std::unique_ptr<TrajectoryWorkflowRunBridge>, WorkflowSpawnFailure>
    SpawnWorkflowRun(const std::string& workflow_run_id,
                     const TrajectoryWorkflowRunBridge::DefinitionInfo& definition);

    // 旁路模型请求桥(Token 账本单 A1):compact/起名/抽取/doctor 等回合外
    // 小请求落 main stream。每次采样现造一只,用完即弃;账开不出(main
    // recorder 不在)给 nullptr,调用方按"没接轨迹"走旧路。identity 的
    // provider/wire 照实填该次请求真用的端(compact 的 cheap 路由可能跨
    // provider,与主会话端不是一家);channel 建议 "host"。
    // v3 场(取消误报 ESC 单 Bug 2;T11-A 起标题精炼入册):purpose 有
    // v3 消息合同落点的用途(memory_extract / title_refine)接 v3 旁路桥
    //——prepared/sent/终态/usage 走 v3 typed 事件,旁路输入输出不进
    // conversation 链;其余用途(compact 走 v3 compact 运行时、doctor 待
    // 各自接)维持 nullptr 旧路,§四清册记账,不在本单冒进。
    std::unique_ptr<TrajectoryBypassBridge> NewBypassBridge(
        TrajectoryTurnBridge::Identity identity,
        accounting::RequestPurpose purpose = accounting::RequestPurpose::OtherHostRequest);

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

    // ---- 宿主目录通知(前缀缓存守恒单 §五 B)----
    // worktree enter/exit 或 slash 搬房成功后,宿主往 SessionV3 输入提交链
    // 追加一条持久化目录通知:role=user、purpose=Conversation(真进 main
    // 链,resume 折算沿链自然带回)、origin=SessionRuntime(宿主来源,与
    // 人类输入/工具输出分得开;只有宿主装配层走这只口,仓库文件与工具
    // 正文伪造不了这行账)。正文带旧目录/新目录/原因;目录版本由消息在
    // 链上的 seq 表达(账面可定位,不另发号)。幂等由调用侧保证:一次
    // 成功切换只调一次,重试/恢复不重复调。主回合开着(turn 内工具触发)
    // 挂当前 turnId,slash 空闲路自起新回合号——user 消息 turnId 必填
    //(schema §1.2),与 memory.recall 注入同一纪律。
    // 返回空串 = 落稳(AppendMessage + AdmitMessages 两步都过);非空 =
    // 稳定错误码——调用方不得静默发送目录已过期的下一请求(应阻断或明
    // 确中止,见 BlockV3Execution)。v2 老账消费场没有主写者,如实报
    // cwd_notice.not_v3,不伪造行。
    std::string RecordHostDirectoryNotice(const std::string& old_cwd_utf8, const std::string& new_cwd_utf8,
                                          const std::string& reason);

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
    // 启动路 resume 的直接来源场 id(manifest.previous_session_id;没
    // resume 给空)。AppServer 接 v3 第一棒:SessionService 用它把来源场
    // 的操作台账种进新场——新 sessionId 不洗掉旧意图(§4.2 沿来源链识
    // 别原键)。
    std::string launch_resume_source_session_id() const;
    // 当前场的来源(§4.67 G1 goal 沿 lineage 恢复用的通用面):start_reason
    //(process_launch | resume | clear)与 previous_session_id。launch 路与
    // 交互 /resume 换场后都读得到(前一枚访问器只盖 --continue 启动路)。
    struct SessionLineageInfo {
        std::string start_reason;
        std::string previous_session_id;  // 空 = 无前驱
    };
    SessionLineageInfo session_lineage() const;
    // 启动路 resume 折叠出的有效对话投影(没 resume 给空)。
    std::vector<api::Message> LaunchResumeHistory() const;
    // 启动路 resume 的 v3 旧史显示投影(源是 v2/没 resume 给 nullopt)。
    // 装配层用它一次性铺终端滚动缓冲,与 live 条目账分开。
    std::optional<RestoredHistoryView> LaunchRestoredHistoryView() const;
    // Soul 会话冻结单 P0(§5.3):--continue 启动路 resume 带回的源场
    // soul 快照(交互 /resume 走 TrajectoryResumeSummary.soul_snapshot)。
    // nullopt = 没 resume 或源场从未锁定过魂。
    std::optional<SessionSoulSnapshot> LaunchResumeSoulSnapshot() const;
    // 启动路源场 soul 快照材料坏(没坏/没 resume 给空串):装配层据此
    // 明说——报错不静默换魂(§5.3)。
    std::string launch_resume_soul_error() const;
    // 上下文预算单 P1:--continue 启动路 resume 折叠出的控制态(交互
    // /resume 走 TrajectoryResumeSummary.outcome.control,两路同一形状)。
    // 没 resume 给 nullopt;在场但 context_window 缺 = 旧档无预算记录,
    // 恢复裁决按"字段缺失回落"处理,不猜。
    std::optional<trajectory::ReplayControlState> LaunchResumeControlState() const;

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

    // ---- T11-A / V3-GAP-06:标题来源分家(session v3 旧设计清理单) ----
    // v3 场标题四路进账,来源如实分流(v2 老路只认 RecordTitleChanged,
    // 行为一字不动):
    //   RecordTitleChanged        —— 手动 /title:session.title.applied
    //                               (source=manual),不伪造 titleGenerationId;
    //   RecordLocalTitleApplied   —— 首问本地启发式/老档补名(source=local);
    //   RecordTitleRequested      —— 自动精炼起飞:title.requested(带
    //                               titleGenerationId 与路由材料);
    //   RecordTitleExtracted      —— 判词到手:title.extracted(迟到结果
    //                               也照记,是否采用另算);
    //   RecordGeneratedTitleApplied —— 精炼被采用:session.title.applied
    //                               (source=generated,带真 titleGenerationId)。
    // 自动生成流的 prompt/assistant 由旁路桥落正式 message(purpose=
    // session_title,§4.34 归首问 turn),不走这批方法。
    void RecordLocalTitleApplied(const std::string& title, const std::string& old_title);
    void RecordTitleRequested(const std::string& title_generation_id, const std::string& model,
                              const std::string& provider);
    void RecordTitleExtracted(const std::string& title_generation_id, const std::string& title);
    void RecordGeneratedTitleApplied(const std::string& title_generation_id, const std::string& title,
                                     const std::string& old_title);

    // ---- P0-4:环境快照(§9.1/§9.2) ----
    // 会话侧身份与材料由装配层采好递进;git/cwd/os 由账本现取。v2 落
    // run.environment.captured(snapshot blob + replay_level + gaps);v3 场
    // (T11-C)落 session.environment.captured,载荷同口径 camelCase。一场
    // run 只落一次,重复调用是幂等 no-op。回空串 = 成功,否则稳定码。
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

    // ---- Soul 会话冻结单 P0(§5.1"宿主串行锁定并持久化快照") ----
    // 把锁定的会话魂快照 blob 落进当前场目录(soul-snapshot.json;正文
    // 全文存档,resume 只认它,不凭魂名重读磁盘文件)。回空串 = 成功;
    // 失败给稳定码人话并记 recent_io_errors——不拦发送(快照 blob 写不住
    // 是恢复材料缺口,不该把会话卡死;resume 侧读不到按未锁定处理,
    // 材料坏则报错拒绝)。
    std::string CommitSoulSnapshot(const SessionSoulSnapshot& snapshot);

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

    // ---- v3 compact 运行时接线(compact 全链单) ----
    // 账本的 v3 主写者:本场会话开卷走 v3(session_switch 接线点 1:
    // LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS 开的新会话)时非空,v2 会话与
    // 未开卷恒 nullptr。/compact 与自动压缩据此分派:v3 会话走
    // v3_compact_runtime 的全链(compact 全链单),v2 会话照旧走本文件
    // 的 RecordCompact* 老路,一字不动。接线点 1 并进后由开卷装配喂真
    // 写者;当前主树尚无 v3 开卷路,恒 nullptr(分派门在,通路等接线)。
    // (v3_main_writer 的正式声明在上方"main stream"段;此处不重复。)

    // ---- v3 结果仓统计(V3-REAL-A02:/context 消费) ----
    // v3 会话的工具结果在提交边界由 v3::ResultStore 存盘(artifacts/res-*,
    // 一份 res-*.json 元数据 = 一枚逻辑工具结果,伴生通道文件同前缀)。
    // 这里按仓的记账单位现数:枚数只数 res-*.json(文件数 = 结果数,可核),
    // 字节收全部 res-* 文件。非 v3 会话返回 nullopt——调用方走旧 artifact
    // 口径,不拿 0 枚冒充"没有结果"。
    struct V3ResultStoreStats {
        std::size_t results = 0;
        std::uint64_t total_bytes = 0;
    };
    std::optional<V3ResultStoreStats> V3ResultStoreStatsOf() const;

    // ---- D3(§5.1.2):compact applied 后的内存换账投影 ----
    // v3 会话专用:重读主卷验卷(读回即 applied 行的持久化确认)→
    // ProjectModelContext 链投影(选中 system 之外的链序输入:生效摘要 +
    // 保留消息)→ EffectiveConversationFromV3 + ProjectHistoryFromReplay
    // 折成 api::Message——与 /resume、/export 同一份投影,不另造账。
    // 调用方在 RunV3Compact 返回 applied(PowerLoss 落稳)后取走,
    // ReplaceHistory 进 loop——v2 compact 换账的同一安全点。非 v3 场或
    // 验卷不过:错误,调用方不换并明说。
    std::expected<std::vector<api::Message>, std::string> ProjectV3ContextHistory() const;

    // ---- T12-A(V3-GAP-07 P0):compact 投影失败的会话级执行阻断 ----
    // 置位:applied 已落稳、ProjectV3ContextHistory/ReplaceHistory 失败的
    // 调用方在报错的同时调它。此后本场所有主会话轮桥的请求最终准入拒绝
    //(V3RequestPrepared 返回空串,loop 本步明败不发模型);CLI/AppServer/
    // Goal/Loop 殊途同门,不是只在 CLI 分支加早退。幂等:已阻断时重复置位
    // 保留首因。非 v3 场(无主账)no-op。reason 用调用方拿到的稳定错误
    //(compact.swap.*)。
    void BlockV3Execution(const std::string& reason);
    // 阻断查询(/doctor、测试、AppServer 状态面):false = 本场可继续。
    bool V3ExecutionBlocked() const;

    // ---- T12-C(V3-GAP-07):当前活动主轮的真实号 ----
    // v3 场且主回合此刻在跑(BeginTurn 后、EndTurn 前)时给回合号;
    // 其余(回合收口后的粘账、v2 场、无账)给 nullopt。中途压缩
    //(pre_send/context_overflow)递它当 parentTurnId;idle 手动压缩拿
    // nullopt 如实表达"无活动主轮",不伪造 parent。读侧与写侧同一本书,
    // 回合串行推进中才变——压缩回调活在回合线程,无锁直读。
    std::optional<std::string> OpenMainTurnId() const;

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
    // Ctrl+T 转录浮层的 v3 分页(P3 第二棒):v3 场按 seq 游标切页(首开
    // 两游标皆空 = 尾页);投影渲染一次入缓存,绑定源文件字节数,变了才
    // 重投——翻页不反复全量重读(§4.10 缓存须绑定源指纹)。非 v3 场(或
    // 场找不着)给 nullopt,调用方走 v2 头尾截断老路,一字不变。
    std::optional<RestoredTranscriptPage> ReadTranscriptPage(
        const std::string& session_id, const std::optional<std::uint64_t>& before_seq,
        const std::optional<std::uint64_t>& after_seq, std::size_t max_lines) const;
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

    // ---- 上下文预算单 P1:窗口预算的写账口(/context、面板、开场快照与
    // 恢复裁决共用)----
    // 有效修改落 control.context_window.changed(v2 场)/ session.context_
    // window.applied(v3 场),载荷带 provider/model 身份与 source 来路
    // (manual|initial|resumed)。old_window 传 0 = 首枚/无旧值。返回
    // true = 已提交;false = 账本没开/写不住——调用方须如实报告"仅本次
    // 生效",不得宣称恢复可用。幂等由调用方保证(值没变别写,§四合同)。
    bool RecordContextWindowChanged(std::size_t window_tokens, std::size_t old_window_tokens,
                                    const std::string& provider, const std::string& model,
                                    const std::string& source);

private:
    // T11-A:session.title.applied 的共用尾段(source 分流 manual/local/
    // generated;generated 才带 titleGenerationId)。
    void AppendTitleAppliedV3_(const std::string& title, const std::string& old_title,
                               std::string_view source, const std::string* title_generation_id);

public:

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
    // 接线点 1:active 是 v3 场时把 v3 共享账绑到当前主账写者(clear/
    // resume 换场后 active 指针会换,books 必须跟着重绑,不然悬空);
    // v2 场清掉。
    void BindV3Books_();
    // v3 新场沿用 v3 源场的生效 system(§4.10 默认):与基础版不同就按
    // §4.3 三步切换,链根与实际发送同拍。v2 源没有 v3 system 概念,不调。
    void AdoptSourceSystemV3_(const std::filesystem::path& source_stream);
    // SpawnSubagent 的 v3 分支(父会话是 v3 场):v3::SubagentSpawn 五步
    //(§4.32)开 subagents/<childSessionId>/<childSessionId>.jsonl。
    std::expected<std::unique_ptr<TrajectorySubagentBridge>, SubagentSpawnFailure> SpawnSubagentV3(
        const std::string& parent_call_id, const std::string& task_label,
        const std::string& parent_run_id);

    // 会话级控制事件(compact 一族)的公共落账口(Host/CompactRuntime)。
    void PutControl_(trajectory::EventKind kind, nlohmann::json payload);
    // 真人命令事件的落账口(actor=user/external_user,§5.5)。
    void PutUserCommand_(trajectory::EventKind kind, nlohmann::json payload);
    // 当前已发到几(seq;source_event_span 的终点)。
    std::uint64_t SpanEndSeq();
};

}  // namespace lubancode::runtime
