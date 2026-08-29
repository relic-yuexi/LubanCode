// 自进化闭环阶段 4:VersionStore——staging、version store、PackageSnapshot
// 与 canary/promote/rollback 的落盘机械。
//
// 契约(docs/features/evolution/README.md"晋升、灰度与回滚")的落地口径:
//   - 正式 Package 不原地改:版本化存放
//     ~/.lubancode/package-store/<包id>/<版本>/,active/canary 指针
//     (channels.json)选中版本;回滚只切指针,不删版本、不抹账。
//   - 晋升六步的 1-3 在这里:候选复制到 staging(包目录下 .staging/<版本>)
//     -> 复算哈希(与批准绑定的对不上即停晋升)-> 再过一遍静态门 ->
//     原子 rename 落入 version store。staging 写一半失败,正式 store 不变。
//   - 会话装配把 store 选中版本折成 PackageSnapshot(路径+期望哈希+现算
//     哈希);store 内文件被手改,哈希对不上即拒挂并指路。
//   - 首版灰度朴素:canary 只在用户点名(/evolve use)时启用,不做流量
//     路由;新会话拿 canary,老任务钉旧快照(会话装配一次,不热生效)。
//
// 状态迁移不在这里:候选状态机的唯一写口仍是 EvolutionCoordinator
// (coordinator.hpp),本模块只管 store 侧的文件与指针账,由它调用。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "package/inventory.hpp"  // PackageCandidate(scope=Store 的现成候选)

namespace lubancode::evolution {

// ---------------------------------------------------------------------------
// channels.json(schema 1)——一只包在 store 里的当前账
// ---------------------------------------------------------------------------

// 已装版本账(versions 表的一行):回滚可指、装配可验的依据。
struct StoreVersionInfo {
    std::string version;
    std::string content_hash;  // install 时复算落账的整包哈希
    std::string candidate_id;  // 从哪只候选装的
    std::string installed_at;  // ISO 8601
};

// active/canary 指针:选中哪一枚版本。
struct StoreChannelPointer {
    std::string version;
    std::string content_hash;
    std::string candidate_id;
    std::string set_at;              // ISO 8601
    std::string via;                 // canary / promote / rollback
};

struct StoreChannels {
    int schema = 1;
    std::string package_id;
    std::map<std::string, StoreVersionInfo> versions;  // 已装版本,键 = 版本号
    std::optional<StoreChannelPointer> active;
    std::optional<StoreChannelPointer> canary;
};

std::string SerializeStoreChannels(const StoreChannels& channels);
std::optional<StoreChannels> ParseStoreChannels(const std::string& text);

// ---------------------------------------------------------------------------
// install-log.jsonl(行 schema 1,只追加)——store 侧的事件账
// ---------------------------------------------------------------------------

struct StoreLogEvent {
    int schema = 1;
    std::int64_t seq = 0;
    std::string package_id;
    std::string event;         // install / canary / promote / rollback
    std::string version;
    std::string content_hash;
    std::string candidate_id;
    std::optional<std::string> from_version;  // promote/rollback:原先那枚
    std::optional<std::string> from_channel;  // active / canary
    std::string reason;
    std::string at;  // ISO 8601
};

std::string SerializeStoreLogEvent(const StoreLogEvent& event);
std::optional<StoreLogEvent> ParseStoreLogEvent(const std::string& line);
std::vector<StoreLogEvent> LoadStoreLog(const std::filesystem::path& log_file);

// ---------------------------------------------------------------------------
// VersionStore
// ---------------------------------------------------------------------------

class VersionStore {
public:
    // root 即 package-store/ 目录(<home>/.lubancode/package-store)。
    explicit VersionStore(std::filesystem::path root) : root_(std::move(root)) {}

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path PackageDir(const std::string& package_id) const;
    std::filesystem::path VersionDir(const std::string& package_id,
                                     const std::string& version) const;

    struct InstallOutcome {
        std::string package_id;
        std::string version;
        std::string content_hash;
        std::filesystem::path version_dir;
        bool already_present = false;  // 同版本同哈希已在(幂等重批)
    };

    // 晋升六步的 1-3:staging 复制 -> 复算哈希 -> 静态门 -> 原子落。
    // package_dir 是候选的 package/ 目录;expected_hash 是批准绑定的整包
    // 哈希,staging 复算对不上即停晋升(文件变过,旧批准作废)。版本号取
    // package.yaml 的 version(候选瞄准的稳定版号)。
    std::expected<InstallOutcome, std::string> Install(const std::filesystem::path& package_dir,
                                                       const std::string& candidate_id,
                                                       const std::string& expected_hash);

    // store 里有哪些包(有 channels.json 的目录)。
    std::vector<std::string> ListPackages() const;
    std::optional<StoreChannels> LoadChannels(const std::string& package_id) const;

    // 指针切换(事件只追加进 install-log.jsonl;版本一枚不删):
    //   SetCanary:   canary 指到已装版本(点名启用)。
    //   PromoteToActive: active <- canary,canary 清空(晋升)。
    //   RollbackTo:  active <- 指定版本(或 version 空 = 撤下,无父可回),
    //                canary 一并清空;返回新的 active 指针(撤下给 nullopt)。
    std::expected<StoreChannelPointer, std::string> SetCanary(const std::string& package_id,
                                                              const std::string& version);
    std::expected<StoreChannelPointer, std::string> PromoteToActive(const std::string& package_id);
    std::expected<std::optional<StoreChannelPointer>, std::string> RollbackTo(
        const std::string& package_id, const std::string& version, const std::string& reason);

    // ---------------------------------------------------------------------------
    // PackageSnapshot:装配折算。选中版本(canary 遮 active)折成
    // "路径 + 期望哈希 + 现算哈希";现算对不上即拒挂(intact=false,note 指路)。
    // ---------------------------------------------------------------------------

    struct SnapshotEntry {
        std::string package_id;
        std::string version;
        std::string channel;  // active / canary
        std::filesystem::path package_root;
        std::string expected_hash;  // install 账里记的
        std::string actual_hash;    // 装配时现算;读不动为空
        bool intact = false;
        std::string note;  // 对不上时的人话(指路)
    };

    struct Snapshot {
        std::vector<SnapshotEntry> entries;   // 每包一枚:canary 遮 active
        std::vector<SnapshotEntry> rejected;  // 哈希对不上被拒挂的(指路)
        bool empty() const { return entries.empty() && rejected.empty(); }
        const SnapshotEntry* Find(const std::string& package_id) const;
    };

    Snapshot BuildSnapshot() const;

    // /package list 与会话装配用:选中版本折成现成候选(scope=Store,
    // manifest 就地解析)。被拒挂的(tamper)也在内——list 是发现账,发现
    // 不等于挂载;挂载侧只收 intact 的。
    std::vector<lubancode::package::PackageCandidate> ScanSelectedCandidates() const;

private:
    std::expected<StoreChannels, std::string> LoadOrInitChannels(const std::string& package_id);
    bool WriteChannelsAtomic(const std::filesystem::path& package_dir,
                             const StoreChannels& channels);
    void AppendLog(const std::filesystem::path& package_dir, StoreLogEvent event);

    std::filesystem::path root_;
};

}  // namespace lubancode::evolution
