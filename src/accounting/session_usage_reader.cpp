#include "accounting/session_usage_reader.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "accounting/usage_projector.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"

namespace lubancode::accounting {
namespace {

// 目录存在与否(undefined/不存在都算没有)。
bool DirExists(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
}

// 一条 stream 的全部信封(按行序)。空文件给空表(合法:占位未写)。
// 坏行/坏信封 → nullopt,调用方点名,不出半条账。
std::optional<std::vector<trajectory::EventEnvelope>> ParseStream(
    const std::filesystem::path& path) {
    const auto lines = trajectory::ReadJournalLines(path);
    if (!lines.has_value()) {
        return std::nullopt;
    }
    std::vector<trajectory::EventEnvelope> envelopes;
    envelopes.reserve(lines->size());
    for (const auto& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            return std::nullopt;
        }
        trajectory::EventEnvelope envelope;
        if (trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            return std::nullopt;
        }
        envelopes.push_back(std::move(envelope));
    }
    return envelopes;
}

}  // namespace

std::optional<std::vector<std::filesystem::path>> ListSessionStreams(
    const std::filesystem::path& session_dir) {
    if (!DirExists(session_dir)) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> streams;
    const auto push_if_file = [&](const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            streams.push_back(path);
        }
    };
    // main.jsonl
    push_if_file(session_dir / "main.jsonl");
    // subagents/<agent_run_id>.jsonl(嵌套同目录,§3.5)
    std::error_code ec;
    const std::filesystem::path subagents = session_dir / "subagents";
    if (std::filesystem::is_directory(subagents, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(subagents, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
                streams.push_back(entry.path());
            }
        }
    }
    // workflows/<run>/{workflow.jsonl, nodes/*.jsonl}(§3.6)
    const std::filesystem::path workflows = session_dir / "workflows";
    if (std::filesystem::is_directory(workflows, ec)) {
        for (const auto& run : std::filesystem::directory_iterator(workflows, ec)) {
            if (!run.is_directory()) {
                continue;
            }
            push_if_file(run.path() / "workflow.jsonl");
            const std::filesystem::path nodes = run.path() / "nodes";
            if (std::filesystem::is_directory(nodes, ec)) {
                for (const auto& node : std::filesystem::directory_iterator(nodes, ec)) {
                    if (node.is_regular_file() && node.path().extension() == ".jsonl") {
                        streams.push_back(node.path());
                    }
                }
            }
        }
    }
    std::sort(streams.begin(), streams.end());
    return streams;
}

SessionUsageRead ReadSessionUsage(const std::filesystem::path& session_dir) {
    SessionUsageRead result;
    const auto streams = ListSessionStreams(session_dir);
    if (!streams.has_value()) {
        result.error_code = "usage.session_not_found";
        result.message = "没有这场 session:" + session_dir.string();
        result.session_id = session_dir.filename().string();
        return result;
    }
    // session.json:静态材料。读不到不拦账(旧目录/半路拷贝),status 写
    // unknown,封口与否调用方按 provisional 处理。
    if (const auto manifest = trajectory::ReadSessionJson(session_dir)) {
        result.session_id = manifest->session_id;
        result.workspace_key = manifest->workspace_key;
        result.status = manifest->status;
    } else {
        result.status = "unknown";
        result.warnings.push_back("usage.session_manifest_missing: " + session_dir.string());
    }

    for (const auto& path : *streams) {
        auto envelopes = ParseStream(path);
        if (!envelopes.has_value()) {
            result.warnings.push_back("usage.stream_unreadable: " + path.filename().string());
            continue;
        }
        UsageProjection projection = ProjectUsage(*envelopes);
        if (!projection.ok) {
            result.warnings.push_back("usage.stream_rejected: " + path.filename().string() + ": " +
                                      projection.error_code);
            continue;
        }
        for (const auto& warning : projection.warnings) {
            result.warnings.push_back(warning);
        }
        for (auto& sample : projection.samples) {
            if (result.workspace_key.empty()) {
                result.workspace_key = sample.workspace_key;
            }
            if (result.session_id.empty()) {
                result.session_id = sample.session_id;
            }
            result.samples.push_back(std::move(sample));
        }
    }
    // session.json 与 sample 都没给名字(全空 stream 的光杆目录):目录名兜底,
    // 报告至少有个可指认的场次名。
    if (result.session_id.empty()) {
        result.session_id = session_dir.filename().string();
    }
    result.ok = true;
    return result;
}

}  // namespace lubancode::accounting
