// 逐调用审批账(只读工具并行与写入串行单 P1)。确认回调的签名不带
// PreToolUse 表态与审批类别——RunOneTool 先跑 PreToolUse/权限预裁定、再问
// 确认,中间产物得有处落脚。旧路靠两只跨调用共享槽(pre_decision_slot/
// approval_class_slot)偷渡:同批两枚调用裁决不同就会串槽,后跑的那枚把
// 前一枚的表态顶掉。改成按 tool_use_id 记账:PreToolUse/预裁定各写自己
// 那一枚,确认口取走自己那一枚(取走即清,不跨调用残留)。查无此 id
//(没跑过 Hook/预裁定的旧路,如 workflow 借来的确认口)回缺省值——与旧
// 槽"没人写过就是默认"同一形状,不冒充、不报错。
//
// 线程契约:本批执行链仍是同线程串行,普通 map 即可;P2 并行化后两只
// 确认口可能同时在跑,这只表要加锁或换并发容器(单子 §四"逐调用上下
// 文"),届时在此处收口,不动各装配点。

#pragma once

#include <map>
#include <string>
#include <utility>

#include "runtime/interaction.hpp"  // ToolHookDecision:PreToolUse 的归并表态
#include "tools/tool.hpp"           // ApprovalClass:审批类别

namespace lubancode::app {

// 一枚调用的审批链中间产物。
struct ToolCallScope {
    lubancode::runtime::ToolHookDecision pre;  // PreToolUse 归并表态(缺省 = 没跑 Hook)
    lubancode::tools::ApprovalClass approval_class = lubancode::tools::ApprovalClass::None;
};

class ToolCallScopeTable {
public:
    // PreToolUse 表态入账(同一 id 重跑 Hook 以最新为准)。
    void RecordPre(const std::string& tool_use_id, lubancode::runtime::ToolHookDecision pre) {
        entries_[tool_use_id].pre = std::move(pre);
    }

    // 权限预裁定递来的审批类别入账。
    void RecordApprovalClass(const std::string& tool_use_id, lubancode::tools::ApprovalClass approval_class) {
        entries_[tool_use_id].approval_class = approval_class;
    }

    // 取走本调用的账并清位(确认口一调用取一次)。
    ToolCallScope Take(const std::string& tool_use_id) {
        const auto it = entries_.find(tool_use_id);
        if (it == entries_.end()) {
            return ToolCallScope{};
        }
        ToolCallScope scope = std::move(it->second);
        entries_.erase(it);
        return scope;
    }

private:
    std::map<std::string, ToolCallScope> entries_;
};

}  // namespace lubancode::app
