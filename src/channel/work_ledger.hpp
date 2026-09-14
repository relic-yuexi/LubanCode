// ChannelWorkLedger:渠道工作绑定账(QQ 接入单 Q2,§六第六项提交次序链
// 的"work/session/turn 绑定"一环)。
//
// 与 AutomationStore 的 occurrence claim/bind/settle 同构,但账挂渠道账号
// 目录(work claim 的 durable 底座是 ingress 状态机 queued -> running;
// 这里只补"running 的 sid 绑到了哪场哪轮"的反查账):
//   - Bind:turn 绑定窗落行(领域绑定先于 V3 gateway.work.bound——恢复器
//     优先走领域行定位原场,与 automation 的 BindOccurrence 同款次序)。
//   - 恢复裁决(泵侧):Running 的 sid 有绑定行 → 经 V3 resolver 定位原场
//     补 selection/outbox;无绑定行(claim 后崩)→ needs_review,不盲重跑。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "trajectory/journal.hpp"

namespace lubancode::channel {

class ChannelWorkLedger {
public:
    ChannelWorkLedger() = default;
    ~ChannelWorkLedger() = default;
    ChannelWorkLedger(const ChannelWorkLedger&) = delete;
    ChannelWorkLedger& operator=(const ChannelWorkLedger&) = delete;

    struct OpenResult {
        bool ok = false;
        std::string error;
    };
    // 打开(或新建)账号的绑定账。ledger_file =
    // <state_root>/<channel>/<account>/work.jsonl(调用方拼好递入)。
    static OpenResult Open(ChannelWorkLedger* out, const std::filesystem::path& ledger_file);

    bool broken() const { return broken_; }
    const std::string& last_error() const { return last_error_; }

    struct BoundWork {
        std::int64_t sid = 0;
        std::string session_key;
        std::string session_id;
        std::string turn_id;
        std::int64_t at_ms = 0;
    };
    // 落绑定(同 sid 重复绑定 = 幂等 no-op,首笔为准)。返回 false = 账写
    // 不进(调用方应按绑定缺失处置,不冒充可恢复)。
    bool Bind(std::int64_t sid, const std::string& session_key, const std::string& session_id,
              const std::string& turn_id, std::int64_t at_ms);
    std::optional<BoundWork> FindBound(std::int64_t sid) const;

private:
    std::filesystem::path ledger_file_;
    bool broken_ = false;
    mutable std::string last_error_;
    mutable std::mutex mutex_;
    std::optional<trajectory::JournalWriter> writer_;  // 首笔才开
    std::map<std::int64_t, BoundWork> bound_;
};

}  // namespace lubancode::channel
