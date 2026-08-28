// 自进化闭环阶段 1:观察账——只追加的 JSONL 与 rejected 去重账。
//
// 落盘形状(README"观察账"节与此处同源):
//   ~/.lubancode/evolution/observations/
//     observations.jsonl   一行一条 EvolutionObservation(schema 1),只追加
//     rejected.jsonl       一行一条被拒 fingerprint,只追加
//
// 规矩:
//   - 只追加:Append 不改旧行;读取时坏行/半截行(崩溃截断)跳过不废整账
//     (与 eval-results.jsonl、各事件账同约定)。
//   - 幂等:观察 id 由 source+source_id 决定,重采同一条账 id 相同,
//     Append 遇已存在的 id 直接跳(DuplicateId),不翻倍记账。
//   - rejected 去重:被拒 fingerprint 不再重复进观察账(契约:"拒绝账按
//     fingerprint 去重:内容未变的同款,不再重提,也不再劝装")。Append 前
//     查 rejected 账,命中即 SuppressedRejected。阶段 1 没有 /evolve reject
//     命令,MarkRejected 留给后续阶段与测试把门。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "evolution/observation.hpp"

namespace lubancode::evolution {

// rejected 账的一行。
struct RejectedFingerprint {
    int schema = 1;
    std::string fingerprint;
    std::string reason;
    std::string rejected_at;  // ISO 日期或日期时间;可空
};

class ObservationStore {
public:
    // root 即 observations/ 目录(建不出不报错,Append 时再算账)。
    explicit ObservationStore(std::filesystem::path root_dir);

    const std::filesystem::path& root() const { return root_; }
    const std::filesystem::path& observations_file() const { return observations_file_; }
    const std::filesystem::path& rejected_file() const { return rejected_file_; }

    enum class AppendStatus {
        Appended,            // 落账成功
        DuplicateId,         // 同 id 已在账(重采),不动文件
        SuppressedRejected,  // fingerprint 已被拒,不再进观察账
    };
    // 追加一条(只追加;先读旧账查 id 与 rejected,再 append+flush)。
    // 建目录/开文件失败返回错误。
    std::expected<AppendStatus, std::string> Append(const EvolutionObservation& observation);

    // 记一笔拒绝(只追加 rejected.jsonl)。
    std::expected<void, std::string> MarkRejected(const std::string& fingerprint,
                                                  const std::string& reason);

    bool IsRejected(const std::string& fingerprint) const;
    bool HasId(const std::string& id) const;

    // 读全账(半截行跳过)。目录/文件不存在给空表。
    std::vector<EvolutionObservation> Load() const;
    std::vector<RejectedFingerprint> LoadRejected() const;

    std::optional<EvolutionObservation> Find(const std::string& id) const;

private:
    std::filesystem::path root_;
    std::filesystem::path observations_file_;
    std::filesystem::path rejected_file_;
};

}  // namespace lubancode::evolution
