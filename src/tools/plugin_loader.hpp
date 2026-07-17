// M7:C ABI DLL 插件。扫 <主目录>/.lubancode/plugins/*.dll,LoadLibraryW +
// GetProcAddress("luban_plugin_entry") 拿 manifest,校验 api_version(不合
// 打警告跳过),每个 luban_tool_def 裹成 PluginTool 挂进工具表。
//
// 生命周期规矩(跟 McpServerRuntime 一个路数):PluginHost 拥有全部已加载
// 的 DLL 模块,析构时 FreeLibrary;PluginTool 手里的 luban_tool_def* 指向
// DLL 里的静态数据,所以 main.cpp 里 PluginHost 必须声明在 ToolRegistry
// 之前——先声明的后析构,registry(里头的 PluginTool)先没,模块后卸,
// 不会出现"工具还活着、代码段已经卸掉"的悬垂。
//
// 跨堆安全:execute 调函数指针拿到 luban_tool_result 后,content 立刻拷成
// std::string,然后回调插件自己的 free_result 释放——两边 CRT 可能不同,
// 谁分配谁释放。DLL 里崩了(同进程)宿主兜不住,风险声明见
// include/luban_plugin.h 和 README。
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "luban_plugin.h"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 裹一条 luban_tool_def。def 指向 DLL 里的静态数据,由 PluginHost 保证
// 活得比这个工具久(见文件头的声明顺序规矩)。单测也可以不经 DLL、直接
// 拿测试进程里造的 luban_tool_def 构造(函数指针指向测试自己的函数)。
class PluginTool : public Tool {
public:
    PluginTool(std::string dll_stem, const luban_tool_def* def);

    std::string name() const override;         // plugin__<dll名>__<工具名>
    std::string description() const override;  // [plugin:<dll名>] 前缀
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }  // 外部代码,一律先问
    bool deferred() const override { return true; }        // tool_search:外挂工具走延迟挂载
    Result execute(const nlohmann::json& input) override;

    const std::string& dll_stem() const { return dll_stem_; }

private:
    std::string dll_stem_;
    const luban_tool_def* def_;
    std::string full_name_;
    std::string description_;
    nlohmann::json schema_;  // 构造时解析一次;解析不出来退化成 {"type":"object"}
};

// 一个已加载的 DLL 插件。module 是 HMODULE,存成 void* 免得这个头文件
// 拖上 windows.h。
struct LoadedPlugin {
    std::string stem;  // 文件名去掉扩展名,拼工具名前缀用
    std::string path;  // 完整路径(UTF-8,报告/警告用)
    void* module = nullptr;
    const luban_plugin_manifest* manifest = nullptr;
};

// 一个插件裹好的全部工具(挂载前的中转:调用方拿去打印 "N 个工具"、逐个
// Register 进 registry)。
struct WrappedPlugin {
    std::string stem;
    std::vector<std::unique_ptr<Tool>> tools;
};

class PluginHost {
public:
    PluginHost() = default;
    ~PluginHost();  // FreeLibrary 全部模块(会话结束)

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // 扫描 dir 下的 *.dll(不递归),逐个加载。坏 DLL / 没有入口 / api_version
    // 不合 / manifest 不像话,统统跳过,往 warnings 里写一条人话,不崩。
    // 目录不存在时什么都不做(插件本来就是可选的)。返回本次新增的警告。
    // 非 Windows 平台是空操作。
    std::vector<std::string> LoadDirectory(const std::filesystem::path& dir);

    const std::vector<LoadedPlugin>& plugins() const { return plugins_; }

    // 把已加载插件的每个 tool_def 裹成 PluginTool。单条 def 缺 name/execute
    // 的跳过并写警告,不连累同插件的其余工具。
    std::vector<WrappedPlugin> WrapTools(std::vector<std::string>& warnings) const;

private:
    std::vector<LoadedPlugin> plugins_;
};

}  // namespace lubancode::tools
