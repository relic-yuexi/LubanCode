// PairingStore:远端 sender 配对账(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/configuration.md §6:pairing 不是
// 平台登录——它决定"哪个远端 sender 可以驱动 LubanCode"。规矩:
//   - 未知 DM 在 dm_policy=pairing 下不进 Agent,生成一次性 code,回固定
//     配对提示,本地 approve 后才放行后续消息;
//   - 记录带 channel、account、sender id、创建/过期时间与尝试次数;
//   - code 用加密随机数,短期有效,存 hash 不存明文;
//   - 重复申请限速;批准只认宿主看到的 sender id。
//
// 持久化:<account_dir>/pairing.json(小快照,原子写:temp + replace)。
// pairing 记录短命数量少,快照文件够用;耐久事件账归 ingress journal,
// 这里不越界。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::channel {

// pairing code 有效期:5 分钟(短期有效;首版钉死,不进配置)。
inline constexpr std::int64_t kPairingCodeTtlMs = 5 * 60 * 1000;
// 同 sender 重复申请限速:30 秒内只发一枚新 code。
inline constexpr std::int64_t kPairingRequestCooldownMs = 30 * 1000;
// code 长度:8 位(大小写字母 + 数字,去掉易混 0/O/1/I)。
inline constexpr std::size_t kPairingCodeLength = 8;

class PairingStore {
public:
    // 值语义被 mutex 禁:工厂返回 unique_ptr,装配方持指针。
    PairingStore() = default;
    PairingStore(const PairingStore&) = delete;
    PairingStore& operator=(const PairingStore&) = delete;

    struct Record {
        std::string channel_id;
        std::string account_id;
        std::string sender_id;
        std::string code_hash;  // SHA-256 hex,明文不落盘
        std::int64_t created_at_ms = 0;
        std::int64_t expires_at_ms = 0;
        enum class Status { Pending, Approved, Rejected, Expired } status = Status::Pending;
    };

    // code 随机源(测试注入固定列)。
    using CodeGenerator = std::function<std::string()>;
    // 生成 kPairingCodeLength 位去易混字符的随机串。随机源:每平台
    // random_device 播种的 mt19937(配对 code 威胁模型是"别猜得中",
    // 不是密钥级);测试用注入。
    static std::string DefaultCodeGenerator();

    // 打开(或新建)账号 pairing 账。account_dir =
    // <state_root>/<channel>/<account>。io_error 时 store 不可写
    // (write_blocked),读到的旧账仍可用。
    static std::unique_ptr<PairingStore> Open(const std::filesystem::path& account_dir,
                                              std::string channel_id, std::string account_id);

    bool write_blocked() const { return write_blocked_; }
    const std::string& last_error() const { return last_error_; }

    // 未知 sender 申请配对。成功返回明文 code(只此一次,交给回复链路);
    // 限速期内重复申请返回错误(stable reason: rate_limited)。
    std::optional<std::string> RequestPairing(const std::string& sender_id, std::int64_t now_ms,
                                              const CodeGenerator& generator = nullptr);

    // 用明文 code 批准/拒绝。成功返回被批准的 sender id;code 不认、过期、
    // 已处理都报错(stable reason: not_found / expired / already_finalized)。
    // 批准只认宿主看到的 sender id——sender 在记录里,不认调用方转述。
    std::optional<std::string> Approve(const std::string& code, std::int64_t now_ms);
    std::optional<std::string> Reject(const std::string& code, std::int64_t now_ms,
                                      std::string* error = nullptr);

    // sender 是否已批准(持久;批准记录不过期)。
    bool IsSenderApproved(const std::string& sender_id) const;

    struct PendingView {
        std::string sender_id;
        std::int64_t expires_at_ms = 0;
    };
    // 待审清单(doctor/后续 /channel pairing list 用),已过期的不列。
    std::vector<PendingView> PendingList(std::int64_t now_ms) const;
    std::size_t approved_count() const;

    // 快照(测试与诊断)。
    std::vector<Record> Records() const;

private:
    std::optional<std::string> FinalizeByCode(const std::string& code, std::int64_t now_ms,
                                              Record::Status target, std::string* sender_out,
                                              std::string* error);
    bool SaveLocked();

    std::filesystem::path pairing_path_;
    std::string channel_id_;
    std::string account_id_;
    bool write_blocked_ = false;
    mutable std::string last_error_;

    mutable std::mutex mutex_;
    std::vector<Record> records_;
};

}  // namespace lubancode::channel
