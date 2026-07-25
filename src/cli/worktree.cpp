#include "cli/worktree.hpp"

#include <cctype>
#include <cstdint>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#include "platform/process.hpp"

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

GitCommandResult DefaultGitRunner(const GitCommand& command) {
    std::vector<std::string> argv = {"git", "-C", PathToUtf8(command.working_directory)};
    argv.insert(argv.end(), command.args.begin(), command.args.end());
    const platform::ProcessResult process = platform::RunProcess(argv, /*timeout_ms=*/120000);
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
            entries.push_back({Utf8ToPath(line.substr(kWorktree.size())), std::string(), false});
            current = &entries.back();
        } else if (current != nullptr && line.starts_with(kBranch)) {
            current->branch = line.substr(kBranch.size());
        } else if (current != nullptr && line == "detached") {
            current->detached = true;
        }
    }
    return entries;
}

}  // namespace

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
        return {WorktreeResultCode::NotRepository, {}, {}, "git returned an empty repository root", {}};
    }
    return {WorktreeResultCode::Listed, Utf8ToPath(root), {}, {}, {}};
}

WorktreeResult WorktreeSession::Create(const std::string& requested_name) {
    if (active_) {
        return {WorktreeResultCode::AlreadyActive, worktree_path_, branch_, {}, {}};
    }
    const std::filesystem::path current = std::filesystem::current_path();
    WorktreeResult repository = FindRepository(current);
    if (repository.code != WorktreeResultCode::Listed) {
        return repository;
    }

    const std::string name = requested_name.empty() ? GenerateWorktreeName() : requested_name;
    if (const auto invalid = ValidateWorktreeName(name); invalid.has_value()) {
        return {WorktreeResultCode::InvalidName, {}, {}, *invalid, {}};
    }

    const std::filesystem::path target = WorktreePath(repository.path, name);
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return {WorktreeResultCode::FilesystemError, target, {}, "target directory already exists", {}};
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return {WorktreeResultCode::FilesystemError, target, {}, ec.message(), {}};
    }

    const std::string branch = "worktree/" + name;
    const GitCommandResult git =
        RunGit(repository.path, {"worktree", "add", "-b", branch, PathToUtf8(target), "HEAD"});
    if (git.exit_code != 0) {
        return {WorktreeResultCode::GitError, target, branch, git.error.empty() ? Trim(git.output) : git.error, {}};
    }

    std::filesystem::current_path(target, ec);
    if (ec) {
        return {WorktreeResultCode::FilesystemError, target, branch, ec.message(), {}};
    }
    active_ = true;
    remove_pending_ = false;
    original_directory_ = current;
    repository_root_ = repository.path;
    worktree_path_ = target;
    branch_ = branch;
    return {WorktreeResultCode::Created, worktree_path_, branch_, {}, {}};
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

    const GitCommandResult remove =
        RunGit(repository_root_, {"worktree", "remove", "--force", PathToUtf8(worktree_path_)});
    if (remove.exit_code != 0) {
        return {WorktreeResultCode::GitError, worktree_path_, branch_,
                remove.error.empty() ? Trim(remove.output) : remove.error, {}};
    }

    active_ = false;
    remove_pending_ = false;
    const GitCommandResult branch = RunGit(repository_root_, {"branch", "-D", branch_});
    if (branch.exit_code != 0) {
        return {WorktreeResultCode::GitError, worktree_path_, branch_,
                branch.error.empty() ? Trim(branch.output) : branch.error, {}};
    }
    return {WorktreeResultCode::Removed, worktree_path_, branch_, {}, {}};
}

}  // namespace lubancode::cli
