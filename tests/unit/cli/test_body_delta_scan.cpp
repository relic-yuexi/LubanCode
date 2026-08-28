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
    std::vector<std::string> out;
    for (const auto& step : steps) {
        if (step.repaint) {
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
