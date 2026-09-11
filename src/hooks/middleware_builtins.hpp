// 内置生产槽位(LuaHook 单 P0-B,§4.36/§4.48 首批逻辑槽位建议):
//   PreRequest/estimate   context.token_estimate  builtin.token_estimate_v1
//   PreRequest/capacity   context.capacity_check  builtin.capacity_check_v1
// 两枚都注册为 required:估算/容量属于功能槽位要求,用户 Lua 可同名替换
// 实现(同键高层胜出),不能靠删 handler 绕过完整请求检查。估算公式固定
// ceil(utf8Bytes/4)(scope=model_input_json_utf8_v1);容量判断消费估算结果
// 与输出预留,返回 allow/recover/reject 准入决定。
//
// runtime 侧不再有绕开 hook 的估算路径:RunPreRequestMiddleware 的估算
// 段吃的就是这里注册的获选实现;compact 运行时(并行棒)暂走旧路,后续
// 棒切同一槽位。
#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"

namespace lubancode::hooks::middleware {

// 逻辑槽位名(§4.48 首批建议)。
inline constexpr std::string_view kTokenEstimateSlot = "context.token_estimate";
inline constexpr std::string_view kCapacityCheckSlot = "context.capacity_check";
// 轨迹 v3 §4.67.8(G3):Goal 验收排程槽。与 PreRequest/control_state.project
// 同一注册池;内置实现只做"这轮验收可不可排"的决定(评估工作项的提出),
// 不在 PostTurn 栈里递归跑模型——评估请求仍经宿主内部请求服务调度留账。
// Lua 可按 (PostTurn, goal.review) 同名替换策略(组织材料/候选下一步),
// 但状态提交、预算、取消、证据准入、去重仍归宿主,不因 overwrite 绕过。
inline constexpr std::string_view kGoalReviewSlot = "goal.review";

// §4.36 的纯计算:最终模型输入快照(紧凑 JSON)的 UTF-8 字节数 ÷ 4,
// 总量一次向上取整(不对每条消息分别 ceil)。非文本块(image/audio/
// video/file 等)不参与 bytes/4:从快照里剥出后以占位引用保留类型信息,
// coverage 落 partial 并列 unestimatedModalities——不把媒体估算补 0,也
// 不拿文本字节数冒充完整预算。
//
// 返回 EST1 形状的结构化测量结果(estimator/estimatorVersion/scope/
// encoding/rounding/inputUtf8Bytes/estimatedInputTokens/coverage/
// unestimatedModalities)。
nlohmann::json ComputeUtf8BytesDiv4Estimate(const nlohmann::json& model_input_snapshot);

// 容量判断的纯计算:估算结果 + 输出预留 + 窗口 -> allow/recover/reject。
//   allow   输入估算 + 预留 + 协议余量装得下,正常发送;
//   recover 装不下但压缩/降档有望救回——runtime 调度恢复,不直接发送;
//   reject  连当前输入本身都装不下(与历史无关),要求缩短输入。
// 输入 json 键:estimatedInputTokens(或 tokenEstimate.estimatedInputTokens)
// /outputReserveTokens/contextWindowTokens;缺键按 0 计(窗口 0 = 未知,
// 不拦)。返回 {"decision": ..., "reason": ..., "estimatedInputTokens": ...,
// "outputReserveTokens": ..., "contextWindowTokens": ...}。
nlohmann::json DecideRequestCapacity(const nlohmann::json& capacity_input);

// 两枚内置定义(源层 builtin;definition_hash 添加时按声明补算)。
MiddlewareDefinition BuiltinTokenEstimateSlot();
MiddlewareDefinition BuiltinCapacityCheckSlot();

// §4.67 G3:Goal 验收排程的纯决定(宿主侧共用——内置 handler 与 goal
// 收口路都吃这一只,不因 hook 覆盖改变门槛)。输入 json 键:
//   lifecycle(string)、phase(string)、waitTaskRefs(数组)、
//   stopRequested(bool)、budgetExhausted(bool)
// 返回 {"decision": "evaluate"|"wait"|"hold", "reason": ...}:
//   evaluate 可排验收(工作轮收口、无等待、无停止、预算在);
//   wait     有相关后台任务未收口,先走等待路径(§4.67.4);
//   hold     停止意图/预算尽/停态/终态——不排,迟到结果不拉起新轮。
// 缺键按保守侧判(unknown lifecycle -> hold),不默认放行。
nlohmann::json DecideGoalReview(const nlohmann::json& review_input);

// §4.67.8(G3):PostTurn/goal.review 内置槽位定义(required:替换实现
// 不能解除门槛;返回值是决定,不携带状态提交)。
MiddlewareDefinition BuiltinGoalReviewSlot();

// 注册进池(装配层一次;§三:builtin 是默认项,同键高层胜出)。
void AddBuiltinRequestSlots(MiddlewarePool& pool);
// §4.67 G3:与请求槽同一池注册(PostTurn 挂点)。
void AddBuiltinGoalReviewSlot(MiddlewarePool& pool);

// 容量判断的协议余量(与 loop 侧 kContextPreflightHeadroomTokens 同值;
// 首版冻结 512)。
inline constexpr std::uint64_t kCapacityProtocolHeadroomTokens = 512;

}  // namespace lubancode::hooks::middleware
