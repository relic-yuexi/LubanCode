#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tools/tool.hpp"

namespace lubancode::tools {

struct AskUserOption {
    std::string label;
    std::string description;
};

struct AskUserQuestion {
    std::string header;
    std::string question;
    std::vector<AskUserOption> options;
    bool multi_select = false;
};

using AskUserHandler =
    std::function<std::expected<std::vector<std::string>, std::string>(const AskUserQuestion&)>;

// 模型在回合中向用户问选择题。每题末尾的“自己填写”由终端 UI 自动补，
// 不让模型重复造一个 Other 选项。handler 由交互入口注入，工具层不碰终端。
class AskUserTool : public Tool {
public:
    explicit AskUserTool(AskUserHandler handler);

    // 换 handler(app-server 一类前端在装配后把终端问话换成反向请求)。
    void SetHandler(AskUserHandler handler) { handler_ = std::move(handler); }

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    AskUserHandler handler_;
};

}  // namespace lubancode::tools
