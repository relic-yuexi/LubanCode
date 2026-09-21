// OwnerLock(SV-01,2026-09-21 架构审查)的实现件:目录锁所有权按
// PID+进程起始 token+随机 owner token 核,不认 mtime 年龄。合同见
// project_memory.hpp。SV-09 拆分前住 project_memory.cpp,搬来一字未动;
// AcquireDirLockWithRetry/ProjectLockRefusal 是锁的人话与等锁件,供
// worker_queue(队列独占)与 topic_store(层内写互斥)共用。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <share.h>
#endif

#include "hooks/hash.hpp"  // Sha256Hex:owner token
#include "memory/internal.hpp"
#include "memory/project_memory.hpp"
#include "platform/atomic_write.hpp"
#include "platform/process.hpp"     // CurrentProcessId
#include "platform/wall_clock.hpp"  // WallClockNowMs
#include "trajectory/session_lock.hpp"  // 锁持有者身份核(PID+进程起始 token)

namespace lubancode::memory {

namespace {

namespace fs = std::filesystem;

// owner 账:锁目录里的 owner 文件(JSON)。读侧宽容未知字段——新版本给
// 账加字段,不把老二进制顶成"读不懂"。
struct LockOwnerRecord {
    unsigned long pid = 0;
    std::string start_token;
    std::string owner_token;
};

// 无 owner 的锁目录多老才算"旧格式/陈"——更年轻的视作对手"建目录与写
// owner 账之间"的在建窗口,按持有保守拒(释放删一半的残迹也走这道门
// 自愈)。取 2 秒:worker.lock 的等锁轮 50x50ms 撑得过整个窗口。
constexpr auto kOwnerEstablishingWindow = std::chrono::seconds(2);

// 读 owner 账。文件在但开不进来/读不懂 → nullopt + 人话(error 透出)。
// Windows 防病毒/过滤驱动的短拒:同一拍有界重开,烧完才算真失败。
std::optional<LockOwnerRecord> ReadLockOwner(const fs::path& owner_file, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<LockOwnerRecord>{};
    };
    std::string text;
    bool opened = false;
    for (int attempt = 0; attempt < 3 && !opened; ++attempt) {
        std::ifstream file(owner_file, std::ios::binary);
        if (file.is_open()) {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            text = buffer.str();
            opened = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!opened) return fail("owner 文件开不进来(重试后仍失败): " + PathUtf8(owner_file));
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) return fail("owner 不是合法 JSON: " + PathUtf8(owner_file));
    if (!json.is_object() || !json.contains("pid") || !json.at("pid").is_number_unsigned()) {
        return fail("owner 缺合格 pid: " + PathUtf8(owner_file));
    }
    LockOwnerRecord record;
    record.pid = static_cast<unsigned long>(json.at("pid").get<std::uint64_t>());
    if (record.pid == 0) return fail("owner 的 pid 不能是 0: " + PathUtf8(owner_file));
    if (json.contains("process_start_token") && json.at("process_start_token").is_string()) {
        record.start_token = json.at("process_start_token").get<std::string>();
    }
    if (json.contains("owner_token") && json.at("owner_token").is_string()) {
        record.owner_token = json.at("owner_token").get<std::string>();
    }
    return record;
}

// 隔离留证:整目录改名 <名>.stale-<ms>,不猜死后直接删(旧格式无 owner
// 的锁与死透持有者的陈锁同款待遇)。挪不动(真占用/瞬态短拒)有界重试,
// 烧完认失败。
bool QuarantineLockDir(const fs::path& dir, fs::path* moved_to, std::string* error) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const fs::path stale =
            dir.parent_path() / Utf8Path(PathUtf8(dir.filename()) + ".stale-" +
                                         std::to_string(platform::WallClockNowMs()));
        std::error_code ec;
        fs::rename(dir, stale, ec);
        if (!ec) {
            if (moved_to != nullptr) *moved_to = stale;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (error != nullptr) {
        *error = "陈锁隔离不成(改名有界重试后仍失败): " + PathUtf8(dir);
    }
    return false;
}

// 随机 owner token:同一进程先后两只句柄也分得开,释放核账靠它。
std::string NewOwnerToken() {
    static std::atomic<unsigned long long> token_sequence{0};
    const std::string material =
        std::to_string(platform::CurrentProcessId()) + "|" +
        trajectory::CurrentProcessStartToken() + "|" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "|" +
        std::to_string(token_sequence.fetch_add(1));
    return hooks::Sha256Hex(material).substr(0, 16);
}

}  // namespace

OwnerLock::Result OwnerLock::TryAcquire(const fs::path& dir, OwnerLock* out) {
    Result result;
    if (out == nullptr) {
        result.detail = "out 指针为空";
        return result;
    }
    out->Release();
    std::string last_error = "反复撞(陈锁隔离后仍占不到位)";
    std::string cleared_note;
    // 每轮头一步都是 create_directory 原子占位——双进程同时走到这里,OS
    // 保证至多一只成功,互斥不依赖读写的先后顺序。
    for (int attempt = 0; attempt < 5; ++attempt) {
        // 尽力建锁的父目录(旧 DirectoryLock 同款;建不成由占位报真错)。
        std::error_code parent_ec;
        fs::create_directories(dir.parent_path(), parent_ec);
        std::error_code ec;
        const bool created = fs::create_directory(dir, ec);
        if (!ec && created) {
            const fs::path owner_file = dir / "owner";
            const std::string token = NewOwnerToken();
            nlohmann::json record = nlohmann::json::object();
            record["schema_version"] = 1;
            record["pid"] = platform::CurrentProcessId();
            record["process_start_token"] = trajectory::CurrentProcessStartToken();
            record["owner_token"] = token;
            record["acquired_at_ms"] = platform::WallClockNowMs();
            // AtomicVisibility 足够:进程崩了丢 owner 账,残留空目录走
            // "无 owner 隔离"那道门自愈,不靠 fsync 保命。
            const auto written = platform::AtomicWriteFile(owner_file, record.dump());
            if (!written.has_value()) {
                std::error_code remove_ec;
                fs::remove_all(dir, remove_ec);  // 自己刚建的目录,拆掉不碰别人
                result.detail = "owner 账写不进: " + written.error().message;
                return result;
            }
            // Windows 上攥住 owner 的只读句柄,他者删不动这个文件、也就拆
            // 不动整个锁目录(死持有者的句柄由内核回收,隔离照常走得通);
            // POSIX 无此语义,开了也无害。开不上不影响所有权——账已落盘。
            std::FILE* handle = nullptr;
#ifdef _WIN32
            handle = _wfsopen(owner_file.c_str(), L"rb", _SH_DENYNO);
#else
            handle = std::fopen(owner_file.c_str(), "rb");
#endif
            out->dir_ = dir;
            out->file_ = handle;
            out->owner_token_ = token;
            result.status = Status::Acquired;
            result.detail = std::move(cleared_note);
            return result;
        }
        if (ec) {
            std::error_code probe_ec;
            if (!fs::exists(dir, probe_ec) || probe_ec) {
                // 建目录真失败(权限/瞬态):有界重试。
                last_error = "建锁目录失败: " + ec.message();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            // 报错但目录在(或同名文件占着路径):落下去核 owner。
        }
        // 没占上:读 owner 账,核身份再定去留。
        const fs::path owner_file = dir / "owner";
        std::error_code owner_ec;
        if (fs::exists(owner_file, owner_ec)) {
            std::string read_error;
            const auto record = ReadLockOwner(owner_file, &read_error);
            if (!record.has_value()) {
                // owner 在但读不懂:不敢动,明报(看不懂就更不能删)。
                result.status = Status::BrokenLock;
                result.detail = read_error;
                return result;
            }
            const trajectory::SessionLockOwner probe{record->pid, record->start_token, 0};
            if (trajectory::ProbeLockHolder(probe) == trajectory::LockHolderState::Alive) {
                result.status = Status::HeldByLiveHolder;
                result.detail = "持有者活着: pid " + std::to_string(record->pid);
                return result;
            }
            // 死透/PID 被复用:陈锁,整目录隔离留证再抢,不直接删。
            fs::path moved;
            std::string quarantine_error;
            if (!QuarantineLockDir(dir, &moved, &quarantine_error)) {
                result.detail = quarantine_error;
                return result;
            }
            cleared_note = "陈锁已隔离留证: " + PathUtf8(moved);
            continue;
        }
        if (owner_ec) {
            // 探不出有没有 owner:按活保守拒。
            result.status = Status::HeldByLiveHolder;
            result.detail = "锁目录在,探不出 owner: " + owner_ec.message();
            return result;
        }
        // 没有 owner 文件:要么旧格式锁(老 DirectoryLock 留下的空目录),
        // 要么对手刚占住目录还没写完账。年轻的按在建拒;老的按旧格式隔离
        // 明报——都不猜死后直接删。
        std::error_code mtime_ec;
        const auto modified = fs::last_write_time(dir, mtime_ec);
        if (mtime_ec) {
            result.status = Status::HeldByLiveHolder;  // 在但读不出时间:按持有处理
            result.detail = "锁目录在但读不出时间: " + mtime_ec.message();
            return result;
        }
        if (fs::file_time_type::clock::now() - modified < kOwnerEstablishingWindow) {
            result.status = Status::HeldByLiveHolder;
            result.detail = "锁正在建立(占目录与写 owner 之间的窗口)";
            return result;
        }
        fs::path moved;
        std::string quarantine_error;
        if (!QuarantineLockDir(dir, &moved, &quarantine_error)) {
            result.detail = quarantine_error;
            return result;
        }
        cleared_note = "旧格式锁(无 owner)已隔离留证: " + PathUtf8(moved);
        continue;
    }
    result.detail = last_error + ": " + PathUtf8(dir);
    return result;
}

bool OwnerLock::HolderAlive(const fs::path& dir, std::string* detail) {
    const auto say = [detail](const std::string& note) {
        if (detail != nullptr) *detail = note;
    };
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        say("锁目录不在");
        return false;
    }
    const fs::path owner_file = dir / "owner";
    std::error_code owner_ec;
    const bool has_owner = fs::exists(owner_file, owner_ec);
    if (owner_ec) {
        say("锁目录在,探不出 owner,按持有保守");
        return true;
    }
    if (!has_owner) {
        std::error_code mtime_ec;
        const auto modified = fs::last_write_time(dir, mtime_ec);
        if (mtime_ec) {
            say("锁目录在但读不出时间,按持有保守");
            return true;
        }
        if (fs::file_time_type::clock::now() - modified < kOwnerEstablishingWindow) {
            say("锁正在建立");
            return true;
        }
        say("旧格式锁(无 owner)");
        return false;  // 交给取锁路隔离
    }
    std::string read_error;
    const auto record = ReadLockOwner(owner_file, &read_error);
    if (!record.has_value()) {
        say("owner 读不懂,按持有保守: " + read_error);
        return true;
    }
    const trajectory::SessionLockOwner probe{record->pid, record->start_token, 0};
    if (trajectory::ProbeLockHolder(probe) == trajectory::LockHolderState::Alive) {
        say("持有者活着: pid " + std::to_string(record->pid));
        return true;
    }
    say("持有者已死: pid " + std::to_string(record->pid));
    return false;
}

OwnerLock::OwnerLock(OwnerLock&& other) noexcept
    : dir_(std::move(other.dir_)),
      file_(other.file_),
      owner_token_(std::move(other.owner_token_)) {
    other.file_ = nullptr;
    other.dir_.clear();
}

OwnerLock& OwnerLock::operator=(OwnerLock&& other) noexcept {
    if (this != &other) {
        Release();
        dir_ = std::move(other.dir_);
        file_ = other.file_;
        owner_token_ = std::move(other.owner_token_);
        other.file_ = nullptr;
        other.dir_.clear();
    }
    return *this;
}

OwnerLock::~OwnerLock() { Release(); }

void OwnerLock::Release() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (dir_.empty()) return;
    // 释放前核 owner:pid+owner token 都对上才删。账被人接管过(陈锁被隔
    // 离后他者重建、账被改写),这把锁已是别人的——旧句柄不得删掉新持有
    // 者的锁。
    const auto record = ReadLockOwner(dir_ / "owner", nullptr);
    if (record.has_value() && record->pid == platform::CurrentProcessId() &&
        !owner_token_.empty() && record->owner_token == owner_token_) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::error_code ec;
            (void)fs::remove_all(dir_, ec);
            if (!ec) break;
            // Windows 瞬态(防病毒/过滤驱动短拒):有界重试。
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        // 删不动只剩留证;残留由下次 TryAcquire 的身份核收口。
    }
    dir_.clear();
    owner_token_.clear();
}

// 有界等锁:活持有者放手要时间,烧完仍撞才回失败;BrokenLock 是真失败,
// 即刻回,不磨。worker.lock 的 50x50ms 轮在调用点自备,这把给 memory.lock
// (跨工作区的用户层 job 会撞同一把)。
OwnerLock::Result AcquireDirLockWithRetry(const fs::path& dir, OwnerLock* out, int attempts,
                                          int interval_ms) {
    OwnerLock::Result result;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        result = OwnerLock::TryAcquire(dir, out);
        if (result.status == OwnerLock::Status::Acquired ||
            result.status == OwnerLock::Status::BrokenLock) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return result;
}

// 锁取不上的人话:持有者在 → 沿用"正由另一个 worker 更新"老口径(带
// 细节);owner 读不懂 → 明报不敢动;其余原样端出来。
std::string ProjectLockRefusal(const OwnerLock::Result& result, const char* what) {
    switch (result.status) {
    case OwnerLock::Status::HeldByLiveHolder:
        return std::string(what) + "正由另一个 worker 更新(" + result.detail + ")";
    case OwnerLock::Status::BrokenLock:
        return std::string(what) + "锁 owner 账读不懂,不敢动: " + result.detail;
    case OwnerLock::Status::Acquired:
        return std::string();
    case OwnerLock::Status::IoError:
        break;
    }
    return std::string(what) + "锁取不上: " + result.detail;
}

}  // namespace lubancode::memory
