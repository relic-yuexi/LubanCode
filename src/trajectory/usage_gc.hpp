// Session 容量账与 derived-only GC(P0 新轨迹记录单 §12.2,P0-4)。
//
// 四笔容量:journal(canonical JSONL + 静态元数据)、referenced blobs
// (artifacts/ 全量——首版无跨 session CAS,session 内 blob 全算"仍被引用",
// 删除只走整间 session 的 /delete,不在这里扫引用图)、rebuildable indexes/
// checkpoints(indexes/ + checkpoints/,含 workflow 各自的 checkpoints/)、
// derived exports/drafts(derived/ + exports/)。
//
// GC 次序定死(§12.2):过期临时文件 -> 可重建 indexes -> 可重建
// checkpoints -> derived exports/drafts -> 停。canonical JSONL 与
// artifacts/ 下的 blob 永不进候选表——PlanSessionGc 压根不扫这两处,不是
// "扫到了但跳过",是候选生成阶段就没有这条路。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lubancode::trajectory {

struct SessionCapacityUsage {
    std::string session_id;
    std::uint64_t journal_bytes = 0;
    std::uint64_t journal_files = 0;
    std::uint64_t referenced_blob_bytes = 0;
    std::uint64_t referenced_blob_files = 0;
    std::uint64_t rebuildable_bytes = 0;
    std::uint64_t rebuildable_files = 0;
    std::uint64_t derived_bytes = 0;
    std::uint64_t derived_files = 0;

    std::uint64_t total_bytes() const {
        return journal_bytes + referenced_blob_bytes + rebuildable_bytes + derived_bytes;
    }
    std::uint64_t total_files() const {
        return journal_files + referenced_blob_files + rebuildable_files + derived_files;
    }
};

// 扫一间 session 目录,按四笔分类计字节数与文件数。纯只读扫描,不删任何
// 东西——扫描与 GC 决策分开,单测各自钉。session_dir 不存在给全零账。
SessionCapacityUsage ScanSessionCapacity(const std::filesystem::path& session_dir);

struct WorkspaceUsageReport {
    std::string workspace_key;
    std::vector<SessionCapacityUsage> sessions;  // 按 session_id 升序
    std::uint64_t total_bytes = 0;
};

// 扫一间 workspace 的 sessions/ 全部子目录(每个视作一场 session)。
WorkspaceUsageReport ScanWorkspaceUsage(const std::filesystem::path& sessions_dir,
                                        const std::string& workspace_key);

struct GcPlanItem {
    std::filesystem::path path;
    std::string category;  // temp | index | checkpoint | derived
    std::uint64_t bytes = 0;
};

struct GcPlan {
    std::vector<GcPlanItem> items;  // 按 §12.2 次序排好(temp -> index -> checkpoint -> derived)
    std::uint64_t reclaimable_bytes = 0;
};

// 列出可回收候选(不删)。canonical(main/subagents/workflows 的
// *.jsonl、goals/loops 的 *.jsonl、workflow definition.json、session.json、
// session.lock)与 artifacts/ 下的全部内容永不出现在候选表里。
GcPlan PlanSessionGc(const std::filesystem::path& session_dir);

enum class GcScope {
    DryRun,       // 只报账,不动文件系统(默认)
    DerivedOnly,  // 真删 PlanSessionGc 列出的候选(四类本身就是"可重建/派生物"的全集)
};

struct GcResult {
    GcPlan plan;
    bool applied = false;  // true = DerivedOnly 且真删过
    std::uint64_t deleted_bytes = 0;
    std::uint64_t deleted_files = 0;
    std::vector<std::string> errors;  // 逐条删除失败的路径+原因,不中断其余候选
};

GcResult RunSessionGc(const std::filesystem::path& session_dir, GcScope scope);

}  // namespace lubancode::trajectory
