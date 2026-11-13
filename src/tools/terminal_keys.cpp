#include "tools/terminal_keys.hpp"

#include <array>
#include <cctype>
#include <optional>

namespace lubancode::tools {

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 记号形状:纯 [A-Za-z0-9-],或 "C-"/"M-"/"A-" 后跟恰好一个可见字符。
bool LooksLikeKeyToken(std::string_view body) {
    if (body.empty()) {
        return false;
    }
    if (body.size() == 3 && body[1] == '-' && std::string_view("CcMmAa").find(body[0]) != std::string_view::npos &&
        static_cast<unsigned char>(body[2]) > 0x20 && static_cast<unsigned char>(body[2]) < 0x7f) {
        return true;
    }
    for (char c : body) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) != 0 || c == '-')) {
            return false;
        }
    }
    return true;
}

// 方向/编辑键与功能键。app 模式只影响方向键与 Home/End(DECCKM)。
std::optional<std::string> NamedKey(const std::string& name, bool app) {
    const std::string csi = "\x1b[";
    const std::string ss3 = "\x1bO";
    const std::string cursor = app ? ss3 : csi;
    if (name == "enter" || name == "cr" || name == "return") return std::string("\r");
    if (name == "tab") return std::string("\t");
    if (name == "s-tab" || name == "btab") return csi + "Z";
    if (name == "esc" || name == "escape") return std::string("\x1b");
    if (name == "bs" || name == "backspace") return std::string("\x7f");
    if (name == "space") return std::string(" ");
    if (name == "lt") return std::string("<");
    if (name == "gt") return std::string(">");
    if (name == "up") return cursor + "A";
    if (name == "down") return cursor + "B";
    if (name == "right") return cursor + "C";
    if (name == "left") return cursor + "D";
    if (name == "home") return cursor + "H";
    if (name == "end") return cursor + "F";
    if (name == "pageup" || name == "pgup") return csi + "5~";
    if (name == "pagedown" || name == "pgdn") return csi + "6~";
    if (name == "insert" || name == "ins") return csi + "2~";
    if (name == "delete" || name == "del") return csi + "3~";
    // F1-F4 走 SS3,F5 起走 CSI n ~(xterm 编号有跳号:16、22 空缺)。
    static constexpr std::array<int, 8> kFnCodes{15, 17, 18, 19, 20, 21, 23, 24};
    if (name.size() >= 2 && name[0] == 'f') {
        int n = 0;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) {
                return std::nullopt;
            }
            n = n * 10 + (name[i] - '0');
            if (n > 12) {
                return std::nullopt;
            }
        }
        if (n >= 1 && n <= 4) {
            return ss3 + static_cast<char>('P' + (n - 1));
        }
        if (n >= 5 && n <= 12) {
            return csi + std::to_string(kFnCodes[static_cast<std::size_t>(n - 5)]) + "~";
        }
    }
    return std::nullopt;
}

// <C-x>:x 为原始字符(大小写原样传进来,字母不分大小写)。
std::optional<std::string> CtrlKey(char x) {
    const auto u = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(x)));
    if (u >= 'a' && u <= 'z') {
        return std::string(1, static_cast<char>(u - 'a' + 1));
    }
    switch (x) {
        case '@':
        case ' ': return std::string(1, '\0');
        case '[': return std::string("\x1b");
        case '\\': return std::string("\x1c");
        case ']': return std::string("\x1d");
        case '^': return std::string("\x1e");
        case '_': return std::string("\x1f");
        case '?': return std::string("\x7f");
        default: return std::nullopt;
    }
}

}  // namespace

std::expected<std::string, std::string> ParseTerminalKeys(std::string_view text, bool app_cursor_keys) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c != '<') {
            out.push_back(c);
            ++i;
            continue;
        }
        // 找闭合 '>':记号最长不过十几个字符,远处的 '>' 不算。
        std::size_t close = std::string_view::npos;
        for (std::size_t j = i + 1; j < text.size() && j <= i + 16; ++j) {
            if (text[j] == '>' && j > i + 1) {
                close = j;
                break;
            }
        }
        if (close == std::string_view::npos) {
            out.push_back('<');
            ++i;
            continue;
        }
        const std::string_view body = text.substr(i + 1, close - i - 1);
        if (!LooksLikeKeyToken(body)) {
            out.push_back('<');
            ++i;
            continue;
        }
        const std::string token(text.substr(i, close - i + 1));
        std::optional<std::string> bytes;
        if (body.size() == 3 && body[1] == '-') {
            const char mod = static_cast<char>(std::tolower(static_cast<unsigned char>(body[0])));
            if (mod == 'c') {
                bytes = CtrlKey(body[2]);
            } else if (mod == 'm' || mod == 'a') {
                bytes = std::string("\x1b") + body[2];
            }
        }
        if (!bytes.has_value()) {
            // <C-Space> 这类长名修饰键:只认 C-space。
            const std::string name = Lower(body);
            if (name == "c-space") {
                bytes = std::string(1, '\0');
            } else {
                bytes = NamedKey(name, app_cursor_keys);
            }
        }
        if (!bytes.has_value()) {
            return std::unexpected("不认识的按键记号 " + token +
                                   "。可用:<Enter> <Tab> <Esc> <BS> <Space> <Up>/<Down>/<Left>/<Right> "
                                   "<Home>/<End> <PageUp>/<PageDown> <Delete> <F1>..<F12> <C-a>..<C-z> "
                                   "<M-x>;字面 \"<\" 写 <lt>,或用 literal:true 原样发送");
        }
        out += *bytes;
        i = close + 1;
    }
    return out;
}

}  // namespace lubancode::tools
