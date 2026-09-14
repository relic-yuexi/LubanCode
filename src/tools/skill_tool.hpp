// M9:内置 "skill" 工具——模型按名字加载一份已扫描到的技能,拿到 SKILL.md
// 的完整正文。
//
// 应用Worker接入单 §六(P2 后续批)在"按需读正文"的口上挂三道受控面:
//   - 漂移校验:扫描时刻记下 SKILL.md 全文哈希(SkillMeta::content_hash),
//     每次读时重算对照,同场中途改文件即拒读(错误码 skill.drifted)——
//     清单与正文永远同源,不许悄悄读到修改版;
//   - 受控资源读取:可选 path 参数读技能目录内的相对引用材料——路径
//     规范化(拒绝对路径/..越根/UNC/盘符)、链接绕过检查(解析后须仍落在
//     技能目录内)、外链(http/file 一类 URL)不自动 fetch,只回数据源
//     待处理的明示错误;
//   - 依赖声明消费:SKILL.md frontmatter 的 requires-tools 只作声明,加载
//     时对照本场冻结工具面(构造时递入),缺获准执行工具回
//     capability_unavailable,不为满足技能文字自动挂 shell/装包。
#pragma once

#include <optional>
#include <set>
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
    // available_tools:本场冻结的工具面(注册表 wire 名,如 mcp__srv__echo、
    // run_command)。有值时消费 requires-tools 依赖声明——声明的工具不在
    // 面上,加载即回 capability_unavailable(§六"依赖只声明,不自动授予";
    // headless 装配递入,终端路缺省不执法——终端是全量注册+运行时审批,
    // 与本合同不同形,如实分家)。
    explicit SkillTool(std::vector<SkillMeta> skills,
                       std::optional<std::set<std::string>> available_tools = std::nullopt)
        : skills_(std::move(skills)), available_tools_(std::move(available_tools)) {}

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
    // §六 受控资源读取的实现体:skill_dir 内相对路径 -> 只读正文。失败
    // 回人话(越根/链接绕过/外链/打不开/超帽各说各的),content_out 装
    // 正文。静态:不碰 skills_。
    static Result ReadSkillResource(const SkillMeta& meta, const std::string& relative_path);

    std::vector<SkillMeta> skills_;
    std::optional<std::set<std::string>> available_tools_;
};

}  // namespace lubancode::tools
