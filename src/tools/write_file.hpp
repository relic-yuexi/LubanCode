#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 写文件:UTF-8 写入,父目录不存在就自动建好。文件已存在就整个覆盖
// (结果里会注明"覆盖了原有文件")。执行前需要用户确认。
class WriteFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
