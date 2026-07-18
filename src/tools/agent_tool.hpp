// 内置 "agent" 工具:把一个独立子任务委托给子代理执行。子代理是一个全新
// 的、空历史的 agent::AgentLoop,只有它自己知道任务细节——主对话历史里
// 只留一次工具调用的入参(prompt)和一段 Result.content(子代理的最终
// 结论),中间的搜索/试错/来回工具调用过程都不会挤占主对话的上下文,
// 这是这个工具存在的全部意义。
//
// 防递归:子代理拿到的工具注册表(sub_registry)不含 "agent" 自己,深度
// 硬限 1——子代理没法再委托一个孙代理。main.cpp 负责建两份 ToolRegistry
// (主表含 agent,子表不含),这里只管拿着子表的引用用,自己不做任何排除
// 逻辑。
//
// 回调贯通:子代理执行期间的确认请求(needs_confirm 的工具)、usage 记账、
// "子代理调了个工具"这件事本身,都要能让上层(main.cpp)看到、按同一套
// 确认模式处理——但子代理的碎碎念文本(on_text_delta)不逐字外放,免得
// 刷屏。做法是 Hooks 结构体:main.cpp 在每一轮 RunTurn 开始时,把这一轮
// 的 on_tool_confirm/on_usage 直接转发过来、外加一个"子代理调了工具"的
// 打印回调,通过 SetHooks 灌进来;execute() 每次跑子代理时都用当前这份
// Hooks 转发,不设(默认构造的空 Hooks)也不会崩,只是不转发/不打印。
#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class AgentTool : public Tool {
public:
    struct Hooks {
        // 子代理内部工具 needs_confirm() 为真时,原样转发给父级
        // on_tool_confirm——三档确认模式(yolo/auto/confirm)在父级那份
        // 回调里已经处理好了,这里不用重复实现。
        std::function<bool(const std::string& name, const nlohmann::json& input)> on_tool_confirm;

        // 子代理发起了一次工具调用(还没执行),给上层打一行提示用,比如
        // "  [子代理·工具] read_file {...}"。跟 on_tool_confirm 分开是因为
        // 这个纯粹用于展示,没有返回值、不影响子代理是否真的执行。
        std::function<void(const std::string& name, const nlohmann::json& input)> on_sub_tool_start;

        // 子代理每次独立请求结束的 usage,原样转发给父级 on_usage——累计
        // 进本轮 token 统计,请求次数也算进去。
        std::function<void(const api::Usage& usage)> on_usage;

        // M9:子代理内部的工具调用也要受 pre_tool/post_tool 钩子管——原样
        // 转发给父级的同名回调,子代理这边不重复实现匹配/执行逻辑。
        std::function<std::optional<std::string>(const std::string& name, const nlohmann::json& input)>
            on_pre_tool_hook;
        std::function<void(const std::string& name, const nlohmann::json& input, const Tool::Result& result)>
            on_post_tool_hook;
    };

    // backend:子代理发请求用的后端。main.cpp 传进来的通常是跟主循环共用
    // 的那条包装链(模型覆盖/推理强度覆盖/转轮),子代理天然继承同一套。
    // sub_registry:子代理能用的工具注册表,调用方保证其中不含 "agent" 自己
    // (防递归,深度硬限 1)。
    // cwd:子代理系统提示词里的工作目录段,跟主代理一致。
    // model:子代理发请求用的 model 字段;如果 backend 链里有会覆盖 model
    // 的包装层(比如 main.cpp 的 ModelOverrideBackend),这里填什么都会被
    // 覆盖掉,留空也没关系。
    // default_max_turns:入参没给 max_turns 时用这个默认值。
    // skills_segment:M9 新增,系统提示词里"可用技能"那一段(见
    // agent::BuildSkillsPromptSegment),子代理跟主代理共用同一份扫描结果,
    // 空串表示没有技能(不注入)。main.cpp 扫描一次,主代理、子代理都传
    // 同一份。
    AgentTool(api::Backend& backend, ToolRegistry& sub_registry, std::string cwd, std::string model = std::string(),
              int default_max_turns = 15, std::string skills_segment = std::string());

    void SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    // tool_search(延迟挂载):子代理注册表同机制。filter 原样灌给每次
    // execute() 新建的 sub_loop(loaded 集合与主会话共享,挂载一次两边
    // 可用);index_provider 每次 execute() 现算"延迟未加载"索引段,拼进
    // 子代理系统提示末尾。两个都不设(默认)= 子代理不启用延迟,行为跟
    // 从前完全一样。启动时(main.cpp)设一次,不随 RunTurn 重灌——它们
    // 跟 Hooks(每轮现算的转发回调)生命周期不同。
    void SetToolFilter(std::function<bool(const Tool&)> filter) { tool_filter_ = std::move(filter); }
    void SetDeferredIndexProvider(std::function<std::string()> provider) {
        deferred_index_provider_ = std::move(provider);
    }

    // 提示词运行时化(0.21.x):用户模块目录(~/.lubancode/prompts)。设了
    // 之后每次 execute() 新建子代理时,系统提示的 features 模块同走"用户
    // 文件优先、嵌入回退"——跟主循环同机制。不设(默认空)= 只用嵌入版,
    // 行为跟从前完全一样。
    void SetPromptsDir(std::string prompts_dir) { prompts_dir_ = std::move(prompts_dir); }

    void SetSkillsSegment(std::string skills_segment) { skills_segment_ = std::move(skills_segment); }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }  // 子代理内部的危险工具各自有确认关
    Result execute(const nlohmann::json& input) override;

private:
    api::Backend& backend_;
    ToolRegistry& sub_registry_;
    std::string cwd_;
    std::string model_;
    int default_max_turns_;
    std::string skills_segment_;
    std::string prompts_dir_;  // 提示词运行时化:空 = 只用嵌入版
    Hooks hooks_;
    std::function<bool(const Tool&)> tool_filter_;            // tool_search:空 = 不过滤
    std::function<std::string()> deferred_index_provider_;    // tool_search:空 = 不注索引段
};

}  // namespace lubancode::tools
