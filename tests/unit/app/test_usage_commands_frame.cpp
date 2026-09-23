// TUI 排版批 5c(/usage 报告)的输出形状册。
//   - 主报告:标题句进 frame 标题(嵌上边框),八节按节名拆键值对、key 列
//     全表对齐;缺口点名是独立键值对框(空 key 续行,值列同栏);
//   - --by 分账表:schema 列头(label/share/req/tokens/cost),share/req/cost
//     右对齐,表头走 table_header 语义色;
//   - 80 列预算:width=80 时整行显示宽不超 80(值列截尾保字头);
//   - plain 主题:零转义、无框,标题降为独立内容行。
// 句子级内容(coverage/cache/成色的话)由 test_usage_command.cpp 钉,本册
// 只钉形状。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "accounting/purpose.hpp"
#include "accounting/usage_aggregate.hpp"
#include "accounting/usage_sample.hpp"
#include "app/commands/usage_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/theme.hpp"

using namespace lubancode;
using namespace lubancode::app;
using lubancode::accounting::RequestPurpose;
using lubancode::accounting::UsageSample;

namespace {

bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ContainsOne(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 剥掉 CSI 序列(与 test_insights_commands_frame.cpp 同一把手写的尺)。
std::string StripAnsiLight(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

// key 段之后 value 的起始显示列(key 补齐 + 两格列距后应处处同列)。
int ValueStartCol(const std::string& row, const std::string& key) {
    const std::size_t at = row.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + key.size();
    while (i < row.size() && row[i] == ' ') {
        ++i;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(row.substr(0, i)));
}

// 某个字符在此行(剥转义后)的显示列;找不到给 -1。
int CharCol(const std::string& plain, char needle) {
    const std::size_t at = plain.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(plain.substr(0, at)));
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

UsageSample Sample(const char* request_id, RequestPurpose purpose, std::int64_t input,
                   std::int64_t cache_read, std::int64_t output) {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260831-000001-UC0001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.request_id = request_id;
    sample.purpose = purpose;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = lubancode::accounting::UsageSource::ProviderReported;
    api::Usage usage;
    usage.input_tokens = input;
    usage.cache_read_tokens = cache_read;
    usage.output_tokens = output;
    sample.usage = usage;
    sample.total_input_tokens = api::TotalInputTokens(usage);
    sample.total_billed_shape_tokens = sample.total_input_tokens + output;
    sample.request_outcome = "completed";
    return sample;
}

UsageSample UnknownSample(const char* request_id) {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260831-000001-UC0001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.request_id = request_id;
    sample.purpose = RequestPurpose::TitleRefine;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = lubancode::accounting::UsageSource::Unknown;
    sample.request_outcome = "completed";
    return sample;
}

UsageReportModel MakeModel(ParsedUsageCommand::By by) {
    std::vector<UsageSample> samples;
    samples.push_back(Sample("req-1", RequestPurpose::MainTurn, 9000, 0, 500));
    samples.push_back(Sample("req-2", RequestPurpose::SubagentTurn, 50, 0, 0));
    samples.push_back(UnknownSample("req-3"));
    UsageReportModel model;
    model.session_id = "20260831-000001-UC0001";
    model.workspace_key = "ws-000000000000";
    model.status = "running";
    model.provisional = true;
    model.pricing_note = "未配价格表";
    model.aggregate = lubancode::accounting::AggregateUsage(samples);
    model.by = by;
    return model;
}

}  // namespace

TEST_CASE("主报告:标题进上边框,八节键值列全表对齐(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    const std::vector<std::string> lines =
        FormatUsageReport(MakeModel(ParsedUsageCommand::By::None), dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    // 标题嵌上边框(dark);key 列走 row_label 语义色。
    CHECK(ContainsOne(joined, "Usage · 20260831-000001-UC0001(未封口 provisional)"));
    CHECK(ContainsOne(joined, dark.row_label));
    // key 列对齐:覆盖/输入/估算费用/成色的 value 起始显示列一致(节名长短
    // 不齐,由助手补齐到同一列)。
    int coverage_col = -1;
    int input_col = -1;
    int cost_col = -1;
    int quality_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (coverage_col < 0) coverage_col = ValueStartCol(plain, "覆盖");
        if (input_col < 0) input_col = ValueStartCol(plain, "输入");
        if (cost_col < 0) cost_col = ValueStartCol(plain, "估算费用");
        if (quality_col < 0) quality_col = ValueStartCol(plain, "成色");
    }
    CHECK(coverage_col > 0);
    CHECK(input_col > 0);
    CHECK(cost_col > 0);
    CHECK(quality_col > 0);
    CHECK(coverage_col == input_col);
    CHECK(coverage_col == cost_col);
    CHECK(coverage_col == quality_col);
}

TEST_CASE("--by 分账表:schema 列头 + share 右对齐 + 表头语义色(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    const std::vector<std::string> lines =
        FormatUsageReport(MakeModel(ParsedUsageCommand::By::Purpose), dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    // 表标题嵌上边框,尾冒号剥掉(批 2 裁量 3):"):" 不连排。
    CHECK(ContainsOne(joined, "按 purpose 分账(占比分母:input+output token)"));
    CHECK(joined.find("token):") == std::string::npos);
    // schema 列头(批 1 裁量 1)与表头语义色。
    CHECK(ContainsOne(joined, "share"));
    CHECK(ContainsOne(joined, "req"));
    CHECK(ContainsOne(joined, "tokens"));
    CHECK(ContainsOne(joined, dark.table_header));
    // share 右对齐:"99%" 与 "1%" 宽窄不一,'%' 落在同一显示列。
    int pct_col_wide = -1;
    int pct_col_narrow = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (plain.find("main_turn") != std::string::npos) {
            pct_col_wide = CharCol(plain, '%');
        }
        if (plain.find("subagent_turn") != std::string::npos) {
            pct_col_narrow = CharCol(plain, '%');
        }
    }
    CHECK(pct_col_wide > 0);
    CHECK(pct_col_narrow > 0);
    CHECK(pct_col_wide == pct_col_narrow);
}

TEST_CASE("缺口点名:独立键值对框,空 key 续行值列同栏(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    UsageReportModel model = MakeModel(ParsedUsageCommand::By::None);
    for (int i = 0; i < 7; ++i) {
        model.aggregate.warnings.push_back(std::string("usage.purpose_missing: req-") +
                                           std::to_string(i));
    }
    const std::vector<std::string> lines = FormatUsageReport(model, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    CHECK(ContainsOne(joined, "缺口点名"));
    CHECK(Contains(lines, "usage.purpose_missing: req-0"));
    CHECK(Contains(lines, "…另有 2 条"));
    // 续行(key 空)与"缺口点名"标题行不在同一框值列断言里较劲——只钉
    // 续行之间同栏:两条 warning 的起始显示列一致。
    int first_col = -1;
    int second_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (plain.find("req-0") != std::string::npos) {
            first_col = static_cast<int>(lubancode::cli::DisplayWidthUtf8(
                plain.substr(0, plain.find("usage.purpose_missing"))));
        }
        if (plain.find("req-1") != std::string::npos &&
            plain.find("usage.purpose_missing") != std::string::npos) {
            second_col = static_cast<int>(lubancode::cli::DisplayWidthUtf8(
                plain.substr(0, plain.find("usage.purpose_missing"))));
        }
    }
    CHECK(first_col > 0);
    CHECK(first_col == second_col);
}

TEST_CASE("80 列预算:整行显示宽不超 80,截断保字头(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    UsageReportModel model = MakeModel(ParsedUsageCommand::By::Purpose);
    // 长警告逼出值列帽。
    model.aggregate.warnings.push_back(
        "usage.purpose_missing: 0123456789012345678901234567890123456789012345678901234567890123"
        "45678901234567890123456789");
    const std::vector<std::string> lines = FormatUsageReport(model, dark, /*width=*/80);
    REQUIRE(!lines.empty());
    for (const std::string& line : lines) {
        CHECK(lubancode::cli::DisplayWidthUtf8(StripAnsiLight(line)) <= 80);
    }
    // 截断保字头:warning 头一段仍在。
    CHECK(Contains(lines, "usage.purpose_missing: 0123456789"));
}

TEST_CASE("plain 主题:零转义、无框,标题降为独立行") {
    const cli::Theme plain = cli::BuiltinTheme("plain");
    const std::vector<std::string> lines =
        FormatUsageReport(MakeModel(ParsedUsageCommand::By::Purpose), plain, /*width=*/0);
    const std::string joined = JoinLines(lines);
    CHECK(joined.find("\x1b") == std::string::npos);
    CHECK(!ContainsOne(joined, kBoxLightTopLeft));
    CHECK(!ContainsOne(joined, kBoxLightVert));
    // 标题独立成行,内容一字不少;分账表表头下垫 "-" 横线(降级合同)。
    CHECK(Contains(lines, "Usage · 20260831-000001-UC0001(未封口 provisional)"));
    CHECK(Contains(lines, "main_turn"));
    CHECK(Contains(lines, "按 purpose 分账(占比分母:input+output token)"));
    bool has_header_rule = false;
    bool saw_header = false;
    for (const std::string& line : lines) {
        if (line.find("share") != std::string::npos) {
            saw_header = true;
            continue;
        }
        if (saw_header && !line.empty() && line.find_first_not_of('-') == std::string::npos) {
            has_header_rule = true;
        }
    }
    CHECK(saw_header);
    CHECK(has_header_rule);
}
