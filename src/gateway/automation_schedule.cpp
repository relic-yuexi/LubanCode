// 周期调度引擎实现(常驻总装 V2)。合同见 automation_schedule.hpp。
#include "gateway/automation_schedule.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace lubancode::gateway {

namespace {

constexpr std::int64_t kMsPerDay = 86400000;
constexpr std::int64_t kMsPerMinute = 60000;

// Hinnant days_from_civil / civil_from_days(1970-01-01 = 0)。
std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void CivilFromDays(std::int64_t z, std::int64_t* y, int* m, int* d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);                // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                       // [0, 11]
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;             // [1, 31]
    const unsigned month = mp + (mp < 10 ? 3 : -9);                // [1, 12]
    *y = year + (month <= 2 ? 1 : 0);
    *m = static_cast<int>(month);
    *d = static_cast<int>(day);
}

int WeekdayFromDays(std::int64_t days) {
    // 1970-01-01 是周四(4);0=周日。
    return static_cast<int>((days % 7 + 7 + 4) % 7);
}

bool IsLeap(std::int64_t year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int WeekdayOfDate(std::int64_t year, int month, int day) {
    return WeekdayFromDays(DaysFromCivil(year, static_cast<unsigned>(month),
                                          static_cast<unsigned>(day)));
}

// 过渡日的日号(nth: 1..5;0 = 该月最后一个 weekday)。
int TransitionDay(const ZoneRules& zone, bool spring, std::int64_t year) {
    const int month = spring ? zone.spring_month : zone.fall_month;
    const int nth = spring ? zone.spring_nth : zone.fall_nth;
    const int weekday = spring ? zone.spring_weekday : zone.fall_weekday;
    if (nth == 0) {
        return LastWeekdayDay(year, month, weekday);
    }
    return NthWeekdayDay(year, month, nth, weekday);
}

// 过渡点的绝对时刻。spring 按标准时墙钟解,fall 按夏令时墙钟解;
// transitions_utc 的区(EU)两边按 UTC 解。
std::int64_t TransitionInstantUtc(const ZoneRules& zone, bool spring, std::int64_t year) {
    CivilTime civil;
    civil.year = year;
    civil.month = spring ? zone.spring_month : zone.fall_month;
    civil.day = TransitionDay(zone, spring, year);
    civil.hour = (spring ? zone.spring_minute_of_day : zone.fall_minute_of_day) / 60;
    civil.minute = (spring ? zone.spring_minute_of_day : zone.fall_minute_of_day) % 60;
    const std::int64_t naive = CivilToUtcMs(civil);  // 墙钟数值当 UTC
    if (zone.transitions_utc) {
        return naive;
    }
    const int offset = spring ? zone.std_offset_min : zone.dst_offset_min;
    return naive - static_cast<std::int64_t>(offset) * kMsPerMinute;
}

bool SameCivil(const CivilTime& a, const CivilTime& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour &&
           a.minute == b.minute && a.second == b.second;
}

// ---- cron 字段解析 -----------------------------------------------------------

struct FieldBounds {
    int min;
    int max;
};

bool ParseNumber(const std::string& text, int* out) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    *out = std::atoi(text.c_str());
    return true;
}

// 一个字段(逗号并列的元素) -> 命中槽位表。normalize_dow7: dow 的 7 归一
// 为 0(周日)。回 false = 形状坏,明拒。
bool ParseCronField(const std::string& token, const FieldBounds& bounds,
                    std::vector<bool>& slots, bool normalize_dow7) {
    slots.assign(static_cast<std::size_t>(bounds.max) + 1, false);
    std::size_t pos = 0;
    bool any = false;
    while (true) {
        const std::size_t comma = token.find(',', pos);
        const std::string element =
            token.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (element.empty()) {
            return false;
        }
        std::string range = element;
        int step = 1;
        const std::size_t slash = element.find('/');
        if (slash != std::string::npos) {
            range = element.substr(0, slash);
            if (!ParseNumber(element.substr(slash + 1), &step) || step < 1) {
                return false;
            }
        }
        int lo = bounds.min;
        int hi = bounds.max;
        if (range != "*") {
            const std::size_t dash = range.find('-');
            if (dash != std::string::npos) {
                if (!ParseNumber(range.substr(0, dash), &lo) ||
                    !ParseNumber(range.substr(dash + 1), &hi)) {
                    return false;
                }
            } else if (!ParseNumber(range, &lo)) {
                return false;
            } else {
                hi = lo;
            }
            if (lo < bounds.min || hi > bounds.max || lo > hi) {
                return false;
            }
        }
        for (int v = lo; v <= hi; v += step) {
            int slot = v;
            if (normalize_dow7 && v == 7) {
                slot = 0;
            }
            slots[static_cast<std::size_t>(slot)] = true;
            any = true;
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return any;
}

}  // namespace

std::int64_t CivilToUtcMs(const CivilTime& civil) {
    const std::int64_t days = DaysFromCivil(civil.year, static_cast<unsigned>(civil.month),
                                            static_cast<unsigned>(civil.day));
    return days * kMsPerDay + civil.hour * 3600000 + civil.minute * kMsPerMinute +
           civil.second * 1000;
}

CivilTime UtcMsToCivil(std::int64_t utc_ms) {
    const std::int64_t days = utc_ms / kMsPerDay;
    const std::int64_t rem = utc_ms - days * kMsPerDay;
    CivilTime civil;
    int month = 0;
    int day = 0;
    CivilFromDays(days, &civil.year, &month, &day);
    civil.month = month;
    civil.day = day;
    civil.hour = static_cast<int>(rem / 3600000);
    civil.minute = static_cast<int>((rem % 3600000) / kMsPerMinute);
    civil.second = static_cast<int>((rem % kMsPerMinute) / 1000);
    civil.weekday = WeekdayFromDays(days);
    return civil;
}

int DaysInMonth(std::int64_t year, int month) {
    static const int kDays[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeap(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 0;
    }
    return kDays[month];
}

int NthWeekdayDay(std::int64_t year, int month, int nth, int weekday) {
    if (nth < 1 || nth > 5 || month < 1 || month > 12) {
        return 0;
    }
    const int first_weekday = WeekdayOfDate(year, month, 1);
    const int day = 1 + (weekday - first_weekday + 7) % 7 + 7 * (nth - 1);
    if (day > DaysInMonth(year, month)) {
        return 0;  // 该月没有第 n 个(内置规则的组合不会撞;防御)
    }
    return day;
}

int LastWeekdayDay(std::int64_t year, int month, int weekday) {
    const int last = DaysInMonth(year, month);
    if (last == 0) {
        return 0;
    }
    return last - ((WeekdayOfDate(year, month, last) - weekday + 7) % 7);
}

std::optional<ZoneRules> FindZone(const std::string& name) {
    if (name == "UTC") {
        ZoneRules zone;
        zone.name = name;
        return zone;
    }
    if (name == "Asia/Shanghai") {
        ZoneRules zone;
        zone.name = name;
        zone.std_offset_min = 480;
        zone.dst_offset_min = 480;
        return zone;
    }
    if (name == "Asia/Tokyo") {
        ZoneRules zone;
        zone.name = name;
        zone.std_offset_min = 540;
        zone.dst_offset_min = 540;
        return zone;
    }
    if (name == "America/New_York") {
        // US 2007+:三月第二个周日 02:00(标准时)起,十一月第一个周日
        // 02:00(夏令时)止。
        ZoneRules zone;
        zone.name = name;
        zone.std_offset_min = -300;
        zone.dst_offset_min = -240;
        zone.has_dst = true;
        zone.spring_month = 3;
        zone.spring_nth = 2;
        zone.spring_weekday = 0;
        zone.spring_minute_of_day = 2 * 60;
        zone.fall_month = 11;
        zone.fall_nth = 1;
        zone.fall_weekday = 0;
        zone.fall_minute_of_day = 2 * 60;
        return zone;
    }
    if (name == "Europe/Berlin") {
        // EU:三月最后一个周日 01:00 UTC 起,十月最后一个周日 01:00 UTC 止。
        ZoneRules zone;
        zone.name = name;
        zone.std_offset_min = 60;
        zone.dst_offset_min = 120;
        zone.has_dst = true;
        zone.transitions_utc = true;
        zone.spring_month = 3;
        zone.spring_nth = 0;  // 最后一个
        zone.spring_weekday = 0;
        zone.spring_minute_of_day = 60;
        zone.fall_month = 10;
        zone.fall_nth = 0;
        zone.fall_weekday = 0;
        zone.fall_minute_of_day = 60;
        return zone;
    }
    // 固定偏移串:UTC+8 / UTC+08:30 / UTC-5(分钟粒度;不带 DST)。
    if (name.rfind("UTC", 0) == 0 && name.size() > 3) {
        const char sign = name[3];
        if (sign != '+' && sign != '-') {
            return std::nullopt;
        }
        const std::string rest = name.substr(4);
        const std::size_t colon = rest.find(':');
        int hours = 0;
        int minutes = 0;
        auto parse_digits = [](const std::string& text, int max, int* out) {
            if (text.empty() || text.size() > 2 ||
                text.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            *out = std::atoi(text.c_str());
            return *out >= 0 && *out <= max;
        };
        if (colon == std::string::npos) {
            if (!parse_digits(rest, 14, &hours)) {
                return std::nullopt;
            }
        } else if (!parse_digits(rest.substr(0, colon), 14, &hours) ||
                   !parse_digits(rest.substr(colon + 1), 59, &minutes)) {
            return std::nullopt;
        }
        ZoneRules zone;
        zone.name = name;
        zone.std_offset_min = (hours * 60 + minutes) * (sign == '-' ? -1 : 1);
        zone.dst_offset_min = zone.std_offset_min;
        return zone;
    }
    return std::nullopt;
}

bool ZoneIsDst(const ZoneRules& zone, std::int64_t utc_ms) {
    if (!zone.has_dst) {
        return false;
    }
    // 年份窗口:用标准时偏移近似本地年(±1 扫描盖住任意边界,近似够用)。
    const std::int64_t local_year =
        UtcMsToCivil(utc_ms + static_cast<std::int64_t>(zone.std_offset_min) * kMsPerMinute)
            .year;
    for (std::int64_t y = local_year - 1; y <= local_year + 1; ++y) {
        const std::int64_t spring = TransitionInstantUtc(zone, true, y);
        const std::int64_t fall = TransitionInstantUtc(zone, false, y);
        if (spring <= fall && utc_ms >= spring && utc_ms < fall) {
            return true;
        }
    }
    return false;
}

CivilTime ZoneUtcToLocal(const ZoneRules& zone, std::int64_t utc_ms) {
    const int offset = ZoneIsDst(zone, utc_ms) ? zone.dst_offset_min : zone.std_offset_min;
    return UtcMsToCivil(utc_ms + static_cast<std::int64_t>(offset) * kMsPerMinute);
}

LocalToUtcResult ZoneLocalToUtc(const ZoneRules& zone, const CivilTime& local) {
    LocalToUtcResult result;
    const std::int64_t naive = CivilToUtcMs(local);
    std::vector<std::int64_t> valid;
    const int offsets[2] = {zone.std_offset_min, zone.dst_offset_min};
    for (int i = 0; i < (zone.has_dst ? 2 : 1); ++i) {
        const std::int64_t candidate =
            naive - static_cast<std::int64_t>(offsets[i]) * kMsPerMinute;
        if (SameCivil(ZoneUtcToLocal(zone, candidate), local)) {
            valid.push_back(candidate);
        }
    }
    std::sort(valid.begin(), valid.end());
    if (valid.empty()) {
        // 缺口:该墙钟不存在(春跳)。first = 过渡点(跳变后第一个绝对时刻)。
        result.kind = LocalToUtcResult::Kind::Gap;
        result.first_utc_ms = TransitionInstantUtc(zone, true, local.year);
        return result;
    }
    if (valid.size() == 1) {
        result.kind = LocalToUtcResult::Kind::Unique;
        result.first_utc_ms = valid[0];
        return result;
    }
    result.kind = LocalToUtcResult::Kind::Ambiguous;
    result.first_utc_ms = valid[0];
    result.second_utc_ms = valid[1];
    return result;
}

std::optional<CronExpr> ParseCronExpr(const std::string& text) {
    // 五字段按空白切;不是五字段 = 明拒。
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        if (pos > start) {
            tokens.push_back(text.substr(start, pos - start));
        }
    }
    if (tokens.size() != 5) {
        return std::nullopt;
    }
    CronExpr expr;
    std::vector<bool> minute;
    std::vector<bool> hour;
    std::vector<bool> dom;
    std::vector<bool> month;
    std::vector<bool> dow;
    if (!ParseCronField(tokens[0], {0, 59}, minute, false) ||
        !ParseCronField(tokens[1], {0, 23}, hour, false) ||
        !ParseCronField(tokens[2], {1, 31}, dom, false) ||
        !ParseCronField(tokens[3], {1, 12}, month, false) ||
        !ParseCronField(tokens[4], {0, 7}, dow, true)) {
        return std::nullopt;
    }
    for (int i = 0; i < 60; ++i) {
        expr.minute[static_cast<std::size_t>(i)] = minute[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < 24; ++i) {
        expr.hour[static_cast<std::size_t>(i)] = hour[static_cast<std::size_t>(i)];
    }
    for (int i = 1; i <= 31; ++i) {
        expr.dom[static_cast<std::size_t>(i)] = dom[static_cast<std::size_t>(i)];
    }
    for (int i = 1; i <= 12; ++i) {
        expr.month[static_cast<std::size_t>(i)] = month[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < 7; ++i) {
        expr.dow[static_cast<std::size_t>(i)] = dow[static_cast<std::size_t>(i)];
    }
    expr.dom_star = tokens[2] == "*";
    expr.dow_star = tokens[4] == "*";
    return expr;
}

bool CronMatches(const CronExpr& expr, const CivilTime& local) {
    if (!expr.month[static_cast<std::size_t>(local.month)]) {
        return false;
    }
    bool day_match = true;
    if (expr.dom_star && expr.dow_star) {
        day_match = true;
    } else if (expr.dom_star) {
        day_match = expr.dow[static_cast<std::size_t>(local.weekday)];
    } else if (expr.dow_star) {
        day_match = expr.dom[static_cast<std::size_t>(local.day)];
    } else {
        day_match = expr.dom[static_cast<std::size_t>(local.day)] ||
                    expr.dow[static_cast<std::size_t>(local.weekday)];
    }
    if (!day_match) {
        return false;
    }
    return expr.hour[static_cast<std::size_t>(local.hour)] &&
           expr.minute[static_cast<std::size_t>(local.minute)];
}

NextFire NextCronFireUtc(const CronExpr& expr, const ZoneRules& zone, std::int64_t after_ms) {
    NextFire result;
    // 从 after 的本地墙钟下一分钟起,按 月 → 日 → 时 → 分 走;月不命中
    // 跳下月头,日不命中跳次日头(两年上界,防永不命中死走)。
    CivilTime cursor = ZoneUtcToLocal(zone, after_ms);
    cursor.second = 0;
    cursor.minute += 1;
    if (cursor.minute >= 60) {
        cursor.minute -= 60;
        cursor.hour += 1;
    }
    if (cursor.hour >= 24) {
        cursor.hour -= 24;
        cursor.day += 1;
    }
    const std::int64_t start_year = cursor.year;
    bool first_day = true;
    for (std::int64_t steps = 0; steps <= 2 * 366 + 24; ++steps) {
        // 月/日规范化(溢出进位)。
        while (cursor.month > 12) {
            cursor.month -= 12;
            cursor.year += 1;
        }
        while (cursor.day > DaysInMonth(cursor.year, cursor.month)) {
            cursor.day -= DaysInMonth(cursor.year, cursor.month);
            cursor.month += 1;
        }
        if (cursor.year > start_year + 2) {
            return result;  // 两年无命中(Feb-30 这类)
        }
        const int weekday = WeekdayOfDate(cursor.year, cursor.month, cursor.day);
        if (!expr.month[static_cast<std::size_t>(cursor.month)]) {
            cursor.month += 1;
            cursor.day = 1;
            cursor.hour = 0;
            cursor.minute = 0;
            first_day = false;
            continue;
        }
        const bool day_match = [&] {
            if (expr.dom_star && expr.dow_star) {
                return true;
            }
            if (expr.dom_star) {
                return expr.dow[static_cast<std::size_t>(weekday)];
            }
            if (expr.dow_star) {
                return expr.dom[static_cast<std::size_t>(cursor.day)];
            }
            return expr.dom[static_cast<std::size_t>(cursor.day)] ||
                   expr.dow[static_cast<std::size_t>(weekday)];
        }();
        if (!day_match) {
            cursor.day += 1;
            cursor.hour = 0;
            cursor.minute = 0;
            first_day = false;
            continue;
        }
        // 这一天内按时分走(首日从 cursor 的时分起)。
        for (int h = cursor.hour; h < 24; ++h) {
            if (!expr.hour[static_cast<std::size_t>(h)]) {
                continue;
            }
            const int minute_begin = (first_day && h == cursor.hour) ? cursor.minute : 0;
            for (int m = minute_begin; m < 60; ++m) {
                if (!expr.minute[static_cast<std::size_t>(m)]) {
                    continue;
                }
                CivilTime cand = cursor;
                cand.hour = h;
                cand.minute = m;
                cand.second = 0;
                cand.weekday = weekday;
                const LocalToUtcResult resolved = ZoneLocalToUtc(zone, cand);
                if (resolved.kind == LocalToUtcResult::Kind::Gap) {
                    continue;  // 缺口墙钟:不补不挪,跳过(头注裁决)
                }
                // 一个墙钟拍只发一次:重复段取第一次出现(较早绝对时刻);
                // 第一次已在过去(启动落在两次之间)就整拍跳过,不补发
                // 第二次——cron 惯例,occurrence slot 也因此唯一。
                if (resolved.first_utc_ms > after_ms) {
                    result.found = true;
                    result.utc_ms = resolved.first_utc_ms;
                    return result;
                }
            }
        }
        cursor.day += 1;
        cursor.hour = 0;
        cursor.minute = 0;
        first_day = false;
    }
    return result;
}

std::string ToString(ScheduleKind kind) {
    switch (kind) {
        case ScheduleKind::Once:
            return "once";
        case ScheduleKind::Interval:
            return "interval";
        case ScheduleKind::Cron:
            return "cron";
    }
    return "once";
}

bool ParseScheduleKind(const std::string& text, ScheduleKind& out) {
    if (text == "once") {
        out = ScheduleKind::Once;
        return true;
    }
    if (text == "interval") {
        out = ScheduleKind::Interval;
        return true;
    }
    if (text == "cron") {
        out = ScheduleKind::Cron;
        return true;
    }
    return false;
}

std::string ToString(MisfirePolicy policy) {
    return policy == MisfirePolicy::Coalesce ? "coalesce" : "skip";
}

bool ParseMisfirePolicy(const std::string& text, MisfirePolicy& out) {
    if (text == "coalesce") {
        out = MisfirePolicy::Coalesce;
        return true;
    }
    if (text == "skip") {
        out = MisfirePolicy::Skip;
        return true;
    }
    return false;
}

std::string ValidateScheduleSpec(const ScheduleSpec& spec) {
    if (!FindZone(spec.timezone).has_value()) {
        return "automation.timezone_invalid: 认不得的时区 \"" + spec.timezone + "\"";
    }
    switch (spec.kind) {
        case ScheduleKind::Once:
            return std::string();
        case ScheduleKind::Interval:
            if (spec.interval_seconds < 1 || spec.interval_seconds > 315360000) {
                return "automation.schedule_invalid: interval 须在 1 秒到 10 年之间";
            }
            if (spec.anchor_ms == 0) {
                return "automation.schedule_invalid: interval 须带锚点(anchor)";
            }
            return std::string();
        case ScheduleKind::Cron: {
            const auto expr = ParseCronExpr(spec.cron_expr);
            if (!expr.has_value()) {
                return "automation.schedule_invalid: cron 表达式不认得(五字段受限子集): \"" +
                       spec.cron_expr + "\"";
            }
            const auto zone = FindZone(spec.timezone);
            if (!zone.has_value()) {
                return "automation.timezone_invalid: 认不得的时区 \"" + spec.timezone + "\"";
            }
            const NextFire probe = NextCronFireUtc(*expr, *zone, 0);
            if (!probe.found) {
                return "automation.schedule_invalid: cron 表达式两年内无触发点: \"" +
                       spec.cron_expr + "\"";
            }
            return std::string();
        }
    }
    return std::string();
}

NextFire FirstSlotAfter(const ScheduleSpec& spec, std::int64_t after_ms) {
    NextFire result;
    switch (spec.kind) {
        case ScheduleKind::Once:
            if (spec.due_at_ms > after_ms) {
                result.found = true;
                result.utc_ms = spec.due_at_ms;
            }
            return result;
        case ScheduleKind::Interval: {
            if (spec.interval_seconds < 1 || spec.anchor_ms == 0) {
                return result;
            }
            const std::int64_t interval_ms = spec.interval_seconds * 1000;
            std::int64_t k = 1;
            if (after_ms >= spec.anchor_ms) {
                k = (after_ms - spec.anchor_ms) / interval_ms + 1;
            }
            result.found = true;
            result.utc_ms = spec.anchor_ms + k * interval_ms;
            return result;
        }
        case ScheduleKind::Cron: {
            const auto expr = ParseCronExpr(spec.cron_expr);
            const auto zone = FindZone(spec.timezone);
            if (!expr.has_value() || !zone.has_value()) {
                return result;  // 调用方应先过校验;坏规格不出拍
            }
            return NextCronFireUtc(*expr, *zone, after_ms);
        }
    }
    return result;
}

}  // namespace lubancode::gateway
