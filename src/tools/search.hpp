#pragma once

#include <memory>

#include "tools/search_ripgrep.hpp"  // IRipgrepRunner:P0-2 装配注入口
#include "tools/tool.hpp"

namespace lubancode::tools {

// 项目内搜索,合成一个工具、按 mode 分两种玩法(理由见 search.cpp 顶部注释):
//   mode = "grep":按正则(Rust regex 语法,ripgrep 同款)搜文件内容,返回 文件:行号:行内容。
//   mode = "glob":按文件名通配(ripgrep globset 语法)找文件,返回相对路径列表。
// 两种模式共用同一套策略(遵守 ignore 文件、硬排除 .git/build/node_modules/
// .evidence、跳过二进制)与同一条后端执行路(随包 ripgrep,设计单迁移 P0-5
// 切主路后唯一一条,无 fallback)。只读操作,不需要用户确认。
class SearchTool : public Tool {
public:
    // 默认构造即生产装配:持随包 BundledRipgrepRunner(定位只认
    // exe-dir/libexec,缺件即稳定错 search_backend_missing,不退本地内核)。
    SearchTool();

    // 装配注入口:生产传默认 runner(默认构造已带),单测传 fake。
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
    // 执行后端(P0-5 起唯一生产路)。execute 之外不许挪用/置空。
    std::shared_ptr<IRipgrepRunner> ripgrep_runner_;
};

}  // namespace lubancode::tools
