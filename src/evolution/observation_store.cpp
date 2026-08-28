// 观察账的磁盘薄壳:append+flush、坏行跳过、rejected 去重。路径窄边界
// (PathToUtf8/Utf8ToPath),其余全是 Serialize/Parse 纯函数的粘合。

#include "evolution/observation_store.hpp"

#include <fstream>
#include <set>

#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"  // WallClockNowMs:拒绝账的时间戳

namespace lubancode::evolution {

namespace {

// 逐行读一个 JSONL 文件,逐行喂 parser;坏行/半截行跳过(崩溃截断的尾巴
// 不废整账)。文件不存在给空。
template <typename T, typename ParseLine>
std::vector<T> ReadJsonl(const std::filesystem::path& file, ParseLine parse_line) {
    std::vector<T> out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return out;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (auto item = parse_line(line); item.has_value()) {
            out.push_back(std::move(*item));
        }
    }
    return out;
}

// append+flush 一行(先补换行)。开文件失败返回错误,调用方如实报。
std::expected<void, std::string> AppendLine(const std::filesystem::path& file, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return std::unexpected("观察账打不开: " + lubancode::platform::PathToUtf8(file));
    }
    out << line << "\n";
    out.flush();
    if (!out.good()) {
        return std::unexpected("观察账写入失败: " + lubancode::platform::PathToUtf8(file));
    }
    return {};
}

}  // namespace

ObservationStore::ObservationStore(std::filesystem::path root_dir)
    : root_(std::move(root_dir)),
      observations_file_(root_ / "observations.jsonl"),
      rejected_file_(root_ / "rejected.jsonl") {}

std::expected<ObservationStore::AppendStatus, std::string> ObservationStore::Append(
    const EvolutionObservation& observation) {
    if (observation.id.empty()) {
        return std::unexpected("观察没有 id,不能落账");
    }
    if (IsRejected(observation.fingerprint)) {
        return AppendStatus::SuppressedRejected;
    }
    if (HasId(observation.id)) {
        return AppendStatus::DuplicateId;
    }
    const auto written = AppendLine(observations_file_, SerializeObservation(observation));
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return AppendStatus::Appended;
}

std::expected<void, std::string> ObservationStore::MarkRejected(const std::string& fingerprint,
                                                                const std::string& reason) {
    if (fingerprint.empty()) {
        return std::unexpected("拒绝账须带 fingerprint");
    }
    nlohmann::json line;
    line["schema"] = 1;
    line["fingerprint"] = fingerprint;
    if (!reason.empty()) {
        line["reason"] = reason;
    }
    const auto now_ms = lubancode::platform::WallClockNowMs();
    const std::string at = FormatEpochMsLocal(now_ms);
    if (!at.empty()) {
        line["rejected_at"] = at;
    }
    return AppendLine(rejected_file_, line.dump());
}

bool ObservationStore::IsRejected(const std::string& fingerprint) const {
    if (fingerprint.empty()) {
        return false;
    }
    for (const RejectedFingerprint& rejected : LoadRejected()) {
        if (rejected.fingerprint == fingerprint) {
            return true;
        }
    }
    return false;
}

bool ObservationStore::HasId(const std::string& id) const {
    if (id.empty()) {
        return false;
    }
    return Find(id).has_value();
}

std::vector<EvolutionObservation> ObservationStore::Load() const {
    return ReadJsonl<EvolutionObservation>(
        observations_file_, [](const std::string& line) { return ParseObservation(line); });
}

std::vector<RejectedFingerprint> ObservationStore::LoadRejected() const {
    return ReadJsonl<RejectedFingerprint>(rejected_file_, [](const std::string& line) {
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) return std::optional<RejectedFingerprint>{};
        if (!j.contains("schema") || !j.at("schema").is_number_integer() ||
            j.at("schema").get<int>() != 1) {
            return std::optional<RejectedFingerprint>{};
        }
        if (!j.contains("fingerprint") || !j.at("fingerprint").is_string()) {
            return std::optional<RejectedFingerprint>{};
        }
        RejectedFingerprint rejected;
        rejected.fingerprint = j.at("fingerprint").get<std::string>();
        if (j.contains("reason") && j.at("reason").is_string()) {
            rejected.reason = j.at("reason").get<std::string>();
        }
        if (j.contains("rejected_at") && j.at("rejected_at").is_string()) {
            rejected.rejected_at = j.at("rejected_at").get<std::string>();
        }
        return std::optional<RejectedFingerprint>{rejected};
    });
}

std::optional<EvolutionObservation> ObservationStore::Find(const std::string& id) const {
    for (const EvolutionObservation& observation : Load()) {
        if (observation.id == id) {
            return observation;
        }
    }
    return std::nullopt;
}

}  // namespace lubancode::evolution
