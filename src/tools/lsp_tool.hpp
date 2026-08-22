// lsp 工具:把 LSP 语义查询(定义/引用/文档符号/诊断)包成一个
// tools::Tool,挂进工具表给模型用。参数:
//   mode: definition / references / symbols / diagnostics
//   file: 必填,要查的文件路径
//   line / character: 定位类(definition/references)必填,1 基(人和编辑器
//     的习惯),内部转成 LSP 的 0 基
// 按扩展名路由到 config lsp 段配置的语言服务器(lsp::Manager 管懒启动/
// 闲置关停),结果格式化成人类可读文本。needs_confirm=false——只读查询,
// 不动任何文件。
//
// 格式化函数单独拎成纯函数(吃 LSP 响应 JSON,吐文本),好不起真服务器就
// 单测各种响应形状(Location/Location[]/LocationLink[]、DocumentSymbol
// 层级/SymbolInformation 平铺)。
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "lsp/manager.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 读某文件第 zero_based_line 行文本用的回调(格式化定义结果时要展示目标
// 行内容)。读不到给 nullopt,格式化层跳过展示,不报错。
using LspLineReader = std::function<std::optional<std::string>(const std::string& path, int zero_based_line)>;

// definition 响应 -> "文件:行:列" + 该行文本。认三种形状:单个 Location、
// Location 数组、LocationLink 数组(取 targetUri/targetSelectionRange);
// null/空数组 -> "没找到定义"。行列输出 1 基。
std::string FormatLspDefinition(const nlohmann::json& result, const LspLineReader& line_reader);

// references 响应 -> 列表,一行一个 "文件:行:列",超过 max_items 截断并
// 注明总数。null/空数组 -> "没找到引用"。
std::string FormatLspReferences(const nlohmann::json& result, std::size_t max_items = 50);

// documentSymbol 响应 -> 名字 + 种类 + 行,层级式(DocumentSymbol 带
// children 的缩进展示)和平铺式(SymbolInformation)都认。
std::string FormatLspSymbols(const nlohmann::json& result);

// 诊断缓存 -> 严重度 + 行 + 消息。nullopt(等了 2s 服务器也没推)->
// "暂无诊断...";空数组 -> 没有问题。
std::string FormatLspDiagnostics(const nlohmann::json& diagnostics);
std::string FormatLspDiagnostics(const std::optional<nlohmann::json>& diagnostics);

class LspTool : public Tool {
public:

    // 逐枚追踪单:LSP 请求只读远端档(不动本地文件)。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::ReadOnlyRemote; }
    explicit LspTool(lsp::Manager& manager);

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;

private:
    lsp::Manager& manager_;
};

}  // namespace lubancode::tools
