// /worktree 的纯逻辑和 Git 编排。终端输入、翻译、确认提示留在 main.cpp，
// 这里因此能拿假 GitRunner 做单测，不必真的建仓库。

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::cli {

enum class WorktreeAction {
    Invalid,
    New,
    List,
    Exit,
};

struct ParsedWorktreeCommand {
    WorktreeAction action = WorktreeAction::Invalid;
    std::string name;       // /worktree new [name]
    std::string exit_mode;  // /worktree exit keep|remove
};

ParsedWorktreeCommand ParseWorktreeCommand(std::string_view args);

// 用户给的名字同时进目录名、分支名。只收一段保守的 ASCII，免得 ../、
// Windows 保留字符和 Git ref 语法混在一起。
std::optional<std::string> ValidateWorktreeName(std::string_view name);
std::string GenerateWorktreeName();
std::filesystem::path WorktreePath(const std::filesystem::path& repository_root, std::string_view name);

struct GitCommandResult {
    int exit_code = 0;
    std::string output;
    std::string error;
};

struct GitCommand {
    std::filesystem::path working_directory;
    std::vector<std::string> args;
};

// 让 Git 调用可替身：生产路径走 git -C，测试只需喂返回值。
using GitRunner = std::function<GitCommandResult(const GitCommand&)>;

// status panel 用的轻量 Git 摘要。普通分支返回短名；游离 HEAD 返回
// "detached@<短哈希>"；不在仓库、git 不可用或查询失败时返回空串。
// runner 可替换，单测不必真的起 git。
std::string CurrentGitBranch(const std::filesystem::path& working_directory,
                             GitRunner runner = {});

struct WorktreeEntry {
    std::filesystem::path path;
    std::string branch;
    bool detached = false;
};

enum class WorktreeResultCode {
    Created,
    Listed,
    Kept,
    Removed,
    NeedsRemoveConfirmation,
    NotRepository,
    InvalidArgument,
    InvalidName,
    AlreadyActive,
    NoActiveWorktree,
    GitError,
    FilesystemError,
};

struct WorktreeResult {
    WorktreeResultCode code = WorktreeResultCode::InvalidArgument;
    std::filesystem::path path;
    std::string branch;
    std::string detail;
    std::vector<WorktreeEntry> entries;
};

// 一场交互会话只管理自己 /worktree new 出来的一棵树。这样 /exit remove
// 不会误删用户原本已有的 worktree；keep/remove 后都会回到 new 前的目录。
class WorktreeSession {
public:
    explicit WorktreeSession(GitRunner runner = {});

    WorktreeResult Create(const std::string& requested_name);
    WorktreeResult List() const;
    WorktreeResult Exit(const std::string& mode);
    WorktreeResult ConfirmRemove();

private:
    WorktreeResult FindRepository(const std::filesystem::path& from) const;
    WorktreeResult RemoveNow();
    GitCommandResult RunGit(const std::filesystem::path& working_directory,
                            std::vector<std::string> args) const;

    GitRunner runner_;
    bool active_ = false;
    bool remove_pending_ = false;
    std::filesystem::path original_directory_;
    std::filesystem::path repository_root_;
    std::filesystem::path worktree_path_;
    std::string branch_;
};

}  // namespace lubancode::cli
