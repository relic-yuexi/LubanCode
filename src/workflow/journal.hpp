// Run Journal(自然语言编排单第 2 批):事件流、checkpoint 与恢复账。
//
// 落盘形状(单子"Run Journal、断点与恢复"):
//   <home>/.lubancode/workflow-runs/<run-id>/
//     manifest.json          workflow id/version/hash、cwd、开始时间、终态
//     definition.json        归一化定义快照(resume 找同一 hash 的锚)
//     events.jsonl           只追加事件(append+flush,半截尾行跳过)
//     checkpoints/<seq>.json 小型 Store 快照
//
// 事件带 run_id/workflow_id/node_id/attempt/seq/ts/type/data;seq 在一场
// run 内单调递增,由 journal 独家分配。写失败置 broken,后续写入空操作
// 并打一次警告,不拦运行本身(recorder 同款规矩)。
//
// 脱敏:入盘前一律过 SanitizeJournalPayload——authorization/cookie/token/
// api_key/password 一类键的值打码(单子"审批、信任与权限"末条)。

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/id_authority.hpp"
#include "workflow/definition.hpp"

namespace lubancode::workflow {

// 事件类型(稳定字符串,线上表示)。
inline constexpr const char* kEventRunStarted = "run_started";
inline constexpr const char* kEventNodeStarted = "node_started";
inline constexpr const char* kEventNodeRetrying = "node_retrying";
inline constexpr const char* kEventNodeWaiting = "node_waiting";
inline constexpr const char* kEventNodeCompleted = "node_completed";
inline constexpr const char* kEventNodeSkipped = "node_skipped";
inline constexpr const char* kEventBranchStarted = "branch_started";
inline constexpr const char* kEventJoinCompleted = "join_completed";
inline constexpr const char* kEventCheckpointSaved = "checkpoint_saved";
inline constexpr const char* kEventRunStateChanged = "run_state_changed";
inline constexpr const char* kEventRunCompleted = "run_completed";

struct JournalEvent {
    std::uint64_t seq = 0;
    std::int64_t ts_ms = 0;
    std::string run_id;
    std::string workflow_id;
    std::string node_id;   // 可空(run 层事件)
    int attempt = 0;
    std::string type;      // kEvent* 之一
    nlohmann::json data = nlohmann::json::object();
};

// 事件 -> 一行 JSON(不带换行)。
std::string SerializeJournalEvent(const JournalEvent& event);
// 一行 -> 事件;坏行/半截行给 nullopt(跳过,不废整场)。
std::optional<JournalEvent> ParseJournalEvent(const std::string& line);

// journal 载荷脱敏(纯函数):键名含 token/secret/password/passwd/
// authorization/cookie/api_key 的字段值换 "[已打码]";字符串值再过一遍
// 文本打码。与 skills::SanitizeToolInput 同规矩,不 include agent/*,两
// 边各维护,单测钉一致。
nlohmann::json SanitizeJournalPayload(const nlohmann::json& payload);
std::string RedactJournalText(const std::string& text);

// 时钟与 id 的注入口(单测喂 fake):默认走墙钟。
struct JournalClock {
    virtual ~JournalClock() = default;
    virtual std::int64_t NowMs() const;
};

// 落盘句柄。Start 建 run 目录、写 manifest 与 definition 快照;此后每条
// 事件 append+flush。Checkpoint 单独落 checkpoints/<seq>.json(先写
// 临时件再原子 rename,单子"恢复不会捡到半份结果")。
class RunJournal {
public:
    struct StartInfo {
        std::string run_id;
        std::string workflow_id;
        std::string workflow_version;
        std::string content_hash;
        std::string cwd;
        std::string definition_json;  // 归一化定义快照文本

        nlohmann::json ToManifestJson() const {
            nlohmann::ordered_json j = nlohmann::ordered_json::object();
            j["run_id"] = run_id;
            j["workflow_id"] = workflow_id;
            j["workflow_version"] = workflow_version;
            j["content_hash"] = content_hash;
            j["cwd"] = cwd;
            j["started_at"] = "";
            return j;
        }
    };

    // 开一场新 run。runs_root 由调用方算好(<home>/.lubancode/workflow-runs)。
    static std::expected<RunJournal, std::string> Start(const std::filesystem::path& runs_root,
                                                        const StartInfo& info, const JournalClock* clock = nullptr);

    // 追加一条事件(seq 自动分配,ts 走 clock)。data 先过 Sanitize。
    void Append(const std::string& type, const std::string& node_id, int attempt, nlohmann::json data);

    // Store 快照落盘(原子):checkpoints/<seq>.json。失败打警告,不拦运行。
    void SaveCheckpoint(std::uint64_t at_seq, const nlohmann::json& store_json);

    // 终态落 manifest(成功/失败/取消/预算耗尽)。
    void Finish(const std::string& final_state, const nlohmann::json& summary);

    std::uint64_t last_seq() const { return seq_ids_ ? seq_ids_->seq_issued() : 0; }
    const std::filesystem::path& dir() const { return dir_; }
    const std::string& run_id() const { return run_id_; }
    bool broken() const { return broken_; }

    RunJournal() = default;
    RunJournal(RunJournal&& other) noexcept;
    RunJournal& operator=(RunJournal&& other) noexcept;
    RunJournal(const RunJournal&) = delete;
    RunJournal& operator=(const RunJournal&) = delete;
    ~RunJournal();

private:
    RunJournal(std::filesystem::path dir, std::string run_id, std::ofstream out, const JournalClock* clock,
               nlohmann::json start_manifest);
    void WriteManifest(const std::string& final_state, const nlohmann::json& summary);

    std::filesystem::path dir_;
    std::string run_id_;
    std::ofstream out_;
    const JournalClock* clock_ = nullptr;
    nlohmann::json start_manifest_ = nlohmann::json();
    // 事件 seq 的发号局(批五):一场 run 一只实例(1 起,run 内单调);
    // unique_ptr 是为了保 RunJournal 的 move(IdAuthority 带 mutex 不可移)。
    std::unique_ptr<runtime::IdAuthority> seq_ids_;
    bool broken_ = false;
    bool finish_called_ = false;
};

// ---------------------------------------------------------------------------
// 恢复
// ---------------------------------------------------------------------------

// 一场 run 的盘点。
struct RunStatus {
    std::string run_id;
    std::string workflow_id;
    std::string workflow_version;
    std::string content_hash;
    std::string cwd;
    std::string started_at;
    std::string final_state;    // 空 = 没跑完(manifest 未记终态)
    nlohmann::json definition;  // 归一化快照
    std::filesystem::path dir;
};

// 扫 runs_root,按 run_id 倒序(时间倒序)。
std::vector<RunStatus> ListRuns(const std::filesystem::path& runs_root);

// 读整场事件(坏行跳过)。
std::vector<JournalEvent> ReadJournalEvents(const std::filesystem::path& run_dir);

// 最新一份 checkpoint 的 Store JSON;没有 checkpoint 给 nullopt。
std::optional<nlohmann::json> ReadLatestCheckpoint(const std::filesystem::path& run_dir);

// 从事件流推节点账:node_id -> {output, meta, state}。恢复的锚:凡
// node_completed 的节点不再重跑(单子:恢复不会捡到半份结果;副作用
// 节点查幂等记录或问用户)。
struct ReplayedNode {
    std::string state;                 // succeeded/skipped
    nlohmann::json output = nlohmann::json::object();
    nlohmann::json meta = nlohmann::json::object();
};
std::map<std::string, ReplayedNode> ReplayNodes(const std::vector<JournalEvent>& events);

}  // namespace lubancode::workflow
