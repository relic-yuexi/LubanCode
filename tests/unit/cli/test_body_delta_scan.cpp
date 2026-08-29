// P2-4 重绘风暴的一半账:一笔 delta 的切段纯函数 ScanBodyDelta。
// 旧实现同一笔 delta 里第二段的收束重画会把第一段再渲染一遍、铺在错
// 的锚点上(UI 泵按 33ms 并批,一笔 delta 常跨好几段)——重复渲染是
// Plan 采样 166k 字符对 16k 正文那十倍账的大头。这册钉死:
//   1. 一段只触发一次收束重画,渲染的是自己的原文;
//   2. 收口即清零,后一段不再回头渲染旧段;
//   3. 代码围栏内的空行不是段落边界;
//   4. 跨 delta 的块账(buffer 续攒)不断。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/live_transcript.hpp"

using lubancode::cli::BodyScanState;
using lubancode::cli::ScanBodyDelta;

namespace {

std::vector<std::string> RepaintTexts(const std::vector<lubancode::cli::BodyDeltaStep>& steps) {
    // 只收"空行收束"的重画步(finalize=true);行/标记边界的增量步另算
    // (IncrementalTexts)——两型重画的账要分开钉。
    std::vector<std::string> out;
    for (const auto& step : steps) {
        if (step.repaint && step.finalize) {
            out.push_back(step.text);
        }
    }
    return out;
}

std::vector<std::string> IncrementalTexts(const std::vector<lubancode::cli::BodyDeltaStep>& steps) {
    std::vector<std::string> out;
    for (const auto& step : steps) {
        if (step.repaint && !step.finalize) {
            out.push_back(step.text);
        }
    }
    return out;
}

std::vector<std::string> PrintPieces(const std::vector<lubancode::cli::BodyDeltaStep>& steps) {
    std::vector<std::string> out;
    for (const auto& step : steps) {
        if (!step.repaint) {
            out.push_back(step.text);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("body delta: 单段一笔,空行触发一次收束") {
    BodyScanState state;
    const auto steps = ScanBodyDelta(state, "第一段正文\n\n", "");
    REQUIRE(RepaintTexts(steps).size() == 1);
    CHECK(RepaintTexts(steps)[0] == "第一段正文\n\n");
    REQUIRE(PrintPieces(steps).size() == 1);
    CHECK(PrintPieces(steps)[0] == "第一段正文\n\n");
}

TEST_CASE("body delta: 一笔 delta 跨三段,各段只渲染自己(不再重复渲染旧段)") {
    BodyScanState state;
    const auto steps = ScanBodyDelta(state, "段一\n\n段二\n\n段三\n\n", "");
    const auto repaints = RepaintTexts(steps);
    REQUIRE(repaints.size() == 3);
    CHECK(repaints[0] == "段一\n\n");
    CHECK(repaints[1] == "段二\n\n");
    CHECK(repaints[2] == "段三\n\n");
    // 全文拼回不丢字:三段原文各自落笔。
    const auto pieces = PrintPieces(steps);
    REQUIRE(pieces.size() == 3);
    CHECK(pieces[0] == "段一\n\n");
    CHECK(pieces[1] == "段二\n\n");
    CHECK(pieces[2] == "段三\n\n");
}

TEST_CASE("body delta: 一万段连发的渲染账是 O(段数),没有平方级回头账") {
    BodyScanState state;
    std::string text;
    for (int i = 0; i < 10000; ++i) {
        text += "段" + std::to_string(i) + "\n\n";
    }
    const auto steps = ScanBodyDelta(state, text, "");
    REQUIRE(RepaintTexts(steps).size() == 10000);
    std::size_t total_bytes = 0;
    for (const std::string& block : RepaintTexts(steps)) {
        total_bytes += block.size();
    }
    // 每段渲染一次:总账约等于全文,不得出现"回头渲染旧段"的翻倍。
    CHECK(total_bytes <= text.size() + 1);
}

TEST_CASE("body delta: 代码围栏内的空行不是段落边界,围栏闭合后的空行才算") {
    BodyScanState state;
    const auto steps = ScanBodyDelta(state, "```python\na = 1\n\nb = 2\n```\n\n尾段", "");
    const auto repaints = RepaintTexts(steps);
    REQUIRE(repaints.size() == 1);
    CHECK(repaints[0] == "```python\na = 1\n\nb = 2\n```\n\n");
    // 尾段没以空行收口:只落笔不收束。
    const auto pieces = PrintPieces(steps);
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[1] == "尾段");
}

TEST_CASE("body delta: 跨 delta 续块——block_so_far 带上,收束渲染整块") {
    BodyScanState state;
    auto first = ScanBodyDelta(state, "开头半句", "");
    REQUIRE(first.size() == 1);
    CHECK(first[0].repaint == false);

    const auto second = ScanBodyDelta(state, "结尾\n\n", "开头半句");
    const auto repaints = RepaintTexts(second);
    REQUIRE(repaints.size() == 1);
    CHECK(repaints[0] == "开头半句结尾\n\n");
}

TEST_CASE("body delta: 只敲空格的行也算空行边界(markdown 段落规矩)") {
    BodyScanState state;
    const auto steps = ScanBodyDelta(state, "a\n  \nb\n\n", "");
    const auto repaints = RepaintTexts(steps);
    REQUIRE(repaints.size() == 2);
    CHECK(repaints[0] == "a\n  \n");
    CHECK(repaints[1] == "b\n\n");
}

TEST_CASE("body delta: 行首制表符的空行同样是边界") {
    BodyScanState state;
    const auto steps = ScanBodyDelta(state, "a\n\t\nb\n\n", "");
    const auto repaints = RepaintTexts(steps);
    REQUIRE(repaints.size() == 2);
    CHECK(repaints[0] == "a\n\t\n");
    CHECK(repaints[1] == "b\n\n");
}

// ---- 增量重画(问题 1:流式粗体不渲染) --------------------------------------

TEST_CASE("body delta: 同一笔里闭合的粗体触发一次增量重画,空行收束照旧") {
    BodyScanState state;
    const auto steps = ScanBodyDelta(state, "方案:**React 前端**落地。", "");
    // 没有空行:没有收束步;`**` 新配成一对 → 一枚增量步,块继续攒。
    CHECK(RepaintTexts(steps).empty());
    const auto incs = IncrementalTexts(steps);
    REQUIRE(incs.size() == 1);
    CHECK(incs[0] == "方案:**React 前端**落地。");
    // 紧跟着的空行到达:收束步渲染整段,增量账清零。
    const auto after = ScanBodyDelta(state, "\n\n", "方案:**React 前端**落地。");
    REQUIRE(RepaintTexts(after).size() == 1);
    CHECK(IncrementalTexts(after).empty());
}

TEST_CASE("body delta: 纯文字滴流不重画,行到/标记闭合才画") {
    BodyScanState state;
    CHECK(IncrementalTexts(ScanBodyDelta(state, "一句没有标记的", "")).empty());
    CHECK(IncrementalTexts(ScanBodyDelta(state, "普通散文,继续攒。", "一句没有标记的")).empty());
    // 新完整行到达:重画。
    const auto lined = ScanBodyDelta(state, "第一行完\n", "");
    REQUIRE(IncrementalTexts(lined).size() == 1);
    CHECK(IncrementalTexts(lined)[0] == "第一行完\n");
    // 同块再无新行、无新配对:不再画。
    CHECK(IncrementalTexts(ScanBodyDelta(state, "第二行半句", "第一行完\n")).empty());
    // 又一对行内码闭合:再画。
    const auto coded = ScanBodyDelta(state, "带 `code` 的", "第一行完\n第二行半句");
    REQUIRE(IncrementalTexts(coded).size() == 1);
    CHECK(IncrementalTexts(coded)[0] == "第一行完\n第二行半句带 `code` 的");
}

TEST_CASE("body delta: 逐字 delta——星号闭合的那一笔触发增量重画") {
    BodyScanState state;
    std::string block;
    std::size_t repaints = 0;
    for (const char c : std::string("**粗体**")) {
        const auto steps = ScanBodyDelta(state, std::string(1, c), block);
        block += c;
        repaints += IncrementalTexts(steps).size();
    }
    // 第二枚 ** 闭合 → 恰好一次增量重画;其余各笔(含开标记那笔)不画。
    CHECK(repaints == 1);
}

TEST_CASE("body delta: 超预算的长块退回只等空行收束,不逐行重画") {
    BodyScanState state;
    std::string block;
    std::size_t incremental = 0;
    for (int i = 0; i < 60; ++i) {
        const std::string delta = "行" + std::to_string(i) + "\n";
        const auto steps = ScanBodyDelta(state, delta, block);
        block += delta;
        incremental += IncrementalTexts(steps).size();
    }
    // 预算 48 行:前 48 行逐行画,其后不再产增量步。
    CHECK(incremental == 48);
}
