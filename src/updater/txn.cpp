// txn.hpp 的实现。语义对齐 scripts/updater.py 的 Transaction/
// list_transactions/cleanup_staging/find_resumable(文件头注释见 txn.hpp)。
#include "updater/txn.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "updater/manifest.hpp"

namespace lubancode::updater {

namespace {

std::string CallNow(const UtcNowFn& now) {
    return now ? now() : UtcNowIso8601();
}

void AtomicWriteOrThrow(const std::filesystem::path& target, const std::string& bytes) {
    const auto result =
        platform::AtomicWriteFile(target, bytes, platform::WriteDurability::ProcessCrashDurability);
    if (!result.has_value()) {
        throw std::runtime_error("事务账落盘失败 " + platform::PathToUtf8(target) + ": " +
                                 result.error().code + " " + result.error().message);
    }
}

// 事务 id 的时间戳段:UTC "YYYYmmddTHHMMSSZ"(python new_txn_id 的
// strftime("%Y%m%dT%H%M%SZ"))。
std::string TxnTimestampNow() {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

}  // namespace

bool IsTxnTerminalForResume(std::string_view state) {
    return state == kTxnFailed || state == kTxnRolledBack || state == kTxnCommitted;
}

nlohmann::json TxnTarget::ToJson() const {
    nlohmann::json target = nlohmann::json::object();
    target["version"] = version;
    target["tag"] = tag;
    target["exe_version"] = exe_version;
    target["platform"] = platform;
    target["dirname"] = dirname;
    target["digest_hex"] = digest_hex;
    target["repo"] = repo.has_value() ? nlohmann::json(*repo) : nlohmann::json();
    target["release_id"] = release_id.has_value() ? nlohmann::json(*release_id) : nlohmann::json();
    target["asset_id"] = asset_id.has_value() ? nlohmann::json(*asset_id) : nlohmann::json();
    target["asset_name"] = asset_name.has_value() ? nlohmann::json(*asset_name) : nlohmann::json();
    target["asset_size"] = asset_size.has_value() ? nlohmann::json(*asset_size) : nlohmann::json();
    target["download_url"] =
        download_url.has_value() ? nlohmann::json(*download_url) : nlohmann::json();
    return target;
}

std::string MakeTxnId() {
    // 8 位小写十六进制随机段(python os.urandom(4).hex())。random_device
    // 在个别工具链上有确定性风险,掺墙钟与 pid 搅匀——id 只求不撞脸,
    // 不担密码学义务(事务唯一性由时间戳段+安装锁共同保证)。
    std::random_device device;
    const std::uint32_t entropy = device() ^
                                  static_cast<std::uint32_t>(platform::WallClockNowMs()) ^
                                  (static_cast<std::uint32_t>(platform::CurrentProcessId()) * 0x9E3779B9u);
    char tail[9];
    std::snprintf(tail, sizeof(tail), "%08x", entropy);
    return TxnTimestampNow() + "-" + tail;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(LayoutPaths paths, std::string txn_id, UtcNowFn now)
    : paths_(std::move(paths)), id_(std::move(txn_id)), now_(std::move(now)) {}

std::optional<Transaction> Transaction::OpenExisting(const std::filesystem::path& ledger_file,
                                                     UtcNowFn now) {
    const auto data = ReadJsonFileTolerant(ledger_file);
    if (!data.has_value() || !data->is_object()) return std::nullopt;
    // 只认有 id 的账(python open_existing:"id" not in data -> None)。
    if (!data->contains("id") || !(*data)["id"].is_string()) return std::nullopt;
    Transaction txn(MakeLayoutPaths(ledger_file.parent_path().parent_path()),
                    (*data)["id"].get<std::string>(), std::move(now));
    txn.data_ = std::move(*data);
    return txn;
}

void Transaction::Create(const TxnTarget& target) {
    // python create:先确保 updates/ 在(AtomicWriteFile 建父目录,同效)。
    data_ = nlohmann::json::object();
    data_["schema"] = 1;
    data_["id"] = id_;
    data_["kind"] = "update";
    data_["created_at_utc"] = CallNow(now_);
    data_["state"] = std::string(kTxnChecking);
    data_["target_version"] = target.version;
    data_["target_dirname"] = target.dirname;
    data_["target_digest"] = target.digest_hex;
    data_["target"] = target.ToJson();
    Flush();
}

void Transaction::Transition(std::string_view state, nlohmann::json fields) {
    if (!fields.is_object() && !fields.is_null()) {
        throw std::runtime_error("transition 的追加字段必须是 JSON object");
    }
    data_["state"] = std::string(state);
    data_["state_at_utc"] = CallNow(now_);
    if (fields.is_object()) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            data_[it.key()] = std::move(*it);
        }
    }
    Flush();
}

void Transaction::Note(std::string_view text) {
    if (!data_.contains("notes") || !data_["notes"].is_array()) {
        data_["notes"] = nlohmann::json::array();
    }
    data_["notes"].push_back(CallNow(now_) + " " + std::string(text));
    Flush();
}

void Transaction::Flush() {
    if (id_.empty()) {
        throw std::runtime_error("空账不能落盘(先 Create 或 OpenExisting)");
    }
    AtomicWriteOrThrow(LedgerPath(), CanonicalJsonDump(data_));
}

std::string Transaction::state() const {
    if (!data_.contains("state") || !data_["state"].is_string()) return std::string();
    return data_["state"].get<std::string>();
}

nlohmann::json& Transaction::data() { return data_; }
const nlohmann::json& Transaction::data() const { return data_; }

std::filesystem::path Transaction::LedgerPath() const { return paths_.updates / (id_ + ".json"); }
std::filesystem::path Transaction::StageDir() const { return paths_.staging / id_; }
std::filesystem::path Transaction::ArchivePath() const { return StageDir() / "archive.bin"; }

// ---------------------------------------------------------------------------
// 清单与续跑裁决
// ---------------------------------------------------------------------------

std::vector<Transaction> ListTransactions(const LayoutPaths& paths, UtcNowFn now) {
    std::vector<Transaction> txns;
    std::error_code ec;
    if (!std::filesystem::is_directory(paths.updates, ec) || ec) return txns;
    // 按文件名排序(python sorted(os.listdir)),裁决顺序才稳定。
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(paths.updates, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        files.push_back(entry.path());
    }
    if (ec) return txns;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.filename().generic_string() < b.filename().generic_string();
    });
    for (const auto& file : files) {
        auto txn = Transaction::OpenExisting(file, now);
        if (txn.has_value()) txns.push_back(std::move(*txn));
    }
    return txns;
}

std::string CleanupStaging(const LayoutPaths& paths, std::string_view txn_id) {
    const std::filesystem::path stage = paths.staging / std::string(txn_id);
    std::error_code ec;
    if (!std::filesystem::is_directory(stage, ec) || ec) return std::string();
    std::filesystem::remove_all(stage, ec);
    if (ec) {
        return "staging 清不掉(" + platform::PathToUtf8(stage) + "): " + ec.message();
    }
    return std::string();
}

ResumableDecision FindResumable(const LayoutPaths& paths, const std::string& target_digest_hex,
                                UtcNowFn now) {
    ResumableDecision decision;
    for (Transaction& txn : ListTransactions(paths, now)) {
        if (!txn.data().contains("kind") || !txn.data()["kind"].is_string() ||
            txn.data()["kind"].get<std::string>() != "update") {
            continue;
        }
        if (IsTxnTerminalForResume(txn.state())) continue;
        std::string digest;
        if (txn.data().contains("target_digest") && txn.data()["target_digest"].is_string()) {
            digest = txn.data()["target_digest"].get<std::string>();
        }
        if (digest != target_digest_hex) {
            // 异目标的在途事务作废清场(字段与文案逐字照 python)。
            nlohmann::json fields = nlohmann::json::object();
            fields["reason"] = "superseded";
            fields["detail"] = "目标版本已换,旧事务作废";
            txn.Transition(kTxnFailed, std::move(fields));
            CleanupStaging(paths, txn.id());
            decision.superseded_ids.push_back(txn.id());
            continue;
        }
        decision.resume = std::move(txn);
        return decision;
    }
    return decision;
}

}  // namespace lubancode::updater
