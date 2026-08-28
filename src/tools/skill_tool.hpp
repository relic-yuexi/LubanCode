// M9:内置 "skill" 工具——模型按名字加载一份已扫描到的技能,拿到 SKILL.md
// 的完整正文。
#pragma once

#include <vector>

#include "tools/skill_loader.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 清单在启动时(main.cpp)扫描好,构造时整份传进来,执行期间不重新扫描
// 磁盘——跟 read_file 这类"每次执行都真读一次磁盘"的工具不一样,是因为
// 技能列表本来就要先出现在系统提示词里(哪些技能存在),构造 SkillTool
// 时用的是同一份扫描结果,两边保证一致。
class SkillTool : public Tool {
public:
    explicit SkillTool(std::vector<SkillMeta> skills) : skills_(std::move(skills)) {}

    void SetSkills(std::vector<SkillMeta> skills) { skills_ = std::move(skills); }

    // 逐枚追踪单:加载技能只读 SKILL.md、把说明装进上下文,不落盘不改
    // 状态(真机实测 P2-3:Plan 模式按只读放行,靠的就是这档声明)。
    lubancode::tools::EffectClass effect_class() const override {
        return lubancode::tools::EffectClass::ReadOnlyLocal;
    }
    lubancode::tools::Idempotency idempotency() const override {
        return lubancode::tools::Idempotency::Idempotent;
    }
    lubancode::tools::RecoveryCapability recovery_capability() const override {
        return lubancode::tools::RecoveryCapability::Retryable;
    }

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;

private:
    std::vector<SkillMeta> skills_;
};

}  // namespace lubancode::tools
