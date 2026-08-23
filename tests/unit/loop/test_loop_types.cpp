// /loop 单第 0 期:领域类型/状态机转换表/interval 解析/错过合并账。
// 全是纯函数,fake clock 不需要(时间轴计算收 ms 整数)。

#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "runtime/loop_types.hpp"

using lubancode::runtime::loop::ComputeDueAdvance;
using lubancode::runtime::loop::ComputeResumeNextDue;
using lubancode::runtime::loop::IsLoopTransitionAllowed;
using lubancode::runtime::loop::LoopDefaults;
using lubancode::runtime::loop::LoopExpired;
using lubancode::runtime::loop::LoopPromptSource;
using lubancode::runtime::loop::LoopTask;
using lubancode::runtime::loop::LoopTaskState;
using lubancode::runtime::loop::LoopTick;
using lubancode::runtime::loop::LoopTickOutcome;
using lubancode::runtime::loop::LooksLikeLoopInterval;
using lubancode::runtime::loop::ParseLoopInterval;
using lubancode::runtime::loop::ParseLoopTaskState;
using lubancode::runtime::loop::ToString;

namespace {
constexpr std::chrono::seconds k5m{300};
constexpr std::int64_t kMs(const std::chrono::seconds s) {
    return s.count() * 1000;
}
}  // namespace

TEST_CASE("interval 解析:正路") {
    CHECK(ParseLoopInterval("1m").has_value());
    CHECK(*ParseLoopInterval("1m") == std::chrono::seconds(60));
    CHECK(*ParseLoopInterval("5m") == std::chrono::seconds(300));
    CHECK(*ParseLoopInterval("2h") == std::chrono::seconds(7200));
    CHECK(*ParseLoopInterval("1d") == std::chrono::seconds(86400));
    // 大小写不敏感。
    CHECK(*ParseLoopInterval("5M") == std::chrono::seconds(300));
    CHECK(*ParseLoopInterval("2H") == std::chrono::seconds(7200));
    CHECK(*ParseLoopInterval("1D") == std::chrono::seconds(86400));
    // 上限恰好 7d,收。
    CHECK(ParseLoopInterval("7d").has_value());
    CHECK(*ParseLoopInterval("7d") == LoopDefaults::kMaximumInterval);
}

TEST_CASE("interval 解析:歪路全拒") {
    // 0、负数、小数。
    CHECK_FALSE(ParseLoopInterval("0m").has_value());
    CHECK_FALSE(ParseLoopInterval("-5m").has_value());
    CHECK_FALSE(ParseLoopInterval("1.5h").has_value());
    // 不认秒。
    CHECK_FALSE(ParseLoopInterval("30s").has_value());
    CHECK_FALSE(ParseLoopInterval("1w").has_value());
    // 无单位、纯数字、连写、空白。
    CHECK_FALSE(ParseLoopInterval("5").has_value());
    CHECK_FALSE(ParseLoopInterval("1h30m").has_value());
    CHECK_FALSE(ParseLoopInterval(" 5m").has_value());
    CHECK_FALSE(ParseLoopInterval("5m ").has_value());
    CHECK_FALSE(ParseLoopInterval("").has_value());
    // 溢出:天数乘出 long long 也装不下的,拒(先于 7d 上限判)。
    CHECK_FALSE(ParseLoopInterval("999999999d").has_value());
    // 超过 7d。
    CHECK_FALSE(ParseLoopInterval("8d").has_value());
    CHECK_FALSE(ParseLoopInterval("10080m1").has_value());  // 单位后还有字
}

TEST_CASE("interval 形状消歧:5migrate 不是 interval") {
    CHECK(LooksLikeLoopInterval("5m"));
    CHECK(LooksLikeLoopInterval("2h"));
    CHECK_FALSE(LooksLikeLoopInterval("5migrate"));
    CHECK_FALSE(LooksLikeLoopInterval("m5"));
    CHECK_FALSE(LooksLikeLoopInterval("5"));
    CHECK_FALSE(LooksLikeLoopInterval("5mm"));
    CHECK_FALSE(LooksLikeLoopInterval(""));
}

TEST_CASE("状态机:正常三拍的路") {
    using S = LoopTaskState;
    CHECK(IsLoopTransitionAllowed(S::Active, "tick_due", S::Due));
    CHECK(IsLoopTransitionAllowed(S::Due, "tick_started", S::Running));
    CHECK(IsLoopTransitionAllowed(S::Running, "tick_finished", S::Active));
    // 审批悬着与回来。
    CHECK(IsLoopTransitionAllowed(S::Running, "permission_wait", S::WaitingPermission));
    CHECK(IsLoopTransitionAllowed(S::WaitingPermission, "permission_resolved", S::Running));
    CHECK(IsLoopTransitionAllowed(S::WaitingPermission, "tick_finished", S::Active));
    // 退避与回来。
    CHECK(IsLoopTransitionAllowed(S::Running, "backoff", S::BackingOff));
    CHECK(IsLoopTransitionAllowed(S::BackingOff, "backoff_done", S::Active));
    CHECK(IsLoopTransitionAllowed(S::BackingOff, "tick_finished", S::Active));
    // pause/resume。
    CHECK(IsLoopTransitionAllowed(S::Active, "pause", S::Paused));
    CHECK(IsLoopTransitionAllowed(S::Paused, "pause", S::Paused));  // 幂等
    CHECK(IsLoopTransitionAllowed(S::Running, "pause", S::Paused));
    CHECK(IsLoopTransitionAllowed(S::Paused, "resume", S::Active));
}

TEST_CASE("状态机:single-flight 与非法跃迁") {
    using S = LoopTaskState;
    // Running 时到点不换 Due(只记 coalesced)。
    CHECK_FALSE(IsLoopTransitionAllowed(S::Running, "tick_due", S::Due));
    CHECK_FALSE(IsLoopTransitionAllowed(S::Active, "tick_started", S::Running));
    CHECK_FALSE(IsLoopTransitionAllowed(S::Paused, "tick_due", S::Due));
    CHECK_FALSE(IsLoopTransitionAllowed(S::Paused, "resume", S::Running));
    // 认不得的 event。
    CHECK_FALSE(IsLoopTransitionAllowed(S::Active, "explode", S::Due));
    CHECK_FALSE(IsLoopTransitionAllowed(S::Active, "", S::Active));
}

TEST_CASE("状态机:终态不复活") {
    using S = LoopTaskState;
    for (const S terminal : {S::Completed, S::Cancelled, S::Expired, S::Broken}) {
        CHECK_FALSE(IsLoopTransitionAllowed(terminal, "resume", S::Active));
        CHECK_FALSE(IsLoopTransitionAllowed(terminal, "tick_due", S::Due));
        CHECK_FALSE(IsLoopTransitionAllowed(terminal, "stop", S::Active));
        // 收口补账的同态回写收(幂等落账)。
        CHECK(IsLoopTransitionAllowed(terminal, "stop", terminal));
    }
    // complete 只从运行态来。
    CHECK(IsLoopTransitionAllowed(S::Running, "complete", S::Completed));
    CHECK(IsLoopTransitionAllowed(S::Due, "complete", S::Completed));
    CHECK_FALSE(IsLoopTransitionAllowed(S::Paused, "complete", S::Completed));
}

TEST_CASE("枚举字符串:稳定且可往返") {
    using lubancode::runtime::loop::ParseLoopTickOutcome;
    using lubancode::runtime::loop::ParseLoopPromptSource;
    LoopTaskState s{};
    CHECK(ParseLoopTaskState("waiting_permission", s));
    CHECK(s == LoopTaskState::WaitingPermission);
    CHECK(ToString(s) == "waiting_permission");
    CHECK_FALSE(ParseLoopTaskState("Running", s));  // 线上形状是小写
    CHECK_FALSE(ParseLoopTaskState("bogus", s));

    LoopTickOutcome o{};
    CHECK(ParseLoopTickOutcome("provider_error", o));
    CHECK(o == LoopTickOutcome::ProviderError);
    CHECK(ToString(o) == "provider_error");
    CHECK_FALSE(ParseLoopTickOutcome("nope", o));

    LoopPromptSource p{};
    CHECK(ParseLoopPromptSource("project_file", p));
    CHECK(p == LoopPromptSource::ProjectFile);
    CHECK(ToString(p) == "project_file");
    CHECK_FALSE(ParseLoopPromptSource("dir", p));
}

TEST_CASE("LoopTask/LoopTick json 往返") {
    LoopTask t;
    t.task_id = "loop-3";
    t.session_id = "20260823-120000";
    t.prompt = "检查 CI";
    t.prompt_source = LoopPromptSource::ProjectFile;
    t.prompt_file = "D:/proj/.lubancode/loop.md";
    t.prompt_sha256 = "abc123";
    t.interval = k5m;
    t.created_at_ms = 1000;
    t.expires_at_ms = 2000;
    t.next_due_at_ms = 1500;
    t.state = LoopTaskState::Running;
    t.tick_seq = 7;
    t.run_count = 5;
    t.skipped_count = 2;
    t.consecutive_failures = 1;
    t.consecutive_denials = 0;
    t.active_turn_id = "turn-9";
    t.cwd_identity = "D:/proj";
    t.creation_seq = 3;
    const LoopTask back = LoopTask::from_json(t.to_json());
    CHECK(back.task_id == t.task_id);
    CHECK(back.prompt == t.prompt);
    CHECK(back.prompt_source == LoopPromptSource::ProjectFile);
    CHECK(back.prompt_file == t.prompt_file);
    CHECK(back.interval == t.interval);
    CHECK(back.state == LoopTaskState::Running);
    CHECK(back.tick_seq == 7);
    CHECK(back.run_count == 5);
    CHECK(back.active_turn_id.has_value());
    CHECK(*back.active_turn_id == "turn-9");
    CHECK(back.cwd_identity == "D:/proj");

    LoopTick k;
    k.task_id = "loop-3";
    k.tick_id = "loop-3#7";
    k.tick_no = 7;
    k.scheduled_at_ms = 1400;
    k.dispatched_at_ms = 1500;
    k.prompt_sha256 = "abc123";
    k.source = LoopPromptSource::Inline;
    k.turn_id = "turn-9";
    k.outcome = LoopTickOutcome::Succeeded;
    k.next_due_at_ms = 2000;
    k.missed_count = 2;
    const LoopTick kb = LoopTick::from_json(k.to_json());
    CHECK(kb.tick_id == "loop-3#7");
    CHECK(kb.tick_no == 7);
    CHECK(kb.outcome == LoopTickOutcome::Succeeded);
    CHECK(kb.missed_count == 2);
    CHECK(kb.turn_id == "turn-9");
}

TEST_CASE("错过合并:休眠两小时只合并一拍") {
    // 5 分钟任务,next_due 在 T,sleep 到 T+2h:只查一次。
    const std::int64_t due = kMs(std::chrono::seconds(10000));
    const auto adv = ComputeDueAdvance(due, due + kMs(std::chrono::seconds(7200)), k5m);
    // floor(7200s / 300s) = 24 格:第一格是本拍,中间跳过 24 格,missed = 24。
    CHECK(adv.missed == 24);
    // 新 slot 在原时间轴上:due + 25*300s(本拍之后第一个 future slot:
    // due+24 格恰等于 now,防御循环又推了一格)。
    CHECK(adv.next_due_at_ms == due + kMs(std::chrono::seconds(25 * 300)));
    CHECK(adv.next_due_at_ms > due + kMs(std::chrono::seconds(7200)));
}

TEST_CASE("错过合并:准点与边界") {
    // 恰到点:missed 0,下一格 +interval。
    const std::int64_t due = 1000000;
    auto adv = ComputeDueAdvance(due, due, k5m);
    CHECK(adv.missed == 0);
    CHECK(adv.next_due_at_ms == due + kMs(k5m));
    // 恰差一毫秒:不算 due,原样。
    adv = ComputeDueAdvance(due, due - 1, k5m);
    CHECK(adv.missed == 0);
    CHECK(adv.next_due_at_ms == due);
    // now 落在 due+interval 恰好:那格已过,missed 1,再推一格。
    adv = ComputeDueAdvance(due, due + kMs(k5m), k5m);
    CHECK(adv.missed == 1);
    CHECK(adv.next_due_at_ms == due + 2 * kMs(k5m));
}

TEST_CASE("resume 重排与 expiry 边界") {
    const std::int64_t now = 500000;
    CHECK(ComputeResumeNextDue(now, k5m) == now + kMs(k5m));
    // expiry 恰等于 now:算过。
    CHECK(LoopExpired(now, now));
    CHECK(LoopExpired(now - 1, now));
    CHECK_FALSE(LoopExpired(now + 1, now));
}
