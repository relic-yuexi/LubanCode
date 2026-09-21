// SV-06 渠道令牌缓存单一状态机的并发与边界合同册:直接驱动共用件
// ExpiringTokenCache 模板(注入假 refresh,零网络零平台载荷),冻结:
//   - 同代并发:一次成功共享 / 一次失败共享(在代等待者领同款错误,
//     不再各刷一遍);
//   - 下一代独立调用允许重试——失败缓存以代次为界,绝不永久;
//   - 刷新在途时 Invalidate 不阻不炸,产出的新 token 不被旧失效清掉;
//   - 恰好到期(now == expires_at_ms 现刷,严格小于才命中)与
//     refresh_margin 边界(lifetime 等于/小于 margin 立即到期)。
//
// 并发夹具的确定性:闸门回包先报 entered 再等放行;主线程在放行前用
// now_ms 探针计数确认后来调用已进门读到代次(进门探针与条件变量等待
// 在同一临界区,计数可见即必已入队等待)。
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "channel/token_cache.hpp"

namespace lubancode::channel {
namespace {

struct TestError {
    int code = 0;
    std::string detail;
};

using CacheResult = std::expected<std::string, TestError>;
using RefreshResult = std::expected<ExpiringTokenCache<TestError>::Refreshed, TestError>;

// 可编程假刷新:按脚本序回结果;支持闸门(先报 entered、等 release 再回)。
struct ScriptedRefresh {
    struct Script {
        RefreshResult result;
        bool gate = false;
    };

    mutable std::mutex mutex;
    std::vector<Script> scripts;  // 按序消费;耗尽后恒 500 错误
    std::atomic<int> calls{0};
    std::promise<void> entered;  // 闸门刷新已进场(单次)
    std::promise<void> release;  // 放行闸门刷新(单次)
    std::future<void> release_future;

    ScriptedRefresh() { release_future = release.get_future(); }

    std::function<RefreshResult()> Func() {
        return [this]() -> RefreshResult {
            ++calls;
            Script script;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (scripts.empty()) {
                    script.result = std::unexpected(TestError{500, "exhausted"});
                } else {
                    script = scripts.front();
                    scripts.erase(scripts.begin());
                }
            }
            if (script.gate) {
                entered.set_value();
                release_future.wait();
            }
            return script.result;
        };
    }
};

struct Fixture {
    ScriptedRefresh refresh;
    std::atomic<std::int64_t> now{10'000};
    std::atomic<int> now_probes{0};  // 缓存进门查到期即探测(并发夹具定序用)

    ExpiringTokenCache<TestError> Cache(std::int64_t margin_secs = 300) {
        ExpiringTokenCache<TestError>::Options options;
        options.now_ms = [this]() {
            now_probes.fetch_add(1, std::memory_order_relaxed);
            return now.load();
        };
        options.refresh_margin_secs = margin_secs;
        options.refresh = refresh.Func();
        return ExpiringTokenCache<TestError>(std::move(options));
    }
};

void SpinUntil(int target, const std::atomic<int>& counter) {
    while (counter.load(std::memory_order_relaxed) < target) {
        std::this_thread::yield();
    }
}

RefreshResult Ok(std::string token, std::int64_t lifetime_secs) {
    return ExpiringTokenCache<TestError>::Refreshed{std::move(token), lifetime_secs};
}

}  // namespace

// ---------------------------------------------------------------------------
// 到期与余量边界(单线程)
// ---------------------------------------------------------------------------

TEST_CASE("token_cache: 未到期命中缓存;恰好到期现刷(严格小于)") {
    Fixture fixture;
    fixture.refresh.scripts.push_back({Ok("T1", 7200)});
    auto cache = fixture.Cache();
    REQUIRE(cache.GetValid().value() == "T1");
    CHECK(fixture.refresh.calls.load() == 1);
    // 7200-300=6900s:差 1ms 仍命中。
    fixture.now = 10'000 + 6'900'000 - 1;
    CHECK(cache.GetValid().value() == "T1");
    CHECK(fixture.refresh.calls.load() == 1);
    // 恰好到期(now == expires_at_ms):严格小于不成立,现刷。
    fixture.refresh.scripts.push_back({Ok("T2", 7200)});
    fixture.now = 10'000 + 6'900'000;
    CHECK(cache.GetValid().value() == "T2");
    CHECK(fixture.refresh.calls.load() == 2);
}

TEST_CASE("token_cache: lifetime 等于/小于 margin 立即到期(0 帽)") {
    {
        Fixture fixture;
        fixture.refresh.scripts.push_back({Ok("T1", 300)});
        auto cache = fixture.Cache();  // margin=300
        REQUIRE(cache.GetValid().value() == "T1");
        // 300-300=0:expires_at 即当下,下一次立刻现刷。
        fixture.refresh.scripts.push_back({Ok("T2", 300)});
        fixture.now += 1;
        CHECK(cache.GetValid().value() == "T2");
        CHECK(fixture.refresh.calls.load() == 2);
    }
    {
        // margin 大于平台报的寿命:负寿命被 0 帽住,同样立刻现刷。
        Fixture fixture;
        fixture.refresh.scripts.push_back({Ok("T1", 100)});
        auto cache = fixture.Cache();
        REQUIRE(cache.GetValid().value() == "T1");
        fixture.refresh.scripts.push_back({Ok("T2", 100)});
        CHECK(cache.GetValid().value() == "T2");
        CHECK(fixture.refresh.calls.load() == 2);
    }
}

TEST_CASE("token_cache: Invalidate 清缓存,下次现刷") {
    Fixture fixture;
    fixture.refresh.scripts.push_back({Ok("T1", 7200)});
    auto cache = fixture.Cache();
    REQUIRE(cache.GetValid().value() == "T1");
    cache.Invalidate();
    fixture.refresh.scripts.push_back({Ok("T2", 7200)});
    CHECK(cache.GetValid().value() == "T2");
    CHECK(fixture.refresh.calls.load() == 2);
}

TEST_CASE("token_cache: 失败不缓存 token;下一次调用即重试(无永久失败缓存)") {
    Fixture fixture;
    fixture.refresh.scripts.push_back(
        {std::unexpected(TestError{401, "denied"})});
    auto cache = fixture.Cache();
    const auto first = cache.GetValid();
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == 401);
    CHECK(fixture.refresh.calls.load() == 1);
    // 下一次独立调用照常重试,不领旧失败。
    fixture.refresh.scripts.push_back({Ok("T2", 7200)});
    CHECK(cache.GetValid().value() == "T2");
    CHECK(fixture.refresh.calls.load() == 2);
}

// ---------------------------------------------------------------------------
// 同代并发合同(两并发请求夹具)
// ---------------------------------------------------------------------------

TEST_CASE("token_cache: 两并发共享一次成功——一次刷新,两调用同 token") {
    Fixture fixture;
    fixture.refresh.scripts.push_back({Ok("T1", 7200)});
    auto cache = fixture.Cache();
    REQUIRE(cache.GetValid().value() == "T1");
    fixture.now += 7'000'000;  // 过窗
    fixture.refresh.scripts.push_back({Ok("T2", 7200), /*gate=*/true});
    fixture.now_probes = 0;

    std::optional<CacheResult> r1;
    std::optional<CacheResult> r2;
    std::thread leader([&] { r1 = cache.GetValid(); });
    fixture.refresh.entered.get_future().wait();
    fixture.now_probes = 0;  // leader 进场探针已花掉,重置后只数后来者
    std::thread waiter([&] { r2 = cache.GetValid(); });
    SpinUntil(1, fixture.now_probes);  // 后来者已进门读到代次,必已入队等待
    fixture.refresh.release.set_value();
    leader.join();
    waiter.join();

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1->has_value());
    REQUIRE(r2->has_value());
    CHECK(r1->value() == "T2");
    CHECK(r2->value() == "T2");
    CHECK(fixture.refresh.calls.load() == 2);  // 预热 1 + 并发 1,单飞不放大
}

TEST_CASE("token_cache: 两并发共享同代失败;下一代独立调用允许重试") {
    Fixture fixture;
    fixture.refresh.scripts.push_back({Ok("T1", 7200)});
    auto cache = fixture.Cache();
    REQUIRE(cache.GetValid().value() == "T1");
    fixture.now += 7'000'000;
    fixture.refresh.scripts.push_back(
        {std::unexpected(TestError{429, "slow down"}), /*gate=*/true});

    std::optional<CacheResult> r1;
    std::optional<CacheResult> r2;
    std::thread leader([&] { r1 = cache.GetValid(); });
    fixture.refresh.entered.get_future().wait();
    fixture.now_probes = 0;
    std::thread waiter([&] { r2 = cache.GetValid(); });
    SpinUntil(1, fixture.now_probes);
    fixture.refresh.release.set_value();
    leader.join();
    waiter.join();

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE_FALSE(r1->has_value());
    REQUIRE_FALSE(r2->has_value());
    CHECK(r1->error().code == 429);
    CHECK(r2->error().code == 429);
    CHECK(fixture.refresh.calls.load() == 2);  // 失败也只刷一次,等待者共享
    // 独立后来者:新代不领旧失败,照常重试成功。
    fixture.refresh.scripts.push_back({Ok("T3", 7200)});
    CHECK(cache.GetValid().value() == "T3");
    CHECK(fixture.refresh.calls.load() == 3);
}

TEST_CASE("token_cache: 三个并发也共享同一次失败(多名在代等待者)") {
    Fixture fixture;
    fixture.refresh.scripts.push_back({Ok("T1", 7200)});
    auto cache = fixture.Cache();
    REQUIRE(cache.GetValid().value() == "T1");
    fixture.now += 7'000'000;
    fixture.refresh.scripts.push_back(
        {std::unexpected(TestError{500, "server"}), /*gate=*/true});

    std::array<std::optional<CacheResult>, 3> results;
    std::thread leader([&] { results[0] = cache.GetValid(); });
    fixture.refresh.entered.get_future().wait();
    fixture.now_probes = 0;
    std::thread w1([&] { results[1] = cache.GetValid(); });
    std::thread w2([&] { results[2] = cache.GetValid(); });
    SpinUntil(2, fixture.now_probes);
    fixture.refresh.release.set_value();
    leader.join();
    w1.join();
    w2.join();

    for (const auto& result : results) {
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->has_value());
        CHECK(result->error().code == 500);
    }
    CHECK(fixture.refresh.calls.load() == 2);
}

// ---------------------------------------------------------------------------
// 在途 Invalidate 的代次合同
// ---------------------------------------------------------------------------

TEST_CASE("token_cache: 刷新在途时 Invalidate 不阻不炸,新 token 不被旧失效清掉") {
    Fixture fixture;
    fixture.refresh.scripts.push_back({Ok("T1", 7200)});
    auto cache = fixture.Cache();
    REQUIRE(cache.GetValid().value() == "T1");
    fixture.now += 7'000'000;
    fixture.refresh.scripts.push_back({Ok("T2", 7200), /*gate=*/true});

    std::optional<CacheResult> r1;
    std::thread leader([&] { r1 = cache.GetValid(); });
    fixture.refresh.entered.get_future().wait();
    // 旧 token 的 401 迟到:此刻刷新在途,Invalidate 即时返回(不占锁
    // 等刷新),清的是当下旧缓存;在途产出的新 token 晚于它落账。
    cache.Invalidate();
    fixture.refresh.release.set_value();
    leader.join();

    REQUIRE(r1.has_value());
    REQUIRE(r1->has_value());
    CHECK(r1->value() == "T2");
    // 新 token 存活:窗口内直接命中,无第三次刷新。
    CHECK(cache.GetValid().value() == "T2");
    CHECK(fixture.refresh.calls.load() == 2);
}

}  // namespace lubancode::channel
