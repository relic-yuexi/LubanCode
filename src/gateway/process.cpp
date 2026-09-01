#include "gateway/process.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::gateway {

namespace {

std::optional<GatewayLockRecord> ReadLockFile(const std::filesystem::path& lock_file,
                                              std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(lock_file, ec) || ec) {
        return std::nullopt;  // 没有锁文件
    }
    std::ifstream stream(lock_file, std::ios::binary);
    if (!stream) {
        if (error != nullptr) *error = "锁文件在,但打不开";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        if (error != nullptr) *error = "锁文件是空的";
        return std::nullopt;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        if (error != nullptr) *error = "锁文件不是合法 JSON(可能写了一半)";
        return std::nullopt;
    }
    std::string parse_error;
    auto record = GatewayLockRecord::FromJsonStrict(parsed, &parse_error);
    if (!record.has_value()) {
        if (error != nullptr) *error = "锁账读不懂: " + parse_error;
        return std::nullopt;
    }
    return record;
}

// 与 channel::AccountLock 同款取舍:锁的对手是本机另一只实例的启动竞态
// (毫秒级),不是断电一致性;半写坏锁走 RefusedBrokenLock 留给人看。
bool WriteLockFile(const std::filesystem::path& lock_file, const GatewayLockRecord& record) {
    std::error_code ec;
    std::filesystem::create_directories(lock_file.parent_path(), ec);
    if (ec && !lock_file.parent_path().empty()) return false;
    std::ofstream stream(lock_file, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    const std::string text = record.ToJson().dump();
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

// ---- 信号面:handler 里只置旗,一切落账在主循环里做 ----
volatile std::sig_atomic_t g_gateway_signal_stop = 0;

void GatewaySignalHandler(int signal) {
    g_gateway_signal_stop = signal;
}

std::int64_t CallNowMs(const std::function<std::int64_t()>& now_ms) {
    return now_ms ? now_ms() : platform::WallClockNowMs();
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 100));
}

}  // namespace

// ---------------------------------------------------------------------------
// GatewayLockRecord
// ---------------------------------------------------------------------------

nlohmann::json GatewayLockRecord::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["pid"] = pid;
    json["start_token"] = start_token;
    json["boot_id"] = boot_id;
    json["acquired_at_ms"] = acquired_at_ms;
    return json;
}

std::optional<GatewayLockRecord> GatewayLockRecord::FromJsonStrict(const nlohmann::json& json,
                                                                   std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<GatewayLockRecord>{};
    };
    if (!json.is_object()) return fail("锁账必须是 JSON object");
    const std::array<const char*, 4> kRequired = {"pid", "start_token", "boot_id",
                                                  "acquired_at_ms"};
    for (const char* key : kRequired) {
        if (!json.contains(key)) return fail(std::string("缺必填字段 ") + key);
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        bool known = false;
        for (const char* key : kRequired) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) return fail("未知字段 " + it.key());
    }
    if (!json["pid"].is_number_integer() || !json["acquired_at_ms"].is_number_integer()) {
        return fail("pid/acquired_at_ms 必须是整数");
    }
    if (!json["start_token"].is_string() || !json["boot_id"].is_string()) {
        return fail("start_token/boot_id 必须是字符串");
    }
    GatewayLockRecord record;
    record.pid = static_cast<unsigned long>(json["pid"].get<std::int64_t>());
    record.start_token = json["start_token"].get<std::string>();
    record.boot_id = json["boot_id"].get<std::string>();
    record.acquired_at_ms = json["acquired_at_ms"].get<std::int64_t>();
    if (record.pid == 0) return fail("pid 不能是 0");
    return record;
}

GatewayLock::AcquireResult GatewayLock::TryAcquire(const std::filesystem::path& lock_file,
                                                   const GatewayLockRecord& self,
                                                   GatewayLock* out) {
    AcquireResult result;
    if (out == nullptr) {
        result.status = AcquireResult::Status::IoError;
        result.detail = "out 指针为空";
        return result;
    }
    out->Release();

    std::string read_error;
    const auto existing = ReadLockFile(lock_file, &read_error);
    if (existing.has_value()) {
        const GatewayLockRecord& holder = *existing;
        const bool same_instance = holder.pid == self.pid && holder.start_token == self.start_token &&
                                   holder.boot_id == self.boot_id;
        if (!same_instance) {
            // 身份核与 trajectory session lock 同一把尺:活进程且 token 对
            // 上才拒绝;死透/PID 复用 = 陈旧,清掉重拿;探不到按活保守。
            const trajectory::SessionLockOwner owner{holder.pid, holder.start_token, 0};
            if (trajectory::ProbeLockHolder(owner) == trajectory::LockHolderState::Alive) {
                result.status = AcquireResult::Status::RefusedAliveHolder;
                result.holder = holder;
                result.detail = "gateway.already_running: pid " + std::to_string(holder.pid) +
                                " 持有本 profile 的锁(boot " + holder.boot_id + ")";
                return result;
            }
            std::error_code ec;
            std::filesystem::remove(lock_file, ec);
            if (ec) {
                result.status = AcquireResult::Status::IoError;
                result.detail = "清陈旧锁 " + platform::PathToUtf8(lock_file) + " 失败: " +
                                ec.message();
                return result;
            }
        }
    } else if (!read_error.empty()) {
        // 锁文件在但读不懂:不敢删,明报(看不懂就更不能删)。
        result.status = AcquireResult::Status::RefusedBrokenLock;
        result.detail = "gateway.lock_stale: " + read_error + "(锁文件: " +
                        platform::PathToUtf8(lock_file) + ")";
        return result;
    }

    if (!WriteLockFile(lock_file, self)) {
        result.status = AcquireResult::Status::IoError;
        result.detail = "写锁文件 " + platform::PathToUtf8(lock_file) + " 失败";
        return result;
    }
    out->lock_file_ = lock_file;
    result.status = AcquireResult::Status::Acquired;
    return result;
}

GatewayLock::GatewayLock(GatewayLock&& other) noexcept : lock_file_(std::move(other.lock_file_)) {
    other.lock_file_.clear();
}

GatewayLock& GatewayLock::operator=(GatewayLock&& other) noexcept {
    if (this != &other) {
        Release();
        lock_file_ = std::move(other.lock_file_);
        other.lock_file_.clear();
    }
    return *this;
}

GatewayLock::~GatewayLock() { Release(); }

void GatewayLock::Release() {
    if (lock_file_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(lock_file_, ec);
    // 删失败只剩日志可打;残留锁由下次 TryAcquire 走身份核收口。
    lock_file_.clear();
}

// ---------------------------------------------------------------------------
// boot history
// ---------------------------------------------------------------------------

nlohmann::json GatewayBootLine::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = kind == Kind::Boot ? "boot" : "shutdown";
    json["boot_id"] = boot_id;
    json["pid"] = pid;
    json["start_token"] = start_token;
    json["at_ms"] = at_ms;
    json["reason"] = reason;
    if (kind == Kind::Shutdown) {
        json["clean"] = clean;
    } else {
        json["safe_mode"] = safe_mode;
        if (!config_error.empty()) json["config_error"] = config_error;
    }
    return json;
}

std::optional<GatewayBootLine> GatewayBootLine::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    GatewayBootLine line;
    if (!json.contains("type") || !json["type"].is_string()) return std::nullopt;
    const std::string type = json["type"].get<std::string>();
    if (type == "boot") {
        line.kind = Kind::Boot;
    } else if (type == "shutdown") {
        line.kind = Kind::Shutdown;
    } else {
        return std::nullopt;
    }
    const auto get_string = [&](const char* key) {
        return json.contains(key) && json[key].is_string() ? json[key].get<std::string>()
                                                           : std::string();
    };
    const auto get_int = [&](const char* key) -> std::int64_t {
        return json.contains(key) && json[key].is_number_integer()
                   ? json[key].get<std::int64_t>()
                   : 0;
    };
    line.boot_id = get_string("boot_id");
    line.start_token = get_string("start_token");
    line.reason = get_string("reason");
    line.config_error = get_string("config_error");
    line.pid = static_cast<unsigned long>(get_int("pid"));
    line.at_ms = get_int("at_ms");
    if (json.contains("clean") && json["clean"].is_boolean()) line.clean = json["clean"].get<bool>();
    if (json.contains("safe_mode") && json["safe_mode"].is_boolean()) {
        line.safe_mode = json["safe_mode"].get<bool>();
    }
    return line;
}

int CountUncleanBootStreak(const std::vector<GatewayBootLine>& lines) {
    int streak = 0;
    for (const GatewayBootLine& line : lines) {
        if (line.kind == GatewayBootLine::Kind::Boot) {
            streak += 1;
        } else if (line.kind == GatewayBootLine::Kind::Shutdown && line.clean) {
            streak = 0;  // 单实例串行:一场干净关机清连击
        }
    }
    return streak;
}

GatewayBootHistory::GatewayBootHistory(std::filesystem::path file) : file_(std::move(file)) {}

std::string GatewayBootHistory::Append(const GatewayBootLine& line) {
    std::error_code ec;
    const std::filesystem::path parent = file_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return "建 boot history 目录失败: " + ec.message();
    }
    std::ofstream stream(file_, std::ios::binary | std::ios::app);
    if (!stream) return "打不开 boot history " + platform::PathToUtf8(file_);
    const std::string text = line.ToJson().dump();
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.write("\n", 1);
    stream.flush();
    if (!stream) return "写 boot history 失败(磁盘满?)";
    return std::string();
}

std::vector<GatewayBootLine> GatewayBootHistory::ReadAll() const {
    std::vector<GatewayBootLine> lines;
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) return lines;
    std::ifstream stream(file_, std::ios::binary);
    if (!stream) return lines;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line_text =
            end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
        if (!line_text.empty()) {
            try {
                const nlohmann::json parsed = nlohmann::json::parse(line_text);
                if (auto line = GatewayBootLine::FromJson(parsed)) {
                    lines.push_back(std::move(*line));
                }
            } catch (const nlohmann::json::exception&) {
                // 半截尾行/坏行:跳过计数,不崩宿主(账还在,那一段查不到)。
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

bool GatewayBootHistory::EvaluateSafeMode(int threshold) const {
    return CountUncleanBootStreak(ReadAll()) >= threshold;
}

// ---------------------------------------------------------------------------
// GatewayProcess
// ---------------------------------------------------------------------------

std::string GatewayProcess::MakeDefaultBootId() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t seq = counter.fetch_add(1);
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "gw-%llx-%lx-%llx",
                  static_cast<unsigned long long>(platform::WallClockNowMs()),
                  platform::CurrentProcessId(), static_cast<unsigned long long>(seq));
    return buffer;
}

GatewayProcess::GatewayProcess(Options options) : options_(std::move(options)) {
    if (!options_.now_ms) options_.now_ms = [] { return platform::WallClockNowMs(); };
    if (!options_.make_boot_id) options_.make_boot_id = [] { return MakeDefaultBootId(); };
}

void GatewayProcess::AddShutdownHook(ShutdownHook hook) { hooks_.push_back(std::move(hook)); }

void GatewayProcess::RequestStop(const std::string& reason) {
    stop_requested_.store(true);
    std::lock_guard<std::mutex> guard(mutex_);
    if (stop_reason_.empty()) stop_reason_ = reason;
}

void GatewayProcess::WriteControl(const std::string& state, const std::string& health) {
    GatewayControlSnapshot snapshot;
    snapshot.profile = options_.paths.name.empty() ? std::string(kDefaultGatewayProfile)
                                                   : options_.paths.name;
    snapshot.boot_id = boot_id_;
    snapshot.pid = platform::CurrentProcessId();
    snapshot.start_token = trajectory::CurrentProcessStartToken();
    snapshot.started_at_ms = options_.now_ms();
    snapshot.state = state;
    snapshot.health = health;
    snapshot.safe_mode = safe_mode_;
    snapshot.version = options_.version;
    snapshot.updated_at_ms = options_.now_ms();
    control_ = snapshot;
    const std::string error = WriteControlSnapshot(options_.paths.control_file, snapshot);
    if (!error.empty()) {
        std::fprintf(stderr, "[gateway] 写控制快照失败: %s\n", error.c_str());
    }
}

void GatewayProcess::Log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(options_.paths.logs_dir, ec);
    std::ofstream stream(options_.paths.log_file, std::ios::binary | std::ios::app);
    if (stream) {
        stream << "[" << options_.now_ms() << "] [" << level << "] " << message << "\n";
        stream.flush();
    }
}

GatewayProcess::StartResult GatewayProcess::Start() {
    StartResult result;
    boot_id_ = options_.make_boot_id();

    std::error_code ec;
    std::filesystem::create_directories(options_.paths.profile_dir, ec);
    if (ec) {
        result.detail = "建 profile 目录失败 " + platform::PathToUtf8(options_.paths.profile_dir) +
                        ": " + ec.message();
        return result;
    }
    std::filesystem::create_directories(options_.paths.control_dir, ec);
    if (ec) {
        result.detail = "建控制目录失败: " + ec.message();
        return result;
    }
    std::filesystem::create_directories(options_.paths.logs_dir, ec);
    if (ec) {
        result.detail = "建日志目录失败: " + ec.message();
        return result;
    }

    // SafeMode 先评(锁还没取):控制面照起,业务面暂停。
    GatewayBootHistory history(options_.paths.boot_history);
    safe_mode_ = history.EvaluateSafeMode(options_.config.safe_mode_threshold);

    GatewayLockRecord self;
    self.pid = platform::CurrentProcessId();
    self.start_token = trajectory::CurrentProcessStartToken();
    self.boot_id = boot_id_;
    self.acquired_at_ms = options_.now_ms();
    const auto acquire = GatewayLock::TryAcquire(options_.paths.lock_file, self, &lock_);
    if (acquire.status == GatewayLock::AcquireResult::Status::RefusedAliveHolder) {
        result.status = StartResult::Status::AlreadyRunning;
        result.detail = acquire.detail;
        result.holder = acquire.holder;
        return result;
    }
    if (acquire.status == GatewayLock::AcquireResult::Status::RefusedBrokenLock) {
        result.status = StartResult::Status::BrokenLock;
        result.detail = acquire.detail;
        return result;
    }
    if (acquire.status != GatewayLock::AcquireResult::Status::Acquired) {
        result.detail = acquire.detail.empty() ? "取锁失败" : acquire.detail;
        return result;
    }

    GatewayBootLine boot;
    boot.kind = GatewayBootLine::Kind::Boot;
    boot.boot_id = boot_id_;
    boot.pid = self.pid;
    boot.start_token = self.start_token;
    boot.at_ms = options_.now_ms();
    boot.reason = "process_launch";
    boot.safe_mode = safe_mode_;
    const std::string boot_error = history.Append(boot);
    if (!boot_error.empty()) {
        lock_.Release();
        result.detail = boot_error;
        return result;
    }

    WriteControl("running", safe_mode_ ? "degraded" : "ok");
    Log("info", "boot " + boot_id_ + " pid " + std::to_string(self.pid) +
                    (safe_mode_ ? " SafeMode(连续非干净关机达阈值,业务面暂停;干净关机即退出)"
                                : std::string()));
    std::fprintf(stderr, "[gateway] profile=%s boot=%s pid=%lu%s\n",
                 (options_.paths.name.empty() ? std::string(kDefaultGatewayProfile)
                                              : options_.paths.name)
                     .c_str(),
                 boot_id_.c_str(), self.pid, safe_mode_ ? " SafeMode" : "");

    if (options_.install_signal_handlers) {
        g_gateway_signal_stop = 0;
        std::signal(SIGINT, &GatewaySignalHandler);
        std::signal(SIGTERM, &GatewaySignalHandler);
    }

    result.status = StartResult::Status::Started;
    result.detail = boot_id_;
    return result;
}

int GatewayProcess::Run() {
    while (!stop_requested_.load()) {
        if (g_gateway_signal_stop != 0) {
            RequestStop("signal:" + std::to_string(g_gateway_signal_stop));
            break;
        }
        if (PollStopCommand(options_.paths.control_dir, boot_id_)) {
            RequestStop("stop_command");
            break;
        }
        SleepMs(options_.poll_interval_ms);
    }
    std::string reason = "stop";
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!stop_reason_.empty()) reason = stop_reason_;
    }
    return Shutdown(reason);
}

int GatewayProcess::Shutdown(const std::string& reason) {
    // 单子 §5.2 关机次序:先停止接活(主循环已出),再摘 wake,再收 turn,
    // 再关 outbox/adapter,最后释放 lock。G1 无业务面,业务收口由钩子承载。
    WriteControl("draining", safe_mode_ ? "degraded" : "ok");
    Log("info", "shutdown begin(" + reason + ")");

    bool clean = true;
    std::string failed_hook;
    for (const ShutdownHook& hook : hooks_) {
        bool done = false;
        try {
            done = hook.close ? hook.close() : true;
        } catch (const std::exception& e) {
            Log("error", "关机钩子 " + hook.name + " 抛异常: " + e.what());
            done = false;
        }
        if (!done) {
            clean = false;
            if (failed_hook.empty()) failed_hook = hook.name;
            Log("error", "关机钩子 " + hook.name + " 未在宽限内收完");
        }
    }

    GatewayBootLine shutdown;
    shutdown.kind = GatewayBootLine::Kind::Shutdown;
    shutdown.boot_id = boot_id_;
    shutdown.pid = platform::CurrentProcessId();
    shutdown.start_token = trajectory::CurrentProcessStartToken();
    shutdown.at_ms = options_.now_ms();
    shutdown.reason = reason;
    shutdown.clean = clean;
    const std::string history_error =
        GatewayBootHistory(options_.paths.boot_history).Append(shutdown);
    if (!history_error.empty()) {
        clean = false;  // 连关机账都落不下:不许自称 clean
        Log("error", history_error);
    }

    std::string last_shutdown = clean ? "clean" : "timeout";
    WriteControl("stopped", clean ? "ok" : "degraded");
    if (control_.has_value()) {
        control_->last_shutdown = last_shutdown;
        const std::string rewrite =
            WriteControlSnapshot(options_.paths.control_file, *control_);
        if (!rewrite.empty()) Log("error", rewrite);
    }
    Log("info", std::string("shutdown end(") + reason + ", " + (clean ? "clean" : "timeout") +
                    (failed_hook.empty() ? std::string() : ", hook " + failed_hook + " 未收净") +
                    ")");
    lock_.Release();
    return clean ? 0 : 4;
}

std::string GatewayProcess::RecordConfigInvalidBoot(const GatewayProfilePaths& paths,
                                                    const std::string& config_error) {
    GatewayBootLine boot;
    boot.kind = GatewayBootLine::Kind::Boot;
    boot.boot_id = GatewayProcess::MakeDefaultBootId();
    boot.pid = platform::CurrentProcessId();
    boot.start_token = trajectory::CurrentProcessStartToken();
    boot.at_ms = platform::WallClockNowMs();
    boot.reason = "config_invalid";
    boot.config_error = config_error;
    return GatewayBootHistory(paths.boot_history).Append(boot);
}

// ---------------------------------------------------------------------------
// StopGateway
// ---------------------------------------------------------------------------

GatewayStopOutcome StopGateway(const GatewayProfilePaths& paths, int timeout_ms,
                               const std::function<std::int64_t()>& now_ms) {
    GatewayStopOutcome outcome;
    const std::int64_t started = CallNowMs(now_ms);

    // 只读面:锁都不在,就没在跑(不建目录、不写文件)。
    std::error_code ec;
    if (!std::filesystem::exists(paths.lock_file, ec) || ec) {
        outcome.status = GatewayStopOutcome::Status::NotRunning;
        outcome.detail = "gateway.not_running: 没有运行中的 Gateway(无锁)";
        return outcome;
    }
    std::string read_error;
    const auto holder = ReadLockFile(paths.lock_file, &read_error);
    if (!holder.has_value()) {
        outcome.status = GatewayStopOutcome::Status::Refused;
        outcome.detail = "gateway.lock_stale: " + read_error + "——不敢投命令,人工核锁";
        return outcome;
    }
    const trajectory::SessionLockOwner owner{holder->pid, holder->start_token, 0};
    if (trajectory::ProbeLockHolder(owner) == trajectory::LockHolderState::Dead) {
        outcome.status = GatewayStopOutcome::Status::NotRunning;
        outcome.detail = "gateway.not_running: 锁是陈旧的(持有进程已死),下次启动自动清";
        return outcome;
    }

    GatewayStopCommand command;
    command.boot_id = holder->boot_id;
    command.requested_at_ms = CallNowMs(now_ms);
    const std::string write_error = WriteStopCommand(paths.control_dir, command);
    if (!write_error.empty()) {
        outcome.status = GatewayStopOutcome::Status::WriteFailed;
        outcome.detail = write_error;
        return outcome;
    }

    const int poll_ms = 100;
    while (true) {
        SleepMs(poll_ms);
        outcome.waited_ms = CallNowMs(now_ms) - started;
        std::error_code probe_ec;
        if (!std::filesystem::exists(paths.lock_file, probe_ec) || probe_ec) {
            // 锁已释放 = 关机流程走完(control 终态先写,锁最后放)。
            outcome.status = GatewayStopOutcome::Status::Stopped;
            outcome.detail = "已干净停下(boot " + holder->boot_id + ")";
            return outcome;
        }
        const trajectory::SessionLockOwner reprobe{holder->pid, holder->start_token, 0};
        if (trajectory::ProbeLockHolder(reprobe) == trajectory::LockHolderState::Dead) {
            outcome.status = GatewayStopOutcome::Status::StoppedUnclean;
            outcome.detail = "进程已退出但非干净关机(账上无 clean 记录;boot " +
                             holder->boot_id + ")";
            return outcome;
        }
        if (outcome.waited_ms >= timeout_ms) {
            outcome.status = GatewayStopOutcome::Status::Timeout;
            outcome.detail = "gateway.shutdown_timeout: 等了 " +
                             std::to_string(outcome.waited_ms) + "ms 仍在跑(boot " +
                             holder->boot_id + ");CLI 不越权代杀,留给 supervisor 或人工";
            return outcome;
        }
    }
}

}  // namespace lubancode::gateway
