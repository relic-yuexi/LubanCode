// P2-4 粘贴归一:同批字符折一次粘贴事务的纯判别(平台无关)。
// ConPTY/VS Code 把一次粘贴拆成一批逐字符 KEY_EVENT,单行内容既没有
// bracketed 标记也没有换行,旧路逐字当打字交付——每个字一轮终端帧。
// 这册钉死判别的三桩账:阈值、标记剥离、尾标记未到。
//
// Unicode/emoji 治理单 §9.1 增册:合批边界 × 代理对跨批重放。阈值按
// UTF-16 单元数算,批次恰好停在半个代理对上时(受控反例:第一批
// 15×'a'+D83D,第二批 DE00),ClassifyTextBurst 只管判"是不是粘贴",
// 代理对的跨批账由 ApplySurrogateCarry(+不够阈值时的逐键状态机)接手
// ——两层拼起来必须把 😀 还原出来,不许整批转码变成替换符。

#include <doctest/doctest.h>

#include <string>

#include "platform/input_recovery.hpp"
#include "platform/paste_burst.hpp"

using lubancode::platform::ApplySurrogateCarry;
using lubancode::platform::ClassifyTextBurst;
using lubancode::platform::SurrogateFeedResult;
using lubancode::platform::SurrogatePairState;
using lubancode::platform::kPasteBurstThreshold;

namespace {
constexpr wchar_t kHighD83D = static_cast<wchar_t>(0xD83D);
constexpr wchar_t kLowDE00 = static_cast<wchar_t>(0xDE00);
}  // namespace

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

// ---------------------------------------------------------------------------
// Unicode/emoji 治理单 §9.1:合批边界 × 代理对跨批重放。重放按生产代码的
// 组合次序:ClassifyTextBurst(判粘贴/剥标记)→ 够阈值的批走
// ApplySurrogateCarry(尾高代理剥出跨批/领头低代理拼回);不够阈值的批
// 不进合批路,字符走逐键状态机(Windows ReadOne 的字符分支)。
// ---------------------------------------------------------------------------

TEST_CASE("paste burst × 代理跨批:第一批 15a+D83D、第二批 DE00,😀 还原(§9.1 受控反例)") {
    // 第一批 15×'a'+D83D 恰好 16 单元,达到阈值折成粘贴——尾高代理剥出
    // pending,整批不许把它变成替换符。
    std::wstring batch1(15, L'a');
    batch1.push_back(kHighD83D);
    const auto d1 = ClassifyTextBurst(batch1);
    REQUIRE(d1.is_paste);
    const auto carry1 = ApplySurrogateCarry(d1.text, std::nullopt);
    CHECK(carry1.text == std::wstring(15, L'a'));
    REQUIRE(carry1.pending_high.has_value());

    // 第二批只有一枚 DE00(不够阈值、不带标记):合批不折,走逐键状态机,
    // pending 里的 D83D 与它拼回完整码点。
    const auto d2 = ClassifyTextBurst(std::wstring(1, kLowDE00));
    REQUIRE_FALSE(d2.is_paste);
    SurrogatePairState machine;
    machine.pending_high = carry1.pending_high;
    const auto fed = machine.Feed(kLowDE00);
    CHECK(fed.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(fed.cp == U'\x1F600');  // 😀
    CHECK_FALSE(machine.pending_high.has_value());
}

TEST_CASE("paste burst × 代理跨批:两批都够阈值,拼回发生在线头(线头低代理前置高代理)") {
    std::wstring batch1(15, L'a');
    batch1.push_back(kHighD83D);
    const auto carry1 = ApplySurrogateCarry(ClassifyTextBurst(batch1).text, std::nullopt);
    REQUIRE(carry1.pending_high.has_value());

    // 第二批 DE00 + 20×'b':21 单元够阈值,合批路拼回后 D83D DE00 相邻,
    // WideToUtf8 转码即得 😀(这里按宽串相邻性断言,转码口径由平台侧管)。
    std::wstring batch2(1, kLowDE00);
    batch2.append(20, L'b');
    const auto d2 = ClassifyTextBurst(batch2);
    REQUIRE(d2.is_paste);
    const auto carry2 = ApplySurrogateCarry(d2.text, carry1.pending_high);
    REQUIRE(carry2.text.size() == 21);
    CHECK(carry2.text[0] == kHighD83D);
    CHECK(carry2.text[1] == kLowDE00);  // 😀 相邻还原
    CHECK(carry2.text.substr(2) == std::wstring(20, L'b'));
    CHECK_FALSE(carry2.pending_high.has_value());
}

TEST_CASE("paste burst × 代理跨批:阈值前(15 单元含高代理)不折批,逐键路同样还原") {
    std::wstring batch1(14, L'a');
    batch1.push_back(kHighD83D);  // 15 单元,差一枚不到阈值
    const auto d1 = ClassifyTextBurst(batch1);
    REQUIRE_FALSE(d1.is_paste);
    SurrogatePairState machine;
    for (const wchar_t wc : batch1) {
        (void)machine.Feed(wc);
    }
    REQUIRE(machine.pending_high.has_value());
    const auto fed = machine.Feed(kLowDE00);
    CHECK(fed.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(fed.cp == U'\x1F600');
}

TEST_CASE("paste burst × 代理跨批:编辑键插入——批尾半对随编辑键复位勾销,不带账") {
    // 批 15a+D83D 折成粘贴(15a),尾高代理 pending;下一枚是编辑键
    // (退格/回车/取消那一类),生产代码走 reset_text_run():pending 一并
    // 勾销。迟到的 DE00 便是孤立项,按替换交付——不许跟陈旧高代理错配。
    std::wstring batch1(15, L'a');
    batch1.push_back(kHighD83D);
    const auto carry1 = ApplySurrogateCarry(ClassifyTextBurst(batch1).text, std::nullopt);
    REQUIRE(carry1.pending_high.has_value());

    SurrogatePairState machine;
    machine.pending_high = carry1.pending_high;
    machine.Reset();  // 编辑键/取消/EOF 的复位口径
    const auto late = machine.Feed(kLowDE00);
    CHECK(late.kind == SurrogateFeedResult::Kind::Replacement);
    CHECK(late.cp == lubancode::platform::kReplacementCodePoint);
}

TEST_CASE("paste burst × 代理跨批:粘贴退出——带尾标记的批剥标记后,尾高代理照样跨批") {
    // 终端把 bracketed 标记拆散混进批次:标记剥掉后正文以高代理收尾
    // (发送方把 😀 劈在了结束标记之后,下一批才到 DE00),跨批账同样成立。
    std::wstring batch1 = L"\x1b[200~";
    batch1.append(20, L'中');
    batch1.push_back(kHighD83D);
    batch1 += L"\x1b[201~";
    const auto d1 = ClassifyTextBurst(batch1);
    REQUIRE(d1.is_paste);
    CHECK(d1.had_markers);
    const auto carry1 = ApplySurrogateCarry(d1.text, std::nullopt);
    CHECK(carry1.text == std::wstring(20, L'中'));
    REQUIRE(carry1.pending_high.has_value());

    const auto fed = SurrogatePairState{carry1.pending_high}.Feed(kLowDE00);
    CHECK(fed.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(fed.cp == U'\x1F600');
}

TEST_CASE("paste burst × 代理跨批:孤立低代理打头的大批原样折叠,替换交给转码层") {
    // 没有 pending 高代理却以低代理打头:携带函数不动它——替换符的交代
    // 在 WideToUtf8 那层(paths_win 的孤立代理替换策略),纯函数不越权
    // 改写批内容;批本身照常折叠成粘贴。
    std::wstring batch(1, kLowDE00);
    batch.append(kPasteBurstThreshold, L'x');
    const auto d = ClassifyTextBurst(batch);
    REQUIRE(d.is_paste);
    const auto carry = ApplySurrogateCarry(d.text, std::nullopt);
    CHECK(carry.text == batch);  // 原样带过,孤立低代理由转码层替换成 U+FFFD
    CHECK_FALSE(carry.pending_high.has_value());
}
