// Journal writer 与验账(P0 新轨迹记录单 §8.3/§7.4)。
//
// 一份 JSONL 一只 writer(单一写者);本件只管机械的追加、耐久与验账:
//   - append 一行(canonical JSON + '\n',二进制口,跨平台同字节);
//   - Durability 三档:Buffered 只入缓冲,ProcessCrash fflush,PowerLoss
//     FlushFileBuffers/fsync;
//   - hash chain:event_hash = SHA256(prev_hash || canonical(无 event_hash)),
//     首枚 prev_hash 为 64 个 '0';
//   - 关柄后算整文件 journal_sha256(§8.3:不能写进文件自身,否则循环);
//   - Verify:逐行 canonical round-trip、schema、seq 连续、链衔接、
//     尾行截断明报。
//
// seq 发号与状态机硬约束在 recorder.hpp;本件不认事件语义。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "trajectory/event.hpp"

namespace lubancode::trajectory {

// 链头:首枚事件的前置 hash(§8.3 的链锚,写进 v1 合同)。
inline constexpr std::string_view kGenesisHash =
    "0000000000000000000000000000000000000000000000000000000000000000";

// event_hash = SHA256(prev_hash || canonical_event_without_event_hash)。
// 输入两段 hex 字符串与"不含 event_hash 键"的 canonical JSON 文本。
std::string ComputeEventHash(std::string_view prev_hash,
                             std::string_view canonical_event_without_event_hash);

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

    // 追加一行(不带换行;本件补 '\n')并按档落稳。写失败 false,此后句柄
    // 视为 broken,调用方应停止提交并按 §7.4 收口。
    bool AppendLine(std::string_view line, Durability durability);

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
