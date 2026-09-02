#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 写文件:UTF-8 写入,父目录不存在就自动建好。文件已存在就整个覆盖
// (结果里会注明"覆盖了原有文件")。执行前需要用户确认。
class WriteFileTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::LocalReversible; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::NonIdempotent; }
    lubancode::tools::RecoveryCapability recovery_capability() const override { return lubancode::tools::RecoveryCapability::ConditionallyUndoable; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    ApprovalClass approval_class() const override { return ApprovalClass::FileEdit; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
