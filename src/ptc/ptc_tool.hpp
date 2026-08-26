// programmatic_tool_calling 工具(PTC P1):模型写 Python 脚本编排入选
// 工具,一段脚本收完把 emit 摘要送回模型。
//
// 关键铁律(规格"安全边界"节):脚本里的每一枚 stub 调用都走 agent::
// RunOneTool 这条与 JSON 后端完全相同的链——schema 复检、PreToolUse、
// PermissionRequest(确认档)、执行、PostToolUse、编码信任边界、审计
// (工具起止上事件流)。PTC 不是越权暗门。
//
// 每轮的确认/钩子接线与事件口由终端装配经 SetHooks 灌入(与 AgentTool
// 的 SetHooks 同一套思路):PtcTool 构造一次活整个会话,回调每轮换新。

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // TurnWiring/ToolHookDecision/ToolPhase:执行链的类型
#include "ptc/runner.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::ptc {

class PtcTool : public tools::Tool {
public:
    // 每轮由终端装配(控制面)刷新的转发钩子——控制口字段语义与
    // agent::TurnWiring 同名项一一对应;不设 = 不转发,工具照常执行。
    struct Hooks {
        std::function<bool(const std::string& tool_use_id, const std::string& name,
                           const nlohmann::json& input)> on_tool_confirm;
        std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_pre_tool_use_hook;
        std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_permission_request;
        std::function<void(const std::string& tool_use_id, const std::string& name, runtime::ToolPhase phase)>
            on_tool_phase;
        std::function<std::vector<std::string>(const std::string& tool_use_id, const std::string& name,
                                               const nlohmann::json& input, const tools::Tool::Result& result)>
            on_post_tool_use_hook;
        // Plan 模式(只读研究硬闸单):PTC 生成的 stub 调用走同一 ModePolicy
        // ——单子明令"PTC 生成的调用也走 RunOneTool 与 ModePolicy,不能只
        // 拦 JSON tool calling"。不设 = 不转发(旧行为)。
        std::function<std::string(const std::string& tool_name, const nlohmann::json& input)> on_mode_policy;
        // 显示出水口(骨架拆解批二余款):stub 调用的起止上宿主的事件流
        // ——16 枚同构调用逐枚有账;subordinate_stream 恒真(从路):终端只
        // 画一张外层卡,不刷满屏(规格 UI 节)。不设 = 不上事件流(旧行为)。
        runtime::TurnEventAdapter* events = nullptr;
        bool subordinate_stream = true;
        const std::atomic<bool>* cancel = nullptr;  // Esc 取消链(每轮的旗子)
    };

    struct Config {
        std::string python_cmd;                    // 解释器(空 = 探测)
        PtcLimits limits;
        bool restricted_token = true;              // Windows 受限 token
        std::vector<std::string> eligible_tools;   // 入选白名单(空 = 默认只读集)
    };

    // registry:主工具表(脚本调用经它 Find + RunOneTool)。
    // tool_filter:tool_search 延迟挂载谓词——没挂载的工具不进 stub 集。
    PtcTool(tools::ToolRegistry& registry, std::function<bool(const tools::Tool&)> tool_filter, Config config);

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

    void SetHooks(Hooks hooks);

    // 给系统提示的 PTC 指南段(用法 + 当前已挂载 stub 的签名索引)。
    // tool_search 挂载集变了就跟着变;放在轮次请求的动态上下文里,不动
    // 稳定的 system(前缀缓存守恒)。
    std::string GuideSegment() const;

    // 本会话是否已注册成功(解释器探测失败时 false,调用方据此不挂)。
    bool available() const { return available_; }
    const std::string& unavailability_reason() const { return unavailability_reason_; }

    // 最近一次运行的结果(审计/状态栏用)。
    std::optional<PtcRunResult> last_run() const;

private:
    tools::ToolRegistry& registry_;
    std::function<bool(const tools::Tool&)> tool_filter_;
    Config config_;
    bool available_ = true;
    std::string unavailability_reason_;
    std::mutex hooks_mutex_;
    Hooks hooks_;
    std::optional<PtcRunResult> last_run_;
    std::uint64_t call_seq_ = 0;  // ptc stub 调用的 tool_use id 序号
};

// 默认入选集:P1 只读真工具(read_file + search 的 grep/glob 两模式;
// LubanCode 没有独立 glob/list 工具,search mode="glob" 即通配列举)。
std::vector<std::string> DefaultEligibleTools();

// 探测本机 Python 解释器:config 里指定了就用指定的(并验证能跑);
// 没指定按平台试 python/python3。返回 {命令, 版本} 或失败原因。
struct PythonProbe {
    bool ok = false;
    std::string command;
    std::string version;
    std::string error;
};
PythonProbe ProbePythonInterpreter(const std::string& configured);

}  // namespace lubancode::ptc
