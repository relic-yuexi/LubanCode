// 项目级长期记忆。正文用小块 Markdown 存，catalog/index 都能从正文头部
// 的严格 JSON 元数据重建。检索只读本地文件；写入先排 job，再由隐藏 worker
// 进程落盘。
//
// SV-09(2026-09-21 架构审查)起本头是 ProjectMemory 门面合同 + 检索纯
// 函数口 + 目录锁合同:轻合同类型(主题/保存请求/回执/召回结果)住
// memory/types.hpp;词法与召回住 recall_engine,主题存储住 topic_store,
// 候选审阅箱住 candidate_store,写队列与 worker 监督住 worker_queue,编辑
// 器适配住 editor_adapter(皆 src/memory 内部件,经各自内部头互认)。
// ProjectMemory 只做编排:身份一份、授权一份,按调用递给各成员群——
// 读写层不自行解析身份,也不各持授权规则。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory/types.hpp"

namespace lubancode::memory {

// ---------------------------------------------------------------------------
// 检索纯函数(实现住 recall_engine.cpp;评测集、/memory why 与注入路共用)。
// ---------------------------------------------------------------------------

// 归一化(查询与索引共用):NFKC 常用子集(全角 ASCII/全角空格/弯引号/
// 长划/连字/半角片假名)、ASCII 小写、路径分隔符统一正斜杠。标点经这套
// 之后要么归半角要么当分隔符,不再黏进中文二元词。
std::string NormalizeForRetrieval(const std::string& text);

// 标识符拆分 + 中英混排分词:camelCase/snake_case/kebab/路径段都拆
// (BuildTurnContext -> build/turn/context),中文出双字片段;查询与
// 索引共用同一套。
std::vector<std::string> TokenizeForRetrieval(const std::string& text);

// 相关性分级(记忆幻觉根治单 A 刀;判据全文见 types.hpp 的 Relevance 注释)。
RelevanceGrade GradeRelevance(const std::string& topic_text, const std::string& summary,
                              const RelevanceQuery& query, const std::vector<std::string>& anchors,
                              bool pinpoint_hit);

// 纯函数排级:硬命中(路径/关键词/symbol/标题/id 稳定实体,词边界匹配)
// + BM25 软分(词项带权重)。返回有得分的条目(含没过门槛的,给 trace
// 用),qualifies 标记是否过最低门槛(至少一次硬命中,或带整词的两次
// 词项命中/三次纯二元命中,且核心分过线)。traced_terms 回填本轮查询词
// 项(含来源与权重),供 trace 落盘;relevance_query 回填查询词组(同源
// 词组:标识符整串与拆段、词典整词与内部二元同组),供相关性分级用。
// archived/conflict 不参与;过期/scope 越区打标记由调用方拦。指纹漂移要
// 摸项目文件,也由调用方判。
std::vector<ScoredEntry> RankEntries(const std::vector<MemoryEntry>& entries, const std::string& query,
                                     const std::string& cwd_relative,
                                     const std::vector<std::string>& hints = {},
                                     std::vector<TraceTerm>* traced_terms = nullptr,
                                     RelevanceQuery* relevance_query = nullptr);

// ---------------------------------------------------------------------------
// SV-01(2026-09-21 架构审查):目录锁所有权。worker.lock(memory-jobs 队列
// 独占)与 memory.lock(项目/用户层写互斥)共用。旧 DirectoryLock 按 mtime
// 年龄判死夺锁——活 worker 连续处理队列超过 30 秒,宿主再唤醒就起第二只
// worker 删旧锁取同一路径,旧句柄析构又删掉新持有者的锁。本锁只认所有权:
//   - owner 账(锁目录里 owner 文件:PID+进程起始 token+随机 owner token)
//     判持有者活死,范式同 GatewayLock/InstallRootLock 的身份核;活持有者
//     不因时间长被夺锁;
//   - 持有者死透/PID 被复用:整目录改名 *.stale-<ms> 隔离留证再重建,不猜
//     死后直接删;无 owner 的旧格式锁(老 DirectoryLock 的空目录)同样
//     隔离明报;
//   - owner 在但读不懂:BrokenLock 明报,不敢动;
//   - 释放前重读 owner 核 pid+owner token,不是自己这只句柄拿的锁不删。
// 探测(HolderAlive)与取锁(TryAcquire)/释放(Release)共用同一份裁决。
// Windows 瞬态(防病毒/过滤驱动短拒)在 owner 读、隔离改名、释放删除里
// 都有界重试,烧完才算真失败。
// ---------------------------------------------------------------------------
class OwnerLock {
public:
    enum class Status {
        Acquired,         // 占住,owner 账已落盘
        HeldByLiveHolder, // 持有者活着(或在建窗口/探不出按活保守),拒绝
        BrokenLock,       // owner 在但读不懂:不敢动,明报
        IoError,          // 占位/隔离/写账真失败
    };
    struct Result {
        Status status = Status::IoError;
        std::string detail;  // 人话诊断(持有者 pid、隔离留证落点、失败原因)
    };

    // 取锁。out 之前持有的锁先释放。陈锁隔离后对手可能先占,内部有界
    // 重试,撞满即报,不无限绕。
    static Result TryAcquire(const std::filesystem::path& dir, OwnerLock* out);
    // 只读探测:目录在且持有者活着(在建窗口/读不懂按活保守)。不创建、
    // 不删、不偷锁——WorkerLockHeld 与取锁路共用这份裁决。
    static bool HolderAlive(const std::filesystem::path& dir, std::string* detail = nullptr);

    OwnerLock() = default;
    ~OwnerLock();
    OwnerLock(OwnerLock&& other) noexcept;
    OwnerLock& operator=(OwnerLock&& other) noexcept;
    OwnerLock(const OwnerLock&) = delete;
    OwnerLock& operator=(const OwnerLock&) = delete;

    bool holds() const { return !dir_.empty(); }
    const std::filesystem::path& dir() const { return dir_; }
    // 显式释放;析构也会做。只删 owner 账核得上的自己的锁。
    void Release();

private:
    std::filesystem::path dir_;
    std::FILE* file_ = nullptr;  // owner 的只读句柄:Windows 上挡他者删/改名
    std::string owner_token_;    // 本次占位的随机 token,释放核账用
};

// 写队列与 worker 监督的实现件(SV-09 拆出;定义住 worker_queue.hpp)。
// 门面经 shared_ptr 间接持有——保住移动构造。
namespace queue { class MemoryWriteQueue; }

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
    // 记忆写入调度单 P0:写路回执收件口挂接。空(默认)= 没人收,写路
    // 一切照旧。纯观测,不接管寿命。
    void set_write_receipt_sink(MemoryWriteReceiptSink* sink) { write_receipt_sink_ = sink; }

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
    // turn_id(T08/V3-GAP-03):将开这轮的主回合号——v3 落账侧给注入快照
    // 的隐藏 user 消息署名用;空 = 不署(老调用方/单测,行为同前)。
    [[nodiscard]] std::string BuildTurnContext(const std::string& query, const std::filesystem::path& cwd,
                                               QueryOrigin origin = QueryOrigin::User,
                                               bool force_retrieval = false,
                                               const std::string& turn_id = std::string()) const;

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
    // 成功值是结构化入队结果(§五 B):job_id + worker 启动账。
    std::expected<MemoryEnqueueResult, std::string> AcceptCandidate(const std::string& id);
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

public:
    // 编辑会话三段式:Begin 建同目录临时副本(原件不被编辑器碰),Commit
    // parse+校验(不得改 id 与层)后原子替换并重建该层派生物;坏 YAML 或
    // 字段不合法,原件分毫不动。EditTopicInEditor 是接 $VISUAL/$EDITOR 的
    // 胶水(进程交互经 editor_adapter);测试直接用 Begin/Commit。
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
    // 记忆写入调度单 P0:source 只喂写路回执(§6.2),不碰 user_initiated
    // 的既有语义与因果边——默认值保持老调用方的口径(model_tool)。
    // 修复单 §五 B:成功值是结构化结果——job_id 纯净,worker 启动成败
    // 另列字段;只有 queue 持久化失败才回 unexpected。
    std::expected<MemoryEnqueueResult, std::string> EnqueueSave(
        const SaveRequest& request, bool user_initiated = false,
        MemoryWriteSource source = MemoryWriteSource::ModelToolSave);
    // P0-4:显式层路由——layer 为 "user"/"project" 时按命令指定的层动
    // (forget global 的删除边界 §6.4:只认用户命令,本口即命令口);空串
    // 保持旧写法(按 id 自动认层)。
    std::expected<MemoryEnqueueResult, std::string> EnqueueForget(const std::string& id,
                                                                  const std::string& layer = std::string());
    std::expected<MemoryEnqueueResult, std::string> EnqueueRebuild();
    // 核验:原 id 复活——重算指纹、盖 last_verified_at、status 回 active。
    // refresh=true 时连 status 一并回炉(verify 只盖时间戳)。layer 同
    // EnqueueForget 的显式层路由。
    std::expected<MemoryEnqueueResult, std::string> EnqueueVerify(const std::string& id, bool refresh,
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

    // ---- worker 监督(修复单 §五 A/P0-B) ----
    // 有 pending job 时确保有一只后台 worker 在跑。共享监督器按规范化
    // state_root 进程级共享:同根多份 ProjectMemory 共用一只池,不再每次
    // enqueue 都拉一批争锁的子进程;活 worker 尚在时合并唤醒,已退出时
    // 先收退出码再看 pending。带 pending 退出记 worker_exited_with_pending,
    // 有界退避(1s→30s 封顶)重试,达上限提示,不忙循环。失败不删 job,
    // 下次还能捞。
    MemoryWorkerWake EnsureWorkerRunning() const;
    // 池里还有活 worker 吗(/memory jobs 的 worker 行与防风暴测试用)。
    bool HasRunningWorker() const;
    // 本进程经共享监督器累计拉起的 worker 数(诊断/防风暴断言)。
    std::uint64_t WorkerSpawnCount() const;
    // 退出收尾(修复单 §五 A"正常退出先给一段有界收尾时间"):有界等活
    // worker 自己跑完;超时不杀——pending 仍在盘上,下次会话恢复。会话
    // 退场路调,grace_ms=0 只看一眼。
    void WaitForWorkersGracefully(int grace_ms) const;

    // ---- 任务台账与重试(修复单 §五 C:/memory jobs) ----
    // 当前工作区的待写/失败任务清单(按 workspace_key 过滤,别区的数不
    // 算进来)。只读。
    struct MemoryJobInfo {
        std::string job_id;        // 文件名(含 .json)
        std::string state;         // pending | failed
        std::string operation;     // upsert | forget | verify | rebuild | ...
        std::string title;         // job 自带的 title(可空)
        std::string layer;         // project | user(按 memory_dir 判)
        std::string created_at;    // job 的 created_at(可空)
        std::string wait_hint;     // 人话等待时长,如 "3m12s"(按文件 mtime)
        std::string error;         // failed 时的 .error.txt 首行(可空)
        std::string worker_state;  // 监督器池状态人话(running/exited(code=N)/none)
        std::string worker_log;    // 最近一只 worker 的日志路径(可空)
    };
    std::vector<MemoryJobInfo> ListWorkspaceJobs() const;
    // 唤醒 pending(相当于 /memory jobs retry 不带参数):EnsureWorkerRunning
    // 的直通口。
    MemoryWorkerWake WakePendingWorker() const;
    // 重试一笔 failed 任务(显式选择):校验工作区归属;原 job 已有 committed
    // 回执的拒重放;否则按新 job 名重排回 pending(新 operation_id,全新
    // lifecycle 账,不回改历史),返回新 job_id。
    std::expected<std::string, std::string> RetryFailedJob(const std::string& job_id);

    // ---- 提交回执消费(修复单 §五 C 尾:"已入库"只认回执) ----
    // 本会话经 EnqueueXxx 排的 job 逐笔记账;DrainWriteCompletions 只从
    // lifecycle/result.json 回收取数(核对 operation_id 与所在 lifecycle
    // 根,workspace 根按目录绑定工作区),pending 消失不冒充成功。同一
    // operation_id 只报一次。用户层 job 用 memory/user/.state/lifecycle 的
    // 同形回执(等价完成依据)。主线程调用(空闲唤醒泵与回合边界)。
    struct MemoryWriteCompletion {
        std::string job_id;
        std::string operation_id;
        std::string layer;      // project | user
        std::string title;      // 排队时记下的标题(可空)
        std::string outcome;    // committed | failed
        std::string memory_id;  // committed 时的正式 id(可空)
        std::string error;      // failed 时的原因(可空)
    };
    // 空闲唤醒源用:有没有值得立刻收的账(回执已落地,或排的 job 还在
    // pending 却已无活 worker——后者顺带在 DrainWriteCompletions 里补拉)。
    bool WakeNeededForWrites() const;
    // 收一次账:已落地的回执折成完成通知并销账;还在 pending 且无活
    // worker 的,过一次 EnsureWorkerRunning(有界退避在监督器里挡着)。
    std::vector<MemoryWriteCompletion> DrainWriteCompletions();

private:
    // job 材料拼装(身份/因果边/正文)后交队列落盘与唤醒——队列件住
    // worker_queue,这里只拼料。
    std::expected<MemoryEnqueueResult, std::string> EnqueueJob(const std::string& operation,
                                                               const SaveRequest* request,
                                                               const std::string& id,
                                                               nlohmann::json extra = nlohmann::json::object(),
                                                               bool user_initiated = false);

    ProjectIdentity identity_;
    std::filesystem::path home_lubancode_;
    std::filesystem::path memory_dir_;
    Options options_;
    std::string source_session_;
    MemoryAccounting* accounting_ = nullptr;  // P0-3:装配层挂的落账口
    // 记忆写入调度单 P0:写路回执收件口(装配层挂;空 = 没人收)。
    MemoryWriteReceiptSink* write_receipt_sink_ = nullptr;
    // 写队列与 worker 监督(SV-09 拆出):pending 落盘、回执追踪、监督器
    // 池都在里头。shared_ptr 间接持有保住移动构造(mutex 直接做成员会
    // 把它删掉),构造在 .cpp(那边看得见完整类型)。
    std::shared_ptr<queue::MemoryWriteQueue> write_queue_;
    // EnqueueSave/EnqueueForget 的原函数体(逻辑一字不动),public 口只加
    // 回执投递。
    std::expected<MemoryEnqueueResult, std::string> EnqueueSaveImpl(const SaveRequest& request,
                                                                    bool user_initiated);
    std::expected<MemoryEnqueueResult, std::string> EnqueueForgetImpl(const std::string& id,
                                                                      const std::string& layer);
    // EnqueueSave/EnqueueForget 的回执投递(源语义见 struct 注释)。
    void EmitWriteReceipt(MemoryWriteSource source, const std::string& operation,
                          const SaveRequest* request, const std::string& layer,
                          const std::expected<MemoryEnqueueResult, std::string>& queued);
    // 回合总结产出的检索扩展词(下一轮 BuildTurnContext 合并进查询)。
    std::vector<std::string> retrieval_hints_;
};

// P0-1 起这是形状适配,不是身份算法:裁决(commondir→marker→config→cwd
// 四级 + 统一 workspace_key)只在 workspace::ResolveWorkspaceIdentity 一处,
// 这里把结果折成 memory 域的 ProjectIdentity,并按 P0-3 把根挪进
// <home>/workspaces/<workspace_key>/(首次开仓由 workspace manifest 原子写)。
std::expected<ProjectIdentity, std::string> ResolveProjectIdentity(
    const std::filesystem::path& cwd, const std::filesystem::path& home_lubancode);

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
