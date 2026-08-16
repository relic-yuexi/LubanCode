#include "agent/model_router.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::agent {

std::string ToString(ModelRole role) {
    switch (role) {
        case ModelRole::Cheap:
            return "cheap";
        case ModelRole::Normal:
            return "normal";
        case ModelRole::Lao:
            return "lao";
    }
    return "normal";
}

std::string ToString(TaskKind kind) {
    switch (kind) {
        case TaskKind::NormalTurn:
            return "普通对话";
        case TaskKind::Plan:
            return "计划";
        case TaskKind::Compact:
            return "全局压缩";
        case TaskKind::CompactRepair:
            return "压缩修补";
        case TaskKind::Microcompact:
            return "微压缩";
        case TaskKind::MemoryExtract:
            return "记忆抽取";
        case TaskKind::RetrievalExpansion:
            return "检索扩词";
        case TaskKind::Classification:
            return "低风险分类";
        case TaskKind::SessionTitle:
            return "会话标题";
        case TaskKind::ResumeSummary:
            return "resume 摘要";
    }
    return "普通对话";
}

ModelRole DefaultRoleForTask(TaskKind kind) {
    switch (kind) {
        case TaskKind::Plan:
            return ModelRole::Lao;
        case TaskKind::NormalTurn:
        case TaskKind::CompactRepair:
            return ModelRole::Normal;
        case TaskKind::Compact:
        case TaskKind::Microcompact:
        case TaskKind::MemoryExtract:
        case TaskKind::RetrievalExpansion:
        case TaskKind::Classification:
        case TaskKind::SessionTitle:
        case TaskKind::ResumeSummary:
            return ModelRole::Cheap;
    }
    return ModelRole::Normal;
}

namespace {

// spec 折成 route:provider 空则继承活跃 provider(规格"provider 留空便
// 继承 active provider")。
ModelRoute SpecToRoute(const ModelRoleSpec& spec, const std::string& active_provider) {
    ModelRoute route;
    route.provider = spec.provider.empty() ? active_provider : spec.provider;
    route.model = spec.model;
    route.effort = spec.effort;
    route.context_window = spec.context_window;
    route.max_output_tokens = spec.max_output_tokens;
    route.source = spec.source;
    return route;
}

}  // namespace

const ModelRoute& ModelRouteTable::RoleRoute(ModelRole role) const {
    switch (role) {
        case ModelRole::Cheap:
            return cheap;
        case ModelRole::Normal:
            return normal;
        case ModelRole::Lao:
            return lao;
    }
    return normal;
}

ModelRoute ModelRouteTable::RouteFor(TaskKind kind) const {
    const ModelRole role = DefaultRoleForTask(kind);
    // compact_model 兼容别名:只顶替压缩类任务(Compact/Microcompact),
    // 记忆抽取、标题、resume 摘要照走 cheap 角色的有效值——旧字段影响面
    // 不悄悄扩大(规格"调用点收拢"节)。
    if (compact_legacy_override.has_value() &&
        (kind == TaskKind::Compact || kind == TaskKind::CompactRepair)) {
        return *compact_legacy_override;
    }
    return RoleRoute(role);
}

ModelRouteTable ResolveModelRoutes(const ModelRoleSpec& normal_spec, const ModelRoleSpec& cheap_spec,
                                   const ModelRoleSpec& lao_spec, const std::string& session_model,
                                   const std::string& active_provider) {
    ModelRouteTable table;

    // normal:配了用配置;没配沿会话模型(来源"当前会话")。
    if (normal_spec.configured()) {
        table.normal = SpecToRoute(normal_spec, active_provider);
    } else {
        table.normal.model = session_model;
        table.normal.provider = active_provider;
        table.normal.source = "当前会话";
    }

    // cheap/lao:配了用配置;没配整体回落 normal(来源写明回落,不重印
    // 同名让用户猜)。
    const auto resolve_or_fallback = [&](const ModelRoleSpec& spec, ModelRoute& out) {
        if (spec.configured()) {
            out = SpecToRoute(spec, active_provider);
            return;
        }
        out = table.normal;
        out.fell_back_to_normal = true;
        out.source = "回落到 normal(" + table.normal.source + ")";
    };
    resolve_or_fallback(cheap_spec, table.cheap);
    resolve_or_fallback(lao_spec, table.lao);

    return table;
}

void ModelUsageLedger::Record(ModelRole role, std::string_view model, const api::Usage& usage,
                              std::int64_t duration_ms, bool reported) {
    auto& entry = by_role_[role];
    entry.calls += 1;
    entry.duration_ms += duration_ms > 0 ? duration_ms : 0;
    entry.last_model = std::string(model);
    if (reported) {
        entry.reported = true;
        entry.input_tokens +=
            usage.input_tokens + usage.cache_read_tokens + usage.cache_creation_tokens;
        entry.output_tokens += usage.output_tokens;
    }
}

void ModelUsageLedger::RecordFallback(TaskKind kind, ModelRole from, ModelRole to, std::string reason) {
    if (from == to) {
        return;
    }
    fallback_notes_.push_back(ToString(from) + " 不可用," + ToString(kind) + "已回落 " + ToString(to) +
                              (reason.empty() ? std::string() : ": " + reason));
}

std::vector<std::string> ModelUsageLedger::ReportLines() const {
    std::vector<std::string> lines;
    // 固定按 cheap -> normal -> lao 出账,顺序稳定好对照 /model roles。
    const ModelRole order[] = {ModelRole::Cheap, ModelRole::Normal, ModelRole::Lao};
    for (const ModelRole role : order) {
        const auto it = by_role_.find(role);
        if (it == by_role_.end() || it->second.empty()) {
            continue;
        }
        const ModelUsageEntry& entry = it->second;
        std::string line = ToString(role) + " · " + entry.last_model + " · " + std::to_string(entry.calls) + " 次调用";
        if (entry.reported) {
            line += " · 输入 " + std::to_string(entry.input_tokens) + " tok · 输出 " +
                    std::to_string(entry.output_tokens) + " tok";
        } else {
            line += " · usage 未报告";
        }
        if (entry.duration_ms > 0) {
            line += " · 用时 " + std::to_string(entry.duration_ms / 1000) + "." +
                    std::to_string((entry.duration_ms % 1000) / 100) + "s";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace lubancode::agent
