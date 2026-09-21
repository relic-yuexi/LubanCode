// 无策略 shell 词法的实现(接口与边界见 shell_lexing.hpp 顶注释)。
// 全部纯函数,零 IO;实现自 command_safety 原词法件逐字迁入(AR-07,
// 行为零变更,行为向量见 tests/unit/tools/test_shell_lexing.cpp 与两道
// 闸各自的既有册)。

#include "tools/shell_lexing.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace lubancode::tools {

std::string ToLowerWord(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

namespace {

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

}  // namespace

std::vector<std::string> SplitSegments(const std::string& command, bool single_quotes) {
    std::vector<std::string> segments;
    std::string current;
    char quote = '\0';  // '\0' = 引号外,否则存着当前引号字符
    for (const char c : command) {
        if (quote != '\0') {
            current.push_back(c);
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            current.push_back(c);
            continue;
        }
        if (c == '&' || c == '|' || c == ';' || c == '\n' || c == '\r') {
            // 分隔符:当前段收工。&&/|| 连着的第二个字符走到这儿时 current
            // 是空的,推出去也会在上层被"纯空白段跳过"处理掉。
            segments.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    segments.push_back(current);
    return segments;
}

bool HasUnquotedRedirection(const std::string& segment, bool single_quotes) {
    char quote = '\0';
    for (const char c : segment) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            continue;
        }
        if (c == '>' || c == '<') {
            return true;
        }
    }
    return false;
}

bool HasSubexpression(const std::string& segment, bool single_quotes) {
    bool in_single = false;
    char prev = '\0';
    for (const char c : segment) {
        if (single_quotes && c == '\'') {
            in_single = !in_single;
        } else if (!in_single && prev == '$' && c == '(') {
            return true;
        }
        prev = c;
    }
    return false;
}

bool HasUnquotedScriptBlock(const std::string& segment, bool single_quotes) {
    char quote = '\0';
    for (const char c : segment) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            continue;
        }
        if (c == '{') {
            return true;
        }
    }
    return false;
}

std::vector<std::string> Tokenize(const std::string& segment, bool single_quotes) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    char quote = '\0';
    for (const char c : segment) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
            in_token = true;
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            in_token = true;
            continue;
        }
        if (IsSpace(c)) {
            if (in_token) {
                tokens.push_back(current);
                current.clear();
                in_token = false;
            }
            continue;
        }
        current.push_back(c);
        in_token = true;
    }
    if (in_token) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string NormalizeWord(const std::string& token) {
    std::string word = token;
    if (const std::size_t pos = word.find_last_of("/\\"); pos != std::string::npos) {
        word = word.substr(pos + 1);
    }
    word = ToLowerWord(std::move(word));
    for (const std::string_view ext : {".exe", ".bat", ".cmd", ".com"}) {
        if (word.size() > ext.size() && word.ends_with(ext)) {
            word.resize(word.size() - ext.size());
            break;
        }
    }
    return word;
}

}  // namespace lubancode::tools
