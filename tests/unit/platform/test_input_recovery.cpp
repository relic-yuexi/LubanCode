// Unicode/emoji 治理单(P2):输入等待与异常恢复。
// 纯状态机部分(两平台都跑):逐键 UTF-16 代理对配对、合批边界的代理跨批
// 账、bracketed paste 正文累积与截断、UTF-8 序列判定。
// POSIX 真路径部分(仅非 Windows):把进程 stdin 换成一根管道,喂受控
// 字节流真走 platform::KeyReader::ReadOne——坏续字节回退、粘贴半包的
// 有界恢复、§9.3 受控反例(ESC[200~😀 后不发结束标记再请求停止)。
// 凡有等待的路径外层一律带看门狗:std::async + wait_for 限时,超时先关
// 管道写端用 EOF 解围再收尸——测试自己不许挂死(挂死也由 ctest 的条目
// 时限兜底判红,180s)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <future>
#include <unistd.h>  // dup/dup2/pipe/write/close
#endif

#include "platform/console.hpp"
#include "platform/input_recovery.hpp"

using namespace lubancode::platform;

namespace {
// 测试用的宽字符代理项字面量:显式 static_cast,不靠 \x 转义的字面量吞字
// 规则,POSIX(wchar_t 32 位)与 Windows(16 位)同一份值。
constexpr wchar_t kHighD83D = static_cast<wchar_t>(0xD83D);
constexpr wchar_t kHighD83E = static_cast<wchar_t>(0xD83E);
constexpr wchar_t kLowDE00 = static_cast<wchar_t>(0xDE00);
constexpr wchar_t kLowDCA9 = static_cast<wchar_t>(0xDCA9);
}  // namespace

// ---------------------------------------------------------------------------
// 纯状态机:逐键 UTF-16 代理对配对
// ---------------------------------------------------------------------------

TEST_CASE("代理对配对:高代理+紧邻低代理拼成完整码点,状态清空") {
    SurrogatePairState machine;
    const auto first = machine.Feed(kHighD83D);
    CHECK(first.kind == SurrogateFeedResult::Kind::Pending);
    const auto second = machine.Feed(kLowDE00);
    CHECK(second.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(second.cp == U'\x1F600');  // 😀
    CHECK_FALSE(machine.pending_high.has_value());
}

TEST_CASE("代理对配对:孤立低代理交 U+FFFD,不带账不吞键") {
    SurrogatePairState machine;
    const auto fed = machine.Feed(kLowDE00);
    CHECK(fed.kind == SurrogateFeedResult::Kind::Replacement);
    CHECK(fed.cp == kReplacementCodePoint);
    CHECK_FALSE(machine.pending_high.has_value());
    const auto next = machine.Feed(U'x');
    CHECK(next.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(next.cp == U'x');
}

TEST_CASE("代理对配对:高代理后插入普通字符,这趟交 U+FFFD,普通字符回带重放") {
    SurrogatePairState machine;
    REQUIRE(machine.Feed(kHighD83D).kind == SurrogateFeedResult::Kind::Pending);
    const auto fed = machine.Feed(U'x');
    CHECK(fed.kind == SurrogateFeedResult::Kind::ReplacementWithRetry);
    CHECK(fed.cp == kReplacementCodePoint);
    CHECK(fed.retry == U'x');
    // 调用方把 retry 还回输入队列,下一趟重放:普通字符照常交付,不吞。
    const auto replay = machine.Feed(fed.retry);
    CHECK(replay.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(replay.cp == U'x');
}

TEST_CASE("代理对配对:连续两枚高代理,前一枚孤立交 U+FFFD,新一枚重新等") {
    SurrogatePairState machine;
    REQUIRE(machine.Feed(kHighD83D).kind == SurrogateFeedResult::Kind::Pending);
    const auto second = machine.Feed(kHighD83E);
    CHECK(second.kind == SurrogateFeedResult::Kind::Replacement);
    CHECK(second.cp == kReplacementCodePoint);
    REQUIRE(machine.pending_high.has_value());
    CHECK(*machine.pending_high == kHighD83E);
    const auto third = machine.Feed(kLowDCA9);
    CHECK(third.kind == SurrogateFeedResult::Kind::CodePoint);
    CHECK(third.cp == U'\x1F629');  // 😩(D83E DCA9)
}

TEST_CASE("代理对配对:Reset 勾销陈旧 pending,半截代理对不污染下一笔输入") {
    SurrogatePairState machine;
    REQUIRE(machine.Feed(kHighD83D).kind == SurrogateFeedResult::Kind::Pending);
    machine.Reset();  // 编辑键/取消/EOF/粘贴收尾的口径
    CHECK_FALSE(machine.pending_high.has_value());
    // 复位后迟到的低代理是孤立项,不许跟陈旧高代理错配。
    const auto late = machine.Feed(kLowDE00);
    CHECK(late.kind == SurrogateFeedResult::Kind::Replacement);
    CHECK(late.cp == kReplacementCodePoint);
}

TEST_CASE("代理对配对:emoji 面板拆半逐键重放,码点流保真") {
    // 流:D83D DE00 | 'a' | D83D 'b' | D83D D83E DE00 | 孤立 DCA9
    // 账:😀 一次、'a' 一次、孤立高 FFFD 后 'b' 重放、孤立高 FFFD 后
    //     😀 一次、孤立低 FFFD。
    const std::vector<wchar_t> stream = {
        kHighD83D, kLowDE00, U'a', kHighD83D, U'b', kHighD83D, kHighD83E, kLowDE00, kLowDCA9,
    };
    std::vector<char32_t> delivered;
    SurrogatePairState machine;
    for (const wchar_t wc : stream) {
        const auto fed = machine.Feed(wc);
        if (fed.kind == SurrogateFeedResult::Kind::Pending) {
            continue;
        }
        if (fed.kind == SurrogateFeedResult::Kind::ReplacementWithRetry) {
            delivered.push_back(fed.cp);
            delivered.push_back(machine.Feed(fed.retry).cp);  // 还回队列后的重放
            continue;
        }
        delivered.push_back(fed.cp);
    }
    const std::vector<char32_t> expected = {
        U'\x1F600', U'a', kReplacementCodePoint, U'b',
        kReplacementCodePoint, U'\x1F600', kReplacementCodePoint,
    };
    REQUIRE(delivered.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(delivered[i] == expected[i]);
    }
}

// ---------------------------------------------------------------------------
// 纯状态机:合批边界的代理跨批账
// ---------------------------------------------------------------------------

TEST_CASE("跨批携带:批尾停在半个代理对,尾高代理剥出留 pending") {
    std::wstring batch(15, L'a');
    batch.push_back(kHighD83D);
    const auto carry = ApplySurrogateCarry(batch, std::nullopt);
    CHECK(carry.text == std::wstring(15, L'a'));
    REQUIRE(carry.pending_high.has_value());
    CHECK(*carry.pending_high == kHighD83D);
}

TEST_CASE("跨批携带:上批尾高代理 + 本批领头低代理,拼回完整代理对") {
    std::wstring batch2(1, kLowDE00);
    batch2 += L"c";
    const auto carry = ApplySurrogateCarry(batch2, kHighD83D);
    REQUIRE(carry.text.size() == 3);
    CHECK(carry.text[0] == kHighD83D);
    CHECK(carry.text[1] == kLowDE00);  // 拼回的 D83D DE00 相邻 = 😀
    CHECK(carry.text[2] == L'c');
    CHECK_FALSE(carry.pending_high.has_value());
}

TEST_CASE("跨批携带:上批尾高代理 + 本批领头非低代理,替换符在批头交代") {
    const auto carry = ApplySurrogateCarry(std::wstring(L"xy"), kHighD83D);
    REQUIRE(carry.text.size() == 3);
    CHECK(carry.text[0] == static_cast<wchar_t>(kReplacementCodePoint));
    CHECK(carry.text.substr(1) == L"xy");
    CHECK_FALSE(carry.pending_high.has_value());
}

TEST_CASE("跨批携带:批尾高代理但剥后为空,不剥——空文本不许跨批悬空") {
    const auto carry = ApplySurrogateCarry(std::wstring(1, kHighD83D), std::nullopt);
    CHECK(carry.text.size() == 1);
    CHECK(carry.text[0] == kHighD83D);
    CHECK_FALSE(carry.pending_high.has_value());
}

TEST_CASE("跨批携带:空批不动 pending,留给下一批") {
    const auto carry = ApplySurrogateCarry(std::wstring(), kHighD83D);
    CHECK(carry.text.empty());
    REQUIRE(carry.pending_high.has_value());
    CHECK(*carry.pending_high == kHighD83D);
}

TEST_CASE("跨批携带:纯 ASCII 批不变形") {
    const auto carry = ApplySurrogateCarry(std::wstring(L"plain text"), std::nullopt);
    CHECK(carry.text == L"plain text");
    CHECK_FALSE(carry.pending_high.has_value());
}

// ---------------------------------------------------------------------------
// 纯状态机:bracketed paste 正文累积
// ---------------------------------------------------------------------------

TEST_CASE("粘贴累积:尾标记整对剥掉,正文原样收口") {
    PasteByteAccumulator acc;
    acc.end_marker = "\x1b[201~";
    for (const char c : std::string("hello")) {
        acc.Feed(c);
    }
    CHECK_FALSE(acc.finished());
    for (const char c : std::string("\x1b[201~")) {
        acc.Feed(c);
    }
    CHECK(acc.done);
    CHECK_FALSE(acc.truncated);
    CHECK(acc.text == "hello");
}

TEST_CASE("粘贴累积:尾标记劈开跨喂入仍能识别") {
    PasteByteAccumulator acc;
    acc.end_marker = "\x1b[201~";
    for (const char c : std::string("body\x1b[2")) {
        acc.Feed(c);
    }
    CHECK_FALSE(acc.done);
    for (const char c : std::string("01~")) {
        acc.Feed(c);
    }
    CHECK(acc.done);
    CHECK(acc.text == "body");
}

TEST_CASE("粘贴累积:取消键字节是正文——半包恢复手段才许接管") {
    // 边界(§6.3):合法粘贴正文里的 Ctrl+C 字节不是取消,是内容;只有
    // 协议半包(等结束标记)状态下的时限/上限/停止才是恢复手段。
    // (\x03 后面跟 'z':十六进制转义不吞非十六进制字符,'z' 断得干净。)
    PasteByteAccumulator acc;
    acc.end_marker = "\x1b[201~";
    for (const char c : std::string("x\x03z\x1b[201~")) {
        acc.Feed(c);
    }
    CHECK(acc.done);
    CHECK(acc.text == "x\x03z");
}

TEST_CASE("粘贴累积:大小上限触发截断,已收正文保留") {
    PasteWideAccumulator acc;
    acc.end_marker = L"\x1b[201~";
    acc.max_units = 4;
    for (const wchar_t c : std::wstring(L"abcde")) {
        if (acc.finished()) {
            break;
        }
        acc.Feed(c);
    }
    CHECK(acc.truncated);
    CHECK_FALSE(acc.done);
    CHECK(acc.text == L"abcd");
}

TEST_CASE("粘贴累积:收口后再喂无效") {
    PasteByteAccumulator acc;
    acc.end_marker = "\x1b[201~";
    for (const char c : std::string("ok\x1b[201~")) {
        acc.Feed(c);
    }
    REQUIRE(acc.done);
    acc.Feed('z');
    CHECK(acc.text == "ok");
}

TEST_CASE("粘贴界限取值:三笔界分开且取宽档,不误伤正常粘贴") {
    // 钉住取值(改动要过单):总时限 2s、空闲期限 1s、上限约 1M 单元、
    // 取消切片 25ms。参考既有 1000/2000ms 档。
    CHECK(kBracketedPasteTotalMs == 2000);
    CHECK(kBracketedPasteIdleMs == 1000);
    CHECK(kMaxPasteUnits == (std::size_t{1} << 20));
    CHECK(kPasteCancelSliceMs > 0);
    CHECK(kPasteCancelSliceMs <= 50);
}

// ---------------------------------------------------------------------------
// 纯状态机:UTF-8 序列判定
// ---------------------------------------------------------------------------

TEST_CASE("UTF-8 首字节形状:ASCII/2/3/4 字节与非法首字节") {
    CHECK(DecodeUtf8Lead('a').extra == 0);
    CHECK(DecodeUtf8Lead('a').cp == U'a');
    CHECK(DecodeUtf8Lead(0xC3).extra == 1);
    CHECK(DecodeUtf8Lead(0xE4).extra == 2);
    CHECK(DecodeUtf8Lead(0xF0).extra == 3);
    CHECK(DecodeUtf8Lead(0x80).extra < 0);  // 孤立续字节
    CHECK(DecodeUtf8Lead(0xF8).extra < 0);  // 超范围首字节
}

TEST_CASE("UTF-8 标量校验:超长/代理项/超范围一律不合法") {
    CHECK(IsValidUtf8Scalar(U'a', 0));
    CHECK(IsValidUtf8Scalar(U'\x00E9', 1));      // C3 A9
    CHECK_FALSE(IsValidUtf8Scalar(U'/', 1));     // C0 AF:超长
    CHECK(IsValidUtf8Scalar(U'\x4E2D', 2));      // E4 B8 AD
    CHECK_FALSE(IsValidUtf8Scalar(U'\xD800', 2));  // ED A0 80:代理项
    CHECK(IsValidUtf8Scalar(U'\x1F600', 3));     // F0 9F 98 80
    CHECK_FALSE(IsValidUtf8Scalar(U'\x110000', 3));  // F4 90 80 80:超范围
    CHECK_FALSE(IsValidUtf8Scalar(U'\x0800', 1));    // 0x800 编不满 2 字节
}

// ---------------------------------------------------------------------------
// POSIX 真路径:管道喂受控字节流,真走 KeyReader::ReadOne(带看门狗)
// ---------------------------------------------------------------------------
#ifndef _WIN32

namespace {

// 把进程 stdin 换成一根管道的 RAII:析构原样还原,别的测试册不受牵连。
class PipeStdin {
public:
    PipeStdin() {
        saved_ = dup(STDIN_FILENO);
        REQUIRE(saved_ >= 0);
        REQUIRE(pipe(fds_) == 0);
        REQUIRE(dup2(fds_[0], STDIN_FILENO) == STDIN_FILENO);
    }
    ~PipeStdin() {
        dup2(saved_, STDIN_FILENO);
        close(saved_);
        close(fds_[0]);
        if (fds_[1] >= 0) {
            close(fds_[1]);
        }
    }
    PipeStdin(const PipeStdin&) = delete;
    PipeStdin& operator=(const PipeStdin&) = delete;

    void Feed(std::string_view bytes) {
        REQUIRE(write(fds_[1], bytes.data(), bytes.size()) ==
                static_cast<ssize_t>(bytes.size()));
    }
    void ForceEof() {  // 看门狗超时的解围手段:EOF 让任何阻塞读必然返回
        if (fds_[1] >= 0) {
            close(fds_[1]);
            fds_[1] = -1;
        }
    }

private:
    int saved_ = -1;
    int fds_[2]{-1, -1};
};

// 看门狗包装的 ReadOne:另一线程跑读取,主线程限时等。超时先关管道写端
// (EOF 解围,读取线程必然从等待里出来)、收尸、判 FAIL——测试自己不挂死,
// 红也红得有名有姓。预算按各路径的界加足余量(最长合法等待 ≈ 总时限 2s)。
std::optional<KeyInput> ReadOneWatchdog(KeyReader& reader, PipeStdin& pipe, int budget_ms,
                                        const char* what) {
    std::future<std::optional<KeyInput>> fut = std::async(std::launch::async,
                                                          [&reader] { return reader.ReadOne(); });
    if (fut.wait_for(std::chrono::milliseconds(budget_ms)) != std::future_status::ready) {
        pipe.ForceEof();
        (void)fut.get();  // 收尸:EOF 解围后必然返回
        FAIL_CHECK("ReadOne 看门狗超时(预算 " << budget_ms << "ms): " << what);
        return std::nullopt;
    }
    return fut.get();
}

}  // namespace

TEST_CASE("POSIX ReadOne:合法 UTF-8 多字节逐字节保真") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed("\xC3\xA9");      // é
    stdin_pipe.Feed("\xF0\x9F\x98\x80");  // 😀
    const auto e = ReadOneWatchdog(reader, stdin_pipe, 2000, "2 字节序列");
    REQUIRE(e.has_value());
    CHECK(e->kind == KeyInput::Kind::Char);
    CHECK(e->ch == U'\x00E9');
    const auto emoji = ReadOneWatchdog(reader, stdin_pipe, 2000, "4 字节序列");
    REQUIRE(emoji.has_value());
    CHECK(emoji->kind == KeyInput::Kind::Char);
    CHECK(emoji->ch == U'\x1F600');
}

TEST_CASE("POSIX ReadOne:坏续字节放回,后续普通字符不吞(§3.5)") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed("\xC3x");
    const auto bad = ReadOneWatchdog(reader, stdin_pipe, 2000, "坏序列");
    REQUIRE(bad.has_value());
    CHECK(bad->kind == KeyInput::Kind::None);  // 坏前缀丢弃,'x' 已放回队头
    const auto x = ReadOneWatchdog(reader, stdin_pipe, 2000, "放回的普通字符");
    REQUIRE(x.has_value());
    CHECK(x->kind == KeyInput::Kind::Char);
    CHECK(x->ch == U'x');
}

TEST_CASE("POSIX ReadOne:坏序列后的取消键不吞(§3.5)") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed("\xC3\x03");
    const auto bad = ReadOneWatchdog(reader, stdin_pipe, 2000, "坏序列");
    REQUIRE(bad.has_value());
    CHECK(bad->kind == KeyInput::Kind::None);
    const auto cancel = ReadOneWatchdog(reader, stdin_pipe, 2000, "放回的 Ctrl+C");
    REQUIRE(cancel.has_value());
    CHECK(cancel->kind == KeyInput::Kind::CtrlC);
}

TEST_CASE("POSIX ReadOne:超长/代理项/超范围序列按 U+FFFD 交付") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed(std::string("\xC0\xAF", 2));      // 超长 '/'
    stdin_pipe.Feed(std::string("\xED\xA0\x80", 3));  // 代理项 U+D800
    stdin_pipe.Feed(std::string("\xF4\x90\x80\x80", 4));  // U+110000
    for (int i = 0; i < 3; ++i) {
        const auto fed = ReadOneWatchdog(reader, stdin_pipe, 2000, "坏标量序列");
        REQUIRE(fed.has_value());
        CHECK(fed->kind == KeyInput::Kind::Char);
        CHECK(fed->ch == kReplacementCodePoint);
    }
}

TEST_CASE("POSIX ReadOne:完整 bracketed paste 照常收口,不误伤") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed(std::string("\x1b[200~ok\x1b[201~"));
    const auto paste = ReadOneWatchdog(reader, stdin_pipe, 4000, "完整粘贴");
    REQUIRE(paste.has_value());
    CHECK(paste->kind == KeyInput::Kind::Paste);
    CHECK(paste->text == "ok");
    CHECK_FALSE(paste->truncated);
}

TEST_CASE("POSIX ReadOne:粘贴半包无结束标记,空闲期限内按截断收场") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed(std::string("\x1b[200~hello", 11));
    const auto start = std::chrono::steady_clock::now();
    // 看门狗预算 6s(总时限 2s + 余量);断言写清时限:空闲期限 1s 收口。
    const auto paste = ReadOneWatchdog(reader, stdin_pipe, 6000, "半包粘贴(无结束标记)");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    REQUIRE(paste.has_value());
    CHECK(paste->kind == KeyInput::Kind::Paste);
    CHECK(paste->text == "hello");  // 已收正文保留
    CHECK(paste->truncated);        // 明确截断,不装完整
    CHECK(elapsed.count() >= 900);  // 不到空闲期限就撒手 = 误伤正常慢速粘贴
    CHECK(elapsed.count() <= 4500); // 超过总时限+余量还不出手 = 无界等待复发
}

TEST_CASE("POSIX ReadOne:粘贴半包一枚正文没有,当没来过(None)") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    stdin_pipe.Feed(std::string("\x1b[200~", 6));
    const auto none = ReadOneWatchdog(reader, stdin_pipe, 6000, "空半包");
    REQUIRE(none.has_value());
    CHECK(none->kind == KeyInput::Kind::None);
}

TEST_CASE("POSIX ReadOne(§9.3 受控反例):ESC[200~😀 后不发结束标记,请求停止须有限时间返回") {
    PipeStdin stdin_pipe;
    KeyReader reader;
    std::atomic<bool> stop{false};
    reader.set_cancel_flag(&stop);
    stdin_pipe.Feed(std::string("\x1b[200~\xF0\x9F\x98\x80", 10));
    // 250ms 后请求停止(模拟监听线程 Stop():置旗后 join)。给足 250ms 是
    // 叫读取线程先把管道里的正文收进累积器(字节早已在管道里,正常几十
    // 微秒的事),再验证"停止请求打断的是半包等待",不是"还没开吃"。
    std::thread stopper([&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        stop.store(true, std::memory_order_release);
    });
    const auto start = std::chrono::steady_clock::now();
    const auto paste = ReadOneWatchdog(reader, stdin_pipe, 6000, "停止打断粘贴半包等待");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    stopper.join();
    REQUIRE(paste.has_value());
    CHECK(paste->kind == KeyInput::Kind::Paste);
    CHECK(paste->text == std::string("\xF0\x9F\x98\x80", 4));  // 已收 😀 正文保留
    CHECK(paste->truncated);
    // 区分度:停止被尊重 ≈ 250ms 置旗 + 25ms 切片;停止被无视则要干等
    // 空闲期限 1000ms。取 900ms 做界(留足调度余量,又压得住倒退)。
    CHECK_MESSAGE(elapsed_ms < 900, "停止请求未被及时尊重,耗时: " << elapsed_ms << "ms");
}

TEST_CASE("POSIX ReadOne:取消旗在无等待路径上不拦正常键") {
    PipeStdin stdin_pipe;
    std::atomic<bool> stop{false};
    KeyReader reader;
    reader.set_cancel_flag(&stop);
    CHECK_FALSE(reader.CancelRequested());
    stdin_pipe.Feed("a");
    const auto key = ReadOneWatchdog(reader, stdin_pipe, 2000, "取消旗未置时的普通键");
    REQUIRE(key.has_value());
    CHECK(key->kind == KeyInput::Kind::Char);
    CHECK(key->ch == U'a');
    stop.store(true, std::memory_order_release);
    CHECK(reader.CancelRequested());
}

#endif  // !_WIN32
