// EmbeddedLuaRuntime(plugins 单第 4 步:收现有 Lua 进新体系)。
//
// legacy `.lua` 一文件一工具的用法零变化:文件格式、返回表、工具名
// plugin__<文件名>__<表里的 name> 全照旧;底下换了个引擎室——每枚
// LuaTool 挂进 RuntimeManager 统一管,profile(pure/trusted)、指令预算、
// 内存帽、取消链都在 tools::LuaTool 的 state 构造期落墙(见 lua_tool.cpp
// 的 NewGuardedState),这一层只做 Definition 形状与挂载账。
//
// 与 PluginManifest 的关系:legacy .lua 没有 plugin.json,这里替它捏一份
// kind=EmbeddedLua 的在册定义(name/version 从文件名来,language=lua),
// /plugins 的统一台账(loaded/unavailable/...)才有账可查。执行不经过
// PluginToolAdapter 的 process 分支——Lua 是同进程直调,协议帧不适用。
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tools/lua_tool.hpp"
#include "tools/tool.hpp"

namespace lubancode::runtime {

// 一枚 Lua 插件的在册定义:/plugins 台账用,不进模型 prompt。
struct LuaPluginRecord {
    std::string id;          // 文件名去扩展名
    std::string version;     // legacy 文件没有版本,占 "legacy"
    std::string tool_name;   // 完整工具名 plugin__<文件名>__<表里的 name>
    tools::LuaTool* tool = nullptr;  // runtime 持有,挂载后不搬家
};

// 一张 registry 的挂载视图:每枚 LuaTool 包一枚轻 adapter(转发到同一
// state;mutex 串行由 LuaTool 自己保),registry 拿 unique_ptr 管 adapter,
// state 仍归 runtime——析构次序上 runtime 后没,adapter 里的裸指针不悬垂
// (声明顺序由 ToolRuntime 保证,与 PluginHost 同一条规矩)。
class LuaToolAdapter : public tools::Tool {
public:
    explicit LuaToolAdapter(tools::LuaTool& tool) : tool_(&tool) {}

    std::string name() const override { return tool_->name(); }
    std::string description() const override { return tool_->description(); }
    nlohmann::json input_schema() const override { return tool_->input_schema(); }
    bool needs_confirm() const override { return true; }
    bool deferred() const override { return true; }
    tools::Tool::Result execute(const nlohmann::json& input) override { return tool_->execute(input); }
    // 取消旗透传(子代理 x 停止失效单):adapter 不许洗掉 context。
    tools::Tool::Result execute(const nlohmann::json& input,
                                const tools::ToolExecutionContext& context) override {
        return tool_->execute(input, context);
    }

private:
    tools::LuaTool* tool_;
};

// Lua 运行时管理器:持有全部 LuaTool(state + mutex + 三道墙),只扫一次
// 目录;每张 registry 各挂一套轻 adapter(MountPlugins 对 main/sub 各调一
// 遍,LoadDirectory 的幂等由调用方以"只调一次"保证——扫两遍同一目录会
// 造出第二套 state,浪费且台账翻倍,这里不静默吞)。
class EmbeddedLuaRuntime {
public:
    EmbeddedLuaRuntime() = default;
    ~EmbeddedLuaRuntime() = default;
    EmbeddedLuaRuntime(const EmbeddedLuaRuntime&) = delete;
    EmbeddedLuaRuntime& operator=(const EmbeddedLuaRuntime&) = delete;

    // 扫 dir 下的 *.lua(不递归,文件名排序稳定)。单文件坏:一条警告
    // 跳过,不连累其余。profile 缺省 Pure(第 4 步起新缺省)。
    // 返回警告(已加载的文件重复调用不重扫,返回空)。
    std::vector<std::string> LoadDirectory(const std::filesystem::path& dir,
                                           const tools::LuaProfile& profile = tools::LuaProfile::PureDefault());

    const std::vector<LuaPluginRecord>& records() const { return records_; }
    const std::vector<std::unique_ptr<tools::LuaTool>>& tools() const { return tools_; }

    // 给一张 registry 造全套轻 adapter(调用方逐个 Register)。
    std::vector<std::unique_ptr<LuaToolAdapter>> MakeAdapters() const;

    // ESC 取消链:装配层每轮灌给全部工具(不设 = 不检查)。
    void SetCancel(const std::atomic<bool>* cancel);

private:
    std::vector<std::unique_ptr<tools::LuaTool>> tools_;
    std::vector<LuaPluginRecord> records_;
    bool loaded_ = false;
};

}  // namespace lubancode::runtime
