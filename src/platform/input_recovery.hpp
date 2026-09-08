// Unicode/emoji 治理单(P2)的输入等待与异常恢复:可注入的纯状态机,不碰
// 控制台,两平台单测直接喂受控流。四块账分开立,谁也不替谁做主:
//   1. SurrogatePairState    Windows 逐键 UTF-16 代理对配对——高代理只与
//      紧邻合法低代理配对;孤立低代理、高代理后插入普通字符按 U+FFFD
//      可解释交付,后续键不吞;编辑键/取消/EOF 的复位走 Reset()。
//   2. ApplySurrogateCarry   合批边界(console_win.cpp 的
//      TryReadCoalescedTextBurst/TryReadNativePasteBurst)的代理跨批账:
//      批尾停在半个代理对上,尾高代理剥出来留 pending 跨批;上一批留下的
//      高代理遇上本批领头低代理,先拼回再整批转码(§9.1 受控反例:第一批
//      15×'a'+D83D,第二批 DE00,要能还原成 😀)。
//   3. BasicPasteAccumulator bracketed paste 正文的逐单元累积:尾标记识别
//      (含标记劈开跨喂入)、大小上限、截断记账。总时限/空闲期限是调用方
//      拿时钟执行的口径,这里只管单元账——三笔界分开(§6.3)。
//   4. UTF-8 序列判定        POSIX 逐键解码的纯件:首字节形状、续字节
//      形状、收齐后的标量值校验(超长/代理项/超范围按替换交付)。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::platform {

// 坏输入的统一交代口径:可解释地交付替换符,继续前进,不吞后续键
//(§9.1 引 paths_win.cpp 宽窄转换那套替换策略)。
inline constexpr char32_t kReplacementCodePoint = 0xFFFD;

inline bool IsHighSurrogate(wchar_t wc) { return wc >= 0xD800 && wc <= 0xDBFF; }
inline bool IsLowSurrogate(wchar_t wc) { return wc >= 0xDC00 && wc <= 0xDFFF; }

// ---------------------------------------------------------------------------
// 1. 逐键 UTF-16 代理对配对
// ---------------------------------------------------------------------------

struct SurrogateFeedResult {
    enum class Kind {
        Pending,               // 这枚高代理先记下,等紧邻低代理凑成完整码点
        CodePoint,             // cp 有效:完整码点(BMP 直送或拼好的代理对)
        Replacement,           // 坏输入交 U+FFFD(cp=U+FFFD),这枚字符已了结
        ReplacementWithRetry,  // 高代理后插入普通字符:这趟交 U+FFFD,普通
                               // 字符没被消费(retry 原样带回,调用方还回
                               // 输入队列,下一趟照常交付)——不吞键
    };
    Kind kind = Kind::Pending;
    char32_t cp = 0;
    wchar_t retry = 0;
};

struct SurrogatePairState {
    std::optional<wchar_t> pending_high;

    SurrogateFeedResult Feed(wchar_t wc) {
        SurrogateFeedResult out;
        if (IsHighSurrogate(wc)) {
            if (pending_high.has_value()) {
                // 连着两枚高代理:前一枚孤立,按替换策略交付;新的一枚重新等。
                pending_high = wc;
                out.kind = SurrogateFeedResult::Kind::Replacement;
                out.cp = kReplacementCodePoint;
                return out;
            }
            pending_high = wc;
            out.kind = SurrogateFeedResult::Kind::Pending;
            return out;
        }
        if (IsLowSurrogate(wc)) {
            if (pending_high.has_value()) {
                const wchar_t high = *pending_high;
                pending_high.reset();
                out.kind = SurrogateFeedResult::Kind::CodePoint;
                out.cp = 0x10000 + ((static_cast<char32_t>(high) - 0xD800) << 10) +
                         (static_cast<char32_t>(wc) - 0xDC00);
                return out;
            }
            // 孤立低代理:替换交付,不带账、不吞后面的键。
            out.kind = SurrogateFeedResult::Kind::Replacement;
            out.cp = kReplacementCodePoint;
            return out;
        }
        if (pending_high.has_value()) {
            // 高代理后插入普通字符:旧高代理孤立。这趟先交 U+FFFD,普通
            // 字符原样带回(还回输入队列),下一趟照常交付。
            pending_high.reset();
            out.kind = SurrogateFeedResult::Kind::ReplacementWithRetry;
            out.cp = kReplacementCodePoint;
            out.retry = wc;
            return out;
        }
        out.kind = SurrogateFeedResult::Kind::CodePoint;
        out.cp = static_cast<char32_t>(wc);
        return out;
    }

    // 编辑键/取消/EOF/模式切换:陈旧 pending 一笔勾销,不带账进下一笔输入
    //(§6.3)。半截代理对就此丢弃——那半枚本来就没进过编辑器。
    void Reset() { pending_high.reset(); }
};

// ---------------------------------------------------------------------------
// 2. 合批边界的代理对跨批账
// ---------------------------------------------------------------------------

struct SurrogateCarry {
    std::wstring text;                    // 处理后的批文本(prior + 本批剥标记后)
    std::optional<wchar_t> pending_high;  // 处理后仍 pending 的高代理(跨批带走)
};

// 把"上一批留下的 pending 高代理"与"本批文本的边界代理"对齐:
//   - carried 高代理 + 本批领头低代理:拼回串头,跨批代理对还原;
//   - carried 高代理 + 领头不是低代理:carried 孤立,替换符在批头交代;
//   - 批尾停在半个代理对(且剥后非空):尾高代理剥出,留给下一批,
//     不许整批 WideToUtf8 把它变成替换符(§9.1)。
inline SurrogateCarry ApplySurrogateCarry(std::wstring text, std::optional<wchar_t> carried_high) {
    SurrogateCarry out;
    if (carried_high.has_value() && !text.empty()) {
        if (IsLowSurrogate(text.front())) {
            text.insert(text.begin(), *carried_high);
            carried_high.reset();
        } else {
            text.insert(text.begin(), static_cast<wchar_t>(kReplacementCodePoint));
            carried_high.reset();
        }
    }
    if (text.size() >= 2 && IsHighSurrogate(text.back())) {
        carried_high = text.back();
        text.pop_back();
    }
    out.text = std::move(text);
    out.pending_high = carried_high;
    return out;
}

// ---------------------------------------------------------------------------
// 3. bracketed paste 正文累积
// ---------------------------------------------------------------------------

// 三笔界分开(§6.3:总时限、空闲期限、大小上限各是各的;取值要宽,参考
// 既有 1000/2000ms 档,正常慢速/大文本粘贴不误伤):
inline constexpr int kBracketedPasteTotalMs = 2000;         // 总时限
inline constexpr int kBracketedPasteIdleMs = 1000;          // 空闲期限
inline constexpr std::size_t kMaxPasteUnits = std::size_t{1} << 20;  // 大小上限(POSIX 按
                                                                    // 字节、Windows 按 UTF-16
                                                                    // 单元,约 1M 单元)
// 取消旗的切片粒度:等待拆成这么小的片,片间查一次停止请求——粘贴半包
// 等待可在这一粒度内被 Stop() 打断(§9.3),不必干等满空闲期限。
inline constexpr int kPasteCancelSliceMs = 25;

// 喂单元的累积器。done = 尾标记到手,正文完整;truncated = 大小上限
// (这里)或时限/空闲/取消/EOF(调用方置位)所致的半途截断,已收正文保留。
// 收口后再喂无效。
template <typename CharT>
struct BasicPasteAccumulator {
    std::basic_string<CharT> text;
    bool done = false;
    bool truncated = false;
    std::size_t max_units = kMaxPasteUnits;
    std::basic_string_view<CharT> end_marker;

    bool finished() const { return done || truncated; }

    void Feed(CharT unit) {
        if (finished()) {
            return;
        }
        text.push_back(unit);
        if (!end_marker.empty() && end_marker.size() <= text.size() &&
            text.compare(text.size() - end_marker.size(), end_marker.size(), end_marker) == 0) {
            text.resize(text.size() - end_marker.size());
            done = true;
            return;
        }
        if (text.size() >= max_units) {
            truncated = true;  // 大小上限:保留已收正文,明确截断
        }
    }
};
using PasteByteAccumulator = BasicPasteAccumulator<char>;
using PasteWideAccumulator = BasicPasteAccumulator<wchar_t>;

// ---------------------------------------------------------------------------
// 4. UTF-8 序列判定(POSIX 逐键解码的纯件)
// ---------------------------------------------------------------------------

struct Utf8LeadInfo {
    char32_t cp = 0;  // 首字节贡献的起始值
    int extra = 0;    // 还差的续字节数;-1 = 非法首字节(孤立续字节/0xF8+)
};
inline Utf8LeadInfo DecodeUtf8Lead(int b0) {
    Utf8LeadInfo out;
    if (b0 >= 0 && b0 < 0x80) {
        out.cp = static_cast<char32_t>(b0);
        out.extra = 0;
    } else if ((b0 & 0xe0) == 0xc0) {
        out.cp = static_cast<char32_t>(b0 & 0x1f);
        out.extra = 1;
    } else if ((b0 & 0xf0) == 0xe0) {
        out.cp = static_cast<char32_t>(b0 & 0x0f);
        out.extra = 2;
    } else if ((b0 & 0xf8) == 0xf0) {
        out.cp = static_cast<char32_t>(b0 & 0x07);
        out.extra = 3;
    } else {
        out.extra = -1;
    }
    return out;
}

inline bool IsUtf8Continuation(int b) { return (b & 0xc0) == 0x80; }

// 收齐后续字节的标量值校验:超长编码、UTF-16 代理项、超范围码点都不算
// 合法 scalar value,按替换符交付(与 Windows 侧孤立代理同口径)。
// lead_extra 是首字节声明的续字节数。
inline bool IsValidUtf8Scalar(char32_t cp, int lead_extra) {
    switch (lead_extra) {
        case 0:
            return cp < 0x80;
        case 1:
            return cp >= 0x80;
        case 2:
            return cp >= 0x800 && !IsLowSurrogate(static_cast<wchar_t>(cp)) &&
                   !IsHighSurrogate(static_cast<wchar_t>(cp));
        case 3:
            return cp >= 0x10000 && cp <= 0x10FFFF;
        default:
            return false;
    }
}

}  // namespace lubancode::platform
