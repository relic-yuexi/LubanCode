#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 对已有文件做字符串替换:先逐字精确找；失败后有限度兼容换行、统一
// 缩进与行尾空白。任何一层出现多处且没开 replace_all 都拒绝猜测。
class EditFileTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::LocalReversible; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::NonIdempotent; }
    lubancode::tools::RecoveryCapability recovery_capability() const override { return lubancode::tools::RecoveryCapability::ConditionallyUndoable; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
