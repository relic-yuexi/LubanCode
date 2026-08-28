// 采集器实现:目录扫描(只读)+ 五路 adapter 的拼装。坏账跳过、计数留痕,
// 单路坏不拦其他路。

#include "evolution/collector.hpp"

#include "agent/tool_trace.hpp"
#include "evolution/adapters.hpp"
#include "platform/paths.hpp"
#include "runtime/tool_trace_hub.hpp"  // ToolTraceHub::BuildLedger(trace 行折叠)
#include "sessions/session_catalog.hpp"
#include "sessions/session_store.hpp"
#include "skills/workflow_recorder.hpp"
#include "workflow/journal.hpp"

namespace lubancode::evolution {

std::vector<EvolutionObservation> CollectObservations(const CollectSources& sources,
                                                      CollectReport* report) {
    std::vector<EvolutionObservation> observations;

    // ---- /record 录制件 ----
    if (!sources.recordings_root.empty()) {
        for (const skills::RecordingStatus& status :
             skills::ListRecordings(sources.recordings_root)) {
            if (report != nullptr && report->recordings_scanned >= sources.max_recordings) {
                break;
            }
            if (report != nullptr) {
                ++report->recordings_scanned;
            }
            if (!status.finished) {
                if (report != nullptr) {
                    ++report->recordings_skipped;
                }
                continue;
            }
            RecordingMaterial material;
            material.status = status;
            material.events = skills::ReadRecordingEvents(status.dir);
            for (EvolutionObservation& observation : ObservationsFromRecording(material)) {
                observations.push_back(std::move(observation));
            }
        }
    }

    // ---- Workflow runs ----
    if (!sources.workflow_runs_root.empty()) {
        std::size_t scanned = 0;
        for (const workflow::RunStatus& run : workflow::ListRuns(sources.workflow_runs_root)) {
            if (scanned >= sources.max_runs) {
                break;
            }
            ++scanned;
            if (report != nullptr) {
                ++report->runs_scanned;
            }
            const std::vector<workflow::JournalEvent> events =
                workflow::ReadJournalEvents(run.dir);
            for (EvolutionObservation& observation : ObservationsFromRun(run, events)) {
                observations.push_back(std::move(observation));
            }
        }
    }

    // ---- 会话档(goal_v1 族 + tool_trace_v1 族;消息正文一行不读)----
    if (!sources.sessions_dir.empty()) {
        sessions::SessionCatalog catalog(sources.sessions_dir);
        catalog.Scan();
        sessions::SessionQuery query;
        query.scope = sessions::SessionScope::All;
        query.state = sessions::SessionState::Active;
        query.sort = sessions::SessionSort::Updated;
        query.limit = sources.max_sessions;
        const sessions::SessionQueryPage page = catalog.Query(query);
        for (const sessions::SessionSummary& summary : page.entries) {
            if (report != nullptr) {
                ++report->sessions_scanned;
            }
            const auto bytes = sessions::ReadSessionFileBytes(summary.file_path);
            if (!bytes.has_value()) {
                if (report != nullptr) {
                    ++report->sessions_unreadable;
                }
                continue;
            }
            const auto loaded = sessions::ParseSessionFile(*bytes);
            if (!loaded.has_value()) {
                if (report != nullptr) {
                    ++report->sessions_unreadable;
                }
                continue;
            }
            for (EvolutionObservation& observation :
                 ObservationsFromGoalEvents(summary.file_path, loaded->goal_events)) {
                observations.push_back(std::move(observation));
            }
            const agent::ToolExecutionLedger ledger =
                runtime::ToolTraceHub::BuildLedger(loaded->tool_trace_events);
            for (EvolutionObservation& observation :
                 ObservationsFromToolTrace(summary.file_path, ledger)) {
                observations.push_back(std::move(observation));
            }
        }
    }

    // ---- Memory(已接受条目,命令层按层喂进)----
    for (const MemoryLayer& layer : sources.memory_layers) {
        if (report != nullptr) {
            report->memory_entries += layer.entries.size();
        }
        for (EvolutionObservation& observation :
             ObservationsFromMemory(layer.entries, layer.layer_label, layer.dir_utf8)) {
            observations.push_back(std::move(observation));
        }
    }

    if (report != nullptr) {
        report->observations = observations.size();
    }
    return observations;
}

}  // namespace lubancode::evolution
