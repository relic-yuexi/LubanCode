// @ 提及支件的实现(会话终章自大类搬出,行为一字未改——注释一并随行)。
#include "app/mention_support.hpp"

#include <algorithm>
#include <set>

#include "cli/i18n.hpp"
#include "cli/image_input.hpp"  // MediaTypeForPath(图片提及走视觉附件路)
#include "runtime/worktree.hpp"  // FindRepositoryRoot
#include "tools/path_utils.hpp"
#include "tools/session_utils.hpp"  // NormalizePathForCompare(P0-6 自 sessions 迁来)

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

std::vector<lubancode::cli::FileMentionEntry> MentionSupport::Snapshot() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const auto root = lubancode::cli::FindRepositoryRoot(cwd);
    const std::filesystem::path base = root.value_or(cwd);
    const std::string root_key = lubancode::tools::PathToUtf8(base);
    if (root_key == mention_index_root_ && !mention_index_.empty()) {
        return mention_index_;
    }
    mention_index_root_ = root_key;
    mention_index_.clear();
    static const std::set<std::string> kExcluded = {
        ".git", "build", "out", "dist", "node_modules", "target", "_deps", "_build",
        ".lubancode", ".cache", "__pycache__", ".venv", "venv", "cmake-build-debug"};
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(base, ec), end;
    while (it != end && mention_index_.size() < 3000) {
        const std::filesystem::path current = it->path();
        const std::string name = lubancode::tools::PathToUtf8(current.filename());
        if (it->is_symlink(ec)) {
            it.disable_recursion_pending();
            ++it;
            continue;  // 符号链接不进清单也不下钻
        }
        const bool is_dir = it->is_directory(ec);
        if (is_dir && (kExcluded.contains(name) || (!name.empty() && name.front() == '.'))) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }
        if (it.depth() > 6) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }
        if (current != base) {
            std::string rel = lubancode::tools::PathToUtf8(current.lexically_relative(base));
            for (char& c : rel) {
                if (c == '\\') {
                    c = '/';
                }
            }
            mention_index_.push_back(lubancode::cli::FileMentionEntry{rel, is_dir});
        }
        ++it;
    }
    // 目录排前、路径短排前——@src/cli 选目录一击即中。
    std::sort(mention_index_.begin(), mention_index_.end(),
              [](const auto& a, const auto& b) {
                  if (a.is_dir != b.is_dir) {
                      return a.is_dir;
                  }
                  return a.relative_path < b.relative_path;
              });
    return mention_index_;
}

void MentionSupport::Invalidate() {
    mention_index_root_.clear();
    mention_index_.clear();
}

std::pair<std::string, std::string> MentionSupport::BuildLedger(const std::string& content) {
    const std::vector<std::string> tokens = lubancode::cli::ExtractTextMentions(content);
    if (tokens.empty()) {
        return {};
    }
    const std::filesystem::path cwd = std::filesystem::current_path();
    const auto root = lubancode::cli::FindRepositoryRoot(cwd);
    const std::filesystem::path base = root.value_or(cwd);
    const std::string base_key = lubancode::tools::NormalizePathForCompare(lubancode::tools::PathToUtf8(base));
    std::string ledger;
    for (const std::string& token : tokens) {
        if (lubancode::cli::MediaTypeForPath(token).has_value()) {
            continue;  // 图片:视觉附件路自己管
        }
        // 相对根解析;根内没有再按 cwd 相对试一次(临时文件那类提及)。
        std::filesystem::path resolved;
        bool found = false;
        for (const std::filesystem::path& candidate : {base / lubancode::tools::Utf8ToPath(token),
                                                       cwd / lubancode::tools::Utf8ToPath(token)}) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                resolved = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            return {trf("mention.missing", token), {}};
        }
        // 项目根校验:解析后的绝对路径必须仍在根内(或等于根),不许 @..
        // 越狱到园子外。
        std::error_code ec;
        const std::filesystem::path canon = std::filesystem::weakly_canonical(resolved, ec);
        const std::string canon_key =
            lubancode::tools::NormalizePathForCompare(lubancode::tools::PathToUtf8(canon));
        if (!canon_key.empty() && canon_key.rfind(base_key + "/", 0) != 0 && canon_key != base_key) {
            return {trf("mention.outside_root", token), {}};
        }
        const bool is_dir = std::filesystem::is_directory(canon, ec);
        ledger += "\n- " + token + " -> " + lubancode::tools::PathToUtf8(canon) +
                  (is_dir ? "(目录)" : "(文件)");
    }
    if (ledger.empty()) {
        return {};
    }
    return {{}, tr("mention.ledger_header") + ledger + "\n"};
}

}  // namespace lubancode::app
