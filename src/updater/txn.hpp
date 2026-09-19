// 更新助手 C++ 化·批二第③单:八相事务账(§七状态流)。
//
// 语义唯一真源 scripts/updater.py 的 Transaction(L309-404)/list_transactions
// /cleanup_staging/find_resumable:
//   - 账住在 updates/<txn_id>.json,schema 1;字段名与 python 版逐字相同:
//     id/kind/state/state_at_utc/target_version/target_dirname/target_digest/
//     target/notes/blocking/rollback_current/layout_before/health_probe/reason
//     及各步骤的追加字段(detail/archive_sha256/committed_at_utc/…)。落盘
//     CanonicalJsonDump,与 python json.dumps(ensure_ascii=True, sort_keys=True,
//     indent=2)+"\n" 逐字节等价——黄金文件对拍(tests/fixtures/updater/
//     txn_*_golden.json,由 python 版真跑落盘)钉死;
//   - 状态流 checking -> downloading -> verified -> staged -> waiting-for-idle
//     -> activating -> healthy -> committed;异常终态 failed / needs-review /
//     rolled-back。每步 transition/note 全量重写(原子,ProcessCrashDurability),
//     重启可续;
//   - txn_id = UTC YYYYmmddTHHMMSSZ + "-" + 8 位小写十六进制随机(python
//     new_txn_id 同款);
//   - find_resumable:同 digest、未终结(failed/rolled-back/committed 之外,
//     needs-review 算可续)的第一笔续跑;异目标的在途事务 transition
//     failed{reason:"superseded", detail:"目标版本已换,旧事务作废"} 并清
//     staging。注:python 版没有独立的 "superseded" 字段——superseded 是
//     reason 字段的值,本侧照抄,不另造字段。
//
// 账本故意保成 nlohmann::json(同 python 的 self.data dict):字段集随步骤
// 增长,typed 结构会把"逐字相同"的合同变成两处维护。要动账,改 data() 再
// Flush()(python 的 txn.data[...] = ...; txn.flush() 同款口)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "updater/layout.hpp"

namespace lubancode::updater {

// §七 状态流的状态名(落盘逐字,别改拼法——与 python 版互认)。
inline constexpr std::string_view kTxnChecking = "checking";
inline constexpr std::string_view kTxnDownloading = "downloading";
inline constexpr std::string_view kTxnVerified = "verified";
inline constexpr std::string_view kTxnStaged = "staged";
inline constexpr std::string_view kTxnWaitingForIdle = "waiting-for-idle";
inline constexpr std::string_view kTxnActivating = "activating";
inline constexpr std::string_view kTxnHealthy = "healthy";
inline constexpr std::string_view kTxnCommitted = "committed";
inline constexpr std::string_view kTxnFailed = "failed";
inline constexpr std::string_view kTxnNeedsReview = "needs-review";
inline constexpr std::string_view kTxnRolledBack = "rolled-back";

// 重跑另开新账的终态(find_resumable 的跳过集:failed/rolled-back/committed。
// needs-review 不在其列——冲突处理完重跑不重下)。
bool IsTxnTerminalForResume(std::string_view state);

// 更新目标(collect_target 的落盘形状):dirname 由 version + digest 前
// 8 位拼成,repo/release_id/asset_id 联网必需,download_url 本地包路恒空。
struct TxnTarget {
    std::string version;
    std::string tag;
    std::string exe_version;
    std::string platform;
    std::string dirname;
    std::string digest_hex;  // 64 位小写十六进制(裸 hex,无 "sha256:" 前缀)

    std::optional<std::string> repo;
    std::optional<std::int64_t> release_id;
    std::optional<std::int64_t> asset_id;
    std::optional<std::string> asset_name;
    std::optional<std::int64_t> asset_size;
    std::optional<std::string> download_url;

    // 与 python collect_target 的 dict 逐字段同形(缺省值落 null)。
    nlohmann::json ToJson() const;
};

// "<UTC YYYYmmddTHHMMSSZ>-<8 位小写十六进制>"(python new_txn_id 同款)。
std::string MakeTxnId();

// 一笔事务的账。默认构造是空账(未 create/未 open),用前先 Create 或
// OpenExisting;move-only 语义由成员自然获得。
class Transaction {
public:
    // 空 seam 用真钟(UtcNowIso8601);测试注固定钟钉死落盘字节。
    Transaction() = default;
    Transaction(LayoutPaths paths, std::string txn_id, UtcNowFn now = {});

    // 打开既有账:文件不在/坏 JSON/不是 object/缺 id 一律 nullopt(python
    // open_existing 同款:只认有 id 的 dict)。
    static std::optional<Transaction> OpenExisting(const std::filesystem::path& ledger_file,
                                                   UtcNowFn now = {});

    // 建账并落盘(schema 1,kind=update,state=checking;python create 同款)。
    void Create(const TxnTarget& target);

    // 换相并落盘:置 state/state_at_utc,合并 fields(同 python transition
    // 的 **fields;fields 须是 object 或空)。
    void Transition(std::string_view state, nlohmann::json fields = nlohmann::json::object());

    // 记一行注:notes 追加 "<now> <text>"(setdefault 语义:缺 notes 先建)。
    void Note(std::string_view text);

    // 全量重写账本(原子,ProcessCrashDurability)。写不进抛 std::runtime_error
    // ——账是事实记录,失败不静默。
    void Flush();

    const std::string& id() const { return id_; }
    // state 缺失给空串(python data.get("state", "") 同款)。
    std::string state() const;
    // 账本的直接读写口(改完自己 Flush)。
    nlohmann::json& data();
    const nlohmann::json& data() const;
    const LayoutPaths& paths() const { return paths_; }

    std::filesystem::path LedgerPath() const;  // updates/<id>.json
    std::filesystem::path StageDir() const;    // staging/<id>
    std::filesystem::path ArchivePath() const; // staging/<id>/archive.bin

    bool empty() const { return id_.empty(); }

private:
    LayoutPaths paths_;
    std::string id_;
    nlohmann::json data_ = nlohmann::json::object();
    UtcNowFn now_;
};

// 列出安装根的全部账(updates/*.json,按文件名排序;坏账跳过——
// python list_transactions 同款)。
std::vector<Transaction> ListTransactions(const LayoutPaths& paths, UtcNowFn now = {});

// 清掉一笔事务的 staging(目录不在则无事;删不动不抛,只如实返回错误
// 文案——python cleanup_staging 是 say 后继续)。
std::string CleanupStaging(const LayoutPaths& paths, std::string_view txn_id);

// 续跑裁决(python find_resumable):同 digest 未终结/needs-review 的第一笔
// 续跑;异目标在途事务作废(reason=superseded)并清 staging。superseded_ids
// 记被作废清场的账号(诊断/测试用)。
struct ResumableDecision {
    std::optional<Transaction> resume;
    std::vector<std::string> superseded_ids;
};

ResumableDecision FindResumable(const LayoutPaths& paths, const std::string& target_digest_hex,
                                UtcNowFn now = {});

}  // namespace lubancode::updater
