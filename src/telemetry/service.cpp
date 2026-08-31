// TelemetryService 的实现。合同见 service.hpp 文件头。
//
// 单写者模型(§17.3):投影、编码、落 spool、推 cursor 全在一只 worker
// 线程;Recorder/Agent 线程只碰 Notify(µs 级小锁)与状态面读。周期 tick
// 是丢 wake 的兜底(§14.1):wake 只管快,tick 管不漏。
#include "telemetry/service.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"
#include "telemetry/identity.hpp"
#include "telemetry/otlp_json.hpp"
#include "telemetry/projector.hpp"
#include "trajectory/event.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::telemetry {
namespace {

inline constexpr std::string_view kStateSchema = "lubancode.telemetry.state";
inline constexpr int kStateVersion = 1;
inline constexpr std::size_t kTombstoneCap = 1000;  // tombstone 簿有帽(§14.2)

std::string StreamKey(std::string_view workspace_key, std::string_view session_id,
                      std::string_view stream_id) {
    return std::string(workspace_key) + "|" + std::string(session_id) + "|" +
           std::string(stream_id);
}

std::string SessionKey(std::string_view workspace_key, std::string_view session_id) {
    return std::string(workspace_key) + "|" + std::string(session_id);
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

// 64 字节随机钥匙的十六进制(§9.3 local_projection_key;不进日志/spool)。
std::string GenerateProjectionKeyHex() {
    static std::mt19937_64 rng(std::random_device{}());
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t chunk = rng();
        for (int nibble = 0; nibble < 16; ++nibble) {
            hex.push_back(kHex[(chunk >> (60 - nibble * 4)) & 0xF]);
        }
    }
    return hex;
}

// 进程一次性实例 id(service.instance.id,§10.1 ephemeral)。
std::string GenerateInstanceIdHex() {
    static std::mt19937_64 rng(std::random_device{}());
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "proc-%08llx-%04x",
                  static_cast<unsigned long long>(rng()), static_cast<unsigned>(rng() & 0xFFFF));
    return buffer;
}

// 装配层没给的 platform 面补齐(§10.1:os.type/host.arch 按编译目标;
// 拿不到的留空由 BuildResourceAttributes 自己裁,不冒充)。
void NormalizeResourcePlatform(ResourceInputs* resource) {
#ifdef _WIN32
    if (resource->os_type.empty()) {
        resource->os_type = "windows";
    }
#elif defined(__APPLE__)
    if (resource->os_type.empty()) {
        resource->os_type = "darwin";
    }
#elif defined(__linux__)
    if (resource->os_type.empty()) {
        resource->os_type = "linux";
    }
#endif
#if defined(__x86_64__) || defined(_M_X64)
    if (resource->host_arch.empty()) {
        resource->host_arch = "amd64";
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (resource->host_arch.empty()) {
        resource->host_arch = "arm64";
    }
#endif
    if (resource->service_instance_id.empty()) {
        resource->service_instance_id = GenerateInstanceIdHex();
    }
}

// journal 行 -> (event_id, event_hash)。坏行回 nullopt。
struct LineIdHash {
    std::string event_id;
    std::string event_hash;
    int schema_version = 1;
};
std::optional<LineIdHash> ParseLineIdHash(const std::string& line) {
    const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::nullopt;
    }
    LineIdHash out;
    if (!json.contains("event_id") || !json.at("event_id").is_string() ||
        !json.contains("event_hash") || !json.at("event_hash").is_string()) {
        return std::nullopt;
    }
    out.event_id = json.at("event_id").get<std::string>();
    out.event_hash = json.at("event_hash").get<std::string>();
    if (json.contains("schema_version") && json.at("schema_version").is_number_integer()) {
        out.schema_version = json.at("schema_version").get<int>();
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// TelemetryService 生命周期
// ---------------------------------------------------------------------------

TelemetryService::TelemetryService(TelemetryServiceOptions options)
    : options_(std::move(options)), queue_(options_.queue) {}

TelemetryService::~TelemetryService() {
    if (running_.load()) {
        Stop();
    }
}

bool TelemetryService::LoadState() {
    const std::filesystem::path state_path = options_.telemetry_root / "state.json";
    const auto content = ReadTextFile(state_path);
    bool changed = false;
    if (content.has_value()) {
        const nlohmann::json json = nlohmann::json::parse(*content, nullptr, false);
        if (!json.is_discarded() && json.is_object() && json.contains("schema") &&
            json.at("schema").is_string() &&
            json.at("schema").get<std::string>() == kStateSchema) {
            if (json.contains("projection_key_hex") &&
                json.at("projection_key_hex").is_string()) {
                projection_key_ = json.at("projection_key_hex").get<std::string>();
            }
            if (json.contains("projection_generation") &&
                json.at("projection_generation").is_number_integer()) {
                projection_generation_ = json.at("projection_generation").get<int>();
            }
            if (json.contains("device_instance_id") &&
                json.at("device_instance_id").is_string()) {
                device_instance_id_ = json.at("device_instance_id").get<std::string>();
            }
            std::string stored_projector;
            if (json.contains("projector_version") && json.at("projector_version").is_string()) {
                stored_projector = json.at("projector_version").get<std::string>();
            }
            // §27.2/§14.2:projector 版本不兼容 -> 另开 generation + 换钥匙。
            // 新 id 不与旧 spool 混账;旧段按自身版本记账,T2 送完或弃置。
            if (!projection_key_.empty() && stored_projector != std::string(kProjectorVersion)) {
                projection_generation_ += 1;
                projection_key_ = GenerateProjectionKeyHex();
                changed = true;
            }
            if (json.contains("tombstones") && json.at("tombstones").is_array()) {
                for (const nlohmann::json& entry : json.at("tombstones")) {
                    if (entry.is_object() && entry.contains("batch_id") &&
                        entry.at("batch_id").is_string()) {
                        std::int64_t acked_at = 0;
                        if (entry.contains("acked_at_ms") &&
                            entry.at("acked_at_ms").is_number_integer()) {
                            acked_at = entry.at("acked_at_ms").get<std::int64_t>();
                        }
                        tombstones_.emplace_back(entry.at("batch_id").get<std::string>(),
                                                 acked_at);
                    }
                }
            }
            if (json.contains("retired_watermark") && json.at("retired_watermark").is_object()) {
                for (auto it = json.at("retired_watermark").begin();
                     it != json.at("retired_watermark").end(); ++it) {
                    if (it.value().is_string()) {
                        retired_watermarks_[it.key()] = it.value().get<std::string>();
                    }
                }
            }
        } else {
            // state 坏:不猜,换新账本(旧 spool 段自带版本,不被动到)。
            changed = true;
        }
    }
    if (projection_key_.empty()) {
        projection_key_ = GenerateProjectionKeyHex();
        changed = true;
    }
    // 匿名设备实例(§9.1:不用机器名;首启随机,此后稳定,可 rotate =
    // 删 state.json 这枚字段)。装配层显式给过(测试钉住)则听装配层。
    if (device_instance_id_.empty()) {
        device_instance_id_ = options_.resource.device_instance_id.empty()
                                  ? "device-" + hooks::Sha256Hex(GenerateProjectionKeyHex()).substr(0, 16)
                                  : options_.resource.device_instance_id;
        changed = true;
    }
    options_.resource.device_instance_id = device_instance_id_;
    return !changed || PersistState();
}

bool TelemetryService::PersistState() {
    nlohmann::json tombstones = nlohmann::json::array();
    // 最新在前,截帽。
    for (auto it = tombstones_.rbegin();
         it != tombstones_.rend() && tombstones.size() < kTombstoneCap; ++it) {
        tombstones.push_back(nlohmann::json{{"batch_id", it->first},
                                            {"acked_at_ms", it->second}});
    }
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kStateSchema;
    json["version"] = kStateVersion;
    json["projection_generation"] = projection_generation_;
    json["projector_version"] = std::string(kProjectorVersion);
    json["projection_key_hex"] = projection_key_;
    json["device_instance_id"] = device_instance_id_;
    json["updated_at_ms"] = platform::WallClockNowMs();
    json["tombstones"] = std::move(tombstones);
    // 退场水位(§18.5):ACK 删除/TTL 清理覆盖到的 stream 端点。
    nlohmann::json retired = nlohmann::json::object();
    for (const auto& [key, id] : retired_watermarks_) {
        retired[key] = id;
    }
    json["retired_watermark"] = std::move(retired);
    std::error_code ec;
    std::filesystem::create_directories(options_.telemetry_root, ec);
    if (ec) {
        return false;
    }
    return WriteTextFileAtomic(options_.telemetry_root / "state.json", json.dump());
}

bool TelemetryService::Start() {
    if (running_.load()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.telemetry_root, ec);
    if (ec) {
        degraded_reason_ = "telemetry.root_unwritable";
        return false;
    }
    if (!LoadState()) {
        degraded_reason_ = "telemetry.state_unwritable";
        // 状态开不出仍继续开 spool:投影能跑就跑,状态面报 degraded。
    }
    NormalizeResourcePlatform(&options_.resource);
    spool_ = std::make_unique<TelemetrySpool>(options_.telemetry_root / "spool",
                                              options_.telemetry_root / "quarantine",
                                              options_.spool);
    recovery_ = spool_->OpenAndRecover(platform::WallClockNowMs());
    if (!recovery_.error_code.empty() && degraded_reason_.empty()) {
        degraded_reason_ = recovery_.error_code;
    }
    started_at_ms_ = platform::WallClockNowMs();
    stop_.store(false);
    worker_ = std::thread([this] { WorkerLoop(); });
    running_.store(true);
    return true;
}

void TelemetryService::Stop() {
    if (!running_.load()) {
        return;
    }
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_cv_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    // §26.3:seal active -> persist cursor(AdvancePendingCursors 已在 worker
    // 末趟做过;这里再补一道 seal,兜 worker 末趟之后的尾巴)-> persist state。
    if (spool_ != nullptr) {
        spool_->SealNow(platform::WallClockNowMs());
        AdvancePendingCursors();
    }
    PersistState();
}

// ---------------------------------------------------------------------------
// wake 面
// ---------------------------------------------------------------------------

void TelemetryService::Notify(const CommitWake& wake) noexcept {
    if (stop_.load() || !running_.load()) {
        missed_wakes_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        pending_wakes_.insert(StreamKey(wake.workspace_key, wake.session_id, wake.stream_id));
    }
    wake_cv_.notify_one();
}

void TelemetryService::RegisterSession(const std::string& workspace_key,
                                       const std::string& session_id,
                                       const std::filesystem::path& session_dir) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sessions_[SessionKey(workspace_key, session_id)] =
            SessionEntry{workspace_key, session_id, session_dir};
    }
    {
        // 注册即有新账:投一枚该 session 的 wake,worker 首趟就能发现 stream。
        std::lock_guard<std::mutex> lock(wake_mutex_);
        pending_wakes_.insert(StreamKey(workspace_key, session_id, "main.jsonl"));
    }
    wake_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// worker:发现、投影、落盘、推 cursor
// ---------------------------------------------------------------------------

void TelemetryService::WorkerLoop() {
    while (!stop_.load()) {
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, std::chrono::milliseconds(options_.tick_ms),
                          [this] { return stop_.load() || !pending_wakes_.empty(); });
        lock.unlock();
        if (stop_.load()) {
            break;
        }
        RunPass(false);
    }
    // §26.3 末趟:final flush(开着的 span 按 missing 收口入账)+ seal +
    // 持久化。
    RunPass(true);
    if (spool_ != nullptr) {
        spool_->SealNow(platform::WallClockNowMs());
        AdvancePendingCursors();
    }
    PersistState();
}

void TelemetryService::DiscoverStreams(const SessionEntry& session) {
    std::vector<std::string> streams;
    std::error_code ec;
    const std::filesystem::path main_path = session.session_dir / "main.jsonl";
    if (std::filesystem::exists(main_path, ec)) {
        streams.push_back("main.jsonl");
    }
    const std::filesystem::path subagents = session.session_dir / "subagents";
    if (std::filesystem::exists(subagents, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(subagents, ec)) {
            const std::string name = entry.path().filename().generic_string();
            if (name.size() > 6 && name.compare(name.size() - 6, std::string::npos, ".jsonl") == 0) {
                streams.push_back("subagents/" + name);
            }
        }
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const std::string& stream : streams) {
        streams_.try_emplace(StreamKey(session.workspace_key, session.session_id, stream));
    }
}

void TelemetryService::RunPass(bool final_flush) {
    std::set<std::string> wakes;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wakes.swap(pending_wakes_);
    }
    std::vector<SessionEntry> sessions;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& [key, entry] : sessions_) {
            sessions.push_back(entry);
        }
    }
    for (const SessionEntry& session : sessions) {
        DiscoverStreams(session);
    }
    // tick 兜底(§14.1:wake 丢了不要紧,周期扫描或 session close 会补):
    // 每趟全量对册,ProjectStream 里用文件大小省无谓的整读。
    std::vector<std::pair<SessionEntry, std::string>> work;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& [key, state] : streams_) {
            const std::size_t last = key.rfind('|');
            const auto session_it = sessions_.find(key.substr(0, last));
            if (session_it == sessions_.end()) {
                continue;
            }
            if (!final_flush && state.final_flushed) {
                continue;
            }
            work.emplace_back(session_it->second, key.substr(last + 1));
        }
    }
    for (const auto& [session, stream_id] : work) {
        ProjectStream(session, stream_id, final_flush);
    }
    DrainQueue();
    if (spool_ != nullptr) {
        spool_->SealIfDue(platform::WallClockNowMs(), options_.flush_interval_ms);
        // 清理半账并进退场水位(§18.5)。
        for (const auto& [key, cov] : spool_->CleanedCoverage()) {
            retired_watermarks_[key] = cov.last_event_id;
        }
        AdvancePendingCursors();
    }
}

std::string TelemetryService::DeriveBatchId(const std::string& workspace_key,
                                            const std::string& session_id,
                                            const std::string& stream_id,
                                            const std::string& last_event_id, const char* signal,
                                            bool final_window) const {
    std::string material = "t1-batch|";
    material += std::to_string(projection_generation_);
    material += "|";
    material += workspace_key;
    material += "|";
    material += session_id;
    material += "|";
    material += stream_id;
    material += "|";
    material += last_event_id;
    material += "|";
    material += signal;
    if (final_window) {
        material += "|final";
    }
    // 24 位十六进制(12 字节):generation 内确定,窗口重放同 id(§18.6)。
    return hooks::Sha256Hex(material).substr(0, 24);
}

void TelemetryService::ProjectStream(const SessionEntry& session, const std::string& stream_id,
                                     bool final_flush) {
    const std::string key = StreamKey(session.workspace_key, session.session_id, stream_id);
    const std::filesystem::path path = session.session_dir / stream_id;
    const std::int64_t now_ms = platform::WallClockNowMs();

    // tick 兜底的省读判据(§14.1):文件没长个就不重读整本——wake 到了
    // 必然长个,长个才值得读;停投/已收口的 stream 同样靠它静默。
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto found = streams_.find(key);
        if (found != streams_.end() && !final_flush && found->second.last_size == size) {
            return;
        }
    }

    auto lines = trajectory::ReadJournalLines(path);
    if (!lines.has_value()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        streams_[key].error_code = "telemetry.io_error";
        return;
    }

    StreamState state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto found = streams_.find(key);
        if (found == streams_.end()) {
            return;
        }
        state = found->second;
        // 懒加载 cursor(§26.1"load cursors"落在发现时,不扫全盘)。
        if (state.cursor.stream.empty()) {
            std::string error;
            auto loaded = LoadCursor(options_.telemetry_root / "cursors", session.workspace_key,
                                     session.session_id, stream_id, &error);
            if (loaded.has_value()) {
                if (loaded->projection_generation != projection_generation_ ||
                    loaded->projector_version != std::string(kProjectorVersion)) {
                    // generation 换账:旧 cursor 过期,从 Journal 头重投
                    //(§27.2 新代新 id,不与旧 spool 混账)。
                    state.cursor = StreamCursor{};
                } else {
                    state.cursor = *loaded;
                }
            } else if (!error.empty()) {
                state.error_code = error;
                streams_[key] = state;
                return;
            }
        }
    }

    // ---- 定位 cursor(§14.2:超前/换账/hash 不合,停整条 stream,不猜)----
    std::size_t cursor_index = 0;
    bool has_cursor = !state.cursor.last_event_id.empty();
    if (has_cursor) {
        std::size_t cursor_found = lines->size();
        for (std::size_t i = 0; i < lines->size(); ++i) {
            auto id_hash = ParseLineIdHash((*lines)[i]);
            if (id_hash.has_value() && id_hash->event_id == state.cursor.last_event_id) {
                if (id_hash->event_hash != state.cursor.last_event_hash) {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state.error_code = "telemetry.cursor_hash_mismatch";
                    state.last_size = size;
                    streams_[key] = state;
                    return;
                }
                cursor_found = i;
                break;
            }
        }
        if (cursor_found == lines->size()) {
            // Journal 里找不到 cursor 的末事件:cursor 超前或 stream 换账。
            std::lock_guard<std::mutex> lock(state_mutex_);
            state.error_code = "telemetry.cursor_ahead";
            state.last_size = size;
            streams_[key] = state;
            return;
        }
        cursor_index = cursor_found;
    }

    // ---- 与 spool durable 覆盖对账(§18.5)----
    bool spool_reaches_cursor = !has_cursor;
    if (spool_ != nullptr) {
        const std::map<std::string, StreamCoverage> coverage = spool_->Coverage();
        const auto cov = coverage.find(key);
        if (cov != coverage.end() && cov->second.projection_generation == projection_generation_) {
            // coverage 端点在 Journal 里的位置(全本找:落在 cursor 之前 =
            // spool 落后,同样要走退场水位判定,不是账目对不上)。
            std::size_t coverage_index = lines->size();
            for (std::size_t i = 0; i < lines->size(); ++i) {
                auto id_hash = ParseLineIdHash((*lines)[i]);
                if (id_hash.has_value() && id_hash->event_id == cov->second.last_event_id) {
                    if (id_hash->event_hash != cov->second.last_event_hash) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state.error_code = "telemetry.coverage_hash_mismatch";
                        state.last_size = size;
                        streams_[key] = state;
                        return;
                    }
                    coverage_index = i;
                    break;
                }
            }
            if (coverage_index == lines->size()) {
                // spool 的覆盖端点不在这本 Journal 上:账目对不上,停。
                std::lock_guard<std::mutex> lock(state_mutex_);
                state.error_code = "telemetry.coverage_mismatch";
                state.last_size = size;
                streams_[key] = state;
                return;
            }
            if (coverage_index > cursor_index || !has_cursor) {
                // spool 比 cursor 多(seal 后 cursor 没落盘就崩了)或 cursor
                // 文件丢了:修前推到 durable 端点,免重投窗口造成跨批重复;
                // 已 durable 的窗口靠 batch id 去重兜底(§18.5)。
                auto id_hash = ParseLineIdHash((*lines)[coverage_index]);
                state.cursor.workspace_key = session.workspace_key;
                state.cursor.session_id = session.session_id;
                state.cursor.stream = stream_id;
                state.cursor.last_event_id = id_hash->event_id;
                state.cursor.last_event_hash = id_hash->event_hash;
                state.cursor.projector_version = std::string(kProjectorVersion);
                state.cursor.projection_generation = projection_generation_;
                state.cursor.updated_at_ms = now_ms;
                StoreCursor(options_.telemetry_root / "cursors", state.cursor);
                cursor_index = coverage_index;
                has_cursor = true;
            }
            spool_reaches_cursor = coverage_index >= cursor_index;
        }
        if (!spool_reaches_cursor) {
            // cursor 超前于现存 spool(无覆盖,或覆盖端点落后):查退场水位
            //(ACK 删除或 TTL 清理;有账 = 正常清理,无账 = 损坏,§18.5)。
            const auto retired = retired_watermarks_.find(key);
            bool accounted = false;
            if (retired != retired_watermarks_.end()) {
                for (std::size_t i = cursor_index; i < lines->size(); ++i) {
                    auto id_hash = ParseLineIdHash((*lines)[i]);
                    if (id_hash.has_value() && id_hash->event_id == retired->second) {
                        accounted = true;
                        break;
                    }
                }
            }
            if (!accounted) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state.error_code = "telemetry.cursor_orphan";
                state.last_size = size;
                streams_[key] = state;
                return;
            }
            spool_reaches_cursor = true;
        }
    }

    // ---- 窗口与投影 ----
    const std::size_t window_begin = has_cursor ? cursor_index + 1 : 0;
    const std::size_t lag = lines->size() > window_begin ? lines->size() - window_begin : 0;
    state.lag_events = lag;
    state.last_size = size;
    if (lag == 0 && !final_flush) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        streams_[key] = state;
        return;
    }

    ProjectorOptions projector_options;
    projector_options.projection_key = projection_key_;
    projector_options.resource = options_.resource;
    projector_options.resource.workspace_key = session.workspace_key;
    projector_options.data_class = options_.data_class;
    if (auto first = ParseLineIdHash(lines->front())) {
        projector_options.resource.trajectory_schema_version = first->schema_version;
    }
    const ProjectionReport report = ProjectJournalFile(path, projector_options);
    if (!report.ok) {
        // 坏链/合同违例:停整条 stream,其他 stream 照跑(§22.5)。
        std::lock_guard<std::mutex> lock(state_mutex_);
        state.error_code = report.error_code;
        streams_[key] = state;
        return;
    }

    // 窗口身份:末行(cursor 的推进目标);final 且窗口空时钉在 cursor 处。
    std::string window_first_id;
    std::string last_id;
    std::string last_hash;
    std::set<std::string> window_ids;
    for (std::size_t i = window_begin; i < lines->size(); ++i) {
        auto id_hash = ParseLineIdHash((*lines)[i]);
        if (!id_hash.has_value()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state.error_code = "telemetry.source_corrupt";
            streams_[key] = state;
            return;
        }
        if (window_first_id.empty()) {
            window_first_id = id_hash->event_id;
        }
        window_ids.insert(id_hash->event_id);
        last_id = id_hash->event_id;
        last_hash = id_hash->event_hash;
    }
    if (last_id.empty()) {
        window_first_id = state.cursor.last_event_id;
        last_id = state.cursor.last_event_id;
        last_hash = state.cursor.last_event_hash;
    }

    // spans 按终事件落窗(§14.1 fast path):终事件在窗内的 span 这一窗
    // 收;仍开着的 span 只有 final flush 才按 missing 收口入账。
    std::vector<TraceSpan> spans_new;
    bool has_error_span = false;
    for (const TraceSpan& span : report.spans) {
        const bool terminal_in_window =
            !span.source_terminal_event_id.empty() &&
            window_ids.count(span.source_terminal_event_id) > 0;
        const bool open_at_final = final_flush && span.source_terminal_event_id.empty();
        if (terminal_in_window || open_at_final) {
            spans_new.push_back(span);
            has_error_span =
                has_error_span || span.status == StatusCode::Error || open_at_final;
        }
    }

    const auto make_cursor = [&](const std::string& id, const std::string& hash) {
        StreamCursor advanced = state.cursor;
        advanced.workspace_key = session.workspace_key;
        advanced.session_id = session.session_id;
        advanced.stream = stream_id;
        advanced.last_event_id = id;
        advanced.last_event_hash = hash;
        advanced.projector_version = std::string(kProjectorVersion);
        advanced.projection_generation = projection_generation_;
        advanced.updated_at_ms = now_ms;
        return advanced;
    };

    const auto enqueue_or_dedup = [&](BatchItem item) {
        if (spool_ != nullptr && spool_->HasBatch(item.batch_id)) {
            // durable 已有(崩溃后重投同一窗口):不重发,直接记推进
            //(批已在 sealed 段里,epoch 0 = 永远可推)。
            pending_cursor_advances_[key] =
                PendingAdvance{make_cursor(item.last_event_id, item.last_event_hash), 0};
            return;
        }
        (void)queue_.TryPush(std::move(item));
        // 投不进(§17.2 第 6 步)或被拒:cursor 不推,下趟重投同 id 窗口。
    };

    if (!spans_new.empty()) {
        BatchItem item;
        item.batch_id =
            DeriveBatchId(session.workspace_key, session.session_id, stream_id, last_id, "traces",
                          final_flush && lag == 0);
        // §17.1 分档:错误/终态异常/run 终态 = P0;正常 span 收发 = P1。
        bool run_terminal = false;
        for (const TraceSpan& span : spans_new) {
            run_terminal = run_terminal || span.name == "lubancode.agent.run";
        }
        item.priority = (has_error_span || run_terminal) ? Priority::P0 : Priority::P1;
        item.workspace_key = session.workspace_key;
        item.session_id = session.session_id;
        item.stream_id = stream_id;
        item.first_event_id = window_first_id;
        item.last_event_id = last_id;
        item.last_event_hash = last_hash;
        item.final_window = final_flush;
        item.resource_attributes = report.resource_attributes;
        item.spans = std::move(spans_new);
        enqueue_or_dedup(std::move(item));
    }
    if (!report.metrics.empty() && lag > 0) {
        BatchItem item;
        item.batch_id = DeriveBatchId(session.workspace_key, session.session_id, stream_id,
                                      last_id, "metrics", false);
        item.priority = Priority::P2;  // §17.1 正常样本/周期 metrics
        item.workspace_key = session.workspace_key;
        item.session_id = session.session_id;
        item.stream_id = stream_id;
        item.first_event_id = window_first_id;
        item.last_event_id = last_id;
        item.last_event_hash = last_hash;
        item.final_window = final_flush;
        item.resource_attributes = report.resource_attributes;
        item.metrics = report.metrics;  // 全量累计快照;队里并系合并(§17.2)
        enqueue_or_dedup(std::move(item));
    }
    if (final_flush) {
        state.final_flushed = true;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        streams_[key] = state;
    }
}

void TelemetryService::DrainQueue() {
    if (spool_ == nullptr) {
        queue_.DrainDiscard();
        return;
    }
    const std::int64_t now_ms = platform::WallClockNowMs();
    while (auto item = queue_.Pop()) {
        SpoolBatchRecord record;
        record.batch_id = item->batch_id;
        record.signal = item->IsMetrics() ? "metrics" : "traces";
        record.priority = item->priority;
        record.workspace_key = item->workspace_key;
        record.session_id = item->session_id;
        record.stream_id = item->stream_id;
        record.first_event_id = item->first_event_id;
        record.last_event_id = item->last_event_id;
        record.last_event_hash = item->last_event_hash;
        record.final_window = item->final_window;
        record.data_class = options_.data_class;
        record.projector_version = std::string(kProjectorVersion);
        record.redaction_version = std::string(kRedactionPolicyVersion);
        record.telemetry_schema_version = kTelemetrySchemaVersion;
        record.projection_generation = projection_generation_;
        record.payload = item->IsMetrics()
                             ? EncodeMetricsRequest(item->resource_attributes, item->metrics)
                             : EncodeTracesRequest(item->resource_attributes, item->spans);
        const std::uint64_t epoch = append_epoch_ + 1;
        if (spool_->AppendBatch(record, now_ms)) {
            append_epoch_ = epoch;
            const std::string key =
                StreamKey(item->workspace_key, item->session_id, item->stream_id);
            StreamCursor advanced;
            advanced.workspace_key = item->workspace_key;
            advanced.session_id = item->session_id;
            advanced.stream = item->stream_id;
            advanced.last_event_id = item->last_event_id;
            advanced.last_event_hash = item->last_event_hash;
            advanced.projector_version = std::string(kProjectorVersion);
            advanced.projection_generation = projection_generation_;
            advanced.updated_at_ms = now_ms;
            pending_cursor_advances_[key] = PendingAdvance{std::move(advanced), epoch};
        } else if (degraded_reason_.empty()) {
            // 批被拒(spool 满盘/IO 坏):派生数据丢一窗,canonical 无损;
            // cursor 不推,下趟重投同 id。状态面报降级。
            degraded_reason_ = "telemetry.spool_rejected";
        }
    }
}

void TelemetryService::AdvancePendingCursors() {
    // cursor 只在派生批次 durable 入 spool 后推进(§14.2):落 active 的批
    // 要等 spool 真封了一段(seal 代数前进)才许推。单写者,无竞态。
    if (spool_ == nullptr) {
        return;
    }
    const std::uint64_t seal_generation = spool_->seal_generation();
    if (seal_generation != last_seen_seal_generation_) {
        // 封段完成:此刻之前落盘的批全部 durable。
        last_seen_seal_generation_ = seal_generation;
        durable_append_epoch_ = append_epoch_;
    }
    for (auto it = pending_cursor_advances_.begin(); it != pending_cursor_advances_.end();) {
        if (it->second.append_epoch > durable_append_epoch_) {
            ++it;
            continue;
        }
        StoreCursor(options_.telemetry_root / "cursors", it->second.cursor);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto found = streams_.find(it->first);
            if (found != streams_.end()) {
                // 只前推不回退(崩溃修复路径可能已推到更后的位置)。
                if (found->second.cursor.last_event_id != it->second.cursor.last_event_id) {
                    found->second.cursor = it->second.cursor;
                }
            }
        }
        it = pending_cursor_advances_.erase(it);
    }
}

void TelemetryService::AckBatches(const std::vector<std::string>& batch_ids) {
    if (spool_ == nullptr || batch_ids.empty()) {
        return;
    }
    const std::int64_t now_ms = platform::WallClockNowMs();
    const auto acked = [&](const std::string& id) {
        return std::find(batch_ids.begin(), batch_ids.end(), id) != batch_ids.end();
    };
    // 删段前先记窗口账:ACK 覆盖到的 stream 端点进退场水位(§18.5),
    // 批 id 进 tombstone 簿(§18.2 删除失败防重复无限发)。
    for (const SealedSegment& segment : spool_->sealed_segments()) {
        for (const SegmentBatchIndex& batch : segment.batches) {
            if (!acked(batch.batch_id)) {
                continue;
            }
            const std::string key =
                batch.workspace_key + "|" + batch.session_id + "|" + batch.stream_id;
            retired_watermarks_[key] = batch.last_event_id;
            tombstones_.emplace_back(batch.batch_id, now_ms);
        }
    }
    if (tombstones_.size() > kTombstoneCap) {
        tombstones_.erase(tombstones_.begin(),
                          tombstones_.begin() + static_cast<std::ptrdiff_t>(
                                                   tombstones_.size() - kTombstoneCap));
    }
    (void)spool_->AckBatches(batch_ids);
    PersistState();
}

TelemetryServiceStatus TelemetryService::Status() const {
    TelemetryServiceStatus status;
    status.running = running_.load();
    status.degraded = !degraded_reason_.empty();
    status.degraded_reason = degraded_reason_;
    status.projection_generation = projection_generation_;
    status.projector_version = std::string(kProjectorVersion);
    status.started_at_ms = started_at_ms_;
    status.missed_wakes = missed_wakes_.load();
    status.queue = queue_.Stats();
    if (spool_ != nullptr) {
        status.spool = spool_->Stats(platform::WallClockNowMs());
    }
    status.recovery = recovery_;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& [key, state] : streams_) {
            const std::size_t first = key.find('|');
            const std::size_t last = key.rfind('|');
            TelemetryServiceStatus::StreamStatus stream;
            stream.workspace_key = key.substr(0, first);
            stream.session_id = key.substr(first + 1, last - first - 1);
            stream.stream_id = key.substr(last + 1);
            stream.last_event_id = state.cursor.last_event_id;
            stream.lag_events = state.lag_events;
            stream.error_code = state.error_code;
            stream.final_flushed = state.final_flushed;
            status.streams.push_back(std::move(stream));
        }
    }
    return status;
}

nlohmann::json TelemetryServiceStatus::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["running"] = running;
    out["degraded"] = degraded;
    out["degraded_reason"] = degraded_reason;
    out["projection_generation"] = projection_generation;
    out["projector_version"] = projector_version;
    out["started_at_ms"] = started_at_ms;
    out["missed_wakes"] = missed_wakes;
    out["queue"] = queue.ToJson();
    out["spool"] = spool.ToJson();
    nlohmann::json recovery_json = nlohmann::json::object();
    recovery_json["sealed_from_active"] = recovery.sealed_from_active;
    recovery_json["quarantined_batches"] = recovery.quarantined_batches;
    recovery_json["orphan_segments"] = recovery.orphan_segments;
    out["recovery"] = std::move(recovery_json);
    nlohmann::json streams = nlohmann::json::array();
    for (const StreamStatus& stream : this->streams) {
        streams.push_back(nlohmann::json{{"workspace_key", stream.workspace_key},
                                         {"session_id", stream.session_id},
                                         {"stream_id", stream.stream_id},
                                         {"last_event_id", stream.last_event_id},
                                         {"lag_events", stream.lag_events},
                                         {"error_code", stream.error_code},
                                         {"final_flushed", stream.final_flushed}});
    }
    out["streams"] = std::move(streams);
    return out;
}

// ---------------------------------------------------------------------------
// 本地面格式化(/telemetry status 与 /doctor telemetry)
// ---------------------------------------------------------------------------

std::vector<std::string> FormatTelemetryStatusLines(const TelemetryServiceStatus& status) {
    std::vector<std::string> lines;
    lines.push_back(status.degraded
                        ? ("遥测: 运行中(降级 " + status.degraded_reason + ")")
                        : (status.running ? "遥测: 运行中" : "遥测: 已停止"));
    lines.push_back("投影代数: " + std::to_string(status.projection_generation) + " (" +
                    status.projector_version + ")");
    const nlohmann::json queue = status.queue.ToJson();
    lines.push_back("队列: " + std::to_string(queue.at("size_items").get<std::uint64_t>()) + "/" +
                    std::to_string(queue.at("capacity_items").get<std::uint64_t>()) +
                    " 项, 丢弃 " +
                    std::to_string(queue.at("dropped_overflow_total").get<std::uint64_t>() +
                                   queue.at("dropped_preempted_total").get<std::uint64_t>()) +
                    " 批");
    lines.push_back("spool: " + std::to_string(status.spool.segments) + " 段 " +
                    std::to_string(status.spool.bytes) + " 字节, 待发 " +
                    std::to_string(status.spool.active_batches) + " 批" +
                    (status.spool.degraded ? " [降级: 磁盘帽]" : ""));
    lines.push_back("流: " + std::to_string(status.streams.size()) + " 条在册");
    return lines;
}

std::vector<std::string> FormatTelemetryDoctorLines(const TelemetryServiceStatus& status) {
    std::vector<std::string> lines = FormatTelemetryStatusLines(status);
    const nlohmann::json queue = status.queue.ToJson();
    lines.push_back("  并系合并: " +
                    std::to_string(queue.at("coalesced_series_total").get<std::uint64_t>()) +
                    " 系; 紧急回绝(P0): " +
                    std::to_string(queue.at("emergency_reject_total").get<std::uint64_t>()) +
                    " 批");
    lines.push_back("  恢复账: active 补封 " +
                    std::to_string(status.recovery.sealed_from_active) + " 段, 隔离 " +
                    std::to_string(status.recovery.quarantined_batches) + " 批, 孤段重造 " +
                    std::to_string(status.recovery.orphan_segments) + " 段");
    lines.push_back("  丢 wake 计数: " + std::to_string(status.missed_wakes));
    for (const TelemetryServiceStatus::StreamStatus& stream : status.streams) {
        lines.push_back("  流 " + stream.session_id + "/" + stream.stream_id + ": 投影到 " +
                        (stream.last_event_id.empty() ? std::string("(头)") : stream.last_event_id) +
                        ", 滞后 " + std::to_string(stream.lag_events) + " 事件" +
                        (stream.error_code.empty() ? "" : (" [停: " + stream.error_code + "]")) +
                        (stream.final_flushed ? " [已收口]" : ""));
    }
    if (status.spool.last_error_code.empty()) {
        lines.push_back("  spool 错误: 无");
    } else {
        lines.push_back("  spool 错误: " + status.spool.last_error_code);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// 装配工厂
// ---------------------------------------------------------------------------

std::unique_ptr<TelemetryService> TryAssembleTelemetryService(
    const TelemetryAssemblyInputs& inputs, std::string* note) {
    const TelemetryActivation activation =
        ResolveTelemetryActivation(inputs.config_telemetry, inputs.config_trajectory);
    if (note != nullptr) {
        *note = activation.reason_code;
    }
    if (!activation.enabled()) {
        // §8.5:非 Active 一行遥测代码不跑——不建目录、不起线程、零副作用。
        return nullptr;
    }
    auto service = std::make_unique<TelemetryService>(inputs.options);
    service->Start();
    return service;
}

}  // namespace lubancode::telemetry
