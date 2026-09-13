// 常驻总装 V0 第四件事:锁的真实双进程竞争册。旧册(test_gateway_process)
// 的锁案全在单进程内按序走,证明不了互斥——单内 §三 点名"GatewayLock::
// TryAcquire 先读文件再写文件,不能仅凭顺序单测断言并发启动互斥"。
// 本册起真 racer 子进程(tests/support/gateway_lock_racer.cpp,跑生产同一
// 份 TryAcquire),两只并发抢同一把锁,按事件流时间戳对账:
//   - 任意时刻至多一只持锁(区间相交即双持;[t_acq,t_rel] 取自实际持锁
//     区间之内,相交无假阳性);
//   - 竞争期真出现过 RefusedAliveHolder(不是各跑各的没碰上);
//   - 无非预期错误(坏锁/IoError)。
// 另钉 ownerEpoch fencing 合同:锁账带 owner_epoch(=boot_id),同进程
// 不同 epoch 互不相认;陈旧锁(PID 复用)清后重拿仍是原子创建占位。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/process.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/session_lock.hpp"

using namespace lubancode::gateway;

namespace {

struct RaceEvent {
    std::int64_t t_ms = 0;
    std::string event;
};

struct RaceLog {
    std::vector<RaceEvent> events;
    int acquired = 0;
    int refused = 0;
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
            log.errors = line.value("errors", 0);
            continue;
        }
        if (!line.contains("t") || !line["t"].is_number_integer()) continue;
        if (!line.contains("event") || !line["event"].is_string()) continue;
        log.events.push_back(RaceEvent{line["t"].get<std::int64_t>(),
                                       line["event"].get<std::string>()});
    }
    return log;
}

// 一只 racer 的持锁区间序列(按事件流配对 acquire/release)。
std::vector<std::pair<std::int64_t, std::int64_t>> HoldIntervals(const RaceLog& log) {
    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    std::optional<std::int64_t> opened;  // doctest 册里 optional 就地用
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

// Windows 宽字符路径与 UTF-8 argv 的桥:platform::RunProcess 收 UTF-8。
std::string RacePathText(const std::filesystem::path& path) {
    return lubancode::tools::PathToUtf8(path);
}

// 起一只 racer 并等它跑完(真子进程,经生产进程口 RunProcess)。
int RunRacer(const char* mode, const std::filesystem::path& lock_file, const std::string& boot_id,
             int rounds, int hold_ms, const std::filesystem::path& events_out) {
    const std::string exe = LUBANCODE_GATEWAY_RACER_EXE;
    const std::vector<std::string> argv = {
        exe, mode, RacePathText(lock_file), boot_id, std::to_string(rounds),
        std::to_string(hold_ms), RacePathText(events_out)};
    const auto result = lubancode::platform::RunProcess(argv, /*timeout_ms=*/90000);
    return static_cast<int>(result.exit_code);
}

GatewayLockRecord SelfRecord(const std::string& boot_id) {
    GatewayLockRecord record;
    record.pid = lubancode::platform::CurrentProcessId();
    record.start_token = lubancode::trajectory::CurrentProcessStartToken();
    record.boot_id = boot_id;
    record.owner_epoch = boot_id;  // V0:一 boot 一 epoch
    record.acquired_at_ms = lubancode::platform::WallClockNowMs();
    return record;
}

std::filesystem::path TempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-gw-race-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

}  // namespace

TEST_CASE("双进程竞争:两只 racer 抢同一把锁,至多一只持锁,且真撞过拒绝") {
    const auto root = TempRoot("two-procs");
    const auto lock_file = root / "default" / "gateway.lock";
    const auto events_a = root / "race-a.jsonl";
    const auto events_b = root / "race-b.jsonl";

    // 真起两只子进程并发跑;各自的 RunProcess 阻塞到 racer 退出。
    std::atomic<int> exit_a{-1};
    std::atomic<int> exit_b{-1};
    std::thread thread_a([&] {
        exit_a =
            RunRacer("gateway", lock_file, "boot-race-a", /*rounds=*/120, /*hold_ms=*/25, events_a);
    });
    std::thread thread_b([&] {
        exit_b =
            RunRacer("gateway", lock_file, "boot-race-b", /*rounds=*/120, /*hold_ms=*/25, events_b);
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
    // 真竞争的证据:至少一只撞过活持有者拒绝(两只都活着抢同一把锁,
    // 交替窗口里必然出现;全零说明两只压根没碰上,竞争册失效)。
    CHECK(log_a.refused + log_b.refused > 0);

    // 互斥对账:任一 A 持锁区间与任一 B 持锁区间不得相交。相等毫秒的
    // 相接(放与取同毫秒)放过——真双持必然跨毫秒(持锁 25ms,输不起
    // 一个毫秒的钟差)。
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

TEST_CASE("ownerEpoch fencing:锁账带 epoch,同进程不同 epoch 互不相认") {
    const auto root = TempRoot("epoch");
    const auto lock_file = root / "default" / "gateway.lock";

    GatewayLock first;
    auto got = GatewayLock::TryAcquire(lock_file, SelfRecord("boot-1"), &first);
    REQUIRE(got.status == GatewayLock::AcquireResult::Status::Acquired);
    CHECK(first.owner_epoch() == "boot-1");

    // 同 pid、同 token、不同 epoch(= 不同 Gateway 实例代):活进程持着,
    // 拒绝。fencing 语义——旧 epoch 不能靠"进程号相同"续锁。
    GatewayLock second;
    auto refused = GatewayLock::TryAcquire(lock_file, SelfRecord("boot-2"), &second);
    REQUIRE(refused.status == GatewayLock::AcquireResult::Status::RefusedAliveHolder);
    CHECK(refused.holder.owner_epoch == "boot-1");
    CHECK_FALSE(second.holds());

    // 同实例重入(四元组全同):幂等续持。
    GatewayLock again;
    auto resumed = GatewayLock::TryAcquire(lock_file, SelfRecord("boot-1"), &again);
    REQUIRE(resumed.status == GatewayLock::AcquireResult::Status::Acquired);
    CHECK(again.holds());
    again.Release();  // 续持分支无句柄,Release 走删文件
    CHECK_FALSE(std::filesystem::exists(lock_file));

    first.Release();
}

TEST_CASE("原子互斥:锁文件占位失败必因已存在,坏锁保守不删") {
    const auto root = TempRoot("atomic");
    const auto lock_file = root / "default" / "gateway.lock";
    std::error_code ec;
    std::filesystem::create_directories(lock_file.parent_path(), ec);

    // 坏锁(半写 JSON):读不懂,保守拒绝,文件原样。
    {
        std::ofstream stream(lock_file, std::ios::binary | std::ios::trunc);
        stream << "{ half written";
    }
    GatewayLock refused;
    auto broken = GatewayLock::TryAcquire(lock_file, SelfRecord("boot-x"), &refused);
    REQUIRE(broken.status == GatewayLock::AcquireResult::Status::RefusedBrokenLock);
    CHECK(broken.detail.find("gateway.lock_stale") != std::string::npos);
    CHECK(std::filesystem::exists(lock_file));

    // 旧 schema(无 owner_epoch)的锁同样读不懂:不猜、不静默升级。
    {
        std::ofstream stream(lock_file, std::ios::binary | std::ios::trunc);
        stream << R"({"pid":123,"start_token":"t","boot_id":"b","acquired_at_ms":1})";
    }
    GatewayLock legacy;
    auto legacy_result = GatewayLock::TryAcquire(lock_file, SelfRecord("boot-y"), &legacy);
    REQUIRE(legacy_result.status == GatewayLock::AcquireResult::Status::RefusedBrokenLock);
    CHECK(std::filesystem::exists(lock_file));  // 也没删
}

TEST_CASE("账号锁双进程竞争:channel AccountLock 同一把尺,至多一只持锁") {
    // V0 同步把 AccountLock 从"先读后写"改成 create-new 原子占位——与
    // GatewayLock 同一把尺。racer 的 account 模式跑生产同一份
    // AccountLock::TryAcquire。
    const auto root = TempRoot("account-two-procs");
    const auto lock_file = root / "locks" / "qq-10086.lock";
    const auto events_a = root / "acct-a.jsonl";
    const auto events_b = root / "acct-b.jsonl";

    std::atomic<int> exit_a{-1};
    std::atomic<int> exit_b{-1};
    std::thread thread_a([&] {
        exit_a = RunRacer("account", lock_file, "mgr-a", /*rounds=*/120, /*hold_ms=*/25, events_a);
    });
    std::thread thread_b([&] {
        exit_b = RunRacer("account", lock_file, "mgr-b", /*rounds=*/120, /*hold_ms=*/25, events_b);
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
    CHECK(log_a.refused + log_b.refused > 0);  // 真撞过

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
