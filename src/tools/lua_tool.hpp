// M7:Lua 插件。扫 <主目录>/.lubancode/plugins/*.lua,每个文件是一个工具:
// 脚本执行后要返回一张表——
//
//   return {
//     name = "工具名",
//     description = "一句话说明",
//     input_schema = <JSON 字符串,或等价的 lua 表>,
//     execute = function(input) ... return result_string end,
//   }
//
// 每个工具一个独立 lua_State(互相隔离,谁也污染不了谁的全局环境)。
// execute 时把模型给的入参 JSON 手工转成 lua 表传进去(nlohmann 遍历,
// 不引第三方绑定库),返回值字符串化:字符串原样收,数字/布尔 tostring,
// 表转回 JSON 文本;lua 侧 error() 用 pcall 接住,报 is_error。
// 工具名前缀 plugin__<文件名>__,needs_confirm 恒真(外部代码一律先问)。
#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tools/tool.hpp"

struct lua_State;

namespace lubancode::tools {

class LuaTool : public Tool {
public:
    // 从脚本文本加载(单测直捣这里,不用真落盘)。stem 是文件名去掉扩展名,
    // 拼工具名前缀用。脚本跑不起来 / 返回值不是表 / 缺 name / 缺 execute /
    // input_schema 不是合法 JSON,都返回一条人话错误。
    static std::expected<std::unique_ptr<LuaTool>, std::string> LoadFromScript(
        const std::string& script, const std::string& stem);

    // 从磁盘文件加载(按二进制原样读,UTF-8 中文不过手不转码)。
    static std::expected<std::unique_ptr<LuaTool>, std::string> LoadFromFile(
        const std::filesystem::path& file);

    ~LuaTool() override;
    LuaTool(const LuaTool&) = delete;
    LuaTool& operator=(const LuaTool&) = delete;

    std::string name() const override;         // plugin__<文件名>__<表里的 name>
    std::string description() const override;  // [plugin:<文件名>] 前缀
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Result execute(const nlohmann::json& input) override;

    const std::string& stem() const { return stem_; }

private:
    LuaTool() = default;

    lua_State* lua_ = nullptr;
    int execute_ref_ = -1;  // execute 函数在 lua 注册表里的引用
    std::string stem_;
    std::string full_name_;
    std::string description_;
    nlohmann::json schema_;
};

// 目录扫描的产物:装好的工具 + 每个坏文件一条警告。
struct LuaScanResult {
    std::vector<std::unique_ptr<LuaTool>> tools;
    std::vector<std::string> warnings;
};

// 扫 dir 下的 *.lua(不递归)。目录不存在返回空(插件本来就是可选的);
// 单个文件坏了(语法错、缺字段……)跳过并写一条警告,不连累其余。
// 结果按文件名排序,顺序稳定。
LuaScanResult LoadLuaPlugins(const std::filesystem::path& dir);

}  // namespace lubancode::tools
