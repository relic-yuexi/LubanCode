#include "platform/text_encoding.hpp"

#include <algorithm>  // std::min(诊断窗口的尾部夹取)

#ifdef _WIN32
#include "platform/paths.hpp"  // AcpBytesToUtf8
#endif

namespace lubancode::platform {

namespace {

// text[i] 起一个 UTF-8 序列的字节长度(1~4);解不出合法序列时是 0。
// IsValidUtf8/ReplaceInvalidWithFFFD 共用这一份判定,别写两遍。
struct DecodedSequence {
    std::size_t length = 0;
};

DecodedSequence DecodeAt(const std::string& text, std::size_t i) {
    const unsigned char first = static_cast<unsigned char>(text[i]);
    if (first <= 0x7f) {
        return {1};
    }

    std::size_t tail_count = 0;
    unsigned int code_point = 0;
    unsigned int minimum = 0;
    if ((first & 0xe0) == 0xc0) {
        tail_count = 1;
        code_point = first & 0x1f;
        minimum = 0x80;
    } else if ((first & 0xf0) == 0xe0) {
        tail_count = 2;
        code_point = first & 0x0f;
        minimum = 0x800;
    } else if ((first & 0xf8) == 0xf0) {
        tail_count = 3;
        code_point = first & 0x07;
        minimum = 0x10000;
    } else {
        return {0};
    }
    if (i + tail_count >= text.size()) {
        return {0};
    }
    for (std::size_t j = 1; j <= tail_count; ++j) {
        const unsigned char tail = static_cast<unsigned char>(text[i + j]);
        if ((tail & 0xc0) != 0x80) {
            return {0};
        }
        code_point = (code_point << 6) | (tail & 0x3f);
    }
    if (code_point < minimum || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
        return {0};
    }
    return {tail_count + 1};
}

// 把非法字节逐段替换成 U+FFFD,合法片段原样保留。调用方保证 text 里确实
// 混着非法字节(先过一遍 IsValidUtf8 判定过),这里不重复短路。
std::string ReplaceInvalidWithFFFD(const std::string& text) {
    static constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD 的 UTF-8 编码

    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const DecodedSequence seq = DecodeAt(text, i);
        if (seq.length == 0) {
            out.append(kReplacement, 3);
            ++i;
            continue;
        }
        out.append(text, i, seq.length);
        i += seq.length;
    }
    return out;
}

}  // namespace

bool IsValidUtf8(const std::string& text) {
    return FirstInvalidUtf8Offset(text) == std::string::npos;
}

std::size_t FirstInvalidUtf8Offset(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const DecodedSequence seq = DecodeAt(text, i);
        if (seq.length == 0) {
            return i;
        }
        i += seq.length;
    }
    return std::string::npos;
}

std::string SanitizeUtf8(const std::string& text) {
    if (IsValidUtf8(text)) {
        return text;
    }

#ifdef _WIN32
    // 十有八九是 PowerShell/cmd 解析期错误、或原生程序绕开控制台编码直接
    // 写出的系统 ANSI 代码页字节,先按 CP_ACP 试着转一遍。
    const std::string acp_converted = AcpBytesToUtf8(text);
    if (IsValidUtf8(acp_converted)) {
        return acp_converted;
    }
#endif

    return ReplaceInvalidWithFFFD(text);
}

namespace {

// 一段文本的成分清点:合法多字节序列几处、非法首字节几个。判定跟
// IsValidUtf8 同一套 DecodeAt,别写第二份解码器。
struct Utf8ScanStats {
    std::size_t valid_multibyte = 0;  // 合法的 2~4 字节序列(中文这类)
    std::size_t invalid_bytes = 0;    // 解不出合法序列的坏字节
};

Utf8ScanStats ScanUtf8Stats(const std::string& text) {
    Utf8ScanStats stats;
    std::size_t i = 0;
    while (i < text.size()) {
        const DecodedSequence seq = DecodeAt(text, i);
        if (seq.length == 0) {
            ++stats.invalid_bytes;
            ++i;
            continue;
        }
        if (seq.length > 1) {
            ++stats.valid_multibyte;
        }
        i += seq.length;
    }
    return stats;
}

}  // namespace

std::string SanitizeExternalText(const std::string& text) {
    if (IsValidUtf8(text)) {
        return text;
    }

#ifdef _WIN32
    // 成分判定:坏字节占大头(或压根没有合法多字节序列)才试 ACP——整段
    // GBK 报错文本那类。零星坏字节混在一堆合法中文里的是混合内容,强转
    // 只会把好的一起转坏,走下面的逐段替换。
    const Utf8ScanStats stats = ScanUtf8Stats(text);
    if (stats.invalid_bytes > stats.valid_multibyte) {
        const std::string acp_converted = AcpBytesToUtf8(text);
        if (IsValidUtf8(acp_converted)) {
            return acp_converted;
        }
    }
#endif

    return ReplaceInvalidWithFFFD(text);
}

std::string DescribeUtf8Issue(const std::string& field, const std::string& text) {
    std::string out = "utf8 field=" + field + " len=" + std::to_string(text.size());
    const std::size_t first_bad = FirstInvalidUtf8Offset(text);
    if (first_bad == std::string::npos) {
        out += " valid=yes";
        return out;
    }
    out += " valid=no first_bad@" + std::to_string(first_bad) + " ctx=";
    const std::size_t begin = first_bad > 6 ? first_bad - 6 : 0;
    const std::size_t end = (std::min)(first_bad + 7, text.size());
    constexpr char kHex[] = "0123456789ABCDEF";
    for (std::size_t i = begin; i < end; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (i != begin) {
            out += ' ';
        }
        out += kHex[byte >> 4];
        out += kHex[byte & 0x0F];
    }
    return out;
}

}  // namespace lubancode::platform
