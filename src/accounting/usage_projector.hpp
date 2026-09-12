// Usage 投影器 A0 半场(Token 账本单 §七/A0 验收)。
//
// 输入:同一条 stream 已经验过的轨迹事件(按 seq 升序);输出:每枚
// request attempt 一条 UsageSample。本件是纯投影——只读 Journal,不回写、
// 不补造事实;五层聚合(/usage 的 turn/run/session/workspace)是 A2 的活。
//
// 合账规则(§6.2):
//   - prepared 提供 purpose/provider/wire/model;usage(v2 owner 事件或
//     v1 completed.payload.usage)提供 token;缺一样标 incomplete_linkage;
//   - v1 stream:usage 从 completed.payload.usage 读,标 legacy_owner;
//     reported 位按"五项任一非零"推断,标 legacy_inferred;
//   - v2 stream:usage 只认 model.usage.recorded;completed 不复制;
//   - provider 没报(reported_by_provider=false 或 owner 事件缺席)照投
//     sample,usage_source=unknown、usage 为空——coverage 靠它数出来;
//   - 一条 stream 混 v1/v2 直接拒绝,不出残账。
//
// v3 半场(T06/V3-GAP-01,2026-09):assistant message.usage 是唯一可累计
// owner(schema §五);model.usage.appended 只作失败/迟到/更正观察,不二次
// 累计。子 session(递归发现)里 conversation 记 SubagentTurn;goal_evaluation
// 等在 RequestPurpose 无对应的用途,如实标 unmapped 不装懂。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/usage_sample.hpp"
#include "api/types.hpp"
#include "trajectory/event.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancode::accounting {

struct UsageProjection {
    bool ok = false;                  // false = stream 拒绝(version 混写等),samples 不算数
    std::string error_code;           // projection.* / schema.* 稳定码
    std::string message;
    std::vector<UsageSample> samples; // 按 stream 内出现序
    std::vector<std::string> warnings;// 不致命的缺口点名(purpose 缺、owner 缺…)
};

// 投一条 stream(全部事件,按 seq 升序)。
UsageProjection ProjectUsage(const std::vector<trajectory::EventEnvelope>& events);

// ---------------------------------------------------------------------------
// v3 半场(T06)
// ---------------------------------------------------------------------------

// v3 owner 键 -> api::Usage 五项的唯一折算口(schema §五键集;键名错一位
// tests/unit/trajectory_v3/test_v3_usage_owner_hooks.cpp 就红)。缺子项省键
// = 保持 0,不补、不猜;reasoningTokens 含在 outputTokens 里,汇总不再加。
api::Usage UsageFromV3Owner(const nlohmann::json& usage);

// v3 purpose 名(schema §1.2 MessagePurpose 线上名)→ 账本 RequestPurpose。
// conversation 按所在账分层(主账 MainTurn/子 session 账 SubagentTurn);
// goal_evaluation/context_summary/capability 在 RequestPurpose 无对应——
// 返回 nullopt,调用方标 unmapped,不硬塞近似枚举。
std::optional<RequestPurpose> MapV3Purpose(std::string_view name, bool is_subagent);

// 投一份 v3 账(单文件,不含子 session)。owner 驱动:每条模型生成的
// assistant(schema 保证带 requestId/provider/wire/model/usage)产一条
// sample;实际发出(model.request.sent)却无 owner 的请求照投 unknown
// sample(coverage 靠它数);只 prepared 未发出的不计(physical spend 按
// 实际发生)。appended 只进 warnings,单列观察。
struct V3UsageProjectorContext {
    bool is_subagent = false;  // 子 session 账:conversation 记 SubagentTurn
    std::string run_kind;      // UsageSample.run_kind 线上名(main_session/subagent/…)
};
UsageProjection ProjectV3Usage(const trajectory::v3::V3Ledger& ledger,
                               const V3UsageProjectorContext& context);

}  // namespace lubancode::accounting
