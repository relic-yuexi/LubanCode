// Gateway 服务安装与常驻运维(总装单 V4)实现。合同见 service.hpp 与
// docs/architecture/gateway/contracts.md §14、runbook.md。
#include "gateway/service.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <share.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lubancode::gateway {

namespace {

// XML 文本节点/属性转义(路径与描述里出现 & < > " ' 都吃得下)。
std::string EscapeXml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string XmlDeclaration() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
}

// 单元文件落盘。utf16 = Windows 宿主上的注册文件(schtasks /XML 只认
// UTF-16/ANSI):文本声明换 UTF-16、字节转宽写、带 BOM。非 Windows 宿主
// (测试喂 platform=Windows)按 UTF-8 落——字节编码不是合同,文本才是。
bool WriteUnitFile(const std::filesystem::path& file, const std::string& text, bool utf16) {
    std::error_code ec;
    const std::filesystem::path parent = file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }
#ifdef _WIN32
    if (utf16) {
        std::string declared = text;
        const std::string from = "encoding=\"UTF-8\"";
        const std::string to = "encoding=\"UTF-16\"";
        const std::size_t at = declared.find(from);
        if (at != std::string::npos) {
            declared.replace(at, from.size(), to);
        }
        const std::wstring wide = platform::Utf8ToWide(declared);
        FILE* stream = _wfsopen(file.c_str(), L"wb", _SH_DENYNO);
        if (stream == nullptr) return false;
        const bool wrote =
            std::fwrite("\xff\xfe", 1, 2, stream) == 2 &&
            std::fwrite(wide.data(), sizeof(wchar_t), wide.size(), stream) == wide.size() &&
            std::fflush(stream) == 0;
        std::fclose(stream);
        return wrote;
    }
#else
    (void)utf16;
#endif
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << text;
    stream.flush();
    return static_cast<bool>(stream);
}

bool WriteUtf8File(const std::filesystem::path& file, const std::string& text) {
    return WriteUnitFile(file, text, false);
}

std::optional<std::string> ReadUtf8File(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return std::nullopt;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// systemd 的 ExecStart:路径带空格要引号(sytemd 单元引号规则)。
std::string QuoteIfNeeded(const std::string& text) {
    if (text.find(' ') == std::string::npos && text.find('\t') == std::string::npos) {
        return text;
    }
    return "\"" + text + "\"";
}

constexpr int kManagerTimeoutMs = 15000;

ServiceOpOutcome RunManagerCommand(const ServiceRunner& runner,
                                   const std::vector<std::string>& argv,
                                   const std::string& what) {
    ServiceOpOutcome outcome;
    if (!runner) {
        outcome.error_code = "service.manager_unavailable";
        outcome.detail = what + ": runner 未装配";
        return outcome;
    }
    const ServiceCommandResult result = runner(argv, kManagerTimeoutMs);
    if (!result.spawned) {
        outcome.error_code = "service.manager_unavailable";
        outcome.detail = what + ": 服务管理器起不来(" +
                         (result.error.empty() ? std::string("未知原因") : result.error) +
                         ");argv[0]=" + (argv.empty() ? std::string() : argv[0]);
        return outcome;
    }
    if (result.exit_code != 0) {
        outcome.error_code = "service.op_failed";
        outcome.detail = what + ": 管理器退出码 " + std::to_string(result.exit_code);
        if (!result.output.empty()) {
            outcome.detail += " —— " + result.output;
        }
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

std::filesystem::path ServiceDirOf(const std::filesystem::path& profile_dir) {
    return profile_dir / "service";
}

// 系统侧单元落位:spec 注入位优先(测试),否则平台默认。
std::filesystem::path ResolveSystemUnit(const GatewayServiceSpec& spec,
                                        ServicePlatform platform) {
    if (!spec.system_unit_dir.empty()) {
        switch (platform) {
            case ServicePlatform::Linux:
                return spec.system_unit_dir / SystemdUnitName(spec.profile);
            case ServicePlatform::MacOS:
                return spec.system_unit_dir / (LaunchdLabel(spec.profile) + ".plist");
            case ServicePlatform::Windows:
                break;  // Windows 注册文件即树内 XML,无系统落位
        }
    }
    switch (platform) {
        case ServicePlatform::Linux: return SystemdUnitPath(spec.profile);
        case ServicePlatform::MacOS: return LaunchdPlistPath(spec.profile);
        case ServicePlatform::Windows: break;
    }
    return {};
}

// launchd 的 gui domain 目标串 uid。macOS 宿主真取;别的宿主(测试喂
// platform=MacOS)给惯用形状值——那里只有 argv 形状可断言,没有真 launchd。
long CurrentLaunchdUid() {
#ifndef _WIN32
    return static_cast<long>(getuid());
#else
    return 501;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// 平台与名字
// ---------------------------------------------------------------------------

ServicePlatform CurrentServicePlatform() {
#if defined(_WIN32)
    return ServicePlatform::Windows;
#elif defined(__APPLE__)
    return ServicePlatform::MacOS;
#else
    return ServicePlatform::Linux;
#endif
}

const char* ServicePlatformName(ServicePlatform platform) {
    switch (platform) {
        case ServicePlatform::Windows: return "windows";
        case ServicePlatform::Linux: return "linux";
        case ServicePlatform::MacOS: return "macos";
    }
    return "unknown";
}

std::optional<ServicePlatform> ServicePlatformFromName(const std::string& name) {
    if (name == "windows") return ServicePlatform::Windows;
    if (name == "linux") return ServicePlatform::Linux;
    if (name == "macos") return ServicePlatform::MacOS;
    return std::nullopt;
}

std::string ScheduledTaskName(const std::string& profile) {
    return "LubanCode Gateway (" + profile + ")";
}

std::string SystemdUnitName(const std::string& profile) {
    return "lubancode-gateway-" + profile + ".service";
}

std::string LaunchdLabel(const std::string& profile) {
    return "ai.lubancode.gateway." + profile;
}

std::filesystem::path SystemdUnitPath(const std::string& profile) {
    const auto home = platform::HomeDir();
    if (!home.has_value()) return {};
    return platform::Utf8ToPath(*home) / ".config" / "systemd" / "user" /
           SystemdUnitName(profile);
}

std::filesystem::path LaunchdPlistPath(const std::string& profile) {
    const auto home = platform::HomeDir();
    if (!home.has_value()) return {};
    return platform::Utf8ToPath(*home) / "Library" / "LaunchAgents" /
           (LaunchdLabel(profile) + ".plist");
}

// ---------------------------------------------------------------------------
// 单元生成(纯函数)
// ---------------------------------------------------------------------------

std::string BuildScheduledTaskXml(const GatewayServiceSpec& spec) {
    const std::string exe = platform::PathToUtf8(spec.exe_path);
    const std::string root = platform::PathToUtf8(spec.gateway_root);
    const std::string log = platform::PathToUtf8(spec.service_log);
    // cmd /c 的引号规则:/s 下整条命令再包一层引号,内层路径各自引号。
    // 1>> 追加重定向 stdout,2>&1 让 stderr 跟上。
    const std::string arguments = "/d /s /c \"\"" + exe + "\" gateway run --profile " +
                                  spec.profile + " --gateway-root \"" + root + "\" 1>> \"" +
                                  log + "\" 2>&1\"";
    std::ostringstream xml;
    xml << XmlDeclaration() << "\n";
    xml << "<Task version=\"1.3\" "
           "xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
    xml << "  <RegistrationInfo>\n";
    xml << "    <Description>LubanCode Gateway profile '" << EscapeXml(spec.profile)
        << "'(持久 Agent 值房;由 lubancode gateway install 注册)</Description>\n";
    xml << "  </RegistrationInfo>\n";
    xml << "  <Triggers>\n";
    xml << "    <LogonTrigger>\n";
    xml << "      <Enabled>true</Enabled>\n";
    xml << "      <Delay>PT30S</Delay>\n";
    if (!spec.user_name.empty()) {
        xml << "      <UserId>" << EscapeXml(spec.user_name) << "</UserId>\n";
    }
    xml << "    </LogonTrigger>\n";
    xml << "  </Triggers>\n";
    xml << "  <Settings>\n";
    xml << "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n";
    xml << "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n";
    xml << "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n";
    xml << "    <AllowHardTerminate>true</AllowHardTerminate>\n";
    xml << "    <StartWhenAvailable>true</StartWhenAvailable>\n";
    xml << "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n";
    xml << "    <AllowStartOnDemand>true</AllowStartOnDemand>\n";
    xml << "    <Enabled>true</Enabled>\n";
    xml << "    <Hidden>false</Hidden>\n";
    xml << "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n";
    xml << "    <WakeToRun>false</WakeToRun>\n";
    // PT0S = 不限时(默认 72h 到点杀,常驻值房不能被它掐)。
    xml << "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n";
    xml << "    <Priority>7</Priority>\n";
    // 坏配置(退出码 3)无法按码豁免,靠限次防重启风暴;间隔分钟级。
    xml << "    <RestartOnFailure>\n";
    xml << "      <Interval>PT" << spec.restart_interval_secs << "S</Interval>\n";
    xml << "      <Count>" << spec.restart_count << "</Count>\n";
    xml << "    </RestartOnFailure>\n";
    xml << "  </Settings>\n";
    xml << "  <Actions Context=\"Author\">\n";
    xml << "    <Exec>\n";
    xml << "      <Command>cmd.exe</Command>\n";
    xml << "      <Arguments>" << EscapeXml(arguments) << "</Arguments>\n";
    xml << "    </Exec>\n";
    xml << "  </Actions>\n";
    xml << "</Task>\n";
    return xml.str();
}

std::string BuildSystemdUnit(const GatewayServiceSpec& spec) {
    const std::string exe = QuoteIfNeeded(platform::PathToUtf8(spec.exe_path));
    const std::string root = platform::PathToUtf8(spec.gateway_root);
    const std::string work = platform::PathToUtf8(spec.working_dir);
    const int stop_secs = spec.shutdown_grace_secs + 15;
    std::ostringstream unit;
    unit << "# " << SystemdUnitName(spec.profile)
         << " —— 由 `lubancode gateway install` 生成;手改会被下次 install 覆盖\n";
    unit << "# 日志:journalctl --user -u " << SystemdUnitName(spec.profile) << "\n";
    unit << "[Unit]\n";
    unit << "Description=LubanCode Gateway (profile '" << spec.profile << "')\n";
    unit << "After=network-online.target\n";
    unit << "Wants=network-online.target\n";
    unit << "\n[Service]\n";
    unit << "Type=simple\n";
    unit << "WorkingDirectory=" << work << "\n";
    unit << "ExecStart=" << exe << " gateway run --profile " << spec.profile
         << " --gateway-root " << root << "\n";
    unit << "Restart=on-failure\n";
    unit << "RestartPreventExitStatus=3\n";
    unit << "TimeoutStopSec=" << stop_secs << "\n";
    unit << "\n[Install]\n";
    unit << "WantedBy=default.target\n";
    return unit.str();
}

std::string BuildLaunchdPlist(const GatewayServiceSpec& spec) {
    const std::string exe = platform::PathToUtf8(spec.exe_path);
    const std::string root = platform::PathToUtf8(spec.gateway_root);
    const std::string work = platform::PathToUtf8(spec.working_dir);
    const std::string log = platform::PathToUtf8(spec.service_log);
    const std::string err = platform::PathToUtf8(spec.service_err);
    std::ostringstream plist;
    plist << XmlDeclaration() << "\n";
    plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist << "<plist version=\"1.0\">\n<dict>\n";
    plist << "    <key>Label</key>\n";
    plist << "    <string>" << EscapeXml(LaunchdLabel(spec.profile)) << "</string>\n";
    plist << "    <key>ProgramArguments</key>\n    <array>\n";
    plist << "        <string>" << EscapeXml(exe) << "</string>\n";
    plist << "        <string>gateway</string>\n";
    plist << "        <string>run</string>\n";
    plist << "        <string>--profile</string>\n";
    plist << "        <string>" << EscapeXml(spec.profile) << "</string>\n";
    plist << "        <string>--gateway-root</string>\n";
    plist << "        <string>" << EscapeXml(root) << "</string>\n";
    plist << "    </array>\n";
    plist << "    <key>WorkingDirectory</key>\n";
    plist << "    <string>" << EscapeXml(work) << "</string>\n";
    plist << "    <key>RunAtLoad</key>\n    <true/>\n";
    // 只在崩溃时拉起:干净退出(0/4)与坏配置(3)都不拉——launchd 侧的
    // 防重启风暴天然豁免按退出码,与 systemd 的 RestartPreventExitStatus
    // 同向;Windows 无按码豁免,靠 RestartOnFailure 限次(runbook 如实列)。
    plist << "    <key>KeepAlive</key>\n    <dict>\n";
    plist << "        <key>Crashed</key>\n        <true/>\n";
    plist << "    </dict>\n";
    plist << "    <key>StandardOutPath</key>\n";
    plist << "    <string>" << EscapeXml(log) << "</string>\n";
    plist << "    <key>StandardErrorPath</key>\n";
    plist << "    <string>" << EscapeXml(err) << "</string>\n";
    plist << "</dict>\n</plist>\n";
    return plist.str();
}

// ---------------------------------------------------------------------------
// runner
// ---------------------------------------------------------------------------

ServiceRunner MakeDefaultServiceRunner() {
    return [](const std::vector<std::string>& argv, int timeout_ms) {
        ServiceCommandResult result;
        const auto run = platform::RunProcess(argv, timeout_ms);
        result.spawned = !run.spawn_failed;
        result.exit_code = static_cast<int>(run.exit_code);
        result.output = run.output;
        if (run.spawn_failed) result.error = run.spawn_error;
        return result;
    };
}

// ---------------------------------------------------------------------------
// install.json
// ---------------------------------------------------------------------------

nlohmann::json ServiceInstallRecord::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["platform"] = platform;
    json["profile"] = profile;
    json["exe_path"] = exe_path;
    json["lubancode_version"] = lubancode_version;
    json["task_name"] = task_name;
    json["unit_name"] = unit_name;
    json["launchd_label"] = launchd_label;
    json["unit_file"] = unit_file;
    json["gateway_root"] = gateway_root;
    json["service_log"] = service_log;
    json["shutdown_grace_secs"] = shutdown_grace_secs;
    json["installed_at_ms"] = installed_at_ms;
    return json;
}

std::optional<ServiceInstallRecord> ServiceInstallRecord::FromJsonStrict(
    const nlohmann::json& json, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<ServiceInstallRecord>{};
    };
    if (!json.is_object()) return fail("install.json 必须是 JSON object");
    const std::array<const char*, 13> kRequired = {
        "schema_version", "platform",  "profile",       "exe_path",
        "lubancode_version", "task_name", "unit_name",   "launchd_label",
        "unit_file",       "gateway_root", "service_log", "shutdown_grace_secs",
        "installed_at_ms"};
    for (const char* key : kRequired) {
        if (!json.contains(key)) return fail(std::string("缺必填字段 ") + key);
    }
    if (!json["schema_version"].is_number_integer() ||
        !json["shutdown_grace_secs"].is_number_integer() ||
        !json["installed_at_ms"].is_number_integer()) {
        return fail("schema_version/shutdown_grace_secs/installed_at_ms 必须是整数");
    }
    for (const char* key : {"platform", "profile", "exe_path", "lubancode_version", "task_name",
                            "unit_name", "launchd_label", "unit_file", "gateway_root",
                            "service_log"}) {
        if (!json[key].is_string()) {
            return fail(std::string(key) + " 必须是字符串");
        }
    }
    ServiceInstallRecord record;
    record.schema_version = json["schema_version"].get<int>();
    record.platform = json["platform"].get<std::string>();
    record.profile = json["profile"].get<std::string>();
    record.exe_path = json["exe_path"].get<std::string>();
    record.lubancode_version = json["lubancode_version"].get<std::string>();
    record.task_name = json["task_name"].get<std::string>();
    record.unit_name = json["unit_name"].get<std::string>();
    record.launchd_label = json["launchd_label"].get<std::string>();
    record.unit_file = json["unit_file"].get<std::string>();
    record.gateway_root = json["gateway_root"].get<std::string>();
    record.service_log = json["service_log"].get<std::string>();
    record.shutdown_grace_secs = json["shutdown_grace_secs"].get<int>();
    record.installed_at_ms = json["installed_at_ms"].get<std::int64_t>();
    if (record.schema_version != 1) {
        return fail("认不得的 schema_version: " + std::to_string(record.schema_version));
    }
    if (!ServicePlatformFromName(record.platform).has_value()) {
        return fail("认不得的平台名: " + record.platform);
    }
    return record;
}

std::optional<ServiceInstallRecord> ServiceInstallRecord::Load(
    const std::filesystem::path& record_file, std::string* error) {
    const auto text = ReadUtf8File(record_file);
    if (!text.has_value()) {
        if (error != nullptr) error->clear();
        return std::nullopt;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(*text);
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) *error = std::string("install.json 解析失败: ") + e.what();
        return std::nullopt;
    }
    return FromJsonStrict(parsed, error);
}

// ---------------------------------------------------------------------------
// 服务操作
// ---------------------------------------------------------------------------

ServiceInstallOutcome InstallGatewayService(const GatewayServiceSpec& spec,
                                            ServicePlatform platform,
                                            const std::filesystem::path& profile_dir,
                                            const ServiceRunner& runner) {
    ServiceInstallOutcome outcome;
    std::string text;
    std::filesystem::path archive_name;  // profile 树内存档文件名
    switch (platform) {
        case ServicePlatform::Windows:
            text = BuildScheduledTaskXml(spec);
            archive_name = "gateway-task.xml";
            break;
        case ServicePlatform::Linux:
            text = BuildSystemdUnit(spec);
            archive_name = SystemdUnitName(spec.profile) + ".unit";
            break;
        case ServicePlatform::MacOS:
            text = BuildLaunchdPlist(spec);
            archive_name = LaunchdLabel(spec.profile) + ".plist";
            break;
    }
    // 系统侧落位(测试可注入;Windows = 树内 XML,/XML 吃这份)。
    std::filesystem::path system_unit =
        platform == ServicePlatform::Windows
            ? ServiceDirOf(profile_dir) / archive_name
            : ResolveSystemUnit(spec, platform);
    if (system_unit.empty()) {
        outcome.op.error_code = "service.unit_write_failed";
        outcome.op.detail = "系统单元落位算不出(主目录不可用)";
        return outcome;
    }

    // 1) profile 树内存档(审计/重装对账;跨平台恒 UTF-8)。
    const std::filesystem::path archive = ServiceDirOf(profile_dir) / archive_name;
    if (!WriteUtf8File(archive, text)) {
        outcome.op.error_code = "service.unit_write_failed";
        outcome.op.detail = "树内存档写不进 " + platform::PathToUtf8(archive);
        return outcome;
    }

    // 2) 写系统位置 + 注册。
    if (platform == ServicePlatform::Windows) {
        // 注册文件即树内 XML;Windows 宿主上按 UTF-16 落盘再注册。
        if (!WriteUnitFile(system_unit, text, /*utf16=*/true)) {
            outcome.op.error_code = "service.unit_write_failed";
            outcome.op.detail = "注册 XML 写不进 " + platform::PathToUtf8(system_unit);
            return outcome;
        }
        outcome.op = RunManagerCommand(
            runner, {"schtasks", "/Create", "/F", "/TN", ScheduledTaskName(spec.profile),
                     "/XML", platform::PathToUtf8(system_unit)},
            "schtasks /Create");
    } else if (platform == ServicePlatform::Linux) {
        if (!WriteUtf8File(system_unit, text)) {
            outcome.op.error_code = "service.unit_write_failed";
            outcome.op.detail = "unit 写不进 " + platform::PathToUtf8(system_unit);
            return outcome;
        }
        outcome.op = RunManagerCommand(runner, {"systemctl", "--user", "daemon-reload"},
                                       "systemctl --user daemon-reload");
        if (outcome.op.ok) {
            outcome.op = RunManagerCommand(
                runner,
                {"systemctl", "--user", "enable", SystemdUnitName(spec.profile)},
                "systemctl --user enable");
        }
    } else {  // MacOS:写 plist 即注册(load 归 start);登录自启由
              // LaunchAgents 目录扫描承担。
        if (!WriteUtf8File(system_unit, text)) {
            outcome.op.error_code = "service.unit_write_failed";
            outcome.op.detail = "plist 写不进 " + platform::PathToUtf8(system_unit);
            return outcome;
        }
        outcome.op.ok = true;
    }

    // 3) install.json(注册失败也落记录?不——注册没成不装样子,如实失败;
    //    已写出的单元文件留在原处供诊断)。
    if (!outcome.op.ok) {
        return outcome;
    }

    ServiceInstallRecord record;
    record.platform = ServicePlatformName(platform);
    record.profile = spec.profile;
    record.exe_path = platform::PathToUtf8(spec.exe_path);
    record.lubancode_version = spec.lubancode_version;
    record.task_name = ScheduledTaskName(spec.profile);
    record.unit_name = SystemdUnitName(spec.profile);
    record.launchd_label = LaunchdLabel(spec.profile);
    record.unit_file = platform::PathToUtf8(system_unit);
    record.gateway_root = platform::PathToUtf8(spec.gateway_root);
    record.service_log = platform::PathToUtf8(spec.service_log);
    record.shutdown_grace_secs = spec.shutdown_grace_secs;
    record.installed_at_ms = platform::WallClockNowMs();
    const std::filesystem::path record_file = ServiceDirOf(profile_dir) / "install.json";
    if (!WriteUtf8File(record_file, record.ToJson().dump())) {
        outcome.op.ok = false;
        outcome.op.error_code = "service.record_write_failed";
        outcome.op.detail = "install.json 写不进 " + platform::PathToUtf8(record_file);
        return outcome;
    }
    outcome.unit_file = system_unit;
    outcome.record_file = record_file;
    outcome.record = record;
    return outcome;
}

ServiceOpOutcome UninstallGatewayService(const GatewayServiceSpec& spec,
                                         ServicePlatform platform,
                                         const std::filesystem::path& profile_dir,
                                         const ServiceRunner& runner) {
    switch (platform) {
        case ServicePlatform::Windows: {
            auto outcome = RunManagerCommand(
                runner, {"schtasks", "/Delete", "/F", "/TN", ScheduledTaskName(spec.profile)},
                "schtasks /Delete");
            if (outcome.ok) {
                std::error_code ec;
                std::filesystem::remove_all(ServiceDirOf(profile_dir), ec);
            }
            return outcome;
        }
        case ServicePlatform::Linux: {
            auto outcome = RunManagerCommand(
                runner,
                {"systemctl", "--user", "disable", "--now", SystemdUnitName(spec.profile)},
                "systemctl --user disable --now");
            if (outcome.ok) {
                outcome = RunManagerCommand(runner, {"systemctl", "--user", "daemon-reload"},
                                            "systemctl --user daemon-reload");
            }
            if (outcome.ok) {
                std::error_code ec;
                std::filesystem::remove(ResolveSystemUnit(spec, platform), ec);
                std::filesystem::remove_all(ServiceDirOf(profile_dir), ec);
            }
            return outcome;
        }
        case ServicePlatform::MacOS: {
            const std::filesystem::path plist = ResolveSystemUnit(spec, platform);
            auto outcome =
                RunManagerCommand(runner, {"launchctl", "unload", platform::PathToUtf8(plist)},
                                  "launchctl unload");
            if (outcome.ok) {
                std::error_code ec;
                std::filesystem::remove(plist, ec);
                std::filesystem::remove_all(ServiceDirOf(profile_dir), ec);
            }
            return outcome;
        }
    }
    ServiceOpOutcome unreachable;
    unreachable.error_code = "service.op_failed";
    unreachable.detail = "未知平台";
    return unreachable;
}

ServiceOpOutcome StartGatewayService(const GatewayServiceSpec& spec, ServicePlatform platform,
                                     const ServiceRunner& runner) {
    switch (platform) {
        case ServicePlatform::Windows:
            return RunManagerCommand(
                runner, {"schtasks", "/Run", "/TN", ScheduledTaskName(spec.profile)},
                "schtasks /Run");
        case ServicePlatform::Linux:
            return RunManagerCommand(
                runner, {"systemctl", "--user", "start", SystemdUnitName(spec.profile)},
                "systemctl --user start");
        case ServicePlatform::MacOS: {
            const std::string label = LaunchdLabel(spec.profile);
            const ServiceCommandResult listed =
                    runner ? runner({"launchctl", "list", label}, kManagerTimeoutMs)
                           : ServiceCommandResult{};
            if (listed.spawned && listed.exit_code == 0) {
                // 已载:kickstart 重踢一脚(干净停走 gateway stop,kickstart
                // 只兜"没在跑也没重载"的场)。
                char target[128];
                std::snprintf(target, sizeof(target), "gui/%ld/%s", CurrentLaunchdUid(),
                              label.c_str());
                return RunManagerCommand(runner, {"launchctl", "kickstart", target},
                                         "launchctl kickstart");
            }
            return RunManagerCommand(runner,
                                     {"launchctl", "load",
                                      platform::PathToUtf8(ResolveSystemUnit(spec, platform))},
                                     "launchctl load");
        }
    }
    ServiceOpOutcome unreachable;
    unreachable.error_code = "service.op_failed";
    unreachable.detail = "未知平台";
    return unreachable;
}

ServiceOpOutcome QueryGatewayService(const GatewayServiceSpec& spec, ServicePlatform platform,
                                     const ServiceRunner& runner) {
    switch (platform) {
        case ServicePlatform::Windows:
            return RunManagerCommand(
                runner, {"schtasks", "/Query", "/TN", ScheduledTaskName(spec.profile)},
                "schtasks /Query");
        case ServicePlatform::Linux:
            return RunManagerCommand(
                runner, {"systemctl", "--user", "is-enabled", SystemdUnitName(spec.profile)},
                "systemctl --user is-enabled");
        case ServicePlatform::MacOS:
            return RunManagerCommand(runner, {"launchctl", "list", LaunchdLabel(spec.profile)},
                                     "launchctl list");
    }
    ServiceOpOutcome unreachable;
    unreachable.error_code = "service.op_failed";
    unreachable.detail = "未知平台";
    return unreachable;
}

}  // namespace lubancode::gateway
