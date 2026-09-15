// 常驻总装 V4:doctor 册。钉的合同(docs/architecture/gateway/
// contracts.md §14):
//   - 体检清单一项一码:未初始化场全 Info/Ok 退 0;坏配置退 2;坏账
//     (needs_review/在飞未跑)退 1;SafeMode 连击可 --ack-safe-mode 显式
//     清零(不伪造 shutdown 事实);
//   - 关机宽限未收净的 work 如实入账(boot-history shutdown 行
//     uncollected_work,不是 cancelled);doctor 把它报成 Warn;
//   - 健康探针:ready = 锁活 + control running + health ok + 非
//     SafeMode;--wait-ready 超时如实退 1 不假 ready;
//   - GatewayProcess 的关机记账:泵 Close 未收净 → 退出码 4 + shutdown
//     行带清单(假泵注入验证 engine 层合同,不依赖 runtime 装配)。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gateway/automation_store.hpp"
#include "gateway/control_server.hpp"
#include "gateway/doctor.hpp"
#include "gateway/process.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "gateway/status.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

using namespace lubancode;
using namespace lubancode::gateway;

namespace {

std::filesystem::path MakeTempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-gw-doc-" + std::string(name));
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

void WriteText(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << text;
}

GatewayProfilePaths PathsOf(const std::filesystem::path& root) {
    return ResolveGatewayProfilePaths(root, "default");
}

// doctor 的测试装配:跳过服务管理器/凭据面(各有独立注入案),配置读
// 真文件。
DoctorOptions MakeOptions() {
    DoctorOptions options;
    options.read_channels_config = [] { return std::optional<std::string>(std::string("{}")); };
    return options;
}

bool HasCheck(const DoctorReport& report, const std::string& code) {
    for (const DoctorCheck& check : report.checks) {
        if (check.code == code) return true;
    }
    return false;
}

const DoctorCheck* FindCheck(const DoctorReport& report, const std::string& code) {
    for (const DoctorCheck& check : report.checks) {
        if (check.code == code) return &check;
    }
    return nullptr;
}

// boot history 写一行(测试铺账)。
void AppendBootLine(const GatewayProfilePaths& paths, const GatewayBootLine& line) {
    (void)GatewayBootHistory(paths.boot_history).Append(line);
}

// outbox 账铺一枚 flagged 行(生产路:hash 不符/原件缺失 → flagged;测试
// 直接按投影读得懂的账行形状铺)。
void AppendFlaggedLine(const std::filesystem::path& log_file, const std::string& delivery_id) {
    nlohmann::json flag = nlohmann::json::object();
    flag["type"] = "item.flagged";
    flag["schemaVersion"] = 1;
    flag["deliveryId"] = delivery_id;
    flag["reason"] = "published_file_hash_mismatch";
    flag["flaggedAtMs"] = platform::WallClockNowMs();
    std::ofstream stream(log_file, std::ios::binary | std::ios::app);
    stream << flag.dump() << "\n";
}

// 假泵:关机合同验证用(engine 层 UncollectedWorkIds 口)。
class FakePump final : public GatewayWorkPump {
public:
    bool TickOnce(std::int64_t) override { return true; }
    void StopAccepting() override {}
    bool Close(int) override { return close_clean_; }
    std::vector<std::string> UncollectedWorkIds() const override { return uncollected; }
    bool close_clean_ = true;
    std::vector<std::string> uncollected;
};

}  // namespace

// ---------------------------------------------------------------------------
// 体检清单
// ---------------------------------------------------------------------------

TEST_CASE("doctor:未初始化场(零目录零写盘)全 Info/Ok,退 0") {
    const auto root = MakeTempRoot("empty");
    const auto paths = PathsOf(root);
    const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
    CHECK(report.ExitCode() == 0);
    CHECK_FALSE(report.HasWarn());
    CHECK_FALSE(report.HasFail());
    CHECK(HasCheck(report, "process.not_running"));
    CHECK(HasCheck(report, "config.missing"));
    CHECK(HasCheck(report, "channels.none_configured"));
    CHECK(HasCheck(report, "disk.no_profile_dir"));
    CHECK(HasCheck(report, "ledger.absent"));
    CHECK(HasCheck(report, "safe_mode.off"));
    CHECK(HasCheck(report, "boot.no_history"));
    CHECK(HasCheck(report, "install.not_recorded"));
    CHECK(HasCheck(report, "service.skipped"));
    // 零副作用:体检不建目录不落盘。
    std::error_code ec;
    const bool root_exists = std::filesystem::exists(root, ec);
    CHECK_FALSE(root_exists);
    CHECK_FALSE(ec);
}

TEST_CASE("doctor:坏配置退 2,好配置退 0(disk 探针即写即删)") {
    const auto root = MakeTempRoot("cfg");
    const auto paths = PathsOf(root);
    WriteText(paths.config_file, "{ not json");
    {
        const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
        CHECK(report.ExitCode() == 2);
        const DoctorCheck* check = FindCheck(report, "config.invalid");
        REQUIRE(check != nullptr);
        CHECK(check->severity == DoctorSeverity::Fail);
        CHECK(check->detail.find("gateway.config_invalid") != std::string::npos);
    }
    WriteText(paths.config_file, R"({"shutdown_grace_secs": 10})");
    {
        const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
        CHECK(report.ExitCode() == 0);
        REQUIRE(FindCheck(report, "config.ok") != nullptr);
        // disk 探针写了又删:目录里只剩配置文件。
        std::error_code ec;
        int files = 0;
        for (auto it = std::filesystem::directory_iterator(paths.profile_dir, ec);
             it != std::filesystem::directory_iterator(); ++it) {
            ++files;
        }
        CHECK(files == 1);  // gateway.json
    }
}

TEST_CASE("doctor:坏账面——needs_review 计数 Warn,在飞未跑 Warn,退 1") {
    const auto root = MakeTempRoot("ledger");
    const auto paths = PathsOf(root);
    // 铺 automation 账:两枚 once,一枚结算 needs_review,一枚 claim 后不结算。
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, paths.automation_log).ok);
    REQUIRE(store.CreateOnceJob("job-a", "p", platform::WallClockNowMs() - 1000,
                                platform::WallClockNowMs(), "idem-a")
                .accepted);
    REQUIRE(store.CreateOnceJob("job-b", "p", platform::WallClockNowMs() - 1000,
                                platform::WallClockNowMs(), "idem-b")
                .accepted);
    const auto first = store.ClaimDue("epoch-1", platform::WallClockNowMs());
    REQUIRE(first.has_value());
    REQUIRE(store.SettleOccurrence(first->occurrence_id, "needs_review", "audit", 1));
    const auto second = store.ClaimDue("epoch-1", platform::WallClockNowMs());
    REQUIRE(second.has_value());  // 留 claimed 不结算 = 在飞

    const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
    CHECK(report.ExitCode() == 1);
    const DoctorCheck* review = FindCheck(report, "ledger.needs_review");
    REQUIRE(review != nullptr);
    CHECK(review->severity == DoctorSeverity::Warn);
    CHECK(review->detail.find("1 枚") != std::string::npos);
    const DoctorCheck* in_flight = FindCheck(report, "ledger.in_flight");
    REQUIRE(in_flight != nullptr);
    CHECK(in_flight->severity == DoctorSeverity::Warn);  // 进程没在跑,在飞=重启待 reconcile
    CHECK(in_flight->detail.find("reconcile") != std::string::npos);
}

TEST_CASE("doctor:死信面——outbox flagged 计数 Warn") {
    const auto root = MakeTempRoot("dead");
    const auto paths = PathsOf(root);
    // 手铺 outbox 账:一枚 flagged(dead letter)。
    DurableReplyOutbox::Paths outbox_paths;
    outbox_paths.log_file = paths.outbox_log;
    outbox_paths.replies_dir = paths.replies_dir;
    outbox_paths.published_dir = paths.published_dir;
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, outbox_paths).ok);
    const auto enqueued =
        outbox.Enqueue("sel-s1-t1", "hello", "s1", "t1", platform::WallClockNowMs());
    REQUIRE(enqueued.accepted);

    const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
    // 无 flagged:Ok 面(死信清零也报 Ok,不藏在沉默里)。
    const DoctorCheck* clear = FindCheck(report, "dead_letter.outbox_flagged");
    REQUIRE(clear != nullptr);
    CHECK(clear->severity == DoctorSeverity::Ok);

    // 手写 flagged 行(生产路:hash 不符 → flagged;测试直接铺投影读得懂
    // 的账行)。
    AppendFlaggedLine(paths.outbox_log, enqueued.delivery_id);
    const DoctorReport flagged_report = RunGatewayDoctor(paths, MakeOptions());
    const DoctorCheck* flagged = FindCheck(flagged_report, "dead_letter.outbox_flagged");
    REQUIRE(flagged != nullptr);
    CHECK(flagged->severity == DoctorSeverity::Warn);
    CHECK(flagged_report.ExitCode() == 1);
}

TEST_CASE("doctor:SafeMode 连击 Warn;--ack-safe-mode 显式清零") {
    const auto root = MakeTempRoot("safe");
    const auto paths = PathsOf(root);
    for (int i = 0; i < 3; ++i) {
        GatewayBootLine boot;
        boot.kind = GatewayBootLine::Kind::Boot;
        boot.boot_id = "boot-" + std::to_string(i);
        boot.pid = 100 + i;
        boot.reason = "process_launch";
        AppendBootLine(paths, boot);
    }
    {
        const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
        const DoctorCheck* check = FindCheck(report, "safe_mode.on");
        REQUIRE(check != nullptr);
        CHECK(check->severity == DoctorSeverity::Warn);
        CHECK(report.unclean_streak == 3);
        CHECK(report.ExitCode() == 1);
        // doctor 只读:没 ack 之前连击不消。
        const DoctorReport again = RunGatewayDoctor(paths, MakeOptions());
        CHECK(again.unclean_streak == 3);
    }
    // ack:落一行 type=ack,连击清零;账上多的是 ack 行,不是伪造的
    // shutdown 行。
    CHECK(AckSafeMode(paths).empty());
    {
        const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
        CHECK(report.unclean_streak == 0);
        REQUIRE(FindCheck(report, "safe_mode.off") != nullptr);
        const std::string history = ReadText(paths.boot_history);
        CHECK(history.find("\"type\":\"ack\"") != std::string::npos);
        CHECK(history.find("ack_safe_mode") != std::string::npos);
    }
}

TEST_CASE("doctor:关机未收净入账报 Warn;干净关机报 Ok") {
    const auto root = MakeTempRoot("boot");
    const auto paths = PathsOf(root);
    {
        GatewayBootLine shutdown;
        shutdown.kind = GatewayBootLine::Kind::Shutdown;
        shutdown.boot_id = "boot-1";
        shutdown.reason = "stop";
        shutdown.clean = false;
        shutdown.uncollected_work = {"occ-aaa", "occ-bbb"};
        AppendBootLine(paths, shutdown);
    }
    const DoctorReport report = RunGatewayDoctor(paths, MakeOptions());
    const DoctorCheck* check = FindCheck(report, "boot.last_shutdown_unclean");
    REQUIRE(check != nullptr);
    CHECK(check->severity == DoctorSeverity::Warn);
    CHECK(check->detail.find("2 枚 work 未收净") != std::string::npos);
    CHECK(check->detail.find("occ-aaa") != std::string::npos);
    // 账上如实:uncollected 不是 cancelled。
    const std::string history = ReadText(paths.boot_history);
    CHECK(history.find("occ-aaa") != std::string::npos);
    CHECK(history.find("cancelled") == std::string::npos);
    // 干净关机翻面。
    {
        GatewayBootLine clean;
        clean.kind = GatewayBootLine::Kind::Shutdown;
        clean.boot_id = "boot-1";
        clean.reason = "stop";
        clean.clean = true;
        AppendBootLine(paths, clean);
    }
    const DoctorReport clean_report = RunGatewayDoctor(paths, MakeOptions());
    REQUIRE(FindCheck(clean_report, "boot.last_shutdown_clean") != nullptr);
}

TEST_CASE("doctor:安装记录对账——exe 没了退 2,版本不符 Warn") {
    const auto root = MakeTempRoot("install");
    const auto paths = PathsOf(root);
    ServiceInstallRecord record;
    record.platform = ServicePlatformName(CurrentServicePlatform());
    record.profile = "default";
    record.exe_path = platform::PathToUtf8(root / "gone" / "lubancode");
    record.lubancode_version = "0.26.1";
    record.unit_file = platform::PathToUtf8(root / "u");
    record.gateway_root = platform::PathToUtf8(root);
    record.service_log = platform::PathToUtf8(root / "l");
    WriteText(paths.profile_dir / "service" / "install.json", record.ToJson().dump());
    {
        DoctorOptions options = MakeOptions();
        options.lubancode_version = "0.26.1";
        const DoctorReport report = RunGatewayDoctor(paths, options);
        const DoctorCheck* check = FindCheck(report, "install.exe_missing");
        REQUIRE(check != nullptr);
        CHECK(check->severity == DoctorSeverity::Fail);
        CHECK(report.ExitCode() == 2);
    }
    // exe 在、版本不符:Warn(升级后没重 install)。
    WriteText(root / "here" / "lubancode", "bin");
    record.exe_path = platform::PathToUtf8(root / "here" / "lubancode");
    WriteText(paths.profile_dir / "service" / "install.json", record.ToJson().dump());
    {
        DoctorOptions options = MakeOptions();
        options.lubancode_version = "0.26.99";
        const DoctorReport report = RunGatewayDoctor(paths, options);
        const DoctorCheck* check = FindCheck(report, "install.version_mismatch");
        REQUIRE(check != nullptr);
        CHECK(check->severity == DoctorSeverity::Warn);
    }
}

// ---------------------------------------------------------------------------
// 健康探针
// ---------------------------------------------------------------------------

TEST_CASE("IsGatewayReady:锁活 + control running + ok + 非 SafeMode 才算") {
    GatewayProbe probe;
    probe.state = GatewayProbe::State::Running;
    GatewayControlSnapshot snapshot;
    snapshot.state = "running";
    snapshot.health = "ok";
    snapshot.safe_mode = false;
    probe.control = snapshot;
    CHECK(IsGatewayReady(probe));
    probe.control->safe_mode = true;
    CHECK_FALSE(IsGatewayReady(probe));  // SafeMode 业务面暂停不算 ready
    probe.control->safe_mode = false;
    probe.control->state = "draining";
    CHECK_FALSE(IsGatewayReady(probe));
    probe.control->state = "running";
    probe.control->health = "degraded";
    CHECK_FALSE(IsGatewayReady(probe));
    probe.control.reset();
    CHECK_FALSE(IsGatewayReady(probe));  // 没有快照不算
    probe.state = GatewayProbe::State::NotRunning;
    CHECK_FALSE(IsGatewayReady(probe));
}

TEST_CASE("WaitForGatewayReady:没在跑超时如实退 1;活实例 ready") {
    const auto root = MakeTempRoot("ready");
    const auto paths = PathsOf(root);
    {
        // 注入时钟:每调一次 +600ms;sleep 不真睡。
        std::int64_t fake_now = 1000;
        const auto outcome = WaitForGatewayReady(
            paths, 1, [&fake_now] { return fake_now += 600; }, [](int) {});
        CHECK_FALSE(outcome.ready);
        CHECK(outcome.detail.find("gateway.not_ready") != std::string::npos);
        CHECK(outcome.waited_ms >= 1000);
    }
    {
        // 铺活实例:锁(自己 pid+token)+ control running/ok。
        GatewayLockRecord holder;
        holder.pid = platform::CurrentProcessId();
        holder.start_token = trajectory::CurrentProcessStartToken();
        holder.boot_id = "boot-doc";
        holder.owner_epoch = "boot-doc";
        holder.acquired_at_ms = 1;
        WriteText(paths.lock_file, holder.ToJson().dump());
        GatewayControlSnapshot snapshot;
        snapshot.profile = "default";
        snapshot.boot_id = "boot-doc";
        snapshot.pid = holder.pid;
        snapshot.state = "running";
        snapshot.health = "ok";
        snapshot.safe_mode = false;
        snapshot.version = "test";
        WriteText(paths.control_file, snapshot.ToJson().dump());
        std::int64_t fake_now = 1000;
        const auto outcome = WaitForGatewayReady(
            paths, 5, [&fake_now] { return fake_now += 100; }, [](int) {});
        REQUIRE(outcome.ready);
        CHECK(outcome.detail.find("boot-doc") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// GatewayProcess 关机记账:未收净如实入账(单子 V4 第四行)
// ---------------------------------------------------------------------------

TEST_CASE("关机宽限到期:泵没收净 → 退 4,shutdown 行带 uncollected_work") {
    const auto root = MakeTempRoot("uncollected");
    const auto paths = PathsOf(root);
    GatewayProcess::Options options;
    options.paths = paths;
    options.install_signal_handlers = false;
    options.poll_interval_ms = 10;
    FakePump pump;
    pump.close_clean_ = false;                      // 宽限内没收净
    pump.uncollected = {"occ-x1", "occ-x2"};        // 泵报上来的未收净清单
    options.pump = &pump;
    GatewayProcess process(std::move(options));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);
    process.RequestStop("stop");
    const int exit_code = process.Run();
    CHECK(exit_code == 4);  // 关机超时语义(没假写 clean)
    // shutdown 账行:clean=false + uncollected_work 清单如实。
    const std::vector<GatewayBootLine> lines = GatewayBootHistory(paths.boot_history).ReadAll();
    REQUIRE_FALSE(lines.empty());
    bool found = false;
    for (const GatewayBootLine& line : lines) {
        if (line.kind == GatewayBootLine::Kind::Shutdown) {
            found = true;
            CHECK_FALSE(line.clean);
            REQUIRE(line.uncollected_work.size() == 2);
            CHECK(line.uncollected_work[0] == "occ-x1");
            CHECK(line.uncollected_work[1] == "occ-x2");
        }
    }
    CHECK(found);
    // 账行往返:FromJson 读得回清单(重启后 doctor/reconcile 可查)。
    const std::string history = ReadText(paths.boot_history);
    CHECK(history.find("occ-x1") != std::string::npos);
    CHECK(history.find("\"uncollected_work\"") != std::string::npos);
}

TEST_CASE("干净关机:泵收净 → 退 0,shutdown 行不带 uncollected_work") {
    const auto root = MakeTempRoot("clean");
    const auto paths = PathsOf(root);
    GatewayProcess::Options options;
    options.paths = paths;
    options.install_signal_handlers = false;
    options.poll_interval_ms = 10;
    FakePump pump;  // close_clean_ = true
    options.pump = &pump;
    GatewayProcess process(std::move(options));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);
    process.RequestStop("stop");
    CHECK(process.Run() == 0);
    const std::string history = ReadText(paths.boot_history);
    CHECK(history.find("uncollected_work") == std::string::npos);
}

TEST_CASE("boot 行 JSON 往返:ack 行与 uncollected_work 字段") {
    // ack 行。
    GatewayBootLine ack;
    ack.kind = GatewayBootLine::Kind::Ack;
    ack.pid = 42;
    ack.reason = "ack_safe_mode";
    const auto ack_back = GatewayBootLine::FromJson(nlohmann::json::parse(ack.ToJson().dump()));
    REQUIRE(ack_back.has_value());
    CHECK(ack_back->kind == GatewayBootLine::Kind::Ack);
    CHECK(ack_back->reason == "ack_safe_mode");
    // shutdown 行带 uncollected_work。
    GatewayBootLine shutdown;
    shutdown.kind = GatewayBootLine::Kind::Shutdown;
    shutdown.clean = false;
    shutdown.uncollected_work = {"occ-1"};
    const auto back = GatewayBootLine::FromJson(nlohmann::json::parse(shutdown.ToJson().dump()));
    REQUIRE(back.has_value());
    REQUIRE(back->uncollected_work.size() == 1);
    CHECK(back->uncollected_work[0] == "occ-1");
    CHECK_FALSE(back->clean);
    // 未知 type 仍跳过(容错)。
    CHECK_FALSE(GatewayBootLine::FromJson(nlohmann::json{{"type", "mystery"}}).has_value());
}
