// 组件名称索引的轻合同(架构审查 SV-10):"按 ComponentKind 查/插本地组件
// 名"的统一存储机械。原先三处各写一份 set + switch——PackageComponentSet
// 的 Has、catalog 与 mounting 各自的 OwnComponents——如今共用这一份,
// kind -> local id 的 membership 语义只此一处,新增组件种类不再抄第三份
// switch。
//
// 合并边界(审查定案):只合并存储机械,不合并各阶段的数据与过滤策略。
// 跨包引用索引(PackageComponentSet)带包身份,轻扫目录而来;catalog 的
// 本包账解析阶段攒,坏组件也在(引用指得到,账上就有名);mounting 只折
// 四类内容组件,plugin/mcp/channel 不折 canonical 短名。各阶段自己构建
// 自己的值,不共用跨阶段可变缓存。
#pragma once

#include <array>
#include <cstddef>
#include <set>
#include <string>

#include "package/component.hpp"

namespace lubancode::package {

// 七类组件的档位表。新增组件种类时在 component.hpp 添枚举档并补这里一
// 枚;下面的 static_assert 挡住漏补。
inline constexpr std::array<ComponentKind, 7> kAllComponentKinds{
    ComponentKind::Agent,    ComponentKind::PromptProfile, ComponentKind::Skill,
    ComponentKind::Workflow, ComponentKind::Plugin,        ComponentKind::McpServer,
    ComponentKind::Channel,
};

static_assert(kAllComponentKinds.size() == static_cast<std::size_t>(ComponentKind::Channel) + 1,
              "kAllComponentKinds 落后于 ComponentKind:新增种类须补一枚档位");

struct ComponentNameIndex {
    // 档位数跟 ComponentKind 声明序(隐式 0..N-1)走,新枚举档自动长一档。
    static constexpr std::size_t kSlotCount = static_cast<std::size_t>(ComponentKind::Channel) + 1;

    std::array<std::set<std::string>, kSlotCount> names;

    bool Has(ComponentKind kind, const std::string& local_id) const {
        const std::size_t slot = static_cast<std::size_t>(kind);
        return slot < kSlotCount && names[slot].count(local_id) > 0;
    }

    void Add(ComponentKind kind, const std::string& local_id) {
        const std::size_t slot = static_cast<std::size_t>(kind);
        if (slot < kSlotCount) names[slot].insert(local_id);
    }
};

}  // namespace lubancode::package
