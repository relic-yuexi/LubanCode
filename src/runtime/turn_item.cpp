// TurnItem/DiffTable 的实现(显示系统剥离单第五步)。
//
// diff 计算从 cli::ToolDisplay 的 BuildFileDiffPreview 里拆出来:那边留
// "排版成终端预览块"(ANSI、宽度、缩进、截断),这边只算行级 LCS 与事实
// 摘要。算法逐句搬自 cli/diff.cpp 的 ComputeLineDiff/BuildEditDiff/
// BuildWriteDiff(语义一个字不改,单测两边同钉);cli 侧 FormatDiff 依旧
// 吃自己那份 DiffLine,本文件不反过来 include cli/*。

#include "runtime/turn_item.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace lubancode::runtime {

namespace {

// 中段 DP 规模上限,与 cli/diff.cpp 的 kDpCellCap 同值同义:超限退化成
// "旧的整删 + 新的整增",不硬算、不爆内存。
constexpr std::size_t kDpCellCap = 4'000'000;

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();  // CRLF 磁盘文件跟模型给的 LF 对得上
        }
        lines.push_back(std::move(line));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return lines;
}

// 行级 LCS(公共前后缀 + 中段朴素 DP),逐句同 cli::ComputeLineDiff。
std::vector<DiffRow> ComputeLineDiff(const std::vector<std::string>& old_lines,
                                     const std::vector<std::string>& new_lines) {
    const std::size_t na = old_lines.size();
    const std::size_t nb = new_lines.size();
    std::vector<DiffRow> out;
    out.reserve(na + nb);

    std::size_t pre = 0;
    while (pre < na && pre < nb && old_lines[pre] == new_lines[pre]) {
        out.push_back(DiffRow{DiffRowKind::Context, old_lines[pre], static_cast<int>(pre) + 1,
                              static_cast<int>(pre) + 1});
        ++pre;
    }
    std::size_t suf = 0;
    while (suf < na - pre && suf < nb - pre && old_lines[na - 1 - suf] == new_lines[nb - 1 - suf]) {
        ++suf;
    }
    const std::size_t ma = na - pre - suf;
    const std::size_t mb = nb - pre - suf;

    if (ma > 0 && mb > 0 && ma * mb <= kDpCellCap) {
        // 朴素 LCS DP:dp[i][j] = old[pre+i..) 与 new[pre+j..) 的 LCS 长度。
        const std::size_t cols = mb + 1;
        std::vector<int> dp((ma + 1) * (mb + 1), 0);
        for (std::size_t i = ma; i-- > 0;) {
            for (std::size_t j = mb; j-- > 0;) {
                if (old_lines[pre + i] == new_lines[pre + j]) {
                    dp[i * cols + j] = dp[(i + 1) * cols + (j + 1)] + 1;
                } else {
                    dp[i * cols + j] =
                        dp[(i + 1) * cols + j] > dp[i * cols + (j + 1)] ? dp[(i + 1) * cols + j]
                                                                         : dp[i * cols + (j + 1)];
                }
            }
        }
        // 回溯:相等走 Context;否则删优先(>=),让每个变更块先 - 后 +。
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < ma && j < mb) {
            if (old_lines[pre + i] == new_lines[pre + j]) {
                out.push_back(DiffRow{DiffRowKind::Context, old_lines[pre + i],
                                      static_cast<int>(pre + i) + 1, static_cast<int>(pre + j) + 1});
                ++i;
                ++j;
            } else if (dp[(i + 1) * cols + j] >= dp[i * cols + (j + 1)]) {
                out.push_back(DiffRow{DiffRowKind::Del, old_lines[pre + i], static_cast<int>(pre + i) + 1, 0});
                ++i;
            } else {
                out.push_back(DiffRow{DiffRowKind::Add, new_lines[pre + j], 0, static_cast<int>(pre + j) + 1});
                ++j;
            }
        }
        for (; i < ma; ++i) {
            out.push_back(DiffRow{DiffRowKind::Del, old_lines[pre + i], static_cast<int>(pre + i) + 1, 0});
        }
        for (; j < mb; ++j) {
            out.push_back(DiffRow{DiffRowKind::Add, new_lines[pre + j], 0, static_cast<int>(pre + j) + 1});
        }
    } else {
        // 一边是空(纯增/纯删),或规模超限——整删整增。
        for (std::size_t i = 0; i < ma; ++i) {
            out.push_back(DiffRow{DiffRowKind::Del, old_lines[pre + i], static_cast<int>(pre + i) + 1, 0});
        }
        for (std::size_t j = 0; j < mb; ++j) {
            out.push_back(DiffRow{DiffRowKind::Add, new_lines[pre + j], 0, static_cast<int>(pre + j) + 1});
        }
    }

    // 公共后缀。
    for (std::size_t k = 0; k < suf; ++k) {
        out.push_back(DiffRow{DiffRowKind::Context, old_lines[na - suf + k],
                              static_cast<int>(na - suf + k) + 1, static_cast<int>(nb - suf + k) + 1});
    }
    return out;
}

// 读旧文件(二进制读入,当 UTF-8 字节串用)。读不到(不存在/是目录/打不开)
// 给 nullopt——write_file 按新文件处理,edit_file 走段内回退,不因此崩。
std::optional<std::string> ReadFileBytes(const std::string& path_utf8) {
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string ReplaceOccurrences(const std::string& text, const std::string& old_s, const std::string& new_s,
                               bool all) {
    std::string result;
    result.reserve(text.size());
    std::size_t pos = 0;
    bool replaced_once = false;
    while (pos < text.size()) {
        if (!replaced_once || all) {
            const std::size_t found = text.find(old_s, pos);
            if (found != std::string::npos) {
                result.append(text, pos, found - pos);
                result.append(new_s);
                pos = found + old_s.size();
                replaced_once = true;
                continue;
            }
        }
        result.append(text, pos, text.size() - pos);
        break;
    }
    return result;
}

}  // namespace

std::uint64_t DiffTable::added_lines() const {
    std::uint64_t count = 0;
    for (const auto& row : rows) {
        if (row.kind == DiffRowKind::Add) {
            ++count;
        }
    }
    return count;
}

std::uint64_t DiffTable::removed_lines() const {
    std::uint64_t count = 0;
    for (const auto& row : rows) {
        if (row.kind == DiffRowKind::Del) {
            ++count;
        }
    }
    return count;
}

std::optional<DiffTable> BuildDiffTable(const std::string& tool_name, const nlohmann::json& input) {
    if (tool_name != "write_file" && tool_name != "edit_file") {
        return std::nullopt;
    }
    DiffTable table;
    table.path = input.value("path", std::string());
    const std::optional<std::string> old_content = ReadFileBytes(table.path);

    if (tool_name == "edit_file") {
        const std::string old_string = input.value("old_string", std::string());
        const std::string new_string = input.value("new_string", std::string());
        const bool replace_all = input.value("replace_all", false);
        const std::size_t occurrences = CountOccurrences(old_content.value_or(std::string()), old_string);
        if (occurrences == 0) {
            // 定位失败(文件读不出来 / old_string 不在里头):只比新旧两段,
            // 行号是段内行号——真执行时工具自己会报错。
            table.located = false;
            table.replaced_count = 0;
            table.rows = ComputeLineDiff(SplitLines(old_string), SplitLines(new_string));
        } else {
            table.located = true;
            table.replaced_count = replace_all ? occurrences : 1;
            const std::string updated =
                ReplaceOccurrences(*old_content, old_string, new_string, replace_all);
            table.rows = ComputeLineDiff(SplitLines(*old_content), SplitLines(updated));
        }
    } else {
        const std::string content = input.value("content", std::string());
        table.old_exists = old_content.has_value();
        table.rows = ComputeLineDiff(SplitLines(old_content.value_or(std::string())), SplitLines(content));
        if (!old_content.has_value()) {
            // 新文件全算新增——ComputeLineDiff 的公共前缀在空旧文上自然全
            // Add,无需特判;真跑一遍保平:
            table.rows.clear();
            const auto lines = SplitLines(content);
            table.rows.reserve(lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i) {
                table.rows.push_back(DiffRow{DiffRowKind::Add, lines[i], 0, static_cast<int>(i) + 1});
            }
        }
    }
    return table;
}

std::string TruncateUtf8Bytes(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;  // 别劈开多字节字符
    }
    return text.substr(0, cut);
}

std::string ToString(TurnItemStatus status) {
    switch (status) {
        case TurnItemStatus::Pending: return "pending";
        case TurnItemStatus::Running: return "running";
        case TurnItemStatus::Succeeded: return "succeeded";
        case TurnItemStatus::Failed: return "failed";
        case TurnItemStatus::Declined: return "declined";
        case TurnItemStatus::Cancelled: return "cancelled";
    }
    return "running";
}

std::string ToString(TurnItemKind kind) {
    switch (kind) {
        case TurnItemKind::Tool: return "tool";
        case TurnItemKind::SubTool: return "subtool";
        case TurnItemKind::Thinking: return "thinking";
        case TurnItemKind::Text: return "text";
        case TurnItemKind::Command: return "command";
        case TurnItemKind::Diff: return "diff";
        case TurnItemKind::Todo: return "todo";
        case TurnItemKind::Subagent: return "subagent";
    }
    return "tool";
}

bool ParseTurnItemStatus(const std::string& s, TurnItemStatus& out) {
    if (s == "pending") { out = TurnItemStatus::Pending; return true; }
    if (s == "running") { out = TurnItemStatus::Running; return true; }
    if (s == "succeeded") { out = TurnItemStatus::Succeeded; return true; }
    if (s == "failed") { out = TurnItemStatus::Failed; return true; }
    if (s == "declined") { out = TurnItemStatus::Declined; return true; }
    if (s == "cancelled") { out = TurnItemStatus::Cancelled; return true; }
    return false;
}

bool ParseTurnItemKind(const std::string& s, TurnItemKind& out) {
    if (s == "tool") { out = TurnItemKind::Tool; return true; }
    if (s == "subtool") { out = TurnItemKind::SubTool; return true; }
    if (s == "thinking") { out = TurnItemKind::Thinking; return true; }
    if (s == "text") { out = TurnItemKind::Text; return true; }
    if (s == "command") { out = TurnItemKind::Command; return true; }
    if (s == "diff") { out = TurnItemKind::Diff; return true; }
    if (s == "todo") { out = TurnItemKind::Todo; return true; }
    if (s == "subagent") { out = TurnItemKind::Subagent; return true; }
    return false;
}

}  // namespace lubancode::runtime
