// InteractionBroker 合同(显示系统剥离单第一步:立合同,不改画面)。
//
// 审批与提问的中立通道:Runtime 发 ApprovalRequested/QuestionRequested、
// 登记 request_id,工作线程等 future;前端从任何线程拿 request_id 回答
// (ResolveApproval/AnswerQuestion)。连接线程/WebSocket 线程/Tauri relay
// 不跟着阻塞——这就是把 on_tool_confirm 的同步问话从 stdin 上解下来的
// 那颗螺钉。
//
// 约定(单子"三、审批与提问"全文照抄进类型注释):
//   1. 决定四态:accept(本次允许)/ accept_for_session(会话总允许,只写
//      本场权限账)/ decline(拒绝)/ cancel(悬空收口)。会话永久放行
//      不落盘;“顺手写进 settings.local.json”另发一枚明确命令,不藏在
//      审批回调里追问第二遍。
//   2. 回合取消、客户端断开、超时或 thread 关闭时,Broker 清掉悬空请求,
//      统一按 cancel 收口;迟到的回答对已失效 request_id 报
//      kStaleRequestId(不等、不存、不崩)。
//   3. ask_user 走同一 Broker,不另开 ReadLine 私门。
//
// 依赖铁律同 event.hpp:零实现依赖,不 include cli/app/frontend。
// 本头只放合同与单态数据;线程化的实现(Broker 本体)后续步骤落地,
// 先把形状钉死。

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 审批/提问共用一枚请求号:同一张 pending 表、同一套四态、同一套悬空
// 收口,不分两张账。
struct InteractionRequestId {
    std::string value;  // Runtime 独家分配,不重号;空串不是合法 id

    bool valid() const { return !value.empty(); }
};

// 决定四态(见文件头约定 1)。
enum class InteractionDecision {
    Accept,             // 本次允许
    AcceptForSession,   // 本会话内该工具/该问题不再问
    Decline,            // 拒绝
    Cancel,             // 悬空收口(打断/断开/超时/关 thread)
};

// ---- 审批(ApprovalRequested / ResolveApproval)----------------------------

// 一枚审批请求的领域描述:工具名 + 入参 + 前端要摆的提示。input 保持
// 原始 JSON(结构化 input 是 TurnItem 的真值,单子"四"),前端自己决定
// 怎么摘要;这里不预先翻成终端文案。
struct ApprovalRequest {
    std::string tool_name;
    nlohmann::json input = nlohmann::json::object();
    // 提示理由(钩子 ask 的 reason、权限档说明等);可空。
    std::string reason;

    nlohmann::json to_json() const;
    static ApprovalRequest from_json(const nlohmann::json& j);
};

// 前端交回的审批决定。
struct ApprovalResponse {
    InteractionDecision decision = InteractionDecision::Decline;
    // decline 时的理由(给模型看的拒绝文案线索);可空。
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

// ---- Broker 挂接点 ----------------------------------------------------------
//
// AgentLoop/工具层不直接认识 Broker;它们只认这枚同步门面(发请求、拿
// future),Broker 的线程化实现(登记 request_id、派事件、超时收口)由
// 会话层装配。终端前端先交一只"当场问完"的实现(还是 ReadChoiceMenu
// 三档菜单),远端前端再换"登记悬起"的实现——内核零改动,这就是单子
// 第 2 步"P2 Broker 先行"的形状。

// 悬挂请求的未来:实现可以包 std::future、也可以包轮询口——合同只钉
// "阻塞到有结果或悬空收口"这一个动作。wait 返回 nullopt = 悬空收口
// (cancel),等价于拒绝,但拒绝文案由调用方按"没人可答"写,不冒充
// 用户拒绝(与 on_tool_denial_text 的分工同思路)。
class InteractionFuture {
public:
    virtual ~InteractionFuture() = default;

    virtual std::optional<ApprovalResponse> WaitApproval() = 0;
    virtual std::optional<QuestionResponse> WaitQuestion() = 0;
};

// 同步门面。Ask* 在工作线程被调;返回的 future 由调用方在同一线程 Wait。
// 实现负责把 ApprovalRequested/QuestionRequested 事件发给 EventSink、
// 登记 pending 表;Resolve/Answer 从任意线程进来。
class InteractionBroker {
public:
    ~InteractionBroker() = default;

    // 发一枚审批请求(工具 needs_confirm 真要问用户时)。
    virtual std::shared_ptr<InteractionFuture> AskApproval(const ApprovalRequest& request) = 0;

    // 发一枚提问(ask_user)。
    virtual std::shared_ptr<InteractionFuture> AskQuestion(const QuestionRequest& request) = 0;

    // 前端回答(任意线程):request 已失效(答完/收口/不认识)返回 false,
    // 迟到回答不 resurrect。
    virtual bool ResolveApproval(const InteractionRequestId& id, const ApprovalResponse& response) = 0;
    virtual bool AnswerQuestion(const InteractionRequestId& id, const QuestionResponse& response) = 0;
};

// 稳定错误码:迟到/失效回答的判据(前端要拿它区分"答对了"与"答晚了")。
inline constexpr const char* kStaleRequestId = "stale_request_id";

// 枚举 <-> 稳定字符串。
std::string ToString(InteractionDecision decision);
bool ParseInteractionDecision(const std::string& s, InteractionDecision& out);

}  // namespace lubancode::runtime
