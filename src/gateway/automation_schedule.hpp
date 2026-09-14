// 周期调度引擎(常驻总装 V2 第一件事):interval/cron 两种周期形态、
// 时区与 DST 边界、occurrence 拍点计算。纯函数零 IO——不读账不写账,
// AutomationStore 与调度扫(SweepSchedule)共用一份。
//
// 纪律(单子 §七 + 总单纪律 7):
//   - cron 是手写受限子集:五字段(分 时 日 月 周),只认 * / N / A-B /
//     A-B/S / */S / 逗号并列;不认英文名、?、L、W、#、@词、六字段。
//     不支持的表达式明拒不猜(ParseCronExpr 回 nullopt)。
//   - 时区显式存储,不拿进程本地时区当隐含值。内置规则表(见下),
//     认不出的名字明拒(automation.timezone_invalid)。不引 tzdata 重型
//     依赖;表里带 DST 的区只按"现行规则"(US 2007+ / EU 1996+)算,
//     不做历史考古——首版单机单用户,如实分账。
//   - DST 边界两裁(写进 contracts.md §13,测试钉):
//       春跳缺口(gap):缺口内的墙钟拍不存在,不补不挪,直接跳过;
//         下一拍是缺口后第一个命中的墙钟。
//       秋拨重复(ambiguous):重复时段的墙钟拍只取第一次出现(较早的
//         绝对时刻);第二次出现不再单独触发。
//   - 拍点(slot)一律计划内 UTC 毫秒;occurrenceId 沿用
//     hash(jobId + revision + slot)(§11.1),时钟倒拨不重跑原 slot。
//
// interval 与时区无关(纯 UTC 锚点等差数列);时区字段对 interval/once
// 只存不参与运算,对 cron 是运算输入。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace lubancode::gateway {

// ---------------------------------------------------------------------------
// 历法:UTC 毫秒 <-> 公历(Hinnant 算法,1970 纪元)
// ---------------------------------------------------------------------------

struct CivilTime {
    std::int64_t year = 1970;
    int month = 1;    // 1..12
    int day = 1;      // 1..31
    int hour = 0;     // 0..23
    int minute = 0;   // 0..59
    int second = 0;   // 0..59
    int weekday = 4;  // 0=周日..6=周六(1970-01-01 是周四)
};

// 公历 -> UTC 毫秒(把 civil 当 UTC 解)。字段越界行为未定义——调用方
// 自产 civil(本头文件的历法函数互为闭环),不收外部任意输入。
std::int64_t CivilToUtcMs(const CivilTime& civil);
// UTC 毫秒 -> 公历(weekday 一并算好)。
CivilTime UtcMsToCivil(std::int64_t utc_ms);
int DaysInMonth(std::int64_t year, int month);  // month 1..12
// 某年某月第 n 个周几的日号(n 1..5);某年某月最后一个周几(weekday 0=周日)。
int NthWeekdayDay(std::int64_t year, int month, int nth, int weekday);
int LastWeekdayDay(std::int64_t year, int month, int weekday);

// ---------------------------------------------------------------------------
// 时区:内置规则表(明拒不猜)
// ---------------------------------------------------------------------------
//
// 表:UTC、固定偏移串(UTC+8 / UTC+08:30 / UTC-5)、Asia/Shanghai(+8 无
// DST)、Asia/Tokyo(+9 无 DST)、America/New_York(US 2007+ 规则)、
// Europe/Berlin(EU 规则,过渡点按 UTC 01:00)。
// 带不带 DST 的区一律显式;进程本地时区永不隐含。

struct ZoneRules {
    std::string name;
    int std_offset_min = 0;   // 东经为正(UTC+8 → 480)
    int dst_offset_min = 0;   // 无 DST 时与 std 相同
    bool has_dst = false;
    bool transitions_utc = false;  // true: 过渡点按 UTC 解(EU);false: 按当地墙钟(US)
    // 夏令时窗口(现行规则):spring = 第 spring_nth 个周 spring_weekday 的
    // spring_month 月 spring_minute_of_day 分(fall_nth = 0 表示最后一个)。
    // 春边按标准时墙钟解,秋边按夏令时墙钟解(US 式);EU 式两边按 UTC。
    int spring_month = 0;
    int spring_nth = 0;
    int spring_weekday = 0;  // 0=周日(现行 US/EU 规则全是周日)
    int spring_minute_of_day = 0;
    int fall_month = 0;
    int fall_nth = 0;
    int fall_weekday = 0;
    int fall_minute_of_day = 0;
};

// 名字 -> 规则。认不得(含坏固定偏移串)回 nullopt——调用方明报
// automation.timezone_invalid,不猜。
std::optional<ZoneRules> FindZone(const std::string& name);

// t 时刻该区是否在夏令时(按年算窗口;无 DST 恒 false)。
bool ZoneIsDst(const ZoneRules& zone, std::int64_t utc_ms);
// UTC 毫秒 -> 当地墙钟(含 weekday)。
CivilTime ZoneUtcToLocal(const ZoneRules& zone, std::int64_t utc_ms);

// 当地墙钟 -> UTC 的消解。
struct LocalToUtcResult {
    enum class Kind { Unique, Ambiguous, Gap };
    Kind kind = Kind::Unique;
    std::int64_t first_utc_ms = 0;   // Unique/Ambiguous: 第一次出现的时刻;
                                     // Gap: 过渡点(跳变后第一个绝对时刻)
    std::int64_t second_utc_ms = 0;  // Ambiguous: 第二次出现的时刻;其余 0
};
LocalToUtcResult ZoneLocalToUtc(const ZoneRules& zone, const CivilTime& local);

// ---------------------------------------------------------------------------
// cron 受限子集
// ---------------------------------------------------------------------------

struct CronExpr {
    std::array<bool, 60> minute{};
    std::array<bool, 24> hour{};
    std::array<bool, 32> dom{};    // 1..31(下标 0 弃)
    std::array<bool, 13> month{};  // 1..12
    std::array<bool, 7> dow{};     // 0=周日..6(cron 的 7 归一为 0)
    bool dom_star = true;
    bool dow_star = true;
};

// 五字段受限解析:字段 = 逗号并列的元素;元素 = * | N | A-B | A-B/S | */S。
// 只认数字;越界/坏形状/非五字段/空 → nullopt(明拒)。
std::optional<CronExpr> ParseCronExpr(const std::string& text);

// 某墙钟是否命中(月/日/周按 Vixie 语义:都带 * 任意;单边限定看单边;
// 双边限定取并)。
bool CronMatches(const CronExpr& expr, const CivilTime& local);

// cron + 区 -> after 之后的下一拍(UTC 毫秒)。DST 裁决见头注;两年
// (732 天)内无命中回 found=false(Feb-30 这类永不命中的表达式靠它拒)。
struct NextFire {
    bool found = false;
    std::int64_t utc_ms = 0;
};
NextFire NextCronFireUtc(const CronExpr& expr, const ZoneRules& zone,
                         std::int64_t after_ms);

// ---------------------------------------------------------------------------
// 调度规格(AutomationStore 的 job.spec;账上存原始字段,用前现解析)
// ---------------------------------------------------------------------------

enum class ScheduleKind { Once, Interval, Cron };
enum class MisfirePolicy { Coalesce, Skip };  // 补一拍(默认)| 跳过

std::string ToString(ScheduleKind kind);
bool ParseScheduleKind(const std::string& text, ScheduleKind& out);
std::string ToString(MisfirePolicy policy);
bool ParseMisfirePolicy(const std::string& text, MisfirePolicy& out);

struct ScheduleSpec {
    ScheduleKind kind = ScheduleKind::Once;
    std::int64_t due_at_ms = 0;        // once:计划 slot
    std::int64_t interval_seconds = 0; // interval:周期秒
    std::int64_t anchor_ms = 0;        // interval:锚点(slot = anchor + k*interval, k>=1)
    std::string cron_expr;             // cron:五字段原文
    std::string timezone = "UTC";      // 显式存储;cron 的运算输入
    MisfirePolicy misfire = MisfirePolicy::Coalesce;
};

// 校验(创建/更新用):interval 界 1s..10y;cron 可解析且两年内有拍;
// 时区认得。回空 = 过;回非空 = 稳定码前缀的人话。
std::string ValidateScheduleSpec(const ScheduleSpec& spec);

// after 之后的第一拍(计划内 UTC 毫秒)。once 已过 → 不出拍;规格坏
//(cron 解析失败/时区认不得)→ found=false——调用方应先过校验。
NextFire FirstSlotAfter(const ScheduleSpec& spec, std::int64_t after_ms);

}  // namespace lubancode::gateway
