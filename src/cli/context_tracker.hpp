// ContextTracker:会话级"上下文占用"记账,给 /context 和自动 compact 用。
//
// 跟 main.cpp 里 UsageStats(按一次 RunTurn() 内所有请求的 input/output
// token 求和,统计"这一问一答总共花了多少 token",每次 RunTurn 都清零重
// 算)不是一回事——ContextTracker 只认"最近一次请求"的用量,新一次请求
// 到达就整个覆盖掉上一次的数字,不跨请求累加。这么定是因为:每次请求都是
// 把当前完整历史重新发一遍给模型,这个用量本来就已经是"这份历史占了多大"
// 的真实度量(真实用量记账,不是拿字符数瞎估),累加多次请求反而是重复
// 计数、数字会越滚越大、跟"当前历史实际占用"这件事对不上。
//
// 占用公式是 TotalInputTokens(input+cache_read+cache_creation) + output
// ——api::Usage 的统一口径下三家 wire 语义一致(input_tokens 一律是"非缓存
// 输入"),这一只公式对所有家都对。

#pragma once

#include <cstddef>
#include <cstdint>

#include "api/types.hpp"

namespace lubancode::cli {

// 占用超过窗口这个百分比时,判定"该自动压缩了"。
constexpr int kAutoCompactThresholdPercent = 80;

class ContextTracker {
public:
    explicit ContextTracker(std::size_t window_tokens);

    // 用最近一次请求的 usage 覆盖当前占用(input_tokens + cache_read_tokens +
    // cache_creation_tokens + output_tokens),不是累加——理由见文件头注释。
    void Update(const api::Usage& usage);

    // 回合内 on_usage 的统一入口:usage 带回有效实测(四项 token 不全为
    // 零)就按 Update 覆盖占用、清掉旧值标记;四项全零(provider 没在流末
    // 给 usage——现有 Usage 的全零默认值分不出"真实为零"与"字段缺失",
    // 而真实请求四项不可能全为零,按"没给"处理)时不清零、不覆盖,只把
    // 现有数字标成旧值。状态栏/状态行据 usage_stale() 带 ~ 提醒,别让人把
    // 上一次的实测当成这一次的新数;ESC/HTTP 错误路径压根不会走到
    // on_usage,自然也不会把旧数伪装成本次新值。
    void ApplyUsage(const api::Usage& usage);

    // 最近一次"请求结束"是否没有带回实测 usage(旧值标记)。一次实测都没
    // 发生过(刚启动,current_tokens 还是 0)时为 false——那时也没有数字
    // 可标旧。/context 与常驻状态行读同一只 tracker,两处口径一致。
    bool usage_stale() const { return usage_stale_; }

    std::size_t current_tokens() const { return current_tokens_; }
    std::size_t window_tokens() const { return window_tokens_; }

    // 最近一次请求的缓存命中量(usage.cache_read_tokens),跟 current_tokens
    // 一样是覆盖式,不累加;/context 分类明细在"对话历史"行尾括注用。
    // 厂商没给(或还没发过请求)就是 0。
    std::int64_t last_cache_read_tokens() const { return last_cache_read_tokens_; }

    // 最近一次请求的完整输入(TotalInputTokens),命中率分母用——只取输入,
    // 不把 output 混进去。0 = 还没实测过。
    std::int64_t last_total_input_tokens() const { return last_input_tokens_; }

    // 最近一次请求的缓存命中率(百分比,四舍五入)。分母只取输入;没实测
    // (总输入为 0)时返回 -1,调用方写"服务端未回报",不许拿 0 冒充真未命中。
    int last_cache_hit_percent() const {
        if (last_input_tokens_ <= 0) {
            return -1;
        }
        const double ratio = static_cast<double>(last_cache_read_tokens_) /
                             static_cast<double>(last_input_tokens_) * 100.0;
        return static_cast<int>(ratio + 0.5);
    }

    // /context <档位> 用:会话级临时改窗口大小,不改配置文件。
    void set_window_tokens(std::size_t window_tokens) { window_tokens_ = window_tokens; }

    // 占用百分比,四舍五入到整数;window_tokens_ 是 0 时按 0 处理(不除零、
    // 不炸)。
    int UsagePercent() const;

    // 占用是否超过 kAutoCompactThresholdPercent,该自动压缩了。
    bool ShouldAutoCompact() const;

private:
    std::size_t current_tokens_ = 0;
    std::size_t window_tokens_;
    std::int64_t last_cache_read_tokens_ = 0;
    std::int64_t last_input_tokens_ = 0;
    bool usage_stale_ = false;
};

}  // namespace lubancode::cli
