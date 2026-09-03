#include "runtime/worktree.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#include "platform/paths.hpp"    // PathComparisonKey:比较键公共件(审计 P2 候选收编)
#include "platform/process.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lubancode::cli {

namespace {

std::string Trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path Utf8ToPath(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

}  // namespace

// 归一化比较键(小写、正斜杠、去尾斜杠):验明正身要拿用户给的路径跟
// git 登记的路径比对,Windows 下大小写与斜杠都不能较真。合同与实现统一
// 在 platform::PathComparisonKey(src 收口审计 P2 候选收编),这里只留
// 领域薄名,房务内部与向量测试在用。
std::string NormalizeKey(const std::filesystem::path& path) {
    return platform::PathComparisonKey(path);
}

namespace {

bool KeyIsUnder(const std::string& key, const std::string& root) {
    return key.size() > root.size() && key.compare(0, root.size(), root) == 0 && key[root.size()] == '/';
}

std::vector<WorktreeEntry> ParsePorcelainList(const std::string& output) {
    std::vector<WorktreeEntry> entries;
    std::istringstream input(output);
    std::string line;
    WorktreeEntry* current = nullptr;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        constexpr std::string_view kWorktree = "worktree ";
        constexpr std::string_view kBranch = "branch refs/heads/";
        if (line.starts_with(kWorktree)) {
            entries.push_back({Utf8ToPath(line.substr(kWorktree.size())), std::string(), false, false});
            current = &entries.back();
        } else if (current != nullptr && line.starts_with(kBranch)) {
            current->branch = line.substr(kBranch.size());
        } else if (current != nullptr && line == "detached") {
            current->detached = true;
        } else if (current != nullptr && line.starts_with("locked")) {
            current->locked = true;
        }
    }
    return entries;
}

// 读一个小文本文件(.git 文件、gitdir 登记文件这类几十字节的),UTF-8/ASCII
// 字节直读;读不成给 nullopt。
std::optional<std::string> ReadSmallFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

// 这一项是不是 reparse point(符号链接/NTFS junction):链接只删自身,
// 绝不追进指向的目录。MSVC STL 的 is_symlink 认不全 junction,Windows 下
// 直接看 FILE_ATTRIBUTE_REPARSE_POINT 属性才算数。
bool IsLinkEntry(const std::filesystem::path& current) {
#ifdef _WIN32
    const std::wstring wide = current.wstring();
    const DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(current, ec));
#endif
}


}  // namespace

// 真跑 git 的默认 runner:生产路径各口共用这一份(字段拼装/超时/失败折形
// 只养一处)。原先在匿名区,agent 派工链的查单口也要用,导出。
GitCommandResult DefaultGitRunner(const GitCommand& command) {
    std::vector<std::string> argv = {"git", "-C", PathToUtf8(command.working_directory)};
    argv.insert(argv.end(), command.args.begin(), command.args.end());
    const int timeout = command.timeout_ms > 0 ? command.timeout_ms : 120000;
    const platform::ProcessResult process = platform::RunProcess(argv, timeout);
    if (process.spawn_failed) {
        return {1, process.output, process.spawn_error};
    }
    if (process.timed_out) {
        return {1, process.output, "git timed out"};
    }
    if (process.output_truncated) {
        return {1, process.output, "git output was truncated"};
    }
    return {static_cast<int>(process.exit_code), process.output, std::string()};
}

ParsedWorktreeCommand ParseWorktreeCommand(std::string_view args) {
    ParsedWorktreeCommand command;
    const std::string text = Trim(args);
    const std::size_t split = text.find_first_of(" \t");
    const std::string action = text.substr(0, split);
    const std::string rest = split == std::string::npos ? std::string() : Trim(std::string_view(text).substr(split + 1));

    if (action == "new") {
        command.action = WorktreeAction::New;
        command.name = rest;
    } else if (action == "list" && rest.empty()) {
        command.action = WorktreeAction::List;
    } else if (action == "exit") {
        command.action = WorktreeAction::Exit;
        command.exit_mode = rest;
    }
    return command;
}

std::optional<std::string> ValidateWorktreeName(std::string_view name) {
    if (name.empty()) {
        return std::string("name is empty");
    }
    if (name.size() > 64) {
        return std::string("name is longer than 64 characters");
    }
    if (name.rfind("agent-", 0) == 0) {
        return std::string("'agent-' prefix is reserved for isolated subagent worktrees");
    }
    for (const char ch : name) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_';
        if (!allowed) {
            return std::string("name may contain only letters, digits, '-' and '_'");
        }
    }
    return std::nullopt;
}

std::string GenerateWorktreeName() {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device seed;
    std::mt19937_64 generator((static_cast<std::uint64_t>(seed()) << 32U) ^ seed());
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(kAlphabet) - 2);

    std::string name = "worktree-";
    for (int i = 0; i < 10; ++i) {
        name += kAlphabet[pick(generator)];
    }
    return name;
}

std::string CurrentGitBranch(const std::filesystem::path& working_directory, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    const GitCommandResult branch =
        runner({working_directory, {"symbolic-ref", "--quiet", "--short", "HEAD"}});
    if (branch.exit_code == 0) {
        return Trim(branch.output);
    }
    const GitCommandResult detached =
        runner({working_directory, {"rev-parse", "--short", "HEAD"}});
    if (detached.exit_code == 0) {
        const std::string hash = Trim(detached.output);
        if (!hash.empty()) {
            return "detached@" + hash;
        }
    }
    return {};
}

std::filesystem::path WorktreePath(const std::filesystem::path& repository_root, std::string_view name) {
    return repository_root / ".lubancode" / "worktrees" / std::string(name);
}

// ---------------------------------------------------------------------------
// 房务自由函数
// ---------------------------------------------------------------------------

bool WorktreeClean(const std::filesystem::path& worktree_path, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    const GitCommandResult status = runner({worktree_path, {"status", "--porcelain"}});
    return status.exit_code == 0 && Trim(status.output).empty();
}

std::optional<std::string> VerifyWorktreeIdentity(const std::filesystem::path& path,
                                                  const std::filesystem::path& main_root) {
    const std::string path_key = NormalizeKey(path);
    const std::string root_key = NormalizeKey(main_root);
    if (root_key.empty()) {
        return std::string("主 checkout 路径无效");
    }
    if (path_key == root_key) {
        return "路径就是主 checkout 本身:" + PathToUtf8(path);
    }
    std::error_code ec;
    const std::filesystem::path dot_git = path / ".git";
    if (std::filesystem::is_directory(dot_git, ec)) {
        return ".git 是目录(主仓本体或嵌套完整仓),不是独立 worktree";
    }
    const auto git_file = ReadSmallFile(dot_git);
    if (!git_file.has_value()) {
        return "没有可读的 .git 文件,不是独立 worktree";
    }
    constexpr std::string_view kGitdir = "gitdir:";
    const std::string content = Trim(*git_file);
    if (!content.starts_with(kGitdir)) {
        return ".git 文件内容不是 gitdir 指针";
    }
    const std::filesystem::path gitdir = Utf8ToPath(Trim(content.substr(kGitdir.size())));
    const std::string worktrees_root_key = NormalizeKey(main_root / ".git" / "worktrees");
    const std::string gitdir_key = NormalizeKey(gitdir);
    if (gitdir_key.empty() || !KeyIsUnder(gitdir_key, worktrees_root_key)) {
        return ".git 指向的不是本仓库 .git/worktrees 之下的登记项:" + PathToUtf8(gitdir);
    }
    // 反向登记:主仓 .git/worktrees/<n>/gitdir 里写的就是这间房的路径。
    const auto registered = ReadSmallFile(gitdir / "gitdir");
    if (!registered.has_value()) {
        return "git 元数据缺反向登记(gitdir 文件),worktree 登记已损坏";
    }
    const std::string registered_key = NormalizeKey(Utf8ToPath(Trim(*registered)));
    if (registered_key != path_key) {
        return "git 反向登记指向别处(" + Trim(*registered) + "),不是这间房";
    }
    return std::nullopt;
}

std::string ResolveFreshBaseRef(const std::filesystem::path& repository_root, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    // 远端默认分支:origin/HEAD 的 symbolic-ref。
    GitCommandResult head =
        runner({repository_root, {"symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"}});
    std::string ref = Trim(head.output);
    if (head.exit_code != 0 || ref.empty() || !ref.starts_with("refs/remotes/")) {
        return "HEAD";
    }
    ref = ref.substr(std::string("refs/remotes/").size());  // "origin/main"

    // 距上次 fetch 超 24 小时补一次(fetch 5 秒封顶,失败回落本地缓存 ref)。
    std::error_code ec;
    const std::filesystem::path fetch_head = repository_root / ".git" / "FETCH_HEAD";
    std::filesystem::file_time_type fetched{};
    bool stale = true;
    if (std::filesystem::exists(fetch_head, ec)) {
        fetched = std::filesystem::last_write_time(fetch_head, ec);
        if (!ec) {
            const auto now = std::filesystem::file_time_type::clock::now();
            stale = now - fetched > std::chrono::hours(24);
        }
    }
    if (stale) {
        runner({repository_root, {"fetch", "--quiet", "--no-tags"}, /*timeout_ms=*/5000});
    }

    // 本地确实有这个 ref 才敢用;没有(远端信息是陈货)回落 HEAD。
    const GitCommandResult verify = runner({repository_root, {"rev-parse", "--verify", "--quiet", ref}});
    if (verify.exit_code != 0) {
        return "HEAD";
    }
    return ref;
}

bool LockWorktree(const std::filesystem::path& worktree_path, const std::string& reason, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    std::vector<std::string> args = {"worktree", "lock"};
    if (!reason.empty()) {
        args.push_back("--reason");
        args.push_back(reason);
    }
    args.push_back(PathToUtf8(worktree_path));
    return runner({worktree_path, std::move(args)}).exit_code == 0;
}

bool UnlockWorktree(const std::filesystem::path& worktree_path, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    return runner({worktree_path, {"worktree", "unlock", PathToUtf8(worktree_path)}}).exit_code == 0;
}

std::vector<WorktreeEntry> ListWorktrees(const std::filesystem::path& repository_root, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    const GitCommandResult git = runner({repository_root, {"worktree", "list", "--porcelain"}});
    if (git.exit_code != 0) {
        return {};
    }
    return ParsePorcelainList(git.output);
}

std::optional<std::string> SafeRemoveTree(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            return ec.message();
        }
        return std::nullopt;  // 本来就没有,视同删干净
    }
    // 逐项下刀:链接(reparse point)只删自身,绝不追进指向的目录。
    std::function<std::optional<std::string>(const std::filesystem::path&)> remove_recursive =
        [&](const std::filesystem::path& current) -> std::optional<std::string> {
        std::error_code local_ec;
        if (IsLinkEntry(current)) {
            std::filesystem::remove(current, local_ec);
            return local_ec ? std::optional<std::string>(local_ec.message()) : std::nullopt;
        }
        if (std::filesystem::is_directory(current, local_ec)) {
            for (std::filesystem::directory_iterator it(current, local_ec), end; !local_ec && it != end; ++it) {
                if (const auto failure = remove_recursive(it->path()); failure.has_value()) {
                    return failure;
                }
            }
            if (local_ec) {
                return local_ec.message();
            }
            std::filesystem::remove(current, local_ec);
            return local_ec ? std::optional<std::string>(local_ec.message()) : std::nullopt;
        }
        std::filesystem::remove(current, local_ec);
        return local_ec ? std::optional<std::string>(local_ec.message()) : std::nullopt;
    };
    return remove_recursive(path);
}

// ---------------------------------------------------------------------------
// .worktreeinclude:极简 gitignore 语法匹配器
// ---------------------------------------------------------------------------

namespace {

// 一段 glob(*、?、** 在段层面展开)匹配一个名字。
bool SegmentGlobMatch(std::string_view pattern, std::string_view name) {
    // 经典双指针回溯:星号记下位置,失配回溯吃一个字符。
    std::size_t p = 0, n = 0;
    std::size_t star_p = std::string_view::npos;
    std::size_t star_n = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_n = n;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

// pattern 段列表(含 ** 通配段)匹配 candidate 段列表。
bool SegmentPathMatch(const std::vector<std::string_view>& pattern, std::size_t pi,
                      const std::vector<std::string_view>& cand, std::size_t ci) {
    if (pi == pattern.size()) {
        return ci == cand.size();
    }
    if (pattern[pi] == "**") {
        // ** 吃零段或多吃几段;但收尾的 ** 按 gitignore 至少吃一段
        // ("a/**" 只匹配 a 里面的一切,不匹配 a 本身)。
        const std::size_t min_skip = pi + 1 == pattern.size() ? ci + 1 : ci;
        for (std::size_t skip = min_skip; skip <= cand.size(); ++skip) {
            if (SegmentPathMatch(pattern, pi + 1, cand, skip)) {
                return true;
            }
        }
        return false;
    }
    if (ci == cand.size()) {
        return false;
    }
    if (!SegmentGlobMatch(pattern[pi], cand[ci])) {
        return false;
    }
    return SegmentPathMatch(pattern, pi + 1, cand, ci + 1);
}

std::vector<std::string_view> SplitSegmentsView(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (i > start) {
                out.push_back(path.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

bool WorktreeIncludeMatches(std::string_view pattern, std::string_view candidate, bool candidate_is_dir) {
    bool dir_only = false;
    if (!pattern.empty() && pattern.back() == '/') {
        dir_only = true;
        pattern.remove_suffix(1);
    }
    if (pattern.empty()) {
        return false;
    }
    // 尾 "/**" 等价于目录锚定下的全通配,交给 ** 段照常处理。
    const bool anchored = pattern.front() == '/';
    if (anchored) {
        pattern.remove_prefix(1);
    }
    const std::vector<std::string_view> pat = SplitSegmentsView(pattern);
    const std::vector<std::string_view> cand = SplitSegmentsView(candidate);
    if (pat.empty() || cand.empty()) {
        return false;
    }
    const bool has_slash = pat.size() > 1;
    if (dir_only && !candidate_is_dir) {
        // 目录模式只吃目录本身;目录下的文件靠"目录段匹配后全通配"那条
        // (build/ 等价 build/**),补一颗 ** 再比。
        std::vector<std::string_view> extended(pat);
        extended.push_back("**");
        return SegmentPathMatch(extended, 0, cand, 0);
    }
    if (!anchored && !has_slash) {
        // 不带斜杠:任意深度按文件名(或目录名)匹配。
        return SegmentGlobMatch(pattern, cand.back());
    }
    return SegmentPathMatch(pat, 0, cand, 0);
}

std::vector<std::filesystem::path> CopyWorktreeInclude(const std::filesystem::path& repository_root,
                                                       const std::filesystem::path& target, GitRunner runner) {
    std::vector<std::filesystem::path> copied;
    const auto include_text = ReadSmallFile(repository_root / ".worktreeinclude");
    if (!include_text.has_value()) {
        return copied;
    }
    // 规则清单:空行/# 注释跳过;! 开头取反。
    struct Rule {
        std::string pattern;
        bool negate = false;
    };
    std::vector<Rule> rules;
    {
        std::istringstream input(*include_text);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            line = Trim(line);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            Rule rule;
            if (line.front() == '!') {
                rule.negate = true;
                line = Trim(line.substr(1));
            }
            if (line.empty()) {
                continue;
            }
            rule.pattern = line;
            rules.push_back(std::move(rule));
        }
    }
    if (rules.empty()) {
        return copied;
    }
    if (!runner) {
        runner = DefaultGitRunner;
    }

    // 候选:主树下的文件(跳过 .git / .lubancode / 各 worktree),深度优先
    // 走目录;对每个文件问三件事:include 匹配?被 gitignore?没被 track?
    // 三问都过才拷。tracked 文件 git checkout 自带,永不拷。
    const std::string root_key = NormalizeKey(repository_root);
    const std::string worktrees_key = NormalizeKey(repository_root / ".lubancode" / "worktrees");
    std::function<void(const std::filesystem::path&, const std::string&)> walk =
        [&](const std::filesystem::path& dir, const std::string& rel) {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; ++it) {
                const std::filesystem::path& entry = it->path();
                const std::string name = PathToUtf8(entry.filename());
                if (name == ".git" || name == ".lubancode") {
                    continue;
                }
                const std::string child_rel = rel.empty() ? name : rel + "/" + name;
                if (IsLinkEntry(entry)) {
                    continue;  // 链接/junction 不拷,防越界
                }
                const bool is_dir = it->is_directory(ec);
                if (ec) {
                    continue;
                }
                if (is_dir) {
                    const std::string entry_key = NormalizeKey(entry);
                    if (KeyIsUnder(entry_key, worktrees_key) || entry_key == worktrees_key) {
                        continue;  // 别的房不进
                    }
                    walk(entry, child_rel);
                    continue;
                }
                // 规则从上到下,最后一条命中的说了算。
                bool matched = false;
                for (const auto& rule : rules) {
                    if (WorktreeIncludeMatches(rule.pattern, child_rel, /*candidate_is_dir=*/false)) {
                        matched = !rule.negate;
                    }
                }
                if (!matched) {
                    continue;
                }
                // gitignore 判定:check-ignore 退出码 0 = 命中忽略规则。
                const GitCommandResult ignored =
                    runner({repository_root, {"check-ignore", "--quiet", child_rel}});
                if (ignored.exit_code != 0) {
                    continue;
                }
                // tracked 判定:ls-files --error-unmatch 退出码 0 = 已跟踪。
                const GitCommandResult tracked = runner(
                    {repository_root, {"ls-files", "--error-unmatch", child_rel}});
                if (tracked.exit_code == 0) {
                    continue;
                }
                std::error_code copy_ec;
                const std::filesystem::path dst = target / Utf8ToPath(child_rel);
                std::filesystem::create_directories(dst.parent_path(), copy_ec);
                if (copy_ec) {
                    continue;
                }
                std::filesystem::copy_file(entry, dst, std::filesystem::copy_options::overwrite_existing,
                                           copy_ec);
                if (!copy_ec) {
                    copied.push_back(Utf8ToPath(child_rel));
                }
            }
        };
    walk(repository_root, std::string());
    return copied;
}

// ---------------------------------------------------------------------------
// WorktreeSession
// ---------------------------------------------------------------------------

WorktreeSession::WorktreeSession(GitRunner runner) : runner_(std::move(runner)) {
    if (!runner_) {
        runner_ = DefaultGitRunner;
    }
}

GitCommandResult WorktreeSession::RunGit(const std::filesystem::path& working_directory,
                                         std::vector<std::string> args) const {
    return runner_({working_directory, std::move(args)});
}

WorktreeResult WorktreeSession::FindRepository(const std::filesystem::path& from) const {
    const GitCommandResult git = RunGit(from, {"rev-parse", "--show-toplevel"});
    if (git.exit_code != 0) {
        return {WorktreeResultCode::NotRepository, {}, {}, git.error.empty() ? Trim(git.output) : git.error, {}};
    }
    const std::string root = Trim(git.output);
    if (root.empty()) {
        return {WorktreeResultCode::Listed, {}, {}, "git returned an empty repository root", {}};
    }
    return {WorktreeResultCode::Listed, Utf8ToPath(root), {}, {}, {}};
}

WorktreeResult WorktreeSession::Create(const std::string& requested_name) {
    // /worktree new 语义不动:基准恒为当前 HEAD。
    return Enter(requested_name, /*base=*/"head");
}

WorktreeResult WorktreeSession::CreateRoom(const std::filesystem::path& repository_root, const std::string& name,
                                           const std::string& base_ref) {
    const std::filesystem::path target = WorktreePath(repository_root, name);
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return {WorktreeResultCode::FilesystemError, target, {}, "target directory already exists", {}};
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return {WorktreeResultCode::FilesystemError, target, {}, ec.message(), {}};
    }

    const std::string branch = "worktree/" + name;
    const GitCommandResult git = RunGit(
        repository_root, {"worktree", "add", "-b", branch, PathToUtf8(target), base_ref});
    if (git.exit_code != 0) {
        return {WorktreeResultCode::GitError, target, branch, git.error.empty() ? Trim(git.output) : git.error, {}};
    }
    CopyWorktreeInclude(repository_root, target, runner_);
    return {WorktreeResultCode::Created, target, branch, {}, {}};
}

WorktreeResult WorktreeSession::Enter(const std::string& name_or_path, const std::string& base,
                                      bool confirmed_outside) {
    if (active_) {
        return {WorktreeResultCode::AlreadyActive, worktree_path_, branch_, {}, {}};
    }
    if (base != "fresh" && base != "head") {
        return {WorktreeResultCode::InvalidArgument, {}, {}, "base 只认 fresh 或 head", {}};
    }
    const std::filesystem::path current = std::filesystem::current_path();
    WorktreeResult repository = FindRepository(current);
    if (repository.code != WorktreeResultCode::Listed) {
        return repository;
    }

    // 定位目标房:名字(自家园子里)、绝对路径、或空(自动生成新建)。
    std::string name;
    std::filesystem::path target;
    const bool looks_like_path =
        name_or_path.find('/') != std::string::npos || name_or_path.find('\\') != std::string::npos;
    if (name_or_path.empty()) {
        name = GenerateWorktreeName();
        target = WorktreePath(repository.path, name);
    } else if (looks_like_path) {
        target = Utf8ToPath(name_or_path);
        // agent-* 是产品隔离房的所有权边界；即使给绝对路径也不许通用
        // worktree 入口创建/接管，否则会绕开调用者 HEAD 与 TaskSnapshot 账。
        if (target.filename().string().rfind("agent-", 0) == 0) {
            return {WorktreeResultCode::InvalidName, {}, {},
                    "'agent-' prefix is reserved for isolated subagent worktrees", {}};
        }
    } else {
        name = name_or_path;
        if (const auto invalid = ValidateWorktreeName(name); invalid.has_value()) {
            return {WorktreeResultCode::InvalidName, {}, {}, *invalid, {}};
        }
        target = WorktreePath(repository.path, name);
    }

    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        if (looks_like_path) {
            return {WorktreeResultCode::FilesystemError, target, {}, "路径不存在: " + name_or_path, {}};
        }
        // 园子里没这间房:到 git 登记的 worktree 里按名字找(用户手起的房)。
        for (const auto& entry : ListWorktrees(repository.path, runner_)) {
            if (entry.path.filename() == Utf8ToPath(name)) {
                target = entry.path;
                break;
            }
        }
        if (!std::filesystem::exists(target, ec)) {
            // 哪都没有:新建。
            const std::string base_ref = base == "fresh" ? ResolveFreshBaseRef(repository.path, runner_) : "HEAD";
            WorktreeResult result = CreateRoom(repository.path, name, base_ref);
            if (result.code != WorktreeResultCode::Created) {
                return result;
            }
            std::filesystem::current_path(result.path, ec);
            if (ec) {
                return {WorktreeResultCode::FilesystemError, result.path, result.branch, ec.message(), {}};
            }
            active_ = true;
            remove_pending_ = false;
            original_directory_ = current;
            repository_root_ = repository.path;
            worktree_path_ = result.path;
            branch_ = result.branch;
            if (scope_hook_) {
                scope_hook_(true, worktree_path_, repository_root_);
            }
            return result;
        }
    }

    // 已有的房:园子外的必须用户点头(硬安全线,确认档压不住)。
    const bool in_yard = KeyIsUnder(NormalizeKey(target),
                                    NormalizeKey(repository.path / ".lubancode" / "worktrees"));
    if (!in_yard && !confirmed_outside) {
        return {WorktreeResultCode::NeedsUserConfirmation, target, {}, PathToUtf8(target), {}};
    }
    return EnterRoom(repository.path, target, base);
}

WorktreeResult WorktreeSession::EnterRoom(const std::filesystem::path& repository_root,
                                          const std::filesystem::path& target, const std::string& base) {
    // 验明正身:不是本仓名下独立 checkout 的马甲房,拒进。
    if (const auto refused = VerifyWorktreeIdentity(target, repository_root); refused.has_value()) {
        return {WorktreeResultCode::VerificationFailed, target, {}, *refused, {}};
    }

    std::string branch = CurrentGitBranch(target, runner_);
    const bool owns_branch = branch.rfind("worktree/", 0) == 0;

    // fresh 且旧房干净、无自有提交:重置到默认分支(对齐 Claude Code 重开)。
    if (base == "fresh" && owns_branch && WorktreeClean(target, runner_)) {
        const std::string base_ref = ResolveFreshBaseRef(repository_root, runner_);
        if (base_ref != "HEAD") {
            const GitCommandResult ancestor =
                RunGit(repository_root, {"merge-base", "--is-ancestor", branch, base_ref});
            if (ancestor.exit_code == 0) {
                RunGit(target, {"reset", "--hard", base_ref});
            }
        }
    }

    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path();
    std::filesystem::current_path(target, ec);
    if (ec) {
        return {WorktreeResultCode::FilesystemError, target, branch, ec.message(), {}};
    }
    active_ = true;
    remove_pending_ = false;
    original_directory_ = current;
    repository_root_ = repository_root;
    worktree_path_ = target;
    branch_ = branch;
    if (scope_hook_) {
        scope_hook_(true, worktree_path_, repository_root_);
    }
    return {WorktreeResultCode::Created, target, branch, {}, {}};
}

WorktreeResult WorktreeSession::EnterByPath(const std::filesystem::path& worktree_path) {
    if (active_) {
        return {WorktreeResultCode::AlreadyActive, worktree_path_, branch_, {}, {}};
    }
    const std::filesystem::path current = std::filesystem::current_path();
    WorktreeResult repository = FindRepository(current);
    if (repository.code != WorktreeResultCode::Listed) {
        return repository;
    }
    return EnterRoom(repository.path, worktree_path, /*base=*/"head");
}

WorktreeResult WorktreeSession::List() const {
    const std::filesystem::path current = std::filesystem::current_path();
    WorktreeResult repository = FindRepository(current);
    if (repository.code != WorktreeResultCode::Listed) {
        return repository;
    }
    const GitCommandResult git = RunGit(repository.path, {"worktree", "list", "--porcelain"});
    if (git.exit_code != 0) {
        return {WorktreeResultCode::GitError, {}, {}, git.error.empty() ? Trim(git.output) : git.error, {}};
    }
    WorktreeResult result;
    result.code = WorktreeResultCode::Listed;
    result.path = repository.path;
    result.entries = ParsePorcelainList(git.output);
    return result;
}

WorktreeResult WorktreeSession::Status() const {
    if (!active_) {
        return {WorktreeResultCode::NoActiveWorktree, {}, {}, {}, {}};
    }
    WorktreeResult result;
    result.code = WorktreeResultCode::Listed;  // 复用 Listed:字段齐全,展示层自排版
    result.path = worktree_path_;
    result.branch = branch_;
    result.detail = WorktreeClean(worktree_path_, runner_) ? "clean" : "dirty";
    return result;
}

std::string WorktreeSession::active_name() const {
    if (!active_ || worktree_path_.empty()) {
        return {};
    }
    return PathToUtf8(worktree_path_.filename());
}

WorktreeResult WorktreeSession::Exit(const std::string& mode) {
    if (mode != "keep" && mode != "remove") {
        return {WorktreeResultCode::InvalidArgument, {}, {}, mode, {}};
    }
    if (!active_) {
        return {WorktreeResultCode::NoActiveWorktree, {}, {}, {}, {}};
    }
    if (mode == "keep") {
        std::error_code ec;
        std::filesystem::current_path(original_directory_, ec);
        if (ec) {
            return {WorktreeResultCode::FilesystemError, original_directory_, branch_, ec.message(), {}};
        }
        active_ = false;
        remove_pending_ = false;
        if (scope_hook_) {
            scope_hook_(false, worktree_path_, repository_root_);
        }
        return {WorktreeResultCode::Kept, worktree_path_, branch_, {}, {}};
    }

    const GitCommandResult status = RunGit(worktree_path_, {"status", "--porcelain"});
    if (status.exit_code != 0) {
        return {WorktreeResultCode::GitError, worktree_path_, branch_,
                status.error.empty() ? Trim(status.output) : status.error, {}};
    }
    if (!Trim(status.output).empty()) {
        remove_pending_ = true;
        return {WorktreeResultCode::NeedsRemoveConfirmation, worktree_path_, branch_, {}, {}};
    }
    return RemoveNow();
}

WorktreeResult WorktreeSession::ConfirmRemove() {
    if (!active_ || !remove_pending_) {
        return {WorktreeResultCode::NoActiveWorktree, {}, {}, {}, {}};
    }
    return RemoveNow();
}

WorktreeResult WorktreeSession::RemoveNow() {
    std::error_code ec;
    std::filesystem::current_path(original_directory_, ec);
    if (ec) {
        return {WorktreeResultCode::FilesystemError, original_directory_, branch_, ec.message(), {}};
    }

    // 删房自家下刀(SafeRemoveTree 逐项验 reparse point,junction 只删链接),
    // 再让 git 收拾登记。yolo 也不豁免脏房确认——脏确认在 Exit/上一关。
    if (const auto failure = SafeRemoveTree(worktree_path_); failure.has_value()) {
        return {WorktreeResultCode::FilesystemError, worktree_path_, branch_, *failure, {}};
    }
    RunGit(repository_root_, {"worktree", "prune"});

    active_ = false;
    remove_pending_ = false;
    if (scope_hook_) {
        scope_hook_(false, worktree_path_, repository_root_);
    }
    // 分支只有自己起的(worktree/<名>)才删;外面进来的房,分支是别人的。
    if (branch_.rfind("worktree/", 0) == 0) {
        const GitCommandResult branch = RunGit(repository_root_, {"branch", "-D", branch_});
        if (branch.exit_code != 0) {
            return {WorktreeResultCode::GitError, worktree_path_, branch_,
                    branch.error.empty() ? Trim(branch.output) : branch.error, {}};
        }
    }
    return {WorktreeResultCode::Removed, worktree_path_, branch_, {}, {}};
}

// ---------------------------------------------------------------------------
// 子代理的房与陈房清扫
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path> FindRepositoryRoot(const std::filesystem::path& from, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    const GitCommandResult git = runner({from, {"rev-parse", "--show-toplevel"}});
    if (git.exit_code != 0) {
        return std::nullopt;
    }
    const std::string root = Trim(git.output);
    if (root.empty()) {
        return std::nullopt;
    }
    return Utf8ToPath(root);
}

namespace {

// agent-<随机> 的房名,撞了重摇(房区里同名目录还在就换一个)。
std::string FreshAgentWorktreeName(const std::filesystem::path& repository_root) {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device seed;
    std::mt19937_64 generator((static_cast<std::uint64_t>(seed()) << 32U) ^ seed());
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::uniform_int_distribution<std::size_t> pick(0, sizeof(kAlphabet) - 2);
        std::string name = "agent-";
        for (int i = 0; i < 10; ++i) {
            name += kAlphabet[pick(generator)];
        }
        std::error_code ec;
        if (!std::filesystem::exists(WorktreePath(repository_root, name), ec)) {
            return name;
        }
    }
    return "agent-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

// 房分支上有没有别的本地分支不含的提交(自有提交)。没有别的本地分支
// (刚 init 的仓)就当没有自有提交——干净即删。
bool RoomHasOwnCommits(const std::filesystem::path& repository_root, const std::string& branch, GitRunner runner) {
    const GitCommandResult refs =
        runner({repository_root, {"for-each-ref", "refs/heads", "--format=%(refname:short)"}});
    if (refs.exit_code != 0) {
        return true;  // 认不出,保守当有活
    }
    std::vector<std::string> others;
    std::istringstream input(refs.output);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = Trim(line);
        if (!line.empty() && line != branch && line.rfind("worktree/", 0) != 0) {
            others.push_back(line);
        }
    }
    if (others.empty()) {
        return false;
    }
    std::vector<std::string> args = {"rev-list", "--count", branch, "--not"};
    args.insert(args.end(), others.begin(), others.end());
    const GitCommandResult count = runner({repository_root, std::move(args)});
    if (count.exit_code != 0) {
        return true;  // 认不出,保守当有活
    }
    const long long parsed = std::strtoll(Trim(count.output).c_str(), nullptr, 10);
    return parsed > 0;
}

// 房分支自基线以来的提交数(派工单 §五):比"别的分支不含的提交"准——
// 调用者自己领先远端的未推提交不算房的活。base 空/查询失败退保守口径。
bool RoomHasCommitsSinceBase(const std::filesystem::path& repository_root, const std::string& branch,
                             const std::string& base_commit, GitRunner runner) {
    if (base_commit.empty()) {
        return RoomHasOwnCommits(repository_root, branch, runner);
    }
    const GitCommandResult count =
        runner({repository_root, {"rev-list", "--count", branch, "--not", base_commit}});
    if (count.exit_code != 0) {
        return true;  // 认不出,保守当有活
    }
    const long long parsed = std::strtoll(Trim(count.output).c_str(), nullptr, 10);
    return parsed > 0;
}

}  // namespace

FrozenWorktreeBase FreezeWorktreeBase(const std::filesystem::path& working_directory, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    FrozenWorktreeBase out;
    const GitCommandResult hash = runner({working_directory, {"rev-parse", "HEAD"}});
    if (hash.exit_code != 0) {
        return out;
    }
    out.commit = Trim(hash.output);
    const GitCommandResult ref = runner({working_directory, {"symbolic-ref", "--quiet", "--short", "HEAD"}});
    out.ref = ref.exit_code == 0 && !Trim(ref.output).empty() ? Trim(ref.output) : "(detached)";
    return out;
}

AgentWorktree CreateAgentWorktree(const std::filesystem::path& repository_root, const std::string& requested_base,
                                  const std::string& requested_base_ref, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    AgentWorktree out;
    // 基线冻结(派工单 §三):优先吃调用方传入的冻结提交;没有就冻结仓库
    // 当前 HEAD。绝不再解析 origin 默认分支——本地领先远端时,子代理会
    // 在旧代码上开工。
    const FrozenWorktreeBase fallback = requested_base.empty()
                                           ? FreezeWorktreeBase(repository_root, runner)
                                           : FrozenWorktreeBase{};
    const std::string base_commit = !requested_base.empty() ? requested_base : fallback.commit;
    const std::string base_ref =
        !requested_base.empty() ? requested_base_ref : (!fallback.ref.empty() ? fallback.ref : "(detached)");
    if (base_commit.empty()) {
        out.error = "拿不到基线提交(调用者 HEAD 解析失败),不建房——绝不回落 origin 默认分支";
        return out;
    }
    const std::string name = FreshAgentWorktreeName(repository_root);
    const std::filesystem::path room = WorktreePath(repository_root, name);
    const std::string branch = "worktree/" + name;

    std::error_code ec;
    std::filesystem::create_directories(room.parent_path(), ec);
    if (ec) {
        out.error = ec.message();
        return out;
    }
    const GitCommandResult git = runner(
        {repository_root, {"worktree", "add", "-b", branch, PathToUtf8(room), base_commit}});
    if (git.exit_code != 0) {
        out.error = git.error.empty() ? Trim(git.output) : git.error;
        return out;
    }
    // 三者对账(派工单 §3.3):房的实际 HEAD 必须逐字等于冻结基线;对不上
    // 当场拆房报错,把三个值全亮出来,不让子任务在错基线上静默开工。
    const GitCommandResult head = runner({room, {"rev-parse", "HEAD"}});
    out.actual_head = Trim(head.output);
    if (head.exit_code != 0 || out.actual_head != base_commit) {
        UnlockWorktree(room, runner);
        SafeRemoveTree(room);
        runner({repository_root, {"worktree", "prune"}});
        runner({repository_root, {"branch", "-D", branch}});
        out.error = "隔离房基线对不上,已拆房不静默开工: base_ref=" + base_ref + " base_commit=" + base_commit +
                    " actual_head=" + (out.actual_head.empty() ? "(读不到)" : out.actual_head);
        return out;
    }
    CopyWorktreeInclude(repository_root, room, runner);
    LockWorktree(room, "lubancode agent " + name, runner);
    out.ok = true;
    out.repo_root = repository_root;
    out.room_path = room;
    out.name = name;
    out.branch = branch;
    out.base_ref = base_ref;
    out.base_commit = base_commit;
    return out;
}

AgentWorktree CreateAgentWorktree(const std::filesystem::path& repository_root, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    const FrozenWorktreeBase frozen = FreezeWorktreeBase(repository_root, runner);
    return CreateAgentWorktree(repository_root, frozen.commit, frozen.ref, runner);
}

AgentWorktreeFinish FinishAgentWorktree(const std::filesystem::path& repository_root,
                                        const std::filesystem::path& room_path, const std::string& branch,
                                        const std::string& base_commit, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    AgentWorktreeFinish out;
    UnlockWorktree(room_path, runner);
    std::error_code ec;
    if (!std::filesystem::exists(room_path, ec)) {
        out.removed = true;
        return out;
    }
    if (!WorktreeClean(room_path, runner)) {
        // 未提交现场绝不删(派工单 §五):解锁留房,note 给主控指路。
        out.awaiting_review = true;
        const GitCommandResult head = runner({room_path, {"rev-parse", "HEAD"}});
        out.head_commit = head.exit_code == 0 ? Trim(head.output) : std::string();
        out.note = "\n\n隔离子代理的工作树有未提交改动,已保留(awaiting_parent_review):\n路径: " +
                   PathToUtf8(room_path) + "\n分支: " + branch +
                   "\n复核: git -C \"" + PathToUtf8(room_path) + "\" status && git -C \"" +
                   PathToUtf8(room_path) + "\" diff" + "\n清理(复核后): git worktree remove \"" +
                   PathToUtf8(room_path) + "\" && git branch -D " + branch +
                   "\n需要后续收尾(提交/合并/清理)。";
        return out;
    }
    if (RoomHasCommitsSinceBase(repository_root, branch, base_commit, runner)) {
        // 已提交现场同样待复核(派工单 §五):子代理的报告拿房路径当复核入口,
        // 主控确认前不删。note 带持久提交引用与复核/清理/重挂命令——房真被
        // 外部清掉后,一条 git worktree add 也能把现场挂回来。
        out.awaiting_review = true;
        const GitCommandResult head = runner({room_path, {"rev-parse", "HEAD"}});
        out.head_commit = head.exit_code == 0 ? Trim(head.output) : std::string();
        const std::string range = base_commit.empty() ? std::string("-5") : base_commit + "..HEAD";
        out.note = "\n\n隔离子代理已在房内提交,现场保留待主控复核(awaiting_parent_review):\n路径: " +
                   PathToUtf8(room_path) + "\n分支: " + branch +
                   (out.head_commit.empty() ? std::string() : "\nHEAD: " + out.head_commit) +
                   "\n复核: git -C \"" + PathToUtf8(room_path) + "\" log --stat " + range +
                   "\n清理(复核完): git worktree remove \"" + PathToUtf8(room_path) + "\" && git branch -D " +
                   branch + "\n房若已不在: git worktree add \"" + PathToUtf8(room_path) + "\" " + branch +
                   " 一条命令重挂。";
        return out;
    }
    if (const auto failure = SafeRemoveTree(room_path); failure.has_value()) {
        out.note = "\n\n隔离子代理的工作树删除失败(" + *failure + "),已保留: " + PathToUtf8(room_path);
        return out;
    }
    runner({repository_root, {"worktree", "prune"}});
    if (branch.rfind("worktree/", 0) == 0) {
        runner({repository_root, {"branch", "-D", branch}});
    }
    out.removed = true;
    return out;
}

StaleAgentWorktreeCleanup CleanStaleAgentWorktrees(const std::filesystem::path& repository_root,
                                                   std::chrono::hours max_age, GitRunner runner) {
    if (!runner) {
        runner = DefaultGitRunner;
    }
    StaleAgentWorktreeCleanup out;
    const std::filesystem::path rooms_root = repository_root / ".lubancode" / "worktrees";
    std::error_code ec;
    if (!std::filesystem::is_directory(rooms_root, ec)) {
        return out;
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now() - max_age;
    for (std::filesystem::directory_iterator it(rooms_root, ec), end; !ec && it != end; ++it) {
        const std::string name = PathToUtf8(it->path().filename());
        if (!name.starts_with("agent-")) {
            continue;  // 只清自家命名规约的房,用户手起的永不碰
        }
        std::error_code mtime_ec;
        const auto mtime = std::filesystem::last_write_time(it->path(), mtime_ec);
        if (mtime_ec || mtime >= cutoff) {
            out.kept_fresh += 1;
            continue;
        }
        // 被杀会话留下的锁先放掉(跑着的代理不该轮到清扫:锁 + 岁数双条件,
        // 真跑着的房 mtime 通常新;即便误放,下面有活检查还会兜住)。
        UnlockWorktree(it->path(), runner);
        const std::string branch = "worktree/" + name;
        if (!WorktreeClean(it->path(), runner) || RoomHasOwnCommits(repository_root, branch, runner)) {
            out.kept_dirty += 1;
            continue;
        }
        if (const auto failure = SafeRemoveTree(it->path()); failure.has_value()) {
            out.kept_dirty += 1;
            continue;
        }
        runner({repository_root, {"worktree", "prune"}});
        runner({repository_root, {"branch", "-D", branch}});
        out.removed += 1;
    }
    return out;
}

}  // namespace lubancode::cli
