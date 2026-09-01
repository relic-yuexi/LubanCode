// TelemetrySpool 的实现。合同见 spool.hpp 文件头。
//
// 文件口走 stdio(fopen/fwrite/fflush),PowerLoss 档加 FlushFileBuffers/
// fsync(与 trajectory journal 同款耐久规矩,§7.4 三档在 spool 侧只用两档:
// 追加 = ProcessCrash,seal = PowerLoss)。
#include "telemetry/spool.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h 的 min/max 宏与 std::min/std::max 撞车
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lubancode::telemetry {
namespace {

std::string SegmentStem(std::uint64_t segment_id) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "seg-%08llu",
                  static_cast<unsigned long long>(segment_id));
    return buffer;
}

bool FlushDurable(std::FILE* file, bool power_loss) {
    if (std::fflush(file) != 0) {
        return false;
    }
    if (!power_loss) {
        return true;
    }
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return FlushFileBuffers(handle) != FALSE;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file.good()) {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return false;
        }
    }
    return platform::ReplaceFileAtomically(tmp, path).has_value();
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::int64_t FileModifiedMs(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    // file_time_type -> epoch 毫秒:先转 system_clock(C++20 clock_cast 语义,
    // 手写同式,免拉 <chrono> 新口在老工具链上的兼容账)。
    const auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(
        modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

Priority PriorityFromInt(int value) {
    switch (value) {
        case 0:
            return Priority::P0;
        case 1:
            return Priority::P1;
        case 2:
            return Priority::P2;
        case 3:
            return Priority::P3;
    }
    return Priority::P2;
}

// 段文件名 -> (段号, 是否 payload)。名字形如 seg-00000001.otlpjson /
// seg-00000001.meta.json;不合规回 nullopt。
std::optional<std::pair<std::uint64_t, bool>> ParseSegmentFileName(const std::string& name) {
    constexpr const char* kPrefix = "seg-";
    if (name.size() <= std::strlen(kPrefix) ||
        name.compare(0, std::strlen(kPrefix), kPrefix) != 0) {
        return std::nullopt;
    }
    const std::size_t dot = name.find('.');
    if (dot == std::string::npos) {
        return std::nullopt;
    }
    const std::string digits = name.substr(std::strlen(kPrefix), dot - std::strlen(kPrefix));
    if (digits.empty() || digits.size() > 12) {
        return std::nullopt;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    const bool is_payload = name.compare(dot, std::string::npos, ".otlpjson") == 0;
    const bool is_meta = name.compare(dot, std::string::npos, ".meta.json") == 0;
    if (!is_payload && !is_meta) {
        return std::nullopt;
    }
    return std::make_pair(std::stoull(digits), is_payload);
}

// payload 线文本 -> 段账(batches + 版本,版本取自行——meta 可忠实重造)。
std::optional<SealedSegment> ParseSegmentFromPayload(const std::string& content) {
    SealedSegment segment;
    std::uint64_t bytes = 0;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const std::size_t newline = content.find('\n', offset);
        const std::string line = content.substr(
            offset, newline == std::string::npos ? std::string::npos : newline - offset);
        offset = newline == std::string::npos ? content.size() : newline + 1;
        if (line.empty()) {
            continue;
        }
        bytes += line.size() + 1;
        const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
        auto record = SpoolBatchRecord::FromJson(json);
        if (!record.has_value()) {
            return std::nullopt;  // 坏行:段账重造不出
        }
        SegmentBatchIndex batch;
        batch.batch_id = record->batch_id;
        batch.signal = record->signal;
        batch.sha256 = hooks::Sha256Hex(line);
        batch.workspace_key = record->workspace_key;
        batch.session_id = record->session_id;
        batch.stream_id = record->stream_id;
        batch.first_event_id = record->first_event_id;
        batch.last_event_id = record->last_event_id;
        batch.last_event_hash = record->last_event_hash;
        batch.priority = record->priority;
        batch.final_window = record->final_window;
        batch.projection_generation = record->projection_generation;
        segment.batches.push_back(std::move(batch));
        // 版本面取自行(末行胜:同段各行同批同版本,正常一致)。
        segment.data_class = record->data_class;
        segment.projector_version = record->projector_version;
        segment.redaction_version = record->redaction_version;
        segment.telemetry_schema_version = record->telemetry_schema_version;
        segment.projection_generation = record->projection_generation;
    }
    if (segment.batches.empty()) {
        return std::nullopt;
    }
    segment.bytes = bytes;
    return segment;
}

}  // namespace

nlohmann::json SpoolBatchRecord::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = "lubancode.telemetry.spool-batch";
    json["version"] = 1;
    json["batch_id"] = batch_id;
    json["signal"] = signal;
    json["priority"] = static_cast<int>(priority);
    json["workspace_key"] = workspace_key;
    json["session_id"] = session_id;
    json["stream_id"] = stream_id;
    json["first_event_id"] = first_event_id;
    json["last_event_id"] = last_event_id;
    json["last_event_hash"] = last_event_hash;
    json["final_window"] = final_window;
    json["data_class"] = DataClassName(data_class);
    json["projector_version"] = projector_version;
    json["redaction_version"] = redaction_version;
    json["telemetry_schema_version"] = telemetry_schema_version;
    json["projection_generation"] = projection_generation;
    json["payload"] = payload;
    return json;
}

std::optional<SpoolBatchRecord> SpoolBatchRecord::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    SpoolBatchRecord record;
    std::string schema;
    if (!read_string("schema", &schema) || schema != "lubancode.telemetry.spool-batch") {
        return std::nullopt;
    }
    if (!read_string("batch_id", &record.batch_id) || record.batch_id.empty()) {
        return std::nullopt;
    }
    if (!read_string("signal", &record.signal)) {
        return std::nullopt;
    }
    if (json.contains("priority") && json.at("priority").is_number_integer()) {
        record.priority = PriorityFromInt(json.at("priority").get<int>());
    }
    read_string("workspace_key", &record.workspace_key);
    read_string("session_id", &record.session_id);
    read_string("stream_id", &record.stream_id);
    read_string("first_event_id", &record.first_event_id);
    read_string("last_event_id", &record.last_event_id);
    read_string("last_event_hash", &record.last_event_hash);
    if (json.contains("final_window") && json.at("final_window").is_boolean()) {
        record.final_window = json.at("final_window").get<bool>();
    }
    std::string data_class;
    if (read_string("data_class", &data_class)) {
        if (auto parsed = DataClassFromName(data_class)) {
            record.data_class = *parsed;
        }
    }
    read_string("projector_version", &record.projector_version);
    read_string("redaction_version", &record.redaction_version);
    if (json.contains("telemetry_schema_version") &&
        json.at("telemetry_schema_version").is_number_integer()) {
        record.telemetry_schema_version = json.at("telemetry_schema_version").get<int>();
    }
    if (json.contains("projection_generation") &&
        json.at("projection_generation").is_number_integer()) {
        record.projection_generation = json.at("projection_generation").get<int>();
    }
    if (json.contains("payload")) {
        record.payload = json.at("payload");
    }
    return record;
}

nlohmann::json SealedSegment::MetaToJson() const {
    nlohmann::json batches = nlohmann::json::array();
    for (const SegmentBatchIndex& batch : this->batches) {
        nlohmann::json entry = nlohmann::json::object();
        entry["batch_id"] = batch.batch_id;
        entry["signal"] = batch.signal;
        entry["sha256"] = batch.sha256;
        entry["workspace_key"] = batch.workspace_key;
        entry["session_id"] = batch.session_id;
        entry["stream_id"] = batch.stream_id;
        entry["first_event_id"] = batch.first_event_id;
        entry["last_event_id"] = batch.last_event_id;
        entry["last_event_hash"] = batch.last_event_hash;
        entry["priority"] = static_cast<int>(batch.priority);
        entry["final_window"] = batch.final_window;
        entry["projection_generation"] = batch.projection_generation;
        batches.push_back(std::move(entry));
    }
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kSegmentMetaSchema;
    json["version"] = kSegmentMetaVersion;
    json["segment_id"] = segment_id;
    json["created_at_ms"] = created_at_ms;
    json["bytes"] = bytes;
    json["data_class"] = DataClassName(data_class);
    json["projector_version"] = projector_version;
    json["redaction_version"] = redaction_version;
    json["telemetry_schema_version"] = telemetry_schema_version;
    json["projection_generation"] = projection_generation;
    json["batches"] = std::move(batches);
    return json;
}

std::optional<SealedSegment> SealedSegment::MetaFromJson(const nlohmann::json& json,
                                                         std::uint64_t segment_id) {
    if (!json.is_object() || !json.contains("schema") || !json.at("schema").is_string() ||
        json.at("schema").get<std::string>() != kSegmentMetaSchema) {
        return std::nullopt;
    }
    SealedSegment segment;
    segment.segment_id = segment_id;
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    if (json.contains("created_at_ms") && json.at("created_at_ms").is_number_integer()) {
        segment.created_at_ms = json.at("created_at_ms").get<std::int64_t>();
    }
    if (json.contains("bytes") && json.at("bytes").is_number_unsigned()) {
        segment.bytes = json.at("bytes").get<std::uint64_t>();
    }
    std::string data_class;
    if (read_string("data_class", &data_class)) {
        if (auto parsed = DataClassFromName(data_class)) {
            segment.data_class = *parsed;
        }
    }
    read_string("projector_version", &segment.projector_version);
    read_string("redaction_version", &segment.redaction_version);
    if (json.contains("telemetry_schema_version") &&
        json.at("telemetry_schema_version").is_number_integer()) {
        segment.telemetry_schema_version = json.at("telemetry_schema_version").get<int>();
    }
    if (json.contains("projection_generation") &&
        json.at("projection_generation").is_number_integer()) {
        segment.projection_generation = json.at("projection_generation").get<int>();
    }
    if (!json.contains("batches") || !json.at("batches").is_array()) {
        return std::nullopt;
    }
    for (const nlohmann::json& entry : json.at("batches")) {
        if (!entry.is_object()) {
            continue;
        }
        SegmentBatchIndex batch;
        const auto read_entry_string = [&](const char* key, std::string* out) {
            return entry.contains(key) && entry.at(key).is_string() &&
                   (*out = entry.at(key).get<std::string>(), true);
        };
        if (!read_entry_string("batch_id", &batch.batch_id) || batch.batch_id.empty()) {
            return std::nullopt;
        }
        read_entry_string("signal", &batch.signal);
        read_entry_string("sha256", &batch.sha256);
        read_entry_string("workspace_key", &batch.workspace_key);
        read_entry_string("session_id", &batch.session_id);
        read_entry_string("stream_id", &batch.stream_id);
        read_entry_string("first_event_id", &batch.first_event_id);
        read_entry_string("last_event_id", &batch.last_event_id);
        read_entry_string("last_event_hash", &batch.last_event_hash);
        if (entry.contains("priority") && entry.at("priority").is_number_integer()) {
            batch.priority = PriorityFromInt(entry.at("priority").get<int>());
        }
        if (entry.contains("final_window") && entry.at("final_window").is_boolean()) {
            batch.final_window = entry.at("final_window").get<bool>();
        }
        if (entry.contains("projection_generation") &&
            entry.at("projection_generation").is_number_integer()) {
            batch.projection_generation = entry.at("projection_generation").get<int>();
        }
        segment.batches.push_back(std::move(batch));
    }
    if (segment.batches.empty()) {
        return std::nullopt;
    }
    return segment;
}

TelemetrySpool::TelemetrySpool(std::filesystem::path spool_dir, std::filesystem::path quarantine_dir,
                               SpoolOptions options)
    : spool_dir_(std::move(spool_dir)), quarantine_dir_(std::move(quarantine_dir)),
      options_(options) {}

TelemetrySpool::~TelemetrySpool() {
    if (active_ != nullptr) {
        std::fclose(active_);
        active_ = nullptr;
    }
}

std::uint64_t TelemetrySpool::NextSegmentId() const {
    std::uint64_t next = 1;
    for (const SealedSegment& segment : sealed_) {
        next = std::max(next, segment.segment_id + 1);
    }
    return next;
}

bool TelemetrySpool::OpenActive() {
    if (active_ != nullptr) {
        return true;
    }
    active_ = std::fopen((spool_dir_ / "active.tmp").string().c_str(), "ab");
    return active_ != nullptr;
}

void TelemetrySpool::RebuildCoverage() {
    coverage_.clear();
    for (const SealedSegment& segment : sealed_) {
        for (const SegmentBatchIndex& batch : segment.batches) {
            const std::string key =
                batch.workspace_key + "|" + batch.session_id + "|" + batch.stream_id;
            coverage_[key] = StreamCoverage{batch.last_event_id, batch.last_event_hash,
                                            batch.final_window, batch.projection_generation};
        }
    }
}

void TelemetrySpool::RecomputeBytes() {
    total_bytes_ = 0;
    for (const SealedSegment& segment : sealed_) {
        total_bytes_ += segment.bytes;
    }
}

bool TelemetrySpool::RebuildSegmentMeta(const std::filesystem::path& payload_path,
                                        std::uint64_t segment_id) {
    const auto content = ReadTextFile(payload_path);
    if (!content.has_value()) {
        return false;
    }
    auto segment = ParseSegmentFromPayload(*content);
    if (!segment.has_value()) {
        return false;  // 坏行:meta 重造不出,交 quarantine 流程
    }
    segment->segment_id = segment_id;
    segment->created_at_ms = FileModifiedMs(payload_path);
    return WriteTextFileAtomic(spool_dir_ / (SegmentStem(segment_id) + ".meta.json"),
                               segment->MetaToJson().dump());
}

void TelemetrySpool::RecoverActive(std::int64_t now_ms, SpoolRecoveryReport* report) {
    const std::filesystem::path active_path = spool_dir_ / "active.tmp";
    std::error_code ec;
    if (!std::filesystem::exists(active_path, ec) || ec) {
        return;
    }
    const auto content = ReadTextFile(active_path);
    if (!content.has_value()) {
        stats_.last_error_code = "telemetry.io_error";
        stats_.quarantined_total += 1;
        report->quarantined_batches += 1;
        std::filesystem::remove(active_path, ec);
        return;
    }
    // 逐行验边界(§18.5):完整行(以 \n 收尾且 JSON 合)算完整批;尾部
    // 半行或坏行移 quarantine。
    std::vector<SpoolBatchRecord> complete;
    std::size_t offset = 0;
    std::size_t bad_lines = 0;
    while (offset <= content->size()) {
        const std::size_t newline = content->find('\n', offset);
        if (newline == std::string::npos) {
            // 尾段:非空即半行(§18.5 半批移 quarantine)。
            if (offset < content->size()) {
                bad_lines += 1;
            }
            break;
        }
        const std::string line = content->substr(offset, newline - offset);
        offset = newline + 1;
        if (line.empty()) {
            continue;
        }
        const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
        auto record = SpoolBatchRecord::FromJson(json);
        if (record.has_value()) {
            complete.push_back(std::move(*record));
        } else {
            bad_lines += 1;
        }
    }
    if (!complete.empty()) {
        // 完整批次照 seal(§18.5):写新段文件(直接最终名——内容此刻在
        // 内存里,不依赖 active.tmp 的半途状态)。
        std::string payload_text;
        for (const SpoolBatchRecord& record : complete) {
            payload_text += record.ToJson().dump();
            payload_text.push_back('\n');
        }
        auto segment = ParseSegmentFromPayload(payload_text);
        if (segment.has_value()) {
            segment->segment_id = NextSegmentId();
            segment->created_at_ms = now_ms;
            const std::filesystem::path payload_path =
                spool_dir_ / (SegmentStem(segment->segment_id) + ".otlpjson");
            if (WriteTextFileAtomic(payload_path, payload_text) &&
                WriteTextFileAtomic(
                    spool_dir_ / (SegmentStem(segment->segment_id) + ".meta.json"),
                    segment->MetaToJson().dump())) {
                sealed_.push_back(std::move(*segment));
                seal_generation_ += 1;
                report->sealed_from_active += 1;
            } else {
                stats_.last_error_code = "telemetry.spool_seal_failed";
                report->error_code = stats_.last_error_code;
            }
        }
    }
    if (bad_lines > 0) {
        // 半批/坏行留 quarantine(取证用),active.tmp 整体重开。
        const std::string quarantine_name =
            "active-" + std::to_string(now_ms) + ".ndjson";
        std::filesystem::copy_file(
            active_path, quarantine_dir_ / quarantine_name,
            std::filesystem::copy_options::overwrite_existing, ec);
        stats_.quarantined_total += bad_lines;
        report->quarantined_batches += bad_lines;
    }
    std::filesystem::remove(active_path, ec);
}

SpoolRecoveryReport TelemetrySpool::OpenAndRecover(std::int64_t now_ms) {
    SpoolRecoveryReport report;
    std::error_code ec;
    std::filesystem::create_directories(spool_dir_, ec);
    std::filesystem::create_directories(quarantine_dir_, ec);

    // 扫段:seg-*.meta.json 为正账;payload 在 meta 丢的,从 payload 重造。
    std::map<std::uint64_t, std::filesystem::path> payloads;
    std::map<std::uint64_t, std::filesystem::path> metas;
    for (const auto& entry : std::filesystem::directory_iterator(spool_dir_, ec)) {
        const std::string name = entry.path().filename().generic_string();
        auto parsed = ParseSegmentFileName(name);
        if (!parsed.has_value()) {
            continue;
        }
        if (parsed->second) {
            payloads[parsed->first] = entry.path();
        } else {
            metas[parsed->first] = entry.path();
        }
    }
    for (const auto& [id, meta_path] : metas) {
        const auto content = ReadTextFile(meta_path);
        if (!content.has_value()) {
            stats_.last_error_code = "telemetry.io_error";
            continue;
        }
        const nlohmann::json json = nlohmann::json::parse(*content, nullptr, false);
        auto segment = SealedSegment::MetaFromJson(json, id);
        if (segment.has_value() && payloads.count(id) > 0) {
            sealed_.push_back(std::move(*segment));
            payloads.erase(id);
        }
    }
    for (const auto& [id, payload_path] : payloads) {
        // meta 丢、payload 在:重造 meta(§18.5 不丢数据);重造不出进
        // quarantine 计账。
        if (RebuildSegmentMeta(payload_path, id)) {
            const auto meta = ReadTextFile(spool_dir_ / (SegmentStem(id) + ".meta.json"));
            const nlohmann::json json = nlohmann::json::parse(*meta, nullptr, false);
            if (auto segment = SealedSegment::MetaFromJson(json, id)) {
                sealed_.push_back(std::move(*segment));
                report.orphan_segments += 1;
            }
        } else {
            std::filesystem::copy_file(payload_path,
                                       quarantine_dir_ / (SegmentStem(id) + ".otlpjson"),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(payload_path, ec);
            std::filesystem::remove(spool_dir_ / (SegmentStem(id) + ".meta.json"), ec);
            stats_.quarantined_total += 1;
            report.quarantined_batches += 1;
            report.orphan_segments += 1;
        }
    }
    std::sort(sealed_.begin(), sealed_.end(),
              [](const SealedSegment& a, const SealedSegment& b) {
                  return a.segment_id < b.segment_id;
              });
    RebuildCoverage();
    RecomputeBytes();
    if (total_bytes_ > options_.total_bytes_cap) {
        Cleanup(now_ms);
    }
    RecoverActive(now_ms, &report);
    // active 里补 seal 的段也进覆盖账(cursor 对账要用,§18.5)。
    RebuildCoverage();
    RecomputeBytes();
    recovery_ = report;
    return report;
}

bool TelemetrySpool::SealLocked(std::int64_t now_ms) {
    if (active_ == nullptr || active_items_ == 0) {
        return false;
    }
    if (!FlushDurable(active_, true)) {
        stats_.last_error_code = "telemetry.spool_flush_failed";
        stats_.degraded = true;
        return false;
    }
    const std::uint64_t segment_id = NextSegmentId();
    const std::filesystem::path payload_path =
        spool_dir_ / (SegmentStem(segment_id) + ".otlpjson");
    const std::filesystem::path meta_path = spool_dir_ / (SegmentStem(segment_id) + ".meta.json");

    // active.tmp 此刻已 durable,关柄后 rename 成段 payload;meta 随后落。
    std::fclose(active_);
    active_ = nullptr;
    std::error_code ec;
    std::filesystem::rename(spool_dir_ / "active.tmp", payload_path, ec);
    if (ec) {
        stats_.last_error_code = "telemetry.spool_seal_failed";
        stats_.degraded = true;
        OpenActive();
        return false;
    }

    // meta 从刚封段读回行哈希(payload 行里带着对账面)。
    const auto content = ReadTextFile(payload_path);
    auto segment = content.has_value() ? ParseSegmentFromPayload(*content) : std::nullopt;
    if (!segment.has_value()) {
        // 读不回(理论上不该有:刚 fflush 过)——按零行段记账,不 silent。
        stats_.last_error_code = "telemetry.spool_seal_readback_failed";
        segment = SealedSegment{};
    }
    segment->segment_id = segment_id;
    segment->created_at_ms = now_ms;
    segment->bytes = segment->bytes == 0 ? active_bytes_ : segment->bytes;
    if (!WriteTextFileAtomic(meta_path, segment->MetaToJson().dump())) {
        // payload 已 durable:下回开张走 orphan 重造路,不丢数据。
        stats_.last_error_code = "telemetry.spool_meta_failed";
    }
    sealed_.push_back(std::move(*segment));
    seal_generation_ += 1;
    RebuildCoverage();
    RecomputeBytes();
    active_bytes_ = 0;
    active_items_ = 0;
    active_opened_ms_ = now_ms;
    OpenActive();
    Cleanup(now_ms);
    return true;
}

bool TelemetrySpool::AppendBatch(const SpoolBatchRecord& record, std::int64_t now_ms) {
    if (stats_.degraded) {
        return false;  // §18.4:degraded 停收新批
    }
    if (!OpenActive()) {
        stats_.last_error_code = "telemetry.spool_open_failed";
        stats_.degraded = true;
        return false;
    }
    const std::string line = record.ToJson().dump();
    const bool first_item = active_items_ == 0;
    if (std::fwrite(line.data(), 1, line.size(), active_) != line.size() ||
        std::fputc('\n', active_) == EOF || std::fflush(active_) != 0) {
        stats_.last_error_code = "telemetry.spool_write_failed";
        stats_.degraded = true;
        return false;
    }
    active_bytes_ += line.size() + 1;
    active_items_ += 1;
    if (first_item || active_opened_ms_ == 0) {
        active_opened_ms_ = now_ms;  // 时间帽的锚:本段首笔落时刻
    }
    if (active_bytes_ >= options_.segment_bytes_cap || active_items_ >= options_.segment_items_cap) {
        return SealLocked(now_ms);
    }
    return true;
}

bool TelemetrySpool::SealIfDue(std::int64_t now_ms, std::int64_t flush_interval_ms) {
    if (active_items_ == 0 || active_opened_ms_ == 0) {
        return false;
    }
    if (now_ms - active_opened_ms_ < flush_interval_ms) {
        return false;
    }
    return SealLocked(now_ms);
}

bool TelemetrySpool::SealNow(std::int64_t now_ms) { return SealLocked(now_ms); }

std::vector<std::uint64_t> TelemetrySpool::AckBatches(
    const std::vector<std::string>& batch_ids) {
    std::vector<std::uint64_t> deleted;
    const auto acked = [&](const std::string& id) {
        return std::find(batch_ids.begin(), batch_ids.end(), id) != batch_ids.end();
    };
    std::error_code ec;
    for (auto it = sealed_.begin(); it != sealed_.end();) {
        bool all_acked = !it->batches.empty();
        for (const SegmentBatchIndex& batch : it->batches) {
            if (!acked(batch.batch_id)) {
                all_acked = false;
                break;
            }
        }
        if (!all_acked) {
            ++it;
            continue;
        }
        const std::filesystem::path payload_path =
            spool_dir_ / (SegmentStem(it->segment_id) + ".otlpjson");
        const std::filesystem::path meta_path =
            spool_dir_ / (SegmentStem(it->segment_id) + ".meta.json");
        std::filesystem::remove(payload_path, ec);
        const bool payload_gone = !std::filesystem::exists(payload_path, ec);
        std::filesystem::remove(meta_path, ec);
        if (!payload_gone) {
            // 删失败:段留着,tombstone 由调用方记(§18.2 防重复无限发)。
            stats_.last_error_code = "telemetry.spool_delete_failed";
            ++it;
            continue;
        }
        deleted.push_back(it->segment_id);
        it = sealed_.erase(it);
    }
    RebuildCoverage();
    RecomputeBytes();
    return deleted;
}

std::vector<SealedSegment> TelemetrySpool::PurgeAll() {
    std::vector<SealedSegment> purged = std::move(sealed_);
    sealed_.clear();
    std::error_code ec;
    for (const SealedSegment& segment : purged) {
        std::filesystem::remove(spool_dir_ / (SegmentStem(segment.segment_id) + ".otlpjson"), ec);
        std::filesystem::remove(spool_dir_ / (SegmentStem(segment.segment_id) + ".meta.json"), ec);
    }
    // active.tmp:关柄删文件重开(批账回收由调用方记,§24.2)。
    if (active_ != nullptr) {
        std::fclose(active_);
        active_ = nullptr;
        std::filesystem::remove(spool_dir_ / "active.tmp", ec);
        active_bytes_ = 0;
        active_items_ = 0;
        active_opened_ms_ = 0;
        OpenActive();
    }
    RebuildCoverage();
    RecomputeBytes();
    return purged;
}

void TelemetrySpool::Cleanup(std::int64_t now_ms) {
    // §18.4:先删过 TTL 的,再删最老的,直到回帽;仍超 → degraded。
    std::error_code ec;
    const auto drop_oldest_until_fit = [&](bool ttl_only) {
        while (total_bytes_ > options_.total_bytes_cap && !sealed_.empty()) {
            SealedSegment* oldest = &sealed_.front();
            for (SealedSegment& segment : sealed_) {
                if (segment.created_at_ms < oldest->created_at_ms) {
                    oldest = &segment;
                }
            }
            if (ttl_only && oldest->created_at_ms + static_cast<std::int64_t>(options_.max_age_ms) > now_ms) {
                return;
            }
            // 清理半账(§18.5):被删段覆盖过的端点先记账再删——cursor
            // 日后超前于现存 spool 时,凭这半账判"正常清理"。
            for (const SegmentBatchIndex& batch : oldest->batches) {
                const std::string key =
                    batch.workspace_key + "|" + batch.session_id + "|" + batch.stream_id;
                cleaned_coverage_[key] =
                    StreamCoverage{batch.last_event_id, batch.last_event_hash,
                                   batch.final_window, batch.projection_generation};
            }
            std::filesystem::remove(
                spool_dir_ / (SegmentStem(oldest->segment_id) + ".otlpjson"), ec);
            std::filesystem::remove(
                spool_dir_ / (SegmentStem(oldest->segment_id) + ".meta.json"), ec);
            stats_.cleaned_segments_total += 1;
            stats_.cleaned_bytes_total += oldest->bytes;
            const std::uint64_t segment_id = oldest->segment_id;
            sealed_.erase(std::remove_if(sealed_.begin(), sealed_.end(),
                                         [segment_id](const SealedSegment& segment) {
                                             return segment.segment_id == segment_id;
                                         }),
                          sealed_.end());
            RecomputeBytes();
        }
    };
    drop_oldest_until_fit(true);
    drop_oldest_until_fit(false);
    stats_.degraded = total_bytes_ > options_.total_bytes_cap;
}

std::map<std::string, StreamCoverage> TelemetrySpool::Coverage() const { return coverage_; }

std::optional<std::vector<SpoolBatchRecord>> TelemetrySpool::ReadSealedBatches(
    std::uint64_t segment_id) const {
    bool in_ledger = false;
    for (const SealedSegment& segment : sealed_) {
        if (segment.segment_id == segment_id) {
            in_ledger = true;
            break;
        }
    }
    if (!in_ledger) {
        return std::nullopt;  // 段不在册:已 ACK 删除或 TTL 清理退场
    }
    const auto content = ReadTextFile(spool_dir_ / (SegmentStem(segment_id) + ".otlpjson"));
    if (!content.has_value()) {
        return std::nullopt;
    }
    std::vector<SpoolBatchRecord> records;
    std::istringstream stream(*content);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_discarded()) {
            continue;  // 单行坏不殃及整段:行级账在 meta,坏行走 quarantine 的旧规矩
        }
        auto record = SpoolBatchRecord::FromJson(json);
        if (record.has_value()) {
            records.push_back(std::move(*record));
        }
    }
    return records;
}


bool TelemetrySpool::HasBatch(const std::string& batch_id) const {
    for (const SealedSegment& segment : sealed_) {
        for (const SegmentBatchIndex& batch : segment.batches) {
            if (batch.batch_id == batch_id) {
                return true;
            }
        }
    }
    return false;
}

SpoolStats TelemetrySpool::Stats(std::int64_t now_ms) const {
    SpoolStats stats = stats_;
    stats.bytes = total_bytes_;
    stats.segments = sealed_.size();
    stats.active_batches = active_items_;
    for (const SealedSegment& segment : sealed_) {
        stats.sealed_batches += segment.batches.size();
    }
    if (!sealed_.empty()) {
        std::int64_t oldest = sealed_.front().created_at_ms;
        for (const SealedSegment& segment : sealed_) {
            oldest = std::min(oldest, segment.created_at_ms);
        }
        stats.oldest_age_ms = std::max<std::int64_t>(0, now_ms - oldest);
    }
    return stats;
}

nlohmann::json SpoolStats::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["bytes"] = bytes;
    out["segments"] = segments;
    out["oldest_age_ms"] = oldest_age_ms;
    out["active_batches"] = active_batches;
    out["sealed_batches"] = sealed_batches;
    out["quarantined_total"] = quarantined_total;
    out["cleaned_segments_total"] = cleaned_segments_total;
    out["cleaned_bytes_total"] = cleaned_bytes_total;
    out["degraded"] = degraded;
    out["last_error_code"] = last_error_code;
    return out;
}

}  // namespace lubancode::telemetry
