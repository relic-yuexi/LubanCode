#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 读文件,按行号显示(cat -n 风格)。支持 offset/limit 只读一部分。
// 路径按 UTF-8 处理,正斜杠反斜杠都认,中文路径也不会乱码。
class ReadFileTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明(只读本地,可建议重试)。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::ReadOnlyLocal; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::Idempotent; }
    lubancode::tools::RecoveryCapability recovery_capability() const override {
        return lubancode::tools::RecoveryCapability::Retryable;
    }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
