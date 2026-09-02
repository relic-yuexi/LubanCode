// 采集器实现:目录扫描(只读)+ 五路 adapter 的拼装。坏账跳过、计数留痕,
// 单路坏不拦其他路。

#include "evolution/collector.hpp"

#include "agent/tool_trace.hpp"
#include "evolution/adapters.hpp"
#include "platform/paths.hpp"
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

    // ---- 会话观察 ----
    // P0-6:旧平铺会话档(sessions/*.jsonl 的 goal_v1/tool_trace_v1 抽取)已删
    // ——goal 与工具追踪的持久账都在 workspace trajectory Journal,evolve 接
    // 新账属自进化单的后续批次,这里不另写第二条扫盘路。


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
