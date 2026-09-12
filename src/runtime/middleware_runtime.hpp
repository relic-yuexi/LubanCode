// 中间件挂点的统一 runtime 派发点(LuaHook 单 P0-B):PreUser/PostUser/
// PreRequest(三段 mutate → freeze → estimate → capacity)经 HookDispatcher
// ::SetMiddleware 的接线缝走 P0-A 执行核。CLI(turn_runner)、one-shot、
// app-server(AgentChannelEngine)三入口共用本文件的函数,不许各接一套;
// 老 Emit 路(UserPromptSubmit/PreTurn/PreStep 等)保留给未迁移挂点,一字
// 不改。
//
// 零行为合同:dispatcher 为空或没挂中间件核(SetMiddleware 未调)时,本
// 文件所有函数返回恒等结果(dispatched=false)——与迁移前逐字节等价,
// 零注册 = 零改写。
//
// 事件账:调用方递 MiddlewareEventSink(实现见 runtime/middleware_v3_sink.hpp,
// 经会话 V3Writer 落账,一个 writer);空 = 只拿 DispatchOutcome 的 UI/诊断
// 投影,不落 v3 事件。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"

namespace lubancode::runtime {

// 一次派发的触发语境(§4.47:origin/purpose/deliveryMode 进匹配条件,
// 不扫描 wire role 猜来源;turn/step/action/request 继承触发对象身份,
// 未到对应层不虚填)。
struct MiddlewareHookContext {
    std::optional<std::string> turn_id, step_id, action_id, request_id;
    std::optional<std::string> origin;         // human/hook/skill/compact/…
    std::optional<std::string> purpose;        // interactive/title/compact/…
    std::optional<std::string> delivery_mode;  // direct/steer/followup/…
    const std::atomic<bool>* cancel = nullptr; // Esc/父任务取消旗
};

// ---------------------------------------------------------------------------
// PreUser(§4.47:队列输入获得接纳机会后、正式 user 内容提交前)
// ---------------------------------------------------------------------------

struct PreUserGate {
    bool dispatched = false;   // 中间件核真跑了(空核/空注册 = false)
    bool blocked = false;      // 业务 deny 或 required 失败:本轮不接纳
    std::string block_code;    // deny_code 或错误码
    std::string block_reason;  // 给用户看的理由
    bool rewritten = false;    // 改写被采用(prompt 与进入时不同)
    std::string prompt;        // 采用后的工作版本(未改写 = 原文)
    // 已采用的 context.append 文本(计划序;带来源前缀由调用方拼)。
    std::vector<std::string> additional_context;
    // UI/诊断投影:计划序逐项账(含跳过项)。
    hooks::middleware::DispatchOutcome outcome;
};

// user_text:候选副本入链;返回采用后的工作版本与准入。terminal = 宿主
// 接纳位(§四:PreUser 的末端返回待接纳候选,正式接纳在调用方)。
PreUserGate RunPreUserMiddleware(hooks::HookDispatcher* dispatcher, const std::string& user_text,
                                 const MiddlewareHookContext& context,
                                 hooks::middleware::MiddlewareEventSink* sink = nullptr);

// ---------------------------------------------------------------------------
// PostUser(§4.47:user 消息与接纳关系落稳后、模型请求准备前)
// ---------------------------------------------------------------------------

struct PostUserAppend {
    bool dispatched = false;
    bool blocked = false;  // required 失败/deny:原 user 保留,本轮不得发送
    std::string block_code, block_reason;
    std::vector<std::string> context_appends;  // 已采用(计划序)
    hooks::middleware::DispatchOutcome outcome;
};

PostUserAppend RunPostUserMiddleware(hooks::HookDispatcher* dispatcher, const std::string& admitted_prompt,
                                     const MiddlewareHookContext& context,
                                     hooks::middleware::MiddlewareEventSink* sink = nullptr);

// ---------------------------------------------------------------------------
// PreRequest(§4.36:每次实际模型请求,mutate → freeze → estimate → capacity)
// ---------------------------------------------------------------------------

struct PreRequestStages {
    bool dispatched = false;
    // mutate 段:改写被采用 → 本批不重建请求(reprepare_required),调用方
    // 按新输入版本重新准备(§4.36:确需改输入,换输入版本再经 hook 估算)。
    bool reprepare_required = false;
    nlohmann::json adopted_input;  // mutate 段收尾的工作版本(= 冻结快照)
    // estimate 段:获选实现的结构化测量结果(EST1 形状;空 object = 没跑)。
    nlohmann::json token_estimate;
    // capacity 段:allow/recover/reject;estimate/capacity 失败时 reject
    //(required 槽位失败,请求不得绕过检查直接发送)。
    std::string decision;  // allow/recover/reject;空 = 没跑
    std::string reason;
    bool Allowed() const { return dispatched && decision == "allow"; }
    hooks::middleware::DispatchOutcome mutate_outcome;
    hooks::middleware::DispatchOutcome estimate_outcome;
    hooks::middleware::DispatchOutcome capacity_outcome;
};

// 本次物理模型请求的冻结预算快照(V3-REAL-07):一次定形,容量 Hook、
// prepared 记账与 /context 展示全从这一对象取数。四个概念分字段,不许
// 互相冒充——
//   declared_max_output_tokens:配置/目录声明上限(524288 一类;0 = unset);
//   policy_reserve_tokens:运行策略预留(主会话输出预留封顶后的值);
//   final_reserve_tokens:本次容量判定实际使用的输出预留(extra_body
//     覆盖、应急收窄之后的值——应急已收窄,Hook 不许再拿旧预留);
//   effective_output_limit_tokens:本次实发生效的输出上限(降级/应急/
//     覆盖后的 max_tokens;0 = unset,交服务端默认)。
struct PreRequestBudget {
    std::uint64_t context_window_tokens = 0;
    std::uint64_t declared_max_output_tokens = 0;
    std::uint64_t policy_reserve_tokens = 0;
    std::uint64_t final_reserve_tokens = 0;
    std::uint64_t effective_output_limit_tokens = 0;
    std::uint64_t protocol_headroom_tokens = 0;
    bool output_limit_overridden = false;  // extra_body/应急写侧覆盖生效
};

// request_snapshot:引擎冻结的最终模型输入快照(BuildRequestSnapshotJson
// 的产物或等价形状);budget 是容量判断的预算对象(§4.36 预算分开:估算
// 只估输入)。
PreRequestStages RunPreRequestMiddleware(hooks::HookDispatcher* dispatcher,
                                         const nlohmann::json& request_snapshot,
                                         const PreRequestBudget& budget,
                                         const MiddlewareHookContext& context,
                                         hooks::middleware::MiddlewareEventSink* sink = nullptr);

// 旧双 uint64 签名的薄适配(legacy:老测试/未迁移调用方)。仅填窗口与
// 最终判定预留,其余字段留默认——新代码一律传 PreRequestBudget。
inline PreRequestStages RunPreRequestMiddleware(hooks::HookDispatcher* dispatcher,
                                                const nlohmann::json& request_snapshot,
                                                std::uint64_t context_window_tokens,
                                                std::uint64_t output_reserve_tokens,
                                                const MiddlewareHookContext& context,
                                                hooks::middleware::MiddlewareEventSink* sink = nullptr) {
    PreRequestBudget budget;
    budget.context_window_tokens = context_window_tokens;
    budget.policy_reserve_tokens = output_reserve_tokens;
    budget.final_reserve_tokens = output_reserve_tokens;
    return RunPreRequestMiddleware(dispatcher, request_snapshot, budget, context, sink);
}

// api::Request → 冻结快照(§4.36 scope=model_input_json_utf8_v1 的计量
// 对象:system、有序消息、工具定义与参数;非文本块保类型,媒体字节由
// 估算器剥离并标 unestimatedModalities)。纯投影,不碰请求本体。
nlohmann::json BuildRequestSnapshotJson(const api::Request& request);

// 装配判据:中间件核在场且 PreRequest 有获选定义(loop 的 on_pre_request_
// hooks 挂不挂看它;零注册 = 不挂,行为与从前逐字节一致)。
bool HasPreRequestMiddleware(const hooks::HookDispatcher* dispatcher);

// 装配判据:中间件核在场且任一消息挂点(PreUser/PostUser)有获选定义。
bool HasUserMiddleware(const hooks::HookDispatcher* dispatcher);

// compact 旁路请求的估算切槽(P1-C,接 P0-B 遗留②;§4.36"估算=内置
// hook"第一次覆盖旁路请求):只跑 PreRequest/estimate 段(不改输入、不进
// 容量判断),purpose 进匹配与事件账(调用方给 "compact")。用户同名替换
// 的估算器对 compact 请求同样生效——旁路不再自带第二份公式。
//   核未装配(装配失败的老路)→ 回落内置 bytes/4(与槽内置实现同一公式,
//     不因装配失败换口径);
//   核在场而估算段失败/形状不合 → 错误(fail closed,不假装核过)。
// 返回 EST1 形状(estimatedInputTokens 等)。
std::expected<nlohmann::json, std::string> EstimateBypassRequestTokens(
    hooks::HookDispatcher* dispatcher, const nlohmann::json& request_snapshot,
    const MiddlewareHookContext& context);

// UI 投影(LuaHook P0-B"一个事实账派生"):dispatch 结果 → 一行摘要 +
// json 明细(hooks 面板/诊断用;不带正文,只有身份/结局/耗时)。
nlohmann::json DescribeDispatchForUi(const hooks::middleware::DispatchOutcome& outcome);

}  // namespace lubancode::runtime
