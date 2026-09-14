// 常驻总装 V2:周期调度引擎册(automation_schedule.*)。钉的合同:
//   - cron 受限子集解析:认得的形状与明拒的形状(不猜);
//   - 时区显式:内置表认得/认不得;固定偏移串;
//   - DST 边界(单子 §七"明确 DST 跳过/重复时段规则;测试不得只用
//     Asia/Shanghai"):春跳缺口拍跳过不补;秋拨重复拍只取第一次出现;
//     一个墙钟拍只发一次;
//   - interval 锚点等差(纯 UTC,与时区无关);
//   - 校验:两年内无拍的表达式明拒。
// 期望值用"公历 + 显式偏移"手算(CivilToUtcMs ± offset),不拿被测的
// zone 引擎自己算自己验——偏移选取正是被测行为。
#include <doctest/doctest.h>

#include <string>

#include "gateway/automation_schedule.hpp"

using namespace lubancode::gateway;

namespace {

// 公历墙钟 + 显式偏移(分钟,东经为正)-> UTC 毫秒。独立于 zone 引擎。
std::int64_t UtcOf(std::int64_t y, int mo, int d, int h, int mi, int offset_min) {
    CivilTime civil;
    civil.year = y;
    civil.month = mo;
    civil.day = d;
    civil.hour = h;
    civil.minute = mi;
    return CivilToUtcMs(civil) - static_cast<std::int64_t>(offset_min) * 60000;
}

NextFire Fire(const char* cron, const char* tz, std::int64_t after_ms) {
    const auto expr = ParseCronExpr(cron);
    const auto zone = FindZone(tz);
    REQUIRE(expr.has_value());
    REQUIRE(zone.has_value());
    return NextCronFireUtc(*expr, *zone, after_ms);
}

}  // namespace

TEST_CASE("历法:UTC 毫秒 <-> 公历往返;weekday 对(1970-01-01 是周四)") {
    const CivilTime civil = UtcMsToCivil(0);
    CHECK(civil.year == 1970);
    CHECK(civil.month == 1);
    CHECK(civil.day == 1);
    CHECK(civil.hour == 0);
    CHECK(civil.weekday == 4);
    const std::int64_t ms = UtcOf(2026, 6, 1, 12, 34, 0);
    const CivilTime back = UtcMsToCivil(ms);
    CHECK(back.year == 2026);
    CHECK(back.month == 6);
    CHECK(back.day == 1);
    CHECK(back.hour == 12);
    CHECK(back.minute == 34);
    CHECK(CivilToUtcMs(back) == ms);
    // 2026-03-01 是周日;2026-11-01 也是周日(DST 断言的前提)。
    CHECK(UtcMsToCivil(UtcOf(2026, 3, 1, 0, 0, 0)).weekday == 0);
    CHECK(UtcMsToCivil(UtcOf(2026, 11, 1, 0, 0, 0)).weekday == 0);
}

TEST_CASE("cron 解析:认得的形状") {
    CHECK(ParseCronExpr("*/15 * * * *").has_value());
    CHECK(ParseCronExpr("0 9 * * 1-5").has_value());
    CHECK(ParseCronExpr("30 2 1 * *").has_value());
    CHECK(ParseCronExpr("0 0 29 2 *").has_value());
    CHECK(ParseCronExpr("1,2,3 4 5 6 7").has_value());   // 列表 + dow 7
    CHECK(ParseCronExpr("0-30/10 * * * *").has_value());  // 区间步进
}

TEST_CASE("cron 解析:明拒不猜") {
    CHECK_FALSE(ParseCronExpr("60 * * * *").has_value());    // 分越界
    CHECK_FALSE(ParseCronExpr("* 25 * * *").has_value());    // 时越界
    CHECK_FALSE(ParseCronExpr("* * 0 * *").has_value());     // 日 0
    CHECK_FALSE(ParseCronExpr("* * * 13 *").has_value());    // 月越界
    CHECK_FALSE(ParseCronExpr("5 x * * *").has_value());     // 非数字
    CHECK_FALSE(ParseCronExpr("? * * * *").has_value());     // ? 不认
    CHECK_FALSE(ParseCronExpr("* * * *").has_value());       // 四字段
    CHECK_FALSE(ParseCronExpr("* * * * * *").has_value());   // 六字段
    CHECK_FALSE(ParseCronExpr("@daily").has_value());        // @词
    CHECK_FALSE(ParseCronExpr("0 9 * * 1#1").has_value());   // # 不认
    CHECK_FALSE(ParseCronExpr("5-1 * * * *").has_value());   // 倒区间
    CHECK_FALSE(ParseCronExpr("*/0 * * * *").has_value());   // 步长 0
    CHECK_FALSE(ParseCronExpr("1,,2 * * * *").has_value());  // 空元素
    CHECK_FALSE(ParseCronExpr("L * * * *").has_value());     // L 不认
}

TEST_CASE("cron 下拍:UTC 与固定偏移区(*/15、每日九点、UTC+05:30)") {
    // UTC:after 12:07 -> 12:15。
    CHECK(Fire("*/15 * * * *", "UTC", UtcOf(2026, 6, 1, 12, 7, 0)).utc_ms ==
          UtcOf(2026, 6, 1, 12, 15, 0));
    // UTC:每日 09:00。after 恰在 09:00 -> 次日(严格大于)。
    CHECK(Fire("0 9 * * *", "UTC", UtcOf(2026, 6, 1, 8, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 1, 9, 0, 0));
    CHECK(Fire("0 9 * * *", "UTC", UtcOf(2026, 6, 1, 9, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 2, 9, 0, 0));
    // Asia/Shanghai(+8,无 DST):每天九点 = 01:00Z。测试不只盯着上海,
    // 但它仍是显式存储的一区。
    CHECK(Fire("0 9 * * *", "Asia/Shanghai", UtcOf(2026, 6, 1, 0, 30, 0)).utc_ms ==
          UtcOf(2026, 6, 1, 9, 0, 480));
    // 固定偏移串 UTC+05:30:九点本地 = 03:30Z。
    CHECK(Fire("0 9 * * *", "UTC+05:30", UtcOf(2026, 6, 1, 0, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 1, 9, 0, 510));
}

TEST_CASE("cron 下拍:工作日区间与 dom/dow 并集(Vixie 语义)") {
    // "0 12 * * 1-5" 周一到周五。2026-06-01 是周一。
    CHECK(Fire("0 12 * * 1-5", "UTC", UtcOf(2026, 5, 31, 0, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 1, 12, 0, 0));  // 周日 -> 周一
    CHECK(Fire("0 12 * * 1-5", "UTC", UtcOf(2026, 6, 4, 13, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 5, 12, 0, 0));  // 周五 13:00 -> 下个周五(周六日跳过)
    // "0 0 13 * 5":13 号或周五(双边限定取并)。2026-06-12 是周五,
    // 2026-06-13 是 13 号(周六)。
    CHECK(Fire("0 0 13 * 5", "UTC", UtcOf(2026, 6, 11, 23, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 12, 0, 0, 0));  // 周五先到
    CHECK(Fire("0 0 13 * 5", "UTC", UtcOf(2026, 6, 12, 1, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 13, 0, 0, 0));  // 13 号(周六,dom 命中)
}

TEST_CASE("cron 下拍:dow 7 归一为周日") {
    CHECK(Fire("0 9 * * 7", "UTC", UtcOf(2026, 6, 4, 0, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 7, 9, 0, 0));  // 2026-06-07 是周日
    CHECK(Fire("0 9 * * 0", "UTC", UtcOf(2026, 6, 4, 0, 0, 0)).utc_ms ==
          UtcOf(2026, 6, 7, 9, 0, 0));  // 同一枚拍
}

TEST_CASE("DST 春跳缺口:America/New_York 2026-03-08 02:30 不存在,拍跳过不补") {
    const auto ny = FindZone("America/New_York");
    REQUIRE(ny.has_value());
    // 消解:02:30 墙钟在缺口里。
    const LocalToUtcResult gap =
        ZoneLocalToUtc(*ny, CivilTime{.year = 2026, .month = 3, .day = 8, .hour = 2, .minute = 30});
    CHECK(gap.kind == LocalToUtcResult::Kind::Gap);
    // 每天 02:30:03-08 的拍跳过,下一拍 = 03-09 02:30 EDT(UTC-4)。
    const NextFire fire =
        Fire("30 2 * * *", "America/New_York", UtcOf(2026, 3, 8, 0, 0, -300));
    REQUIRE(fire.found);
    CHECK(fire.utc_ms == UtcOf(2026, 3, 9, 2, 30, -240));
    // 缺口前一天照常:03-07 02:30 EST(UTC-5)。
    const NextFire before =
        Fire("30 2 * * *", "America/New_York", UtcOf(2026, 3, 6, 0, 0, -300));
    REQUIRE(before.found);
    CHECK(before.utc_ms == UtcOf(2026, 3, 6, 2, 30, -300));
    // ZoneIsDst 边界:过渡点前(06:59:59Z=01:59:59 EST)冬令时,过渡点起
    //(07:00Z=03:00 EDT)夏令时。
    CHECK_FALSE(ZoneIsDst(*ny, UtcOf(2026, 3, 8, 1, 59, -300) + 59999));
    CHECK(ZoneIsDst(*ny, UtcOf(2026, 3, 8, 2, 0, -300)));
}

TEST_CASE("DST 秋拨重复:America/New_York 2026-11-01 01:30 出现两次,只取第一次") {
    const auto ny = FindZone("America/New_York");
    REQUIRE(ny.has_value());
    const LocalToUtcResult repeated = ZoneLocalToUtc(
        *ny, CivilTime{.year = 2026, .month = 11, .day = 1, .hour = 1, .minute = 30});
    REQUIRE(repeated.kind == LocalToUtcResult::Kind::Ambiguous);
    CHECK(repeated.first_utc_ms == UtcOf(2026, 11, 1, 1, 30, -240));  // EDT 先来
    CHECK(repeated.second_utc_ms == UtcOf(2026, 11, 1, 1, 30, -300));  // EST 后到
    // 每天 01:30:从 11-01 00:00 EDT 起,第一拍 = 05:30Z(第一次出现)。
    const NextFire fire =
        Fire("30 1 * * *", "America/New_York", UtcOf(2026, 11, 1, 0, 0, -240));
    REQUIRE(fire.found);
    CHECK(fire.utc_ms == UtcOf(2026, 11, 1, 1, 30, -240));
    // 启动落在两次之间(05:30Z 之后):同一墙钟拍已发过,不补发第二次,
    // 下一拍 = 01:31 的第一次出现(EDT)。
    const NextFire mid = Fire("31 1 * * *", "America/New_York",
                              UtcOf(2026, 11, 1, 1, 30, -240) + 1);
    REQUIRE(mid.found);
    CHECK(mid.utc_ms == UtcOf(2026, 11, 1, 1, 31, -240));
    // 重复段之后的拍回到 EST:01:45 在 05:45Z(EDT)与 06:45Z(EST)各
    // 出现一次,第二次不再单独发——02:00 的拍按 EST(-5)算。
    const NextFire after_repeat =
        Fire("0 2 * * *", "America/New_York", UtcOf(2026, 11, 1, 1, 0, -300));
    REQUIRE(after_repeat.found);
    CHECK(after_repeat.utc_ms == UtcOf(2026, 11, 1, 2, 0, -300));
}

TEST_CASE("DST:Europe/Berlin(过渡点按 UTC 01:00)") {
    const auto berlin = FindZone("Europe/Berlin");
    REQUIRE(berlin.has_value());
    // 2026 春换线:03-29 01:00Z = 02:00 CET -> 03:00 CEST;02:xx 墙钟缺口。
    const LocalToUtcResult gap = ZoneLocalToUtc(
        *berlin, CivilTime{.year = 2026, .month = 3, .day = 29, .hour = 2, .minute = 30});
    CHECK(gap.kind == LocalToUtcResult::Kind::Gap);
    CHECK_FALSE(ZoneIsDst(*berlin, UtcOf(2026, 3, 29, 0, 59, 0)));
    CHECK(ZoneIsDst(*berlin, UtcOf(2026, 3, 29, 1, 1, 0)));
    const NextFire spring =
        Fire("30 2 * * *", "Europe/Berlin", UtcOf(2026, 3, 28, 12, 0, 60));
    REQUIRE(spring.found);
    CHECK(spring.utc_ms == UtcOf(2026, 3, 30, 2, 30, 120));  // 跳过 03-29,次日 CEST
    // 2026 秋换线:10-25 01:00Z = 03:00 CEST -> 02:00 CET;02:xx 重复。
    const LocalToUtcResult repeated = ZoneLocalToUtc(
        *berlin, CivilTime{.year = 2026, .month = 10, .day = 25, .hour = 2, .minute = 0});
    REQUIRE(repeated.kind == LocalToUtcResult::Kind::Ambiguous);
    CHECK(repeated.first_utc_ms == UtcOf(2026, 10, 25, 2, 0, 120));
    CHECK(repeated.second_utc_ms == UtcOf(2026, 10, 25, 2, 0, 60));
}

TEST_CASE("时区表:认不得的名字明拒;固定偏移串;无 DST 的区恒冬令时") {
    CHECK_FALSE(FindZone("Mars/Olympus").has_value());
    CHECK_FALSE(FindZone("utc+8").has_value());   // 大小写敏感
    CHECK_FALSE(FindZone("UTC+").has_value());    // 缺值
    CHECK_FALSE(FindZone("UTC+25").has_value());  // 时越界
    CHECK_FALSE(FindZone("UTC+08:70").has_value());
    CHECK(FindZone("UTC+8")->std_offset_min == 480);
    CHECK(FindZone("UTC+08:30")->std_offset_min == 510);
    CHECK(FindZone("UTC-5")->std_offset_min == -300);
    const auto shanghai = FindZone("Asia/Shanghai");
    REQUIRE(shanghai.has_value());
    CHECK_FALSE(ZoneIsDst(*shanghai, UtcOf(2026, 7, 1, 0, 0, 0)));
    // 当地墙钟(ZoneUtcToLocal)在无 DST 区就是恒偏移折算。
    const CivilTime local = ZoneUtcToLocal(*shanghai, UtcOf(2026, 6, 1, 0, 0, 0));
    CHECK(local.hour == 8);
}

TEST_CASE("校验:坏规格明拒;两年内无拍的表达式拒") {
    ScheduleSpec spec;
    spec.kind = ScheduleKind::Cron;
    spec.cron_expr = "0 9 * * 1-5";
    spec.timezone = "UTC";
    CHECK(ValidateScheduleSpec(spec).empty());
    spec.cron_expr = "0 0 30 2 *";  // 二月三十:永不命中
    CHECK_FALSE(ValidateScheduleSpec(spec).empty());
    spec.cron_expr = "0 9 * * 1-5";
    spec.timezone = "Mars/Olympus";
    CHECK(ValidateScheduleSpec(spec).rfind("automation.timezone_invalid", 0) == 0);
    spec.timezone = "UTC";
    spec.kind = ScheduleKind::Interval;
    spec.interval_seconds = 0;
    spec.anchor_ms = 1000;
    CHECK_FALSE(ValidateScheduleSpec(spec).empty());
    spec.interval_seconds = 60;
    spec.anchor_ms = 0;
    CHECK_FALSE(ValidateScheduleSpec(spec).empty());  // 须带锚点
    spec.anchor_ms = 1000;
    CHECK(ValidateScheduleSpec(spec).empty());
    spec.interval_seconds = 315360001;  // 十年零一秒
    CHECK_FALSE(ValidateScheduleSpec(spec).empty());
}

TEST_CASE("interval 拍点:锚点等差,严格大于 after;与时区无关") {
    ScheduleSpec spec;
    spec.kind = ScheduleKind::Interval;
    spec.interval_seconds = 60;
    spec.anchor_ms = 1000;
    spec.timezone = "America/New_York";  // 存了也不参与运算
    CHECK(FirstSlotAfter(spec, 500).utc_ms == 61000);
    CHECK(FirstSlotAfter(spec, 1000).utc_ms == 61000);    // 锚点本身不是拍
    CHECK(FirstSlotAfter(spec, 61000).utc_ms == 121000);  // 恰在拍上 -> 下一拍
    CHECK(FirstSlotAfter(spec, 61999).utc_ms == 121000);
    CHECK(FirstSlotAfter(spec, 120999).utc_ms == 121000);
    // once:过期不出拍。
    ScheduleSpec once;
    once.kind = ScheduleKind::Once;
    once.due_at_ms = 5000;
    CHECK(FirstSlotAfter(once, 4000).found);
    CHECK(FirstSlotAfter(once, 4000).utc_ms == 5000);
    CHECK_FALSE(FirstSlotAfter(once, 5000).found);
    CHECK_FALSE(FirstSlotAfter(once, 6000).found);
}

TEST_CASE("时钟倒拨的第一拍:同一 after 恒同一拍(纯函数),slot 稳定") {
    // 放行门的另一半(不重跑原 slot)在 store 册钉;这里钉查询语义:
    // 同一 after 恒同一拍(纯函数),游标由账管。
    ScheduleSpec spec;
    spec.kind = ScheduleKind::Interval;
    spec.interval_seconds = 60;
    spec.anchor_ms = 1000;
    const NextFire a = FirstSlotAfter(spec, 100000);
    const NextFire b = FirstSlotAfter(spec, 100000);
    REQUIRE(a.found);
    CHECK(a.utc_ms == b.utc_ms);
}
