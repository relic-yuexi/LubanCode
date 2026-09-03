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

// 最低召回门槛:稳定实体硬命中(路径 12/关键词与 symbol 8/标题 5/id 6)
// 一次即过线;纯 BM25 软分要两个不同词组的稀有词项才凑得满。单个常见
// 中文双字片段过不了这条线,带虚词字符的句式碎片更是连词组都算不上。
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
    // 用户级记忆(住 <主目录>/memory/user/,跨项目偏好与反馈):另设一道
    // 全局授权,项目配置无权开启或写入。召回时两层各查、同 id/同证据去
    // 重、项目层压过用户层。
    bool user_enabled = false;
    LearnMode learn = LearnMode::Review;
    LearnMode learn_ceiling = LearnMode::Review;
    std::size_t max_index_bytes = 16 * 1024;      // index.md 留给人看,不再进 prompt
    std::size_t max_retrieval_bytes = 8 * 1024;   // 每轮注入正文总预算
    std::size_t max_results = 3;                  // 每轮最多注入条数
};

struct ProjectIdentity {
    std::filesystem::path project_root;
    std::filesystem::path identity_root;
    // P0-3:项目记忆搬进 workspace——不再有 <home>/projects/<key>/ 的
    // project_dir,记忆根是 <home>/workspaces/<workspace_key>/memory/。
    std::filesystem::path workspace_dir;
    std::string workspace_key;
    std::string display_name;
    bool git = false;
};

// P0-1 起这是形状适配,不是身份算法:裁决(commondir→marker→config→cwd
// 四级 + 统一 workspace_key)只在 workspace::ResolveWorkspaceIdentity 一处,
// 这里把结果折成 memory 域的 ProjectIdentity,并按 P0-3 把根挪进
// <home>/workspaces/<workspace_key>/(首次开仓由 workspace manifest 原子写)。
std::expected<ProjectIdentity, std::string> ResolveProjectIdentity(
    const std::filesystem::path& cwd, const std::filesystem::path& home_lubancode);

// ---------------------------------------------------------------------------
// P0-3:memory 的落账口(召回快照与写入因果边)。memory 域只认这只纯接口,
// trajectory 侧的实现在装配层(app/memory_ledger_bridge),memory 不反向
// include trajectory。
// ---------------------------------------------------------------------------

// 一条真正注入模型的记忆(合同 §四 context.injected 的载荷)。content 只
// 用来算 hash 与落快照,不进 trace、不进 memory 自身账。
struct InjectedMemoryRecord {
    std::string target_run_id;  // 空=主会话;非空=派工进该子代理的冻结快照
    std::string memory_level;   // project | user
    std::string memory_id;
    int memory_schema = 0;
    std::string memory_updated_at;
    std::string content;
    std::string content_sha256;                    // hooks::Sha256Hex(content)
    std::vector<std::string> source_evidence_refs; // 全限定引用
    std::size_t injected_bytes = 0;
};

// memory.save.requested 的申报材料(合同 §四因果边)。
struct SaveLedgerNote {
    std::string operation;       // upsert | forget | verify | rebuild
    std::string layer;           // project | user
    std::string kind;            // fact | preference | feedback(空=非 upsert)
    std::string memory_id;       // 空=自动起 id
    std::string title;
    std::string source_session;  // 裸 session id(全限定由落账方拼)
    std::string originator;      // user_command | model_tool | auto_extraction
};

class MemoryAccounting {
public:
    virtual ~MemoryAccounting() = default;
    // 落一枚 context.injected(快照 artifact 先行写稳)。失败=快照写不稳
    // (§9.2 memory.recall_snapshot_failed),调用方本轮不注入该条。
    virtual std::expected<void, std::string> RecordRecallInjection(const InjectedMemoryRecord& record) = 0;
    // 落 memory.save.requested,回该事件的全限定引用(workspace/session/
    // run/event);失败回空串,调用方用无轨迹的兜底引用。
    virtual std::string RecordSaveRequested(const SaveLedgerNote& note) = 0;
    // 当前落账的 session id(clear 换账后跟着走)。没有账的场合回空串。
    virtual std::string current_session_id() const { return std::string(); }
};

// fact=可核验的项目事实;preference=用户主动选定的项目技术偏好;
// feedback=用户对 LubanCode 行事方式的明确纠正(版本节奏、验收习惯、提交
// 规矩),只收 user-stated,模型推断不得直写。
enum class MemoryKind { Fact, Preference, Feedback };

std::string MemoryKindName(MemoryKind kind);
std::expected<MemoryKind, std::string> ParseMemoryKind(const std::string& raw);

// 主题范围(schema 2/3):project 全项目;subtree/path 限子树或单文件,当前
// cwd 不在范围内时不注入(该用才用)。schema 3 加 level:project|user,
// 用户层主题 level=user 且 kind=user,不得假借项目路径作证据。
// 跨项目/全局经验本期不做——键位预留:以后加 "global" 时只认全局配置授权,
// 存储键须另行分账,别混进 project key 这套目录。
struct MemoryScope {
    std::string kind = "project";  // project | subtree | path | user
    std::string value;             // kind 为 subtree/path 时必填,项目内相对路径
    std::string level = "project"; // project | user(schema 3 起;旧主题读入填默认)
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
    // schema 3 新增:name 是文件 slug(层内唯一),id 去掉类型前缀便是;
    // created_at 记首次创建(旧主题读入时用 updated_at 补);schema 记这份
    // 主题当下的格式(1/2/3),经 upsert/verify 改写后一律成 3。
    std::string name;
    std::string created_at;
    int schema = 2;
    // content 进索引(LoCoMo 改进单第一刀):content 是主题文件的正文本体
    //(标题行除外),文件扫描路读入,catalog 不回存全文;content_index 是
    // 预分词的正文词袋("term:count ..." 空格分隔,分词与查询同款双路手
    // 艺,单词条数与词条总数封顶),catalog 存读——生产检索每轮只解析词
    // 袋,不重切全文。排级侧两条路等价:有词袋吃词袋,没词袋有正文就现切。
    std::string content;
    std::string content_index;
};

// 检索排级的纯函数结果(评测集与 /memory why 共用)。injected 只是"过
// 门槛值得注入",预算裁剪另算。
struct ScoredEntry {
    const MemoryEntry* entry = nullptr;
    int hard_hits = 0;     // 稳定实体(路径/关键词/symbol/标题/id)硬命中次数
    int token_hits = 0;    // 分词后命中的有效词项数(虚词碎片不计)
    int content_hits = 0;  // 命中落在正文侧的有效词组数(注入配对用:正文命
                           // 中的条目除摘要外要带正文相关段)
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
    bool user_enabled = false;  // 用户级记忆(全局授权另设)
    std::string learn;   // off | review | auto(本场档位)
    std::string workspace_key;  // P0-3:与 session 共用的统一钥匙
    std::filesystem::path memory_dir;
    std::filesystem::path user_memory_dir;
    std::size_t entry_count = 0;
    std::size_t user_entry_count = 0;
    std::size_t pending_jobs = 0;
    std::size_t failed_jobs = 0;  // P0-3:worker 挪进 failed 的(job 回执有账)
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
    std::string layer = "project";  // project | user(命中来自哪一层)
    int score = 0;
    int hard_hits = 0;    // 稳定实体(路径/关键词/symbol/标题/id)硬命中次数
    int term_hits = 0;    // 分词后命中的有效词项数(虚词碎片不计)
    int content_hits = 0; // 命中落在正文侧的有效词组数(schema 4 起)
    bool injected = false;
    bool stale_blocked = false;   // 指纹漂移,只提示不注正文
    bool below_threshold = false; // 分数没过最低门槛
    bool budget_dropped = false;  // 过了门槛但预算/条数不够
    bool scope_blocked = false;   // scope 不符当前 cwd,不注入
    bool expired = false;         // 已过 expires_at,不召回
    bool duplicate_dropped = false;  // 同一事实/相同证据,去重让位
    bool layer_superseded = false;   // 用户层同主题被项目层压过
    // P0-3:召回快照落不稳(§9.2)——本轮没注入该条,不得"注了却无账"。
    bool snapshot_failed = false;
    // schema 4(预算选条规则):单条超预算截断只削正文,摘要保完整——此处
    // 记截断事实;drop_reason 是"拦了什么、为什么"的明细(max_results|
    // budget_bytes|empty_payload),不静默。
    bool content_truncated = false;
    std::string drop_reason;
    std::size_t bytes = 0;
};

struct RecallTrace {
    bool valid = false;
    std::string at;
    // P0-3:键名随统一身份换 workspace_key(schema 3;旧档的 project_key
    // 读回时兜底认)。
    std::string workspace_key;
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

    // P0-3:落账口挂接(召回快照 + 写入因果边)。空(默认)= 没接轨迹的
    // 场合(单发/单测),一笔不落,行为与从前一致。归装配层所有,这里只
    // 借指针,不接管寿命。
    void set_accounting(MemoryAccounting* accounting) { accounting_ = accounting; }

    const ProjectIdentity& identity() const { return identity_; }
    const std::filesystem::path& memory_dir() const { return memory_dir_; }
    // P0-3:项目记忆住 <workspace>/memory/,与 session 共一棵 workspace 树;
    // 不再新建 <home>/projects/ 下任何文件。
    const std::filesystem::path& workspace_dir() const { return identity_.workspace_dir; }
    // 用户级记忆目录:<主目录>/memory/user/。与项目记忆分账,各自一份
    // index.md 与 .state/catalog.json。
    std::filesystem::path user_memory_dir() const { return home_lubancode_ / "memory" / "user"; }

    std::expected<void, std::string> SetWorkingDirectory(const std::filesystem::path& cwd);

    // 每条外层用户消息调用一次。返回空串表示本轮无记忆段;没有过门槛的
    // 命中时零注入零脚手架(规格"零命中不塞空脚手架")。origin 记这条
    // 查询从哪来:user 才跑检索,合成控制消息(后台完成唤醒等)默认整轮
    // 跳过,只留 trace 来源;确需事实的合成回流可传 force_retrieval。
    [[nodiscard]] std::string BuildTurnContext(const std::string& query, const std::filesystem::path& cwd,
                                               QueryOrigin origin = QueryOrigin::User,
                                               bool force_retrieval = false) const;

    // P0-3(§6.2):子代理派工的冻结召回——父任务派工当刻按 task prompt
    // 检索一次,结果整段冻结下发,子代理不再自己扫库。target_run_id 是
    // 子代理的 agent_run_id(没有轨迹账时给空串),快照事件以
    // relations.child_run_id 记在父账上。
    [[nodiscard]] std::string BuildTurnContextForDispatch(const std::string& task_prompt,
                                                          const std::filesystem::path& cwd,
                                                          const std::string& target_run_id) const;

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

    // ---- show/open(规格:front matter 摘要与正文;外部编辑回来先校验再
    // 原子替换,坏 YAML 不覆盖原件) ----
    // 按 id 找主题(两层都找)。返回 <主题全文, 所在目录>;找不到给错误。
    std::expected<std::pair<std::string, std::filesystem::path>, std::string> ReadTopicForShow(
        const std::string& id) const;

private:
    std::optional<std::pair<MemoryEntry, std::filesystem::path>> FindTopic(const std::string& id) const;

public:
    // 编辑会话三段式:Begin 建同目录临时副本(原件不被编辑器碰),Commit
    // parse+校验(不得改 id 与层)后原子替换并重建该层派生物;坏 YAML 或
    // 字段不合法,原件分毫不动。EditTopicInEditor 是接 $VISUAL/$EDITOR 的
    // 胶水;测试直接用 Begin/Commit。
    struct TopicEditSession {
        std::filesystem::path original;
        std::filesystem::path scratch;
        std::filesystem::path dir;
        std::string id;
        std::string level;
    };
    std::expected<TopicEditSession, std::string> BeginTopicEdit(const std::string& id) const;
    std::expected<void, std::string> CommitTopicEdit(const TopicEditSession& session) const;
    std::expected<void, std::string> EditTopicInEditor(const std::string& id) const;
    // /memory open 不带 id:编辑器看一眼项目层 index.md(派生物,不校验)。
    std::expected<void, std::string> OpenIndexInEditor() const;

    // ---- 显式迁移(规格"迁移":旧格式主题批迁 schema 3) ----
    // 先出计划:将改几份(schema 1/2)、跳过几份(已是 3 或 archive)、警告
    // 几份(读不动的);不动盘。
    struct MigrationItem {
        std::string file;
        std::string id;
        std::string action;  // migrate | skip | warn
        std::string reason;
    };
    struct MigrationPlan {
        std::vector<MigrationItem> items;
        std::size_t to_migrate = 0;
        std::size_t to_skip = 0;
        std::size_t warnings = 0;
    };
    MigrationPlan PlanMigration() const;
    // 确认后批迁:.state/migration-backup/<时间>/ 留原件,全部写妥、catalog
    // 与 index 重建成功才报完成;中途失败删掉本轮新文件,旧主题与 catalog
    // 仍可用。重跑不重复(已是 schema 3 的跳过)、不改 id、不丢来源会话。
    struct MigrationResult {
        std::size_t migrated = 0;
        std::string backup_dir;
    };
    std::expected<MigrationResult, std::string> RunMigration() const;

    // user_initiated=true 只服务显式用户命令(/memory remember):因果边
    // (memory.save.requested)按 user 记;模型工具与回合尾抽取走默认 false。
    // P0-4 起全局层写入也只认 user_initiated=true 的路。
    std::expected<std::string, std::string> EnqueueSave(const SaveRequest& request,
                                                        bool user_initiated = false);
    // P0-4:显式层路由——layer 为 "user"/"project" 时按命令指定的层动
    // (forget global 的删除边界 §6.4:只认用户命令,本口即命令口);空串
    // 保持旧写法(按 id 自动认层)。
    std::expected<std::string, std::string> EnqueueForget(const std::string& id,
                                                          const std::string& layer = std::string());
    std::expected<std::string, std::string> EnqueueRebuild();
    // 核验:原 id 复活——重算指纹、盖 last_verified_at、status 回 active。
    // refresh=true 时连 status 一并回炉(verify 只盖时间戳)。layer 同
    // EnqueueForget 的显式层路由。
    std::expected<std::string, std::string> EnqueueVerify(const std::string& id, bool refresh,
                                                          const std::string& layer = std::string());

    // 陈旧清单:指纹漂移的与已过期的,附原因(/memory stale 用)。
    struct StaleEntry {
        MemoryEntry entry;
        std::string reason;  // "fingerprint" | "expired"
    };
    std::vector<StaleEntry> ListStaleEntries() const;

    std::vector<MemoryEntry> ListEntries(std::string* error = nullptr) const;
    // 用户层条目(全局授权关着时为空表)。/memory list 合并两层展示。
    std::vector<MemoryEntry> ListUserEntries(std::string* error = nullptr) const;
    // P0-4:全局层的管理读口——/memory list|show global 是用户自己的管理
    // 命令,不看召回授权(user_enabled 只闸召回与写入,不闸眼看自己的库)。
    std::vector<MemoryEntry> ListGlobalEntriesForManagement(std::string* error = nullptr) const;
    RuntimeStatus Status() const;

    // 有 pending job 时起一枚会话级后台 worker。失败不删 job，下次还能捞。
    std::expected<void, std::string> LaunchWorker() const;

private:
    std::string BuildTurnContextImpl(const std::string& query, const std::filesystem::path& cwd,
                                     QueryOrigin origin, bool force_retrieval,
                                     const std::string& target_run_id) const;
    std::expected<std::string, std::string> EnqueueJob(const std::string& operation,
                                                       const SaveRequest* request,
                                                       const std::string& id,
                                                       nlohmann::json extra = nlohmann::json::object(),
                                                       bool user_initiated = false);
    std::filesystem::path CandidatesDir() const;

    ProjectIdentity identity_;
    std::filesystem::path home_lubancode_;
    std::filesystem::path memory_dir_;
    Options options_;
    std::string executable_;
    std::string source_session_;
    MemoryAccounting* accounting_ = nullptr;  // P0-3:装配层挂的落账口
    // 回合总结产出的检索扩展词(下一轮 BuildTurnContext 合并进查询)。
    std::vector<std::string> retrieval_hints_;
};

// 隐藏 CLI 子命令调用。串行捞 pending/*.json；成功删 job，坏 job 挪 failed。
std::expected<std::size_t, std::string> RunPendingMemoryJobs(
    const std::filesystem::path& home_lubancode);

// P0-4:全局记忆目录的健康自检(user-only 权限、symlink 越根、failed job
// 与旧 projects/ 遗留)。/doctor memory 的引擎体:一行一条,先状态字后
// 说明,不发请求、不改盘(HardenDirectoryUserOnly 的复紧是幂等的,算
// 修复不算改账)。
std::vector<std::string> CheckGlobalMemoryHealth(const std::filesystem::path& home_lubancode);

// 测试与 /memory rebuild 共用的同步底层。不起进程。user_layer=true 时按
// 用户层扫描(preferences/feedback,没有 facts),index 头写 User Memory。
std::expected<void, std::string> RebuildMemoryIndex(const std::filesystem::path& memory_dir,
                                                    bool user_layer = false);

}  // namespace lubancode::memory
