// /worktree 与模型侧 worktree 工具共用的纯逻辑和 Git 编排。终端输入、
// 翻译、确认提示留在 interactive_session,这里因此能拿假 GitRunner 做单测,
// 不必真的建仓库。
//
// 骨架拆解反弹·问题 6 自 src/cli/ 迁来:这是领域逻辑(git 房务),不是
// 展示层代码,住 runtime/ 名正言顺;target 仍编进 engine(engine 的
// tools 吃它,runtime target 反向链 engine 收不得)。namespace 沿用
// lubancode::cli 旧名不改——二十来个消费方的 cli:: 前缀不动,纯搬家
// 不夹带改名。
//
// 0.27.x 起 besides /worktree 的三件事也住在这里(模型侧工具薄壳在
// tools/worktree_tool,子代理房务在 tools/agent_tool,都只调这里的函数):
//   - 基准:Enter 支持 fresh(远端默认分支,fetch 5 秒封顶失败回落本地
//     缓存 ref,再不行回落 HEAD)/ head(当前 HEAD)两种;
//   - 房务:验明正身(VerifyWorktreeIdentity)、干净与否(WorktreeClean)、
//     lock/unlock、.worktreeinclude 拷贝、Windows 安全删树(SafeRemoveTree,
//     reparse point 只删链接不追删)、陈房清扫(CleanStaleAgentWorktrees)。

#pragma once

#include <chrono>
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

// 用户给的名字同时进目录名、分支名。只收一段保守的 ASCII,免得 ../、
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
    int timeout_ms = 0;  // 0 = 默认 120 秒;fresh 基准的补 fetch 用 5000 封顶
};

// 让 Git 调用可替身:生产路径走 git -C,测试只需喂返回值。
using GitRunner = std::function<GitCommandResult(const GitCommand&)>;

// status panel 用的轻量 Git 摘要。普通分支返回短名;游离 HEAD 返回
// "detached@<短哈希>";不在仓库、git 不可用或查询失败时返回空串。
// runner 可替换,单测不必真的起 git。
std::string CurrentGitBranch(const std::filesystem::path& working_directory,
                             GitRunner runner = {});

struct WorktreeEntry {
    std::filesystem::path path;
    std::string branch;
    bool detached = false;
    bool locked = false;  // porcelain 里带 locked 行
};

enum class WorktreeResultCode {
    Created,
    Listed,
    Kept,
    Removed,
    NeedsRemoveConfirmation,
    // 进 .lubancode/worktrees 之外的已有房:先要用户点头(硬安全线,确认
    // 档压不住)。detail 带房路径;点头后带 confirmed_outside=true 重进。
    NeedsUserConfirmation,
    // 验明正身没过:这路径不是(或不再是)主仓名下独立的 worktree。
    VerificationFailed,
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

// ---------------------------------------------------------------------------
// 房务的自由函数(WorktreeSession 与 agent_tool 的子代理房共用)
// ---------------------------------------------------------------------------

// 房干不干净:git status --porcelain 输出为空即干净。runner 可替身。
bool WorktreeClean(const std::filesystem::path& worktree_path, GitRunner runner = {});

// 验明正身:path 是不是 main_root 名下一间独立的 worktree——有 .git 文件、
// 指向 main_root/.git/worktrees/<x>、git 的反向登记(gitdir 文件)也对得
// 上。.git 是目录(主仓本体/嵌套完整仓)、指向别家仓库、core.worktree 改
// 道、登记对不上,都算马甲,返回拒绝原因;nullopt = 通过。
// 纯文件系统读取,不跑 git,单测直接在临时目录里摆形状。
std::optional<std::string> VerifyWorktreeIdentity(const std::filesystem::path& path,
                                                  const std::filesystem::path& main_root);

// fresh 基准:origin/HEAD 指的远端默认分支。距上次 fetch 超 24 小时先补
// 一次 fetch(5 秒封顶,失败继续用本地缓存);最终拿不到远端信息就回落
// "HEAD"。返回值直接喂 git worktree add 的起点参数。
std::string ResolveFreshBaseRef(const std::filesystem::path& repository_root, GitRunner runner = {});

// 跑着上锁 / 收工解锁(git worktree lock/unlock)。reason 进 lock 的备注。
bool LockWorktree(const std::filesystem::path& worktree_path, const std::string& reason, GitRunner runner = {});
bool UnlockWorktree(const std::filesystem::path& worktree_path, GitRunner runner = {});

// .worktreeinclude(仓库根,gitignore 语法):把"匹配它又被 gitignore"的
// 文件从主树拷进新房。tracked 文件永不拷。返回拷了的相对路径清单;没有
// include 文件给空表。runner 用于 git check-ignore 判定。
std::vector<std::filesystem::path> CopyWorktreeInclude(const std::filesystem::path& repository_root,
                                                       const std::filesystem::path& target, GitRunner runner = {});

// gitignore 语法的极简匹配器(glob:*、**、?,尾 / 目录锚定,头 / 仓库根
// 锚定,! 取反——include 那点用法足够,不做全套)。pattern 相对仓库根;
// candidate 是仓库根下的相对路径(正斜杠)。最后一条命中的规则说了算,
// 全都没碰上算不匹配。
bool WorktreeIncludeMatches(std::string_view pattern, std::string_view candidate, bool candidate_is_dir);

// Windows 安全删树:remove_all 前逐项验 reparse point(junction/目录符号
// 链接),链接只删自身、绝不追进指向的目录。POSIX 下退 remove_all
// (symlink 本来就不追)。出错返回非空 error message。
std::optional<std::string> SafeRemoveTree(const std::filesystem::path& path);

// 主仓名下全部 worktree(porcelain 解析,含 locked 标记)。
std::vector<WorktreeEntry> ListWorktrees(const std::filesystem::path& repository_root, GitRunner runner = {});

// from 所在仓库的根(rev-parse --show-toplevel);不在仓库/查询失败给
// nullopt。启动时的陈房清扫用。
std::optional<std::filesystem::path> FindRepositoryRoot(const std::filesystem::path& from, GitRunner runner = {});

// ---------------------------------------------------------------------------
// 子代理的房(agent- 前缀,命名规约是清扫的边界:只清 agent- 的房,
// 用户手起的房永不碰)
// ---------------------------------------------------------------------------

struct AgentWorktree {
    bool ok = false;
    std::string error;                    // !ok 时的原因
    std::filesystem::path repo_root;      // 主 checkout 根
    std::filesystem::path room_path;      // 房路径
    std::string name;                     // 房名(agent-xxxx)
    std::string branch;                   // worktree/agent-xxxx
    // ---- 基线账(派工单 §三:基线=派工瞬间的调用者 HEAD,不解析 origin)----
    std::string base_ref;                 // 冻结时调用者所在分支短名;detached 给 "(detached)"
    std::string base_commit;              // 冻结的调用者 HEAD 全哈希(建房起点)
    std::string actual_head;              // 建成后房的实际 HEAD;与 base_commit 对不上即拆房报错
    std::string caller_note;              // 给调用方看的基线附言(未提交改动明示等)
};

// 冻结调用者 HEAD(派工单 §三):working_directory 里 rev-parse 出全哈希,
// symbolic-ref 出分支短名(detached 给 "(detached)")。拿不到 commit 给空串,
// 调用方据此明拒——绝不回落 origin 默认分支。
struct FrozenWorktreeBase {
    std::string commit;
    std::string ref;
};
FrozenWorktreeBase FreezeWorktreeBase(const std::filesystem::path& working_directory, GitRunner runner = {});

// 给隔离子代理建房:名字 agent-<随机>,基准=冻结的调用者提交(base_commit,
// 传空自动冻结 repository_root 的 HEAD——测试/旧调用方用),建成后拷
// .worktreeinclude 并上锁(reason 记来源,跑着的房不怕并发清扫误删)。
// 三者对账:actual_head != base_commit 时当场拆房报错,不让子任务在错
// 基线上静默开工。房区固定在 .lubancode/worktrees 之下,无须用户确认。
AgentWorktree CreateAgentWorktree(const std::filesystem::path& repository_root, const std::string& base_commit,
                                  const std::string& base_ref, GitRunner runner = {});
// 旧签名(基准冻结在调用当口的仓库 HEAD):等价于先 FreezeWorktreeBase
// 再进上面的主口。
AgentWorktree CreateAgentWorktree(const std::filesystem::path& repository_root, GitRunner runner = {});

// 收工房务:解锁;房干净 → 删房删分支(removed=true);有改动 → 留着,
// note 给模型看的附言(房路径与分支,让主代理或用户后续去收)。
struct AgentWorktreeFinish {
    bool removed = false;
    std::string note;
};
AgentWorktreeFinish FinishAgentWorktree(const std::filesystem::path& repository_root,
                                        const std::filesystem::path& room_path, const std::string& branch,
                                        GitRunner runner = {});

// 陈房清扫:只扫 .lubancode/worktrees 下 agent- 前缀、修改时间早于
// now - max_age 的房。锁着的先解锁(被杀会话留下的;用户手上的锁只在
// 非 agent 房上,永不碰);有活(未提交改动,或分支上有别的本地分支
// 没有的提交)的跳过;干净且无自有提交的删房删分支。
struct StaleAgentWorktreeCleanup {
    int removed = 0;
    int kept_dirty = 0;   // 有活留下的
    int kept_fresh = 0;   // 还没到岁数的
};
StaleAgentWorktreeCleanup CleanStaleAgentWorktrees(const std::filesystem::path& repository_root,
                                                   std::chrono::hours max_age, GitRunner runner = {});

// 一场交互会话只管理自己经手的一棵树(/worktree new 或模型 worktree
// enter)。这样 /exit remove 不会误删用户原本已有的 worktree;keep/remove
// 后都会回到进房前的目录。主代理 enter 走 chdir(整场会话一起搬,跟
// 用户敲 /worktree 一个语义);子代理隔离不走这里(见 tools/agent_tool)。
class WorktreeSession {
public:
    explicit WorktreeSession(GitRunner runner = {});

    WorktreeResult Create(const std::string& requested_name);
    // 模型侧 worktree enter:建房或进已有房。
    //   name_or_path:名字(生成 .lubancode/worktrees/<名字>)、绝对路径
    //   (匹配已有 worktree)、或空(自动生成名字新建)。
    //   base:"fresh"(缺省,远端默认分支)|"head"。
    //   confirmed_outside:进 .lubancode/worktrees 之外的已有房,先拿
    //   NeedsUserConfirmation 回去;用户点头后带 true 重进。
    WorktreeResult Enter(const std::string& name_or_path, const std::string& base = "fresh",
                         bool confirmed_outside = false);
    // 恢复/回搬专用:按既有路径进房(先验明正身;失败返回 VerificationFailed)。
    WorktreeResult EnterByPath(const std::filesystem::path& worktree_path);
    WorktreeResult List() const;
    WorktreeResult Status() const;
    WorktreeResult Exit(const std::string& mode);
    WorktreeResult ConfirmRemove();

    bool active() const { return active_; }
    const std::filesystem::path& active_path() const { return worktree_path_; }
    const std::string& active_branch() const { return branch_; }
    // 状态行 WT <名字> 用;没住房给空串。
    std::string active_name() const;

    // 进房/出房的通知钩子(0.27.x 隔离三道闸用):entered=true 时 worktree
    // 是房路径、main_root 是主 checkout;entered=false 是刚出房。app 层拿它
    // 把隔离范围压进/弹出 tools 层的范围栈;不设则零影响(纯 cli 用法,
    // 单测照旧)。
    void SetScopeHook(std::function<void(bool entered, const std::filesystem::path& worktree,
                                         const std::filesystem::path& main_root)>
                         hook) {
        scope_hook_ = std::move(hook);
    }

private:
    WorktreeResult FindRepository(const std::filesystem::path& from) const;
    WorktreeResult CreateRoom(const std::filesystem::path& repository_root, const std::string& name,
                              const std::string& base_ref);
    WorktreeResult EnterRoom(const std::filesystem::path& repository_root, const std::filesystem::path& target,
                             const std::string& base);
    WorktreeResult RemoveNow();
    GitCommandResult RunGit(const std::filesystem::path& working_directory,
                            std::vector<std::string> args) const;

    GitRunner runner_;
    std::function<void(bool entered, const std::filesystem::path& worktree,
                       const std::filesystem::path& main_root)>
        scope_hook_;
    bool active_ = false;
    bool remove_pending_ = false;
    std::filesystem::path original_directory_;
    std::filesystem::path repository_root_;
    std::filesystem::path worktree_path_;
    std::string branch_;
};

}  // namespace lubancode::cli
