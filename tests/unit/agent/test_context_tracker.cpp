// cli/context_tracker.hpp:ContextTracker 用最近一次请求的 usage 覆盖占用
// (不累加)、占用百分比计算、自动压缩阈值判断。

#include <doctest/doctest.h>

#include <string>

#include "agent/model_router.hpp"
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

TEST_CASE("ContextTracker: 逐请求命中历史按请求记、环形保留最近 N 次") {
    cli::ContextTracker tracker(1000);
    CHECK(tracker.cache_request_history().empty());  // 一次实测都没有
    // 三次请求:冷 miss、吃缓存、半命中。历史按时间序,每次分子分母独立。
    tracker.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-1", 0);    // 输入 1k,命中 0
    tracker.ApplyUsage(api::Usage{1000, 10, 1000, 0}, "turn-1", 1); // 输入 2k,命中 1k
    tracker.ApplyUsage(api::Usage{500, 10, 500, 0}, "turn-2", 0);   // 输入 1k,命中 0.5k
    REQUIRE(tracker.cache_request_history().size() == 3);
    CHECK(tracker.cache_request_history()[0].input_tokens == 1000);
    CHECK(tracker.cache_request_history()[0].cache_read_tokens == 0);
    CHECK(tracker.cache_request_history()[0].hit_percent() == 0);
    CHECK(tracker.cache_request_history()[1].input_tokens == 2000);
    CHECK(tracker.cache_request_history()[1].cache_read_tokens == 1000);
    CHECK(tracker.cache_request_history()[1].hit_percent() == 50);
    CHECK(tracker.cache_request_history()[2].input_tokens == 1000);
    CHECK(tracker.cache_request_history()[2].cache_read_tokens == 500);
    CHECK(tracker.cache_request_history()[2].hit_percent() == 50);
    CHECK(tracker.total_model_requests() == 3);
    // 第 4 次:未到环形上限(12),4 条都保留,按时间序。
    tracker.ApplyUsage(api::Usage{100, 0, 900, 0}, "turn-2", 1);
    REQUIRE(tracker.cache_request_history().size() == 4);
    CHECK(tracker.cache_request_history()[0].input_tokens == 1000);  // 第一次还在
    CHECK(tracker.cache_request_history()[3].input_tokens == 1000);  // 新请求在末尾
    CHECK(tracker.total_model_requests() == 4);
    // 全零 usage(provider 没回)也记一笔缺测(unreported),不整行蒸发。
    tracker.ApplyUsage(api::Usage{}, "turn-2", 2);
    REQUIRE(tracker.cache_request_history().size() == 5);
    CHECK(tracker.cache_request_history()[4].unreported);
    CHECK(tracker.cache_request_history()[4].hit_percent() == -1);  // 缺测不是 0%
    CHECK(tracker.cache_request_history()[4].step_index == 2);
    CHECK(tracker.total_model_requests() == 5);  // 缺测也是一次真实请求
    // 超出环形上限:窗口保留最近 kCacheHistorySize 次,总账不跟着挤。
    cli::ContextTracker big(1000);
    for (int i = 0; i < 20; ++i) {
        big.ApplyUsage(api::Usage{100, 0, 100, 0}, "turn-x", i);
    }
    REQUIRE(big.cache_request_history().size() == cli::ContextTracker::kCacheHistorySize);
    CHECK(big.cache_request_history().front().input_tokens == 200);  // 最旧 = 第 9 次
    CHECK(big.cache_request_history().back().input_tokens == 200);
    CHECK(big.total_model_requests() == 20);  // 总数不被 12 冒充
}

TEST_CASE("ContextTracker: 缓存记录带 turn_id 与请求序,可追外层轮次(问题 5)") {
    cli::ContextTracker tracker(1000);
    // 一条用户请求连调 5 次工具 = 同一 turn 下 6 次模型请求(step 0..5)。
    for (int step = 0; step <= 5; ++step) {
        tracker.ApplyUsage(api::Usage{1000 + step, 10, 900, 0}, "turn-3", step);
    }
    REQUIRE(tracker.cache_request_history().size() == 6);
    for (int step = 0; step <= 5; ++step) {
        CHECK(tracker.cache_request_history()[step].turn_id == "turn-3");
        CHECK(tracker.cache_request_history()[step].step_index == step);
    }
}

TEST_CASE("ContextTracker: 用户轮次登记、陌生轮次自动补号、空 id 不登记") {
    cli::ContextTracker tracker(1000);
    CHECK(tracker.FindTurnLabel("turn-1") == nullptr);
    tracker.BeginUserTurn("turn-1", "做一个图书管理系统");
    tracker.BeginUserTurn("turn-2", "为什么后台任务会退出?");
    REQUIRE(tracker.turn_labels().size() == 2);
    CHECK(tracker.turn_labels()[0].ordinal == 1);
    CHECK(tracker.turn_labels()[1].ordinal == 2);
    // 重复登记同一轮:只补标签,序号不动。
    tracker.BeginUserTurn("turn-1", "做一个图书管理系统(改)");
    REQUIRE(tracker.turn_labels().size() == 2);
    CHECK(tracker.turn_labels()[0].ordinal == 1);
    CHECK(tracker.turn_labels()[0].label.find("(改)") != std::string::npos);
    // 陌生 turn_id(单发/续跑路径没走 BeginUserTurn)由记录自动补号。
    tracker.ApplyUsage(api::Usage{100, 0, 0, 0}, "turn-9", 0);
    REQUIRE(tracker.turn_labels().size() == 3);
    CHECK(tracker.turn_labels()[2].turn_id == "turn-9");
    CHECK(tracker.turn_labels()[2].ordinal == 3);
    CHECK(tracker.turn_labels()[2].label.empty());
    // 空 turn_id 不登记。
    tracker.ApplyUsage(api::Usage{100, 0, 0, 0}, "", 0);
    CHECK(tracker.turn_labels().size() == 3);
}

TEST_CASE("ContextTracker: 场景账——纯文本直答/一次工具/多次工具/失败与打断/本地 slash/续接") {
    // 纯文本直答:一条用户输入,一次模型请求,一步到位。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "你好");
        tracker.ApplyUsage(api::Usage{500, 80, 0, 0}, "turn-1", 0);
        REQUIRE(tracker.cache_request_history().size() == 1);
        CHECK(tracker.cache_request_history()[0].turn_id == "turn-1");
        CHECK(tracker.turn_labels().size() == 1);
    }
    // 一次工具:同轮两次请求(工具前一问,工具结果回来再问)。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "读一下这个文件");
        tracker.ApplyUsage(api::Usage{500, 80, 0, 0}, "turn-1", 0);
        tracker.ApplyUsage(api::Usage{900, 60, 400, 0}, "turn-1", 1);
        REQUIRE(tracker.cache_request_history().size() == 2);
        CHECK(tracker.cache_request_history()[1].step_index == 1);
    }
    // 多次工具:5 次工具 = 6 次请求,仍然只有一个用户轮次。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "改这五个文件");
        for (int step = 0; step <= 5; ++step) {
            tracker.ApplyUsage(api::Usage{900 + step, 60, 800, 0}, "turn-1", step);
        }
        REQUIRE(tracker.cache_request_history().size() == 6);
        std::size_t distinct_turns = 0;
        std::string previous;
        for (const auto& record : tracker.cache_request_history()) {
            if (record.turn_id != previous) {
                ++distinct_turns;
                previous = record.turn_id;
            }
        }
        CHECK(distinct_turns == 1);  // 6 次模型请求,1 个用户轮次
    }
    // 请求失败/ESC 打断:请求没走完,on_usage 压根不来,不记半截账。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "会失败的一问");
        tracker.ApplyUsage(api::Usage{500, 80, 0, 0}, "turn-1", 0);  // 第一次成功
        // 第二次请求 HTTP 错误被 ESC 打断:没有 ApplyUsage,表里不添行。
        REQUIRE(tracker.cache_request_history().size() == 1);
        CHECK_FALSE(tracker.usage_stale());
    }
    // 本地 slash 命令:不发模型请求,这张请求级缓存表一条不增。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "第一问");
        tracker.ApplyUsage(api::Usage{500, 80, 0, 0}, "turn-1", 0);
        const auto size_before = tracker.cache_request_history().size();
        // /context、/help 这类本地命令不走 ApplyUsage——这里不发就是不发,
        // 表与总账都不动(钉住口径:这张表只认 provider 请求)。
        CHECK(tracker.cache_request_history().size() == size_before);
        CHECK(tracker.total_model_requests() == 1);
    }
    // compact 请求:压缩/标题这类后台采样走 ModelUsageLedger 另记,
    // 不进这张主会话逐请求表(口径:仅主会话回合请求)。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "第一问");
        tracker.ApplyUsage(api::Usage{500, 80, 0, 0}, "turn-1", 0);
        agent::ModelUsageLedger ledger;  // compact 的账在隔壁,不碰 tracker
        api::Usage compact_usage{3000, 400, 0, 0};
        ledger.Record(agent::ModelRole::Cheap, "m-cheap", compact_usage, 900, true);
        CHECK(tracker.cache_request_history().size() == 1);
        CHECK(tracker.total_model_requests() == 1);
    }
    // 续接会话:恢复后新轮次接着记,跨环形边界分组不断档。
    {
        cli::ContextTracker tracker(100000);
        tracker.BeginUserTurn("turn-1", "旧会话第一问");
        for (int step = 0; step <= 3; ++step) {
            tracker.ApplyUsage(api::Usage{900, 60, 800, 0}, "turn-1", step);
        }
        // /resume 之后的新用户输入:新一轮,新 turn_id。
        tracker.BeginUserTurn("turn-2", "续上的第二问");
        tracker.ApplyUsage(api::Usage{4000, 60, 3900, 0}, "turn-2", 0);
        REQUIRE(tracker.cache_request_history().size() == 5);
        CHECK(tracker.cache_request_history()[4].turn_id == "turn-2");
        CHECK(tracker.turn_labels().size() == 2);
    }
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

// ---- 问题 9:每请求缓存诊断账与 miss 分型 ----------------------------------

namespace {

cli::ContextTracker::CacheDiagnostics Diag(bool first, bool append_only,
                                           const char* break_reason = "") {
    cli::ContextTracker::CacheDiagnostics diag;
    diag.present = true;
    diag.cache_epoch = 1;
    diag.epoch_break_reason = break_reason;
    diag.prefix_append_only = append_only;
    diag.epoch_first_request = first;
    diag.system_hash = "0123456789abcdef";
    diag.tools_hash = "fedcba9876543210";
    diag.prefix_hash = append_only && !first ? "abcdef0123456789" : "";
    diag.stable_prefix_messages = append_only && !first ? 4 : 0;
    diag.total_messages = append_only && !first ? 6 : 2;
    diag.wire_common_prefix_bytes = -1;
    return diag;
}

}  // namespace

TEST_CASE("ClassifyMiss: 分型四态各归各,缺测最优先,本地断因先于上游结论") {
    using Kind = cli::ContextTracker::CacheMissKind;
    // 缺测:不管本地视角如何,先记"未回报",不冒充 0% 也不猜断因。
    CHECK(cli::ContextTracker::ClassifyMiss(false, 0, Diag(false, true)) == Kind::Unreported);
    CHECK(cli::ContextTracker::ClassifyMiss(false, 500, Diag(false, true)) == Kind::Unreported);

    // epoch 首请求:没有前一份可比,miss 是天然的。
    CHECK(cli::ContextTracker::ClassifyMiss(true, 0, Diag(true, true)) == Kind::FirstRequest);
    // 首请求报了命中也不改口:它不是 miss,分型按命中算。
    CHECK(cli::ContextTracker::ClassifyMiss(true, 100, Diag(true, true)) == Kind::FirstRequest);

    // 明确断 epoch:锅在本地,断因在诊断账里另有点名。
    CHECK(cli::ContextTracker::ClassifyMiss(true, 0, Diag(false, false, "tools_changed")) ==
          Kind::EpochBreak);

    // 本地前缀稳定:报了命中是命中,报零是上游没接住——锅不背到本地头上。
    CHECK(cli::ContextTracker::ClassifyMiss(true, 800, Diag(false, true)) == Kind::Hit);
    CHECK(cli::ContextTracker::ClassifyMiss(true, 0, Diag(false, true)) == Kind::UpstreamMiss);
}

TEST_CASE("ApplyUsage 带诊断账: epoch/追加律/稳定前缀逐笔记进请求账") {
    cli::ContextTracker tracker(100000);
    tracker.BeginUserTurn("turn-1", "第一问");
    // 首请求:epoch 1、无前一份。
    tracker.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-1", 0, Diag(true, true));
    // 追加请求、provider 报零:本地稳定、上游未命中。
    tracker.ApplyUsage(api::Usage{2000, 10, 0, 0}, "turn-1", 1, Diag(false, true));
    // 追加请求、报命中。
    tracker.ApplyUsage(api::Usage{2000, 10, 1500, 0}, "turn-1", 2, Diag(false, true));
    // 断 epoch(换工具表)。
    tracker.ApplyUsage(api::Usage{800, 10, 0, 0}, "turn-1", 3, Diag(false, false, "tools_changed"));
    // 缺测一笔。
    tracker.ApplyUsage(api::Usage{}, "turn-1", 4, Diag(false, true));

    const auto& history = tracker.cache_request_history();
    REQUIRE(history.size() == 5);

    CHECK(history[0].diagnostics_present);
    CHECK(history[0].epoch_first_request);
    CHECK(history[0].miss_kind == cli::ContextTracker::CacheMissKind::FirstRequest);

    CHECK(history[1].prefix_append_only);
    CHECK(history[1].stable_prefix_messages == 4);
    CHECK(history[1].total_messages == 6);
    CHECK(history[1].system_hash == "0123456789abcdef");
    CHECK(history[1].prefix_hash.size() == 16);
    CHECK(history[1].wire_common_prefix_bytes == -1);  // 诊断模式没开:不可得,不冒充 0
    CHECK(history[1].miss_kind == cli::ContextTracker::CacheMissKind::UpstreamMiss);

    CHECK(history[2].miss_kind == cli::ContextTracker::CacheMissKind::Hit);

    CHECK_FALSE(history[3].prefix_append_only);
    CHECK(history[3].epoch_break_reason == "tools_changed");
    CHECK(history[3].miss_kind == cli::ContextTracker::CacheMissKind::EpochBreak);

    CHECK(history[4].unreported);
    CHECK(history[4].miss_kind == cli::ContextTracker::CacheMissKind::Unreported);
}

TEST_CASE("ApplyUsage 缺诊断: 记账不炸,标诊断未随行,不拿默认值分型") {
    cli::ContextTracker tracker(1000);
    tracker.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-1", 0);  // 老调用点(单测/单发)
    REQUIRE(tracker.cache_request_history().size() == 1);
    CHECK_FALSE(tracker.cache_request_history()[0].diagnostics_present);
    CHECK(tracker.cache_request_history()[0].miss_kind == cli::ContextTracker::CacheMissKind::Unknown);
}
