#pragma once

#include <cstddef>
#include <string>

#include "tools/tool.hpp"

namespace lubancode::tools {

// 不写盘的 edit_file 计划(AR-05:预览与执行共用同一颗决策源)。
// 匹配语义与 EditFileTool::execute 完全同源——精确 -> 换行归一 ->
// 缩进/行尾空白归一;任何一层多处候选没开 replace_all 都拒绝,不猜。
struct EditPlan {
    bool ok = false;                 // false:计划被拒,error 是与工具一致的拒绝文案
    std::string error;               // 拒绝原因(预览据此走段内回退,执行原样报给模型)
    std::string updated;             // ok:替换后的完整文件字节(执行落盘的就是这份)
    std::size_t replaced_count = 0;  // ok:实际替换处数
    std::string match_mode;          // ok:"精确"/"换行归一"/"缩进/行尾空白归一"
    std::string original_sha256;     // 原件指纹:预览与执行之间核"文件变没变"的对账依据
};

// 给定原件字节与编辑入参,算出不写盘的完整计划。纯函数(除 sha256),
// 不碰文件系统——调用方各自读原件:预览与执行都读当下的盘,谁也不许
// 拿旧计划静默套新文件。path_utf8 只进错误文案,不参与匹配。
EditPlan BuildEditPlan(const std::string& original, const std::string& path_utf8,
                       const std::string& old_string, const std::string& new_string, bool replace_all);

// 对已有文件做字符串替换:先逐字精确找；失败后有限度兼容换行、统一
// 缩进与行尾空白。任何一层出现多处且没开 replace_all 都拒绝猜测。
// 匹配/替换的决策全在 BuildEditPlan 里,execute 只管校验、落盘与记账。
class EditFileTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::LocalReversible; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::NonIdempotent; }
    lubancode::tools::RecoveryCapability recovery_capability() const override { return lubancode::tools::RecoveryCapability::ConditionallyUndoable; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    ApprovalClass approval_class() const override { return ApprovalClass::FileEdit; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
