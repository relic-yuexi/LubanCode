// cli/context_tracker.hpp:ContextTracker 用最近一次请求的 usage 覆盖占用
// (不累加)、占用百分比计算、自动压缩阈值判断。

#include <doctest/doctest.h>

#include "cli/context_tracker.hpp"

using namespace lubancode;

TEST_CASE("ContextTracker: 初始占用为 0") {
    cli::ContextTracker tracker(1000);
    CHECK(tracker.current_tokens() == 0);
    CHECK(tracker.UsagePercent() == 0);
    CHECK_FALSE(tracker.ShouldAutoCompact());
}

TEST_CASE("ContextTracker: Update 用 input+output 覆盖当前占用") {
    cli::ContextTracker tracker(1000);
    tracker.Update(api::Usage{300, 50});
    CHECK(tracker.current_tokens() == 350);
    CHECK(tracker.UsagePercent() == 35);
}

TEST_CASE("ContextTracker: 新一次 Update 整个覆盖上一次,不是累加") {
    cli::ContextTracker tracker(1000);
    tracker.Update(api::Usage{300, 50});
    tracker.Update(api::Usage{100, 20});
    CHECK(tracker.current_tokens() == 120);
}

TEST_CASE("ContextTracker: 占用超过 80% 判定该自动压缩了") {
    cli::ContextTracker tracker(1000);
    tracker.Update(api::Usage{750, 50});  // 800/1000 = 80%,边界值,该触发
    CHECK(tracker.ShouldAutoCompact());

    tracker.Update(api::Usage{700, 50});  // 750/1000 = 75%,不该触发
    CHECK_FALSE(tracker.ShouldAutoCompact());
}

TEST_CASE("ContextTracker: set_window_tokens 会话级临时改窗口大小") {
    cli::ContextTracker tracker(1000);
    tracker.Update(api::Usage{500, 0});
    CHECK(tracker.UsagePercent() == 50);

    tracker.set_window_tokens(5000);
    CHECK(tracker.window_tokens() == 5000);
    CHECK(tracker.UsagePercent() == 10);
}

TEST_CASE("ContextTracker: 窗口大小是 0 时不除零,百分比按 0 处理") {
    cli::ContextTracker tracker(0);
    tracker.Update(api::Usage{100, 0});
    CHECK(tracker.UsagePercent() == 0);
    CHECK_FALSE(tracker.ShouldAutoCompact());
}
