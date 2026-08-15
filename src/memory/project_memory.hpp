// 项目级长期记忆。正文用小块 Markdown 存，catalog/index 都能从正文头部
// 的严格 JSON 元数据重建。检索只读本地文件；写入先排 job，再由隐藏 worker
// 进程落盘。

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::memory {

struct Options {
    // 授权分两层(规格"授权与本场状态分开"):
    //   global_allowed  用户全局配置授予的能力,构造后运行时不可翻开;
    //   enabled         本场总开关,只能在 global_allowed 范围内开关。
    // use/generate 仍是本场子开关。项目配置只能收窄不能扩权,这条在
    // config merge 层守过一次,这里再守一次。
    bool global_allowed = false;
    bool enabled = false;
    bool use = true;
    bool generate = true;
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

struct RuntimeStatus {
    bool global_allowed = false;
    bool enabled = false;
    bool use = false;
    bool generate = false;
    std::string project_key;
    std::filesystem::path memory_dir;
    std::size_t entry_count = 0;
    std::size_t pending_jobs = 0;
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
    bool use_enabled() const { return options_.global_allowed && options_.enabled && options_.use; }
    bool generate_enabled() const {
        return options_.global_allowed && options_.enabled && options_.generate;
    }
    // 本场总开关。global_allowed 为假时,传 true 只会拿到错误(闸在运行
    // 对象上再守一道),传 false 照常允许收窄。
    std::expected<void, std::string> set_enabled(bool enabled);
    void set_use(bool enabled) { options_.use = enabled; }
    void set_generate(bool enabled) { options_.generate = enabled; }
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

    ProjectIdentity identity_;
    std::filesystem::path home_lubancode_;
    std::filesystem::path memory_dir_;
    Options options_;
    std::string executable_;
    std::string source_session_;
};

// 隐藏 CLI 子命令调用。串行捞 pending/*.json；成功删 job，坏 job 挪 failed。
std::expected<std::size_t, std::string> RunPendingMemoryJobs(
    const std::filesystem::path& home_lubancode);

// 测试与 /memory rebuild 共用的同步底层。不起进程。
std::expected<void, std::string> RebuildMemoryIndex(const std::filesystem::path& memory_dir);

}  // namespace lubancode::memory
