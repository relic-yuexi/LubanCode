// Journal writer 与验账(P0 新轨迹记录单 §8.3/§7.4)。
//
// 一份 JSONL 一只 writer(单一写者);本件只管机械的追加、耐久与验账:
//   - append 一行(canonical JSON + '\n',二进制口,跨平台同字节);
//   - Durability 三档:原实现各档均 fflush,PowerLoss 再
//     FlushFileBuffers/fsync(只确认文件,不确认目录名);
//   - hash chain:event_hash = SHA256(prev_hash || canonical(无 event_hash)),
//     首枚 prev_hash 为 64 个 '0';
//   - 关柄后算整文件 journal_sha256(§8.3:不能写进文件自身,否则循环);
//   - Verify:逐行 canonical round-trip、schema、seq 连续、链衔接、
//     尾行截断明报。
//
// seq 发号与状态机硬约束在 recorder.hpp;本件不认事件语义。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trajectory/event.hpp"

namespace lubancode::trajectory {

// 链头:首枚事件的前置 hash(§8.3 的链锚,写进 v1 合同)。
inline constexpr std::string_view kGenesisHash =
    "0000000000000000000000000000000000000000000000000000000000000000";

// Internal native witness. Capture follows the existing journal path, keeps the
// opened regular object alive and owns bytes. Providers cannot mint its identity.
class JournalFileAnchor {
public:
    struct Capture;
    JournalFileAnchor(JournalFileAnchor&&) noexcept;
    JournalFileAnchor& operator=(JournalFileAnchor&&) noexcept;
    ~JournalFileAnchor();
    JournalFileAnchor(const JournalFileAnchor&) = delete;
    JournalFileAnchor& operator=(const JournalFileAnchor&) = delete;
    static std::expected<Capture, std::string> ReadExisting(
        const std::filesystem::path& path, std::optional<std::size_t> max_bytes,
        bool follow_path = true);
    std::expected<void, std::string> Close();
private:
    struct Impl;
    explicit JournalFileAnchor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class JournalWriter;
};

struct JournalFileAnchor::Capture {
    std::string bytes;
    std::shared_ptr<JournalFileAnchor> anchor;
};

// event_hash = SHA256(prev_hash || canonical_event_without_event_hash)。
// 输入两段 hex 字符串与"不含 event_hash 键"的 canonical JSON 文本。
std::string ComputeEventHash(std::string_view prev_hash,
                             std::string_view canonical_event_without_event_hash);

enum class JournalAppendStatus { RejectedBeforeIO, Committed, Unconfirmed };
enum class JournalBeforeIoReason { None, Broken, Closed, EmptyLine };
enum class JournalNativeStage { None, BodyWrite, NewlineWrite, Flush, FileHandle, FileSync, Close };
enum class JournalNativeErrorDomain { None, Errno, Win32 };

// Fixed owned observations. Native success/error and controlled uncertainty are
// independent: an injected boundary does not rewrite the native return value.
struct JournalNativeIoResult {
    JournalNativeStage stage = JournalNativeStage::None;
    bool attempted = false;
    bool succeeded = false;
    std::size_t requested_bytes = 0;
    std::size_t written_bytes = 0;
    int native_return = 0; // fflush/fsync/FlushFileBuffers/fclose; writes use written_bytes.
    JournalNativeErrorDomain error_domain = JournalNativeErrorDomain::None;
    std::int64_t native_error = 0;
    bool injected_unconfirmed = false;
};

// Internal, explicit native-boundary test seam. Called after the real I/O;
// true injects uncertainty, never a fake native result. No writer/FILE borrow.
// Trusted synchronous probes must not reenter the writer, block or throw.
class JournalNativeIoProbe {
public:
    virtual ~JournalNativeIoProbe() = default;
    virtual bool After(const JournalNativeIoResult& actual) noexcept = 0;
};

struct JournalAppendReceipt {
    JournalAppendStatus status = JournalAppendStatus::RejectedBeforeIO;
    Durability requested_durability = Durability::Buffered;
    std::optional<Durability> confirmed_durability;
    JournalBeforeIoReason rejection = JournalBeforeIoReason::None;
    JournalNativeIoResult body, newline, flush, file_sync;
    std::optional<JournalNativeIoResult> failure;
    std::uint64_t line_count = 0;
};

struct JournalCloseReceipt {
    enum class Status { NoOpenHandle, Closed, Unconfirmed };
    Status status = Status::NoOpenHandle;
    std::optional<JournalNativeIoResult> native;
    bool broken_before = false;
    bool broken_after = false;
    bool ok() const noexcept { return !broken_after; } // The original bool Close contract.
};

class JournalWriter {
public:
    enum class OpenMode {
        CreateNew,  // §3.10:原子占住目标 JSONL,已存在即失败
        Append,     // 恢复场景:打开已有文件续写(调用方自证单写者)
    };

    JournalWriter() = default;
    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&& other) noexcept;
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    ~JournalWriter();

    static std::expected<JournalWriter, std::string> Open(const std::filesystem::path& path,
                                                          OpenMode mode);
    static std::expected<JournalWriter, std::string> OpenWithNativeIoProbe(
        const std::filesystem::path& path, OpenMode mode, std::shared_ptr<JournalNativeIoProbe> probe);

    // No create, same opened object as Capture, complete owned prefix and EOF.
    // The returned stream retains native append mode for the unchanged writer.
    static std::expected<JournalWriter, std::string> OpenExistingVerified(
        const std::filesystem::path& path, std::string_view prefix,
        const JournalFileAnchor& anchor);

    // 追加一行(不带换行;本件补 '\n')并按档落稳。写失败 false,此后句柄
    // 视为 broken,调用方应停止提交并按 §7.4 收口。
    bool AppendLine(std::string_view line, Durability durability);
    JournalAppendReceipt AppendLineDetailed(std::string_view line, Durability durability) noexcept;

    // 关掉文件句柄,保留路径/计数供只读查询。可重复调用;关闭或既有写入
    // 出错时返回 false。关闭后 AppendLine 拒绝,不等对象析构才释放文件。
    bool Close();
    // Repeated calls return the cached first witness, not a new native close.
    JournalCloseReceipt CloseDetailed() noexcept;
    std::optional<JournalAppendReceipt> first_unconfirmed_append() const noexcept {
        return first_unconfirmed_append_;
    }
    std::optional<JournalCloseReceipt> first_close_receipt() const noexcept { return first_close_receipt_; }

    const std::filesystem::path& path() const { return path_; }
    std::uint64_t line_count() const { return line_count_; }
    bool broken() const { return broken_; }

    // 关柄后算整文件 SHA-256(§8.3 journal_sha256;不写进文件自身)。
    static std::expected<std::string, std::string> ComputeJournalSha256(
        const std::filesystem::path& path);

private:
    std::filesystem::path path_;
    std::FILE* file_ = nullptr;  // 二进制口,'\n' 不经文本模式翻译
    std::uint64_t line_count_ = 0;
    bool broken_ = false;
    std::optional<JournalAppendReceipt> first_unconfirmed_append_;
    std::optional<JournalCloseReceipt> first_close_receipt_;
    std::shared_ptr<JournalNativeIoProbe> native_probe_;
};

// 验账报告:verify 只认链与 schema,不重放状态机(那是 P0-3 validator 的活)。
struct JournalVerifyReport {
    bool ok = false;
    bool truncated_tail = false;  // 尾行缺 '\n'(崩溃截断;§16.3 明报)
    std::uint64_t events = 0;     // 校验通过的事件数(截断尾行不计)
    std::string first_event_hash;
    std::string last_event_hash;
    std::string error_code;
    std::string message;
};

// 逐行验一份 JSONL:JSON 可解析、canonical round-trip 字节一致、
// ParseAndValidateEventLine 全过、seq 从 1 连续、prev_hash 衔接、
// event_hash 重算对得上。尾行无 '\n' 记 truncated_tail 且 ok=false
// (run 判 incomplete,不伪造终态)。
JournalVerifyReport VerifyJournalFile(const std::filesystem::path& path);

// 读回全部行(去掉换行)。空行跳过。文件打不开给 nullopt。
std::optional<std::vector<std::string>> ReadJournalLines(const std::filesystem::path& path);

}  // namespace lubancode::trajectory
