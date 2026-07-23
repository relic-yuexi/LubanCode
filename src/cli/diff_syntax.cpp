#include "cli/diff_syntax.hpp"

#include <cctype>

namespace lubancode::cli {

namespace {

bool IsIdentStart(char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return std::isalpha(c) != 0 || ch == '_';
}

bool IsIdentContinue(char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return std::isalnum(c) != 0 || ch == '_';
}

bool WordInList(std::string_view word, std::string_view words) {
    std::size_t pos = words.find(word);
    while (pos != std::string_view::npos) {
        const bool left = pos == 0 || words[pos - 1] == ' ';
        const std::size_t end = pos + word.size();
        const bool right = end == words.size() || words[end] == ' ';
        if (left && right) {
            return true;
        }
        pos = words.find(word, pos + 1);
    }
    return false;
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string_view FileName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view Extension(std::string_view filename) {
    const std::size_t dot = filename.find_last_of('.');
    return dot == std::string_view::npos ? std::string_view{} : filename.substr(dot);
}

bool UsesSlashComments(DiffSyntaxLanguage language) {
    return language == DiffSyntaxLanguage::Cpp || language == DiffSyntaxLanguage::JavaScript ||
           language == DiffSyntaxLanguage::Json || language == DiffSyntaxLanguage::Rust ||
           language == DiffSyntaxLanguage::Go;
}

bool UsesHashComments(DiffSyntaxLanguage language) {
    return language == DiffSyntaxLanguage::Python || language == DiffSyntaxLanguage::Shell;
}

bool IsKeyword(std::string_view word, DiffSyntaxLanguage language) {
    switch (language) {
        case DiffSyntaxLanguage::Cpp:
            return WordInList(word,
                              "alignas alignof and asm auto break case catch class concept const "
                              "consteval constexpr constinit const_cast continue co_await co_return "
                              "co_yield decltype default delete do dynamic_cast else enum explicit "
                              "export extern for friend goto if inline mutable namespace new noexcept "
                              "not operator or override private protected public register reinterpret_cast "
                              "requires return sizeof static static_assert static_cast struct switch "
                              "template this thread_local throw try typedef typeid typename union using "
                              "virtual volatile while xor");
        case DiffSyntaxLanguage::Python:
            return WordInList(word,
                              "and as assert async await break case class continue def del elif else "
                              "except finally for from global if import in is lambda match nonlocal not "
                              "or pass raise return try while with yield");
        case DiffSyntaxLanguage::JavaScript:
            return WordInList(word,
                              "as async await break case catch class const continue debugger default "
                              "delete do else export extends finally for from function get if implements "
                              "import in instanceof interface let new of package private protected public "
                              "return set static super switch throw try typeof var void while with yield");
        case DiffSyntaxLanguage::Json:
            return WordInList(word, "true false null");
        case DiffSyntaxLanguage::Shell:
            return WordInList(word,
                              "case coproc do done elif else esac fi for function if in select then time "
                              "until while export local readonly declare typeset return source");
        case DiffSyntaxLanguage::Rust:
            return WordInList(word,
                              "as async await break const continue crate dyn else enum extern false fn for "
                              "if impl in let loop match mod move mut pub ref return self Self static struct "
                              "super trait true type union unsafe use where while");
        case DiffSyntaxLanguage::Go:
            return WordInList(word,
                              "break case chan const continue default defer else fallthrough for func go "
                              "goto if import interface map package range return select struct switch type var");
        case DiffSyntaxLanguage::Unknown:
            return false;
    }
    return false;
}

bool IsType(std::string_view word, DiffSyntaxLanguage language) {
    switch (language) {
        case DiffSyntaxLanguage::Cpp:
            return WordInList(word,
                              "bool char char8_t char16_t char32_t double float int long short signed "
                              "unsigned void wchar_t size_t ssize_t string string_view vector map set "
                              "unordered_map unordered_set optional variant tuple pair auto std");
        case DiffSyntaxLanguage::Python:
            return WordInList(word,
                              "bool bytearray bytes complex dict float frozenset int list memoryview object "
                              "range set str tuple type None True False");
        case DiffSyntaxLanguage::JavaScript:
            return WordInList(word,
                              "Array ArrayBuffer BigInt Boolean Date Error Function Map Number Object "
                              "Promise RegExp Set String Symbol WeakMap WeakSet any bigint boolean never "
                              "number object string symbol unknown");
        case DiffSyntaxLanguage::Json:
        case DiffSyntaxLanguage::Shell:
            return false;
        case DiffSyntaxLanguage::Rust:
            return WordInList(word,
                              "bool char f32 f64 i8 i16 i32 i64 i128 isize str String u8 u16 u32 u64 u128 "
                              "usize Vec Option Result Box HashMap HashSet");
        case DiffSyntaxLanguage::Go:
            return WordInList(word,
                              "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 "
                              "int64 rune string uint uint8 uint16 uint32 uint64 uintptr any comparable");
        case DiffSyntaxLanguage::Unknown:
            return false;
    }
    return false;
}

const std::string& TokenColor(std::string_view word, bool function, DiffSyntaxLanguage language,
                              const Theme& theme) {
    if (IsKeyword(word, language)) {
        return theme.diff_syntax_keyword;
    }
    if (IsType(word, language)) {
        return theme.diff_syntax_type;
    }
    if (function) {
        return theme.diff_syntax_function;
    }
    return theme.diff_syntax_plain;
}

void AppendColored(std::string& out, const std::string& color, std::string_view token,
                   const Theme& theme) {
    if (color.empty() || color == theme.diff_syntax_plain) {
        out.append(token);
        return;
    }
    out += color;
    out.append(token);
    out += theme.diff_syntax_plain;
}

}  // namespace

DiffSyntaxLanguage DetectDiffSyntaxLanguage(std::string_view file_path) {
    const std::string filename = LowerAscii(FileName(file_path));
    const std::string ext = LowerAscii(Extension(filename));
    if (filename == "dockerfile" || filename == "makefile" || ext == ".sh" || ext == ".bash" ||
        ext == ".zsh" || ext == ".fish" || ext == ".ps1") {
        return DiffSyntaxLanguage::Shell;
    }
    if (ext == ".c" || ext == ".h" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
        ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".m" || ext == ".mm" ||
        ext == ".cs" || ext == ".java" || ext == ".kt" || ext == ".kts" || ext == ".swift") {
        return DiffSyntaxLanguage::Cpp;
    }
    if (ext == ".py" || ext == ".pyw") {
        return DiffSyntaxLanguage::Python;
    }
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs" || ext == ".ts" ||
        ext == ".tsx") {
        return DiffSyntaxLanguage::JavaScript;
    }
    if (ext == ".json" || ext == ".jsonc") {
        return DiffSyntaxLanguage::Json;
    }
    if (ext == ".rs") {
        return DiffSyntaxLanguage::Rust;
    }
    if (ext == ".go") {
        return DiffSyntaxLanguage::Go;
    }
    return DiffSyntaxLanguage::Unknown;
}

std::string HighlightDiffCodeLine(std::string_view code, DiffSyntaxLanguage language,
                                  const Theme& theme) {
    if (language == DiffSyntaxLanguage::Unknown || theme.diff_syntax_plain.empty()) {
        return std::string(code);
    }

    std::string out;
    out.reserve(code.size() + 32);
    std::size_t i = 0;
    while (i < code.size()) {
        if (UsesSlashComments(language) && i + 1 < code.size() && code[i] == '/' &&
            (code[i + 1] == '/' || code[i + 1] == '*')) {
            const bool block = code[i + 1] == '*';
            std::size_t end = code.size();
            if (block) {
                const std::size_t close = code.find("*/", i + 2);
                if (close != std::string_view::npos) {
                    end = close + 2;
                }
            }
            AppendColored(out, theme.diff_syntax_comment, code.substr(i, end - i), theme);
            i = end;
            continue;
        }
        if (UsesHashComments(language) && code[i] == '#') {
            AppendColored(out, theme.diff_syntax_comment, code.substr(i), theme);
            break;
        }

        const bool template_string = language == DiffSyntaxLanguage::JavaScript && code[i] == '`';
        if (code[i] == '\'' || code[i] == '"' || template_string) {
            const char quote = code[i];
            std::size_t end = i + 1;
            bool escaped = false;
            while (end < code.size()) {
                const char ch = code[end++];
                if (ch == quote && !escaped) {
                    break;
                }
                if (ch == '\\' && !escaped) {
                    escaped = true;
                } else {
                    escaped = false;
                }
            }
            std::size_t after = end;
            while (after < code.size() && (code[after] == ' ' || code[after] == '\t')) {
                ++after;
            }
            const bool json_key = language == DiffSyntaxLanguage::Json && after < code.size() &&
                                  code[after] == ':';
            AppendColored(out, json_key ? theme.diff_syntax_function : theme.diff_syntax_string,
                          code.substr(i, end - i), theme);
            i = end;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(code[i])) != 0) {
            std::size_t end = i + 1;
            while (end < code.size()) {
                const char ch = code[end];
                if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '.' && ch != '_' &&
                    ch != '+' && ch != '-') {
                    break;
                }
                ++end;
            }
            AppendColored(out, theme.diff_syntax_number, code.substr(i, end - i), theme);
            i = end;
            continue;
        }

        if (IsIdentStart(code[i])) {
            std::size_t end = i + 1;
            while (end < code.size() && IsIdentContinue(code[end])) {
                ++end;
            }
            std::size_t next = end;
            while (next < code.size() && (code[next] == ' ' || code[next] == '\t')) {
                ++next;
            }
            const bool function = next < code.size() && code[next] == '(';
            const std::string_view word = code.substr(i, end - i);
            AppendColored(out, TokenColor(word, function, language, theme), word, theme);
            i = end;
            continue;
        }

        out.push_back(code[i]);
        ++i;
    }
    return out;
}

}  // namespace lubancode::cli
