// cli/format_utils:FormatTokenCount 的 k/M 化边界、状态行文本拼装、
// /context 分类占用分析(FormatContextBreakdown)。全是纯函数,断言直接
// 钉字符串。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/format_utils.hpp"
#include "cli/context_tracker.hpp"
#include "cli/theme.hpp"

using namespace lubancode;
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

TEST_CASE("StreamHintText: 忙时占位写明排队与 Esc 打断") {
    // 旧状态行尾巴"⎋ 打断"撤了以后,空队列时全屏没有打断提示(可发现性
    // 丢了);如今提示回到占位本身——草稿空才显示,恰好是 Esc 真打断的
    // 那个状态。措辞与队列标题对齐(键名拼 Esc,不用 ⎋ 字形)。
    const std::string rich = StreamHintText(/*plain=*/false);
    CHECK(rich.find("排队") != std::string::npos);
    CHECK(rich.find("Esc") != std::string::npos);
    CHECK(rich.find("打断") != std::string::npos);
    CHECK(rich.find("\xe2\x8e\x8b") == std::string::npos);  // 键名拼 Esc,不用 ⎋
    const std::string plain = StreamHintText(/*plain=*/true);
    CHECK(plain.find("排队") != std::string::npos);
    CHECK(plain.find("Esc") != std::string::npos);
    CHECK(plain.find("打断") != std::string::npos);
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

TEST_CASE("StatusPanel: 后台任务段非空恒挂,空账收起(background 管理面单)") {
    // 空 = 没后台任务,整段不挂,状态行零变化。
    {
        StatusPanelData data;
        data.model = "gpt-test";
        const auto segments = BuildStatusPanelSegments({"model"}, ConfirmMode::Confirm, data);
        bool seen = false;
        for (const auto& segment : segments) {
            if (segment.key == "background") seen = true;
        }
        CHECK_FALSE(seen);
    }
    // 非空 = 恒挂,items 没配也挂(与 REC/WT/tools/plan/goal_loop 同待遇:
    // 后台有没有东西在跑,用户没配也得看得见)。
    {
        StatusPanelData data;
        data.model = "gpt-test";
        data.background = "后台 2 运行 / 1 完成";
        const auto segments = BuildStatusPanelSegments({"model"}, ConfirmMode::Confirm, data);
        bool seen = false;
        for (const auto& segment : segments) {
            if (segment.key == "background") {
                seen = true;
                CHECK(segment.text == "后台 2 运行 / 1 完成");
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
    // d1a148c 起 /context 改分组卡片式,表头从"上下文占用分析(窗口 N)"换成
    // "── 占用 ──(窗口 N)"(cmd.context.group.usage);此处按新表头认。
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

// ---- WrapStatusRows(P3-3 括号断行) --------------------------------------------------

TEST_CASE("WrapStatusRows: node(v24.0.0) 80/100 列整行一排,不折") {
    const std::string line = "解释器可用: node(v24.0.0)";
    CHECK(lubancode::cli::WrapStatusRows(line, 80) == std::vector<std::string>{line});
    CHECK(lubancode::cli::WrapStatusRows(line, 100) == std::vector<std::string>{line});
}

TEST_CASE("WrapStatusRows: 窄终端快照——右括号跟着词走,不独自起行") {
    const std::string line = "解释器可用: node(v24.0.0)";
    // 14 列:"解释器可用:"(12 列)在首行,"node(v24.0.0)"(12 列)整段挪次行。
    const auto narrow = lubancode::cli::WrapStatusRows(line, 14);
    REQUIRE(narrow.size() == 2);
    CHECK(narrow[0] == "解释器可用:");
    CHECK(narrow[1] == "node(v24.0.0)");
    // 再窄(10 列):前缀 12 列也装不下,无断点,原样占一行不切字。
    const auto tiny = lubancode::cli::WrapStatusRows(line, 10);
    REQUIRE(tiny.size() == 2);
    CHECK(tiny[0] == "解释器可用:");
    CHECK(tiny[1] == "node(v24.0.0)");
    // 任意宽度下,没有一行以右括号开头。
    for (const int width : {8, 12, 14, 16, 20, 24, 30, 40, 60}) {
        for (const std::string& row : lubancode::cli::WrapStatusRows(line, width)) {
            if (!row.empty()) {
                CHECK(row.front() != ')');
                CHECK(row.find('\n') == std::string::npos);
            }
        }
    }
}

TEST_CASE("WrapStatusRows: 中文宽度两列记账,折点只认空格,不切半个宽字") {
    // 前缀(4 列) + 半角冒号(1) + 空格(1) + 八个汉字(16) = 22 列。
    const std::string line = "前缀: 一二三四五六七八";
    CHECK(lubancode::cli::WrapStatusRows(line, 22).size() == 1);
    CHECK(lubancode::cli::WrapStatusRows(line, 21).size() == 2);
    const auto rows = lubancode::cli::WrapStatusRows(line, 10);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "前缀:");
    CHECK(rows[1] == "一二三四五六七八");  // 16 列超宽也整词占行,不切
}

TEST_CASE("WrapStatusRows: ANSI 转义零宽,不占折行宽度也不被切断") {
    const std::string line = "[32m绿色[0m tail-of-the-line";
    const auto rows = lubancode::cli::WrapStatusRows(line, 12);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "[32m绿色[0m");
    CHECK(rows[1] == "tail-of-the-line");
}

TEST_CASE("WrapStatusRows: 中文收口符同样不许起行") {
    const std::string line = "abc 【标签】";
    const auto rows = lubancode::cli::WrapStatusRows(line, 6);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "abc");
    CHECK(rows[1] == "【标签】");
    // 任意窄度折出的行,首字符都不是收口符"】"(U+3011 的 UTF-8 首字节 0xE3)。
    // 任意窄度折出的行都不以收口符“】”起头(开括号“【”起头是正当的)。
    const std::string closer = "】";
    for (const int width : {4, 5, 6, 7}) {
        for (const std::string& row : lubancode::cli::WrapStatusRows(line, width)) {
            CHECK(row.compare(0, closer.size(), closer) != 0);
        }
    }
}

// ---- 逐请求缓存命中块(真实实测问题单问题 5) -------------------------------
// 一行 = 一次带回 usage 的模型请求(不是用户轮);按外层用户轮次分组,
// 上限/总数/未回报都说破。拼装全在 BuildCacheRequestHistoryLines,这里钉
// 字符串。

TEST_CASE("BuildCacheRequestHistoryLines: 一请求五工具 = 1 个用户轮次 / 6 次模型请求") {
    lubancode::cli::ContextTracker tracker(100000);
    tracker.BeginUserTurn("turn-3", "那你挂后台启动吧");
    for (int step = 0; step <= 5; ++step) {
        tracker.ApplyUsage(api::Usage{27000 + step * 100, 400, 25000 + step * 100, 0}, "turn-3", step);
    }
    const auto lines = lubancode::cli::BuildCacheRequestHistoryLines(tracker);
    REQUIRE(lines.size() == 2 + 1 + 6);  // 头两行 + 组头 + 6 次请求
    // 表头:口径说死——逐请求、仅主会话、会话内、不是用户轮。
    CHECK(lines[0].find("逐请求缓存命中") != std::string::npos);
    CHECK(lines[0].find("仅主会话") != std::string::npos);
    CHECK(lines[0].find("6 次模型请求") != std::string::npos);
    // 计数行:1 个用户轮次 / 6 次模型请求,两种计数各叫各名。
    CHECK(lines[1].find("1 个用户轮次 / 6 次模型请求") != std::string::npos);
    // 组头:轮次序号 + turn_id 可追 + 用户输入标签。
    CHECK(lines[2].find("用户轮次 #1") != std::string::npos);
    CHECK(lines[2].find("turn-3") != std::string::npos);
    CHECK(lines[2].find("那你挂后台启动吧") != std::string::npos);
    // 请求行:请求 1..6,输入/命中/百分比。
    CHECK(lines[3].find("请求 1") != std::string::npos);
    // 完整输入 = input + cache_read(Anthropic 语义,TotalInputTokens):
    // 请求 1 = 27000 + 25000 = 52000 -> "52k"。
    CHECK(lines[3].find("52k") != std::string::npos);
    CHECK(lines[7].find("请求 5") != std::string::npos);
    CHECK(lines[8].find("请求 6") != std::string::npos);
}

TEST_CASE("BuildCacheRequestHistoryLines: 跨用户轮次分组,轮次不明与未登记各有措辞") {
    lubancode::cli::ContextTracker tracker(100000);
    tracker.BeginUserTurn("turn-1", "第一问");
    tracker.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-1", 0);
    tracker.ApplyUsage(api::Usage{2000, 10, 1000, 0}, "turn-1", 1);
    tracker.BeginUserTurn("turn-2", "第二问");
    tracker.ApplyUsage(api::Usage{3000, 10, 2800, 0}, "turn-2", 0);
    const auto lines = lubancode::cli::BuildCacheRequestHistoryLines(tracker);
    CHECK(lines[1].find("2 个用户轮次 / 3 次模型请求") != std::string::npos);
    CHECK(lines[2].find("用户轮次 #1") != std::string::npos);
    CHECK(lines[5].find("用户轮次 #2") != std::string::npos);

    // 事件没带 turn_id:按"轮次不明"分组,不猜。
    lubancode::cli::ContextTracker orphan(100000);
    orphan.ApplyUsage(api::Usage{1000, 10, 0, 0}, "", 0);
    const auto orphan_lines = lubancode::cli::BuildCacheRequestHistoryLines(orphan);
    REQUIRE(orphan_lines.size() >= 4);
    CHECK(orphan_lines[2].find("轮次不明") != std::string::npos);

    // 陌生 turn_id(没走 BeginUserTurn 的路径):自动补号,标签写"未登记用户输入"。
    lubancode::cli::ContextTracker plain(100000);
    plain.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-7", 0);
    const auto plain_lines = lubancode::cli::BuildCacheRequestHistoryLines(plain);
    CHECK(plain_lines[2].find("turn-7") != std::string::npos);
    CHECK(plain_lines[2].find("未登记用户输入") != std::string::npos);
}

TEST_CASE("BuildCacheRequestHistoryLines: 达上限明写仅保留最近 12 次,不冒充总数") {
    lubancode::cli::ContextTracker tracker(100000);
    for (int turn = 1; turn <= 15; ++turn) {
        const std::string turn_id = "turn-" + std::to_string(turn);
        tracker.BeginUserTurn(turn_id, "第 " + std::to_string(turn) + " 问");
        tracker.ApplyUsage(api::Usage{1000, 10, 900, 0}, turn_id, 0);
    }
    REQUIRE(tracker.cache_request_history().size() == lubancode::cli::ContextTracker::kCacheHistorySize);
    const auto lines = lubancode::cli::BuildCacheRequestHistoryLines(tracker);
    // 头三行:表头、计数、上限说明(全会话共 15 次,12 只是窗口)。
    CHECK(lines[0].find("12 次模型请求") != std::string::npos);
    CHECK(lines[2].find("仅保留最近 12 次") != std::string::npos);
    CHECK(lines[2].find("全会话共 15 次模型请求") != std::string::npos);
    // 窗口里只剩 turn-4..turn-15,最旧的 turn-1..3 被挤掉。
    CHECK(lines[3].find("turn-4") != std::string::npos);

    // 未到上限:不说"仅保留",总数如实。
    lubancode::cli::ContextTracker small(100000);
    small.BeginUserTurn("turn-1", "只有一问");
    small.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-1", 0);
    const auto small_lines = lubancode::cli::BuildCacheRequestHistoryLines(small);
    for (const auto& line : small_lines) {
        CHECK(line.find("仅保留") == std::string::npos);
    }
}

TEST_CASE("BuildCacheRequestHistoryLines: 未回报请求标缺测,不冒充 0%") {
    lubancode::cli::ContextTracker tracker(100000);
    tracker.BeginUserTurn("turn-1", "这一问");
    tracker.ApplyUsage(api::Usage{1000, 10, 0, 0}, "turn-1", 0);
    tracker.ApplyUsage(api::Usage{}, "turn-1", 1);  // 第二次请求 provider 没回 usage
    const auto lines = lubancode::cli::BuildCacheRequestHistoryLines(tracker);
    REQUIRE(lines.size() == 2 + 1 + 2);
    // 时间序:请求 1 是实测(命中 0 如实报 0%),请求 2 是缺测(未回报)。
    CHECK(lines[3].find("请求 1") != std::string::npos);
    CHECK(lines[3].find("0%") != std::string::npos);       // 实测过 0 命中,如实 0%
    CHECK(lines[4].find("请求 2") != std::string::npos);
    CHECK(lines[4].find("未回报") != std::string::npos);
    CHECK(lines[4].find("%") == std::string::npos);  // 缺测行没有百分比
}

TEST_CASE("BuildCacheRequestHistoryLines: 一次请求都没有返回空表(本地 slash 不进账)") {
    lubancode::cli::ContextTracker tracker(100000);
    tracker.BeginUserTurn("turn-1", "还没发请求");
    CHECK(lubancode::cli::BuildCacheRequestHistoryLines(tracker).empty());
}
