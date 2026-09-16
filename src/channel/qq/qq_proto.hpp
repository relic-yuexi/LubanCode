// QQ 开放平台 v2 协议纯函数(QQ 机器人接入单 Q1)。
//
// 唯一事实依据:官方 bot.q.qq.com v2 文档(定案记入 todo §十五):
//   - 通用 payload/opcode/intents:dev-prepare/event-emit/payload.html
//   - C2C 事件:autogen/event/c2c_message_create.html(事件体三例固化为测试 fixture)
//   - 发送单聊消息:autogen/api/v2_users_user_openid_messages.post.html
//     (msg_type/msg_id/msg_seq 合同与全量错误码表)
// 官方文档与 SDK 冲突处(如富媒体上传分片起点)保留为测试案例,不静默采信。
// Ready/Resume 的 d 字段形状官方 payload 页未逐字列全,按 opcode 表语义与
// 官方 SDK 交叉核;真平台联调核验归 Q3,todo 记"未验"。
//
// 本文件零 IO:吃 JSON 吐 JSON/结构体。token 值只经参数传递,错误文案不带值。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "channel/types.hpp"

namespace lubancode::channel::qq {

// ---------------------------------------------------------------------------
// 网关 payload(官方通用数据结构页)
// ---------------------------------------------------------------------------

// intents 位(官方事件订阅表)。Q1 只订单聊事件;群聊/互动后批再开。
inline constexpr std::uint32_t kIntentGroupAndC2cEvent = 1u << 25;
// INTERACTION_CREATE(Q6 远端审批;官方事件订阅表 1<<26)。按钮回调进网关
// 事件泵,宿主裁决后经 PUT /interactions/{id} 回应。真平台是否对本账号
// 开放互动订阅——归 Q3 真机未验,文档位先订上。
inline constexpr std::uint32_t kIntentInteraction = 1u << 26;
// QQ 适配器的默认订阅:单聊消息 + 互动(审批按钮)。
inline constexpr std::uint32_t kIntentDefaultBot =
    kIntentGroupAndC2cEvent | kIntentInteraction;

enum class GatewayOp {
    Dispatch = 0,       // 服务端推送
    Heartbeat = 1,      // 客户端发,携带最新 s
    Identify = 2,       // 客户端鉴权
    Resume = 6,         // 客户端恢复连接
    Reconnect = 7,      // 服务端通知重连
    InvalidSession = 9, // identify/resume 参数错
    Hello = 10,         // 连接后首条,带心跳间隔
    HeartbeatAck = 11,  // 心跳回执
};

std::optional<GatewayOp> GatewayOpFromInt(int value);

struct GatewayPayload {
    int op_raw = -1;              // 原文 op(陌生值保留,-1 = 字段缺失)
    std::optional<GatewayOp> op;  // 认得的 op
    std::int64_t s = -1;          // 下行序号;缺省 -1(心跳未收到过任何事件时发 null)
    std::string id;               // 事件 id(外层)
    std::string t;                // 事件类型(op=0 Dispatch)
    nlohmann::json d = nlohmann::json::object();
};

// 严格解析:非 object、缺 op、op 非整数一律拒绝(错误落 *error)。d 的形状
// 随 op 而异(Hello 是 object、Heartbeat 是数字、Invalid Session 是 bool),
// 不在通用层限定——各 op 的解析函数自行校验。
std::optional<GatewayPayload> ParseGatewayPayload(const nlohmann::json& payload,
                                                  std::string* error);

// 宽松整数解析:真机教训(2026-09-15 用户 Q3 实测)——QQ 平台 JSON 不严格,
// expires_in 等数值字段可能以数字字符串("7200")回传。收数字与纯数字字符串
// (容首尾空白);浮点、带杂质的字符串、其它类型一律 nullopt,不静默截断。
std::optional<std::int64_t> ParseLooseInt64(const nlohmann::json& value);

// Hello(op=10)心跳间隔(A01 修正):官方字段是 d.heartbeat_interval(来源:
// bot.q.qq.com event-emit 页示例 {"op":10,"d":{"heartbeat_interval":45000}},
// 2026-09-17 核对;单位毫秒)。旧实现误读 heartbeat_interval_ms——历史
// 服务端从未发过此名,只作显式兼容兜底:官方字段缺失时才读,两字段并存
// 且不等时报冲突错误,不猜。缺失/类型错/非正数/超范围(> 24h 帽,防下游
// int 截断翻转)各自有明确错误文案(error 出参);返回 nullopt 即连接层
// 必须按 hello_bad_payload 断线,不得带默认间隔硬跑。
inline constexpr std::int64_t kHelloIntervalMaxMs = 86'400'000;  // 24 小时帽
std::optional<std::int64_t> ParseHelloInterval(const nlohmann::json& d,
                                               std::string* error = nullptr);

// Identify(op=2):token 前缀 "QQBot "(官方鉴权口径,AccessToken 鉴权)。
nlohmann::json BuildIdentify(const std::string& access_token, std::uint32_t intents);

// Heartbeat(op=1):d = 最新 s(没收到过事件为 null——官方 opcode 表"携带客户端
// 收到的最新的 s")。
nlohmann::json BuildHeartbeat(std::optional<std::int64_t> last_seq);

// Resume(op=6):d = {token, session_id, seq}。
nlohmann::json BuildResume(const std::string& access_token, const std::string& session_id,
                           std::int64_t seq);

// Ready(t=READY)的 d:{session_id, user{id}}。
struct GatewayReadyInfo {
    std::string session_id;
    std::string user_id;
};
std::optional<GatewayReadyInfo> ParseReady(const nlohmann::json& d, std::string* error);

// Invalid Session(op=9)的 d 是 bool:可恢复(true,重连后 Resume)/不可(false,
// 清 session 重新 Identify)。
std::optional<bool> ParseInvalidSessionResumable(const nlohmann::json& payload);

// ---------------------------------------------------------------------------
// RFC3339 时间戳(事件 timestamp 字段)
// ---------------------------------------------------------------------------

// "2026-07-21T10:00:00+08:00" / 带 Z / 带小数秒 -> epoch 毫秒。解析不动原文。
std::optional<std::int64_t> ParseRfc3339Ms(const std::string& text);

// ---------------------------------------------------------------------------
// C2C_MESSAGE_CREATE -> ChannelInboundEvent 映射
// ---------------------------------------------------------------------------

struct C2cEventMapping {
    ChannelInboundEvent event;
    std::string warnings;  // 非致命说明(卡片未展开/附件降级),进诊断不进模型正文
};

// 映射规则(Q1 首版):
//   - conversation = Direct,id 取 author.user_openid(单聊会话键);
//   - sender.id = user_openid,display_name = username,is_bot = author.bot;
//     is_owner 恒 false(宿主 allowlist 推导,sidecar 不得自称);
//   - provider_event_id = d.id + "|" + msg_idx(message_scene.ext 有则拼,无则
//     单独 d.id)——官方"相同 msg_id 可能重复推送,结合 msg_seq 去重";
//   - message_type=0/3 落 text part(content 即平台摘要);103 落 text 并置
//     hints.is_reply(引用正文 Q4 展开);其余未知类型拒绝;
//   - attachments(Q4)按官方 content_type 枚举落真类型 part(image/* ->
//     Image、video/* -> Video、voice/audio -> Audio、其余含 "file" ->
//     File);url 存 remote_ref,下载归宿主接纳服务(准入通过才拉原件),
//     白名单外类型由接纳层如实拒,映射层不虚报能解析;
//   - hints.mentions_bot = true(单聊消息天然发给 bot)。
std::optional<C2cEventMapping> MapC2cMessageCreate(const nlohmann::json& d,
                                                   const std::string& envelope_event_id,
                                                   const std::string& channel_id,
                                                   const std::string& account_id,
                                                   const std::string& delivery_id,
                                                   std::int64_t received_at_ms,
                                                   std::string* error);

// ---------------------------------------------------------------------------
// Access Token(官方鉴权:POST /app/getAppAccessToken @ bots.qq.com)
// ---------------------------------------------------------------------------

nlohmann::json BuildAccessTokenRequest(const std::string& app_id,
                                       const std::string& client_secret);

struct AccessTokenResponse {
    std::string access_token;
    std::int64_t expires_in_secs = 0;
};

// 响应 {access_token, expires_in};缺字段/空 token 拒绝。
std::optional<AccessTokenResponse> ParseAccessTokenResponse(const nlohmann::json& body,
                                                            std::string* error);

// ---------------------------------------------------------------------------
// v2 发送单聊消息
// ---------------------------------------------------------------------------

struct C2cSendRequest {
    std::string openid;              // 接收方 user_openid
    std::string content;             // 纯文本(msg_type=0);带 media 时不用
    std::string msg_id;              // 被动回复锚(来信 d.id);主动消息留空
    // 与 msg_id 联合防重;同回复重试复用同一值。0 = 调用方未指定(A05:
    // 宿主 outbox 持久分配的值 >0 时原样直达 QQ,适配器不再重选号;未指定
    // 时由 QqMessageSender 内存兜底分配——仅覆盖不走 outbox 的调用方)。
    std::uint32_t msg_seq = 0;
    std::string outbound_delivery_id;  // 宿主 delivery 账(不入平台载荷)
    // Q4 富媒体:非空 file_info 时载荷走 msg_type=7(media 字段),不带
    // content——官方示例 msg_type=7 只传 media+msg_id+msg_seq。file_info
    // 来自 /v2/users/{openid}/files(透传,不自己解码),文本与附件由
    // outbox 拆成不同段分开发送。
    std::string media_file_info;
    // Q6 审批卡片:非空 object 时载荷走 msg_type=2(markdown)+keyboard
    //(keyboard 挂在 markdown 消息底部,官方消息按钮页)。内容为宿主冻结
    // 的完整 keyboard JSON(自定义键盘,需平台开通;权限未开通按发送失败
    // 分型如实报,不静默降级为纯文本)。media 与 keyboard 互斥——装配层
    // 保证;两者同置按 keyboard 优先并留警告进 warnings 账。
    nlohmann::json keyboard = nlohmann::json::object();
};

// ---------------------------------------------------------------------------
// INTERACTION_CREATE -> 互动回调(Q6 远端审批;官方互动事件页)
// ---------------------------------------------------------------------------

// 互动事件映射(type=11 消息按钮;其余 type 只记 type 不解析 data)。
struct QqInteractionEvent {
    std::string interaction_id;   // d.id:回应接口的路径参数(不带事件名前缀)
    std::int64_t type = 0;        // 11=消息按钮;12=快捷菜单;18/19=授权……
    std::string scene;            // c2c=单聊 / group=群聊 / guild=频道
    std::int64_t chat_type = -1;  // 0=频道 1=群聊 2=单聊(-1=缺失)
    std::string user_openid;      // 单聊:操作者 OpenID(宿主身份复核用)
    std::string group_openid;     // 群聊:群 OpenID
    std::string group_member_openid;  // 群聊:群成员 OpenID(人才是身份,不是群)
    std::string button_data;      // data.resolved.button_data(宿主发的 opaque token)
    std::string button_id;        // data.resolved.button_id(可空)
    std::string application_id;   // 校验:事件所属机器人(与连接账号对账)
    std::int64_t version = 1;
    std::int64_t received_at_ms = 0;  // d.timestamp(RFC3339)折算;解析失败 0
};

// 严格映射:非 object / 缺 id / 缺 data.resolved 拒绝。数值字段宽松解析
//(ParseLooseInt64:平台可能把 version/type 发成字符串——真机教训)。
std::optional<QqInteractionEvent> MapInteractionCreate(const nlohmann::json& d,
                                                        std::string* error);

// 互动事件 -> bridge notification params(sidecar 发宿主;嵌套保留原文)。
nlohmann::json InteractionEventToJson(const QqInteractionEvent& event,
                                      const std::string& channel_id,
                                      const std::string& account_id,
                                      const std::string& delivery_id);

// PUT /interactions/{interaction_id} 的回应体:{"code": N}。
// code 官方口径:0 成功 / 1 操作失败 / 2 操作频繁 / 3 重复操作 / 4 没有权限
// / 5 仅管理员。code=0 只表示回调处理成功,不表示工具执行成功。
nlohmann::json BuildInteractionAckPayload(int code);
std::string InteractionAckPath(const std::string& interaction_id);

// POST https://api.sgroup.qq.com/v2/users/{openid}/messages 的请求体。
nlohmann::json BuildC2cSendPayload(const C2cSendRequest& request);
std::string C2cSendPath(const std::string& openid);

struct C2cSendResponse {
    std::string provider_message_id;  // 响应 id
    std::string timestamp;
    std::string ref_idx;  // ext_info.ref_idx(可空)
};
std::optional<C2cSendResponse> ParseC2cSendResponse(const nlohmann::json& body,
                                                    std::string* error);

// ---------------------------------------------------------------------------
// v2 富媒体上传(Q4;官方单聊富媒体/预上传/分片完成三页,2026-09-15 核读)
// ---------------------------------------------------------------------------

// file_type 业务类型:1=图片(png/jpg)、2=视频(mp4)、3=语音(silk)、4=文件。
// 上传请求体的 file_size 是字符串(官方字段表;SDK 1.0.4 发 number 属漂移,
// 按官方页);md5_10m = 前 10002432 字节(约 10MB)的 MD5。
nlohmann::json BuildUploadPrepareRequest(int file_type, std::int64_t file_size,
                                         const std::string& file_name, const std::string& md5,
                                         const std::string& sha1, const std::string& md5_10m);

struct UploadPreparePart {
    std::int64_t index = 0;         // 官方"从 0 开始";SDK 漂移从 1 起——
                                    // 上传循环按数组序取偏移,原值只回显
    std::string presigned_url;      // 预签名 PUT 入口(COS 签名,不带 QQ 头)
    std::int64_t block_size = 0;    // 该分块字节数(响应为字符串,宽松解析)
};

struct UploadPrepareResponse {
    std::string upload_id;
    std::int64_t block_size = 0;    // 分块基準字节数(响应为字符串,宽松解析)
    std::vector<UploadPreparePart> parts;
    // upload_config 官方在嵌套对象下(SDK 1.0.4 误读顶层,不采信);本版
    // 上传恒串行(concurrency 只记账不消费,防错误并发进循环)。
    std::int64_t concurrency = 1;
    std::int64_t retry_timeout_secs = 0;
    std::int64_t retry_delay_secs = 0;
};
std::optional<UploadPrepareResponse> ParseUploadPrepareResponse(const nlohmann::json& body,
                                                                std::string* error);

// 分片完成:{upload_id, part_index(平台原值), block_size(该片实际字节,
// 字符串), md5(该片 MD5)}。
nlohmann::json BuildUploadPartFinishRequest(const std::string& upload_id,
                                            std::int64_t part_index, std::int64_t block_size,
                                            const std::string& part_md5);
std::string UploadPreparePath(const std::string& openid);
std::string UploadPartFinishPath(const std::string& openid);

// 合并/直传:POST /v2/users/{openid}/files——本机产物走分片合并路
// (upload_id),url 路只服务于公网资源;srv_send_msg 恒 false(可靠 outbox
// 不占主动消息频次,发送另走 messages 接口)。
nlohmann::json BuildFileUploadBody(int file_type, const std::string& file_name,
                                   const std::string& upload_id);
std::string FileUploadPath(const std::string& openid);

struct FileUploadResponse {
    std::string file_uuid;
    std::string file_info;   // 透传件:发送接口 media.file_info(不解码)
    std::int64_t ttl_secs = -1;  // 有效期秒;0 = 长期;缺失 = -1(按过期处理)
};
std::optional<FileUploadResponse> ParseFileUploadResponse(const nlohmann::json& body,
                                                          std::string* error);

// ---------------------------------------------------------------------------
// 平台错误体共用解析与错误分型(A03)
//
// 官方两代错误形状并存(2026-09-17 核对):
//   - API 调用指南(api-call-guide 页):{"err_code":..,"message":..,
//     "trace_id":..};err_code 0=成功、非 0=失败;官方明言"不要依据
//     message 判定请求是否失败";trace 另经 X-Tps-trace-ID 头暴露。
//   - 发送接口错误码表(发送页):{"code":..,"message":..}。
// 数值域同源(指南失败示例即 40034005),两形状的业务码同一张分型表。
// ---------------------------------------------------------------------------

// 解析只报事实不裁决:哪个字段在、哪个非法(在但解不出——不许 value_or(0)
// 当成功)、是否冲突(两字段都合法且不等——不许猜哪个对)。
struct QqErrorBodyShape {
    bool body_is_json_object = false;
    bool has_code = false;       // 旧形状字段存在
    bool has_err_code = false;   // 官方新形状字段存在
    bool code_illegal = false;   // 字段在但 ParseLooseInt64 解不出
    bool err_code_illegal = false;
    bool conflict = false;       // 两字段均合法且值不等
    std::optional<std::int64_t> code;
    std::optional<std::int64_t> err_code;
    std::string trace_id;  // 白名单诊断;超 128 字符截断
};
QqErrorBodyShape ParseQqErrorBody(const std::string& body);

// 有效业务码:唯一字段取其值;并存等值取该值。返回 nullopt 的两种情形
// ——冲突、唯一字段非法——必须由调用方看 conflict/*_illegal 标志区分,
// 都不得折算成 0(0 = 平台报成功)。
std::optional<std::int64_t> QqErrorEffectiveCode(const QqErrorBodyShape& shape);

enum class QqApiErrorKind {
    RateLimited,         // 40034100 / HTTP 429:延后重试
    MsgIdExpired,        // 304103/40034005/40034026:回复窗口时间过期,不重试
    ReplyQuotaExhausted, // 40034128:被动回复"时间或次数"超限(官方两义并列,
                         // 不再混入 MsgIdExpired);锚点已死,不重试同锚
    Deduped,             // 40054005:同 msg_id+msg_seq 平台已收——按已送达收账
    NoFriend,            // 40054004:无好友关系,永久拒绝
    FriendCheckFailed,   // 40054006:验证好友关系失败,官方建议"重试"——可重试
    UserRejected,        // 40054013:用户拒收,永久拒绝
    BotOffline,          // 40054016:机器人已下线——状态可恢复,有限重试
    ContentRejected,     // 40034006/304061/40054007/40054018/22006/304080 及
                         // markdown/keyboard 形状族(50059/304062/40034008-11/
                         // 40034124/40034129):内容/形状永久拒绝
    PermissionDenied,    // 304004/40034105/40034127/11253:平台权限未开,永久
                         // 拒绝(申请权限前重试无意义)
    Unauthorized,        // HTTP 401/403 + 业务 11243(令牌校验不过):刷新后可重试
    ServerError,         // HTTP 5xx / 50055002 / 40034004(转存失败,官方建议
                         // 重试) / 850026/850027/40093001:可重试
    InvalidResponse,     // 2xx 但成功合同无法核对(body 解不出/码字段非法或冲突)
    NetworkError,        // 传输失败(调用方折算进来)
    UnknownError,        // 其余 4xx / 码冲突
};

struct QqApiError {
    QqApiErrorKind kind = QqApiErrorKind::UnknownError;
    int http_status = 0;
    std::int64_t platform_code = 0;  // 分型依据:有效业务码(冲突/非法时 0)
    std::int64_t platform_err_code = 0;  // 官方 err_code 字段原值(有则;分型同表)
    std::string trace_id;                // 受控诊断(截 128;不带 message)
    std::string detail;                  // 脱敏;不带 token/正文
};

// 按 HTTP 状态 + body 分型(共用错误解析器;code/err_code 两形状同表)。
// 非法/冲突的码字段不折算为成功:2xx 下判 InvalidResponse,非 2xx 落
// HTTP 状态档,detail 带字段非法/冲突记号。
QqApiError ClassifyQqSendFailure(int http_status, const std::string& body);

}  // namespace lubancode::channel::qq
