// 自进化闭环阶段 1:采集器——扫五路账本,产观察。只读各家账(不改一行),
// 产物交给 ObservationStore 落账。不生成 Package、不决定晋升(契约)。
//
// 扫描口径(上限防大目录拖慢命令;P0-6 起旧会话档路已删,goal/trace 走新账属后续批次):
//   recordings_root  ListRecordings 倒序取最近 max_recordings 件(只收
//                    finished 的,半截件跳过并计数);
//   workflow_runs_root  ListRuns 倒序取最近 max_runs 场;
//   memory_entries   由调用方喂(ProjectMemory::ListEntries/ListUserEntries
//                    的已接受条目)——采集器不碰 ProjectMemory 对象本身,
//                    免得把授权/开关的判断散到第二处。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "evolution/observation.hpp"
#include "memory/project_memory.hpp"

namespace lubancode::evolution {

// Memory 的一层(项目层/用户层各一份;两层同 id 条目靠 layer 分开)。
struct MemoryLayer {
    std::vector<memory::MemoryEntry> entries;  // 已接受条目(命令层喂)
    std::string layer_label;                   // "project" / "user"
    std::string dir_utf8;                      // 该层目录(证据引用前缀)
};

struct CollectSources {
    std::filesystem::path recordings_root;    // <home>/.lubancode/recordings;空 = 不扫
    std::filesystem::path workflow_runs_root; // <home>/.lubancode/workflow-runs;空 = 不扫
    std::vector<MemoryLayer> memory_layers;   // 已接受条目(命令层喂;授权与开关的判断在 ProjectMemory 一处)

    std::size_t max_recordings = 20;
    std::size_t max_runs = 20;
};

struct CollectReport {
    std::size_t recordings_scanned = 0;   // ListRecordings 里过的件数(含半截)
    std::size_t recordings_skipped = 0;   // 未 finished 跳过的件数
    std::size_t runs_scanned = 0;
    std::size_t memory_entries = 0;
    std::size_t observations = 0;         // 五路合计产出的观察数
};

// 扫描 + 五路 adapter。IO 只读;任何一路坏档都跳过计数,不抛、不半途而废。
std::vector<EvolutionObservation> CollectObservations(const CollectSources& sources,
                                                      CollectReport* report = nullptr);

}  // namespace lubancode::evolution
