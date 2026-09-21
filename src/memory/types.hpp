// SV-09(2026-09-21 架构审查)拆出的记忆域轻合同:主题、保存请求、回执、
// 召回结果与各路枚举。只放数据形状与配套的名字/解析函数,不放运行编排
// ——ProjectMemory 门面、worker、检索引擎都不在这。只需 MemoryEntry 的头
// (frontmatter/evolution)引这份就够,不再拖进整套运行接口。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------------
// P0-3:memory 的落账口(召回快照与写入因果边)。memory 域只认这只纯接口,
// trajectory 侧的实现在装配层(app/memory_ledger_bridge),memory 不反向
// include trajectory。
// ---------------------------------------------------------------------------

// 一条真正注入模型的记忆(合同 §四 context.injected 的载荷)。content 只
// 用来算 hash 与落快照,不进 trace、不进 memory 自身账。
struct InjectedMemoryRecord {
    std::string target_run_id;  // 空=主会话;非空=派工进该子代理的冻结快照
    // T08(V3-GAP-03):这次注入挂的主回合号。v3 落账要用它给隐藏 user
    // 消息署 turnId(schema:user 消息 turnId 必填)——召回发生在回合开跑
    // 之前,调用方(BuildTurnContext 的 turn_id 形参)把将开那轮的号先递
    // 进来。空 = 调用方不知道(派工快照/老调用方),落账方自己处置。
    std::string turn_id;
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

// ---------------------------------------------------------------------------
// 记忆写入调度单 P0(§6.2):写路回执。纯观测——不改任何控制流,sink
// 默认空,一切行为与从前逐字节一致。四路显式写路(显式命令保存/模型
// 工具保存/显式忘记/候选接受)外加回合尾抽取的 auto 直写,在 job 排队
// 成败的当口各投一张回执。
//
// outcome 诚实两态:queued(job 已排进 pending,尚未落盘)/rejected(没
// 排进去)。落盘与否是 worker 的 lifecycle 账,P0 不冒充 committed——
// "job 只排队未执行须记 queued,不得冒充已落盘"。
//
// 枚举线上名稳定(§15:同一冻结输入跨平台判断一致),改名即改合同。
// ---------------------------------------------------------------------------

// 写入来源五路:§6.2 的四路 + 抽取 auto 直写(§10.2 auto_queued 的分子)。
enum class MemoryWriteSource {
    ExplicitCommandSave,  // /memory remember(§6.2 explicit_command_save)
    ModelToolSave,        // 主模型 memory_save 工具(§6.2 model_tool_save)
    ExplicitForget,       // /memory forget(§6.2 explicit_forget)
    CandidateAccept,      // /memory accept(§6.2 candidate_accept)
    AutoExtraction,       // 回合尾抽取的 auto 档直写(§10.2 auto_queued)
};
std::string MemoryWriteSourceName(MemoryWriteSource source);

// 排队成败两态(§6.2)。不叫 MemoryWriteOutcome——worker 落盘回执的
// 四件套同文件另有同名结构,撞名会让 TU 内的裸引用歧义。
enum class MemoryWriteReceiptOutcome { Queued, Rejected };
std::string MemoryWriteReceiptOutcomeName(MemoryWriteReceiptOutcome outcome);

// 一张回执。只带稳定枚举、job id 与稳定错误码;标题、正文、路径不进
// 回执(§10.3 隐私线:本地 typed event 与 telemetry 同此口径)。
struct MemoryWriteReceipt {
    MemoryWriteSource source = MemoryWriteSource::ModelToolSave;
    MemoryWriteReceiptOutcome outcome = MemoryWriteReceiptOutcome::Queued;
    std::string operation;  // upsert | forget
    std::string job_id;     // queued 时的 job 文件名;rejected 空
    std::string error_code; // rejected 时的稳定码(StableWriteErrorCode)
    std::string layer;      // project | user
    std::string kind;       // fact | preference | feedback(空 = forget)
};

// 回执收件口。实现方自己管线程安全(写路可能在回合内的工具执行里)。
class MemoryWriteReceiptSink {
public:
    virtual ~MemoryWriteReceiptSink() = default;
    virtual void OnMemoryWriteReceipt(const MemoryWriteReceipt& receipt) = 0;
};

// 把 EnqueueXxx 的拒绝人话折成稳定码。认不出的落 "other",不猜——
// 错误文案改动只会降级成 other,不会错归因。
std::string StableWriteErrorCode(const std::string& error);

// ---------------------------------------------------------------------------
// 结构化入队结果(修复单 §五 B):queue 持久化与 worker 启动是两笔账,
// 不许再混进一个字符串。排队成功 = expected 的成功值;只有 queue 持久化
// 失败才判"未入队"(unexpected)。job_id 是纯文件名(含 .json),诊断
// 文字一律走 worker_error/worker_error_code,不污染 job_id(回执、
// /memory jobs 查账都靠它)。
// ---------------------------------------------------------------------------

// queue 侧只有一态:persisted(已原子写进 pending)。将来若加内存排队
// 再扩,现在不预造假态。
enum class MemoryQueueState { Persisted };

// worker 侧四态(修复单 §五 B):
//   Started         这次调用真拉起了一只 worker
//   AlreadyRunning  已有活 worker(或退避合并窗口内的合并唤醒),不再起一只
//   StartFailed     拉了没起来,或带 pending 退出后的有界退避中——job 仍在
//                   pending,两个事实都在,不算入队失败
//   Unavailable     本对象没配 executable(单测/单发形态),不冒充启动成功
//   Idle            盘上已无待写任务,无需 worker(EnsureWorkerRunning 的
//                   常态回执;入队结果撞见它 = job 刚落盘就被现役 worker 吃掉)
enum class MemoryWorkerLaunchState { Started, AlreadyRunning, StartFailed, Unavailable, Idle };
std::string MemoryWorkerLaunchStateName(MemoryWorkerLaunchState state);

struct MemoryEnqueueResult {
    std::string job_id;  // pending/<job_id> 的文件名,如 "1789753353477-0.json"
    MemoryQueueState queue_state = MemoryQueueState::Persisted;
    MemoryWorkerLaunchState worker_state = MemoryWorkerLaunchState::Unavailable;
    std::string worker_error_code;  // StartFailed/Unavailable 时的稳定码(可为空)
    std::string worker_error;       // StartFailed/Unavailable 时的短说明(可为空)
};

// EnsureWorkerRunning 的回执:worker 池这次协调的结局(不带 queue 账,
// 只报 worker 侧)。state 语义同 MemoryWorkerLaunchState;error 字样随
// state 走。会话启动/唤醒路(/memory jobs retry、session_stack)用它。
struct MemoryWorkerWake {
    MemoryWorkerLaunchState state = MemoryWorkerLaunchState::Unavailable;
    std::string error_code;
    std::string error;
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
    // 时间线锚点(记忆写入侧改进单):fact 事件发生时间,从材料里提——
    // 提不出留空,不造假。注入侧带时间的多条召回按它排成时间线。
    std::string occurred_at;              // 空 = 材料里没有时间;ISO 日期或日期时间
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
    // 时间线锚点(schema 3 演进,向后兼容):fact 事件发生时间,旧条目无此
    // 字段读入为空,不参与时间排序。frontmatter 键 occurred_at,catalog 同名。
    std::string occurred_at;
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
    int core = 0;          // 硬命中分 + BM25 折算分(不含 cwd 排位加分;门槛
                           // 与弱档地板判这个)
    bool qualifies = false;       // 过最低门槛,值得注入(调用方据此判)
    bool stale_blocked = false;   // 指纹漂移,只提示不注正文(由调用方判)
    bool expired = false;         // 已过 expires_at,不召回
    bool scope_blocked = false;   // scope 不符当前 cwd,不注入
    // 相关性分级(记忆幻觉根治单 A 刀)材料:anchors 是硬命中的稳定实体
    // 词面(关键词/标题/id——问题点名、条目自报的实体,归一化后);路径
    // 与 symbol 硬命中单记 pinpoint_hit——问题点名了具体文件或符号,定位
    // 精确,不再要行级共现背书。
    std::vector<std::string> anchors;
    bool pinpoint_hit = false;
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

// occurred_at/expires_at 的宽松日期形状(YYYY-MM-DD 起头,可带 ISO 时
// 间;字典序即时间序)。写入校验与抽取侧清洗共用:模型给的日期不像样就
// 落空,不造假也不拦整条保存。
bool LooksLikeMemoryDate(const std::string& raw);

// ---- 相关性分级(记忆幻觉根治单 A 刀) ----
// 词组:同源查询词组(整串与拆段、整词与内部二元同组),门槛计数同款
// 口径(权重 >= 0.5 的词面才算数)。锚:硬命中的稳定实体(关键词/标题/
// id),即"问题点名且条目自报"的实体。
//
// 判据(工头定形,五场 adversarial 数据上校准):
//   强相关 = 路径/symbol 硬命中(问题点名具体文件或符号,pinpoint),
//           或某一行里 >= 2 个词组与 >= 1 个锚同现(实体-动作-对象一句
//           话说全,不是散在各行的词面重叠;问题本身不足两个词组时门槛
//           收到 1——"部署节奏是什么"这类单实体问法,锚与唯一词组同行
//           即算实质匹配),
//           或条目无锚可用(没硬命中任何实体)时某一行 >= 3 个词组同现
//           (纯内容匹配的实质共现兜底——中文内容命中的生产常态,别把没
//           有实体词典的库全判弱;有锚时锚必须进同一行,否则英文长问题
//           的闲聊行凑三个词就能混进强档)。
//   其余只过检索门槛的命中 = 弱相关:词面重叠、话题沾边。adversarial 五
//   场实测:top3 全弱率 28%(整段不注即回裸底),而可答四桶证据丢失仅
//   0~6%——分级不是为了把弱档说死,是把不确定性如实传给模型。
struct RelevanceGroup {
    std::vector<std::string> terms;  // 该词组的归一化词面(去重)
};

struct RelevanceQuery {
    std::vector<RelevanceGroup> groups;
};

struct RelevanceGrade {
    bool strong = false;      // 强相关(判据见上)
    int best_line_groups = 0; // 单行同现词组数上限(trace 审计用)
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
    std::string occurred_at;  // 时间线锚点:材料里有才填,accept 时转正式条目
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
    // budget_bytes|empty_payload|weak_floor),不静默。
    bool content_truncated = false;
    // 相关性分级(schema 5):weak=按弱相关档注入(垫尾+[弱相关]标注);
    // cooccur=单行同现词组数上限(判据审计用);weak_dropped=弱档被
    // "直接不注"处置拦下(kDropWeakRecalls/kWeakRecallFloor)。
    bool weak = false;
    int cooccur = 0;
    bool weak_dropped = false;
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

}  // namespace lubancode::memory
