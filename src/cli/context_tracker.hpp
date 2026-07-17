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
// 占用公式是 input_tokens + cache_read_tokens + cache_creation_tokens +
// output_tokens,不是单纯 input+output——Anthropic 语义里 usage.input_tokens
// 不含缓存命中/缓存写入那部分,真实提示词体积得把这两块加回来,不然大量
// 命中缓存时会把占用严重低估(细节见 context_tracker.cpp 里 Update() 的注释)。

#pragma once

#include <cstddef>

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

    std::size_t current_tokens() const { return current_tokens_; }
    std::size_t window_tokens() const { return window_tokens_; }

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
};

}  // namespace lubancode::cli
