// 工具注册表:agent 启动时把所有工具注册进去,运行时按名字查找、
// 把全部工具的定义(name/description/input_schema)导出来供拼请求用。

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tools/tool.hpp"

namespace lubancode::tools {

class ToolRegistry {
public:
    void Register(std::unique_ptr<Tool> tool);

    // 按名字查找,找不到返回 nullptr。所有权还在注册表手里,调用方不用管释放。
    Tool* Find(const std::string& name) const;

    // 注册进来的全部工具,按注册顺序排列。
    const std::vector<std::unique_ptr<Tool>>& All() const { return tools_; }

private:
    std::vector<std::unique_ptr<Tool>> tools_;
};

}  // namespace lubancode::tools
