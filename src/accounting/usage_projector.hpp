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
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "accounting/usage_sample.hpp"
#include "trajectory/event.hpp"

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

}  // namespace lubancode::accounting
