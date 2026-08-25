// cli/format_utils:FormatTokenCount 的 k/M 化边界、状态行文本拼装、
// /context 分类占用分析(FormatContextBreakdown)。全是纯函数,断言直接
// 钉字符串。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/format_utils.hpp"
#include "cli/theme.hpp"

using lubancode::cli::BuildStatusLineText;
using lubancode::cli::BuildStatusPanelSegments;
using lubancode::cli::BuildStatusPanelText;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::ConfirmMode;
using lubancode::cli::CompactStatusPath;
using lubancode::cli::FormatContextBreakdown;
using lubancode::cli::FormatTokenCount;
using lubancode::cli::StatusLineInfoSegment;
using lubancode::cli::StatusLineModeSegment;
using lubancode::cli::StatusPanelData;
using lubancode::cli::StreamFooterInterruptText;
using lubancode::cli::StreamHintText;

namespace {
// needle 在 s 里出现几次(█/░ 是多字节 UTF-8,按子串数)。
std::size_t CountOccurrences(const std::string& s, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}
}  // namespace

TEST_CASE("FormatTokenCount: 10000 以下原样十进制") {
    CHECK(FormatTokenCount(0) == "0");
    CHECK(FormatTokenCount(1) == "1");
    CHECK(FormatTokenCount(999) == "999");
    CHECK(FormatTokenCount(9999) == "9999");
}

TEST_CASE("FormatTokenCount: 10000 起一位小数 k,尾随 .0 省略") {
    CHECK(FormatTokenCount(10000) == "10k");    // 10.0k -> 10k
    CHECK(FormatTokenCount(10500) == "10.5k");
    CHECK(FormatTokenCount(12300) == "12.3k");
    CHECK(FormatTokenCount(99950) == "100k");   // 四舍五入后尾随 .0 照样省
    CHECK(FormatTokenCount(200000) == "200k");
}

TEST_CASE("FormatTokenCount: 一位小数按四舍五入取") {
    CHECK(FormatTokenCount(10449) == "10.4k");
    CHECK(FormatTokenCount(10450) == "10.5k");
}

TEST_CASE("FormatTokenCount: 1000000 起两位小数 M,尾随 0 逐位省略") {
    CHECK(FormatTokenCount(1000000) == "1M");     // 1.00M -> 1M
    CHECK(FormatTokenCount(1049999) == "1.05M");  // 四舍五入到两位小数
    CHECK(FormatTokenCount(1050000) == "1.05M");
    CHECK(FormatTokenCount(1500000) == "1.5M");   // 1.50M -> 1.5M
    CHECK(FormatTokenCount(2340000) == "2.34M");
}

TEST_CASE("FormatTokenCount: 负数不猜,原样十进制") {
    CHECK(FormatTokenCount(-5) == "-5");
}

TEST_CASE("StatusLineModeSegment: 三档各有一个展示词,提示 shift+tab 切换") {
    const std::string confirm = StatusLineModeSegment(ConfirmMode::Confirm);
    CHECK(confirm.find("确认模式") != std::string::npos);
    CHECK(confirm.find("shift+tab") != std::string::npos);
    CHECK(StatusLineModeSegment(ConfirmMode::Auto).find("auto") != std::string::npos);
    CHECK(StatusLineModeSegment(ConfirmMode::Yolo).find("yolo") != std::string::npos);
}

TEST_CASE("StatusLineInfoSegment: 模型名 + context 百分比,token 数 k 化") {
    const std::string seg = StatusLineInfoSegment("MiniMax-M3", 8, 16400, 200000);
    CHECK(seg.find("MiniMax-M3") != std::string::npos);
    CHECK(seg.find("context 8%") != std::string::npos);
    CHECK(seg.find("16.4k/200k") != std::string::npos);
}

TEST_CASE("StatusLineInfoSegment: 还没有用量(0 tokens)时不摆括号数字") {
    const std::string seg = StatusLineInfoSegment("MiniMax-M3", 0, 0, 200000);
    CHECK(seg.find("context 0%") != std::string::npos);
    CHECK(seg.find("(") == std::string::npos);
}

TEST_CASE("StatusLineInfoSegment: 模型名为空就跳过那一节") {
    const std::string seg = StatusLineInfoSegment("", 3, 0, 0);
    CHECK(seg.find(" ·  · ") == std::string::npos);
    CHECK(seg.find("context 3%") != std::string::npos);
}

TEST_CASE("StreamHintText: 输入行占位只提排队,不再捎带打断说明") {
    const std::string rich = StreamHintText(/*plain=*/false);
    CHECK(rich.find("排队") != std::string::npos);
    CHECK(rich.find("\xe2\x8e\x8b") == std::string::npos);  // ⎋ 挪去了状态行
    const std::string plain = StreamHintText(/*plain=*/true);
    CHECK(plain.find("排队") != std::string::npos);
    CHECK(plain.find("\xe2\x8e\x8b") == std::string::npos);
}

TEST_CASE("StreamFooterInterruptText: 打断提示进状态行,plain 回退纯 ESC 文字") {
    const std::string rich = StreamFooterInterruptText(/*plain=*/false);
    // ⎋ = U+238B, UTF-8 = E2 8E 8B。
    CHECK(rich.find("\xe2\x8e\x8b") != std::string::npos);
    const std::string plain = StreamFooterInterruptText(/*plain=*/true);
    CHECK(plain.find("\xe2\x8e\x8b") == std::string::npos);  // plain 不夹 ⎋
    CHECK(plain.find("ESC") != std::string::npos);
}

TEST_CASE("BuildStatusLineText: 整行 = 模式段 + 信息段") {
    const std::string line = BuildStatusLineText(ConfirmMode::Confirm, "MiniMax-M3", 8, 16400, 200000);
    CHECK(line ==
          StatusLineModeSegment(ConfirmMode::Confirm) + StatusLineInfoSegment("MiniMax-M3", 8, 16400, 200000));
    CHECK(line.find("⏵⏵") != std::string::npos);
}

TEST_CASE("StatusPanel: items 控制字段顺序，空值字段自动跳过") {
    StatusPanelData data;
    data.model = "gpt-test";
    data.cwd = "D:\\work\\demo";
    data.git_branch = "feature/ui";
    data.context_percent = 12;
    data.used_tokens = 24000;
    data.window_tokens = 256000;

    const std::vector<std::string> items{"cwd", "git_branch", "model", "provider", "context", "tokens"};
    CHECK(BuildStatusPanelText(items, " | ", ConfirmMode::Confirm, data) ==
          "D:\\work\\demo | feature/ui | gpt-test | context 12% | 24k/256k");

    const auto segments = BuildStatusPanelSegments(items, ConfirmMode::Confirm, data);
    REQUIRE(segments.size() == 5);
    CHECK(segments[0].key == "cwd");
    CHECK(segments[1].key == "git_branch");
}

TEST_CASE("StatusPanel: provider、effort 与 permission mode 可按需启用") {
    StatusPanelData data;
    data.provider = "sub-openai";
    data.effort = "xhigh";
    const std::string text = BuildStatusPanelText(
        {"provider", "effort", "permission_mode"}, " · ", ConfirmMode::Auto, data);
    CHECK(text.find("provider sub-openai") != std::string::npos);
    CHECK(text.find("effort xhigh") != std::string::npos);
    CHECK(text.find("auto") != std::string::npos);
}

TEST_CASE("StatusPanel: goal/loop 段非空恒挂,不进 items 配置") {
    // 空 = 整段不挂(没立 goal 也没 loop 的会话零影响)。
    {
        StatusPanelData data;
        data.model = "gpt-test";
        const auto segments = BuildStatusPanelSegments({"model"}, ConfirmMode::Confirm, data);
        bool seen = false;
        for (const auto& segment : segments) {
            if (segment.key == "goal_loop") seen = true;
        }
        CHECK_FALSE(seen);
    }
    // 非空 = 恒挂,items 没配也挂(与 REC/WT/tools/plan 同待遇)。
    {
        StatusPanelData data;
        data.model = "gpt-test";
        data.goal_loop = "goal run·iter3·r2 · loop×2 next 4m";
        const auto segments = BuildStatusPanelSegments({"model"}, ConfirmMode::Confirm, data);
        bool seen = false;
        for (const auto& segment : segments) {
            if (segment.key == "goal_loop") {
                seen = true;
                CHECK(segment.text == "goal run·iter3·r2 · loop×2 next 4m");
            }
        }
        CHECK(seen);
    }
}

TEST_CASE("CompactStatusPath: 长路径从左收起，保住盘符和末级目录") {
    CHECK(CompactStatusPath("D:\\very\\long\\folder\\project", 15) == "D:\\…\\project");
    CHECK(CompactStatusPath("/home/user/very/long/project", 12) == "/…/project");
    CHECK(CompactStatusPath("D:\\short", 40) == "D:\\short");
}

// ---------------------------------------------------------------------------
// FormatContextBreakdown:/context 裸敲的分类占用分析。
// 行序固定:header / 系统提示 / 工具定义 / 对话历史 / 分隔线 / 已用 /
// 自动压缩线 / 剩余 / 估算口径说明,共 9 行。
// ---------------------------------------------------------------------------

TEST_CASE("FormatContextBreakdown: 三类正常,token 直传(统一口径),占比与条形按窗口取") {
    // token 由调用方按统一口径算好直传:10000/5000/20000;窗口 256000。
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, /*cache_read=*/0,
                                               /*window=*/256000, /*measured=*/0, BuiltinTheme("dark"), 16);
    REQUIRE(lines.size() == 9);
    // 表头随 0.26.x 分组卡片化改成 "── 占用 ──(窗口 256k)" 风格。
    CHECK(lines[0].find("占用") != std::string::npos);
    CHECK(lines[0].find("256k") != std::string::npos);

    // 系统提示:10000/256000 ≈ 3.9% → 4%;条形 0.625 格 → 1 实心 15 空。
    CHECK(lines[1].find("系统提示") != std::string::npos);
    CHECK(lines[1].find("~10k") != std::string::npos);
    CHECK(lines[1].find(" 4%") != std::string::npos);
    CHECK(CountOccurrences(lines[1], "█") == 1);
    CHECK(CountOccurrences(lines[1], "░") == 15);

    // 工具定义:5000/256000 ≈ 2%;0.3125 格 → 0 实心。
    CHECK(lines[2].find("工具定义") != std::string::npos);
    CHECK(lines[2].find("~5000") != std::string::npos);
    CHECK(lines[2].find(" 2%") != std::string::npos);
    CHECK(CountOccurrences(lines[2], "█") == 0);

    // 对话历史:20000/256000 ≈ 7.8% → 8%;缓存 0 就不摆括号。
    CHECK(lines[3].find("对话历史") != std::string::npos);
    CHECK(lines[3].find("~20k") != std::string::npos);
    CHECK(lines[3].find(" 8%") != std::string::npos);
    CHECK(lines[3].find("缓存命中") == std::string::npos);

    CHECK(lines[4].find("─") != std::string::npos);

    // 已用 = 三类之和 35000(实测 0 更小,用估算带 ~):35000/256000 ≈ 14%。
    CHECK(lines[5].find("已用") != std::string::npos);
    CHECK(lines[5].find("~35k") != std::string::npos);
    CHECK(lines[5].find(" 14%") != std::string::npos);
    CHECK(lines[5].find("实测") == std::string::npos);

    // 自动压缩线 = 256000 * 80% = 204800。
    CHECK(lines[6].find("自动压缩线") != std::string::npos);
    CHECK(lines[6].find("204.8k(80%)") != std::string::npos);

    // 剩余 = 256000 - 35000 = 221000。
    CHECK(lines[7].find("剩余") != std::string::npos);
    CHECK(lines[7].find("221k") != std::string::npos);

    CHECK(lines[8].find("统一口径") != std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 某类为 0 打 ~0、0%、全空条") {
    const auto lines = FormatContextBreakdown(10000, 0, 20000, 0, 256000, 0, BuiltinTheme("dark"), 16);
    REQUIRE(lines.size() == 9);
    CHECK(lines[2].find("~0") != std::string::npos);
    CHECK(lines[2].find(" 0%") != std::string::npos);
    CHECK(CountOccurrences(lines[2], "█") == 0);
    CHECK(CountOccurrences(lines[2], "░") == 16);
}

TEST_CASE("FormatContextBreakdown: 历史带缓存命中就在行尾括注") {
    // 缓存命中意味着发过请求,配一份实测 100000。
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, /*cache_read=*/4600, 256000,
                                               /*measured=*/100000, BuiltinTheme("dark"), 16);
    CHECK(lines[3].find("(缓存命中 4600)") != std::string::npos);
    // 反推口径与缓存括注同挂对话历史那一行。
    CHECK(lines[3].find("=实测总量−系统−工具") != std::string::npos);
    // 缓存只挂在对话历史那一行,别的行不沾。
    CHECK(lines[1].find("缓存命中") == std::string::npos);
    CHECK(lines[5].find("缓存命中") == std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 带命中率就在括注里补百分比,没回报只摆命中量") {
    // hit_percent=50:括注带 ",50%";-1(服务端没回报 usage)退成不带百分比。
    const auto with_ratio = FormatContextBreakdown(10000, 5000, 20000, 4600, 256000, 100000,
                                                   BuiltinTheme("dark"), 16, /*hit_percent=*/50);
    CHECK(with_ratio[3].find("(缓存命中 4600,50%)") != std::string::npos);
    const auto without_ratio = FormatContextBreakdown(10000, 5000, 20000, 4600, 256000, 100000,
                                                      BuiltinTheme("dark"), 16, /*hit_percent=*/-1);
    CHECK(without_ratio[3].find("(缓存命中 4600)") != std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 有实测——总量用实测不带~、历史反推、三项和=实测") {
    // sys=10000、tools=5000(统一口径估,带 ~);实测 100000。
    // 历史反推 = 100000-10000-5000 = 85000(不带 ~,行尾注反推口径)。
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, /*cache_read=*/0,
                                               /*window=*/256000, /*measured=*/100000, BuiltinTheme("dark"), 16);
    REQUIRE(lines.size() == 9);
    // 系统/工具照旧字符估带 ~。
    CHECK(lines[1].find("~10k") != std::string::npos);
    CHECK(lines[2].find("~5000") != std::string::npos);
    // 历史 = 实测 - 系统 - 工具 = 85000,不带 ~,行尾注明反推。
    CHECK(lines[3].find("85k") != std::string::npos);
    CHECK(lines[3].find("~85k") == std::string::npos);
    CHECK(lines[3].find("=实测总量−系统−工具") != std::string::npos);
    // 已用 = 实测 100000,不带 ~,标 (实测);100000/256000 ≈ 39%。
    CHECK(lines[5].find("100k") != std::string::npos);
    CHECK(lines[5].find("~100k") == std::string::npos);
    CHECK(lines[5].find("(实测)") != std::string::npos);
    CHECK(lines[5].find(" 39%") != std::string::npos);
    // 剩余按实测扣:256000-100000=156000,不带 ~。
    CHECK(lines[7].find("156k") != std::string::npos);
    CHECK(lines[7].find("~") == std::string::npos);
    // 末行口径说明走"实测"支。
    CHECK(lines[8].find("上一轮实测") != std::string::npos);
    // 三分项之和 == 实测总量:10000+5000+85000 = 100000。
    CHECK(10000 + 5000 + 85000 == 100000);
}

TEST_CASE("FormatContextBreakdown: 无实测——三项纯字符估、整体带~、注明启动估算") {
    // 统一口径 token 直传:10000/5000/20000;实测 0。
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, /*cache_read=*/0,
                                               /*window=*/256000, /*measured=*/0, BuiltinTheme("dark"), 16);
    REQUIRE(lines.size() == 9);
    CHECK(lines[3].find("~20k") != std::string::npos);
    CHECK(lines[3].find("=实测总量") == std::string::npos);  // 无实测不注反推
    // 已用 = 三类之和 35000,带 ~、不标 (实测)。
    CHECK(lines[5].find("~35k") != std::string::npos);
    CHECK(lines[5].find("(实测)") == std::string::npos);
    CHECK(lines[8].find("尚无实测") != std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 实测 < 系统+工具,历史钉 0") {
    // sys=10000、tools=5000,合计 15000;实测 9000 < 15000。
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, /*cache_read=*/0,
                                               /*window=*/256000, /*measured=*/9000, BuiltinTheme("dark"), 16);
    // 历史反推下限钉 0,不带 ~,仍注反推口径。
    CHECK(lines[3].find(" 0") != std::string::npos);
    CHECK(lines[3].find("=实测总量−系统−工具") != std::string::npos);
    CHECK(CountOccurrences(lines[3], "█") == 0);
    // 已用 = 实测 9000,不带 ~。
    CHECK(lines[5].find("9000") != std::string::npos);
    CHECK(lines[5].find("~9000") == std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 超窗口截断——条形打满、百分比钉 100、剩余 0") {
    // 300000 tokens > 窗口 256000。
    const auto lines = FormatContextBreakdown(300000, 0, 0, 0, 256000, 0, BuiltinTheme("dark"), 16);
    CHECK(lines[1].find("100%") != std::string::npos);
    CHECK(CountOccurrences(lines[1], "█") == 16);
    CHECK(CountOccurrences(lines[1], "░") == 0);
    CHECK(lines[5].find("100%") != std::string::npos);
    // 剩余行:窗口已被吃穿,0。
    CHECK(lines[7].find("剩余") != std::string::npos);
    CHECK(lines[7].find("0") != std::string::npos);
    CHECK(lines[7].find("256") == std::string::npos);
}

TEST_CASE("FormatContextBreakdown: plain 主题回退 # 和 -,不掺 Unicode 条形") {
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, 0, 256000, 0, BuiltinTheme("plain"), 16);
    CHECK(CountOccurrences(lines[1], "#") == 1);
    CHECK(CountOccurrences(lines[1], "-") == 15);
    CHECK(lines[1].find("█") == std::string::npos);
    CHECK(lines[1].find("░") == std::string::npos);
    // 分隔线同样回退 ASCII。
    CHECK(lines[4].find("─") == std::string::npos);
    CHECK(lines[4].find("-") != std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 窄宽度条形照比例取整") {
    // 50 tokens,窗口 100 → 50%,宽度 4 → 2 实 2 空。
    const auto lines = FormatContextBreakdown(50, 0, 0, 0, 100, 0, BuiltinTheme("dark"), 4);
    CHECK(CountOccurrences(lines[1], "█") == 2);
    CHECK(CountOccurrences(lines[1], "░") == 2);
    CHECK(lines[1].find(" 50%") != std::string::npos);
}

TEST_CASE("FormatContextBreakdown: 窗口为 0 不除零,百分比一律 0、条形全空") {
    const auto lines = FormatContextBreakdown(10000, 5000, 20000, 0, /*window=*/0, 0,
                                               BuiltinTheme("dark"), 16);
    REQUIRE(lines.size() == 9);
    CHECK(lines[1].find(" 0%") != std::string::npos);
    CHECK(CountOccurrences(lines[1], "█") == 0);
    CHECK(CountOccurrences(lines[1], "░") == 16);
    CHECK(lines[5].find(" 0%") != std::string::npos);
    // 压缩线 0(80%)、剩余 0,不炸就是胜利。
    CHECK(lines[6].find("0(80%)") != std::string::npos);
}
