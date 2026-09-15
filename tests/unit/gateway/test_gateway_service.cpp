// 常驻总装 V4:服务安装册。钉的合同(docs/architecture/gateway/
// contracts.md §14 与 runbook.md):
//   - 单元生成物三平台文本断言(Windows schtasks XML/systemd user unit/
//     macOS launchd plist):exe 路径/工作目录/参数落死、延迟启动、失败
//     重启策略、不限时、stdout/stderr 落位;
//   - install.json 安装记录:钉 exe 路径与版本(升级回滚对账),严格
//     解析;
//   - 服务管理器操作:经注入 runner 断言 argv 形状;管理器缺席/
//     命令失败走稳定码明错,零猜测;
//   - CI 边界如实:本册只验生成物与命令面,真实服务注册三平台真机
//     未验(runbook 分平台列)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gateway/service.hpp"
#include "platform/paths.hpp"

using namespace lubancode;
using namespace lubancode::gateway;

namespace {

std::filesystem::path MakeTempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-gw-svc-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

std::string ReadText(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return {};
    std::ifstream stream(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// 记录每次调用的 argv 并按剧本回结果的假 runner。
struct FakeRunner {
    struct Call {
        std::vector<std::string> argv;
    };
    std::vector<Call> calls;
    // 剧本:第 n 次调用的回包;不够长时用缺省。
    std::vector<ServiceCommandResult> script;
    ServiceRunner Bind() {
        return [this](const std::vector<std::string>& argv, int /*timeout_ms*/) {
            Call call;
            call.argv = argv;
            calls.push_back(std::move(call));
            if (calls.size() <= script.size()) {
                return script[calls.size() - 1];
            }
            ServiceCommandResult ok;
            ok.spawned = true;
            ok.exit_code = 0;
            return ok;
        };
    }
};

ServiceCommandResult OkResult() {
    ServiceCommandResult result;
    result.spawned = true;
    result.exit_code = 0;
    return result;
}

ServiceCommandResult MissingManager() {
    ServiceCommandResult result;  // spawned = false:管理器缺席
    result.error = "No such file or directory";
    return result;
}

GatewayServiceSpec MakeSpec(const std::filesystem::path& root) {
    GatewayServiceSpec spec;
    spec.profile = "default";
    spec.exe_path = root / "bin" / "lubancode";
    spec.gateway_root = root / "gateway";
    spec.working_dir = root / "gateway";
    spec.service_log = root / "gateway" / "profiles" / "default" / "logs" / "service.log";
    spec.service_err = root / "gateway" / "profiles" / "default" / "logs" / "service.err.log";
    spec.lubancode_version = "0.26.999";
    spec.shutdown_grace_secs = 30;
    spec.system_unit_dir = root / "system-units";  // 测试注入,不写真 home
    return spec;
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// 名字定式
// ---------------------------------------------------------------------------

TEST_CASE("服务名定式:task/unit/label 各带 profile 且可反查") {
    CHECK(ScheduledTaskName("default") == "LubanCode Gateway (default)");
    CHECK(ScheduledTaskName("work") == "LubanCode Gateway (work)");
    CHECK(SystemdUnitName("default") == "lubancode-gateway-default.service");
    CHECK(LaunchdLabel("default") == "ai.lubancode.gateway.default");
    CHECK(ServicePlatformFromName("windows") == ServicePlatform::Windows);
    CHECK(ServicePlatformFromName("linux") == ServicePlatform::Linux);
    CHECK(ServicePlatformFromName("macos") == ServicePlatform::MacOS);
    CHECK_FALSE(ServicePlatformFromName("plan9").has_value());
    // 编译目标的平台与名字自洽。
    const std::string name = ServicePlatformName(CurrentServicePlatform());
    CHECK(ServicePlatformFromName(name) == CurrentServicePlatform());
}

// ---------------------------------------------------------------------------
// 生成物:Windows schtasks XML
// ---------------------------------------------------------------------------

TEST_CASE("schtasks XML:参数/工作目录落死,延迟启动,失败重启限次,不限时") {
    const auto root = MakeTempRoot("xml");
    GatewayServiceSpec spec = MakeSpec(root);
    spec.user_name = "DESKTOP\\tester";
    const std::string xml = BuildScheduledTaskXml(spec);

    // 动作:cmd 包一层,exe 绝对路径 + gateway run --profile + --gateway-root
    // 钉死,stdout/stderr 重定向到 service.log。注意 Arguments 走了 XML
    // 转义(& -> &amp;),按转义后的形式断言。
    CHECK(Contains(xml, "<Command>cmd.exe</Command>"));
    CHECK(Contains(xml, platform::PathToUtf8(spec.exe_path)));
    CHECK(Contains(xml, "gateway run --profile default --gateway-root"));
    CHECK(Contains(xml, platform::PathToUtf8(spec.gateway_root)));
    CHECK(Contains(xml, platform::PathToUtf8(spec.service_log)));
    CHECK(Contains(xml, "1&gt;"));      // 1>> 重定向(XML 转义后)
    CHECK(Contains(xml, "2&gt;&amp;1"));  // stderr 跟上(XML 转义后)
    // 登录触发 + 30s 延迟 + 钉当前用户。
    CHECK(Contains(xml, "<LogonTrigger>"));
    CHECK(Contains(xml, "<Delay>PT30S</Delay>"));
    CHECK(Contains(xml, "<UserId>DESKTOP\\tester</UserId>"));
    // 失败重启:限次限间隔(坏配置退出码 3 无法按码豁免,靠限次防风暴)。
    CHECK(Contains(xml, "<RestartOnFailure>"));
    CHECK(Contains(xml, "<Count>3</Count>"));
    CHECK(Contains(xml, "<Interval>PT60S</Interval>"));
    // 不限时(默认 72h 到点杀,常驻值房不能被它掐) + 多实例忽略。
    CHECK(Contains(xml, "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"));
    CHECK(Contains(xml, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"));
    // user_name 为空时不钉 UserId(= 任意用户登录触发,runbook 写差异)。
    spec.user_name.clear();
    CHECK_FALSE(Contains(BuildScheduledTaskXml(spec), "<UserId>"));
}

// ---------------------------------------------------------------------------
// 生成物:systemd user unit
// ---------------------------------------------------------------------------

TEST_CASE("systemd unit:Restart=on-failure,退出码 3 豁免,停止宽限=grace+15") {
    const auto root = MakeTempRoot("unit");
    GatewayServiceSpec spec = MakeSpec(root);
    spec.shutdown_grace_secs = 45;
    const std::string unit = BuildSystemdUnit(spec);

    CHECK(Contains(unit, "ExecStart=" + platform::PathToUtf8(spec.exe_path) +
                             " gateway run --profile default --gateway-root " +
                             platform::PathToUtf8(spec.gateway_root)));
    CHECK(Contains(unit, "WorkingDirectory=" + platform::PathToUtf8(spec.working_dir)));
    CHECK(Contains(unit, "Restart=on-failure"));
    // 坏配置稳定退出码 3 不重拉:防重启风暴的 systemd 半场。
    CHECK(Contains(unit, "RestartPreventExitStatus=3"));
    CHECK(Contains(unit, "TimeoutStopSec=60"));  // 45 + 15
    CHECK(Contains(unit, "WantedBy=default.target"));
    CHECK(Contains(unit, "journalctl --user -u lubancode-gateway-default.service"));
    // exe 路径带空格时 ExecStart 引号包路径。
    spec.exe_path = root / "Program Files" / "lubancode.exe";
    CHECK(Contains(BuildSystemdUnit(spec), "\"" + platform::PathToUtf8(spec.exe_path) + "\""));
}

// ---------------------------------------------------------------------------
// 生成物:macOS launchd plist
// ---------------------------------------------------------------------------

TEST_CASE("launchd plist:RunAtLoad,KeepAlive 只在崩溃拉起,stdout/stderr 落位") {
    const auto root = MakeTempRoot("plist");
    const GatewayServiceSpec spec = MakeSpec(root);
    const std::string plist = BuildLaunchdPlist(spec);

    CHECK(Contains(plist, "<string>ai.lubancode.gateway.default</string>"));
    CHECK(Contains(plist, "<string>" + platform::PathToUtf8(spec.exe_path) + "</string>"));
    CHECK(Contains(plist, "<string>gateway</string>"));
    CHECK(Contains(plist, "<string>--gateway-root</string>"));
    CHECK(Contains(plist, "<string>" + platform::PathToUtf8(spec.gateway_root) + "</string>"));
    CHECK(Contains(plist, "<key>RunAtLoad</key>"));
    CHECK(Contains(plist, "<key>KeepAlive</key>"));
    CHECK(Contains(plist, "<key>Crashed</key>"));
    CHECK(Contains(plist, "<key>StandardOutPath</key>"));
    CHECK(Contains(plist, "<key>StandardErrorPath</key>"));
    CHECK(Contains(plist, platform::PathToUtf8(spec.service_err)));
}

TEST_CASE("XML 转义:路径里的 & < > 不破单元文本") {
    const auto root = MakeTempRoot("esc");
    GatewayServiceSpec spec = MakeSpec(root);
    spec.exe_path = root / "a&b<c>d" / "lubancode";
    const std::string xml = BuildScheduledTaskXml(spec);
    CHECK(Contains(xml, platform::PathToUtf8(root / "a&amp;b&lt;c&gt;d" / "lubancode")));
    CHECK_FALSE(Contains(xml, "a&b<c>d"));  // 原样未转义不得出现
    const std::string plist = BuildLaunchdPlist(spec);
    CHECK(Contains(plist, "a&amp;b&lt;c&gt;d"));
}

// ---------------------------------------------------------------------------
// install.json 记录
// ---------------------------------------------------------------------------

TEST_CASE("install.json:ToJson/FromJsonStrict 往返;缺字段/坏平台名拒") {
    ServiceInstallRecord record;
    record.platform = "windows";
    record.profile = "default";
    record.exe_path = "C:/opt/lubancode.exe";
    record.lubancode_version = "0.26.100";
    record.task_name = "LubanCode Gateway (default)";
    record.unit_file = "C:/Users/x/.lubancode/gateway/profiles/default/service/gateway-task.xml";
    record.gateway_root = "C:/Users/x/.lubancode/gateway";
    record.service_log = "C:/Users/x/.lubancode/gateway/profiles/default/logs/service.log";
    record.shutdown_grace_secs = 30;
    record.installed_at_ms = 1700000000000;

    const std::string text = record.ToJson().dump();
    std::string error;
    const auto loaded = ServiceInstallRecord::FromJsonStrict(nlohmann::json::parse(text), &error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->platform == record.platform);
    CHECK(loaded->exe_path == record.exe_path);
    CHECK(loaded->lubancode_version == record.lubancode_version);
    CHECK(loaded->installed_at_ms == record.installed_at_ms);

    // 缺必填字段:拒。
    auto json = nlohmann::json::parse(text);
    json.erase("exe_path");
    error.clear();
    CHECK_FALSE(ServiceInstallRecord::FromJsonStrict(json, &error).has_value());
    CHECK(Contains(error, "exe_path"));
    // 坏平台名:拒(零猜测)。
    json = nlohmann::json::parse(text);
    json["platform"] = "plan9";
    error.clear();
    CHECK_FALSE(ServiceInstallRecord::FromJsonStrict(json, &error).has_value());
    // 文件不在 = nullopt 且无错(未安装是合法态)。
    error = "stale";
    CHECK_FALSE(ServiceInstallRecord::Load(MakeTempRoot("nope") / "install.json", &error)
                    .has_value());
    CHECK(error.empty());
}

// ---------------------------------------------------------------------------
// install/uninstall/start:注入 runner 的命令面
// ---------------------------------------------------------------------------

TEST_CASE("install(windows):注册 schtasks /Create /XML,树内存档与 install.json 落盘") {
    const auto root = MakeTempRoot("instw");
    const GatewayServiceSpec spec = MakeSpec(root);
    FakeRunner runner;
    const auto outcome =
        InstallGatewayService(spec, ServicePlatform::Windows, root / "profiles" / "default",
                              runner.Bind());
    REQUIRE(outcome.op.ok);
    REQUIRE(runner.calls.size() == 1);
    const auto& argv = runner.calls[0].argv;
    REQUIRE(argv.size() == 7);
    CHECK(argv[0] == "schtasks");
    CHECK(argv[1] == "/Create");
    CHECK(argv[2] == "/F");
    CHECK(argv[3] == "/TN");
    CHECK(argv[4] == "LubanCode Gateway (default)");
    CHECK(argv[5] == "/XML");
    // 注册文件 = profile 树内 XML。
    CHECK(argv[6] == platform::PathToUtf8(root / "profiles" / "default" / "service" /
                                          "gateway-task.xml"));
    // 注册文件存在即证(Windows 宿主上字节是 UTF-16,文本形状由
    // BuildScheduledTaskXml 的纯函数案钉,这里不读字节)。
    CHECK(std::filesystem::exists(outcome.unit_file));
    // install.json 钉 exe 与版本(升级回滚对账)。
    std::string error;
    const auto record = ServiceInstallRecord::Load(outcome.record_file, &error);
    REQUIRE(record.has_value());
    CHECK(record->exe_path == platform::PathToUtf8(spec.exe_path));
    CHECK(record->lubancode_version == "0.26.999");
    CHECK(record->task_name == "LubanCode Gateway (default)");
}

TEST_CASE("install(linux):unit 写系统落位,daemon-reload + enable 两发") {
    const auto root = MakeTempRoot("instl");
    const GatewayServiceSpec spec = MakeSpec(root);
    FakeRunner runner;
    const auto outcome = InstallGatewayService(spec, ServicePlatform::Linux,
                                               root / "profiles" / "default", runner.Bind());
    REQUIRE(outcome.op.ok);
    REQUIRE(runner.calls.size() == 2);
    CHECK(runner.calls[0].argv ==
          std::vector<std::string>({"systemctl", "--user", "daemon-reload"}));
    CHECK(runner.calls[1].argv ==
          std::vector<std::string>({"systemctl", "--user", "enable",
                                    "lubancode-gateway-default.service"}));
    // unit 落系统侧(测试注入位,非真 home),文本是 ExecStart 钉死那份。
    CHECK(outcome.unit_file == spec.system_unit_dir / "lubancode-gateway-default.service");
    CHECK(Contains(ReadText(outcome.unit_file), "ExecStart="));
    // 树内存档副本同文本(.unit 后缀)。
    const std::string archive = ReadText(root / "profiles" / "default" / "service" /
                                         "lubancode-gateway-default.service.unit");
    CHECK(Contains(archive, "ExecStart="));
}

TEST_CASE("install(macos):写 plist 即注册(零命令),install.json 落盘") {
    const auto root = MakeTempRoot("instm");
    const GatewayServiceSpec spec = MakeSpec(root);
    FakeRunner runner;  // macOS install 不调管理器
    const auto outcome = InstallGatewayService(spec, ServicePlatform::MacOS,
                                               root / "profiles" / "default", runner.Bind());
    REQUIRE(outcome.op.ok);
    CHECK(runner.calls.empty());
    CHECK(outcome.unit_file == spec.system_unit_dir / "ai.lubancode.gateway.default.plist");
    CHECK(Contains(ReadText(outcome.unit_file), "<key>RunAtLoad</key>"));
    CHECK(std::filesystem::exists(outcome.record_file));
}

TEST_CASE("install:管理器缺席/命令失败走稳定码,零猜测") {
    const auto root = MakeTempRoot("instfail");
    const GatewayServiceSpec spec = MakeSpec(root);
    {
        FakeRunner runner;
        runner.script.push_back(MissingManager());
        const auto outcome = InstallGatewayService(spec, ServicePlatform::Windows,
                                                   root / "profiles" / "default",
                                                   runner.Bind());
        CHECK_FALSE(outcome.op.ok);
        CHECK(outcome.op.error_code == "service.manager_unavailable");
        CHECK(Contains(outcome.op.detail, "schtasks"));
    }
    {
        FakeRunner runner;
        ServiceCommandResult refused = OkResult();
        refused.exit_code = 1;
        refused.output = "ERROR: Access is denied.";
        runner.script.push_back(refused);
        const auto outcome = InstallGatewayService(spec, ServicePlatform::Windows,
                                                   root / "profiles" / "default",
                                                   runner.Bind());
        CHECK_FALSE(outcome.op.ok);
        CHECK(outcome.op.error_code == "service.op_failed");
        CHECK(Contains(outcome.op.detail, "Access is denied"));
        // 注册失败不落 install.json(不装样子)。
        CHECK_FALSE(std::filesystem::exists(root / "profiles" / "default" / "service" /
                                            "install.json"));
    }
    {
        // runner 未装配:明错,不悄悄当成功。
        const auto outcome = InstallGatewayService(spec, ServicePlatform::Windows,
                                                   root / "profiles" / "default", {});
        CHECK_FALSE(outcome.op.ok);
        CHECK(outcome.op.error_code == "service.manager_unavailable");
    }
}

TEST_CASE("uninstall(windows):schtasks /Delete,树内 service 目录摘除") {
    const auto root = MakeTempRoot("uninstw");
    const GatewayServiceSpec spec = MakeSpec(root);
    FakeRunner install_runner;
    REQUIRE(InstallGatewayService(spec, ServicePlatform::Windows, root / "profiles" / "default",
                                  install_runner.Bind())
                .op.ok);
    FakeRunner runner;
    const auto outcome =
        UninstallGatewayService(spec, ServicePlatform::Windows, root / "profiles" / "default",
                                runner.Bind());
    REQUIRE(outcome.ok);
    REQUIRE(runner.calls.size() == 1);
    CHECK(runner.calls[0].argv[0] == "schtasks");
    CHECK(runner.calls[0].argv[1] == "/Delete");
    CHECK(runner.calls[0].argv[2] == "/F");
    CHECK(runner.calls[0].argv[3] == "/TN");
    CHECK(runner.calls[0].argv[4] == "LubanCode Gateway (default)");
    CHECK_FALSE(std::filesystem::exists(root / "profiles" / "default" / "service"));
}

TEST_CASE("uninstall(linux):disable --now + daemon-reload,unit 摘除") {
    const auto root = MakeTempRoot("uninstl");
    const GatewayServiceSpec spec = MakeSpec(root);
    FakeRunner install_runner;
    REQUIRE(InstallGatewayService(spec, ServicePlatform::Linux, root / "profiles" / "default",
                                  install_runner.Bind())
                .op.ok);
    FakeRunner runner;
    const auto outcome =
        UninstallGatewayService(spec, ServicePlatform::Linux, root / "profiles" / "default",
                                runner.Bind());
    REQUIRE(outcome.ok);
    REQUIRE(runner.calls.size() == 2);
    CHECK(runner.calls[0].argv ==
          std::vector<std::string>({"systemctl", "--user", "disable", "--now",
                                    "lubancode-gateway-default.service"}));
    CHECK_FALSE(std::filesystem::exists(spec.system_unit_dir / "lubancode-gateway-default.service"));
}

TEST_CASE("start:schtasks /Run | systemctl start | launchctl load/kickstart") {
    const auto root = MakeTempRoot("start");
    const GatewayServiceSpec spec = MakeSpec(root);
    {
        FakeRunner runner;
        const auto outcome =
            StartGatewayService(spec, ServicePlatform::Windows, runner.Bind());
        REQUIRE(outcome.ok);
        REQUIRE(runner.calls.size() == 1);
        CHECK(runner.calls[0].argv ==
              std::vector<std::string>({"schtasks", "/Run", "/TN",
                                        "LubanCode Gateway (default)"}));
    }
    {
        FakeRunner runner;
        const auto outcome = StartGatewayService(spec, ServicePlatform::Linux, runner.Bind());
        REQUIRE(outcome.ok);
        CHECK(runner.calls[0].argv ==
              std::vector<std::string>({"systemctl", "--user", "start",
                                        "lubancode-gateway-default.service"}));
    }
    {
        // macOS 未载:list 失败 → load plist。
        FakeRunner runner;
        ServiceCommandResult not_loaded = OkResult();
        not_loaded.exit_code = 1;
        runner.script.push_back(not_loaded);
        const auto outcome = StartGatewayService(spec, ServicePlatform::MacOS, runner.Bind());
        REQUIRE(outcome.ok);
        REQUIRE(runner.calls.size() == 2);
        CHECK(runner.calls[0].argv ==
              std::vector<std::string>({"launchctl", "list", "ai.lubancode.gateway.default"}));
        CHECK(runner.calls[1].argv[0] == "launchctl");
        CHECK(runner.calls[1].argv[1] == "load");
        CHECK(runner.calls[1].argv[2] == platform::PathToUtf8(spec.system_unit_dir /
                                                              "ai.lubancode.gateway.default.plist"));
    }
    {
        // macOS 已载:list 成功 → kickstart gui/<uid>/<label>(不硬杀现跑,
        // 重踢一脚;干净停走 gateway stop)。
        FakeRunner runner;
        runner.script.push_back(OkResult());
        const auto outcome = StartGatewayService(spec, ServicePlatform::MacOS, runner.Bind());
        REQUIRE(outcome.ok);
        REQUIRE(runner.calls.size() == 2);
        CHECK(runner.calls[1].argv[0] == "launchctl");
        CHECK(runner.calls[1].argv[1] == "kickstart");
        CHECK(Contains(runner.calls[1].argv[2], "gui/"));
        CHECK(Contains(runner.calls[1].argv[2], "ai.lubancode.gateway.default"));
    }
}

TEST_CASE("query:exit 0 = 注册在;非 0 = 没注册;缺席 = 管理器不可用") {
    const auto root = MakeTempRoot("query");
    const GatewayServiceSpec spec = MakeSpec(root);
    {
        FakeRunner runner;
        runner.script.push_back(OkResult());
        CHECK(QueryGatewayService(spec, ServicePlatform::Windows, runner.Bind()).ok);
    }
    {
        FakeRunner runner;
        ServiceCommandResult absent = OkResult();
        absent.exit_code = 1;
        runner.script.push_back(absent);
        const auto outcome = QueryGatewayService(spec, ServicePlatform::Windows, runner.Bind());
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error_code == "service.op_failed");
    }
    {
        FakeRunner runner;
        runner.script.push_back(MissingManager());
        const auto outcome = QueryGatewayService(spec, ServicePlatform::Windows, runner.Bind());
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error_code == "service.manager_unavailable");
    }
}
