// 可追回 artifact(第二期)的布局实测驱动器:不进 ctest,手动跑——
//   artifact_layout_dump <输出根目录>
// 在给定目录开一场仓,卸两枚不同类型的长结果,把目录树与登记行打出来,
// 供验收记录"blob 布局实测"用(规格第二期"量内存、磁盘、恢复时间")。
#include <filesystem>
#include <iostream>
#include <string>

#include "agent/artifact_store.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "用法: artifact_layout_dump <输出根目录>\n";
        return 1;
    }
    const std::string root = std::string(argv[1]) + "/20260816-120000-demo/context";
    lubancode::agent::ContextArtifactStore store;
    if (!store.Open(root, "20260816-120000-demo")) {
        std::cerr << "开仓失败: " << root << "\n";
        return 1;
    }

    std::string log;
    for (int i = 1; i <= 2000; ++i) {
        log += "[12:00:" + std::to_string(i % 60) + "] build step " + std::to_string(i) +
               (i == 1533 ? " FAILED: undefined reference to `foo()'" : " ok") + "\n";
    }
    std::string markdown = "# 构建报告\n\n";
    for (int section = 1; section <= 20; ++section) {
        markdown += "## 第 " + std::to_string(section) + " 节\n";
        for (int i = 0; i < 40; ++i) {
            markdown += "- 条目 " + std::to_string(section) + "." + std::to_string(i) + ":内容照常\n";
        }
    }

    const auto first = store.Offload("toolu-demo-1", "run_command", log, 3);
    const auto second = store.Offload("toolu-demo-2", "read_file", markdown, 7);
    if (!first.has_value() || !second.has_value()) {
        std::cerr << "卸载失败\n";
        return 1;
    }

    std::cout << "root=" << root << "\n";
    for (const auto& ref : store.refs()) {
        std::cout << "artifact_id=" << ref.artifact_id << " tool=" << ref.tool_name << " bytes=" << ref.bytes
                  << " lines=" << ref.lines << " sha256=" << ref.sha256.substr(0, 12)
                  << " blob=" << ref.blob_path << " chunks=" << ref.chunk_index_path << "\n";
        std::cout << "  chunks=" << store.ChunksFor(ref).size() << "\n";
        const auto hits = store.Search(ref, ref.artifact_id == "a0001" ? "FAILED" : "第 3 节", 3);
        if (hits.has_value()) {
            for (const auto& hit : *hits) {
                std::cout << "  search hit: 行 " << hit.line << " (块 " << hit.chunk_id << ") " << hit.snippet
                          << "\n";
            }
        }
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        std::cout << "  tree: " << std::filesystem::relative(entry.path(), root).generic_string() << " ("
                  << (std::filesystem::is_regular_file(entry) ? std::filesystem::file_size(entry) : 0)
                  << " bytes)\n";
    }
    std::cout << "stats: artifacts=" << store.StatsOf().artifacts
              << " total_bytes=" << store.StatsOf().total_bytes << "\n";
    return 0;
}
