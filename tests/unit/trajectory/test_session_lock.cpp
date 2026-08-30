// Session 独占锁测试(§7.5/§16.7):独占创建、同 session 第二 writer 拒绝、
// 只读 inspect 并行、陈旧锁核 PID+起始 token 身份(PID 复用防撞)、身份
// 读不出保守拒绝。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "platform/process.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode::trajectory;

namespace {

// 极不可能存在的 PID:Windows OpenProcess 报 INVALID_PARAMETER(死),
// POSIX kill(pid,0) 报 ESRCH(死)。
constexpr unsigned long kDeadPid = 4194303UL;

std::filesystem::path MakeDir(const char* tag) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lubancode-traj-lock-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 手植一枚残留锁(模拟上一只进程崩了留下的文件)。
void PlantLock(const std::filesystem::path& dir, const SessionLockOwner& owner) {
    std::ofstream file(dir / "session.lock", std::ios::binary | std::ios::trunc);
    file << owner.ToJson().dump();
}

SessionLockOwner DeadOwner() {
    SessionLockOwner owner;
    owner.pid = kDeadPid;
    owner.process_start_token = "1a2b3c";
    owner.acquired_at_ms = 1759000000000LL;
    return owner;
}

SessionLockOwner CurrentOwner() { return SessionManagerClock{}.LockOwner(); }

}  // namespace

TEST_CASE("lock: 独占创建,持有期间文件在,释放即删") {
    const std::filesystem::path dir = MakeDir("basic");
    const SessionLockOwner owner = CurrentOwner();
    auto lock = SessionLock::Acquire(dir, owner);
    REQUIRE(lock.has_value());
    CHECK(lock->holds());
    CHECK(std::filesystem::exists(dir / "session.lock"));
    // 元数据落盘齐:pid/token/时间。
    const auto held = SessionLock::Inspect(dir);
    REQUIRE(held.has_value());
    CHECK(held->pid == owner.pid);
    CHECK(held->process_start_token == owner.process_start_token);
    CHECK(held->acquired_at_ms == owner.acquired_at_ms);
    lock->Release();
    CHECK_FALSE(std::filesystem::exists(dir / "session.lock"));
    CHECK_FALSE(lock->holds());
    // 放了再拿,拿得到。
    auto again = SessionLock::Acquire(dir, owner);
    REQUIRE(again.has_value());
}

TEST_CASE("lock: 同 session 第二只 writer 拒绝(活锁)") {
    const std::filesystem::path dir = MakeDir("second-writer");
    const SessionLockOwner owner = CurrentOwner();
    auto first = SessionLock::Acquire(dir, owner);
    REQUIRE(first.has_value());
    auto second = SessionLock::Acquire(dir, owner);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().rfind("lock.held_by_live_process", 0) == 0);
    // 第一只放掉,第二只便进得来。
    first->Release();
    auto third = SessionLock::Acquire(dir, owner);
    REQUIRE(third.has_value());
}

TEST_CASE("lock: 只读 inspect 可并行,不抢锁不碰文件") {
    const std::filesystem::path dir = MakeDir("inspect");
    auto lock = SessionLock::Acquire(dir, CurrentOwner());
    REQUIRE(lock.has_value());
    // inspect 期间文件不动,锁仍归持有者。
    const auto before = SessionLock::Inspect(dir);
    REQUIRE(before.has_value());
    CHECK(SessionLock::Acquire(dir, CurrentOwner()).error().rfind("lock.held_by_live_process", 0) ==
          0);
    const auto after = SessionLock::Inspect(dir);
    REQUIRE(after.has_value());
    CHECK(after->pid == before->pid);
    // 无锁文件给 nullopt。
    const std::filesystem::path empty = MakeDir("inspect-empty");
    CHECK_FALSE(SessionLock::Inspect(empty).has_value());
}

TEST_CASE("lock: 死 PID 陈旧锁核身份后可恢复") {
    const std::filesystem::path dir = MakeDir("stale-dead");
    PlantLock(dir, DeadOwner());
    // 残锁持有者死透:清掉重拿。
    auto lock = SessionLock::Acquire(dir, CurrentOwner());
    REQUIRE(lock.has_value());
    const auto held = SessionLock::Inspect(dir);
    REQUIRE(held.has_value());
    CHECK(held->pid == CurrentOwner().pid);
}

TEST_CASE("lock: PID 复用——活 PID 但起始 token 对不上,判 stale") {
    const std::filesystem::path dir = MakeDir("pid-reuse");
    SessionLockOwner reused = DeadOwner();
    reused.pid = lubancode::platform::CurrentProcessId();  // PID 活着
    reused.process_start_token = "00000000";    // 但那是上一个进程的 token
    PlantLock(dir, reused);
    auto lock = SessionLock::Acquire(dir, CurrentOwner());
    REQUIRE(lock.has_value());
    // 反之:活 PID 且 token 对得上,必须拒绝,不许抢。
    SessionLockOwner real = CurrentOwner();
    if (!real.process_start_token.empty()) {
        const std::filesystem::path dir2 = MakeDir("pid-live");
        PlantLock(dir2, real);
        auto refused = SessionLock::Acquire(dir2, CurrentOwner());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().rfind("lock.held_by_live_process", 0) == 0);
    }
}

TEST_CASE("lock: 身份读不出的锁,保守拒绝不强抢") {
    const std::filesystem::path dir = MakeDir("unreadable");
    {
        std::ofstream file(dir / "session.lock", std::ios::binary | std::ios::trunc);
        file << "not json at all";
    }
    auto refused = SessionLock::Acquire(dir, CurrentOwner());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().rfind("lock.unreadable", 0) == 0);
    // 文件原样保留,没被清。
    CHECK(std::filesystem::exists(dir / "session.lock"));
}

TEST_CASE("lock: 进程起始 token 本进程稳定,不同 pid 可探") {
    const std::string mine = CurrentProcessStartToken();
    MESSAGE("current start token: ", mine);
    // 本进程恒非空(Windows FILETIME / /proc starttime)。
    CHECK_FALSE(mine.empty());
    // 死 PID 探不到,给空串。
    CHECK(ProcessStartTokenOf(kDeadPid).empty());
    CHECK_EQ(ProbeLockHolder(DeadOwner()), LockHolderState::Dead);
}
