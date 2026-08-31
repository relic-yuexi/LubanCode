#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 项目内搜索,合成一个工具、按 mode 分两种玩法(理由见 search.cpp 顶部注释):
//   mode = "grep":按正则(ECMAScript 语法)搜文件内容,返回 文件:行号:行内容。
//   mode = "glob":按文件名通配找文件,返回相对路径列表。
// 两种模式共用同一套目录遍历/跳过规则(跳过 .git/、build/、node_modules/、
// 跳过二进制文件)。只读操作,不需要用户确认。
class SearchTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::ReadOnlyLocal; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::Idempotent; }
    lubancode::tools::RecoveryCapability recovery_capability() const override { return lubancode::tools::RecoveryCapability::Retryable; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;
};

}  // namespace lubancode::tools
