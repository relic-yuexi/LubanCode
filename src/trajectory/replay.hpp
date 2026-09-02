// Replay Projection(P0 新轨迹记录单 §十):纯读 Journal,折叠出 ReplayState。
//
//   run state / turn state / effective conversation / provider request steps
//   tool ledger / control state / evidence ledger / artifact refs
//   quality & integrity report
//
// 三不规矩(§10.1):不调模型、不执行工具、不读墙钟不随机发号。同一份
// Journal、同一 replay version,规范状态 hash 必须相同(§10.2)——hash 输入
// 只取折叠账的确定性投影,顺序一律按事件折叠序,不掺 I/O 抖动。
//
// checkpoint(§10.4 末段):只是缓存。payload 带
// source_seq/source_event_hash/state_hash;对不上便丢,从 Journal 重算。
//
// session verifier(§十四 verify):扫目录逐文件验 hash chain,再交叉核
// 父子边(§3.9 五条)。它不调模型、不执行工具。
//
// 依赖铁律:trajectory 纯库,不 include app/cli/runtime。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/event.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::trajectory {

// 折叠器的投影版本。改动折叠形状(加减折叠字段、改投影规则)必须升这枚
// 版本;升版后旧 checkpoint 全数作废(重算),state hash 换新值。
// v2(Token 账本单 A1):ReplayRequestStep 加 purpose 折叠字段;宿主旁路
// 用途(compact/memory/title/doctor/insights/other_host)的模型输出不再
// 折进 effective_conversation——它们是工作产物,不是会话历史。
inline constexpr int kReplayProjectionVersion = 2;

// ---------------------------------------------------------------------------
// 折叠账(ReplayState 的零件)
// ---------------------------------------------------------------------------

// effective conversation 的一条(§15.4 HistoryItem:projected message 带
// source event id + origin,不另抄正文)。
struct ReplayMessage {
    enum class Role { User, Assistant, Tool } role = Role::User;
    std::string origin;  // 信封 origin(external_user/queued_user/provider_model/…)
    std::optional<std::string> call_id;  // tool result 的配对键
    nlohmann::json blocks = nlohmann::json::array();  // 规范 blocks(text/tool_call/…)
    std::string source_event_id;                     // 来源事件(qualified:run 内 event_id)
    std::string source_event_hash;

    nlohmann::json ToJson() const;
    static std::optional<ReplayMessage> FromJson(const nlohmann::json& json);
};

// 一次 provider 请求步(prepared→sent→output 三段)。prepared 段保留
// model/provider/wire/parameters/message_refs——harness 桩的指纹材料。
struct ReplayRequestStep {
    std::string request_id;
    std::string prepared_event_id;
    std::string model;
    std::string provider;
    std::string wire;
    // 请求用途(model.request.prepared.payload.purpose;空 = 事件没带,
    // v1 旧账与不带 purpose 的旧 v2 账都是空)。折叠与投影按它分"对话
    // 轮"与"回合外的宿主小请求"(Token 账本单 A1):compact/起名/抽取
    // 一类的输出是宿主吃掉的工作产物,不是会话历史。
    std::string purpose;
    nlohmann::json parameters = nlohmann::json::object();
    std::vector<std::string> message_refs;
    bool sent = false;
    std::string output_state;  // "" | completed | failed | cancelled
    std::string output_event_id;
    nlohmann::json output_blocks = nlohmann::json::array();
    std::string stop_reason;
    bool usage_recorded = false;  // v2 model.usage.recorded 已见

    nlohmann::json ToJson() const;
    static std::optional<ReplayRequestStep> FromJson(const nlohmann::json& json);
};

// 该 purpose 的模型输出算不算会话历史(Token 账本单 A1):main_turn/
// subagent_turn/goal_continue/loop_iteration 是对话轮,照旧折叠;compact/
// memory/title/doctor/insights/other_host 一类宿主旁路请求的输出是工作
// 产物,不进 effective_conversation(purpose 为空的旧账照旧折叠——不因
// 新字段改老行为)。
bool PurposeFoldsIntoConversation(const std::string& purpose);

// 工具台账一条(§5.4 六事件折叠;悬空按 started/finished/result 三道账分档)。
struct ReplayToolEntry {
    std::string call_id;
    std::string tool_name;
    bool planned = false;
    bool effective = false;
    bool started = false;
    bool terminal = false;
    std::string terminal_kind;  // finished|failed|cancelled|unknown;"" = 悬空
    std::string outcome;        // succeeded|…;unknown 终态给 reason
    std::string effective_arguments_sha256;
    bool result_committed = false;
    // 子代理边界(§3.5):relations.child_run_id 与父侧记录的子账终态 hash。
    std::optional<std::string> child_run_id;
    std::string child_terminal_event_hash;  // 父侧记录值;空 = 未记(后台派工)
    std::string started_event_id;
    std::string terminal_event_id;

    // §10.4 第 4 步:悬空三道账的判据收在 CollectDanglingTools;这里只留
    // 未知副作用一问(unknown 不算 success,不可重跑)。terminal_kind 存
    // 的是完整 kind 名(tool.execution.unknown)。
    bool UnknownSideEffect() const { return terminal_kind == "tool.execution.unknown"; }

    nlohmann::json ToJson() const;
    static std::optional<ReplayToolEntry> FromJson(const nlohmann::json& json);
};

// 控制态折叠(title/cwd/mode/queue/compact/selection/resume 来历)。
struct ReplayControlState {
    std::optional<std::string> title;
    std::optional<std::string> cwd;
    std::optional<std::string> mode;
    std::optional<std::uint64_t> context_window;
    int compact_epoch = 0;
    std::string last_compact_new_state_hash;
    std::vector<std::string> open_queue_items;  // enqueued 未终态
    std::string active_record_selection;        // started 未终态的 record_id
    std::optional<std::string> resumed_from_session_id;

    nlohmann::json ToJson() const;
    static std::optional<ReplayControlState> FromJson(const nlohmann::json& json);
};

// 证据台账一条(verification.recorded/invalidated + outcome.assessed)。
struct ReplayEvidenceEntry {
    std::string verification_id;
    std::string kind;
    bool passed = false;
    bool invalidated = false;
    std::uint64_t observed_after_seq = 0;

    nlohmann::json ToJson() const;
    static std::optional<ReplayEvidenceEntry> FromJson(const nlohmann::json& json);
};

// 一枚外层 turn 的折叠。
struct ReplayTurnEntry {
    std::string turn_id;
    std::string trigger;
    std::string terminal_state;  // ""=open | completed | failed | cancelled
    std::string outcome;

    nlohmann::json ToJson() const;
    static std::optional<ReplayTurnEntry> FromJson(const nlohmann::json& json);
};

// 质量与完整性报告(§8.4 fail-closed:未知 stateful/execution/evidence 事件
// → unsupported,不静默略过)。
struct ReplayIntegrity {
    std::uint64_t events_folded = 0;
    std::string last_event_hash;
    bool truncated_tail = false;  // 尾行截断:已验证前缀照折,run 判 incomplete
    bool unsupported = false;     // 未知关键事件/超前的 min_reader_version
    std::string unsupported_reason;
    std::uint64_t dangling_tools = 0;
    bool unknown_side_effects = false;

    nlohmann::json ToJson() const;
    static std::optional<ReplayIntegrity> FromJson(const nlohmann::json& json);
};

// 折叠账本体。
struct ReplayState {
    // run 身份与终态。
    std::string workspace_key;
    std::string session_id;
    std::string run_id;
    RunKind run_kind = RunKind::MainSession;
    std::string start_reason;
    std::string run_terminal_state;  // ""=running | completed | failed | cancelled
    std::string session_end_state;   // ""=未封 | ended
    std::optional<std::string> next_session_id;  // session.ended 带

    std::vector<ReplayTurnEntry> turns;
    std::vector<ReplayMessage> effective_conversation;
    std::vector<ReplayRequestStep> requests;
    std::vector<ReplayToolEntry> tools;
    ReplayControlState control;
    std::vector<ReplayEvidenceEntry> evidence;
    std::vector<std::string> artifact_refs;  // 折叠序里见到的 blob sha256
    ReplayIntegrity integrity;

    // 折叠到几(seq 高水位)。
    std::uint64_t folded_seq = 0;

    nlohmann::json ToJson() const;
    static std::optional<ReplayState> FromJson(const nlohmann::json& json);
};

struct ReplayReport {
    // 空 error_code = 折叠成功(integrity 里可带警告)。
    //   replay.read_failed / replay.verify_failed / replay.unsupported
    std::string error_code;
    std::string message;
    ReplayState state;
    bool ok() const { return error_code.empty(); }
};

// 折叠一份 stream(先整本验链,再逐行折叠)。尾行截断:已验证前缀照折,
// integrity.truncated_tail=true,不伪造终态(§16.3)。链中断/schema 坏:
// replay.verify_failed,不折。未知关键事件/超前 min_reader_version:
// replay.unsupported(fail-closed,§8.4)。
ReplayReport FoldStreamReplay(const std::filesystem::path& stream_path);

// 规范状态 hash:SHA256("replay-v<N>" || canonical(确定性投影))。同一
// Journal 同一 replay version 折两次必同(§10.2,单测钉)。
std::string ComputeReplayStateHash(const ReplayState& state);

// ---------------------------------------------------------------------------
// checkpoint(§10.4:只是缓存,对不上便丢)
// ---------------------------------------------------------------------------

struct ReplayCheckpoint {
    int schema_version = 1;
    int replay_version = kReplayProjectionVersion;
    std::string stream_name;  // "main" / 子 run id(目录 checkpoints/ 下文件名前缀)
    std::uint64_t source_seq = 0;
    std::string source_event_hash;
    std::string state_hash;
    ReplayState folded;  // 折到 source_seq 的账(续折从它起)

    nlohmann::json ToJson() const;
    static std::optional<ReplayCheckpoint> FromJson(const nlohmann::json& json);
};

// 落 checkpoint 文件:<session>/checkpoints/<stream_name>-<seq>.json(临时 +
// 原子 rename;同 seq 重写允许——同账同值幂等)。
std::expected<void, std::string> WriteReplayCheckpoint(const std::filesystem::path& session_dir,
                                                       const ReplayCheckpoint& checkpoint);

// 高水位恢复(§10.4 第 2 步):取 seq 最大、且 source_event_hash 与 Journal
// 第 seq 枚事件 hash 对得上、折叠账 last_event_hash 自洽的 checkpoint;对不上
// 便丢(返回 nullopt,调用方从头折叠)。stream 打不开也给 nullopt。
std::optional<ReplayCheckpoint> FindLatestUsableCheckpoint(const std::filesystem::path& session_dir,
                                                           const std::string& stream_name,
                                                           const std::filesystem::path& stream_path);

// 从 checkpoint 续折:折叠 stream_path 中 seq > checkpoint.source_seq 的事件
// 到 state(就地续写)。返回 false 时 error 说明读账失败;unsupported 照
// FoldStreamReplay 口径置 integrity。
bool ContinueFoldFrom(const std::filesystem::path& stream_path, ReplayState* state, std::string* error);

// 悬空工具清单(§10.4 第 4 步:started/finished/result 三道账给出明确状态;
// 未知副作用不可重跑)。
struct ReplayDanglingTool {
    std::string call_id;
    std::string tool_name;
    std::string state;  // planned_not_started | started_no_terminal | terminal_no_result
    bool unknown_side_effect = false;
};
std::vector<ReplayDanglingTool> CollectDanglingTools(const ReplayState& state);

// ---------------------------------------------------------------------------
// session verifier(§十四 verify:扫目录,逐文件验链,交叉核父子边)
// ---------------------------------------------------------------------------

// 单份 stream 的验账结果。
struct VerifiedStream {
    std::string relative_path;  // session 内相对路径(main.jsonl / subagents/…)
    bool ok = false;
    std::string error_code;
    std::string message;        // 人话补充(turn.* 一族核错的定位说明;空 = 无)
    std::string run_id;
    RunKind run_kind = RunKind::MainSession;
    std::string parent_run_id;  // 子账 run.started relations 报的 owner
    std::uint64_t events = 0;
    std::string last_event_hash;
    bool run_terminal = false;
    std::string terminal_kind;

    nlohmann::json ToJson() const;
};

// 一条父子边的交叉核对(§3.9 五条):
//   1. child owner 与 parent run 对得上;
//   2. 父账确有带 relations.child_run_id 的派发引用(spawn 边;前台派工
//      才有——子账 relations 带 parent_call_id 的是前台,不带的是后台
//      派工,P0-2 起后台不在父账落派发边,父轮早已收口);
//   3. 父接受结果时,child 已有终态;
//   4. child final hash 对得上(父侧记录值 vs 子文件实读值;后台派工父侧
//      留空,verifier 回填子文件实读值再核——P0-2 遗留#5 的跨文件核对);
//   5. 同一 child 结果至多接受一次。
struct ChildEdgeReport {
    std::string child_run_id;
    std::string parent_run_id;
    std::string parent_call_id;
    bool background_spawn = false;     // 子账 relations 无 parent_call_id(后台)
    bool child_stream_found = false;
    bool child_has_terminal = false;
    std::string child_terminal_hash;   // 子文件实读(后台回填就落这)
    std::string parent_recorded_hash;  // 父侧 result_ref.child_terminal_event_hash
    bool owner_matches = false;
    bool spawn_reference_found = false;
    bool dispatch_on_started = false;  // P0-2:派发引用是否落在 started 上(生产时序常落终态)
    bool hash_matches = false;    // 前台:父记 hash 必须相等;后台:核 child 有终态即过
    bool accepted_once = false;   // 恰一次(0 次未收 = 悬空,由 child terminal 标)
    std::string error_code;       // 空 = 过;child.* / edge.* 稳定码

    nlohmann::json ToJson() const;
};

struct SessionVerifyReport {
    bool ok = false;
    std::string error_code;  // 空 = 过;verify.* / edge.* 汇总码
    std::string message;
    std::vector<VerifiedStream> streams;
    std::vector<ChildEdgeReport> child_edges;
    std::map<std::string, std::string> run_kinds;  // run_id -> kind 名(查询缓存)
};

// 扫一间 session 目录:main + subagents/* + workflows/*/workflow.jsonl 与
// nodes/* + goals/* + loops/*,逐文件验 hash chain;再对每条父子边交叉核。
// 不调模型、不执行工具;`trajectory verify` 的引擎体。
SessionVerifyReport VerifySessionDir(const std::filesystem::path& session_dir);

}  // namespace lubancode::trajectory
