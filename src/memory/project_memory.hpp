// 项目级长期记忆。正文用小块 Markdown 存，catalog/index 都能从正文头部
// 的严格 JSON 元数据重建。检索只读本地文件；写入先排 job，再由隐藏 worker
// 进程落盘。

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::memory {

// 学习三档(规格"学习改成三档"):off 不提候选不写入;review 自动提候选,
// 用户审过才入库(建议默认);auto 自动写入,须用户在全局配置显式授权。
// learn_ceiling 是配置层(config merge)算好的上限:本场 set_learn 只能
// 在 ceiling 范围内降,不能升——auto 这道授权只认全局配置。
enum class LearnMode { Off = 0, Review = 1, Auto = 2 };

std::string LearnModeName(LearnMode mode);
std::expected<LearnMode, std::string> ParseLearnMode(const std::string& raw);

struct Options {
    // 授权分两层(规格"授权与本场状态分开"):
    //   global_allowed  用户全局配置授予的能力,构造后运行时不可翻开;
    //   enabled         本场总开关,只能在 global_allowed 范围内开关。
    // use 是本场召回子开关;learn 是本场学习档位。项目配置只能收窄不能
    // 扩权,这条在 config merge 层守过一次,这里再守一次。
    bool global_allowed = false;
    bool enabled = false;
    bool use = true;
    LearnMode learn = LearnMode::Review;
    LearnMode learn_ceiling = LearnMode::Review;
    std::size_t max_index_bytes = 16 * 1024;      // index.md 留给人看,不再进 prompt
    std::size_t max_retrieval_bytes = 8 * 1024;   // 每轮注入正文总预算
    std::size_t max_results = 3;                  // 每轮最多注入条数
};

struct ProjectIdentity {
    std::filesystem::path project_root;
    std::filesystem::path common_root;
    std::filesystem::path project_dir;
    std::string key;
    std::string display_name;
    bool git = false;
};

// Git 项目以规范化后的 common git dir 为身份，故同仓库 worktree 共用 key；
// 非 Git 项目向上找最近的 .lubancode/config.json，找不到就用 cwd。
std::expected<ProjectIdentity, std::string> ResolveProjectIdentity(
    const std::filesystem::path& cwd, const std::filesystem::path& home_lubancode);

enum class MemoryKind { Fact, Preference };

std::string MemoryKindName(MemoryKind kind);
std::expected<MemoryKind, std::string> ParseMemoryKind(const std::string& raw);

struct SaveRequest {
    MemoryKind kind = MemoryKind::Fact;
    std::string id;
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string source_session;
};

struct MemoryEntry {
    std::string id;
    MemoryKind kind = MemoryKind::Fact;
    std::string title;
    std::string summary;
    std::string file;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string status = "active";
    std::string updated_at;
    std::vector<std::string> source_sessions;
};

// 待审候选(规格"候选审阅箱")。回合收尾抽取产出,先住待审区,用户
// /memory accept|edit|reject 之后才动正式库。按项目 key 分账,耐退出。
struct MemoryCandidate {
    std::string id;      // cand-<hex>
    MemoryKind kind = MemoryKind::Fact;
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string confidence;   // user-stated | verified | inferred
    std::string task_type;    // code | research | config | docs | other
    std::string created_at;
};

struct RuntimeStatus {
    bool global_allowed = false;
    bool enabled = false;
    bool use = false;
    bool generate = false;
    std::string learn;   // off | review | auto(本场档位)
    std::string project_key;
    std::filesystem::path memory_dir;
    std::size_t entry_count = 0;
    std::size_t pending_jobs = 0;
    std::size_t pending_candidates = 0;
};

// 一轮召回的本地 trace(规格"每次召回都说得清")。只存归一化词项、id、
// 分数与字节,不抄主题正文,也不记用户完整问题。
struct RecallTraceEntry {
    std::string id;
    int score = 0;
    int hard_hits = 0;    // 路径/关键词/标题硬命中次数
    int term_hits = 0;    // 词法词项命中次数
    bool injected = false;
    bool stale_blocked = false;   // 指纹漂移,只提示不注正文
    bool below_threshold = false; // 分数没过最低门槛
    bool budget_dropped = false;  // 过了门槛但预算/条数不够
    std::size_t bytes = 0;
};

struct RecallTrace {
    bool valid = false;
    std::string at;
    std::string project_key;
    std::vector<std::string> terms;
    std::vector<RecallTraceEntry> entries;  // 计分过的候选,含被拦与落选
    std::size_t injected_count = 0;
    std::size_t injected_bytes = 0;
};

class ProjectMemory {
public:
    ProjectMemory(ProjectIdentity identity, std::filesystem::path home_lubancode,
                  Options options, std::string executable = std::string());

    bool global_allowed() const { return options_.global_allowed; }
    bool enabled() const { return options_.enabled; }
    LearnMode learn_mode() const { return options_.learn; }
    LearnMode learn_ceiling() const { return options_.learn_ceiling; }
    bool use_enabled() const { return options_.global_allowed && options_.enabled && options_.use; }
    // 写入能力 = 学习档位不在 off(提候选与 memory_save 都算"写")。
    bool generate_enabled() const {
        return options_.global_allowed && options_.enabled && options_.learn != LearnMode::Off;
    }
    // 本场总开关。global_allowed 为假时,传 true 只会拿到错误(闸在运行
    // 对象上再守一道),传 false 照常允许收窄。
    std::expected<void, std::string> set_enabled(bool enabled);
    void set_use(bool enabled) { options_.use = enabled; }
    // 本场学习档位。只能降到 learn_ceiling 以内;越界(典型:全局只授了
    // review 却想开 auto)返回错误。
    std::expected<void, std::string> set_learn(LearnMode mode);
    void set_source_session(std::string id) { source_session_ = std::move(id); }

    const ProjectIdentity& identity() const { return identity_; }
    const std::filesystem::path& memory_dir() const { return memory_dir_; }

    std::expected<void, std::string> SetWorkingDirectory(const std::filesystem::path& cwd);

    // 每条外层用户消息调用一次。返回空串表示本轮无记忆段;没有过门槛的
    // 命中时只保留极短能力说明,不再注入整份 index.md。
    std::string BuildTurnContext(const std::string& query, const std::filesystem::path& cwd) const;

    // 上一轮召回的 trace(读 .state/trace-last.json)。没有记录时
    // valid=false。/memory why 用。
    RecallTrace LastTrace() const;

    // ---- 候选审阅箱 ----
    // 待审候选列表,按创建时间升序。
    std::vector<MemoryCandidate> ListCandidates() const;
    std::optional<MemoryCandidate> GetCandidate(const std::string& id) const;
    // 新候选入库(待审区)。做过长度/敏感内容/路径校验;kind+标题 查重,
    // 与现有待审候选同主题时原位更新;此前被拒过的同主题候选直接拒收
    // (短哈希账本挡死缠烂打)。返回候选 id;被挡时返回错误。
    std::expected<std::string, std::string> AddCandidate(MemoryCandidate candidate);
    // 接受:候选转正式 upsert job(同 id 语义沿用),候选文件删除。
    std::expected<std::string, std::string> AcceptCandidate(const std::string& id);
    // 改标题/正文后仍留待审区。
    std::expected<void, std::string> EditCandidate(const std::string& id, const std::string& title,
                                                   const std::string& content);
    // 拒绝:删候选,只留短哈希与理由进 rejected 账本,不存被拒正文。
    std::expected<void, std::string> RejectCandidate(const std::string& id, std::string reason);

    // ---- 回合总结顺手产出的检索扩展词(下一轮 BM25/词法查询合并用) ----
    // 不许为此额外打模型请求(用户基调 3):learn off 或抽取失败时不清空
    // 旧值,查询自然退回纯词法。
    void SetRetrievalHints(std::vector<std::string> hints);

    std::expected<std::string, std::string> EnqueueSave(const SaveRequest& request);
    std::expected<std::string, std::string> EnqueueForget(const std::string& id);
    std::expected<std::string, std::string> EnqueueRebuild();

    std::vector<MemoryEntry> ListEntries(std::string* error = nullptr) const;
    RuntimeStatus Status() const;

    // 有 pending job 时起一枚会话级后台 worker。失败不删 job，下次还能捞。
    std::expected<void, std::string> LaunchWorker() const;

private:
    std::expected<std::string, std::string> EnqueueJob(const std::string& operation,
                                                       const SaveRequest* request,
                                                       const std::string& id);
    std::filesystem::path CandidatesDir() const;

    ProjectIdentity identity_;
    std::filesystem::path home_lubancode_;
    std::filesystem::path memory_dir_;
    Options options_;
    std::string executable_;
    std::string source_session_;
    // 回合总结产出的检索扩展词(下一轮 BuildTurnContext 合并进查询)。
    std::vector<std::string> retrieval_hints_;
};

// 隐藏 CLI 子命令调用。串行捞 pending/*.json；成功删 job，坏 job 挪 failed。
std::expected<std::size_t, std::string> RunPendingMemoryJobs(
    const std::filesystem::path& home_lubancode);

// 测试与 /memory rebuild 共用的同步底层。不起进程。
std::expected<void, std::string> RebuildMemoryIndex(const std::filesystem::path& memory_dir);

}  // namespace lubancode::memory
