// 更新器安装锁册(批二第③单,真子进程集成)。覆盖:
//   - 取锁/放锁基本盘:锁账三字段(pid/acquired_at_utc/start_token)、
//     RAII 析构放锁、updates/ 自动建;
//   - 活拒:真子进程(racer)持锁,本进程 TryAcquire 吃 RefusedAliveHolder
//     (holder_pid 对得上);子进程退出后重抢成功;
//   - 死 pid 陈锁:真退出的子进程 pid 写进锁账 -> rename 成 .stale-<ts>
//     重抢(断线恢复);
//   - PID 复用陈锁:pid 是自己但 start_token 对不上 -> 同样按陈锁清抢;
//   - 旧格式锁(缺 start_token):按 pid 裁决 -> 同 pid 不可重入拒;
//   - 坏锁保守拒:半截 JSON 不删不动(口径差见 lock.hpp 文件头);
//   - release 只删自己 pid 的锁:外进程 pid 的锁,非持有对象 Release 不动;
//   - 双进程 racer 竞争:两只真子进程并发抢同一把 updates/.lock,按事件
//     流时间戳对账"至多一只持锁"+真撞过拒绝(照 gateway 的双进程册写法,
//     不拿同进程顺序单测冒充互斥)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "updater/layout.hpp"
#include "updater/lock.hpp"

namespace {

using namespace lubancode::updater;
using Status = InstallRootLock::AcquireResult::Status;

std::filesystem::path TempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_updater_lock_" + std::string(name) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void WriteBytes(const std::filesystem::path& file, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::optional<std::string> ReadBytes(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string RacePathText(const std::filesystem::path& path) {
    return lubancode::platform::PathToUtf8(path);
}

// 起一只 racer 并等它跑完(真子进程,经生产进程口)。
int RunRacer(const std::filesystem::path& install_root, int rounds, int hold_ms,
             const std::filesystem::path& events_out) {
    const std::string exe = LUBANCODE_UPDATER_RACER_EXE;
    const std::vector<std::string> argv = {exe, RacePathText(install_root),
                                           std::to_string(rounds), std::to_string(hold_ms),
                                           RacePathText(events_out)};
    const auto result = lubancode::platform::RunProcess(argv, /*timeout_ms=*/90000);
    return static_cast<int>(result.exit_code);
}

// 后台起一只 racer,拿回 pid(活拒/死 pid 夹具用)。起不来给 nullopt,
// 断言留给调用方(doctest 断言宏在非 void 函数里编不过)。
struct BackgroundRacer {
    std::shared_ptr<lubancode::platform::BackgroundProcessHandle> handle;
    unsigned long pid = 0;
};

std::optional<BackgroundRacer> StartRacer(const std::filesystem::path& install_root, int rounds,
                                          int hold_ms) {
    const std::string exe = LUBANCODE_UPDATER_RACER_EXE;
    const std::vector<std::string> argv = {exe, RacePathText(install_root),
                                           std::to_string(rounds), std::to_string(hold_ms),
                                           RacePathText(install_root / "events.jsonl")};
    auto spawned = lubancode::platform::RunProcessBackground(argv);
    if (!spawned.success) return std::nullopt;
    return BackgroundRacer{spawned.handle, spawned.pid};
}

// 等锁文件出现(子进程起跑到持锁有毫秒级窗口),最多 wait_ms。
bool WaitForLockFile(const std::filesystem::path& lock_file, int wait_ms) {
    for (int waited = 0; waited < wait_ms; waited += 25) {
        std::error_code ec;
        if (std::filesystem::exists(lock_file, ec)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::error_code ec;
    return std::filesystem::exists(lock_file, ec);
}

// ---- racer 事件流(与 gateway 竞争册同款) ----

struct RaceEvent {
    std::int64_t t_ms = 0;
    std::string event;
};

struct RaceLog {
    std::vector<RaceEvent> events;
    int acquired = 0;
    int refused = 0;
    int stale = 0;
    int errors = 0;
};

RaceLog ReadRaceLog(const std::filesystem::path& file) {
    RaceLog log;
    std::ifstream in(file, std::ios::binary);
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) continue;
        const nlohmann::json line = nlohmann::json::parse(text, nullptr, false);
        if (!line.is_object()) continue;
        if (line.contains("summary") && line["summary"].is_boolean() && line["summary"].get<bool>()) {
            log.acquired = line.value("acquired", 0);
            log.refused = line.value("refused", 0);
            log.stale = line.value("stale", 0);
            log.errors = line.value("errors", 0);
            continue;
        }
        if (!line.contains("t") || !line["t"].is_number_integer()) continue;
        if (!line.contains("event") || !line["event"].is_string()) continue;
        log.events.push_back(RaceEvent{line["t"].get<std::int64_t>(), line["event"].get<std::string>()});
    }
    return log;
}

std::vector<std::pair<std::int64_t, std::int64_t>> HoldIntervals(const RaceLog& log) {
    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    std::optional<std::int64_t> opened;
    for (const RaceEvent& event : log.events) {
        if (event.event == "acquire") {
            opened = event.t_ms;
        } else if (event.event == "release" && opened.has_value()) {
            intervals.emplace_back(*opened, event.t_ms);
            opened.reset();
        }
    }
    return intervals;
}

}  // namespace

// ------------------------------------------------------------- 基本盘 ---

TEST_CASE("取锁/放锁: 锁账三字段,RAII 析构放锁,updates/ 自动建") {
    const auto root = TempRoot("basic");
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallRootLock lock;
    const auto result = InstallRootLock::TryAcquire(paths, &lock);
    REQUIRE(result.status == Status::Acquired);
    CHECK(lock.holds());
    CHECK_FALSE(result.cleared_stale);

    // 锁账:{pid, acquired_at_utc, start_token}(start_token 是本侧新增的
    // 可选字段,python 写的旧锁没有它,读侧缺省按 pid 裁决)。
    const auto bytes = ReadBytes(InstallLockPath(paths));
    REQUIRE(bytes.has_value());
    const nlohmann::json record = nlohmann::json::parse(*bytes, nullptr, false);
    REQUIRE(record.is_object());
    CHECK(record["pid"] == lubancode::platform::CurrentProcessId());
    CHECK(record.contains("acquired_at_utc"));
    CHECK(record["acquired_at_utc"].is_string());
    CHECK(record.contains("start_token"));
    CHECK(record["start_token"].is_string());

    lock.Release();
    CHECK_FALSE(lock.holds());
    CHECK_FALSE(std::filesystem::exists(InstallLockPath(paths)));
    lock.Release();  // 幂等

    // RAII:析构即放锁。
    {
        InstallRootLock scoped;
        REQUIRE(InstallRootLock::TryAcquire(paths, &scoped).status == Status::Acquired);
    }
    CHECK_FALSE(std::filesystem::exists(InstallLockPath(paths)));

    // move:句柄跟过去,原对象失持。
    InstallRootLock moved_from;
    REQUIRE(InstallRootLock::TryAcquire(paths, &moved_from).status == Status::Acquired);
    InstallRootLock moved_to = std::move(moved_from);
    CHECK_FALSE(moved_from.holds());
    CHECK(moved_to.holds());
    moved_to.Release();
}

TEST_CASE("同进程重入: 不可重入拒(python 口径)") {
    const auto root = TempRoot("reentrant");
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallRootLock first;
    REQUIRE(InstallRootLock::TryAcquire(paths, &first).status == Status::Acquired);

    InstallRootLock second;
    const auto refused = InstallRootLock::TryAcquire(paths, &second);
    REQUIRE(refused.status == Status::RefusedReentrant);
    CHECK(refused.holder_pid == lubancode::platform::CurrentProcessId());
    CHECK_FALSE(second.holds());
    CHECK(std::filesystem::exists(InstallLockPath(paths)));  // first 还持着

    first.Release();
}

// ------------------------------------------------------- 陈锁裁决 ---

TEST_CASE("死 pid 陈锁: rename 成 .stale-<ts> 重抢(断线恢复)") {
    const auto root = TempRoot("stale-dead");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // 夹具:真退出的子进程 pid(拿 racer 在旁的根跑一枪,等它退干净)。
    const auto other_root = TempRoot("stale-dead-helper");
    auto helper_started = StartRacer(other_root, 1, 1);
    REQUIRE(helper_started.has_value());
    const unsigned long dead_pid = helper_started->pid;
    REQUIRE(helper_started->handle->Wait(30000));

    // 旧格式锁账(python 写的形状:没有 start_token)。
    WriteBytes(InstallLockPath(paths),
               nlohmann::json{{"pid", dead_pid}, {"acquired_at_utc", "2026-09-20T12:00:00Z"}}.dump());

    InstallRootLock lock;
    const auto result = InstallRootLock::TryAcquire(paths, &lock);
    REQUIRE(result.status == Status::Acquired);
    CHECK(result.cleared_stale);
    // 陈锁挪走了(.stale-<epoch秒> 留现场),原位换成我们的新账。
    REQUIRE_FALSE(result.stale_moved_to.empty());
    CHECK(std::filesystem::exists(lubancode::platform::Utf8ToPath(result.stale_moved_to)));
    const auto now_bytes = ReadBytes(InstallLockPath(paths));
    REQUIRE(now_bytes.has_value());
    const nlohmann::json record = nlohmann::json::parse(*now_bytes, nullptr, false);
    CHECK(record["pid"] == lubancode::platform::CurrentProcessId());
    lock.Release();
}

TEST_CASE("PID 复用陈锁: pid 是自己但 token 对不上,按陈锁清抢") {
    const auto root = TempRoot("stale-reuse");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // pid = 本进程,token 是别人的(旧进程用过这个 pid 后死透)。
    WriteBytes(InstallLockPath(paths),
               nlohmann::json{{"pid", lubancode::platform::CurrentProcessId()},
                              {"acquired_at_utc", "2026-09-20T12:00:00Z"},
                              {"start_token", "bogus-token-from-dead-process"}}
                   .dump());

    InstallRootLock lock;
    const auto result = InstallRootLock::TryAcquire(paths, &lock);
    REQUIRE(result.status == Status::Acquired);
    CHECK(result.cleared_stale);
    lock.Release();
}

TEST_CASE("活拒: 真子进程持锁,RefusedAliveHolder 对得上 pid;退出后重抢") {
    const auto root = TempRoot("alive");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // racer 持锁 4 秒(rounds=1, hold=4000)。
    auto holder_started = StartRacer(root, 1, 4000);
    REQUIRE(holder_started.has_value());
    const unsigned long holder_pid = holder_started->pid;
    REQUIRE(WaitForLockFile(InstallLockPath(paths), 10000));

    InstallRootLock mine;
    const auto refused = InstallRootLock::TryAcquire(paths, &mine);
    REQUIRE(refused.status == Status::RefusedAliveHolder);
    CHECK(refused.holder_pid == holder_pid);
    CHECK_FALSE(mine.holds());
    CHECK(std::filesystem::exists(InstallLockPath(paths)));  // 别人的锁不动

    // 等持锁子进程退干净,再抢就成功。
    REQUIRE(holder_started->handle->Wait(30000));
    InstallRootLock retry;
    const auto again = InstallRootLock::TryAcquire(paths, &retry);
    REQUIRE(again.status == Status::Acquired);
    CHECK_FALSE(again.cleared_stale);  // 正常释放不是陈锁
    retry.Release();
}

TEST_CASE("坏锁保守拒: 半截 JSON 不删不动") {
    const auto root = TempRoot("broken");
    const LayoutPaths paths = MakeLayoutPaths(root);
    WriteBytes(InstallLockPath(paths), "{ half written");

    InstallRootLock lock;
    const auto result = InstallRootLock::TryAcquire(paths, &lock);
    REQUIRE(result.status == Status::RefusedBrokenLock);
    CHECK_FALSE(lock.holds());
    // 文件原样(口径差:python 挪走再抢,本侧照 GatewayLock 保守不动)。
    const auto bytes = ReadBytes(InstallLockPath(paths));
    REQUIRE(bytes.has_value());
    CHECK(*bytes == "{ half written");
}

TEST_CASE("release 只删自己 pid 的锁: 外进程 pid 的账,非持有对象不动") {
    const auto root = TempRoot("foreign-pid");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // 夹具:一只活着的子进程 pid(racer 持锁 4 秒,但持的是旁的根的锁)。
    const auto other_root = TempRoot("foreign-pid-helper");
    auto helper_started = StartRacer(other_root, 1, 4000);
    REQUIRE(helper_started.has_value());
    const unsigned long helper_pid = helper_started->pid;
    REQUIRE(WaitForLockFile(InstallLockPath(MakeLayoutPaths(other_root)), 10000));

    // 我们的根上手写一笔外进程 pid 的锁账;非持有对象的 Release 不动它。
    WriteBytes(InstallLockPath(paths),
               nlohmann::json{{"pid", helper_pid},
                              {"acquired_at_utc", "2026-09-20T12:00:00Z"},
                              {"start_token", "someone-else"}}
                   .dump());
    InstallRootLock bystander;
    bystander.Release();
    CHECK(std::filesystem::exists(InstallLockPath(paths)));

    REQUIRE(helper_started->handle->Wait(30000));
}

// --------------------------------------------------------- 双进程册 ---

TEST_CASE("双进程竞争: 两只 racer 抢同一把 updates/.lock,至多一只持锁") {
    const auto root = TempRoot("two-procs");
    const auto events_a = root / "race-a.jsonl";
    const auto events_b = root / "race-b.jsonl";

    std::atomic<int> exit_a{-1};
    std::atomic<int> exit_b{-1};
    std::thread thread_a([&] {
        exit_a = RunRacer(root, /*rounds=*/120, /*hold_ms=*/25, events_a);
    });
    std::thread thread_b([&] {
        exit_b = RunRacer(root, /*rounds=*/120, /*hold_ms=*/25, events_b);
    });
    thread_a.join();
    thread_b.join();

    REQUIRE(exit_a.load() == 0);
    REQUIRE(exit_b.load() == 0);
    const RaceLog log_a = ReadRaceLog(events_a);
    const RaceLog log_b = ReadRaceLog(events_b);
    REQUIRE(log_a.acquired > 0);
    REQUIRE(log_b.acquired > 0);
    REQUIRE(log_a.errors == 0);
    REQUIRE(log_b.errors == 0);
    // 真竞争的证据:至少一只撞过拒绝。
    CHECK(log_a.refused + log_b.refused > 0);
    // 竞争册里不该清陈锁(两只都活着)——清了说明裁决有毛病,如实红。
    CHECK(log_a.stale == 0);
    CHECK(log_b.stale == 0);

    // 互斥对账:任一 A 持锁区间与任一 B 持锁区间不得相交(相等毫秒的
    // 相接放过——gateway 册同款口径)。
    const auto intervals_a = HoldIntervals(log_a);
    const auto intervals_b = HoldIntervals(log_b);
    REQUIRE_FALSE(intervals_a.empty());
    REQUIRE_FALSE(intervals_b.empty());
    int overlaps = 0;
    for (const auto& [a1, a2] : intervals_a) {
        for (const auto& [b1, b2] : intervals_b) {
            if (a1 < b2 && b1 < a2) {
                ++overlaps;
            }
        }
    }
    CHECK(overlaps == 0);
}
