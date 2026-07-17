#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 读文件,按行号显示(cat -n 风格)。支持 offset/limit 只读一部分。
// 路径按 UTF-8 处理,正斜杠反斜杠都认,中文路径也不会乱码。
class ReadFileTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
