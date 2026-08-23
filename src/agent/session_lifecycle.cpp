// session_lifecycle.hpp 的实现:搬与删的磁盘薄壳。
//
// MSVC/libc++ 盲区(单子开工前实锤的五雷,这里全躲):
//   - rename/remove 一律走 std::error_code 形态,不抛异常;
//   - 搬/删之前先问 active_file_(调用方注入的 flush/close 回调),
//     Windows 的 append 句柄不关就动文件必吃 sharing violation;
//   - canonical 用 error_code 形态,失败(文件不在)按 lexically_normal
//     收口,不炸;
//   - 路径比较两边都过 weakly_canonical(error_code 形态),符号链接
//     绕出根的在这里现形。

#include "agent/session_lifecycle.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace lubancode::agent {

namespace {

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// UTF-8 文件名段 -> path:与 Utf8Path 同款转换。Windows 上直接拿
// std::string 拼路径(operator/ 收窄串按 ANSI 代码页解码),"甲"的 UTF-8
// 三字节会被解成乱码,exists/remove 全找错名——中文 slug 的会话档在
// Windows 上删不掉的根因。文件名与目录一样,必须走 u8string。
std::filesystem::path Utf8Name(const std::string& utf8_name) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8_name.data()), utf8_name.size()));
}

// canonical 的稳妥版:文件在就 weakly_canonical;不在(还没建)退
// lexically_normal。两种都不许抛——error_code 形态。
std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return path.lexically_normal();
}

// 短 id:完整 id 的日期时刻段("yyyymmdd-HHMMSS" 那一截,第一杠之前加
// 时刻)。引用消歧失败列账用——别把整条 slug 都糊给人。
std::string ShortId(const std::string& id) {
    // id 形如 "yyyymmdd-HHMMSS-slug"。短 id 取前 15 字(日期+杠+时刻);
    // 认不出这个形状就原样(老档命名不合规矩时至少能认)。
    if (id.size() >= 15 && id[8] == '-') {
        return id.substr(0, 15);
    }
    return id;
}

}  // namespace

// ---------------------------------------------------------------------------
// 引用解析(纯函数)
// ---------------------------------------------------------------------------

std::optional<std::vector<SessionRefCandidate>> ResolveSessionRef(const std::vector<SessionRefCandidate>& candidates,
                                                                  const std::string& ref, bool& ambiguous) {
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
    // 2. id 前缀。
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
    // 3. 标题精确相等(唯一命中才收;标题为空不参)。
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
    // 4. 都不是。
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 磁盘薄壳
// ---------------------------------------------------------------------------

SessionLifecycle::SessionLifecycle(std::string sessions_dir) : sessions_dir_(std::move(sessions_dir)) {}

void SessionLifecycle::SetActiveFile(std::string active_file,
                                     std::function<bool(const std::string&)> flush_close) {
    active_file_ = std::move(active_file);
    flush_close_ = std::move(flush_close);
}

bool SessionLifecycle::PathInsideRoot(const std::filesystem::path& path,
                                      const std::filesystem::path& root_dir) const {
    const std::filesystem::path normalized = NormalizedPath(path);
    const std::filesystem::path root = NormalizedPath(root_dir);
    // 越界判定:normalized 必须是 root 自己或它的孩子。weakly_canonical
    // 解开符号链接——链到根外的路径在这里现形,拒绝。
    if (normalized == root) {
        return false;  // 目标是目录本身,不是里头的一场会话
    }
    auto parent = normalized.parent_path();
    while (parent.has_relative_path() || !parent.empty()) {
        if (parent == root) {
            return true;
        }
        if (parent == parent.root_path()) {
            break;
        }
        parent = parent.parent_path();
    }
    return false;
}

std::string SessionLifecycle::PathOf(const std::string& session_id, bool& in_archive) const {
    namespace fs = std::filesystem;
    in_archive = false;
    // id 里不许有路径分隔符/点点——拼路径前先验,别让人拿 "../../etc" 冒充。
    if (session_id.find('/') != std::string::npos || session_id.find('\\') != std::string::npos ||
        session_id == "." || session_id == "..") {
        return std::string();
    }
    std::error_code ec;
    const fs::path name = Utf8Name(session_id + ".jsonl");
    const fs::path root_file = Utf8Path(sessions_dir_) / name;
    if (fs::exists(root_file, ec) && !ec) {
        return PathToUtf8(root_file);
    }
    const fs::path archive_file = Utf8Path(sessions_dir_) / "archive" / name;
    if (fs::exists(archive_file, ec) && !ec) {
        in_archive = true;
        return PathToUtf8(archive_file);
    }
    return std::string();
}

SessionLifecycleResult SessionLifecycle::ArchiveSession(const std::string& session_id) {
    SessionLifecycleResult result;
    result.session_id = session_id;
    namespace fs = std::filesystem;

    bool in_archive = false;
    const std::string path = PathOf(session_id, in_archive);
    if (path.empty()) {
        result.code = SessionLifecycleCode::NotFound;
        return result;
    }
    if (in_archive) {
        // 已归档:幂等成功(单子没说重复归档要报错;不搬第二次,原样)。
        result.file_path = path;
        return result;
    }

    // 活动句柄:目标是当前会话时先收柄(Windows 的 sharing violation 闸)。
    if (!active_file_.empty() && NormalizedPath(Utf8Path(active_file_)) == NormalizedPath(Utf8Path(path))) {
        if (flush_close_ == nullptr || !flush_close_(active_file_)) {
            result.code = SessionLifecycleCode::IoError;
            return result;
        }
    }

    const fs::path source = Utf8Path(path);
    const fs::path archive_dir = Utf8Path(sessions_dir_) / "archive";
    const fs::path target = archive_dir / Utf8Name(session_id + ".jsonl");
    // 逐枚追踪单第 5 期(retention 联动):context 目录(artifact 仓)与
    // jsonl 并排搬进 archive——归档的语义是"整套带走",blob 留在原地铁定
    // 变孤儿。搬不动只在 detail 里记账,归档本体不受阻(jsonl 已挪成,
    // 回头搬回去反而不一致);unarchive 同款对称。

    // 路径校验:源在根内、后缀对。
    if (!PathInsideRoot(source, Utf8Path(sessions_dir_)) || source.extension() != ".jsonl") {
        result.code = SessionLifecycleCode::PathOutsideRoot;
        return result;
    }
    // 目标同名已存在:拒绝覆盖(半路失败也不能两头各剩半份)。
    std::error_code ec;
    if (fs::exists(target, ec) && !ec) {
        result.code = SessionLifecycleCode::TargetExists;
        return result;
    }
    fs::create_directories(archive_dir, ec);
    if (ec) {
        result.code = SessionLifecycleCode::IoError;
        return result;
    }
    // 字节原样搬:rename 原子,失败源文件在原地,原账可用。
    fs::rename(source, target, ec);
    if (ec) {
        result.code = SessionLifecycleCode::IoError;
        return result;
    }
    // context 目录(artifact 仓)随迁:与 jsonl 同名并排,搬进
    // archive/<session-id>/context。失败只记账(jsonl 已挪成,归档本体
    // 成立);目录不在(没开过仓的会话)空过。
    {
        const fs::path context_source = source.parent_path() / Utf8Name(session_id) / "context";
        const fs::path context_target = archive_dir / Utf8Name(session_id) / "context";
        std::error_code context_ec;
        if (fs::exists(context_source, context_ec) && !context_ec) {
            fs::create_directories(context_target.parent_path(), context_ec);
            fs::rename(context_source, context_target, context_ec);
            if (context_ec) {
                result.detail = "context 目录随迁失败(" + context_ec.message() +
                                "),artifact 留在原地,归档本体不受阻";
            }
        }
    }
    result.file_path = PathToUtf8(target);
    return result;
}

SessionLifecycleResult SessionLifecycle::UnarchiveSession(const std::string& session_id) {
    SessionLifecycleResult result;
    result.session_id = session_id;
    namespace fs = std::filesystem;

    bool in_archive = false;
    const std::string path = PathOf(session_id, in_archive);
    if (path.empty()) {
        result.code = SessionLifecycleCode::NotFound;
        return result;
    }
    if (!in_archive) {
        // 已在根里:幂等成功。
        result.file_path = path;
        return result;
    }

    const fs::path source = Utf8Path(path);
    const fs::path target = Utf8Path(sessions_dir_) / Utf8Name(session_id + ".jsonl");
    if (!PathInsideRoot(source, Utf8Path(sessions_dir_) / "archive") || source.extension() != ".jsonl") {
        result.code = SessionLifecycleCode::PathOutsideRoot;
        return result;
    }
    std::error_code ec;
    if (fs::exists(target, ec) && !ec) {
        result.code = SessionLifecycleCode::TargetExists;
        return result;
    }
    fs::rename(source, target, ec);
    if (ec) {
        result.code = SessionLifecycleCode::IoError;
        return result;
    }
    // context 目录(artifact 仓)随迁回根(逐枚追踪单 retention 联动,
    // 与 ArchiveSession 对称):失败只记 detail,本体不受阻。
    {
        const fs::path context_source = source.parent_path() / Utf8Name(session_id) / "context";
        const fs::path context_target = target.parent_path() / Utf8Name(session_id) / "context";
        std::error_code context_ec;
        if (fs::exists(context_source, context_ec) && !context_ec) {
            fs::create_directories(context_target.parent_path(), context_ec);
            fs::rename(context_source, context_target, context_ec);
            if (context_ec) {
                result.detail = "context 目录随迁失败(" + context_ec.message() +
                                "),artifact 留在 archive,本体不受阻";
            }
        }
    }
    result.file_path = PathToUtf8(target);
    return result;
}

SessionLifecycleResult SessionLifecycle::DeleteSession(const std::string& session_id, bool confirmed) {
    SessionLifecycleResult result;
    result.session_id = session_id;
    if (!confirmed) {
        result.code = SessionLifecycleCode::ConfirmationRequired;
        return result;
    }
    namespace fs = std::filesystem;

    bool in_archive = false;
    const std::string path = PathOf(session_id, in_archive);
    if (path.empty()) {
        result.code = SessionLifecycleCode::NotFound;
        return result;
    }
    const fs::path target = Utf8Path(path);
    const fs::path root = in_archive ? Utf8Path(sessions_dir_) / "archive" : Utf8Path(sessions_dir_);
    if (!PathInsideRoot(target, root) || target.extension() != ".jsonl") {
        result.code = SessionLifecycleCode::PathOutsideRoot;
        return result;
    }
    // 活动句柄先收(会话内 /delete 走到这里时调用方已 Reset;顶层命令
    // 没有活动句柄,这里空过)。
    if (!active_file_.empty() && NormalizedPath(Utf8Path(active_file_)) == NormalizedPath(target)) {
        if (flush_close_ == nullptr || !flush_close_(active_file_)) {
            result.code = SessionLifecycleCode::IoError;
            return result;
        }
    }
    std::error_code ec;
    fs::remove(target, ec);
    if (ec) {
        result.code = SessionLifecycleCode::IoError;
        return result;
    }
    // 逐枚追踪单第 5 期(retention 联动):会话删了,它的 context 目录
    // (artifact 仓:blob/chunks/index,与 <id>.jsonl 并排)一并删——
    // 单子"retention、artifact 清理、session 删除联动"。删不动只记
    // 账(孤儿 blob 由仓的清理路兜),不把删除判成失败:会话本体已删
    // 成,回滚没有意义。
    const fs::path context_dir = target.parent_path() / Utf8Name(session_id) / "context";
    std::error_code context_ec;
    if (fs::exists(context_dir, context_ec)) {
        fs::remove_all(context_dir, context_ec);
        if (context_ec) {
            result.detail = "context 目录删除失败(" + context_ec.message() +
                            "),孤儿 artifact 留待清理路兜底";
        }
    }
    result.file_path = path;  // 删除前的旧路径(报账用)
    return result;
}

std::vector<SessionRefCandidate> SessionLifecycle::ListActive() {
    namespace fs = std::filesystem;
    std::vector<SessionRefCandidate> out;
    std::error_code ec;
    fs::directory_iterator it(Utf8Path(sessions_dir_), ec);
    if (ec) {
        return out;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl") {
            continue;
        }
        SessionRefCandidate candidate;
        candidate.id = PathToUtf8(entry.path().stem());
        candidate.file_path = PathToUtf8(entry.path());
        out.push_back(std::move(candidate));
    }
    std::sort(out.begin(), out.end(),
              [](const SessionRefCandidate& a, const SessionRefCandidate& b) { return a.id > b.id; });
    return out;
}

std::vector<SessionRefCandidate> SessionLifecycle::ListArchived() {
    namespace fs = std::filesystem;
    std::vector<SessionRefCandidate> out;
    std::error_code ec;
    fs::directory_iterator it(Utf8Path(sessions_dir_) / "archive", ec);
    if (ec) {
        return out;  // archive 目录还没立:一场都没归档过
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl") {
            continue;
        }
        SessionRefCandidate candidate;
        candidate.id = PathToUtf8(entry.path().stem());
        candidate.file_path = PathToUtf8(entry.path());
        out.push_back(std::move(candidate));
    }
    std::sort(out.begin(), out.end(),
              [](const SessionRefCandidate& a, const SessionRefCandidate& b) { return a.id > b.id; });
    return out;
}

}  // namespace lubancode::agent
