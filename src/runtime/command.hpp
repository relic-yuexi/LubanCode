// Runtime 命令合同(显示系统剥离单第一步:立合同,不改画面)。
//
// "画面向内核说话"的唯一形状:前端只发 typed ClientCommand,拿即时
// ClientReceipt;长活另在线程跑,进度走 EventSink。不再拿 slash 字符串
// 冒充 API——slash parser 在终端适配层把文字翻成这里的命令。
//
// 形状对齐单子第 1 步与"命令与事件"一节:
//   StartThread / ResumeThread / ListThreads / ReadThread
//   StartTurn / SteerTurn / InterruptTurn
//   ResolveApproval / AnswerQuestion
//   SetModel / SetThink / SetProvider / SetLanguage
//   ClearThread / SetTitle / Compact / Export
//
// 依赖铁律同 event.hpp:只认标准库与 nlohmann/json,不 include cli/app/
// frontend,零实现依赖。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// ClientCommand(单态结构,kind 领域字段按需填——与 ServerEvent 同思路,
// 理由见 event.hpp)
// ---------------------------------------------------------------------------
enum class ClientCommandKind {
    // thread 生命周期与查询
    StartThread,
    ResumeThread,
    ListThreads,
    ReadThread,
    // thread 搬与删(会话管理器单:全经 SessionLifecycle,稳定错误码)
    ArchiveThread,
    UnarchiveThread,
    DeleteThread,
    // turn
    StartTurn,
    SteerTurn,
    InterruptTurn,
    // 交互回答(见 interaction_broker.hpp 的 request_id 约定)
    ResolveApproval,
    AnswerQuestion,
    // 设置
    SetModel,
    SetThink,
    SetProvider,
    SetLanguage,
    // thread 内容操作
    ClearThread,
    SetTitle,
    Compact,
    Export,
    // 终端专属动作(/copy、清屏、焦点……)不进协议,留在前端,这里没有
    // 它们的位置——单子"五"定死的边界。
};

struct ClientCommand {
    ClientCommandKind kind = ClientCommandKind::StartTurn;
    std::string thread_id;  // 目标会话;StartThread 可空(由 Runtime 起名)

    // 领域参数(按 kind 取用):
    //   StartTurn.text          用户输入(或图片等富内容的结构表示)
    //   SteerTurn.text          追加指令(不打断当前请求)
    //   ResumeThread.thread_ref 存档引用(id 或列表序号)
    //   ReadThread.thread_ref / .from_seq  读档起点(重连补账:from_seq+1 起)
    //   ArchiveThread/UnarchiveThread/DeleteThread.thread_id 目标会话 id;
    //     DeleteThread.payload.confirm == true 才动手(确认策略归调用方,
    //     协议不替人决定);拒绝走 error_code(confirmation_required 等)
    //   ListThreads.payload     查询形状(scope/state/sort/search/cursor/
    //     limit,单子 SessionQuery 同款);receipt.payload.threads 是结构化
    //     SessionSummary 数组
    //   ResolveApproval.*       见 interaction_broker.hpp 的决定四态
    //   AnswerQuestion.answers  选择题答案
    //   SetModel.value / SetThink.value / SetProvider.value / SetLanguage.value
    //   SetTitle.value / Export.value(目标路径)
    // 不匹配 kind 的字段序列化照带,消费方按 kind 取用。
    std::string text;
    std::string value;
    std::vector<std::string> answers;
    nlohmann::json payload = nlohmann::json::object();  // 其余结构参数

    nlohmann::json to_json() const;
    static ClientCommand from_json(const nlohmann::json& j);
};

// 命令的即时回执:只说明"收下了/没门",长活结果走事件流。
// error 为空串 = 成功;非空 = 拒绝原因(稳定 code 打头,人话跟后头)。
struct ClientReceipt {
    bool accepted = true;
    std::string error_code;         // 空 = 无错;非空时是稳定错误码
    std::string error_message;      // 人话兜底(可空)
    nlohmann::json payload = nlohmann::json::object();  // 即时数据
                                                       // (ListThreads 的清单等)

    nlohmann::json to_json() const;
    static ClientReceipt from_json(const nlohmann::json& j);
};

// 枚举 <-> 稳定字符串(理由同 event.hpp:线上是字符串,不是数字)。
std::string ToString(ClientCommandKind kind);
bool ParseClientCommandKind(const std::string& s, ClientCommandKind& out);

}  // namespace lubancode::runtime
