// Gateway 服务安装与常驻运维(总装单 V4 §十"服务安装与常驻运维"):
// 平台服务单元生成(纯函数,可测)+ 服务管理器操作(install/uninstall/
// start/query,经可注入 runner 起 schtasks/systemctl/launchctl)+ 安装
// 记录 install.json(钉 exe 路径与版本,升级回滚对账)。
//
// 唯一真源 docs/architecture/gateway/contracts.md §14(V4 裁决)与
// runbook.md(三平台安装/启停/升级回滚手册)。几条铁律:
//   - install 只注册服务,不 start(start 单独命令);install 前校验配置
//     可装载(坏配置拒绝安装,坏配置重启风暴防线的一半;另一半在退出码
//     3 的 supervisor 豁免,见单元生成物)。
//   - 服务管理器只管"拉起/摘除/查询"。停止语义统一走既有文件控制面
//     (gateway stop:投 stop 命令 → drain → 宽限,不越权代杀)——
//     uninstall 由 CLI 编排"先文件面 stop 再摘服务";本件不碰进程。
//   - 工作目录/exe 路径/参数落死在单元生成物里(含 --gateway-root 绝对
//     路径,服务起来不依赖用户环境变量)。
//   - CI 只验生成物文本与命令面(argv 形状,经注入 runner);真实服务
//     注册三平台真机未验,分平台如实列(runbook §未验边界)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::gateway {

// ---------------------------------------------------------------------------
// 平台与名字
// ---------------------------------------------------------------------------

enum class ServicePlatform { Windows, Linux, MacOS };

// 编译目标平台(schtasks / systemd / launchd)。
ServicePlatform CurrentServicePlatform();
const char* ServicePlatformName(ServicePlatform platform);
std::optional<ServicePlatform> ServicePlatformFromName(const std::string& name);

// schtasks 任务名(Windows):"LubanCode Gateway (<profile>)"。
std::string ScheduledTaskName(const std::string& profile);
// systemd user 单元名(Linux):"lubancode-gateway-<profile>.service"。
std::string SystemdUnitName(const std::string& profile);
// launchd label(macOS):"ai.lubancode.gateway.<profile>"。
std::string LaunchdLabel(const std::string& profile);

// systemd user 单元落位:~/.config/systemd/user/<unit>(home 拿不到给空)。
std::filesystem::path SystemdUnitPath(const std::string& profile);
// launchd plist 落位:~/Library/LaunchAgents/<label>.plist(home 拿不到给空)。
std::filesystem::path LaunchdPlistPath(const std::string& profile);

// ---------------------------------------------------------------------------
// 单元生成(纯函数,零 IO)
// ---------------------------------------------------------------------------

// 生成一份服务定义的全部输入。路径全部绝对、由调用方(CLI 装配)定死。
struct GatewayServiceSpec {
    std::string profile;                    // profile 名(合法单段名)
    std::filesystem::path exe_path;         // lubancode 可执行绝对路径
    std::filesystem::path gateway_root;     // gateway 状态根(钉进 --gateway-root)
    std::filesystem::path working_dir;      // 服务工作目录(缺省 = gateway_root)
    std::filesystem::path service_log;      // Windows/macOS 服务 stdout 落位
    std::filesystem::path service_err;      // macOS stderr 落位(Windows 与 log 合流)
    std::string lubancode_version;          // 进 install.json 对账(升级回滚)
    int shutdown_grace_secs = 30;           // systemd TimeoutStopSec = grace + 15
    int restart_count = 3;                  // Windows RestartOnFailure 次数
    int restart_interval_secs = 60;         // Windows RestartOnFailure 间隔
    std::string user_name;                  // Windows LogonTrigger UserId(可空)
    // 系统侧单元落位目录(空 = 平台默认:~/.config/systemd/user 或
    // ~/Library/LaunchAgents;Windows 用 profile 树内 XML 无系统落位)。
    // 测试注入临时目录,不写真用户 home。
    std::filesystem::path system_unit_dir;
};

// Windows 计划任务 XML(Task Schema 1.3;文本 UTF-8,落盘时转 UTF-16)。
// ONLOGON 触发 + 30s 延迟;失败重启限次(退出码 3 无法按码豁免,靠限次
// 防风暴,如实写 runbook);ExecutionTimeLimit=PT0S 不限时;stdout/stderr
// 经 cmd 重定向到 service_log。
std::string BuildScheduledTaskXml(const GatewayServiceSpec& spec);

// systemd user 单元文本。Restart=on-failure + RestartPreventExitStatus=3
// (坏配置稳定退出码,防重启风暴);TimeoutStopSec = grace + 15(留观察
// 余量,同 CLI stop 的 +5s 传统放宽到 journal 收尾)。
std::string BuildSystemdUnit(const GatewayServiceSpec& spec);

// macOS LaunchAgent plist。RunAtLoad=true(登录自启);KeepAlive 只在崩溃
// 时拉起(干净退出/稳定退出码 3 不拉);stdout/stderr 落 StandardOutPath/
// StandardErrorPath。
std::string BuildLaunchdPlist(const GatewayServiceSpec& spec);

// ---------------------------------------------------------------------------
// runner seam(服务管理器命令)
// ---------------------------------------------------------------------------

struct ServiceCommandResult {
    bool spawned = false;  // 管理器本身起没起来(CI 无 schtasks 时 false)
    int exit_code = -1;
    std::string output;
    std::string error;     // spawn 失败的人话
};
using ServiceRunner =
    std::function<ServiceCommandResult(const std::vector<std::string>& argv, int timeout_ms)>;

// 生产 runner:platform::RunProcess 包一层(argv 数组,不经 shell)。
ServiceRunner MakeDefaultServiceRunner();

// ---------------------------------------------------------------------------
// 安装记录(install.json)
// ---------------------------------------------------------------------------

// profile 树内的安装记录:<profile_dir>/service/install.json。升级回滚的
// 对账凭据:exe 路径与版本钉死在此,doctor 拿它对"服务单元还指着谁、
// 当前 exe 是哪个版本"。
struct ServiceInstallRecord {
    int schema_version = 1;
    std::string platform;         // windows | linux | macos
    std::string profile;
    std::string exe_path;         // UTF-8
    std::string lubancode_version;
    std::string task_name;        // windows
    std::string unit_name;        // linux
    std::string launchd_label;    // macos
    std::string unit_file;        // 系统侧单元文件绝对路径(UTF-8)
    std::string gateway_root;     // UTF-8
    std::string service_log;      // UTF-8
    int shutdown_grace_secs = 30;
    std::int64_t installed_at_ms = 0;

    nlohmann::json ToJson() const;
    static std::optional<ServiceInstallRecord> FromJsonStrict(const nlohmann::json& json,
                                                              std::string* error);
    // 读 profile 树内 install.json;文件不在给 nullopt(error 空)。
    static std::optional<ServiceInstallRecord> Load(const std::filesystem::path& record_file,
                                                    std::string* error);
};

// ---------------------------------------------------------------------------
// 服务操作(install/uninstall/start/query)
// ---------------------------------------------------------------------------

struct ServiceOpOutcome {
    bool ok = false;
    std::string error_code;  // service.manager_unavailable | service.op_failed
                             // | service.unit_write_failed | service.record_write_failed
    std::string detail;
};

struct ServiceInstallOutcome {
    ServiceOpOutcome op;
    std::filesystem::path unit_file;    // 系统侧单元(windows 用树内 XML)
    std::filesystem::path record_file;  // install.json 落位
    ServiceInstallRecord record;        // ok 时 = 落盘的记录
};

// install:生成单元 → profile 树内存档(service/<文件名>)→ 写系统位置 →
// 注册(windows/linux;macOS 写 plist 即注册,load 归 start)→ 落
// install.json。不 start。
ServiceInstallOutcome InstallGatewayService(const GatewayServiceSpec& spec,
                                            ServicePlatform platform,
                                            const std::filesystem::path& profile_dir,
                                            const ServiceRunner& runner);

// uninstall:摘服务(windows /Delete;linux disable --now + daemon-reload +
// 删 unit;macos unload + 删 plist)→ 删 profile 树内 service/ 目录。
// 文件面 stop 归 CLI 编排(先 drain 再摘,语义与 gateway stop 统一)。
ServiceOpOutcome UninstallGatewayService(const GatewayServiceSpec& spec,
                                         ServicePlatform platform,
                                         const std::filesystem::path& profile_dir,
                                         const ServiceRunner& runner);

// start:经服务管理器拉起(不裸 spawn)。schtasks /Run | systemctl --user
// start | launchctl load(未载)/kickstart(已载)。
ServiceOpOutcome StartGatewayService(const GatewayServiceSpec& spec, ServicePlatform platform,
                                     const ServiceRunner& runner);

// query:服务注册在吗。ok = 管理器报"在"。
ServiceOpOutcome QueryGatewayService(const GatewayServiceSpec& spec, ServicePlatform platform,
                                     const ServiceRunner& runner);

}  // namespace lubancode::gateway
