// 多渠道消息接入单阶段 2:账号锁册(configuration.md §11)。
// 规矩:核进程存活再清假死锁;活进程持有不可抢;读不懂的锁不删。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "channel/account_lock.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeTempDir(const char* test_name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-lock-test" + std::string(test_name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

AccountLockRecord MakeRecord(unsigned long pid, std::int64_t start_ms) {
    AccountLockRecord record;
    record.pid = pid;
    record.start_time_ms = start_ms;
    record.acquired_at_ms = 1724700000000;
    record.generation = 1;
    return record;
}

}  // namespace

TEST_CASE("空位取锁成功,RAII 释放删文件") {
    const auto dir = MakeTempDir("acquire_release");
    const auto lock_file = dir / "locks" / "qqbot-main.lock";
    AccountLock lock;
    const auto result =
        AccountLock::TryAcquire(lock_file, MakeRecord(4321, 100), [](unsigned long) { return true; },
                                &lock);
    REQUIRE(result.status == AccountLock::AcquireResult::Status::Acquired);
    CHECK(lock.holds());
    CHECK(std::filesystem::exists(lock_file));
    lock.Release();
    CHECK_FALSE(lock.holds());
    CHECK_FALSE(std::filesystem::exists(lock_file));
    // 幂等:再 Release 无事。
    lock.Release();
}

TEST_CASE("活进程持有时拒绝,报持有者 pid") {
    const auto dir = MakeTempDir("alive_holder");
    const auto lock_file = dir / "locks" / "qqbot-main.lock";
    {
        AccountLock first;
        const auto got = AccountLock::TryAcquire(
            lock_file, MakeRecord(1111, 100), [](unsigned long pid) { return pid == 1111; }, &first);
        REQUIRE(got.status == AccountLock::AcquireResult::Status::Acquired);
        // 不释放,直接挪走文件语义(模拟另一实例持锁):重新落一份持有人账。
    }
    // 手写一份"活进程"的锁(不走 RAII,免得析构删掉)。
    std::filesystem::create_directories(lock_file.parent_path());
    std::ofstream stream(lock_file);
    stream << MakeRecord(1111, 100).ToJson().dump();
    stream.close();

    AccountLock second;
    const auto refused = AccountLock::TryAcquire(
        lock_file, MakeRecord(2222, 200), [](unsigned long pid) { return pid == 1111; }, &second);
    REQUIRE(refused.status == AccountLock::AcquireResult::Status::RefusedAliveHolder);
    CHECK(refused.holder.pid == 1111);
    CHECK_FALSE(second.holds());
    CHECK(std::filesystem::exists(lock_file));  // 原锁不动
}

TEST_CASE("假死锁核过进程存活才清,清后可取") {
    const auto dir = MakeTempDir("stale_lock");
    const auto lock_file = dir / "locks" / "qqbot-main.lock";
    std::filesystem::create_directories(lock_file.parent_path());
    {
        std::ofstream stream(lock_file);
        stream << MakeRecord(9999, 100).ToJson().dump();
    }
    // alive-checker 报 9999 已死:清掉重拿。
    AccountLock lock;
    const auto result = AccountLock::TryAcquire(
        lock_file, MakeRecord(1234, 300), [](unsigned long pid) { return pid != 9999; }, &lock);
    REQUIRE(result.status == AccountLock::AcquireResult::Status::Acquired);
    CHECK(lock.holds());
}

TEST_CASE("读不懂的锁不删,明报") {
    const auto dir = MakeTempDir("broken_lock");
    const auto lock_file = dir / "locks" / "qqbot-main.lock";
    std::filesystem::create_directories(lock_file.parent_path());
    {
        std::ofstream stream(lock_file);
        stream << "{ this is not json";
    }
    AccountLock lock;
    const auto result = AccountLock::TryAcquire(
        lock_file, MakeRecord(1234, 100), [](unsigned long) { return false; }, &lock);
    REQUIRE(result.status == AccountLock::AcquireResult::Status::RefusedBrokenLock);
    CHECK_FALSE(lock.holds());
    CHECK(std::filesystem::exists(lock_file));  // 看不懂就不删

    // 半写账(缺字段)同样拒。
    {
        std::ofstream stream(lock_file, std::ios::trunc);
        stream << nlohmann::json{{"pid", 9999}}.dump();
    }
    const auto again = AccountLock::TryAcquire(
        lock_file, MakeRecord(1234, 100), [](unsigned long) { return false; }, &lock);
    CHECK(again.status == AccountLock::AcquireResult::Status::RefusedBrokenLock);
}

TEST_CASE("同一进程重入视为可续") {
    const auto dir = MakeTempDir("same_process");
    const auto lock_file = dir / "locks" / "qqbot-main.lock";
    std::filesystem::create_directories(lock_file.parent_path());
    {
        std::ofstream stream(lock_file);
        stream << MakeRecord(4321, 777).ToJson().dump();
    }
    AccountLock lock;
    const auto result = AccountLock::TryAcquire(
        lock_file, MakeRecord(4321, 777), [](unsigned long) { return true; }, &lock);
    // 同 pid 同 start_time:不用问 alive(自己当然活着),直接续。
    REQUIRE(result.status == AccountLock::AcquireResult::Status::Acquired);
}

TEST_CASE("锁账 JSON 往返严格,不记密钥") {
    const AccountLockRecord record = MakeRecord(4321, 777);
    const nlohmann::json json = record.ToJson();
    std::string error;
    const auto back = AccountLockRecord::FromJsonStrict(json, &error);
    REQUIRE(back.has_value());
    CHECK(back->pid == 4321);
    CHECK(back->start_time_ms == 777);
    // 锁账里没有密钥字段可放:未知键拒绝。
    nlohmann::json bad = json;
    bad["secret"] = "nope";
    CHECK_FALSE(AccountLockRecord::FromJsonStrict(bad, &error).has_value());
    // pid 0 拒。
    nlohmann::json zero_pid = json;
    zero_pid["pid"] = 0;
    CHECK_FALSE(AccountLockRecord::FromJsonStrict(zero_pid, &error).has_value());
}
