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
//
// plugins 单第 4 步:profile 分级(pure 缺省 / trusted 全开)。pure 关
// os.execute、os.exit、io、package.loadlib;instruction hook 设 CPU 指令
// 预算 + ESC 取消检查;自定义 allocator 设内存帽。三道墙都是软的:Lua
// 层的 luaL_error 路子,拦的是跑野的脚本,不是恶意绕洞——真不可信代码
// 走 process 隔离,那是另一条合同。
#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tools/tool.hpp"

struct lua_State;

namespace lubancode::tools {

// LuaTool 的运行时账(指令/内存/取消):定义在 .cpp(躲 lua 头),这里
// 只前向声明给 unique_ptr 用。
struct LuaGuard;

// Lua 插件的运行画像(plugins 单「核心定案」A 节)。
struct LuaProfile {
    enum class Level {
        Pure,     // 缺省:关 os.execute/os.exit/io/package.loadlib
        Trusted,  // 显式批准后全开(legacy 行为)
    };
    Level level = Level::Pure;

    // 指令预算:0 = 不设。hook 每 kHookStride 条指令走一次,到预算即
    // luaL_error("cpu 预算耗尽")。
    std::uint64_t instruction_budget = 200'000'000;  // 约 0.2~2s 量级的脚本计数
    // 内存帽(字节,allocator 记账口径):0 = 不设。超帽分配返回 NULL,
    // Lua 按 OOM 报错,不拖垮宿主堆。
    std::size_t memory_cap_bytes = 256 * 1024 * 1024;

    static LuaProfile PureDefault();
    static LuaProfile TrustedDefault();
};

class LuaTool : public Tool {
public:
    // 从脚本文本加载(单测直捣这里,不用真落盘)。stem 是文件名去掉扩展名,
    // 拼工具名前缀用。脚本跑不起来 / 返回值不是表 / 缺 name / 缺 execute /
    // input_schema 不是合法 JSON,都返回一条人话错误。
    // profile 缺省 Pure(第 4 步起新缺省;老行为全开须显式 Trusted)。
    static std::expected<std::unique_ptr<LuaTool>, std::string> LoadFromScript(
        const std::string& script, const std::string& stem,
        const LuaProfile& profile = LuaProfile::PureDefault());

    // 从磁盘文件加载(按二进制原样读,UTF-8 中文不过手不转码)。
    static std::expected<std::unique_ptr<LuaTool>, std::string> LoadFromFile(
        const std::filesystem::path& file, const LuaProfile& profile = LuaProfile::PureDefault());

    ~LuaTool() override;
    LuaTool(const LuaTool&) = delete;
    LuaTool& operator=(const LuaTool&) = delete;

    std::string name() const override;         // plugin__<文件名>__<表里的 name>
    std::string description() const override;  // [plugin:<文件名>] 前缀
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    bool deferred() const override { return true; }  // tool_search:外挂工具走延迟挂载
    Result execute(const nlohmann::json& input) override;

    const std::string& stem() const { return stem_; }
    const LuaProfile& profile() const { return profile_; }

    // ESC 取消链:装配层每轮灌指针(不设 = 不检查)。execute 期间 hook 里
    // 查这面旗,置位即 luaL_error("用户取消"),照常走 is_error 终态。
    void SetCancel(const std::atomic<bool>* cancel) { cancel_ = cancel; }

private:
    LuaTool() = default;

    lua_State* lua_ = nullptr;
    int execute_ref_ = -1;  // execute 函数在 lua 注册表里的引用
    std::mutex execute_mutex_;  // 同一 state 不许被并发子代理同时碰
    std::string stem_;
    std::string full_name_;
    std::string description_;
    nlohmann::json schema_;
    LuaProfile profile_;
    const std::atomic<bool>* cancel_ = nullptr;
    // 三道墙的运行时账(hook/allocator 回调从这里取)。
    std::unique_ptr<LuaGuard> guard_;
};

// 目录扫描的产物:装好的工具 + 每个坏文件一条警告。
struct LuaScanResult {
    std::vector<std::unique_ptr<LuaTool>> tools;
    std::vector<std::string> warnings;
};

// 扫 dir 下的 *.lua(不递归)。目录不存在返回空(插件本来就是可选的);
// 单个文件坏了(语法错、缺字段……)跳过并写一条警告,不连累其余。
// 结果按文件名排序,顺序稳定。profile 缺省 Pure。
LuaScanResult LoadLuaPlugins(const std::filesystem::path& dir,
                             const LuaProfile& profile = LuaProfile::PureDefault());

}  // namespace lubancode::tools
