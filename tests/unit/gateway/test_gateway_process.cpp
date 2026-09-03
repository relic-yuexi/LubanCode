// 持久 Agent 总装单 G1:Gateway 骨架册。钉的合同(docs/architecture/gateway/
// contracts.md):
//   - 唯一实例锁:活持有者拒绝、陈旧锁(pid 对 token 错 = PID 复用)清后
//     重拿、坏锁保守不删;
//   - boot history 与 SafeMode:连续非干净关机达阈值进 SafeMode,干净关机
//     破连击;
//   - 文件控制面:control.json 快照、stop 命令(boot_id 定向,陈旧命令不
//     追杀新实例);
//   - graceful shutdown:干净 0 / 钩子没收净 4,关机账如实;
//   - disabled 零副作用:status/stop 在未运行时零建目录零写盘;
//   - CLI 解析:gateway run|status|stop --profile --json。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "app/cli_options.hpp"
#include "gateway/control_server.hpp"
#include "gateway/process.hpp"
#include "gateway/profile.hpp"
#include "gateway/status.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

using namespace lubancode::gateway;

namespace {

std::filesystem::path MakeTempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-gw-test" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;  // 刻意不建:零副作用断言要从"目录根本不存在"起步
}

std::string ReadText(const std::filesystem::path& file) {
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

GatewayLockRecord SelfRecord(const std::string& boot_id) {
    GatewayLockRecord record;
    record.pid = lubancode::platform::CurrentProcessId();
    record.start_token = lubancode::trajectory::CurrentProcessStartToken();
    record.boot_id = boot_id;
    record.acquired_at_ms = lubancode::platform::WallClockNowMs();
    return record;
}

GatewayProcess::Options MakeOptions(const GatewayProfilePaths& paths) {
    GatewayProcess::Options options;
    options.paths = paths;
    options.install_signal_handlers = false;  // 测试不动全局 signal 面
    options.poll_interval_ms = 20;
    options.make_boot_id = [] { return GatewayProcess::MakeDefaultBootId(); };
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// profile 与配置
// ---------------------------------------------------------------------------

TEST_CASE("profile 名裁决:单段名合法,路径形状拒绝") {
    CHECK(IsValidGatewayProfileName("default"));
    CHECK(IsValidGatewayProfileName("work-2.main"));
    CHECK_FALSE(IsValidGatewayProfileName(""));
    CHECK_FALSE(IsValidGatewayProfileName(".."));
    CHECK_FALSE(IsValidGatewayProfileName("a/b"));
    CHECK_FALSE(IsValidGatewayProfileName("a\\b"));
    CHECK_FALSE(IsValidGatewayProfileName(".hidden"));
    CHECK_FALSE(IsValidGatewayProfileName("名 字"));
    const auto paths = ResolveGatewayProfilePaths(MakeTempRoot("p"), "../evil");
    CHECK(paths.root.empty());  // 非法名回空,拼不出逃逸路径
}

TEST_CASE("gateway.json:缺文件默认,坏文件/未知字段/越界明报 config_invalid") {
    const auto root = MakeTempRoot("cfg");
    const auto paths = PathsOf(root);
    {
        const auto load = LoadGatewayConfig(paths.config_file);  // 连目录都没有
        CHECK(load.status == GatewayConfigLoad::Status::Missing);
        CHECK(load.config.shutdown_grace_secs == 30);
    }
    WriteText(paths.config_file, "{ not json");
    {
        const auto load = LoadGatewayConfig(paths.config_file);
        REQUIRE(load.status == GatewayConfigLoad::Status::Invalid);
        CHECK(load.error.find("gateway.config_invalid") != std::string::npos);
    }
    WriteText(paths.config_file, R"({"no_such_field": 1})");
    {
        const auto load = LoadGatewayConfig(paths.config_file);
        CHECK(load.status == GatewayConfigLoad::Status::Invalid);
    }
    WriteText(paths.config_file, R"({"shutdown_grace_secs": 0})");
    {
        const auto load = LoadGatewayConfig(paths.config_file);
        CHECK(load.status == GatewayConfigLoad::Status::Invalid);
    }
    WriteText(paths.config_file, R"({"shutdown_grace_secs": 5, "max_concurrent_sessions": 2})");
    {
        const auto load = LoadGatewayConfig(paths.config_file);
        REQUIRE(load.status == GatewayConfigLoad::Status::Ok);
        CHECK(load.config.shutdown_grace_secs == 5);
    }
}

// ---------------------------------------------------------------------------
// 唯一实例锁
// ---------------------------------------------------------------------------

TEST_CASE("锁:空位取锁、RAII 释放;第二实例(不同 boot_id)拒绝") {
    const auto root = MakeTempRoot("lock1");
    const auto paths = PathsOf(root);
    GatewayLock first;
    auto got = GatewayLock::TryAcquire(paths.lock_file, SelfRecord("boot-a"), &first);
    REQUIRE(got.status == GatewayLock::AcquireResult::Status::Acquired);
    CHECK(std::filesystem::exists(paths.lock_file));

    // 同进程、同 token、不同 boot_id = 另一只 Gateway 实例:身份核自己
    // pid+token 是活的,拒绝。
    GatewayLock second;
    auto refused = GatewayLock::TryAcquire(paths.lock_file, SelfRecord("boot-b"), &second);
    REQUIRE(refused.status == GatewayLock::AcquireResult::Status::RefusedAliveHolder);
    CHECK(refused.holder.boot_id == "boot-a");
    CHECK(refused.detail.find("gateway.already_running") != std::string::npos);
    CHECK_FALSE(second.holds());

    first.Release();
    CHECK_FALSE(std::filesystem::exists(paths.lock_file));
}

TEST_CASE("锁:陈旧锁(pid 复用 token 对不上)清后重拿;坏锁保守不删") {
    const auto root = MakeTempRoot("lock2");
    const auto paths = PathsOf(root);
    // PID 复用:持有者 pid 是本进程,但 start token 对不上 → ProbeLockHolder
    // 判 Dead(陈旧),清掉重拿。
    GatewayLockRecord reused = SelfRecord("boot-old");
    reused.start_token = "0000000000000000-not-this-process";
    WriteText(paths.lock_file, reused.ToJson().dump());
    GatewayLock fresh;
    auto got = GatewayLock::TryAcquire(paths.lock_file, SelfRecord("boot-new"), &fresh);
    REQUIRE(got.status == GatewayLock::AcquireResult::Status::Acquired);
    CHECK(fresh.holds());

    // 坏锁:文件在但读不懂,不敢删,保守拒绝。
    const auto other = ResolveGatewayProfilePaths(root, "broken");
    WriteText(other.lock_file, "{ half written");
    GatewayLock refused;
    auto broken = GatewayLock::TryAcquire(other.lock_file, SelfRecord("boot-x"), &refused);
    REQUIRE(broken.status == GatewayLock::AcquireResult::Status::RefusedBrokenLock);
    CHECK(broken.detail.find("gateway.lock_stale") != std::string::npos);
    CHECK(std::filesystem::exists(other.lock_file));  // 没删
}

// ---------------------------------------------------------------------------
// boot history 与 SafeMode
// ---------------------------------------------------------------------------

TEST_CASE("boot history:干净关机清连击,未收口累计;坏行跳过") {
    const auto root = MakeTempRoot("history");
    const auto paths = PathsOf(root);
    GatewayBootHistory history(paths.boot_history);
    CHECK(history.ReadAll().empty());
    CHECK_FALSE(history.EvaluateSafeMode(3));

    auto boot = [&](const std::string& id) {
        GatewayBootLine line;
        line.kind = GatewayBootLine::Kind::Boot;
        line.boot_id = id;
        line.pid = 42;
        line.at_ms = 1;
        REQUIRE(history.Append(line).empty());
    };
    auto shutdown = [&](const std::string& id, bool clean) {
        GatewayBootLine line;
        line.kind = GatewayBootLine::Kind::Shutdown;
        line.boot_id = id;
        line.pid = 42;
        line.at_ms = 2;
        line.clean = clean;
        REQUIRE(history.Append(line).empty());
    };

    boot("a");
    CHECK(CountUncleanBootStreak(history.ReadAll()) == 1);
    shutdown("a", true);
    CHECK(CountUncleanBootStreak(history.ReadAll()) == 0);

    boot("b");
    shutdown("b", false);  // 超时:连击不清
    boot("c");
    CHECK(CountUncleanBootStreak(history.ReadAll()) == 2);
    CHECK_FALSE(history.EvaluateSafeMode(3));
    boot("d");
    CHECK(history.EvaluateSafeMode(3));

    // 半截尾行(崩溃写了一半)跳过,不崩。前面已有 6 笔合法账
    // (a/b/c/d 四 boot + a/b 两 shutdown)。
    {
        std::ofstream stream(paths.boot_history, std::ios::binary | std::ios::app);
        stream << "{\"type\":\"boot\",\"boot_id\":\"half";
    }
    CHECK(history.ReadAll().size() == 6);
}

// ---------------------------------------------------------------------------
// GatewayProcess 生命周期
// ---------------------------------------------------------------------------

TEST_CASE("进程:Start 落锁与 running 快照,RequestStop 干净关机退 0") {
    const auto root = MakeTempRoot("life1");
    const auto paths = PathsOf(root);
    GatewayProcess process(MakeOptions(paths));
    const auto start = process.Start();
    REQUIRE(start.status == GatewayProcess::StartResult::Status::Started);
    CHECK_FALSE(process.safe_mode());

    {
        std::string error;
        const auto control = ReadControlSnapshot(paths.control_file, &error);
        REQUIRE(control.has_value());
        CHECK(control->state == "running");
        CHECK(control->health == "ok");
        CHECK(control->boot_id == process.boot_id());
        CHECK(control->pid == lubancode::platform::CurrentProcessId());
    }
    REQUIRE(std::filesystem::exists(paths.lock_file));
    REQUIRE(std::filesystem::exists(paths.log_file));  // 统一日志目录
    CHECK_FALSE(ReadText(paths.log_file).empty());

    std::atomic<int> exit_code{-1};
    std::thread runner([&] { exit_code = process.Run(); });
    process.RequestStop("test");
    runner.join();
    REQUIRE(exit_code.load() == 0);

    {
        std::string error;
        const auto control = ReadControlSnapshot(paths.control_file, &error);
        REQUIRE(control.has_value());
        CHECK(control->state == "stopped");
        CHECK(control->health == "ok");
        CHECK(control->last_shutdown == "clean");
    }
    CHECK_FALSE(std::filesystem::exists(paths.lock_file));
    const auto lines = GatewayBootHistory(paths.boot_history).ReadAll();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].kind == GatewayBootLine::Kind::Boot);
    CHECK(lines[1].kind == GatewayBootLine::Kind::Shutdown);
    CHECK(lines[1].clean);
    CHECK(lines[1].reason == "test");
}

TEST_CASE("控制文件:替换失败不许删正式目标换成功(src 收口审计 P1 原子写)") {
    // 目标位被一个空目录占着:原子替换换不上去,写侧必须报错收场,且那个
    // 目录必须原样还在。老的私房写法(rename 失败 -> 先 remove(target) ->
    // 再 rename)会把目录删掉、把写报成"成功"——那份成功是拿"正式目标先
    // 消失"换来的,Windows 上留出文件不存在窗口,正是审计单点名要杀的。
    const auto root = MakeTempRoot("ctrl-no-delete");
    const auto control_file = root / "control.json";
    std::error_code ec;
    std::filesystem::create_directories(control_file, ec);
    REQUIRE(std::filesystem::is_directory(control_file));

    GatewayControlSnapshot snapshot;
    snapshot.profile = "default";
    snapshot.boot_id = "boot-test";
    snapshot.state = "running";
    snapshot.health = "ok";

    const std::string error = WriteControlSnapshot(control_file, snapshot);
    CHECK_MESSAGE(!error.empty(), "替换失败必须报错,不许删掉占位目标换成功");
    CHECK(std::filesystem::is_directory(control_file));
    // 失败收场后不许留下自己的临时件。
    bool tmp_left = false;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.path().filename().string().find(".tmp") != std::string::npos) {
            tmp_left = true;
        }
    }
    CHECK_FALSE(tmp_left);
}

TEST_CASE("进程:stop 控制命令定向 boot_id,陈旧命令不追杀新实例") {
    const auto root = MakeTempRoot("ctrl");
    const auto paths = PathsOf(root);
    GatewayProcess process(MakeOptions(paths));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);

    // 陈旧命令:boot_id 指上一只实例——消费即删,不停。
    GatewayStopCommand stale;
    stale.boot_id = "gw-not-this-one";
    stale.requested_at_ms = 1;
    REQUIRE(WriteStopCommand(paths.control_dir, stale).empty());
    CHECK_FALSE(PollStopCommand(paths.control_dir, process.boot_id()));
    CHECK_FALSE(std::filesystem::exists(paths.control_dir / "stop.json"));

    // 指名命令:该停。
    GatewayStopCommand mine;
    mine.boot_id = process.boot_id();
    mine.requested_at_ms = 2;
    REQUIRE(WriteStopCommand(paths.control_dir, mine).empty());
    CHECK(PollStopCommand(paths.control_dir, process.boot_id()));

    std::atomic<int> exit_code{-1};
    std::thread runner([&] { exit_code = process.Run(); });
    // Run 的主循环也会撞见命令文件;这里再补一刀进程内停止,两条路都收口。
    process.RequestStop("test");
    runner.join();
    CHECK(exit_code.load() == 0);
}

TEST_CASE("进程:StopGateway 投命令并等到干净退出") {
    const auto root = MakeTempRoot("stopgw");
    const auto paths = PathsOf(root);
    GatewayProcess process(MakeOptions(paths));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);

    std::atomic<int> exit_code{-1};
    std::thread runner([&] { exit_code = process.Run(); });
    const auto outcome = StopGateway(paths, 8000);
    runner.join();
    REQUIRE(outcome.status == GatewayStopOutcome::Status::Stopped);
    CHECK(exit_code.load() == 0);
    CHECK_FALSE(std::filesystem::exists(paths.lock_file));
}

TEST_CASE("进程:第二实例抢锁退 AlreadyRunning,不碰第一只的账") {
    const auto root = MakeTempRoot("second");
    const auto paths = PathsOf(root);
    GatewayProcess first(MakeOptions(paths));
    REQUIRE(first.Start().status == GatewayProcess::StartResult::Status::Started);

    GatewayProcess second(MakeOptions(paths));
    const auto start = second.Start();
    REQUIRE(start.status == GatewayProcess::StartResult::Status::AlreadyRunning);
    CHECK(start.holder.boot_id == first.boot_id());
    // 各自的 root 各自收;这里不跑 Run,锁由析构 RAII 释放。
}

TEST_CASE("进程:关机钩子没收净 → 退 4,账上 timeout 不假写 clean") {
    const auto root = MakeTempRoot("hook");
    const auto paths = PathsOf(root);
    GatewayProcess process(MakeOptions(paths));
    process.AddShutdownHook({"stubborn", [] { return false; }});
    process.AddShutdownHook({"fine", [] { return true; }});
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);

    std::atomic<int> exit_code{-1};
    std::thread runner([&] { exit_code = process.Run(); });
    process.RequestStop("test");
    runner.join();
    REQUIRE(exit_code.load() == 4);

    {
        std::string error;
        const auto control = ReadControlSnapshot(paths.control_file, &error);
        REQUIRE(control.has_value());
        CHECK(control->state == "stopped");
        CHECK(control->health == "degraded");
        CHECK(control->last_shutdown == "timeout");
    }
    const auto lines = GatewayBootHistory(paths.boot_history).ReadAll();
    REQUIRE(lines.size() == 2);
    CHECK_FALSE(lines[1].clean);
    CHECK(ReadText(paths.log_file).find("stubborn") != std::string::npos);
}

TEST_CASE("SafeMode:连续非干净关机达阈值,启动即 degraded;干净关机破连击") {
    const auto root = MakeTempRoot("safe");
    const auto paths = PathsOf(root);
    // 预写三连击(boot 无干净收口)。
    for (int i = 0; i < 3; ++i) {
        GatewayBootLine line;
        line.kind = GatewayBootLine::Kind::Boot;
        line.boot_id = "old-" + std::to_string(i);
        line.pid = 42;
        line.at_ms = i;
        REQUIRE(GatewayBootHistory(paths.boot_history).Append(line).empty());
    }

    GatewayProcess process(MakeOptions(paths));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);
    CHECK(process.safe_mode());
    {
        std::string error;
        const auto control = ReadControlSnapshot(paths.control_file, &error);
        REQUIRE(control.has_value());
        CHECK(control->state == "running");
        CHECK(control->health == "degraded");
        CHECK(control->safe_mode);
    }

    std::atomic<int> exit_code{-1};
    std::thread runner([&] { exit_code = process.Run(); });
    process.RequestStop("test");
    runner.join();
    CHECK(exit_code.load() == 0);  // SafeMode 里干净关机照记 clean
    CHECK(CountUncleanBootStreak(GatewayBootHistory(paths.boot_history).ReadAll()) == 0);

    GatewayProcess next(MakeOptions(paths));
    REQUIRE(next.Start().status == GatewayProcess::StartResult::Status::Started);
    CHECK_FALSE(next.safe_mode());  // 连击已破,回正常
    next.RequestStop("test");
}

// ---------------------------------------------------------------------------
// status 探测与零副作用
// ---------------------------------------------------------------------------

TEST_CASE("status:未运行时零建目录零写盘(disabled 零副作用合同)") {
    const auto root = MakeTempRoot("zerowrite");
    const auto paths = PathsOf(root);
    const auto probe = ProbeGateway(paths);
    CHECK(probe.state == GatewayProbe::State::NotRunning);
    CHECK_FALSE(std::filesystem::exists(root));  // 连根都没建

    const auto stop = StopGateway(paths, 200);
    CHECK(stop.status == GatewayStopOutcome::Status::NotRunning);
    CHECK_FALSE(std::filesystem::exists(root));  // stop 也不留一片纸
}

TEST_CASE("status:活实例报 running;锁没了快照还在报 stale remnant") {
    const auto root = MakeTempRoot("probe");
    const auto paths = PathsOf(root);
    GatewayProcess process(MakeOptions(paths));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);

    const auto running = ProbeGateway(paths);
    REQUIRE(running.state == GatewayProbe::State::Running);
    CHECK(running.holder.boot_id == process.boot_id());
    CHECK(running.control.has_value());
    CHECK(ProbeToJson(running)["state"] == "running");

    process.RequestStop("test");
    // 硬杀模拟:锁直接删(进程没走关机流程,control 仍说 running)。
    std::error_code ec;
    std::filesystem::remove(paths.lock_file, ec);
    const auto remnant = ProbeGateway(paths);
    CHECK(remnant.state == GatewayProbe::State::StaleRemnant);
}

TEST_CASE("status:坏 control endpoint 不崩,降级报读不懂") {
    const auto root = MakeTempRoot("badctl");
    const auto paths = PathsOf(root);
    GatewayProcess process(MakeOptions(paths));
    REQUIRE(process.Start().status == GatewayProcess::StartResult::Status::Started);
    WriteText(paths.control_file, "{ broken json");

    const auto probe = ProbeGateway(paths);
    REQUIRE(probe.state == GatewayProbe::State::Running);  // 锁仍判活
    CHECK(probe.control_unreadable);
    CHECK_FALSE(probe.control.has_value());
    const auto json = ProbeToJson(probe);
    CHECK(json["error_code"] == "gateway.control_unreachable");
    CHECK(probe.detail.find("控制快照读不懂") != std::string::npos);
    process.RequestStop("test");
}

TEST_CASE("status:陈旧锁(持有者死透/PID 复用)报 stale_lock") {
    const auto root = MakeTempRoot("staleprobe");
    const auto paths = PathsOf(root);
    GatewayLockRecord reused = SelfRecord("boot-dead");
    reused.start_token = "0000000000000000-token-mismatch";
    WriteText(paths.lock_file, reused.ToJson().dump());

    const auto probe = ProbeGateway(paths);
    CHECK(probe.state == GatewayProbe::State::StaleLock);
}

// ---------------------------------------------------------------------------
// CLI 解析
// ---------------------------------------------------------------------------

TEST_CASE("CLI:gateway 子命令解析与用法错误") {
    using lubancode::app::ParseCliArgs;
    {
        const auto parsed = ParseCliArgs({"lubancode", "gateway", "run"});
        REQUIRE(parsed.action == lubancode::app::CliAction::RunGateway);
        CHECK(parsed.gateway.verb == "run");
        CHECK(parsed.gateway.profile.empty());
    }
    {
        const auto parsed = ParseCliArgs({"lubancode", "gateway", "status", "--json"});
        REQUIRE(parsed.action == lubancode::app::CliAction::RunGateway);
        CHECK(parsed.gateway.json);
    }
    {
        const auto parsed =
            ParseCliArgs({"lubancode", "gateway", "stop", "--profile", "work.main"});
        REQUIRE(parsed.action == lubancode::app::CliAction::RunGateway);
        CHECK(parsed.gateway.verb == "stop");
        CHECK(parsed.gateway.profile == "work.main");
    }
    {
        const auto parsed = ParseCliArgs({"lubancode", "gateway"});
        CHECK(parsed.action == lubancode::app::CliAction::BadGateway);
    }
    {
        const auto parsed = ParseCliArgs({"lubancode", "gateway", "install"});
        CHECK(parsed.action == lubancode::app::CliAction::BadGateway);
        CHECK(parsed.error_text.find("G1 未实现") != std::string::npos);
    }
    {
        const auto parsed = ParseCliArgs({"lubancode", "gateway", "run", "--json"});
        CHECK(parsed.action == lubancode::app::CliAction::BadGateway);
    }
    {
        const auto parsed =
            ParseCliArgs({"lubancode", "gateway", "run", "--profile", "a/b"});
        CHECK(parsed.action == lubancode::app::CliAction::BadGateway);
    }
}
