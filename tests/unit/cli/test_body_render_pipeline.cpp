// 真实实测问题 1(流式粗体不渲染)的回归册:穿过"分块 → 收束/增量重画
// 渲染"的真实决策链(PlanBodyDelta:ScanBodyDelta 切步 + DetectMarkdownStructure
// + RenderMarkdown),不用真终端就能钉死问题 1 验收的渲染半边:
//   1. 单块与跨块两种 **粗体** 都剥星号上 bold;
//   2. 逐字 delta——标记闭合的那笔触发增量重画,不必苦等段尾空行;
//   3. 正文段后直接跟工具边界(无空行)的形状,让路定格(OnBlockBreak 兜底)
//      用的 PrepareBodyRenderPlan 渲染行同样剥星号;
//   4. 中英混排、窄终端 width-1 铁律;
//   5. 代码块内与 \** 转义字面量不误渲染。
// 问题 3(分块渲染吃掉标题前空行)的 H 系:块级前距自带 + 空行连发吸收,
// 断"分块最终画面 == 整篇渲染",三种到达方式(单笔/跨块/逐字)都钉。
// 锁内落笔(擦行/锚点/VT 批)由 body_render_driver 刮屏冒烟盖,这里只钉决策。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/live_transcript.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8
#include "cli/markdown.hpp"
#include "cli/theme.hpp"

using lubancode::cli::BodyRenderStep;
using lubancode::cli::BodyScanState;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::DetectConsoleWidth;
using lubancode::cli::DisplayWidthUtf8;
using lubancode::cli::PlanBodyDelta;
using lubancode::cli::PrepareBodyRenderPlan;

namespace {

std::string StripAnsi(const std::string& line) {
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && !((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z'))) {
                ++i;
            }
            if (i < line.size()) {
                ++i;
            }
            continue;
        }
        out += line[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += StripAnsi(line);
        out += '\n';
    }
    return out;
}

// 把一笔笔 delta 穿过真实决策链,顺手维护"当前未收束块的累计正文"——
// 那正是 StreamBodyTracker::buffer_ 的账(PrintPieceLocked 落一笔补一笔,
// 收束清零)。返回全部步骤,断言挑着看。
struct FeedResult {
    std::vector<BodyRenderStep> steps;  // 全部步骤按序
    std::string block;                  // 喂完后仍未收束的累计正文
};

FeedResult Feed(const std::vector<std::string>& deltas) {
    FeedResult out;
    BodyScanState state;
    std::string buffer;
    for (const std::string& delta : deltas) {
        const std::vector<BodyRenderStep> steps = PlanBodyDelta(state, buffer, delta, BuiltinTheme("dark"), 80);
        for (const BodyRenderStep& step : steps) {
            if (!step.repaint) {
                buffer += step.piece;  // 与 PrintPieceLocked 的 buffer_ += text 对账
            } else if (step.finalize) {
                buffer.clear();        // 与 RepaintBlockLocked 的清账对账
            }
            out.steps.push_back(step);
        }
    }
    out.block = buffer;
    return out;
}

// 最后一个重画步(收束或增量)的渲染行——"这块此刻定格成什么样"。
std::vector<std::string> LastRepaintLines(const FeedResult& fed) {
    for (auto it = fed.steps.rbegin(); it != fed.steps.rend(); ++it) {
        if (it->repaint && it->plan.hit) {
            return it->plan.lines;
        }
    }
    return {};
}

// 屏上不残留裸 ** 的判据:块已定型的渲染行(去 ANSI)里没有星号对。
bool LinesFreeOfBareBold(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (Contains(StripAnsi(line), "**")) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---- 单块与跨块的粗体 -------------------------------------------------------

TEST_CASE("P1 渲染链: 单块粗体——空行收束步剥星号上 bold") {
    const auto fed = Feed({"方案:**React + Vite 前端、Express 后端**，不引数据库。\n\n"});
    const auto lines = LastRepaintLines(fed);
    REQUIRE_FALSE(lines.empty());
    CHECK(LinesFreeOfBareBold(lines));
    bool saw_bold = false;
    for (const std::string& line : lines) {
        if (Contains(line, "\x1b[1m")) {
            saw_bold = true;
        }
    }
    CHECK(saw_bold);
    CHECK(Contains(JoinLines(lines), "React + Vite"));
}

TEST_CASE("P2 渲染链: 跨 delta 粗体——星号与正文分笔到,闭合那笔增量重画") {
    // `**` 开标记一笔、正文一笔、闭标记一笔:闭合之前露星号是原样流式的
    // 本分,闭合那笔(第三笔)就该有增量步把整块定格成渲染版。
    const auto fed = Feed({"我将采用", "**React", " + Vite 前端、Express 后端**", "，可好。"});
    // 前两笔:半开标记,不画也不误吞。
    bool incremental_after_close = false;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.repaint && !step.finalize && step.plan.hit) {
            incremental_after_close = true;
            CHECK(LinesFreeOfBareBold(step.plan.lines));
        }
    }
    CHECK(incremental_after_close);
    CHECK(fed.block == "我将采用**React + Vite 前端、Express 后端**，可好。");
}

TEST_CASE("P3 渲染链: 逐字 delta——闭合那一笔触发恰好一次增量重画") {
    std::vector<std::string> deltas;
    for (const char c : std::string("定案:**粗体**。")) {
        deltas.emplace_back(1, c);
    }
    const auto fed = Feed(deltas);
    std::size_t repaints = 0;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.repaint) {
            ++repaints;
            CHECK(step.plan.hit);
            CHECK(LinesFreeOfBareBold(step.plan.lines));
        }
    }
    CHECK(repaints == 1);  // 星号闭合那笔;其余逐字笔不画
    CHECK(Contains(JoinLines(LastRepaintLines(fed)), "粗体"));
}

TEST_CASE("P4 渲染链: 正文段后直接跟工具边界(无空行)——让路定格预案剥星号") {
    // 实测真机最痛的形状:正文段没有空行结尾,tool_use 直接开画。决策链
    // 里没有收束步(这正是要 OnBlockBreak 兜底的样子),兜底用的预案
    // (PrepareBodyRenderPlan(buffer_))必须把闭合粗体渲染掉。
    const auto fed = Feed({"当前目录为空。我将按常见方案实现：**React + Vite 前端、Express 后端、"
                           "JSON 文件持久化**，支持搜索及完整图书增删改查。"});
    bool saw_finalize = false;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.finalize) {
            saw_finalize = true;
        }
    }
    CHECK_FALSE(saw_finalize);  // 没等到空行:决策链里不该有收束步
    const auto plan = PrepareBodyRenderPlan(fed.block, BuiltinTheme("dark"), 80);
    REQUIRE(plan.hit);
    CHECK(LinesFreeOfBareBold(plan.lines));
    CHECK(Contains(JoinLines(plan.lines), "React + Vite"));
    bool saw_bold = false;
    for (const std::string& line : plan.lines) {
        if (Contains(line, "\x1b[1m")) {
            saw_bold = true;
        }
    }
    CHECK(saw_bold);
}

// ---- 混排 / 窄终端 ----------------------------------------------------------

TEST_CASE("P5 渲染链: 中英标点混排的粗体照样闭合渲染") {
    const std::string text = "选型是**React 前端、Express 后端(JSON 持久化)**的方案。";
    const auto fed = Feed({text, "\n\n"});
    const auto lines = LastRepaintLines(fed);
    REQUIRE_FALSE(lines.empty());
    CHECK(LinesFreeOfBareBold(lines));
    CHECK(Contains(JoinLines(lines), "Express 后端(JSON 持久化)"));
}

TEST_CASE("P6 渲染链: 窄终端下增量/收束渲染行都守 width-1 铁律") {
    const std::vector<std::string> deltas = {
        "很长的中文段落开头，**这段粗体文字特别长，窄终端必须折行而不许物理折行破坏行数账**，"
        "后头再跟一些正文。"};
    BodyScanState state;
    std::string buffer;
    for (const std::string& delta : deltas) {
        for (const BodyRenderStep& step :
             PlanBodyDelta(state, buffer, delta, BuiltinTheme("dark"), 20)) {
            if (!step.repaint) {
                buffer += step.piece;
            } else if (step.finalize) {
                buffer.clear();
            }
            if (step.repaint && step.plan.hit) {
                for (const std::string& line : step.plan.lines) {
                    CHECK(DisplayWidthUtf8(StripAnsi(line)) <= 19);
                }
            }
        }
    }
}

// ---- 代码块与转义字面量 -----------------------------------------------------

TEST_CASE("P7 渲染链: 代码围栏内的 ** 原样保留,围栏外照常渲染") {
    const auto fed = Feed({"代码如下:\n```js\nconst s = `**不是粗体**`;\n```\n\n**外面的才是粗体**\n\n"});
    // 围栏行在第一个收束步,围栏外粗体在第二个——两步的渲染行都要看。
    std::string joined;
    std::vector<std::string> all_lines;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.repaint && step.plan.hit) {
            for (const std::string& line : step.plan.lines) {
                all_lines.push_back(line);
            }
        }
    }
    joined = JoinLines(all_lines);
    CHECK(Contains(joined, "**不是粗体**"));  // 围栏内一字不动
    // 围栏外那对粗体剥掉:全屏只剩围栏内那两枚 ** 标记。
    std::size_t star_pairs = 0;
    for (const std::string& line : all_lines) {
        const std::string visible = StripAnsi(line);
        for (std::size_t at = visible.find("**"); at != std::string::npos;
             at = visible.find("**", at + 2)) {
            ++star_pairs;
        }
    }
    CHECK(star_pairs == 2);
}

TEST_CASE("P8 渲染链: 转义字面量 \\** 不被吞成粗体") {
    // CommonMark 口径:\* 吃反斜杠出字面星,该星不参与配对。\*\* 全字面。
    const auto fed = Feed({"想看星号就写 \\*\\*字面星号\\*\\* 或者 \\**半转义**。\n\n"});
    const auto lines = LastRepaintLines(fed);
    REQUIRE_FALSE(lines.empty());
    const std::string joined = JoinLines(lines);
    CHECK(Contains(joined, "**字面星号**"));  // \*\* 字面:两枚星都在,不上样式
    CHECK(Contains(joined, "*半转义"));       // \** → 字面 * + 斜体(不吞粗体)
}

TEST_CASE("P9 渲染链: 增量步在长块超出预算后退场,空行收束照常兜底") {
    std::vector<std::string> deltas;
    for (int i = 0; i < 60; ++i) {
        deltas.push_back("第" + std::to_string(i) + "行,**粗**。\n");
    }
    deltas.push_back("\n");
    const auto fed = Feed(deltas);
    std::size_t incremental = 0;
    std::size_t finalize = 0;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.repaint && !step.finalize) {
            ++incremental;
            CHECK(LinesFreeOfBareBold(step.plan.lines));
        }
        if (step.repaint && step.finalize) {
            ++finalize;
            CHECK(LinesFreeOfBareBold(step.plan.lines));
        }
    }
    CHECK(incremental == 48);  // 预算内逐行画,超了退场
    REQUIRE(finalize == 1);    // 空行收束兜底,整块最终定格成渲染版
}

// ---- 问题 3:分块渲染吃掉标题前空行 ------------------------------------------
//
// 前一块定格成渲染版时,它的重画把块尾那行分隔空行一并擦掉(渲染版头尾
// 空行都剪),后一块的渲染预案得自带一行前距,否则两块贴死——标题前那
// 行空行就是这么丢的。这节穿过 ScanBodyDelta 的真实空行切块 + PlanBodyDelta
// 的预案拼装断"最终画面",锁内落笔的几何由 body_render_driver 刮屏盖。

namespace {

// 屏面模拟器:按 StreamBodyTracker 的锚点契约回放决策链的步骤——原样段
// 逐字落进光标行(换行才滚新行),命中的收束/增量步把自己那片(块首锚点
// 到光标)整块换成渲染行;渲染版带换行收梢时光标落到新空行。光标行及其
// 以下是"下一块的地界",不算已定格画面。
std::vector<std::string> ScreenLines(const std::vector<std::string>& deltas) {
    std::vector<std::string> screen{std::string()};
    std::size_t block_start = 0;
    bool in_block = false;
    BodyScanState state;
    std::string buffer;
    const auto print_raw = [&screen](const std::string& text) {
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t nl = text.find('\n', at);
            if (nl == std::string::npos) {
                screen.back() += text.substr(at);
                return;
            }
            screen.back() += text.substr(at, nl - at);
            screen.emplace_back();
            at = nl + 1;
        }
    };
    for (const std::string& delta : deltas) {
        for (const BodyRenderStep& step : PlanBodyDelta(state, buffer, delta, BuiltinTheme("dark"), 80)) {
            if (!step.repaint) {
                if (!in_block) {
                    in_block = true;
                    block_start = screen.size() - 1;  // 锚点 = 光标行
                }
                print_raw(step.piece);
                buffer += step.piece;
                continue;
            }
            if (in_block && step.plan.hit) {
                screen.erase(screen.begin() + static_cast<std::ptrdiff_t>(block_start), screen.end());
                screen.insert(screen.begin() + static_cast<std::ptrdiff_t>(block_start), step.plan.lines.begin(),
                              step.plan.lines.end());
                if (step.plan.ended_with_newline) {
                    screen.emplace_back();  // 渲染版换行收梢:光标落到新行
                }
            }
            if (step.finalize) {
                in_block = false;
                buffer.clear();
            }
        }
    }
    while (screen.size() > 1 && screen.back().empty()) {
        screen.pop_back();
    }
    for (std::string& line : screen) {
        line = StripAnsi(line);
    }
    return screen;
}

// 整篇一次渲染的行(去 ANSI)——分块画面的对照基准。
std::vector<std::string> WholeDocLines(const std::string& text) {
    std::vector<std::string> lines = lubancode::cli::RenderMarkdown(text, BuiltinTheme("dark"), 80);
    for (std::string& line : lines) {
        line = StripAnsi(line);
    }
    return lines;
}

std::vector<std::string> CharDeltas(const std::string& text) {
    std::vector<std::string> out;
    for (const char c : text) {
        out.emplace_back(1, c);
    }
    return out;
}

std::vector<std::vector<std::string>> FinalizedPlans(const FeedResult& fed) {
    std::vector<std::vector<std::string>> out;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.finalize && step.plan.hit) {
            out.push_back(step.plan.lines);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("H1 渲染链: 列表接标题——标题块预案自带恰好一行前距(单 delta)") {
    const std::string text = "- 列表末项\n\n### 标题\n\n";
    const auto fed = Feed({text});
    const auto plans = FinalizedPlans(fed);
    REQUIRE(plans.size() == 2);
    CHECK_FALSE(plans[0].empty());
    CHECK_FALSE(plans[0].front().empty());  // 第一块:不凭空多首行空白
    REQUIRE(plans[1].size() == 2);
    CHECK(plans[1].front().empty());  // 标题块:恰好一行前距
    CHECK(Contains(StripAnsi(plans[1][1]), "标题"));
    // 最终画面:列表、空行、标题——与整篇渲染一字不差。
    CHECK(ScreenLines({text}) == WholeDocLines(text));
}

TEST_CASE("H2 渲染链: 跨 delta 到达——两块分笔到,标题块照样带前距") {
    const std::string text = "- 列表末项\n\n### 标题\n\n";
    const auto fed = Feed({"- 列表末项\n\n", "### 标题\n\n"});
    const auto plans = FinalizedPlans(fed);
    REQUIRE(plans.size() == 2);
    CHECK_FALSE(plans[0].front().empty());
    REQUIRE(plans[1].size() == 2);
    CHECK(plans[1].front().empty());
    CHECK(ScreenLines({"- 列表末项\n\n", "### 标题\n\n"}) == WholeDocLines(text));
}

TEST_CASE("H3 渲染链: 逐字 delta——标题块的增量步与收束步都带前距") {
    const std::string text = "- 列表末项\n\n### 标题\n\n";
    const auto fed = Feed(CharDeltas(text));
    std::size_t heading_repaints = 0;
    for (const BodyRenderStep& step : fed.steps) {
        if (step.repaint && step.plan.hit && Contains(JoinLines(step.plan.lines), "标题")) {
            ++heading_repaints;
            REQUIRE(step.plan.lines.size() >= 2);
            CHECK(step.plan.lines.front().empty());  // 中途画面也不贴死
        }
    }
    CHECK(heading_repaints >= 2);  // 至少:行边界增量 + 空行收束
    CHECK(ScreenLines(CharDeltas(text)) == WholeDocLines(text));
}

TEST_CASE("H4 渲染链: 段落/列表/代码块接标题——三种到达的分块画面与整篇渲染逐行一致") {
    const std::string text = "先说结论:**方案已定**。\n\n"
                             "### 运行方式\n\n"
                             "- 先 npm install\n- 再 npm run dev\n\n"
                             "```js\nnpm run dev\n```\n\n"
                             "### 收尾\n\n"
                             "- 干完收工。\n";
    const auto want = WholeDocLines(text);
    CHECK(ScreenLines({text}) == want);  // 单笔
    CHECK(ScreenLines({"先说结论:**方案已定**。\n\n",
                       "### 运行方式\n\n",
                       "- 先 npm install\n- 再 npm run dev\n\n",
                       "```js\nnpm run dev\n```\n\n",
                       "### 收尾\n\n",
                       "- 干完收工。\n"}) == want);            // 跨块
    CHECK(ScreenLines(CharDeltas(text)) == want);               // 逐字
    // 标题前恰好一行(不多不少):整篇基准里每枚标题的上一行都是空行,
    // 上上行都不是。
    for (std::size_t i = 1; i < want.size(); ++i) {
        if (Contains(want[i], "运行方式") || Contains(want[i], "收尾")) {
            CHECK(want[i - 1].empty());
            CHECK_FALSE(want[i - 2].empty());
        }
    }
}

TEST_CASE("H5 渲染链: 无标记段落接标题——前距由原样空行保住,预案不叠加") {
    const std::string text = "纯段落没有标记。\n\n### 标题\n\n";
    const auto fed = Feed({text});
    const auto plans = FinalizedPlans(fed);
    REQUIRE(plans.size() == 1);  // 段落块没探到结构,只有标题块有收束预案
    CHECK_FALSE(plans[0].front().empty());  // 空行还在屏上:预案不带前距
    CHECK(ScreenLines({text}) == WholeDocLines(text));
    CHECK(ScreenLines(CharDeltas(text)) == WholeDocLines(text));
}

TEST_CASE("H6 渲染链: 连续多枚空行收成恰好一行——吸收在切段层办掉") {
    const std::string text = "段落收束。\n\n\n\n### 标题\n\n";
    const auto fed = Feed({text});
    std::vector<std::string> pieces;
    for (const BodyRenderStep& step : fed.steps) {
        if (!step.repaint) {
            pieces.push_back(step.piece);
        }
    }
    // 落笔段只有两枚:段落段(带着第一枚空行)与标题段;中间空行整枚吸收。
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0] == "段落收束。\n\n");
    CHECK(pieces[1] == "### 标题\n\n");
    CHECK(ScreenLines({text}) == WholeDocLines(text));
    // 空行逐笔到(跨 delta 的空行连发)同样收成一行。
    CHECK(ScreenLines({"段落收束。\n", "\n", "\n", "\n", "### 标题\n\n"}) == WholeDocLines(text));
    CHECK(ScreenLines(CharDeltas(text)) == WholeDocLines(text));
}

TEST_CASE("H7 渲染链: 标题在开头——不凭空多首行空白,开头空行照吸") {
    {
        const auto fed = Feed({"### 标题\n\n"});
        const auto lines = LastRepaintLines(fed);
        REQUIRE(lines.size() == 1);
        CHECK_FALSE(lines[0].empty());
        CHECK(Contains(StripAnsi(lines[0]), "标题"));
        CHECK(ScreenLines({"### 标题\n\n"}) == WholeDocLines("### 标题\n\n"));
    }
    {
        // 模型开头自带空行的写法:空行吸收,标题落首行,不垫首行空白。
        const std::string text = "\n\n### 标题\n\n";
        CHECK(ScreenLines({text}) == WholeDocLines(text));
        CHECK(ScreenLines(CharDeltas(text)) == WholeDocLines(text));
    }
}
