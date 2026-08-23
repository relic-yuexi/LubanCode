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

TEST_CASE("ContextTracker: Update 把 cache_read/cache_creation 也算进占用(Anthropic 语义里 input_tokens 不含缓存)") {
    cli::ContextTracker tracker(2000);
    // 实测场景:压缩后一轮 input=144、cache_read=1472,旧公式(只算
    // input+output)会报 0%(current_tokens=144),修完应报约 1600 tokens。
    tracker.Update(api::Usage{144, 0, 1472, 0});
    CHECK(tracker.current_tokens() == 1616);
    CHECK(tracker.UsagePercent() == 81);  // 1616/2000 ≈ 80.8%,四舍五入 81

    // cache_creation_tokens 同样要算进去。
    tracker.Update(api::Usage{100, 20, 300, 500});
    CHECK(tracker.current_tokens() == 920);  // 100+20+300+500
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

TEST_CASE("ContextTracker: last_cache_read_tokens 覆盖式记最近一次缓存命中") {
    cli::ContextTracker tracker(1000);
    CHECK(tracker.last_cache_read_tokens() == 0);  // 还没发过请求

    tracker.Update(api::Usage{144, 0, 1472, 0});
    CHECK(tracker.last_cache_read_tokens() == 1472);

    // 新一次 Update 整个覆盖,不累加;没命中就归 0。
    tracker.Update(api::Usage{300, 50});
    CHECK(tracker.last_cache_read_tokens() == 0);
}

TEST_CASE("ContextTracker: 命中率分母只取输入,没实测记 -1 不伪造 0%") {
    cli::ContextTracker tracker(1000);
    // 还没发过请求:总输入 0,命中率 -1(服务端未回报),不是 0。
    CHECK(tracker.last_total_input_tokens() == 0);
    CHECK(tracker.last_cache_hit_percent() == -1);

    // DeepSeek 49k hit + 1k miss:总输入 50k,命中率 98%。
    tracker.Update(api::Usage{1000, 80, 49000, 0});
    CHECK(tracker.last_total_input_tokens() == 50000);
    CHECK(tracker.last_cache_hit_percent() == 98);

    // cache_creation 也算进输入分母(Anthropic 语义)。
    tracker.Update(api::Usage{100, 20, 300, 500});
    CHECK(tracker.last_total_input_tokens() == 900);
    CHECK(tracker.last_cache_hit_percent() == 33);  // 300/900

    // 全冷未命中:命中率如实报 0,不是 -1(实测过)。
    tracker.Update(api::Usage{500, 10, 0, 0});
    CHECK(tracker.last_cache_hit_percent() == 0);
}

TEST_CASE("ContextTracker: 窗口大小是 0 时不除零,百分比按 0 处理") {
    cli::ContextTracker tracker(0);
    tracker.Update(api::Usage{100, 0});
    CHECK(tracker.UsagePercent() == 0);
    CHECK_FALSE(tracker.ShouldAutoCompact());
}

TEST_CASE("ContextTracker: 本场累计命中率跨请求累加,与最近一次分开记账") {
    cli::ContextTracker tracker(1000);
    CHECK(tracker.session_cache_hit_percent() == -1);  // 一次实测都没有
    // 第一轮冷 miss(总输入 1k,命中 0),第二轮吃缓存(总输入 2k,命中 1k):
    // 本场累计 = 命中 1k / 输入 3k = 33%;最近一次 = 1k/2k = 50%。
    tracker.ApplyUsage(api::Usage{1000, 10, 0, 0});
    tracker.ApplyUsage(api::Usage{1000, 10, 1000, 0});
    CHECK(tracker.session_cache_read_total() == 1000);
    CHECK(tracker.session_input_total() == 3000);
    CHECK(tracker.session_cache_hit_percent() == 33);
    CHECK(tracker.last_cache_hit_percent() == 50);
    // provider 没回 usage(全零):累计账不动,只标旧值。
    tracker.ApplyUsage(api::Usage{});
    CHECK(tracker.session_input_total() == 3000);
    CHECK(tracker.usage_stale());
}

TEST_CASE("ContextTracker: 服务端前缀缓存结论三态,默认未验证") {
    cli::ContextTracker tracker(1000);
    CHECK_FALSE(tracker.server_prefix_caching().has_value());  // 没读过 metrics
    tracker.set_server_prefix_caching(false);                  // /doctor cache 读到 False
    REQUIRE(tracker.server_prefix_caching().has_value());
    CHECK_FALSE(*tracker.server_prefix_caching());
    tracker.set_server_prefix_caching(std::nullopt);  // 重新变回未知
    CHECK_FALSE(tracker.server_prefix_caching().has_value());
    tracker.set_server_prefix_caching(true);
    CHECK(*tracker.server_prefix_caching());
}
