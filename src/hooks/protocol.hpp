// hooks 的输入输出协议层(schema 2)。
//
// 输入:每只 command hook 从 stdin 收一份 JSON——公共字段(session_id/
// turn_id/cwd/transcript_path/permission_mode/agent_id/agent_type)只在这
// 里拼一遍,事件字段由 HookPayload.fields 带进来。大 JSON 走 stdin,不塞
// 环境变量(Windows 环境块有尺寸与编码边界)。
//
// 输出:stdout 可回一份结构化 JSON(continue/stopReason/systemMessage/
// hookSpecificOutput{...})。逐事件校验字段——用错报 schema_error,不悄悄
// 吞,也不因解析失败反把危险工具放过去而不留痕。
//
// 退出码:
//   0  成功;stdout 是 JSON 就按事件 schema 解
//   2  阻断;stderr 作阻断理由
//   其它  hook 自己失败;照 failure_policy 处理
// legacy adapter(旧四类配置)不走这套:任意非零仍拦、LUBAN_TOOL_* 照导、
// 30 秒不变(见 dispatcher 的 legacy 分支)。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/types.hpp"

namespace lubancode::hooks {

// 组出给 handler 的 stdin JSON(公共字段 + 事件字段)。hook_run_id 每次
// 发射生成一个,给日志对账用。
nlohmann::json BuildStdinPayload(const HookPayload& payload, const HookContext& context,
                                 const std::string& hook_run_id);

// ---------------------------------------------------------------------------
// stdout 解析结果。一只 handler 的单份表态,归并之前的中间态。
// ---------------------------------------------------------------------------
struct HookOutput {
    // ok:stdout 为空或合法 JSON。schema_error:字段用错/不是 object/JSON
    // 坏。解析失败时 decision 全部保持 None,由调用方按 failure_policy 算账。
    bool ok = false;
    std::string error;

    bool has_continue = false;  // stdout 里写了 continue 字段
    bool continue_flag = true;
    std::string stop_reason;
    std::string system_message;

    bool has_permission_decision = false;
    HookEventResult::Permission permission = HookEventResult::Permission::None;
    std::string permission_reason;
    bool has_updated_input = false;
    nlohmann::json updated_input;
    bool has_additional_context = false;
    std::string additional_context;
};

// 按事件 schema 解析并校验 stdout。stdout 全是空白 = 没有结构化输出(ok,
// 全部字段缺省)。hookSpecificOutput.hookEventName 若写了、须与事件一致。
HookOutput ParseStdoutJson(HookEvent event, const std::string& stdout_text);

// 单只 handler 表态的裁决(退出码 + stdout 解析结果 + failure_policy):
// 返回 outcome(ok/blocked/failure/schema_error/timeout/spawn_failed 之一)
// 与 decision 字符串。纯函数,单测钉死矩阵:exit 0/2/1、坏 JSON、空 stdout。
struct SingleOutcome {
    std::string outcome;
    std::string decision;  // allow/deny/ask/none
    std::string detail;
};
SingleOutcome JudgeSingleRun(HookEvent event, unsigned long exit_code, bool timed_out, bool spawn_failed,
                             const HookOutput& parsed, const std::string& stderr_text);

// legacy adapter 的环境变量表(LUBAN_TOOL_*;文档已标 deprecated,新能力
// 只走 stdin JSON)。
std::vector<std::pair<std::string, std::string>> BuildLegacyToolEnv(const std::string& tool_name,
                                                                    const nlohmann::json& tool_input,
                                                                    const std::optional<std::string>& tool_result,
                                                                    bool tool_is_error);

}  // namespace lubancode::hooks
