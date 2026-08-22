// undo_file_edit(逐枚追踪单第四期"补偿执行器"):write_file/edit_file 的
// 条件式撤销。
//
// 单子《本地文件条件式撤销》的执行侧:
//   - write/edit 落账时带 undo token(path/preimage sha/postimage sha/
//     preimage 正文/是否新建);
//   - 撤销前重读目标:当前 sha 等于 postimage 才恢复 preimage(其后没人
//     再改);已变则拒绝自动撤销,给三方 diff 交用户;
//   - 新建文件只在内容仍等于 postimage 时可移走;
//   - undo 本身也是工具调用:needs_confirm 恒真,走与 write/edit 同一道
//     确认门(单子:"须确认"),trace 里留 compensates 关系(见
//     ToolTraceContext::compensates),失败也留账。
//
// token 从哪来:会话的 trace 账(ToolTraceHub 的账本)。工具不自己去翻
// JSONL——装配层把"按 execution_id 查 undo token"的查表函数灌进来;
// 查不到就说查不到,不猜。

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 账本侧的 undo 查表口:execution_id -> 该枚执行的 undo token。
// 实现由装配层给(ToolTraceHub 的账本);返回 nullopt = 账里没有这枚
// execution 或它没带 token。
struct UndoTokenLookup {
    std::function<std::optional<agent::ToolUndoToken>(const std::string& execution_id)> find;
    // 这枚 undo 补偿的是哪枚 execution(token 的主人)——compensates
    // 关系的两端。查不到与 find 同步给 nullopt。
    std::function<std::string(const std::string& execution_id)> owner_of;

    bool alive() const { return static_cast<bool>(find); }
};

class UndoFileEditTool : public Tool {
public:
    explicit UndoFileEditTool(UndoTokenLookup lookup) : lookup_(std::move(lookup)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;

    // 撤销是写操作:恒须确认(单子:"undo 本身也是工具调用,须确认")。
    // 即便用户 accept_for_session 过 write_file,这里也不吃那份免问——
    // 撤销的方向与写相反,免问账不通用。
    bool needs_confirm() const override { return true; }

    // 撤销的撤销还是一次文件改动:与 write/edit 同档(本地可逆)。
    EffectClass effect_class() const override { return EffectClass::LocalReversible; }
    Idempotency idempotency() const override { return Idempotency::NonIdempotent; }
    RecoveryCapability recovery_capability() const override {
        return RecoveryCapability::ConditionallyUndoable;
    }

    Result execute(const nlohmann::json& input) override;

    // 本枚 undo 补偿的目标(上一次 execute 查到的 owner);装配层在发
    // trace 前取走,填进 ToolTraceContext::compensates。
    const std::string& last_compensates() const { return last_compensates_; }

private:
    UndoTokenLookup lookup_;
    std::string last_compensates_;
};

// 条件式撤销的判定(纯函数,单测钉死):
//   - token 不可用(超限没内联 preimage)-> 不撤销,给"须走 Git/worktree
//     检查点"的实话;
//   - 目标当前内容 sha == postimage:新建文件移走,旧文件恢复 preimage;
//   - 不等:拒绝自动撤销,给三方 diff(preimage / postimage / 当前)。
// 返回 {正文, is_error, 撤销是否真发生}。
struct ConditionalUndoOutcome {
    std::string message;
    bool is_error = false;
    bool performed = false;
};
ConditionalUndoOutcome ApplyConditionalUndo(const std::filesystem::path& path, const agent::ToolUndoToken& token);

}  // namespace lubancode::tools
