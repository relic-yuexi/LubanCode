// GatewayProcess(总装单 G1):前台 Gateway 骨架——唯一实例锁、boot history、
// SafeMode 骨架、文件控制面主循环与 graceful shutdown。
//
// 唯一真源 docs/architecture/gateway/contracts.md(G0 冻结)与本页注释。
// G1 边界(单子 G1 批):不接模型、不接渠道、不起 automation——只守唯一
// 实例、可查可停、陈旧锁安全裁决、坏配置稳定退出。业务面(scheduler/
// channel/outbox)从 G2 起挂进 ShutdownHook 与主循环。
//
// 锁裁决(contracts.md §10):gateway.lock 记 pid + 进程 start token(与
// trajectory session lock 同源的身份核法)+ boot_id(实例令牌)。陈旧锁 =
// 持有者死透或 PID 复用(token 对不上);读不懂的锁保守拒绝不删。
//
// SafeMode(contracts.md §10):boot history 里连续非干净关机次数达到阈值
// (默认 3)即进 SafeMode——控制面照起、锁照取、业务面暂停、状态明示。
// 干净关机即破连击;显式 ack 口留给 G2 的 doctor。
#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/control_server.hpp"
#include "gateway/profile.hpp"

namespace lubancode::gateway {

// 进程形态(G0 冻结,contracts.md §1)。只有 Gateway 能起 scheduler/
// channel manager/control endpoint/持锁/接 webhook/拉 headless/续 outbox。
enum class ProcessMode {
    Interactive,
    Pipe,
    AppServer,
    Gateway,
};

// ---------------------------------------------------------------------------
// 唯一实例锁
// ---------------------------------------------------------------------------

// 锁文件里的账(不含任何密钥)。
struct GatewayLockRecord {
    unsigned long pid = 0;
    std::string start_token;  // 进程起始 token(防 PID 复用)
    std::string boot_id;      // 实例令牌(同进程双 Gateway 互不相认)
    std::int64_t acquired_at_ms = 0;

    nlohmann::json ToJson() const;
    static std::optional<GatewayLockRecord> FromJsonStrict(const nlohmann::json& json,
                                                           std::string* error);
};

// RAII 独占锁:析构即删锁文件(幂等)。move-only。
class GatewayLock {
public:
    struct AcquireResult {
        enum class Status {
            Acquired,            // 本实例拿到锁(含清掉陈旧锁后重拿)
            RefusedAliveHolder,  // 活进程持着:gateway.already_running
            RefusedBrokenLock,   // 锁文件在但读不懂:保守不动
            IoError,
        };
        Status status = Status::IoError;
        GatewayLockRecord holder;  // RefusedAliveHolder 时 = 活着的持有者
        std::string detail;
    };

    // 身份核:持有者活着且 token 对得上才算活;死透或 PID 复用 = 陈旧可清;
    // 探不到按活保守(宁拒不抢)。直接复用 trajectory 的 ProbeLockHolder
    //(同一把尺,两处锁不各养一套身份判定)。
    static AcquireResult TryAcquire(const std::filesystem::path& lock_file,
                                    const GatewayLockRecord& self, GatewayLock* out);

    GatewayLock() = default;
    ~GatewayLock();
    GatewayLock(const GatewayLock&) = delete;
    GatewayLock& operator=(const GatewayLock&) = delete;
    GatewayLock(GatewayLock&& other) noexcept;
    GatewayLock& operator=(GatewayLock&& other) noexcept;

    bool holds() const { return !lock_file_.empty(); }
    void Release();

private:
    std::filesystem::path lock_file_;  // 空 = 未持锁
};

// ---------------------------------------------------------------------------
// boot history 与 SafeMode
// ---------------------------------------------------------------------------

// boot-history.jsonl 的一行。type=boot:一次启动(带 safe_mode/config_error);
// type=shutdown:一次关机(clean=false 即非干净,含超时)。
struct GatewayBootLine {
    enum class Kind { Boot, Shutdown };
    Kind kind = Kind::Boot;
    std::string boot_id;
    unsigned long pid = 0;
    std::string start_token;
    std::int64_t at_ms = 0;
    std::string reason;         // boot: process_launch|config_invalid;shutdown: stop|signal
    bool clean = true;          // shutdown 行:true=干净关机
    bool safe_mode = false;     // boot 行
    std::string config_error;   // boot 行可带(gateway.config_invalid 的人话)

    nlohmann::json ToJson() const;
    // 读侧容错:坏行/半截行给 nullopt 由调用方跳过计数,不崩宿主。
    static std::optional<GatewayBootLine> FromJson(const nlohmann::json& json);
};

// 连续非干净关机连击:逐行走,boot 计一,干净 shutdown 清零(单实例串行,
// 一次只有一场在跑)。>= threshold 即进 SafeMode(contracts.md §10)。
int CountUncleanBootStreak(const std::vector<GatewayBootLine>& lines);

class GatewayBootHistory {
public:
    explicit GatewayBootHistory(std::filesystem::path file);

    // append + flush。建父目录(写侧专用;只读面走 ReadAll/Evaluate)。
    // 返回空 = 成功,否则人话错误。
    std::string Append(const GatewayBootLine& line);

    std::vector<GatewayBootLine> ReadAll() const;  // 坏行跳过
    bool EvaluateSafeMode(int threshold) const;

private:
    std::filesystem::path file_;
};

// ---------------------------------------------------------------------------
// GatewayProcess
// ---------------------------------------------------------------------------

class GatewayProcess {
public:
    struct Options {
        GatewayProfilePaths paths;
        GatewayProfileConfig config;
        std::string version;                   // lubancode 版本(进 control 快照)
        std::function<std::int64_t()> now_ms;  // 时钟 seam(测试注固定钟)
        std::function<std::string()> make_boot_id;  // 实例 id seam(测试钉死)
        bool install_signal_handlers = true;   // 测试/嵌入装配关掉,不动全局
        int poll_interval_ms = 100;            // 控制命令轮询粒度(不 busy)
    };

    // 关机钩子:G1 无业务面,机制先立。close 返回 true = 收干净;false =
    // 没在宽限内收完,记 gateway.shutdown_timeout(不假写 clean)。
    // G2+ 的 scheduler/channel/outbox 从这里挂。
    struct ShutdownHook {
        std::string name;
        std::function<bool()> close;
    };

    explicit GatewayProcess(Options options);

    GatewayProcess(const GatewayProcess&) = delete;
    GatewayProcess& operator=(const GatewayProcess&) = delete;

    // 启动结果。
    struct StartResult {
        enum class Status { Started, AlreadyRunning, BrokenLock, IoError };
        Status status = Status::IoError;
        std::string detail;
        GatewayLockRecord holder;  // AlreadyRunning 时 = 活着的持有者
    };

    // 启动:建目录、评 SafeMode、取锁、记 boot、写 control(running)、开日志。
    // 配置装载归调用方(坏配置在取锁前就该退稳定码 3,不占锁)。
    StartResult Start();

    // 主循环:阻塞等 stop(控制命令 / 进程内 RequestStop / SIGINT·SIGTERM),
    // 然后 graceful shutdown。返回进程退出码(contracts.md:0 干净/4 超时/1 错)。
    int Run();

    // 进程内请求关机(控制面/测试用)。reason 进 shutdown 账。
    void RequestStop(const std::string& reason);

    void AddShutdownHook(ShutdownHook hook);

    // ---- 观测 ----
    const std::string& boot_id() const { return boot_id_; }
    bool safe_mode() const { return safe_mode_; }
    bool stop_requested() const { return stop_requested_.load(); }
    const GatewayProfilePaths& paths() const { return options_.paths; }

    // 坏配置的启动尝试也记 boot(reason=config_invalid):supervisor 反复拉起
    // 坏配置时,boot history 有账可查,doctor 有处指认。返回空 = 记上了。
    static std::string RecordConfigInvalidBoot(const GatewayProfilePaths& paths,
                                               const std::string& config_error);

    // 默认 boot id:<now-ms-十六进制>-<pid>-<自增串>。
    static std::string MakeDefaultBootId();

private:
    void WriteControl(const std::string& state, const std::string& health);
    void Log(const std::string& level, const std::string& message);
    int Shutdown(const std::string& reason);

    Options options_;
    std::string boot_id_;
    bool safe_mode_ = false;
    std::atomic<bool> stop_requested_{false};
    std::string stop_reason_;
    std::mutex mutex_;  // 日志/控制快照写的串行闸
    GatewayLock lock_;
    std::vector<ShutdownHook> hooks_;
    std::optional<GatewayControlSnapshot> control_;
};

// ---------------------------------------------------------------------------
// stop 命令的等待收口(`lubancode gateway stop` 用)
// ---------------------------------------------------------------------------

struct GatewayStopOutcome {
    enum class Status {
        Stopped,      // 干净停下(锁已释放)
        StoppedUnclean,  // 进程已退(硬杀;账上无 clean 记录)
        NotRunning,   // 本来就没在跑
        Timeout,      // 到时限仍在跑(gateway.shutdown_timeout)
        Refused,      // 锁读不懂,不敢投命令
        WriteFailed,  // 命令文件写不进
    };
    Status status = Status::NotRunning;
    std::string detail;
    std::int64_t waited_ms = 0;
};

// 投 stop 命令并等进程退出:写 control/stop.json(带锁里的 boot_id),
// 轮询锁释放/持有者死透,至多 timeout_ms。纯本地文件操作,不杀进程——
// 超时由调用方(或 supervisor)裁决,CLI 不越权代杀。
GatewayStopOutcome StopGateway(const GatewayProfilePaths& paths, int timeout_ms,
                               const std::function<std::int64_t()>& now_ms = {});

}  // namespace lubancode::gateway
