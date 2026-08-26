// 引擎侧交互合同(骨架拆解批四:审批四态从 agent/loop.hpp 归位 runtime)。
//
// 这里放的是"内核问、宿主答"那一族中立形状:审批请求与四态回答
//(ApprovalRequest / ApprovalResponse / InteractionDecision)、提问的选择题
// 形状(QuestionOption / QuestionRequest / QuestionResponse)、悬挂未来的
// 最小动作(InteractionFuture),以及 PreToolUse 钩子的归并表态与工具
// 生命周期相位(ToolHookDecision / ToolPhase)。完整合同(请求号、Broker
// 线程化装配)在 runtime/interaction_broker.hpp,那边 include 这头;引擎
//(agent/loop)反向只 include 这头,不认识 Broker。
//
// 依赖铁律:本头零实现依赖,不 include cli/app/tools;agent/ 只许依赖
// 这一层,不许往上够 Broker。

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 决定四态(显示系统剥离单约定 1):accept(本次允许)/ accept_for_session
//(会话总允许,只写本场权限账)/ decline(拒绝)/ cancel(悬空收口——打断、
// 断开、超时或关 thread;等价拒绝,但拒绝文案须写"没人可答",不冒充用户
// 拒绝)。
enum class InteractionDecision {
    Accept,             // 本次允许
    AcceptForSession,   // 本会话内该工具/该问题不再问
    Decline,            // 拒绝
    Cancel,             // 悬空收口(打断/断开/超时/关 thread)
};

// ---- 审批(ApprovalRequested / ResolveApproval)----------------------------

// 一枚审批请求的领域描述:工具名 + 入参 + 前端要摆的提示。input 保持
// 原始 JSON(结构化 input 是 TurnItem 的真值,单子"四"),前端自己决定
// 怎么摘要;这里不预先翻成终端文案。tool_use_id 钉在条目上(ToolUseBlock.id;
// PTC stub 合成的是 ptc-N),远端前端凭它把审批事件路由回条目,可空。
struct ApprovalRequest {
    std::string tool_use_id;
    std::string tool_name;
    nlohmann::json input = nlohmann::json::object();
    // 提示理由(钩子 ask 的 reason、权限档说明等);可空。
    std::string reason;

    nlohmann::json to_json() const;
    static ApprovalRequest from_json(const nlohmann::json& j);
};

// 前端交回的审批决定。reason 是 decline 的理由(给模型看的拒绝文案线索),
// 可空。
struct ApprovalResponse {
    InteractionDecision decision = InteractionDecision::Decline;
    std::string reason;

    nlohmann::json to_json() const;
    static ApprovalResponse from_json(const nlohmann::json& j);
};

// ---- 提问(QuestionRequested / AnswerQuestion)-----------------------------

// ask_user 的选择题形状(tools/AskUserQuestion 的中立镜像,字段同名同义;
// 不 include tools/*,两边各自维护,漂移由单测钉住——见
// test_runtime_contract.cpp 的"与 tools::AskUserQuestion 字段对齐"用例)。
struct QuestionOption {
    std::string label;
    std::string description;

    nlohmann::json to_json() const;
    static QuestionOption from_json(const nlohmann::json& j);
};

struct QuestionRequest {
    std::string header;      // 简短题头,可空
    std::string question;    // 完整问题
    std::vector<QuestionOption> options;
    bool multi_select = false;

    nlohmann::json to_json() const;
    static QuestionRequest from_json(const nlohmann::json& j);
};

// 前端交回的答案:选中的 label 与/或自填文本,与现有 PromptAskUser 的
// 返回形状对齐(自定义答案也排在 answers 里)。
struct QuestionResponse {
    std::vector<std::string> answers;
    // 空 answers + error 非空 = 问题被取消/无法作答(工具层据此回
    // is_error 结果,同今日 PromptAskUser 返回 unexpected 的那条路)。
    std::string error;

    nlohmann::json to_json() const;
    static QuestionResponse from_json(const nlohmann::json& j);
};

// ---- 悬挂未来 ----------------------------------------------------------------
//
// AgentLoop/工具层不直接认识 Broker;它们只认这枚同步门面(发请求、拿
// future),Broker 的线程化实现(登记 request_id、派事件、超时收口)由
// 会话层装配。实现可以包 std::future、也可以包轮询口——合同只钉"阻塞到
// 有结果或悬空收口"这一个动作;返回 nullopt = 悬空收口(cancel),等价
// 拒绝,但拒绝文案由调用方按"没人可答"写,不冒充用户拒绝(与
// on_tool_denial_text 的分工同思路)。只走其中一路的实现给另一路一个
// 返回 nullopt 的空实现即可——合同宽、实现窄。

class InteractionFuture {
public:
    virtual ~InteractionFuture() = default;

    // 审批路:工具 needs_confirm 真要问用户时。
    virtual std::optional<ApprovalResponse> WaitApproval() = 0;
    // 提问路:ask_user。审批路的实现用不上,空实现返回 nullopt。
    virtual std::optional<QuestionResponse> WaitQuestion() = 0;
};

// hooks 框架第三步:工具生命周期相位(UI 状态机 requested -> checking_hook
// -> waiting_permission -> running -> done,被拦时停在 blocked,不冒充"运行
// 过又失败")。on_tool_start 算 requested;下面几个相位由 on_tool_phase 报。
enum class ToolPhase {
    CheckingHook,      // PreToolUse 钩子跑起来了(权限确认之前)
    WaitingPermission, // 即将问用户确认(只有真要弹确认才报)
    Running,           // 钩子与确认都过了,工具真开跑
    Blocked,           // 被钩子拦下,不会执行
};

// PreToolUse 钩子的归并表态(deny > ask > allow;updatedInput 只与 allow
// 同返,改写后须重过工具 schema、deny 规则与权限判断——不许借钩子越权)。
struct ToolHookDecision {
    enum class Decision { None, Allow, Ask, Deny };
    Decision decision = Decision::None;
    std::string reason;  // deny/ask 的理由,给用户与模型看
    std::optional<nlohmann::json> updated_input;
    std::vector<std::string> additional_context;  // 给模型的追加上下文
};

}  // namespace lubancode::runtime
