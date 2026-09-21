// SV-09(2026-09-21 架构审查)拆分后 memory 域各成员群(recall_engine/
// topic_store/candidate_store/worker_queue/门面)共用的一把基础小件:
// 路径换算、时间戳、读文件、id/路径校验、单行化、原子写、目录包含判定
// 与目录锁的人话/等锁件。只给 src/memory 里头的 TU 用,不进公共合同
// ——对外形状都在 memory/types.hpp 与 memory/project_memory.hpp。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "memory/project_memory.hpp"  // OwnerLock:锁人话与有界等锁要它

namespace lubancode::memory {

// 主题字段的硬上限(写入校验与候选校验、正文拼装共用一套)。
constexpr std::size_t kMaxTopicBytes = 8 * 1024;
constexpr std::size_t kMaxTitleBytes = 200;
constexpr std::size_t kMaxSummaryBytes = 500;
constexpr std::size_t kMaxKeywords = 16;
constexpr std::size_t kMaxPaths = 24;

// UTF-8 窄串 <-> fs::path(Windows 宽字符边界的唯一换算口)。
std::filesystem::path Utf8Path(const std::string& utf8);
std::string PathUtf8(const std::filesystem::path& path);
std::filesystem::path AbsoluteNormal(const std::filesystem::path& path);

std::string Trim(std::string value);
std::string ReadFile(const std::filesystem::path& path);
std::string ReadBounded(const std::filesystem::path& path, std::size_t max_bytes);
std::string LowerAscii(std::string value);

// FNV-1a 64:去重键与短哈希账本用(内容寻址,不抗碰撞攻击)。
std::uint64_t StableHash(std::string_view value);
std::string HexHash(std::string_view value);

std::string NowIsoUtc();
// job 文件名时间戳:"<millis>-<seq>",进程内自增保唯一。
std::string JobStamp();

// 记忆 id 合法性:字母数字点横线下划线,带 fact./preference./feedback. 前缀。
bool IsValidId(const std::string& id);
// 项目内相对路径(拒绝绝对路径、盘符与 ..)。
bool IsSafeRelativePath(const std::string& raw);

// 压成单行并截到 UTF-8 前缀边界(max_bytes 默认 240)。
std::string OneLine(std::string value, std::size_t max_bytes = 240);

// 统一原子写(审计 P1):平台原子替换,失败清临时件、不动正式件。
std::expected<void, std::string> AtomicWrite(const std::filesystem::path& target,
                                             const std::string& content);

// child 是否落在 parent 之内(双双规范化后逐段比对)。
bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent);

// 有界等锁:活持有者放手要时间,烧完仍撞才回失败;BrokenLock 即刻回。
// memory.lock(跨工作区的用户层 job 会撞同一把)用它,worker.lock 的
// 50x50ms 轮在调用点自备。
OwnerLock::Result AcquireDirLockWithRetry(const std::filesystem::path& dir, OwnerLock* out,
                                          int attempts, int interval_ms);
// 锁取不上的人话:持有者在 → "正由另一个 worker 更新";owner 读不懂 →
// 明报不敢动;其余原样端出来。
std::string ProjectLockRefusal(const OwnerLock::Result& result, const char* what);

}  // namespace lubancode::memory
