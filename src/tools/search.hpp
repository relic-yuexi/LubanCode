#pragma once

#include <memory>

#include "tools/search_ripgrep.hpp"  // IRipgrepRunner:P0-2 装配注入口
#include "tools/tool.hpp"

namespace lubancode::tools {

// 项目内搜索,合成一个工具、按 mode 分两种玩法(理由见 search.cpp 顶部注释):
//   mode = "grep":按正则(ECMAScript 语法)搜文件内容,返回 文件:行号:行内容。
//   mode = "glob":按文件名通配找文件,返回相对路径列表。
// 两种模式共用同一套目录遍历/跳过规则(跳过 .git/、build/、node_modules/、
// 跳过二进制文件)。只读操作,不需要用户确认。
class SearchTool : public Tool {
public:
    // 默认构造:走内置 std::regex 内核(ripgrep 迁移 P0-4 之前的唯一生产
    // 路径,行为与从前一字不差)。
    SearchTool() = default;

    // ripgrep 迁移单 P0-2 的装配注入口:注入 ripgrep runner(生产装默认
    // BundledRipgrepRunner,单测装 fake)。P0-5 切主路之前 execute 不消费
    // 它——本口只是把"用什么后端"从工具内部挪到装配层,生产搜索行为不变。
    explicit SearchTool(std::shared_ptr<IRipgrepRunner> ripgrep_runner);

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::ReadOnlyLocal; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::Idempotent; }
    lubancode::tools::RecoveryCapability recovery_capability() const override { return lubancode::tools::RecoveryCapability::Retryable; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;

private:
    // P0-2 注入口持有的后端(可能为空 = 默认构造)。execute 暂不消费;
    // P0-5 切主路时这里是 BundledRipgrepRunner 的真调用点。
    std::shared_ptr<IRipgrepRunner> ripgrep_runner_;
};

}  // namespace lubancode::tools
