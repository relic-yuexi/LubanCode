#pragma once

// Private supervisor persistence and scheduling. These helpers consume public
// SDK snapshots only; they never open the SDK trajectory or artifact layout.
#include <lubancore/core.hpp>
#include <lubancore/results.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lubancode::worker_host::result_support {
using Json = nlohmann::json;
namespace result = lubancore::results::v1;
inline constexpr std::size_t kFrameBytes = 1024 * 1024;
// Trusted local read budget; the emitted projection and IPC frame have separate
// limits. Use the same budget on first projection and restoration.
inline constexpr std::size_t kSnapshotTextBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kStorageBytes = 4 * 1024 * 1024;
inline std::filesystem::path Path(std::string_view utf8) { return std::filesystem::u8path(utf8.begin(), utf8.end()); }
struct Failure : std::runtime_error {
    explicit Failure(std::string code, std::string sdk = {}) : std::runtime_error(std::move(code)), sdk_code(std::move(sdk)) {}
    std::string sdk_code;
};
inline std::string ModeName(result::Mode mode) { return mode == result::Mode::Full ? "full" : "preview"; }
inline Json Identity(const result::ToolResultIdentity& id) {
    return {{"session_id", id.session_id}, {"operation_id", id.operation_id}, {"turn_id", id.turn_id},
            {"tool_call_id", id.tool_call_id}, {"persisted_event_id", id.persisted_event_id}, {"result_id", id.result_id}};
}
inline bool Formal(const result::ToolResultSummary& item) {
    return item.selected && item.identity.result_id.starts_with("res-");
}
// File routing only, not integrity or authentication. The stored full identity
// and SDK restoration checks reject collisions instead of mixing records.
inline std::string FileKey(std::string_view value) {
    std::uint64_t a = 14695981039346656037ULL, b = 7809847782465536322ULL;
    for (unsigned char c : value) { a = (a ^ c) * 1099511628211ULL; b = (b ^ c) * 14029467366897019727ULL; }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << a << std::setw(16) << b;
    return out.str();
}
inline std::string ReadFile(const std::filesystem::path& file, std::size_t maximum) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) throw Failure("worker.result_record_unavailable");
    auto size = std::filesystem::file_size(file, ec);
    if (ec || size > maximum) throw Failure("worker.result_record_invalid");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw Failure("worker.result_record_unavailable");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) throw Failure("worker.result_record_invalid");
    return bytes;
}
inline void AtomicWrite(const std::filesystem::path& file, const std::string& bytes) {
    if (bytes.size() > kStorageBytes) throw Failure("worker.result_storage_too_large");
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) throw Failure("worker.result_storage_failed");
    const auto temp = std::filesystem::path(file.native() + std::filesystem::path(".pending").native());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{temp};
#ifdef _WIN32
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw Failure("worker.result_storage_failed");
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                    && written == bytes.size() && FlushFileBuffers(handle);
    const bool closed = CloseHandle(handle) != 0;
    if (!ok || !closed || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Failure("worker.result_storage_failed");
#else
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw Failure("worker.result_storage_failed");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) { ::close(fd); throw Failure("worker.result_storage_failed"); }
        offset += static_cast<std::size_t>(written);
    }
    const bool flushed = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!flushed || !closed || ::rename(temp.c_str(), file.c_str()) != 0) throw Failure("worker.result_storage_failed");
    fd = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw Failure("worker.result_storage_failed");
    const bool synced = ::fsync(fd) == 0;
    const bool directory_closed = ::close(fd) == 0;
    if (!synced || !directory_closed) throw Failure("worker.result_storage_failed");
#endif
}
inline std::filesystem::path Directory(const std::filesystem::path& root, const std::string& session_id) {
    return root / "worker-result-policies" / FileKey(session_id);
}
// Serialize supervisor records even when an old closed handle and a resumed
// worker both exist. No unbounded lock wait can obstruct shutdown.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& directory) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) throw Failure("worker.result_storage_failed");
        const auto path = directory / "supervisor.lock";
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw Failure("worker.result_storage_busy");
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) throw Failure("worker.result_storage_failed");
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) { ::close(fd_); fd_ = -1; throw Failure("worker.result_storage_busy"); }
#endif
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};
inline Json ReadManifest(const std::filesystem::path& root, const std::string& session_id) {
    auto file = Directory(root, session_id) / "session.json";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) throw Failure("worker.result_policy_missing");
    auto value = Json::parse(ReadFile(file, kStorageBytes), nullptr, false);
    if (!value.is_object() || value.size() != 6 || !value.contains("schema")
        || value["schema"] != "luban-worker.result-policy.v1" || !value.contains("session_id")
        || !value["session_id"].is_string() || value["session_id"].get_ref<const std::string&>() != session_id
        || !value.contains("records") || !value["records"].is_array()
        || value["records"].size() > 4096 || !value.contains("session_policy_version")
        || !value["session_policy_version"].is_number_unsigned()
        || value["session_policy_version"].get<std::uint64_t>() == 0
        || !value.contains("mode") || (value["mode"] != "preview" && value["mode"] != "full")
        || !value.contains("binding") || !value["binding"].is_string()
        || value["binding"].get_ref<const std::string&>().size() != 64) throw Failure("worker.result_policy_invalid");
    std::set<std::string> identities;
    for (const auto& id : value["records"]) {
        if (!id.is_object() || id.size() != 6 || !id.contains("session_id") || !id["session_id"].is_string()
            || id["session_id"].get_ref<const std::string&>() != session_id)
            throw Failure("worker.result_policy_invalid");
        for (const char* field : {"session_id", "operation_id", "turn_id", "tool_call_id", "persisted_event_id", "result_id"})
            if (!id.contains(field) || !id[field].is_string() || id[field].get_ref<const std::string&>().empty()
                || id[field].get_ref<const std::string&>().size() > 200) throw Failure("worker.result_policy_invalid");
        if (!identities.insert(id.dump()).second) throw Failure("worker.result_policy_invalid");
    }
    return value;
}

class Store {
public:
    Store(std::filesystem::path root, result::SessionResultPolicy policy,
          std::shared_ptr<result::ResultProjector> projector, bool resume)
        : root_(std::move(root)), policy_(std::move(policy)), directory_(Directory(root_, policy_.session_id)), projector_(std::move(projector)) {
        FileLock file_lock(directory_);
        if (resume) {
            manifest_ = ReadManifest(root_, policy_.session_id);
            if (manifest_["mode"].get_ref<const std::string&>() != ModeName(policy_.mode)
                || manifest_["session_policy_version"].get<std::uint64_t>() != policy_.version
                || manifest_["binding"].get_ref<const std::string&>() != projector_->BindingFingerprint())
                throw Failure("worker.result_policy_conflict");
        } else {
            manifest_ = {{"schema", "luban-worker.result-policy.v1"}, {"session_id", policy_.session_id},
                         {"mode", ModeName(policy_.mode)}, {"session_policy_version", policy_.version},
                         {"binding", projector_->BindingFingerprint()}, {"records", Json::array()}};
            std::error_code ec;
            if (std::filesystem::exists(directory_ / "session.json", ec) || ec) throw Failure("worker.result_policy_conflict");
            AtomicWrite(directory_ / "session.json", manifest_.dump());
        }
    }
    Json Read(const result::SavedSnapshot& snapshot) {
        std::lock_guard lock(mutex_);
        FileLock file_lock(directory_);
        manifest_ = ReadManifest(root_, policy_.session_id);
        if (manifest_["mode"].get_ref<const std::string&>() != ModeName(policy_.mode)
            || manifest_["session_policy_version"].get<std::uint64_t>() != policy_.version
            || manifest_["binding"].get_ref<const std::string&>() != projector_->BindingFingerprint())
            throw Failure("worker.result_policy_conflict");
        const auto identity = Identity(snapshot.result().summary.identity);
        const auto file = directory_ / (FileKey(identity.dump()) + ".projection.json");
        const bool registered = std::find(manifest_["records"].begin(), manifest_["records"].end(), identity)
                                != manifest_["records"].end();
        // Admission precedes projection and filesystem writes. A full register
        // must not leave an unbounded series of new, unregistered files behind.
        if (!registered && manifest_["records"].size() == 4096) throw Failure("worker.result_record_limit");
        std::error_code ec;
        const bool exists = std::filesystem::exists(file, ec);
        if (ec || (registered && !exists)) throw Failure("worker.result_record_unavailable");
        auto frozen = exists ? projector_->RestoreSavedProjection(ReadFile(file, kStorageBytes), snapshot)
                             : projector_->Project(snapshot);
        if (!frozen) throw Failure("worker.result_projection_failed", frozen.error().code);
        auto transmission = frozen->ForTransmission(*projector_);
        if (!transmission) throw Failure("worker.result_projection_failed", transmission.error().code);
        auto wire = Json::parse(*transmission, nullptr, false);
        if (!wire.is_object()) throw Failure("worker.result_projection_failed");
        // Reserve the largest allowed correlation ID before registering/sending.
        if (Json{{"id", std::string(200, 'x')}, {"result", {{"query_id", std::string(200, 'x')},
                      {"state", "ready"}, {"value", wire}}}}.dump().size() + 1 > kFrameBytes)
            throw Failure("worker.result_frame_too_large");
        if (!exists) {
            auto stored = frozen->SerializeForStorage();
            if (!stored) throw Failure("worker.result_projection_failed", stored.error().code);
            AtomicWrite(file, *stored);
        }
        if (!registered) {
            auto next = manifest_;
            next["records"].push_back(identity);
            AtomicWrite(directory_ / "session.json", next.dump());
            manifest_ = std::move(next);
        }
        return wire;
    }
private:
    std::filesystem::path root_;
    result::SessionResultPolicy policy_;
    std::filesystem::path directory_;
    std::shared_ptr<result::ResultProjector> projector_;
    Json manifest_;
    std::mutex mutex_;
};

// One owned thread. Admission/get are brief mutex operations on the parent IPC
// thread, while filesystem reads and redaction stay on this bounded executor.
class Queries {
    struct Entry {
        std::string id, session_id, client_key;
        Json specification, value, error;
        bool done = false;
        std::function<Json()> work;
    };
public:
    explicit Queries(std::string boot) : boot_(std::move(boot)), thread_([this] { Run(); }) {}
    ~Queries() { Stop(); Join(); }
    Json Start(std::string session, std::string client_key, Json specification, std::function<Json()> work) {
        std::lock_guard lock(mutex_);
        if (stopping_) throw Failure("worker.stopping");
        for (auto& [id, entry] : entries_) {
            if (entry->session_id != session || entry->client_key != client_key) continue;
            if (entry->specification != specification) throw Failure("worker.result_query_conflict");
            return {{"query_id", id}, {"duplicate", true}};
        }
        if (pending_.size() >= 8) throw Failure("worker.result_query_queue_full");
        // Process-local receipts only. Retire old terminal entries, never active
        // ones; durable replay uses the per-result record, not these query IDs.
        while (entries_.size() >= 64) {
            auto old = std::find_if(order_.begin(), order_.end(), [&](const auto& id) { return entries_.at(id)->done; });
            if (old == order_.end()) throw Failure("worker.result_query_limit");
            entries_.erase(*old); order_.erase(old);
        }
        const auto id = boot_ + ":query:" + std::to_string(++serial_);
        auto entry = std::make_shared<Entry>();
        entry->id = id; entry->session_id = std::move(session); entry->client_key = std::move(client_key);
        entry->specification = std::move(specification); entry->work = std::move(work);
        entries_.emplace(id, entry); order_.push_back(id); pending_.push_back(entry);
        ready_.notify_one();
        return {{"query_id", id}, {"duplicate", false}};
    }
    Json Get(const std::string& session, const std::string& id) {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(id);
        if (found == entries_.end() || found->second->session_id != session) throw Failure("worker.result_query_not_found");
        const auto& entry = *found->second;
        if (!entry.done) return {{"query_id", id}, {"state", "pending"}};
        if (!entry.error.is_null()) return {{"query_id", id}, {"state", "failed"}, {"error", entry.error}};
        return {{"query_id", id}, {"state", "ready"}, {"value", entry.value}};
    }
    void Stop() {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& entry : pending_) {
            entry->error = {{"code", "worker.stopping"}}; entry->done = true; entry->work = nullptr;
        }
        pending_.clear(); ready_.notify_all();
    }
    void Join() { if (thread_.joinable()) thread_.join(); }
private:
    void Run() noexcept {
        while (true) {
            std::shared_ptr<Entry> entry;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if (stopping_) return;
                entry = std::move(pending_.front()); pending_.pop_front();
            }
            Json value, error;
            try { value = entry->work(); }
            catch (const Failure& failure) {
                error = {{"code", failure.what()}};
                if (!failure.sdk_code.empty()) error["sdk_code"] = failure.sdk_code;
            }
            catch (...) { error = {{"code", "worker.result_query_failed"}}; }
            // Release SDK handles/captures outside the bookkeeping lock.
            entry->work = nullptr;
            {
                std::lock_guard lock(mutex_);
                entry->value = std::move(value); entry->error = std::move(error); entry->done = true;
            }
        }
    }
    std::string boot_;
    std::uint64_t serial_ = 0;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_ = false;
    std::deque<std::shared_ptr<Entry>> pending_;
    std::deque<std::string> order_;
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    std::thread thread_;
};
} // namespace lubancode::worker_host::result_support
