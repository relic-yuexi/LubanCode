#include "trajectory/usage_gc.hpp"

#include <algorithm>

namespace lubancode::trajectory {
namespace {

// 崩溃留下的临时文件识别:两个已知命名法——blob_store.cpp 的
// "<hash>.tmp-<counter>",与其余原子写(session.json/checkpoint 等)常见
// 的 "*.tmp" 后缀。命中即"过期临时文件"候选,不论落在 session 目录的
// 哪个子树(含 artifacts/ 内部——已完成的 blob 文件名是纯 64 位 hex,不
// 含 ".tmp",两者不会误判)。
bool IsTempArtifact(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
        return true;
    }
    return name.find(".tmp-") != std::string::npos;
}

// 首段路径分量("artifacts"/"indexes"/"workflows"/... 或空 = 顶层文件)。
std::string TopComponent(const std::filesystem::path& relative) {
    if (relative.empty()) {
        return std::string();
    }
    return relative.begin()->string();
}

// workflows/<id>/checkpoints/** 也算可重建;workflows/<id>/ 下其余(
// workflow.jsonl、definition.json、nodes/*.jsonl)算 canonical journal 一族。
bool IsWorkflowCheckpointPath(const std::filesystem::path& relative) {
    auto it = relative.begin();
    if (it == relative.end() || it->string() != "workflows") {
        return false;
    }
    ++it;  // <workflow_run_id>
    if (it == relative.end()) {
        return false;
    }
    ++it;
    return it != relative.end() && it->string() == "checkpoints";
}

}  // namespace

SessionCapacityUsage ScanSessionCapacity(const std::filesystem::path& session_dir) {
    SessionCapacityUsage usage;
    usage.session_id = session_dir.filename().string();
    std::error_code ec;
    if (!std::filesystem::exists(session_dir, ec)) {
        return usage;
    }
    std::filesystem::recursive_directory_iterator it(
        session_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(it->file_size(file_ec));
        if (file_ec) {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(it->path(), session_dir, file_ec);
        if (file_ec) {
            continue;
        }
        const std::string top = TopComponent(relative);
        if (top == "artifacts") {
            usage.referenced_blob_bytes += bytes;
            ++usage.referenced_blob_files;
        } else if (top == "indexes" || top == "checkpoints" || IsWorkflowCheckpointPath(relative)) {
            usage.rebuildable_bytes += bytes;
            ++usage.rebuildable_files;
        } else if (top == "derived" || top == "exports") {
            usage.derived_bytes += bytes;
            ++usage.derived_files;
        } else {
            // main.jsonl / subagents/*.jsonl / workflows/*/{workflow.jsonl,
            // definition.json,nodes/*.jsonl} / goals/*.jsonl / loops/*.jsonl
            // / session.json / session.lock:canonical 与静态元数据同归一账。
            usage.journal_bytes += bytes;
            ++usage.journal_files;
        }
    }
    return usage;
}

WorkspaceUsageReport ScanWorkspaceUsage(const std::filesystem::path& sessions_dir,
                                        const std::string& workspace_key) {
    WorkspaceUsageReport report;
    report.workspace_key = workspace_key;
    std::error_code ec;
    if (!std::filesystem::exists(sessions_dir, ec)) {
        return report;
    }
    std::vector<std::filesystem::path> session_dirs;
    for (const auto& entry : std::filesystem::directory_iterator(
             sessions_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code dir_ec;
        if (entry.is_directory(dir_ec) && !dir_ec) {
            session_dirs.push_back(entry.path());
        }
    }
    std::sort(session_dirs.begin(), session_dirs.end());
    for (const auto& dir : session_dirs) {
        SessionCapacityUsage usage = ScanSessionCapacity(dir);
        report.total_bytes += usage.total_bytes();
        report.sessions.push_back(std::move(usage));
    }
    return report;
}

GcPlan PlanSessionGc(const std::filesystem::path& session_dir) {
    GcPlan plan;
    std::error_code ec;
    if (!std::filesystem::exists(session_dir, ec)) {
        return plan;
    }

    // §12.2 次序:temp -> index -> checkpoint -> derived。四段各自收集再
    // 拼接,保证候选表顺序稳定(GC 执行按这个次序删,先清临时再清可重建)。
    std::vector<GcPlanItem> temp_items, index_items, checkpoint_items, derived_items;

    std::filesystem::recursive_directory_iterator it(
        session_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(it->file_size(file_ec));
        if (file_ec) {
            continue;
        }
        const std::filesystem::path& path = it->path();

        if (IsTempArtifact(path)) {
            temp_items.push_back({path, "temp", bytes});
            continue;
        }

        const std::filesystem::path relative = std::filesystem::relative(path, session_dir, file_ec);
        if (file_ec) {
            continue;
        }
        const std::string top = TopComponent(relative);
        if (top == "artifacts") {
            continue;  // canonical blob:永不进候选表(已在上面把 .tmp 变体收走)
        }
        if (top == "indexes") {
            index_items.push_back({path, "index", bytes});
        } else if (top == "checkpoints" || IsWorkflowCheckpointPath(relative)) {
            checkpoint_items.push_back({path, "checkpoint", bytes});
        } else if (top == "derived" || top == "exports") {
            derived_items.push_back({path, "derived", bytes});
        }
        // 其余(main.jsonl/subagents/workflows 的 *.jsonl/definition.json/
        // goals/loops/session.json/session.lock)不进任何候选段:canonical
        // 与静态元数据,GC 永不碰。
    }

    for (auto* group : {&temp_items, &index_items, &checkpoint_items, &derived_items}) {
        for (auto& item : *group) {
            plan.reclaimable_bytes += item.bytes;
            plan.items.push_back(std::move(item));
        }
    }
    return plan;
}

GcResult RunSessionGc(const std::filesystem::path& session_dir, GcScope scope) {
    GcResult result;
    result.plan = PlanSessionGc(session_dir);
    if (scope == GcScope::DryRun) {
        return result;
    }
    result.applied = true;
    for (const auto& item : result.plan.items) {
        std::error_code ec;
        if (std::filesystem::remove(item.path, ec) && !ec) {
            result.deleted_bytes += item.bytes;
            ++result.deleted_files;
        } else {
            result.errors.push_back(item.path.string() + ": " + ec.message());
        }
    }
    return result;
}

}  // namespace lubancode::trajectory
