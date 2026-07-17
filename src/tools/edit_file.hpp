#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 对已有文件做精确字符串替换:old_string 必须在文件里逐字唯一出现
// (除非 replace_all 置真),找不到、或者出现多次但没开 replace_all,
// 都报错并说清楚原因(比如出现了几次)。执行前需要用户确认。
class EditFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
