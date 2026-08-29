// 真实实测问题 1(流式粗体不渲染)的回归册:穿过"分块 → 收束/增量重画
// 渲染"的真实决策链(PlanBodyDelta:ScanBodyDelta 切步 + DetectMarkdownStructure
// + RenderMarkdown),不用真终端就能钉死问题 1 验收的渲染半边:
//   1. 单块与跨块两种 **粗体** 都剥星号上 bold;
//   2. 逐字 delta——标记闭合的那笔触发增量重画,不必苦等段尾空行;
//   3. 正文段后直接跟工具边界(无空行)的形状,让路定格(OnBlockBreak 兜底)
//      用的 PrepareBodyRenderPlan 渲染行同样剥星号;
//   4. 中英混排、窄终端 width-1 铁律;
//   5. 代码块内与 \** 转义字面量不误渲染。
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
