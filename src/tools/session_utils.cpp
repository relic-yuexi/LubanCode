// session_utils.hpp 的实现(自 sessions/session_store.cpp 与
// session_lifecycle.cpp 原样迁来,行为一字未改)。
#include "tools/session_utils.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>
#include <variant>

#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::tools {

namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;
using platform::Utf8ToPath;

std::filesystem::path Utf8Path(const std::string& utf8) {
    const std::u8string_view view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(view);
}

std::size_t Utf8CharLen(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;  // 坏首字节按单字节处理,绝不越界
}

std::string FormatLocalTime(const char* fmt) {
    // 钟读 platform 统一墙钟;本地串格式随迁不动。
    const std::time_t now = platform::WallClockToTimeT(platform::WallClockNowMs());
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), fmt, &tm_buf);
    return std::string(buf);
}

// Markdown 导出用:一段文本取前 n 行,超了标注省略了多少行。
std::string FirstLinesWithNote(const std::string& text, int max_lines) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);
    }
    std::string out;
    const int total = static_cast<int>(lines.size());
    for (int i = 0; i < total && i < max_lines; ++i) {
        out += lines[static_cast<std::size_t>(i)];
        out += "\n";
    }
    if (total > max_lines) {
        out += "...(共 " + std::to_string(total) + " 行,已省略其余)\n";
    }
    return out;
}

}  // namespace

std::string NowTimestamp() { return FormatLocalTime("%Y-%m-%d %H:%M:%S"); }

std::string NowIdTimestamp() { return FormatLocalTime("%Y%m%d-%H%M%S"); }

std::string MakeSessionSlug(const std::string& first_user_text, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    std::size_t pos = 0;
    bool last_dash = true;  // 起手视为刚放过 '-',天然剥掉开头的 '-'
    while (pos < first_user_text.size() && chars < max_chars) {
        const auto lead = static_cast<unsigned char>(first_user_text[pos]);
        std::size_t len = Utf8CharLen(lead);
        if (pos + len > first_user_text.size()) {
            len = 1;  // 尾巴上残缺的多字节序列,按单字节危险字符处理
        }
        ++chars;
        if (len == 1) {
            const char c = first_user_text[pos];
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (safe) {
                out += c;
                last_dash = false;
            } else if (!last_dash) {
                out += '-';
                last_dash = true;
            }
        } else {
            out.append(first_user_text, pos, len);  // 中文等多字节字符原样留
            last_dash = false;
        }
        pos += len;
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) {
        return "untitled";
    }
    return out;
}

std::string MakeSessionId(const std::string& timestamp, const std::string& first_user_text) {
    return timestamp + "-" + MakeSessionSlug(first_user_text);
}

std::string TruncateUtf8Chars(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    std::size_t pos = 0;
    while (pos < text.size() && chars < max_chars) {
        std::size_t len = Utf8CharLen(static_cast<unsigned char>(text[pos]));
        if (pos + len > text.size()) {
            len = 1;
        }
        pos += len;
        ++chars;
    }
    if (pos >= text.size()) {
        return text;
    }
    return text.substr(0, pos) + "…";
}

std::string AbbreviateUtf8Middle(const std::string& text, std::size_t max_chars) {
    if (max_chars < 2) {
        return TruncateUtf8Chars(text, max_chars);
    }
    // 先数码点、记每个码点的字节起点。
    std::vector<std::size_t> starts;
    std::size_t pos = 0;
    while (pos < text.size()) {
        starts.push_back(pos);
        std::size_t len = Utf8CharLen(static_cast<unsigned char>(text[pos]));
        if (pos + len > text.size()) {
            len = 1;
        }
        pos += len;
    }
    if (starts.size() <= max_chars) {
        return text;
    }
    // 头尾各留一半(省略号占一个字位),路径场景开头是盘符、结尾是目录名,
    // 两头都要看得见。
    const std::size_t keep = max_chars - 1;
    const std::size_t head = keep - keep / 2;  // 头多分一个
    const std::size_t tail = keep / 2;
    const std::size_t head_end = starts[head];
    const std::size_t tail_begin = tail == 0 ? text.size() : starts[starts.size() - tail];
    return text.substr(0, head_end) + "…" + text.substr(tail_begin);
}

std::string NormalizePathForCompare(const std::string& utf8_path) {
    if (utf8_path.empty()) {
        return std::string();
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(Utf8Path(utf8_path), ec);
    if (ec || p.empty()) {
        p = Utf8Path(utf8_path).lexically_normal();
    }
    std::string s = PathToUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    // 大小写按 Windows 习惯不敏感(盘符、目录名都是 ASCII 场景居多;多字节
    // 字符的字节不落在 A-Z 区间,这个循环碰不着它们)。
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::optional<std::vector<SessionRefCandidate>> ResolveSessionRef(
    const std::vector<SessionRefCandidate>& candidates, const std::string& ref, bool& ambiguous) {
    ambiguous = false;
    if (ref.empty()) {
        return std::nullopt;
    }
    // 1. 完整 id 相等。
    for (const auto& candidate : candidates) {
        if (candidate.id == ref) {
            return std::vector<SessionRefCandidate>{candidate};
        }
    }
    // 2. id 唯一前缀。
    std::vector<SessionRefCandidate> prefix_hits;
    for (const auto& candidate : candidates) {
        if (candidate.id.rfind(ref, 0) == 0) {
            prefix_hits.push_back(candidate);
        }
    }
    if (prefix_hits.size() == 1) {
        return prefix_hits;
    }
    if (prefix_hits.size() > 1) {
        ambiguous = true;
        return prefix_hits;
    }
    // 3. 标题精确相等。
    std::vector<SessionRefCandidate> title_hits;
    for (const auto& candidate : candidates) {
        if (!candidate.title.empty() && candidate.title == ref) {
            title_hits.push_back(candidate);
        }
    }
    if (title_hits.size() == 1) {
        return title_hits;
    }
    if (title_hits.size() > 1) {
        ambiguous = true;
        return title_hits;
    }
    return std::nullopt;
}

std::string ExportSessionMarkdown(const ExportSessionHeader& header,
                                  const std::vector<api::Message>& messages, const std::string& session_id,
                                  int max_result_lines, const std::string& title,
                                  const std::vector<std::size_t>& compact_positions) {
    // tool_use id -> 结果内容,就近配对(id 冲突在正常存档里不会有,真有也
    // 只是后写的盖前头的,导出展示无伤大雅)。
    struct ResultRef {
        std::string content;
        bool is_error = false;
    };
    std::map<std::string, ResultRef> results;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                results[result->tool_use_id] = ResultRef{result->content, result->is_error};
            }
        }
    }

    std::string out;
    if (!title.empty()) {
        // /title 设过标题:标题当大标题,会话 id 降成一行元信息。
        out += "# " + title + "\n\n";
        out += "- 会话: " + session_id + "\n";
    } else {
        out += "# 会话 " + session_id + "\n\n";
    }
    out += "- 开始时间: " + (header.started_at.empty() ? std::string("(未知)") : header.started_at) + "\n";
    out += "- wire: " + header.wire + "\n";
    out += "- model: " + header.model + "\n";
    out += "- cwd: " + header.cwd + "\n";

    // 压缩标注:compact_positions 升序,next_compact 指着下一个还没插的。
    std::size_t next_compact = 0;
    const auto emit_compact_notes = [&](std::size_t message_index) {
        while (next_compact < compact_positions.size() && compact_positions[next_compact] <= message_index) {
            out += "\n> ⚡ 此处发生过一次上下文压缩\n";
            ++next_compact;
        }
    };

    for (std::size_t mi = 0; mi < messages.size(); ++mi) {
        const auto& message = messages[mi];
        emit_compact_notes(mi);
        // 这条消息里有没有值得单开一节的正文(文本块)?只装着 tool_result
        // 的 user 消息不开"用户"节——结果已折进对应 tool_use 的 details。
        std::string text;
        for (const auto& block : message.content) {
            if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
                if (!text.empty()) {
                    text += "\n";
                }
                text += tb->text;
            } else if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
                if (!text.empty()) {
                    text += "\n";
                }
                text += "[图片] " + image->filename + " (" + std::to_string(image->width) + "x" +
                        std::to_string(image->height) + ")";
            } else if (const auto* generated = std::get_if<api::ModelImageBlock>(&block)) {
                // 模型输出图片:导出里给引用(文件在会话 images/ 目录),
                // 不嵌 base64。
                if (!text.empty()) {
                    text += "\n";
                }
                text += "[模型图片] " + generated->path + " (" + std::to_string(generated->width) + "x" +
                        std::to_string(generated->height) + ", " + std::to_string(generated->bytes) + " 字节)";
            }
        }
        const bool assistant = message.role == api::Role::Assistant;
        bool has_tool_use = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                has_tool_use = true;
                break;
            }
        }
        if (text.empty() && !has_tool_use) {
            continue;
        }
        out += assistant ? "\n## 助手\n\n" : "\n## 用户\n\n";
        if (!text.empty()) {
            out += text;
            out += "\n";
        }
        for (const auto& block : message.content) {
            const auto* use = std::get_if<api::ToolUseBlock>(&block);
            if (use == nullptr) {
                continue;
            }
            out += "\n<details>\n<summary>工具调用: " + use->name + "</summary>\n\n";
            out += "输入:\n\n```json\n" + use->input.dump(2) + "\n```\n";
            const auto found = results.find(use->id);
            if (found != results.end()) {
                out += "\n结果" + std::string(found->second.is_error ? "(出错)" : "") + ":\n\n```\n";
                out += FirstLinesWithNote(found->second.content, max_result_lines);
                out += "```\n";
            } else {
                out += "\n结果: (缺失)\n";
            }
            out += "\n</details>\n";
        }
    }
    // 压缩发生在最后一条消息之后(极少见,但位置合法):标注补在末尾。
    emit_compact_notes(messages.size());
    return out;
}

}  // namespace lubancode::tools
