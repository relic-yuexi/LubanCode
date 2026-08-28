// 自进化闭环阶段 1:五路只读 adapter——各家账本 -> EvolutionObservation。
//
// 输入口径(逐家钉死;账本没有的来源类型不硬造):
//   recording  skills::RecordingStatus + skills::ReadRecordingEvents 的事件流。
//              只收 finished 的录制件(有 record_stop);目标口述、变量名
//              (不是值)、验收、最后一次验证、工具名与输入形状(键名)。
//              未录完的不收——半截示范不是可复用做法。
//   run        workflow::RunStatus + workflow::ReadJournalEvents 的事件流。
//              workflow id/version/content_hash(manifest 簿记)、终态、节点
//              序列、重试次数。cwd 不进观察(run 档里有,观察不需要)。
//   goal       sessions::GoalSessionEvent 流(session 存档的 goal_v1 族)。
//              objective(最新 revision)、iteration 数、evidence 数与种类、
//              最后一次 evaluation 的 decision 与 summary(终点判词是
//              evaluator 的结构化判词,不是模型长篇思考原文)。消息正文
//              一处不读。
//   tooltrace  agent::ToolExecutionLedger(session 存档的 tool_trace_v1 折叠)。
//              只收"明确失败"的 execution:has_finished 且 outcome 不是
//              Succeeded 且不是两档 cancelled(被用户收掉是会话事实,不是
//              可复用的失败路)。工具名、outcome、error_code、duration、
//              结果字节数。账本没有输入原文(只有 effective_input_sha256,
//              它是内容指纹,进观察会让同类聚合失配,不收)。
//   memory     memory::MemoryEntry 列表(命令层用 ProjectMemory::ListEntries
//              喂进来——已接受的正式库条目;候选审阅箱的未审件不属于
//              "已接受",不收)。只收 status=active。kind/title/summary/
//              confidence/keywords/scope;正文留在主题文件里,证据引用指回。
//
// TODO(user_feedback): 用户当场说出的纠正/批准/拒绝,首版没有独立账本
// (散在会话消息正文里;Memory 的 feedback 条目只覆盖已被采纳的一小部分)。
// 等后续单子立了纠正账,再补第六路 adapter;这里不硬造来源。
//
// 全部纯函数:不碰磁盘、不发请求、不改任何一家的账。
#pragma once

#include <string>
#include <vector>

#include "agent/tool_trace.hpp"
#include "evolution/observation.hpp"
#include "memory/project_memory.hpp"
#include "sessions/goal_session.hpp"
#include "skills/workflow_recorder.hpp"
#include "workflow/journal.hpp"

namespace lubancode::evolution {

// ---------------------------------------------------------------------------
// recording:/record 的录制件
// ---------------------------------------------------------------------------

// status 与 events 分开递:status 是目录盘点(ListRecordings 的产物),
// events 是整读的事件流(ReadRecordingEvents)。两者由调用方对齐同一目录。
struct RecordingMaterial {
    skills::RecordingStatus status;
    std::vector<skills::RecordEvent> events;
};

std::vector<EvolutionObservation> ObservationsFromRecording(const RecordingMaterial& material);

// ---------------------------------------------------------------------------
// run:Workflow 的一场 run
// ---------------------------------------------------------------------------

std::vector<EvolutionObservation> ObservationsFromRun(const workflow::RunStatus& run,
                                                      const std::vector<workflow::JournalEvent>& events);

// ---------------------------------------------------------------------------
// goal:session 存档里的 goal_v1 事件族(一场会话可有多只 goal,逐只出观察)
// ---------------------------------------------------------------------------

// session_file_utf8:存档文件路径(证据引用与 source_ref 用)。
std::vector<EvolutionObservation> ObservationsFromGoalEvents(
    const std::string& session_file_utf8, const std::vector<sessions::GoalSessionEvent>& events);

// ---------------------------------------------------------------------------
// tooltrace:session 存档的 tool_trace_v1 折叠账
// ---------------------------------------------------------------------------

std::vector<EvolutionObservation> ObservationsFromToolTrace(
    const std::string& session_file_utf8, const agent::ToolExecutionLedger& ledger);

// ---------------------------------------------------------------------------
// memory:项目/用户两层已接受条目
// ---------------------------------------------------------------------------

// layer_label:"project"/"user"(进 details,两层同 id 条目分得开);
// dir_utf8:该层 memory 目录(证据引用的前缀)。
std::vector<EvolutionObservation> ObservationsFromMemory(const std::vector<memory::MemoryEntry>& entries,
                                                         const std::string& layer_label,
                                                         const std::string& dir_utf8);

}  // namespace lubancode::evolution
