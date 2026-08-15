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

#include <nlohmann/json.hpp>

namespace lubancode::memory {

// 最低召回门槛:硬命中(路径 12/关键词 8/标题 5)一次即过线;纯 BM25
// 软分要两三个稀有词项才凑得满。单个常见中文双字片段过不了这条线。
constexpr int kMemoryMinRecallScore = 8;

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

// 主题范围(schema 2):project 全项目;subtree/path 限子树或单文件,当前
// cwd 不在范围内时不注入(该用才用)。跨项目/全局经验本期不做——键位
// 预留:以后加 "global" 时只认全局配置授权,存储键须另行分账,别混进
// project key 这套目录。
struct MemoryScope {
    std::string kind = "project";  // project | subtree | path
    std::string value;             // kind != project 时必填,项目内相对路径
};

struct MemoryEvidence {
    std::string path;    // 项目内相对路径
    std::string symbol;  // 可选:函数/类/配置键
};

struct SaveRequest {
    MemoryKind kind = MemoryKind::Fact;
    std::string id;
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string source_session;
    // schema 2 新增:范围、证据、置信度与寿命。
    std::string confidence;               // user-stated | verified | inferred
    MemoryScope scope;
    std::vector<MemoryEvidence> evidence;
    std::string expires_at;               // 空 = 永不过期;ISO 日期或日期时间
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
    // schema 2 新增;schema 1 旧主题读入时填缺省值(confidence 按 kind
    // 推定,scope=project),照常可读、可列、可召回、可 rebuild。
    std::string confidence;
    MemoryScope scope;
    std::vector<MemoryEvidence> evidence;
    std::string last_verified_at;
    std::string expires_at;  // 空 = 永不过期
};

// 检索排级的纯函数结果(评测集与 /memory why 共用)。injected 只是"过
// 门槛值得注入",预算裁剪另算。
struct ScoredEntry {
    const MemoryEntry* entry = nullptr;
    int hard_hits = 0;     // 稳定实体(路径/关键词/symbol/标题/id)硬命中次数
    int token_hits = 0;    // 分词后命中的有效词项数(虚词碎片不计)
    double bm25 = 0.0;     // 本地 BM25 软分(词项按来源权重折算)
    int score = 0;         // 硬命中分 + cwd 排位加分 + BM25 折算分(排序用)
    bool qualifies = false;       // 过最低门槛,值得注入(调用方据此判)
    bool stale_blocked = false;   // 指纹漂移,只提示不注正文(由调用方判)
    bool expired = false;         // 已过 expires_at,不召回
    bool scope_blocked = false;   // scope 不符当前 cwd,不注入
};

// 检索词的来源、词路与权重(trace 报账用):source 说词从哪来(query 本体
// 还是回合总结的扩展词),kind 说走哪条词路(word=整词/标识符/词典实体,
// gram=中文二元片段),weight 是进 BM25 与门槛判定的乘子——带虚词字符的
// 句式碎片拿 kWeakGramWeight,凑不了门槛,也拉不动分数。
struct TraceTerm {
    std::string text;
    std::string source = "query";  // query | hint
    std::string kind = "gram";     // word | gram
    double weight = 0.8;
};

// 归一化(查询与索引共用):NFKC 常用子集(全角 ASCII/全角空格/弯引号/
// 长划/连字/半角片假名)、ASCII 小写、路径分隔符统一正斜杠。标点经这套
// 之后要么归半角要么当分隔符,不再黏进中文二元词。
std::string NormalizeForRetrieval(const std::string& text);

// 标识符拆分 + 中英混排分词:camelCase/snake_case/kebab/路径段都拆
// (BuildTurnContext -> build/turn/context),中文出双字片段;查询与
// 索引共用同一套。
std::vector<std::string> TokenizeForRetrieval(const std::string& text);

// 纯函数排级:硬命中(路径/关键词/symbol/标题/id 稳定实体,词边界匹配)
// + BM25 软分(词项带权重)。返回有得分的条目(含没过门槛的,给 trace
// 用),qualifies 标记是否过最低门槛(至少一次硬命中,或带整词的两次
// 词项命中/三次纯二元命中,且核心分过线)。traced_terms 回填本轮查询词
// 项(含来源与权重),供 trace 落盘。archived/conflict 不参与;过期/
// scope 越区打标记由调用方拦。指纹漂移要摸项目文件,也由调用方判。
std::vector<ScoredEntry> RankEntries(const std::vector<MemoryEntry>& entries, const std::string& query,
                                     const std::string& cwd_relative,
                                     const std::vector<std::string>& hints = {},
                                     std::vector<TraceTerm>* traced_terms = nullptr);

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

// 查询来源(规格"合成事件隔离"):只有 user 的提问(与显式要求事实的
// main 回流)才跑检索;后台完成唤醒、钩子、压缩续跑、系统播报这类宿主
// 合成 prompt 默认整轮跳过——不产检索词,不占检索预算,trace 只记来源。
enum class QueryOrigin { User, BackgroundCompletion, Hook, Compact, System };
std::string QueryOriginName(QueryOrigin origin);

// 一轮召回的本地 trace(规格"每次召回都说得清")。只存归一化词项(带
// 来源与权重)、id、分数与字节,不抄主题正文,也不记用户完整问题。
struct RecallTraceEntry {
    std::string id;
    int score = 0;
    int hard_hits = 0;    // 稳定实体(路径/关键词/symbol/标题/id)硬命中次数
    int term_hits = 0;    // 分词后命中的有效词项数(虚词碎片不计)
    bool injected = false;
    bool stale_blocked = false;   // 指纹漂移,只提示不注正文
    bool below_threshold = false; // 分数没过最低门槛
    bool budget_dropped = false;  // 过了门槛但预算/条数不够
    bool scope_blocked = false;   // scope 不符当前 cwd,不注入
    bool expired = false;         // 已过 expires_at,不召回
    bool duplicate_dropped = false;  // 同一事实/相同证据,去重让位
    std::size_t bytes = 0;
};

struct RecallTrace {
    bool valid = false;
    std::string at;
    std::string project_key;
    std::string query_origin = "user";  // user | background_completion | hook | compact | system
    bool skipped = false;               // 合成控制消息:本轮没跑检索
    std::vector<TraceTerm> terms;
    std::vector<RecallTraceEntry> entries;  // 计分过的候选,含被拦与落选
    std::size_t injected_count = 0;
    std::size_t injected_bytes = 0;  // 去重后有效字节
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
    // 命中时零注入零脚手架(规格"零命中不塞空脚手架")。origin 记这条
    // 查询从哪来:user 才跑检索,合成控制消息(后台完成唤醒等)默认整轮
    // 跳过,只留 trace 来源;确需事实的合成回流可传 force_retrieval。
    std::string BuildTurnContext(const std::string& query, const std::filesystem::path& cwd,
                                 QueryOrigin origin = QueryOrigin::User,
                                 bool force_retrieval = false) const;

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
    // 核验:原 id 复活——重算指纹、盖 last_verified_at、status 回 active。
    // refresh=true 时连 status 一并回炉(verify 只盖时间戳)。
    std::expected<std::string, std::string> EnqueueVerify(const std::string& id, bool refresh);

    // 陈旧清单:指纹漂移的与已过期的,附原因(/memory stale 用)。
    struct StaleEntry {
        MemoryEntry entry;
        std::string reason;  // "fingerprint" | "expired"
    };
    std::vector<StaleEntry> ListStaleEntries() const;

    std::vector<MemoryEntry> ListEntries(std::string* error = nullptr) const;
    RuntimeStatus Status() const;

    // 有 pending job 时起一枚会话级后台 worker。失败不删 job，下次还能捞。
    std::expected<void, std::string> LaunchWorker() const;

private:
    std::expected<std::string, std::string> EnqueueJob(const std::string& operation,
                                                       const SaveRequest* request,
                                                       const std::string& id,
                                                       nlohmann::json extra = nlohmann::json::object());
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
