// P2-4 粘贴归一:同批字符折一次粘贴事务的纯判别(平台无关)。
// ConPTY/VS Code 把一次粘贴拆成一批逐字符 KEY_EVENT,单行内容既没有
// bracketed 标记也没有换行,旧路逐字当打字交付——每个字一轮终端帧。
// 这册钉死判别的三桩账:阈值、标记剥离、尾标记未到。

#include <doctest/doctest.h>

#include <string>

#include "platform/paste_burst.hpp"

using lubancode::platform::ClassifyTextBurst;
using lubancode::platform::kPasteBurstThreshold;

TEST_CASE("paste burst: 少量字符是打字,不折事务") {
    const auto decision = ClassifyTextBurst(L"你好");
    CHECK_FALSE(decision.is_paste);
    CHECK_FALSE(decision.had_markers);
    CHECK(decision.text == L"你好");
}

TEST_CASE("paste burst: 同批攒过阈值按粘贴收") {
    std::wstring burst;
    for (int i = 0; i < static_cast<int>(kPasteBurstThreshold) + 1; ++i) {
        burst += L"字";
    }
    const auto decision = ClassifyTextBurst(burst);
    CHECK(decision.is_paste);
    CHECK_FALSE(decision.had_markers);
    CHECK(decision.text == burst);
}

TEST_CASE("paste burst: 恰好阈值也收——一万字粘贴的帧数帽靠它") {
    std::wstring burst(kPasteBurstThreshold, L'x');
    CHECK(ClassifyTextBurst(burst).is_paste);
}

TEST_CASE("paste burst: 终端拆散的 bracketed 标记整对剥掉,正文原样") {
    const std::wstring burst = L"\x1b[200~" + std::wstring(20, L'中') + L"\x1b[201~";
    const auto decision = ClassifyTextBurst(burst);
    CHECK(decision.is_paste);
    CHECK(decision.had_markers);
    CHECK(decision.text == std::wstring(20, L'中'));
}

TEST_CASE("paste burst: 头标记前的杂字保留,标记只剥协议本身") {
    const std::wstring burst = L"a\x1b[200~bc\x1b[201~d";
    const auto decision = ClassifyTextBurst(burst);
    CHECK(decision.is_paste);
    CHECK(decision.text == L"abcd");
}

TEST_CASE("paste burst: 尾标记没到(批次从中间切开)先收正文,短的不折") {
    const std::wstring burst = L"\x1b[200~ab";
    const auto decision = ClassifyTextBurst(burst);
    CHECK(decision.is_paste);   // 带标记的整批不论长短都按粘贴收
    CHECK(decision.had_markers);
    CHECK(decision.text == L"ab");
}

TEST_CASE("paste burst: 多行正文原样带换行,归一交给上层") {
    std::wstring burst = L"\x1b[200~alpha\r\nbeta\r\n\x1b[201~";
    const auto decision = ClassifyTextBurst(burst);
    CHECK(decision.text == L"alpha\r\nbeta\r\n");
}
