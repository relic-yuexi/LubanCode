// 工具调用操作账(§4.14-4.15/§4.18-4.20):一次逻辑工具调用一枚全局
// actionId(== payload.tool_call_id),等待/执行/终态三段全链事件、attempt
// 重试链、幂等键、结果选用与最终 tool 消息。
//
// 顺序合同(§4.18 写死):assistant 调用消息落稳 -> 接纳/等待(pending)
// -> PreAction hook 与效果采用 -> 准入复核 -> started(effectiveArgsRef)
// -> 执行终态 -> 结果持久化(result.persisted) -> PostAction hook 效果
// -> 结果选用(result.selected) -> 32 KiB 预览 -> tool 消息 -> 接纳。
//
// 状态纪律:每次尝试最多一个执行终态(第二次拒收);终态后迟到响应另记
// 观察事件不改旧账;重试先落上一 attempt 终态,再对 attempt+1 记 pending;
// rejected 没有执行、不带 started;快工具不虚造 waiting。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::trajectory::v3 {

// 工具身份(§4.15):不能只存一个可能重名的 get_weather;须绑定注册
// 来源、版本与执行目标。
struct ToolIdentity {
    std::string logical_name;          // 模型声明的逻辑名(不改写,§4.22)
    std::string registration_source;   // 注册来源(builtin/plugin/...)
    std::string version;
    std::string execution_scope;       // cwd/远端目标等语义上下文

    nlohmann::json ToJson() const {
        return nlohmann::json::object({{"logicalName", logical_name},
                                       {"registrationSource", registration_source},
                                       {"version", version},
                                       {"executionScope", execution_scope}});
    }
};

// 幂等键(§4.15):sha256(canonical({version,action_id,tool_identity,
// effective_args_hash,execution_scope_hash}))。同 Action 的传输重试复用
// (不算 attempt);两次新声明各持新 actionId,即使参数相同也得到不同 key。
// 只有下游支持并实际执行去重时才有外部保障,不替工具变得幂等。
std::string ComputeToolIdempotencyKey(std::string_view action_id,
                                      const ToolIdentity& identity,
                                      std::string_view effective_args_hash,
                                      std::string_view execution_scope_hash,
                                      std::string_view key_version = "v1");

class ToolActionSession {
public:
    enum class Terminal { None, Finished, Failed, Cancelled, Rejected, Unknown };

    // 接纳:tool.execution.pending(reason:queued/approval/dependency/backoff)。
    // attempt 从 1 起;provider_tool_call_id 原样留档(不作全局主键)。
    // extra_payload 供派生执行注入 parentActionId/hookInvocationId 等关联。
    static ToolActionSession Admit(V3Writer& writer, std::string turn_id,
                                   std::string step_id, std::string action_id,
                                   std::string reason,
                                   std::optional<std::string> assistant_message_ref,
                                   std::optional<std::string> provider_tool_call_id,
                                   nlohmann::json extra_payload = nlohmann::json::object(),
                                   Durability durability = Durability::ProcessCrash);

    // 恢复侧把手(失败与恢复单 P1-A):续卷后对已有 Action 补保存/补选用/
    // 补 tool 消息——只补账,不执行、不造新 pending,不改 attempt(记 1 号,
    // 与账上既有 attempt 对齐)。缺结果/缺消息的补交走它;执行动作禁止用。
    static ToolActionSession Reopen(std::string turn_id, std::string step_id, std::string action_id) {
        return ToolActionSession(std::move(turn_id), std::move(step_id), std::move(action_id));
    }

    // 越过执行准入栅栏:tool.execution.started(effectiveArgsRef + 工具身份
    // + 幂等键快照)。extra_payload 供 hook 子执行注入 parentActionId 等。
    WriteReceipt Start(V3Writer& writer, std::string effective_args_ref,
                       ToolIdentity identity,
                       std::optional<std::string> idempotency_key = std::nullopt,
                       nlohmann::json extra_payload = nlohmann::json::object(),
                       Durability durability = Durability::ProcessCrash);

    // 执行中等待外部条件(§4.14:须带 reason 与可恢复等待引用)/继续。
    WriteReceipt Wait(V3Writer& writer, std::string reason, std::string wait_ref,
                      Durability durability = Durability::ProcessCrash);
    WriteReceipt Resume(V3Writer& writer, Durability durability = Durability::ProcessCrash);

    // 执行终态五型。每次尝试最多一个;来第二枚拒收(v3tool.already_terminal)。
    WriteReceipt Finish(V3Writer& writer, std::optional<std::int64_t> exit_code,
                        std::optional<std::uint64_t> execution_duration_ms = std::nullopt,
                        Durability durability = Durability::PowerLoss);
    WriteReceipt Fail(V3Writer& writer, std::string error_code,
                      std::optional<std::uint64_t> execution_duration_ms = std::nullopt,
                      Durability durability = Durability::PowerLoss);
    // phase: before_started(执行前取消,无 started)/during_execution。
    WriteReceipt Cancel(V3Writer& writer, std::string phase, std::string reason,
                        Durability durability = Durability::PowerLoss);
    // 参数/权限/准入拒绝:没有执行。可在 Start 前落。
    WriteReceipt Reject(V3Writer& writer, std::string reason,
                        Durability durability = Durability::PowerLoss);
    // 启动后失联/崩溃/结果无法确认:不能当确定失败。
    WriteReceipt MarkUnknown(V3Writer& writer, std::string reason,
                             Durability durability = Durability::PowerLoss);

    // 上一 attempt 终态后,为 attempt+1 记 pending(§4.14 重试规则:
    // 不能把已结束的尝试改回 pending)。
    WriteReceipt BeginNextAttempt(V3Writer& writer, std::string reason,
                                  Durability durability = Durability::ProcessCrash);

    // 结果持久化(§4.16/§4.18):证明特定结果文件已落稳,关联执行终态;
    // 不重复正文。result_ref 为六键 artifactRef 数组(只有一份也 [ref])。
    WriteReceipt PersistedResult(V3Writer& writer,
                                 const std::vector<nlohmann::json>& result_ref,
                                 std::optional<std::string> execution_event_ref,
                                 std::optional<std::uint64_t> attempt = std::nullopt,
                                 Durability durability = Durability::PowerLoss);
    // 执行成功而结果存储失败(§4.18):保留 done,另报持久化失败;
    // 不改称"工具没有执行"。
    WriteReceipt PersistFailed(V3Writer& writer, std::string reason,
                               std::optional<std::uint64_t> attempt = std::nullopt,
                               Durability durability = Durability::PowerLoss);

    // 结果选用(§4.23):tool.result.selected。effectiveOutcome ∈
    // done|failed|substituted|error;无改写时也明确选择原结果
    // (sourceResultEventRefs 指结果持久化事件,hookEffectEventRefs 可为空)。
    WriteReceipt SelectResult(V3Writer& writer,
                              const std::vector<std::string>& source_result_event_refs,
                              const std::vector<std::string>& hook_effect_event_refs,
                              std::string_view effective_outcome,
                              std::optional<std::uint64_t> attempt = std::nullopt,
                              Durability durability = Durability::PowerLoss,
                              std::optional<std::string> summary_event_ref = std::nullopt);

    // 最终 tool 消息(§4.18/§4.19):content 为模型可见的完整预览版本,
    // 落稳后接纳进上下文;不因 resume 按今天的规则重新生成。is_error 随
    // 回喂语义落档(失败与恢复单 P1-B/FA-02):以 Hook 处理后真正交给模型
    // 的结果为准,只写真值(缺键 = false,旧账两读法都兼容)。
    WriteReceipt AppendToolMessage(V3Writer& writer, std::string content,
                                   std::optional<std::string> result_selection_ref,
                                   bool is_error = false,
                                   Durability durability = Durability::PowerLoss);

    const std::string& action_id() const { return action_id_; }
    const std::string& turn_id() const { return turn_id_; }
    const std::string& step_id() const { return step_id_; }
    std::uint64_t attempt() const { return attempt_; }
    Terminal terminal() const { return terminal_; }
    bool started() const { return started_; }
    const std::optional<std::string>& selected_event_id() const {
        return selected_event_id_;
    }
    const std::optional<std::string>& last_event_id() const { return last_event_id_; }

private:
    ToolActionSession(std::string turn_id, std::string step_id, std::string action_id);
    // 通用事件提交:信封三件 + 公共 payload(tool_call_id/attempt)。
    WriteReceipt Emit(V3Writer& writer, EventKindV3 kind, std::optional<OpStatus> status,
                      nlohmann::json payload, Durability durability);
    // 终态收口:校验"每尝试一个终态"并翻状态。
    WriteReceipt CloseTerminal(V3Writer& writer, EventKindV3 kind, nlohmann::json payload,
                               Terminal terminal, Durability durability);

    std::string turn_id_;
    std::string step_id_;
    std::string action_id_;
    std::uint64_t attempt_ = 1;
    bool started_ = false;
    bool waiting_ = false;
    Terminal terminal_ = Terminal::None;
    std::optional<std::string> selected_event_id_;
    std::optional<std::string> last_event_id_;
};

}  // namespace lubancode::trajectory::v3
