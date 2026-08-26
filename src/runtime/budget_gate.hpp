// 预算闸(骨架拆解批五先行半批:外壳横切件抽公共件)。
//
// loop_scheduler、goal_coordinator、workflow/runtime 各养各的"要不要再跑
// 一轮、跑多久停"(总单病三·外壳三份)。三家语义真不同——一个管节拍、
// 一个管判定、一个管图——抽出来的是机制,不是语义:
//   - 三把尺:次数(count)/时长(elapsed)/token,哪把不设限就空着;
//     尺上的数字由各家声明(goal 的 max_iterations、workflow 的
//     max_steps/tool_calls/tokens/timeout_secs、loop 的 7d expiry)。
//   - 停因分型:核闸只报"哪把尺拦的"(BudgetStopReason),停因文案各家
//     自己拼,一字不改(行为不变铁律)。
//   - 两种核闸口径,对应两家问法的真差别:
//       Headroom:used >= limit 即拦——"下一轮装不下",开新轮之前问。
//         (goal 的 iterations/elapsed/tokens、loop 的 expiry 边界。)
//       Overrun: used > limit 才拦——"已经越帽",每步终态后对账。
//         (workflow 的 max_steps/tool_calls/tokens/timeout。)
//     两口径的差恰是"开跑前问"与"跑完后问"的差,不是随手定的。
//   - 连败/连拒/防空转/同 blocker 反复,是同一枚机制:同因连撞计数,
//     撞到阈值报 tripped,异因或成功归零。StreakMeter。
//     计数落在各家的域字段上(存档要),这里只管"怎么数、何时拦"。
//
// 依赖铁律:只认标准库,零实现依赖;不 include loop/goal/workflow 的
// 任何头——它是三家的下游,不是上游。

#pragma once

#include <cstdint>
#include <optional>

namespace lubancode::runtime {

// 停因分型:哪把尺拦的。文案不在机制件里(各家照旧)。
enum class BudgetStopReason {
    kNone,     // 三把尺都放行
    kCount,    // 次数尺
    kElapsed,  // 时长尺
    kTokens,   // token 尺
};

// 一副三尺(各家声明自己的尺;不设限的尺留空)。
struct BudgetScales {
    std::optional<std::int64_t> count;      // 次数帽(轮数/步数/调用数)
    std::optional<std::int64_t> elapsed_ms; // 时长帽(毫秒)
    std::optional<std::int64_t> tokens;     // token 帽(累计)
};

class BudgetGate {
public:
    BudgetGate() = default;
    explicit BudgetGate(BudgetScales scales) : scales_(scales) {}

    void set_scales(const BudgetScales& scales) { scales_ = scales; }
    const BudgetScales& scales() const { return scales_; }

    // ---- 合闸:三把尺按 count → elapsed → tokens 的次序问,先拦先报 ----
    // used 给 nullopt = 这把尺没账可对(provider 没报 usage、goal 还没
    // started 一类),跳过不拦。
    BudgetStopReason CheckHeadroom(std::optional<std::int64_t> count_used,
                                   std::optional<std::int64_t> elapsed_used_ms,
                                   std::optional<std::int64_t> tokens_used) const;
    BudgetStopReason CheckOverrun(std::optional<std::int64_t> count_used,
                                  std::optional<std::int64_t> elapsed_used_ms,
                                  std::optional<std::int64_t> tokens_used) const;

    // ---- 单尺:各家在自家关口分开问的场合 ----
    bool HeadroomCount(std::int64_t used) const;
    bool HeadroomElapsed(std::int64_t used_ms) const;
    bool HeadroomTokens(std::int64_t used) const;
    bool OverrunCount(std::int64_t used) const;
    bool OverrunElapsed(std::int64_t used_ms) const;
    bool OverrunTokens(std::int64_t used) const;

private:
    BudgetScales scales_;
};

// 连撞计数器(连败五拍自停/防空转/同 blocker 反复的机制件)。
// 值类型:计数存各家的域字段(要进存档),每拍取数-记结果-写回。
struct StreakMeter {
    std::int64_t threshold = 0;  // 连撞几拍算到顶;0 = 不设(永不 tripped)
    std::int64_t count = 0;      // 现值

    void NoteBad() { count += 1; }   // 同因再撞:账 +1
    void NoteGood() { count = 0; }   // 成功/换了新因:归零
    bool tripped() const { return threshold > 0 && count >= threshold; }
};

}  // namespace lubancode::runtime
