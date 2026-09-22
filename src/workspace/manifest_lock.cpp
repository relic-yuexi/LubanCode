// SV-11:workspace.json 读改写事务锁的实现。声明与合同见 manifest_lock.hpp。
#include "workspace/manifest_lock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "platform/wall_clock.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <share.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lubancode::workspace {
namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;
using platform::Utf8ToPath;

// ---------------------------------------------------------------------------
// 进程身份探针(与 trajectory/session_lock.cpp 同形;分层不许 workspace
// 反向 include trajectory,各持一份,收编由集成者协调——见 hpp 注记)。
// ---------------------------------------------------------------------------

#ifdef _WIN32
// Windows 起始 token:进程 creation FILETIME 的 64 位十六进制。
std::string StartTokenFromHandle(HANDLE process) {
    if (process == nullptr) {
        return {};
    }
    FILETIME creation = {};
    FILETIME exit_time = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!GetProcessTimes(process, &creation, &exit_time, &kernel, &user)) {
        return {};
    }
    ULARGE_INTEGER value;
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    if (value.QuadPart == 0) {
        return {};
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llx",
                  static_cast<unsigned long long>(value.QuadPart));
    return buffer;
}
#endif

std::string ProcessStartTokenOf(unsigned long pid) {
    if (pid == 0) {
        return {};
    }
#ifdef _WIN32
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return {};
    }
    const std::string token = StartTokenFromHandle(process);
    CloseHandle(process);
    return token;
#elif defined(__APPLE__)
    // macOS 没有 /proc:sysctl KERN_PROC_PID 拿 kinfo_proc 的 p_starttime
    //(自 boot 的微秒 timeval),折成同一枚十进制文本。拿不到给空串,上层
    // 保守判活(PID 复用案探不到 token,锁抢不掉)。
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc info;
    std::size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size < sizeof(info)) {
        return {};
    }
    const std::int64_t micros =
        static_cast<std::int64_t>(info.kp_proc.p_starttime.tv_sec) * 1000000 +
        info.kp_proc.p_starttime.tv_usec;
    return std::to_string(micros);
#else
    // /proc/<pid>/stat 第 22 字段 starttime(自 boot 的时钟滴答)。comm 字段
    // 可能含空格与括号,先找最后一个 ')' 再从尾巴取。
    std::ifstream file(fs::path("/proc") / std::to_string(pid) / "stat");
    if (!file.is_open()) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) {
        return {};
    }
    std::istringstream tail(line.substr(close + 1));
    std::string field;
    // 尾巴第 1 个字段是 state(全表第 3);starttime 是全表第 22 → 尾巴第 20。
    for (int index = 1; index <= 20; ++index) {
        if (!(tail >> field)) {
            return {};
        }
    }
    return field;
#endif
}

std::string CurrentProcessStartToken() {
#ifdef _WIN32
    return StartTokenFromHandle(GetCurrentProcess());
#else
    return ProcessStartTokenOf(platform::CurrentProcessId());
#endif
}

enum class HolderState { Alive, Dead };

// 身份核对:先探活,再比起始 token。token 对不上 = 这个 PID 已经换成了
// 另一个进程(PID 复用),锁是上个世纪的残留,判死。任一侧 token 探不到,
// 只凭活死保守判活。
HolderState ProbeHolder(unsigned long pid, const std::string& start_token) {
    if (pid == 0) {
        return HolderState::Dead;
    }
    if (!platform::IsProcessAlive(pid)) {
        return HolderState::Dead;
    }
    const bool same_process = pid == platform::CurrentProcessId();
    const std::string actual =
        same_process ? CurrentProcessStartToken() : ProcessStartTokenOf(pid);
    if (actual.empty() || start_token.empty()) {
        return HolderState::Alive;
    }
    return actual == start_token ? HolderState::Alive : HolderState::Dead;
}

// ---------------------------------------------------------------------------
// owner 账与锁目录机械(范式同 memory::OwnerLock,按 manifest 场景裁)。
// ---------------------------------------------------------------------------

// owner 账:锁目录里的 owner 文件(JSON)。读侧宽容未知字段——新版本给账
// 加字段,不把老二进制顶成"读不懂"。
struct LockOwnerRecord {
    unsigned long pid = 0;
    std::string start_token;
    std::string owner_token;
};

// 无 owner 的锁目录多老才算"旧格式/陈"——更年轻的视作对手"建目录与写
// owner 账之间"的在建窗口,按持有保守拒。取 2 秒:开房路的有界等锁
// 撑得过整个窗口(等锁档见 manifest.cpp kLockWait*)。
constexpr auto kOwnerEstablishingWindow = std::chrono::seconds(2);

// owner 读档的短拒重试档(10×25ms=250ms 预算,首试即中则零等待)。档的
// 依据:windows-msvc 腿三案间歇红(2026-09 run 35611620658 att1 /
// 35668147611 att1 / 35667534366 att6,均挂 ledger 册并发开房段)的病灶
// 之一——防病毒/过滤驱动对 owner 的拦截窗实测可达几十毫秒(manifest.cpp
// kTransientRead* 同一宗实测),旧档 3×25ms=75ms 装不下,耗尽后被折成
// BrokenLock 死拒(Acquire 对它不磨),一次短拒就把整段排队顶翻。250ms
// 给足 5 倍余量;POSIX 无共享违例,重试路径零开销零行为变化。
constexpr int kOwnerReadAttempts = 10;
constexpr auto kOwnerReadBackoff = std::chrono::milliseconds(25);

// 读 owner 账。文件在但开不进来/读不懂 → nullopt + 人话(error 透出);
// vanished 置真 = 打开失败中途文件已消失(持有者正释放:remove_all 先删
// owner 后拆目录的窗)——这不是"读不懂",调用方按无 owner 路重新裁决,
// 不许折 BrokenLock 终局拒绝。
std::optional<LockOwnerRecord> ReadLockOwner(const fs::path& owner_file, std::string* error,
                                             bool* vanished = nullptr) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<LockOwnerRecord>{};
    };
    std::string text;
    bool opened = false;
    for (int attempt = 0; attempt < kOwnerReadAttempts && !opened; ++attempt) {
        std::ifstream file(owner_file, std::ios::binary);
        if (file.is_open()) {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            text = buffer.str();
            opened = true;
            break;
        }
        std::error_code gone_ec;
        if (!fs::exists(owner_file, gone_ec) && !gone_ec) {
            if (vanished != nullptr) *vanished = true;
            return fail("owner 文件在读取途中消失(持有者释放竞态): " +
                        PathToUtf8(owner_file));
        }
        std::this_thread::sleep_for(kOwnerReadBackoff);
    }
    if (!opened) return fail("owner 文件开不进来(重试后仍失败): " + PathToUtf8(owner_file));
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) return fail("owner 不是合法 JSON: " + PathToUtf8(owner_file));
    if (!json.is_object() || !json.contains("pid") || !json.at("pid").is_number_unsigned()) {
        return fail("owner 缺合格 pid: " + PathToUtf8(owner_file));
    }
    LockOwnerRecord record;
    record.pid = static_cast<unsigned long>(json.at("pid").get<std::uint64_t>());
    if (record.pid == 0) return fail("owner 的 pid 不能是 0: " + PathToUtf8(owner_file));
    if (json.contains("process_start_token") && json.at("process_start_token").is_string()) {
        record.start_token = json.at("process_start_token").get<std::string>();
    }
    if (json.contains("owner_token") && json.at("owner_token").is_string()) {
        record.owner_token = json.at("owner_token").get<std::string>();
    }
    return record;
}

// 隔离留证:整目录改名 .manifest.lock.stale-<ms>,不猜死后直接删(旧格式
// 无 owner 的锁与死透持有者的陈锁同款待遇)。挪不动(真占用/瞬态短拒)
// 有界重试,烧完认失败。
bool QuarantineLockDir(const fs::path& dir, fs::path* moved_to, std::string* error) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const fs::path stale =
            dir.parent_path() /
            Utf8ToPath(PathToUtf8(dir.filename()) + ".stale-" +
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
        *error = "陈锁隔离不成(改名有界重试后仍失败): " + PathToUtf8(dir);
    }
    return false;
}

// 随机 owner token:同一进程先后两只句柄也分得开,释放核账靠它。
std::string NewOwnerToken() {
    static std::atomic<unsigned long long> token_sequence{0};
    const std::string material =
        std::to_string(platform::CurrentProcessId()) + "|" + CurrentProcessStartToken() +
        "|" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "|" + std::to_string(token_sequence.fetch_add(1));
    return platform::Sha256Hex(material).substr(0, 16);
}

}  // namespace

fs::path ManifestLockDir(const fs::path& workspace_dir) {
    return workspace_dir / ".manifest.lock";
}

ManifestLock::Result ManifestLock::TryAcquire(const fs::path& workspace_dir, ManifestLock* out) {
    Result result;
    if (out == nullptr) {
        result.detail = "out 指针为空";
        return result;
    }
    out->Release();
    const fs::path dir = ManifestLockDir(workspace_dir);
    std::string last_error = "反复撞(陈锁隔离后仍占不到位)";
    std::string cleared_note;
    // 每轮头一步都是 create_directory 原子占位——双进程同时走到这里,OS
    // 保证至多一只成功,互斥不依赖读写的先后顺序。
    for (int attempt = 0; attempt < 5; ++attempt) {
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
            record["process_start_token"] = CurrentProcessStartToken();
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
            bool vanished = false;
            const auto record = ReadLockOwner(owner_file, &read_error, &vanished);
            if (!record.has_value() && vanished) {
                // 持有者释放竞态(owner 先删、目录后拆):不是坏锁。退回去
                // 下一轮重占——目录拆完即得手;残留空壳落 mtime/旧格式门。
                std::this_thread::sleep_for(kOwnerReadBackoff);
                continue;
            }
            if (!record.has_value()) {
                // owner 在但读不懂:不敢动,明报(看不懂就更不能删)。
                result.status = Status::BrokenLock;
                result.detail = read_error;
                return result;
            }
            if (ProbeHolder(record->pid, record->start_token) == HolderState::Alive) {
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
            cleared_note = "陈锁已隔离留证: " + PathToUtf8(moved);
            continue;
        }
        if (owner_ec) {
            // 探不出有没有 owner:按活保守拒。
            result.status = Status::HeldByLiveHolder;
            result.detail = "锁目录在,探不出 owner: " + owner_ec.message();
            return result;
        }
        // 没有 owner 文件:要么对手刚占住目录还没写完账,要么残留的空锁
        // 目录。年轻的按在建拒;老的按旧格式隔离明报——都不猜死后直接删。
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
        cleared_note = "旧格式锁(无 owner)已隔离留证: " + PathToUtf8(moved);
        continue;
    }
    result.detail = last_error + ": " + PathToUtf8(dir);
    return result;
}

ManifestLock::Result ManifestLock::Acquire(const fs::path& workspace_dir, ManifestLock* out,
                                           int attempts, int interval_ms) {
    Result result;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        result = TryAcquire(workspace_dir, out);
        if (result.status == Status::Acquired || result.status == Status::BrokenLock) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return result;
}

// 无 owner 账(或 owner 读取途中消失)时的裁决:目录年轻=在建窗口,按
// 持有;老=旧格式锁,交取锁路隔离。探不出时间按持有保守。
bool OwnerlessHeld(const fs::path& dir, std::string* note) {
    std::error_code mtime_ec;
    const auto modified = fs::last_write_time(dir, mtime_ec);
    if (mtime_ec) {
        if (note != nullptr) *note = "锁目录在但读不出时间,按持有保守";
        return true;
    }
    if (fs::file_time_type::clock::now() - modified < kOwnerEstablishingWindow) {
        if (note != nullptr) *note = "锁正在建立";
        return true;
    }
    if (note != nullptr) *note = "旧格式锁(无 owner)";
    return false;
}

bool ManifestLock::HolderAlive(const fs::path& workspace_dir, std::string* detail) {
    const auto say = [detail](const std::string& note) {
        if (detail != nullptr) *detail = note;
    };
    const fs::path dir = ManifestLockDir(workspace_dir);
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
        return OwnerlessHeld(dir, detail);  // 交给取锁路隔离
    }
    std::string read_error;
    bool vanished = false;
    const auto record = ReadLockOwner(owner_file, &read_error, &vanished);
    if (!record.has_value() && vanished) {
        // owner 在读取途中消失(持有者释放竞态):按无 owner 的 mtime 重新
        // 裁决,不按"读不懂"保守判活。
        return OwnerlessHeld(dir, detail);
    }
    if (!record.has_value()) {
        say("owner 读不懂,按持有保守: " + read_error);
        return true;
    }
    if (ProbeHolder(record->pid, record->start_token) == HolderState::Alive) {
        say("持有者活着: pid " + std::to_string(record->pid));
        return true;
    }
    say("持有者已死: pid " + std::to_string(record->pid));
    return false;
}

ManifestLock::ManifestLock(ManifestLock&& other) noexcept
    : dir_(std::move(other.dir_)),
      file_(other.file_),
      owner_token_(std::move(other.owner_token_)) {
    other.file_ = nullptr;
    other.dir_.clear();
}

ManifestLock& ManifestLock::operator=(ManifestLock&& other) noexcept {
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

ManifestLock::~ManifestLock() { Release(); }

void ManifestLock::Release() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (dir_.empty()) return;
    // 释放前核 owner:pid + owner token 都对上才删。账被人接管过(陈锁被
    // 隔离后他者重建、账被改写),这把锁已是别人的——旧句柄不得删掉新持
    // 有者的锁。
    const auto record = ReadLockOwner(dir_ / "owner", nullptr);
    if (record.has_value() && record->pid == platform::CurrentProcessId() &&
        !owner_token_.empty() && record->owner_token == owner_token_) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::error_code ec;
            (void)fs::remove_all(dir_, ec);
            if (!ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    dir_.clear();
    owner_token_.clear();
}

}  // namespace lubancode::workspace
