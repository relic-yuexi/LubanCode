#pragma once

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "workflow/account.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::workflow;

inline fs::path TempRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const fs::path dir =
        fs::temp_directory_path() / ("lubancode_wf_rt3_" + std::string(tag) + "_" +
                                     std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) +
                                     "_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

inline std::vector<std::string> SegmentLines(const fs::path& run_dir, const std::string& segment) {
    std::ifstream file(run_dir / "segments" / segment / "workflow.jsonl", std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

inline std::vector<nlohmann::json> SegmentEvents(const fs::path& run_dir, const std::string& segment) {
    std::vector<nlohmann::json> events;
    for (const auto& raw : SegmentLines(run_dir, segment)) {
        events.push_back(nlohmann::json::parse(raw));
    }
    return events;
}

// 按节点计数的假执行器:每个节点一档脚本,耗尽后重复最后一格。
class PerNodeExecutor : public NodeExecutor {
public:
    struct Step {
        bool ok = true;
        std::string error_code;
        nlohmann::json output = nlohmann::json::object();
        std::int64_t tokens = 0;
    };
    std::map<std::string, std::vector<Step>> script;
    std::map<std::string, int> calls;

    NodeExecResult Execute(const NodeExecRequest& request) override {
        const std::string id = request.node->id;
        calls[id] += 1;
        const auto it = script.find(id);
        if (it == script.end() || it->second.empty()) {
            NodeExecResult result;
            result.ok = true;
            result.output = nlohmann::json{{"echo", true}};
            return result;
        }
        const std::size_t index = std::min(static_cast<std::size_t>(calls[id]), it->second.size()) - 1;
        const Step& step = it->second[index];
        NodeExecResult result;
        result.ok = step.ok;
        result.error_code = step.error_code;
        result.output = step.output;
        result.tokens_used = step.tokens;
        return result;
    }
};

inline RuntimeOptions BaseOptions(const fs::path& account_root, std::shared_ptr<NodeExecutor> executor) {
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.executors[NodeKind::Template] = executor;
    options.account_root = account_root;
    options.run_id_generator = [] { return "run-acc"; };
    return options;
}

const char* kLinearYaml = R"YAML(
schema_version: 1
id: acc-linear
version: 1.0.0
entry: a
nodes:
  a: { type: transform, operation: echo }
  b: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: b }
  - { from: b, on: success, to: fin }
result:
  a: "${nodes.a.output}"
)YAML";

}  // namespace
