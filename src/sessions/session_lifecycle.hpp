// SessionLifecycle(会话管理器单第四、五步):会话文件搬与删的唯一收口。
//
// 只管三件事:ArchiveSession / UnarchiveSession / DeleteSession。解析
// 引用、校验根目录、检测当前活动句柄、flush/close、rename/remove、错误
// 码,全聚在这里——UI、slash handler 与 app-server 不直接碰 filesystem。
//
// 目录形状:
//   ~/.lubancode/sessions/<id>.jsonl          活动会话
//   ~/.lubancode/sessions/archive/<id>.jsonl  归档会话
//
// 归档 = 字节原样搬进 archive/ 子目录(rename,同盘原子);反归档 = 搬回
// 根。半路失败原文件可用(rename 失败源文件还在原地,不会两头各剩半份)。
// 删除 = remove。三者都先验路径:canonical 之后必须落在 sessions 根或
// archive 子目录内,后缀必须是 .jsonl,符号链接绕出根一律拒绝。
//
// Windows 句柄账:活动会话的 append 句柄(SessionStore)长期开着,不先
// flush/close 就 rename/remove 会吃 sharing violation。调用方把"当前
// 活动的那一场"经 active_session_file 告诉这里;这里对它先走
// FlushActiveFile(由调用方注入的收柄回调,session_commands 层接
// SessionStore::Reset)。不知道活动句柄是谁(顶层命令,没有会话在跑)
// 就不关——顶层目标本就不该是活动会话。
//
// 结果码(单子"代码边界 SessionLifecycle"一节,稳定不改名):
//   ok / not_found / ambiguous / active_turn / confirmation_required /
//   path_outside_root / target_exists / io_error

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::sessions {

// 生命周期操作的稳定结果码。ok 之外都是拒绝,人话在 message 里。
enum class SessionLifecycleCode {
    Ok,
    NotFound,             // 引用解不出任何一场
    Ambiguous,            // 标题重名,多场命中(不清账,调用方列短 id 叫用户点明)
    ConfirmationRequired, // 删除没带确认(调用方先走确认屏)
    PathOutsideRoot,      // canonical 越界/符号链接绕出根/后缀不对
    TargetExists,         // 归档目标同名已存在(拒绝覆盖)
    ActiveTurn,           // 回合在跑/工具在飞/审批悬着(由调用方判定后回这个码)
    IoError,              // flush/close/rename/remove 失败
};

struct SessionLifecycleResult {
    SessionLifecycleCode code = SessionLifecycleCode::Ok;
    std::string message;    // 人话(空 = 成功);引用消歧失败时列短 id
    // 逐枚追踪单第 5 期(retention 联动):context 目录(artifact 仓)
    // 随迁/随删失败的降级说明。空 = 一起搬/删干净了。本体操作不受阻
    //(jsonl 已挪/删成,回滚没有意义),这行话让调用方能如实告警。
    std::string detail;
    std::string session_id; // 成功时搬/删的那一场
    std::string file_path;  // 成功后的新路径(archive)或删除前的旧路径

    bool ok() const { return code == SessionLifecycleCode::Ok; }
};

// 引用解析的候选账(ambiguous 时给调用方列给人看)。
struct SessionRefCandidate {
    std::string id;
    std::string file_path;
    std::string title;   // 展示用,可为空
};

// 会话引用:id(完整或唯一前缀)或标题。解析顺序:先完整 id,再唯一
// 前缀,再标题唯一命中;重名/多义给 Ambiguous 并列短 id。
// candidates 由调用方从 SessionCatalog 或目录扫描喂进来(这里不读盘,
// 纯函数可单测)。
struct SessionRef {
    std::string id;     // 完整 id
    std::string title;  // 标题(可空 = 按 id 解)
};

// 纯函数:引用 -> 候选里的命中。返回 nullopt = 没有命中(NotFound);
// hits 非空但多场 = Ambiguous(由调用方组装 message)。
// 规则(单子"产品定案四"):
//   1. 引用与某候选 id 完全相等 -> 唯一命中;
//   2. 引用是某候选 id 的前缀且只匹配一场 -> 命中;多场 -> Ambiguous;
//   3. 否则按标题精确相等找:唯一命中 -> 命中;多场 -> Ambiguous;
//   4. 都不是 -> NotFound。
std::optional<std::vector<SessionRefCandidate>> ResolveSessionRef(const std::vector<SessionRefCandidate>& candidates,
                                                                  const std::string& ref, bool& ambiguous);

// ---------------------------------------------------------------------------
// 磁盘薄壳:搬与删
// ---------------------------------------------------------------------------

class SessionLifecycle {
public:
    // sessions_dir:~/.lubancode/sessions(根)。archive 子目录按需建。
    explicit SessionLifecycle(std::string sessions_dir);

    // 归档:根目录里的 <id>.jsonl -> archive/<id>.jsonl。字节原样(rename)。
    // 已在 archive 里给 TargetExists 的反面:Ok 且原样(幂等,不搬第二次)。
    // 目标已存在别的文件 -> TargetExists 拒绝覆盖。
    SessionLifecycleResult ArchiveSession(const std::string& session_id);

    // 反归档:archive/<id>.jsonl -> 根/<id>.jsonl。根里已有同名 -> TargetExists。
    SessionLifecycleResult UnarchiveSession(const std::string& session_id);

    // 永久删除:<id>.jsonl(根或 archive)。confirmed=false 一律
    // ConfirmationRequired,不动盘。只删目标一场;它的 context 目录
    // (artifact 仓)一并删(逐枚追踪单 retention 联动),删不动只记
    // 进 detail,本体不受阻。
    SessionLifecycleResult DeleteSession(const std::string& session_id, bool confirmed);

    // 活动句柄收口(Windows sharing violation 的闸):调用方在 archive/
    // delete 前把"当前 append 句柄还开着的那场"的路径告诉这里,操作前
    // 先调 flush_close 回调(接线层接 SessionStore::Reset——flush+close
    // 一并做了)。回调返回 false 视为 IoError。空路径 = 没有活动会话。
    void SetActiveFile(std::string active_file,
                       std::function<bool(const std::string&)> flush_close);

    // 只读口:根/archive 目录里的 .jsonl 各列一场(candidates 喂
    // ResolveSessionRef 用;title 由调用方从摘要补)。
    std::vector<SessionRefCandidate> ListActive();
    std::vector<SessionRefCandidate> ListArchived();

    const std::string& sessions_dir() const { return sessions_dir_; }

private:
    // id -> 根或 archive 里的真路径。两处都在(不该发生)取根。找不到给空。
    std::string PathOf(const std::string& session_id, bool& in_archive) const;
    // canonical 校验:路径解析后必须真落在 root_dir 之内且后缀 .jsonl,
    // 符号链接绕出根拒绝。root_dir 是 sessions 根或 archive 子目录。
    bool PathInsideRoot(const std::filesystem::path& path, const std::filesystem::path& root_dir) const;

    std::string sessions_dir_;
    std::string active_file_;
    std::function<bool(const std::string&)> flush_close_;
};

}  // namespace lubancode::sessions
