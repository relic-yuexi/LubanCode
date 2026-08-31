// 渠道账号锁(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/configuration.md §11:同一
// channel_id + account_id 只许一只本机实例持有;锁文件记 pid、start
// time、generation,不记密钥;假死锁须核进程存活再清,不可见锁便直接删。
//
// 锁文件落位:<state_root>/locks/<channel>-<account>.lock(configuration.md
// §5)。持锁是 RAII:AccountLock 析构即删锁文件(幂等)。
//
// 进程存活检查默认接 platform::IsProcessAlive,可注入假件(单测伪造
// "pid 还活着"/"pid 已死"两条路)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

// 锁文件里的账(不含任何密钥)。
struct AccountLockRecord {
    unsigned long pid = 0;
    std::int64_t start_time_ms = 0;   // 持锁进程的启动时刻(区分同 pid 复用)
    std::int64_t acquired_at_ms = 0;  // 本次取锁时刻
    int generation = 1;
    // 实例令牌:同一进程里的两只 ChannelManager 也互不相认(测试/嵌入式
    // 同进程多实例的常态)。仅 pid+start_time 相同不够——须 token 也同
    // 才算"同一实例重入续锁"。
    std::string instance_token;

    nlohmann::json ToJson() const;
    static std::optional<AccountLockRecord> FromJsonStrict(const nlohmann::json& json,
                                                           std::string* error);
};

class AccountLock {
public:
    // 进程存活检查口。默认绑 platform::IsProcessAlive;单测注入假件。
    using AliveChecker = std::function<bool(unsigned long)>;
    static AliveChecker DefaultAliveChecker();

    struct AcquireResult {
        enum class Status {
            Acquired,          // 本实例拿到锁(含清掉假死锁后重拿)
            RefusedAliveHolder,  // 别的活进程持着:不可抢
            RefusedBrokenLock,   // 锁文件在但读不懂:不敢删,留给人看
            IoError,             // 读写锁文件失败(detail 说明)
        };
        Status status = Status::IoError;
        AccountLockRecord holder;  // RefusedAliveHolder 时 = 活着的持有者
        std::string detail;        // 脱敏人话
    };

    // 尝试取锁。成功时 *out 持锁(RAII);失败时 out 不持任何东西。
    static AcquireResult TryAcquire(const std::filesystem::path& lock_file,
                                    const AccountLockRecord& self, const AliveChecker& alive,
                                    AccountLock* out);

    AccountLock() = default;
    AccountLock(const AccountLock&) = delete;
    AccountLock& operator=(const AccountLock&) = delete;
    AccountLock(AccountLock&& other) noexcept;
    AccountLock& operator=(AccountLock&& other) noexcept;
    ~AccountLock();

    bool holds() const { return !lock_file_.empty(); }
    const std::filesystem::path& lock_file() const { return lock_file_; }
    // 释放:删锁文件并摘持锁标记。幂等;未持锁时无事。
    void Release();

private:
    std::filesystem::path lock_file_;  // 空 = 未持锁
};

}  // namespace lubancode::channel
