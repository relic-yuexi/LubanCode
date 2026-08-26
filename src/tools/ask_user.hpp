#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>
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

enum class AskUserResponseKind {
    Answered,
    Declined,
    Discuss,
};

// 用户面对问题时有三条正常去路：作答、拒答、转为自由讨论。三者都不是
// 工具故障；handler 的 unexpected 只留给断线、超时与前端读写失败。
struct AskUserResponse {
    AskUserResponseKind kind = AskUserResponseKind::Answered;
    std::vector<std::string> answers;
    std::string message;

    static AskUserResponse Answered(std::vector<std::string> values) {
        return AskUserResponse{AskUserResponseKind::Answered, std::move(values), {}};
    }
    static AskUserResponse Declined() {
        return AskUserResponse{AskUserResponseKind::Declined, {}, {}};
    }
    static AskUserResponse Discuss(std::string value) {
        return AskUserResponse{AskUserResponseKind::Discuss, {}, std::move(value)};
    }
};

using AskUserHandler =
    std::function<std::expected<AskUserResponse, std::string>(const AskUserQuestion&)>;

// 模型在回合中向用户问选择题。“自己填写”与“聊聊这个问题”由终端 UI
// 自动补，不让模型重复造选项。handler 由交互入口注入，工具层不碰终端。
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
