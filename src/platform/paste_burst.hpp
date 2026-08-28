// 一批同时到达的输入字符是不是"一次粘贴事务"——纯逻辑,不碰控制台。
// Windows 键读层(console_win.cpp)在逐键交付前先问这一句;两平台单测
// 都能直接跑(INPUT_RECORD 进不了 POSIX 编译单元,判别本身不认平台)。
//
// 两桩病各有一份账:
//   1. ConPTY/VS Code 把一次粘贴拆成一批 KEY_EVENT,单行内容既没有
//      bracketed paste 标记、也没有换行,TryReadNativePasteBurst 的
//      "换行后还有正文"形状认不出,只能逐字当打字交付——每个字一轮
//      终端帧,一万字中文就是一万轮(P2-4 的输入重绘风暴)。
//   2. 终端把 bracketed paste 标记 ESC[200~/ESC[201~ 也拆成逐字符事件
//      送进输入队列时,标记不再是"看不见的协议",会漏成正文
//      ("[200~"直接打进编辑器)。
// 判别规则:同批字符攒过阈值(带标记的整批不论长短),就折成一枚
// Paste——一次编辑事务、一轮帧。输入法整词提交通常几个字,不越线,
// 照旧逐键交付。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace lubancode::platform {

inline constexpr std::wstring_view kBracketedPasteBegin = L"\x1b[200~";
inline constexpr std::wstring_view kBracketedPasteEnd = L"\x1b[201~";

// 同批字符攒到多少按粘贴收。输入法整句提交一般不超过一二十字;粘贴
// 动辄几百上千。取 16:宁可把一小段快打连击并成一次事务(视觉与逐键
// 相同,还省帧),不叫大粘贴漏网。
inline constexpr std::size_t kPasteBurstThreshold = 16;

struct TextBurstDecision {
    bool is_paste = false;
    bool had_markers = false;
    std::wstring text;  // 剥掉 bracketed 标记后的正文(换行归一之前)
};

// burst 是"这一批同时到达的 UTF-16 字符"(含终端拆散的 ESC 标记字符)。
// 返回剥标记后的正文与是否按粘贴收。尾标记没到(批次从中间切开)时,
// 正文先收下——line editor 的 paste_run 会把紧邻的后继 Paste 并进同一
// 枚附件,内容不丢。
inline TextBurstDecision ClassifyTextBurst(std::wstring burst) {
    TextBurstDecision out;
    const std::size_t begin = burst.find(kBracketedPasteBegin);
    if (begin != std::wstring::npos) {
        const std::size_t after = begin + kBracketedPasteBegin.size();
        const std::size_t end = burst.find(kBracketedPasteEnd, after);
        std::wstring stripped = burst.substr(0, begin);
        if (end != std::wstring::npos) {
            stripped += burst.substr(after, end - after);
            stripped += burst.substr(end + kBracketedPasteEnd.size());
        } else {
            stripped += burst.substr(after);
        }
        out.had_markers = true;
        burst = std::move(stripped);
    }
    out.text = std::move(burst);
    out.is_paste = out.had_markers || out.text.size() >= kPasteBurstThreshold;
    return out;
}

}  // namespace lubancode::platform
