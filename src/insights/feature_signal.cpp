#include "insights/feature_signal.hpp"

#include <algorithm>
#include <cstdio>

namespace lubancode::insights {
namespace {

int SharePercent(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) {
        return 0;
    }
    return static_cast<int>((part * 200 + whole) / (whole * 2));
}

EvidenceItem Ev(std::string metric, nlohmann::json value) {
    EvidenceItem item;
    item.metric = std::move(metric);
    item.value = std::move(value);
    return item;
}

}  // namespace

std::vector<FeatureSignal> DetectFeatureSignals(const FeatureSignalInput& input) {
    std::vector<FeatureSignal> out;
    int seq = 0;
    const auto next_id = [&seq]() {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "FS-%02d", ++seq);
        return std::string(buffer);
    };

    // §12.2 第 5 行:同一验收步骤反复手敲 → skill / workflow。
    // 先决"步骤稳定且权限边界清楚"——同类验证 ≥3 次只证步骤稳定;
    // 权限边界由主人看单定,信号里写明。
    {
        std::string hot_kind;
        std::int64_t hot_count = 0;
        for (const auto& [kind, count] : input.verification_kinds) {
            if (count > hot_count || (count == hot_count && !kind.empty() && kind < hot_kind)) {
                hot_kind = kind;
                hot_count = count;
            }
        }
        if (hot_count >= 3) {
            FeatureSignal signal;
            signal.signal_id = next_id();
            signal.feature = "/skill 或 /workflow";
            signal.summary = "同类验证(" + hot_kind + ")在这场里跑了 " +
                             std::to_string(hot_count) + " 次;步骤稳定,可固化成技能或工作流";
            signal.precondition = "步骤稳定已证(同类 ≥3 次);权限边界请主人过目";
            signal.evidence.push_back(Ev("verification_kind", hot_kind));
            signal.evidence.push_back(Ev("verification_count", hot_count));
            out.push_back(std::move(signal));
        }
    }

    // §12.2 第 3 行:system/tool 占比过高 → Prompt Profile、延迟工具、裁 MCP。
    // 先决"模块占比可定位"——manifest 的段级表就是定位账。
    if (input.total_input_tokens > 0) {
        const int tool_share =
            SharePercent(input.tool_definition_tokens, input.total_input_tokens);
        if (tool_share >= 40) {
            FeatureSignal signal;
            signal.signal_id = next_id();
            signal.feature = "Prompt Profile / 延迟工具索引(tool_search)/ 裁 MCP";
            signal.summary = "工具定义估算约占实测输入 " + std::to_string(tool_share) +
                             "%(≥40%);工具面可收窄";
            signal.precondition = "模块占比可定位(manifest 段级表在 /prompt audit)";
            signal.evidence.push_back(Ev("tool_definition_tokens_estimated",
                                         input.tool_definition_tokens));
            signal.evidence.push_back(Ev("total_input_tokens", input.total_input_tokens));
            signal.evidence.push_back(Ev("tool_share_percent", tool_share));
            out.push_back(std::move(signal));
        }
    }

    // §12.2 第 8 行:cache 前缀抖 → 固定排序、稳定段前置、动态段后置。
    // 先决"provider 明报 cache 或有诊断证据"——有实测 cache 账才算。
    if (input.prefix_breaks_same_epoch >= 2 && input.cache_read_tokens > 0) {
        FeatureSignal signal;
        signal.signal_id = next_id();
        signal.feature = "固定工具排序;稳定段前置、动态段后置";
        signal.summary = "同 epoch 内稳定前缀断了 " +
                         std::to_string(input.prefix_breaks_same_epoch) +
                         " 次,另有疑似未命中候选 " +
                         std::to_string(input.unexpected_miss_candidates) +
                         " 笔(候选;TTL 过期也长这模样)";
        signal.precondition = "本场有实测 cache 账(cache_read>0)";
        signal.evidence.push_back(Ev("prefix_breaks_same_epoch",
                                     input.prefix_breaks_same_epoch));
        signal.evidence.push_back(Ev("unexpected_miss_candidates",
                                     input.unexpected_miss_candidates));
        out.push_back(std::move(signal));
    }

    // §12.2 第 7 行:subagent token 暴涨 → 收窄工具、prompt、并发与任务。
    // 先决"子流 usage 已独立"——Trajectory 分 stream 记账,已满足。
    if (input.subagent_tokens > input.main_tokens && input.subagent_tokens > 0) {
        FeatureSignal signal;
        signal.signal_id = next_id();
        signal.feature = "收窄子代理的工具面与任务边界";
        signal.summary = "子执行 token(" + std::to_string(input.subagent_tokens) +
                         ")超过主会话(" + std::to_string(input.main_tokens) + ")";
        signal.precondition = "子流 usage 已独立(分 stream 记账)";
        signal.evidence.push_back(Ev("subagent_tokens", input.subagent_tokens));
        signal.evidence.push_back(Ev("main_tokens", input.main_tokens));
        out.push_back(std::move(signal));
    }

    return out;
}

}  // namespace lubancode::insights
