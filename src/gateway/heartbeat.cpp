#include "gateway/heartbeat.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "platform/paths.hpp"
#include "platform/sha256.hpp"

namespace lubancode::gateway {

namespace {

bool IsAsciiSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimAscii(std::string_view text) {
    while (!text.empty() && IsAsciiSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsAsciiSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// UTF-8 安全截断:不把多字节字符劈成半截。
std::string TruncateUtf8Safe(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t cut = max_bytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return std::string(text.substr(0, cut));
}

// 词法规范化单个 watch 相对路径;非法回空 optional。
bool NormalizeSingleWatchPath(std::string_view raw, std::string* normalized,
                              std::string* reason) {
    const std::string_view trimmed = TrimAscii(raw);
    if (trimmed.empty()) {
        if (reason != nullptr) *reason = "路径不能为空";
        return false;
    }
    if (trimmed.size() > 512) {
        if (reason != nullptr) *reason = "单条路径超过 512 字节上限";
        return false;
    }
    if (trimmed.front() == '/' || trimmed.front() == '\\') {
        if (reason != nullptr) *reason = "只允许工作区内相对路径,禁止前导斜杠";
        return false;
    }
    for (char ch : trimmed) {
        if (ch == '\0' || ch == ':') {
            if (reason != nullptr) *reason = "路径包含非法字符(':' 或空字节)";
            return false;
        }
    }
    std::vector<std::string_view> segments;
    std::size_t pos = 0;
    while (pos < trimmed.size()) {
        std::size_t sep = pos;
        while (sep < trimmed.size() && trimmed[sep] != '/' && trimmed[sep] != '\\') {
            ++sep;
        }
        const std::string_view seg = trimmed.substr(pos, sep - pos);
        if (seg == "..") {
            if (reason != nullptr) *reason = "禁止 '..' 上级目录逃逸";
            return false;
        }
        if (!seg.empty() && seg != ".") {
            segments.push_back(seg);
        }
        pos = sep + 1;
    }
    if (segments.empty()) {
        // 允许显式 "." 代表工作区根目录自身。
        if (trimmed == "." || trimmed == "./" || trimmed == ".\\") {
            if (normalized != nullptr) *normalized = ".";
            return true;
        }
        if (reason != nullptr) *reason = "路径归一化后为空";
        return false;
    }
    if (normalized != nullptr) {
        std::string joined;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (i > 0) joined.push_back('/');
            joined.append(segments[i].data(), segments[i].size());
        }
        *normalized = std::move(joined);
    }
    return true;
}

// 校验 target 解析软链后仍落在 root_canonical 树内。
bool IsContainedInCanonicalRoot(const std::filesystem::path& root_canonical,
                                const std::filesystem::path& target) {
    if (root_canonical.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(target, ec);
    if (ec || resolved.empty()) {
        return false;
    }
    auto root_it = root_canonical.begin();
    auto target_it = resolved.begin();
    for (; root_it != root_canonical.end(); ++root_it, ++target_it) {
        if (target_it == resolved.end() || *root_it != *target_it) {
            return false;
        }
    }
    return true;
}

bool ReadBoundedBytes(const std::filesystem::path& path, std::size_t cap_bytes,
                      std::string* out, bool* truncated = nullptr) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::string buf(cap_bytes + 1, '\0');
    stream.read(buf.data(), static_cast<std::streamsize>(cap_bytes + 1));
    const auto got = static_cast<std::size_t>(stream.gcount());
    if (truncated != nullptr) {
        *truncated = got > cap_bytes;
    }
    buf.resize(std::min(got, cap_bytes));
    *out = std::move(buf);
    return true;
}

// 判断 HEARTBEAT.md 是否含实质巡检项(剔除空白、标题行、HTML 注释块及空复选框壳)。
bool HasActionableChecklistContent(std::string_view content) {
    // 先剥除 <!-- ... --> 块注释。
    std::string stripped;
    stripped.reserve(content.size());
    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t open = content.find("<!--", pos);
        if (open == std::string_view::npos) {
            stripped.append(content.substr(pos));
            break;
        }
        stripped.append(content.substr(pos, open - pos));
        const std::size_t close = content.find("-->", open + 4);
        if (close == std::string_view::npos) {
            break;  // 未闭合注释视为到文件尾
        }
        pos = close + 3;
    }

    std::size_t line_start = 0;
    while (line_start < stripped.size()) {
        std::size_t line_end = stripped.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = stripped.size();
        }
        std::string_view line =
            TrimAscii(std::string_view(stripped).substr(line_start, line_end - line_start));
        line_start = line_end + 1;
        if (line.empty()) continue;
        if (line.front() == '#') continue;  // 纯 Markdown 标题不单算待办
        if (line == "-" || line == "*" || line == "- [ ]" || line == "* [ ]" ||
            line == "- [x]" || line == "- [X]") {
            continue;
        }
        return true;
    }
    return false;
}

bool IsSkippedWatchSubdir(std::string_view rel_posix) {
    return rel_posix == ".git/objects" || rel_posix == "build" ||
           rel_posix == "node_modules" || rel_posix == ".lubancode/worktrees" ||
           rel_posix == ".claude/worktrees";
}

std::string FileDigestToken(const std::filesystem::path& file_path,
                            std::uintmax_t file_size) {
    if (file_size <= kMaxWatchInlineHashBytes) {
        std::string bytes;
        if (ReadBoundedBytes(file_path, kMaxWatchInlineHashBytes, &bytes)) {
            return "sha256:" + platform::Sha256Hex(bytes);
        }
    }
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(file_path, ec);
    const auto ticks = ec ? 0LL
                          : static_cast<long long>(
                                mtime.time_since_epoch().count());
    return "mtime:" + std::to_string(ticks);
}

}  // namespace

bool IsHeartbeatSilentReply(std::string_view reply_text) {
    std::string_view trimmed = TrimAscii(reply_text);
    // 兼容模型偶发套代码块围栏 ```[lang]\nHEARTBEAT_OK\n``` 或反引号 `HEARTBEAT_OK`。
    if (trimmed.size() >= 6 && trimmed.substr(0, 3) == "```" &&
        trimmed.substr(trimmed.size() - 3) == "```") {
        const std::size_t newline = trimmed.find('\n');
        if (newline != std::string_view::npos && newline + 1 <= trimmed.size() - 3) {
            trimmed = TrimAscii(trimmed.substr(newline + 1, trimmed.size() - 3 - (newline + 1)));
        }
    } else if (trimmed.size() >= 2 && trimmed.front() == '`' && trimmed.back() == '`') {
        trimmed = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
    }
    return trimmed == kHeartbeatSilentToken;
}

HeartbeatDecision EvaluateHeartbeatReply(std::string_view reply_text,
                                         std::string_view last_observed_sha) {
    HeartbeatDecision decision;
    if (IsHeartbeatSilentReply(reply_text)) {
        const std::string silent_sha =
            platform::Sha256Hex(std::string(kHeartbeatSilentToken));
        decision.suppress = HeartbeatSuppressReason::SilentToken;
        decision.observed_sha = silent_sha;
        decision.update_last_observed_on_suppress =
            (!last_observed_sha.empty() && last_observed_sha != silent_sha);
        decision.settle_detail = "heartbeat_ok_suppressed";
        return decision;
    }
    const std::string sha = platform::Sha256Hex(std::string(reply_text));
    decision.observed_sha = sha;
    if (!last_observed_sha.empty() && sha == last_observed_sha) {
        decision.suppress = HeartbeatSuppressReason::UnchangedHash;
        decision.update_last_observed_on_suppress = false;
        decision.settle_detail = "unchanged_notification_suppressed";
    }
    return decision;
}

WorkspaceHeartbeatChecklist LoadWorkspaceHeartbeatChecklist(
    const std::filesystem::path& workspace_root) {
    WorkspaceHeartbeatChecklist result;
    if (workspace_root.empty()) {
        return result;
    }
    std::error_code ec;
    const std::filesystem::path root_canonical =
        std::filesystem::weakly_canonical(workspace_root, ec);
    if (ec || root_canonical.empty()) {
        return result;
    }
    const std::pair<const char*, std::filesystem::path> candidates[] = {
        {".lubancode/HEARTBEAT.md",
         workspace_root / platform::Utf8ToPath(".lubancode") /
             platform::Utf8ToPath("HEARTBEAT.md")},
        {"HEARTBEAT.md", workspace_root / platform::Utf8ToPath("HEARTBEAT.md")},
    };
    for (const auto& [rel_name, full_path] : candidates) {
        std::error_code stat_ec;
        const auto status = std::filesystem::symlink_status(full_path, stat_ec);
        if (stat_ec || !std::filesystem::exists(status)) {
            continue;
        }
        if (!IsContainedInCanonicalRoot(root_canonical, full_path)) {
            continue;  // 软链逃出工作区:忽略不读
        }
        if (!std::filesystem::is_regular_file(full_path, stat_ec) || stat_ec) {
            continue;
        }
        std::string raw;
        bool truncated = false;
        if (!ReadBoundedBytes(full_path, kMaxHeartbeatChecklistBytes, &raw, &truncated)) {
            continue;
        }
        result.present = true;
        result.relative_source = rel_name;
        result.content = TruncateUtf8Safe(raw, kMaxHeartbeatChecklistBytes);
        result.has_actionable_items = HasActionableChecklistContent(result.content);
        if (truncated) {
            result.content += "\n...(HEARTBEAT.md 超过 8 KiB 上限,已截断)";
        }
        return result;
    }
    return result;
}

std::string ValidateWatchPaths(const std::vector<std::string>& raw_paths,
                               std::vector<std::string>* normalized_out) {
    if (raw_paths.size() > kMaxHeartbeatWatchPaths) {
        return "automation.watch_invalid: watch_paths 最多允许 " +
               std::to_string(kMaxHeartbeatWatchPaths) + " 项";
    }
    std::vector<std::string> normalized;
    normalized.reserve(raw_paths.size());
    std::set<std::string> seen;
    for (const std::string& item : raw_paths) {
        std::string clean;
        std::string reason;
        if (!NormalizeSingleWatchPath(item, &clean, &reason)) {
            return "automation.watch_invalid: " + reason;
        }
        if (seen.insert(clean).second) {
            normalized.push_back(std::move(clean));
        }
    }
    if (normalized_out != nullptr) {
        *normalized_out = std::move(normalized);
    }
    return std::string();
}

WatchProbeResult ProbeWorkspaceWatch(const std::filesystem::path& workspace_root,
                                     const std::vector<std::string>& watch_paths) {
    WatchProbeResult out;
    std::vector<std::string> clean_paths;
    if (!ValidateWatchPaths(watch_paths, &clean_paths).empty() || clean_paths.empty()) {
        out.fingerprint_sha = platform::Sha256Hex("empty");
        return out;
    }
    std::error_code root_ec;
    const std::filesystem::path root_canonical =
        std::filesystem::weakly_canonical(workspace_root, root_ec);

    std::string manifest;
    std::string summary;
    for (const std::string& rel : clean_paths) {
        const std::filesystem::path target =
            rel == "." ? workspace_root : (workspace_root / platform::Utf8ToPath(rel));
        std::error_code ec;
        const auto sym_status = std::filesystem::symlink_status(target, ec);
        if (ec || !std::filesystem::exists(sym_status)) {
            ++out.missing_count;
            manifest += "missing|" + rel + "\n";
            summary += "- " + rel + " (不存在)\n";
            continue;
        }
        if (root_ec || !IsContainedInCanonicalRoot(root_canonical, target)) {
            ++out.missing_count;
            manifest += "escaped|" + rel + "\n";
            summary += "- " + rel + " (越出工作区边界,已忽略)\n";
            continue;
        }
        if (std::filesystem::is_regular_file(target, ec) && !ec) {
            ++out.existing_count;
            const auto sz = std::filesystem::file_size(target, ec);
            const std::uintmax_t safe_sz = ec ? 0 : sz;
            const std::string digest = FileDigestToken(target, safe_sz);
            manifest += "file|" + rel + "|" + std::to_string(safe_sz) + "|" + digest + "\n";
            summary += "- " + rel + " (文件, " + std::to_string(safe_sz) + "B)\n";
            continue;
        }
        if (std::filesystem::is_directory(target, ec) && !ec) {
            ++out.existing_count;
            std::vector<std::pair<std::string, std::filesystem::path>> entries;
            std::filesystem::recursive_directory_iterator it(
                target, std::filesystem::directory_options::none, ec);
            std::filesystem::recursive_directory_iterator end;
            bool capped = false;
            for (; !ec && it != end; it.increment(ec)) {
                const std::filesystem::path entry_path = it->path();
                std::error_code rel_ec;
                const std::filesystem::path entry_rel_path =
                    std::filesystem::relative(entry_path, workspace_root, rel_ec);
                if (rel_ec) continue;
                std::string entry_rel = platform::PathToUtf8(entry_rel_path);
                std::replace(entry_rel.begin(), entry_rel.end(), '\\', '/');

                std::error_code st_ec;
                const auto entry_sym = it->symlink_status(st_ec);
                if (st_ec) continue;
                if (std::filesystem::is_symlink(entry_sym)) {
                    if (it->is_directory(st_ec)) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (std::filesystem::is_directory(entry_sym)) {
                    if (IsSkippedWatchSubdir(entry_rel)) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (!std::filesystem::is_regular_file(entry_sym)) {
                    continue;
                }
                entries.emplace_back(std::move(entry_rel), entry_path);
                if (entries.size() >= kMaxWatchDirEntries) {
                    capped = true;
                    break;
                }
            }
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            manifest += "dir|" + rel + "|count=" + std::to_string(entries.size()) +
                        "|capped=" + (capped ? "1" : "0") + "\n";
            for (const auto& [sub_rel, sub_path] : entries) {
                std::error_code sz_ec;
                const auto sz = std::filesystem::file_size(sub_path, sz_ec);
                const std::uintmax_t safe_sz = sz_ec ? 0 : sz;
                manifest += "  sub|" + sub_rel + "|" + std::to_string(safe_sz) + "|" +
                            FileDigestToken(sub_path, safe_sz) + "\n";
            }
            summary += "- " + rel + "/ (目录, 采样 " + std::to_string(entries.size()) +
                       " 个文件" + (capped ? ", 达上限截断" : "") + ")\n";
            continue;
        }
        ++out.existing_count;
        manifest += "other|" + rel + "\n";
        summary += "- " + rel + " (特殊文件)\n";
    }
    out.fingerprint_sha = platform::Sha256Hex(manifest);
    if (!summary.empty() && summary.back() == '\n') {
        summary.pop_back();
    }
    out.summary = std::move(summary);
    return out;
}

std::string BuildHeartbeatTurnPrompt(std::string_view base_prompt,
                                     const WorkspaceHeartbeatChecklist& checklist,
                                     const WatchProbeResult* watch_probe) {
    std::string out(base_prompt);
    if (checklist.present && checklist.has_actionable_items && !checklist.content.empty()) {
        out += "\n\n[工作区巡检清单 (" + checklist.relative_source + ")]\n";
        out += checklist.content;
    }
    if (watch_probe != nullptr && !watch_probe->summary.empty()) {
        out += "\n\n[前置探针观测摘要]\n";
        out += watch_probe->summary;
    }
    out += "\n\n[主动巡检回复契约]\n"
           "请依据任务要求及巡检清单核查当前状态(必要时在已获授权的工具范围内检查或处理)。\n"
           "- 若检查后一切正常、无新增风险且无需打扰用户，请只回复 HEARTBEAT_OK"
           "（不要附加任何其他文字或标点）。\n"
           "- 若发现异常、需要关注的进展或待确认事项，请直接输出简明清晰的中文汇报"
           "（正文中不要包含 HEARTBEAT_OK）。";
    return out;
}

}  // namespace lubancode::gateway
