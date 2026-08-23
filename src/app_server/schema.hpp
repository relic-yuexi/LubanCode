// app-server 报文形状与校验(纯函数):一行 JSON 进来,先折成三种信封
// 之一——请求(带 id/method/params)、通知(只带 method/params)、响应
// (带 id,二选一带 result 或 error)。坏报文折不成,给出稳定错误码与
// 一句人话,交回连接层去回错、退线。
//
// 校验只查信封与各方法的参数表,不执行任何业务。jsonrpc:"2.0" 字段
// 已冻结(阶段 3):入站一律不校验(带不带都认),出站不带
// (kEmitJsonRpcField = false,翻它 = 协议破坏,须 bump 版本)。
//
// 测试不许把整条黄金报文写死成字符串比对:所有断言都 parse 之后逐字段
// 查,给字段增减留活口。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/protocol.hpp"

namespace lubancode::app_server {

// ---------------------------------------------------------------------------
// 出站信封
// ---------------------------------------------------------------------------

// 一条事件(notification 形状,出站的主体):
//   {"method":"item/delta","params":{...}}
// jsonrpc 字段按 kEmitJsonRpcField 决定带不带。
nlohmann::json MakeEvent(std::string_view method, nlohmann::json params);

// 一条响应(成功):{"id":N,"result":{...}}。
nlohmann::json MakeResult(std::int64_t id, nlohmann::json result);

// 一条响应(失败):{"id":N,"error":{"code":C,"message":M,"data":D?}}。
// id 已知必须原样回;彻底解不出 id 的入站消息(parse 失败)回 null id。
// data 可给可不给,给了塞进 error 对象。
nlohmann::json MakeError(std::int64_t id, int code, std::string_view message,
                         const nlohmann::json& data = nlohmann::json());

// id 为 null 的错误响应(入站整行不是 JSON 时唯一能回的东西)。
nlohmann::json MakeErrorForUnparseable(int code, std::string_view message);

// 折好的出站信封 -> 一行 UTF-8 JSON(不带换行)。走 platform::
// DumpJsonSanitized 兜底,树里万一混进坏 UTF-8 也绝不抛异常——出站行
// 必须永远可解析。
std::string SerializeMessage(const nlohmann::json& message);

// ---------------------------------------------------------------------------
// 入站信封
// ---------------------------------------------------------------------------

// 入站消息折出来的形状。三种互斥:
//   Request    —— 带数字 id 与 method,要回响应;
//   Notification —— 只带 method,不回;
//   Response   —— 带 id 与 result/error 二选一:前端对服务端反向请求
//                 (审批/ask_user,骨架期只留位)的答复走这个形状进来。
struct IncomingRequest {
    std::int64_t id = 0;
    std::string method;
    nlohmann::json params = nlohmann::json::object(); // 缺省空对象
};

struct IncomingNotification {
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct IncomingResponse {
    std::int64_t id = 0;
    bool is_error = false;
    nlohmann::json result = nlohmann::json(); // 成功:result;失败:error 对象
};

// 折不成任何合法信封时的错。code 是稳定错误码(见 protocol.hpp),
// message 给人看。
struct EnvelopeError {
    int code = kErrInvalidRequest;
    std::string message;
    // 坏消息里如果还捞得出数字 id,带上——响应好对上号;捞不出用
    // has_id=false,回 null id。
    bool has_id = false;
    std::int64_t id = 0;
};

// 折好的入站消息。
struct IncomingMessage {
    enum class Kind { Request, Notification, Response };
    Kind kind = Kind::Notification;
    // 三种形状按 kind 取用,其余的保持默认值。
    IncomingRequest request;
    IncomingNotification notification;
    IncomingResponse response;
};

// 一行 JSON -> 入站信封。合法折出 Kind;不合法给 EnvelopeError:
//   - 整行不是 JSON(或不是对象)→ kErrParseError,id 无从谈起;
//   - 带数字 id 但没有 method(有 result/error 任一)→ Response;
//   - 带数字 id 与 method(字符串)→ Request;
//   - 只带 method(字符串)→ Notification;
//   - id 类型不对(字符串/浮点/布尔)、method 不是字符串、既没 method
//     又没 result/error → kErrInvalidRequest。
// 空 params 视为合法(方法自己的参数表再查)。
std::optional<IncomingMessage> ParseIncoming(const std::string& line, EnvelopeError& out_error);

// ---------------------------------------------------------------------------
// 各方法参数表(骨架期接线的四个:initialize/thread/start/turn/start,
// 外加 initialized/shutdown/thread/stop/thread/list 这些轻量法子)
// ---------------------------------------------------------------------------

struct InitializeParams {
    std::string client_name;    // 可选,给日志看
    std::string client_version; // 可选
};

struct ThreadStartParams {
    std::string cwd; // 可选:本场会话的工作目录(空 = 服务进程当前目录)
};

struct TurnStartParams {
    std::string thread_id;   // 必填
    std::string text;        // 必填(空串不合法;只发图片也得带一句说明文字)
    // 图片输入(阶段 3 冻结):images 数组,元素形状对齐 api::ImageBlock——
    // mediaType/data(不带 data URL 前缀的 base64)/可选 filename/width/
    // height。不传或空数组 = 纯文本。
    std::vector<nlohmann::json> images;
};

// 参数校验结果。ok=false 时 code/message 直接进错误响应。
struct ParamsCheck {
    bool ok = true;
    int code = kErrInvalidParams;
    std::string message;
};

// params 是不是对象(所有方法的底线)。不是对象直接 kErrInvalidParams。
ParamsCheck CheckParamsIsObject(const nlohmann::json& params, std::string_view method);

// initialize:无必填字段。
ParamsCheck CheckInitializeParams(const nlohmann::json& params);

// thread/start:无必填字段(cwd 可选)。
ParamsCheck CheckThreadStartParams(const nlohmann::json& params);

// turn/start:threadId(字符串)、text(字符串)两个必填;images 数组
// 可选(元素须是对象,mediaType/data 是字符串——宽松校验,深校验归执行链)。
ParamsCheck CheckTurnStartParams(const nlohmann::json& params, std::string& out_thread_id,
                                 std::string& out_text, std::vector<nlohmann::json>& out_images);

// thread/stop:threadId(字符串)必填。
ParamsCheck CheckThreadStopParams(const nlohmann::json& params, std::string& out_thread_id);

// thread/archive|unarchive|delete 共用:threadId(字符串)必填(delete 另
// 带 confirm 布尔,执行链查)。
ParamsCheck CheckThreadLifecycleParams(const nlohmann::json& params, std::string& out_thread_id);

// workflow/query:runId(字符串)必填;lastSeq 可选(0 = 全量)。
ParamsCheck CheckWorkflowQueryParams(const nlohmann::json& params, std::string& out_run_id,
                                     std::uint64_t& out_last_seq);

// trace/query(逐枚追踪单):threadId(字符串)必填;lastSeq 可选
// (0 = 全量);executionId/toolUseId/turnId 可选过滤;errorsOnly 可选
// 布尔(只回明确失败与 unknown)。
ParamsCheck CheckTraceQueryParams(const nlohmann::json& params, std::string& out_thread_id,
                                   std::uint64_t& out_last_seq);

// turn/interrupt:threadId(字符串)必填;turnId 可选(空 = 打断该 thread
// 当前在跑的回合)。
ParamsCheck CheckTurnInterruptParams(const nlohmann::json& params, std::string& out_thread_id,
                                     std::string& out_turn_id);

// ---------------------------------------------------------------------------
// 出站事件参数的拼装助手(一处拼、处处用,字段名冻结前不许散着抄)
// ---------------------------------------------------------------------------

// thread/started 的 params。session_id 是远端 SessionStore 立出来的会话 id
// (文件名安全 slug,前端拿它当 threadId 用)。cwd 回实际工作目录。
nlohmann::json MakeThreadStartedParams(const std::string& thread_id, const std::string& cwd);

// thread/stopped 的 params。
nlohmann::json MakeThreadStoppedParams(const std::string& thread_id);

// turn/started 的 params。
nlohmann::json MakeTurnStartedParams(const std::string& thread_id, const std::string& turn_id);

// turn/completed 的 params。status 见 protocol.hpp 的 kTurnStatus*。
// error_message 只在 status=error 时给(别的终态给空串,不落字段)。
nlohmann::json MakeTurnCompletedParams(const std::string& thread_id, const std::string& turn_id,
                                       std::string_view status, const std::string& error_message,
                                       const nlohmann::json& usage, int steps_used);

// item/started 的 params。item_type 见 protocol.hpp 的 kItemType*;
// item_id 由回合驱动器派发(回合内单调);payload 装条目自己的字段
// (工具名、路径、diff 之类)。
nlohmann::json MakeItemStartedParams(const std::string& thread_id, const std::string& turn_id,
                                     const std::string& item_id, std::string_view item_type,
                                     nlohmann::json payload);

// item/delta 的 params。delta_text 是这次追加的正文/思考/输出增量。
nlohmann::json MakeItemDeltaParams(const std::string& thread_id, const std::string& turn_id,
                                   const std::string& item_id, std::string_view delta_text);

// item/completed 的 params。payload 装条目的收尾字段(工具结果摘要、
// is_error、diff、usage 之类)。
nlohmann::json MakeItemCompletedParams(const std::string& thread_id, const std::string& turn_id,
                                       const std::string& item_id, nlohmann::json payload);

// queue/overflow 的 params。dropped 是这次丢掉的条目数;coalesced 是
// 靠合并省下的条目数(阶段 3 起真算)。
nlohmann::json MakeQueueOverflowParams(const std::string& thread_id, const std::string& turn_id,
                                       std::uint64_t dropped, std::uint64_t coalesced);

// turn/usage 的 params。usage 是五项 camelCase(与 turn/completed 同形)。
nlohmann::json MakeTurnUsageParams(const std::string& thread_id, const std::string& turn_id,
                                   const nlohmann::json& usage, const std::string& model);

// turn/context 的 params(上下文压力通报,字段见 ContextPressure)。
nlohmann::json MakeTurnContextParams(const std::string& thread_id, const std::string& turn_id,
                                     const nlohmann::json& context);

// thread/archive|unarchive|delete 的结果。
nlohmann::json MakeThreadLifecycleResult(const std::string& thread_id, const std::string& state);

// ---------------------------------------------------------------------------
// initialize 结果(能力表)
// ---------------------------------------------------------------------------

// 骨架期能力表:报出协议版本、LubanCode 版本、平台、以及已接线/留位的
// 方法面。前端拿它决定能调什么——不许口头宣称兼容,能做的就是接了线的。
nlohmann::json MakeInitializeResult(std::string_view lubancode_version, std::string_view platform);

// thread/list 的结果:一场一条 {threadId,startedAt,cwd,title,firstUserText,
// messageCount}。
nlohmann::json MakeThreadListResult(const std::vector<nlohmann::json>& entries);

// thread/stop 的结果。
nlohmann::json MakeThreadStoppedResult();

}  // namespace lubancode::app_server
