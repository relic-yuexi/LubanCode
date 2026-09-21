// ProjectMemory 门面(SV-09,2026-09-21 架构审查):身份一份、授权一份,
// 按调用把材料递给各成员群——召回住 recall_engine,主题存储住 topic_store,
// 候选审阅箱住 candidate_store,写队列与 worker 监督住 worker_queue,编辑
// 器交互住 editor_adapter。函数体从原大一统实现原样搬来:BuildTurnContext
// 只递料,EnqueueJob 只拼 job 材料,授权判定与回执投递仍在本层(授权规
// 则不许拆成多份)。

#include "memory/project_memory.hpp"

#include <chrono>
#include <system_error>

#include <nlohmann/json.hpp>

#include "memory/candidate_store.hpp"
#include "memory/editor_adapter.hpp"
#include "memory/internal.hpp"
#include "memory/recall_engine.hpp"
#include "memory/topic_store.hpp"
#include "memory/worker_queue.hpp"
#include "trajectory/safety.hpp"     // P0-4:全局目录 user-only 收紧与越根检查
#include "workspace/identity.hpp"    // P0-1:身份裁决唯一入口
#include "workspace/manifest.hpp"    // P0-3:memory 根进 workspace 树,首仓原子写

namespace lubancode::memory {

namespace fs = std::filesystem;

// P0-1 起这是形状适配,不是身份算法(实现注释放这,声明注释放头):裁决
// 只在 workspace::ResolveWorkspaceIdentity 一处,这里把 WorkspaceIdentity
// 折成 memory 域的 ProjectIdentity;P0-3 把根搬进
// <home>/workspaces/<workspace_key>/(与 session 同一棵树,首仓 manifest
// 原子写),不再落 <home>/projects/ 下任何文件。
std::expected<ProjectIdentity, std::string> ResolveProjectIdentity(
    const fs::path& cwd, const fs::path& home_lubancode) {
    if (home_lubancode.empty()) return std::unexpected("找不到 LubanCode 主目录");
    auto resolved = workspace::ResolveWorkspaceIdentity(cwd, home_lubancode);
    if (!resolved.has_value()) return std::unexpected(resolved.error());

    ProjectIdentity identity;
    identity.project_root = resolved->project_root;
    identity.identity_root = resolved->identity_root;
    identity.git = resolved->git();
    identity.display_name = resolved->display_name;
    identity.workspace_key = resolved->workspace_key;
    const fs::path home = AbsoluteNormal(home_lubancode);
    // 首仓原子写/开仓对账与 trajectory 侧同一只口:同 key 幂等,manifest
    // 与算法不合会报错(不自动改名并账)。memory 单独跑(单发/worker 命中
    // 新仓)时也要有 manifest,session 开张时不重复造。账本制起目录名走
    // 门牌,房门从注册口回填,不再拿 workspace_key 拼目录。
    fs::path workspace_dir;
    const auto registered = workspace::OpenOrRegisterWorkspace(
        home / "workspaces", *resolved, std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count(),
        nullptr, &workspace_dir);
    if (!registered.has_value()) return std::unexpected(registered.error());
    identity.workspace_dir = workspace_dir;
    return identity;
}

ProjectMemory::ProjectMemory(ProjectIdentity identity, fs::path home_lubancode,
                             Options options, std::string executable)
    : identity_(std::move(identity)),
      home_lubancode_(AbsoluteNormal(home_lubancode)),
      memory_dir_(identity_.workspace_dir / "memory"),
      options_(options),
      write_queue_(std::make_shared<queue::MemoryWriteQueue>(home_lubancode_,
                                                             std::move(executable))) {
    // P0-4:全局目录已是 user-only(§9.3);已存在的目录顺手复紧一遍
    //(幂等,失败留给 /doctor memory 报)。
    std::error_code ec;
    if (fs::is_directory(user_memory_dir(), ec)) {
        (void)trajectory::HardenDirectoryUserOnly(user_memory_dir());
    }
}

std::expected<void, std::string> ProjectMemory::SetWorkingDirectory(const fs::path& cwd) {
    auto identity = ResolveProjectIdentity(cwd, home_lubancode_);
    if (!identity.has_value()) return std::unexpected(identity.error());
    identity_ = std::move(*identity);
    memory_dir_ = identity_.workspace_dir / "memory";
    return {};
}

std::string ProjectMemory::BuildTurnContext(const std::string& query, const fs::path& cwd,
                                            QueryOrigin origin, bool force_retrieval,
                                            const std::string& turn_id) const {
    return recall::BuildTurnContext(options_, identity_, memory_dir_, user_memory_dir(),
                                    retrieval_hints_, accounting_, query, cwd, origin,
                                    force_retrieval, /*target_run_id=*/std::string(), turn_id);
}

std::string ProjectMemory::BuildTurnContextForDispatch(const std::string& task_prompt,
                                                       const fs::path& cwd,
                                                       const std::string& target_run_id) const {
    // §6.2:子代理不自动扫整库——派工当刻检索一次,结果整段冻结,事件的
    // relations.child_run_id 记 target_run_id(父账上说得清发给了哪只孩子)。
    // turn_id 不递:派工快照落父账只是一枚事实(父模型没见过这段),v3
    // 落账侧不给它署主回合的隐藏消息,回合号无从谈起。
    return recall::BuildTurnContext(options_, identity_, memory_dir_, user_memory_dir(),
                                    retrieval_hints_, accounting_, task_prompt, cwd,
                                    QueryOrigin::User, /*force_retrieval=*/false,
                                    target_run_id, /*turn_id=*/std::string());
}

RecallTrace ProjectMemory::LastTrace() const {
    return recall::ReadRecallTrace(memory_dir_);
}

std::expected<void, std::string> ProjectMemory::set_enabled(bool enabled) {
    if (enabled && !options_.global_allowed) {
        return std::unexpected("全局配置未授权开启项目记忆；本场命令开不了");
    }
    options_.enabled = enabled;
    return {};
}

std::expected<void, std::string> ProjectMemory::set_learn(LearnMode mode) {
    if (!options_.global_allowed) {
        return std::unexpected("全局配置未授权开启项目记忆；本场命令开不了");
    }
    if (mode > options_.learn_ceiling) {
        return std::unexpected("learn 档位超出了配置授权上限(" + LearnModeName(options_.learn_ceiling) +
                               ");auto 须在全局配置 memory.learn 里显式授权");
    }
    options_.learn = mode;
    return {};
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueSave(const SaveRequest& request,
                                                                            bool user_initiated,
                                                                            MemoryWriteSource source) {
    // 记忆写入调度单 P0:函数体一字未动(搬进 Impl),外壳只加回执投递。
    const auto queued = EnqueueSaveImpl(request, user_initiated);
    EmitWriteReceipt(source, "upsert", &request,
                     request.scope.level == "user" ? "user" : "project", queued);
    return queued;
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueSaveImpl(const SaveRequest& request,
                                                                                bool user_initiated) {
    if (!generate_enabled()) return std::unexpected("本场记忆写入未开启");
    // 存储 v2 P0-4(§6.1):全局层只认用户主动命令——memory_save 工具、
    // 回合尾抽取、候选 accept 都不带 user_initiated,一律拒;项目配置也
    // 开不了这道口(config merge 层只认全局授权)。
    if (request.scope.level == "user" && !user_initiated) {
        return std::unexpected(
            "memory.global_unauthorized: 全局记忆只认用户主动命令(/memory remember global ...),"
            "模型工具与回合尾抽取不得直写");
    }
    // 用户级记忆的授权另设一道:项目配置无权开启或写入(规格"用户层必须
    // 另设全局授权")。
    if (request.scope.level == "user" && !options_.user_enabled) {
        return std::unexpected("用户级记忆未在全局配置授权(memory.user_enabled),本场命令开不了");
    }
    SaveRequest with_source = request;
    if (with_source.source_session.empty()) with_source.source_session = source_session_;
    if (auto valid = store::ValidateSaveRequest(with_source); !valid.has_value()) return std::unexpected(valid.error());
    return EnqueueJob("upsert", &with_source, with_source.id, nlohmann::json::object(), user_initiated);
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueForget(const std::string& id,
                                                                              const std::string& layer) {
    // forget 恒为用户命令口(§6.4),回执按 explicit_forget 投。
    const auto queued = EnqueueForgetImpl(id, layer);
    EmitWriteReceipt(MemoryWriteSource::ExplicitForget, "forget", nullptr,
                     layer == "user" ? "user" : "project", queued);
    return queued;
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueForgetImpl(const std::string& id,
                                                                                  const std::string& layer) {
    if (!options_.global_allowed || !options_.enabled) return std::unexpected("本场记忆未开启");
    if (!IsValidId(id)) return std::unexpected("记忆 id 不合法");
    // 显式层(P0-4 的 forget global|project)路由到指定层;没给层的旧写法
    // 保持自动:id 住在哪一层就忘了哪一层(用户层开着且在那边找得到,job
    // 落到用户目录)。全局层的删除只认用户命令(§6.4),这里本就是命令口。
    nlohmann::json extra;
    const bool to_user = layer == "user" || (layer.empty() && options_.user_enabled &&
                                             store::LayerHasEntry(user_memory_dir(), id));
    if (layer == "user" && !options_.user_enabled) {
        return std::unexpected("用户级记忆未在全局配置授权(memory.user_enabled),本场命令开不了");
    }
    if (to_user) {
        extra["layer"] = "user";
        extra["memory_dir"] = PathUtf8(user_memory_dir());
    }
    return EnqueueJob("forget", nullptr, id, extra, /*user_initiated=*/true);
}

// 记忆写入调度单 P0(§6.2):排队成败的当口投一张回执。sink 空 = 没人
// 收,纯空操作。queued 有值 = job 已进 pending(outcome=queued,不冒充
// 落盘);否则 rejected + 稳定码。修复单 §五 B:job_id 取结构化结果里
// 的纯文件名,不再吃混着启动说明的字符串。
void ProjectMemory::EmitWriteReceipt(MemoryWriteSource source, const std::string& operation,
                                     const SaveRequest* request, const std::string& layer,
                                     const std::expected<MemoryEnqueueResult, std::string>& queued) {
    if (write_receipt_sink_ == nullptr) return;
    MemoryWriteReceipt receipt;
    receipt.source = source;
    receipt.operation = operation;
    receipt.layer = layer;
    if (request != nullptr) receipt.kind = MemoryKindName(request->kind);
    if (queued.has_value()) {
        receipt.outcome = MemoryWriteReceiptOutcome::Queued;
        receipt.job_id = queued->job_id;
    } else {
        receipt.outcome = MemoryWriteReceiptOutcome::Rejected;
        receipt.error_code = StableWriteErrorCode(queued.error());
    }
    write_receipt_sink_->OnMemoryWriteReceipt(receipt);
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueRebuild() {
    if (!options_.global_allowed || !options_.enabled) return std::unexpected("本场记忆未开启");
    return EnqueueJob("rebuild", nullptr, std::string());
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueVerify(const std::string& id, bool refresh,
                                                                              const std::string& layer) {
    if (!options_.global_allowed || !options_.enabled) return std::unexpected("本场记忆未开启");
    if (!IsValidId(id)) return std::unexpected("记忆 id 不合法");
    // 显式层路由同 forget(P0-4);旧写法按 id 自动认层。
    nlohmann::json extra{{"refresh", refresh}};
    const bool to_user = layer == "user" || (layer.empty() && options_.user_enabled &&
                                             store::LayerHasEntry(user_memory_dir(), id));
    if (layer == "user" && !options_.user_enabled) {
        return std::unexpected("用户级记忆未在全局配置授权(memory.user_enabled),本场命令开不了");
    }
    if (to_user) {
        extra["layer"] = "user";
        extra["memory_dir"] = PathUtf8(user_memory_dir());
    }
    return EnqueueJob("verify", nullptr, id, extra, /*user_initiated=*/true);
}

std::vector<ProjectMemory::StaleEntry> ProjectMemory::ListStaleEntries() const {
    std::vector<StaleEntry> out;
    for (const auto& stored : store::LoadCatalog(memory_dir_)) {
        if (stored.public_entry.status == "archived") continue;
        StaleEntry item;
        item.entry = stored.public_entry;
        if (recall::EntryExpired(stored.public_entry)) {
            item.reason = "expired";
            out.push_back(std::move(item));
            continue;
        }
        if (!store::FingerprintsCurrent(stored, identity_.project_root)) {
            item.reason = "fingerprint";
            out.push_back(std::move(item));
        }
    }
    return out;
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::EnqueueJob(const std::string& operation,
                                                                          const SaveRequest* request,
                                                                          const std::string& id,
                                                                          nlohmann::json extra,
                                                                          bool user_initiated) {
    // P0-3:先落 memory.save.requested 因果边(合同 §四)。sink 在场时事件
    // id 进全限定引用;不在场(单发/单测)用 workspace+session 兜底段,
    // 生产写入一律全限定,不再落裸 session id。
    SaveLedgerNote note;
    note.operation = operation;
    note.layer = request != nullptr && request->scope.level == "user" ? "user" : "project";
    // session 段活取:clear 换账后 source_session_ 可能是旧值,落账桥知道
    // 现役 session id。request 显式带来的(全限定/单测注入)优先。
    note.source_session = request != nullptr && !request->source_session.empty()
                              ? request->source_session
                              : (accounting_ != nullptr ? accounting_->current_session_id()
                                                        : source_session_);
    note.originator = user_initiated ? "user_command" : "model_tool";
    if (request != nullptr) {
        note.kind = MemoryKindName(request->kind);
        note.memory_id = request->id;
        note.title = request->title;
    }
    std::string source_ref;
    if (accounting_ != nullptr) {
        source_ref = accounting_->RecordSaveRequested(note);
    }
    if (source_ref.empty()) {
        source_ref = "workspace_key=" + identity_.workspace_key + "/session_id=" +
                     (note.source_session.empty() ? std::string("none") : note.source_session) +
                     "/run_id=none/event_id=none";
    }

    nlohmann::json job{
        {"schema", 1},
        {"operation", operation},
        {"workspace_key", identity_.workspace_key},
        {"display_name", identity_.display_name},
        {"project_root", PathUtf8(identity_.project_root)},
        {"workspace_dir", PathUtf8(identity_.workspace_dir)},
        {"memory_dir", PathUtf8(request != nullptr && request->scope.level == "user"
                                    ? user_memory_dir()
                                    : memory_dir_)},
        {"source_event_ref", source_ref},
        {"created_at", NowIsoUtc()},
    };
    if (!extra.is_object()) extra = nlohmann::json::object();
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        job[it.key()] = it.value();
    }
    if (!id.empty()) job["id"] = id;
    if (request != nullptr) {
        job["kind"] = MemoryKindName(request->kind);
        job["title"] = request->title;
        job["summary"] = request->summary;
        job["content"] = request->content;
        job["keywords"] = request->keywords;
        job["paths"] = request->paths;
        job["source_session"] = source_ref;
        if (!request->confidence.empty()) job["confidence"] = request->confidence;
        if (!request->expires_at.empty()) job["expires_at"] = request->expires_at;
        if (!request->occurred_at.empty()) job["occurred_at"] = request->occurred_at;
        if (request->scope.level != "project" || request->scope.kind != "project" ||
            !request->scope.value.empty()) {
            job["scope"] = nlohmann::json{{"level", request->scope.level},
                                          {"kind", request->scope.kind},
                                          {"value", request->scope.value}};
        }
        if (!request->evidence.empty()) {
            nlohmann::json evidence = nlohmann::json::array();
            for (const auto& item : request->evidence) {
                evidence.push_back(nlohmann::json{{"path", item.path}, {"symbol", item.symbol}});
            }
            job["evidence"] = std::move(evidence);
        }
    }
    // 落盘、回执立账与唤醒都交队列(worker_queue);失败只在 queue 持久化
    // 那一步,worker 启动账另列字段。
    return write_queue_->PersistJobAndWake(job, request != nullptr ? request->title : std::string());
}

MemoryWorkerWake ProjectMemory::EnsureWorkerRunning() const {
    return write_queue_->EnsureWorkerRunning();
}

MemoryWorkerWake ProjectMemory::WakePendingWorker() const { return EnsureWorkerRunning(); }

bool ProjectMemory::HasRunningWorker() const { return write_queue_->HasRunningWorker(); }

std::uint64_t ProjectMemory::WorkerSpawnCount() const { return write_queue_->WorkerSpawnCount(); }

void ProjectMemory::WaitForWorkersGracefully(int grace_ms) const {
    write_queue_->WaitForWorkersGracefully(grace_ms);
}

// ---- 任务台账与重试(修复单 §五 C:/memory jobs) ----

std::vector<ProjectMemory::MemoryJobInfo> ProjectMemory::ListWorkspaceJobs() const {
    return write_queue_->ListWorkspaceJobs(identity_.workspace_key);
}

std::expected<std::string, std::string> ProjectMemory::RetryFailedJob(const std::string& job_id) {
    return write_queue_->RetryFailedJob(identity_.workspace_key, job_id);
}

// ---- 提交回执消费(修复单 §五 C 尾) ----

bool ProjectMemory::WakeNeededForWrites() const { return write_queue_->WakeNeededForWrites(); }

std::vector<ProjectMemory::MemoryWriteCompletion> ProjectMemory::DrainWriteCompletions() {
    return write_queue_->DrainWriteCompletions();
}

std::vector<MemoryEntry> ProjectMemory::ListEntries(std::string* error) const {
    std::vector<MemoryEntry> out;
    for (const auto& entry : store::LoadCatalog(memory_dir_, error)) out.push_back(entry.public_entry);
    return out;
}

std::vector<MemoryEntry> ProjectMemory::ListUserEntries(std::string* error) const {
    std::vector<MemoryEntry> out;
    if (!options_.user_enabled) return out;
    for (const auto& entry : store::LoadCatalog(user_memory_dir(), error, "user")) {
        out.push_back(entry.public_entry);
    }
    return out;
}

std::vector<MemoryEntry> ProjectMemory::ListGlobalEntriesForManagement(std::string* error) const {
    // P0-4:管理读口不吃召回授权——用户看自己的全局库不需要开召回。
    std::vector<MemoryEntry> out;
    for (const auto& entry : store::LoadCatalog(user_memory_dir(), error, "user")) {
        out.push_back(entry.public_entry);
    }
    return out;
}

RuntimeStatus ProjectMemory::Status() const {
    RuntimeStatus status;
    status.global_allowed = options_.global_allowed;
    status.enabled = options_.enabled;
    status.use = use_enabled();
    status.generate = generate_enabled();
    status.user_enabled = options_.user_enabled;
    status.learn = LearnModeName(options_.learn);
    status.workspace_key = identity_.workspace_key;
    status.memory_dir = memory_dir_;
    if (options_.user_enabled) {
        status.user_memory_dir = user_memory_dir();
        status.user_entry_count = ListUserEntries().size();
    }
    status.entry_count = ListEntries().size();
    status.pending_candidates = ListCandidates().size();
    std::error_code ec;
    fs::directory_iterator it(home_lubancode_ / "memory-jobs" / "pending", ec);
    if (!ec) {
        for (const auto& item : it) {
            if (!item.is_regular_file(ec) || item.path().extension() != ".json") continue;
            try {
                const auto job = nlohmann::json::parse(ReadFile(item.path()));
                if (job.is_object() && job.value("workspace_key", std::string()) == identity_.workspace_key) {
                    ++status.pending_jobs;
                }
            } catch (const nlohmann::json::exception&) {
                // 坏 job 留给 worker 挪进 failed；状态页不把别的项目或坏文件算进来。
            }
        }
    }
    ec.clear();
    fs::directory_iterator failed_it(home_lubancode_ / "memory-jobs" / "failed", ec);
    if (!ec) {
        for (const auto& item : failed_it) {
            if (!item.is_regular_file(ec) || item.path().extension() != ".json") continue;
            try {
                const auto job = nlohmann::json::parse(ReadFile(item.path()));
                if (job.is_object() && job.value("workspace_key", std::string()) == identity_.workspace_key) {
                    ++status.failed_jobs;
                }
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    return status;
}

// ---- 候选审阅箱(授权与入队编排在门面,文件与账本住 candidate_store) ----

std::vector<MemoryCandidate> ProjectMemory::ListCandidates() const {
    return candidates::ListCandidates(memory_dir_);
}

std::optional<MemoryCandidate> ProjectMemory::GetCandidate(const std::string& id) const {
    return candidates::GetCandidate(memory_dir_, id);
}

std::expected<std::string, std::string> ProjectMemory::AddCandidate(MemoryCandidate candidate) {
    if (!generate_enabled()) return std::unexpected("本场记忆学习未开启");
    return candidates::AddCandidate(memory_dir_, std::move(candidate));
}

std::expected<MemoryEnqueueResult, std::string> ProjectMemory::AcceptCandidate(const std::string& id) {
    // 记忆写入调度单 P0:accept 的早失败也投 candidate_accept 回执
    //(rejected + 稳定码);排队成功经 EnqueueSave 投(source=accept)。
    const auto reject_receipt = [&](const std::string& error) {
        MemoryWriteReceipt receipt;
        receipt.source = MemoryWriteSource::CandidateAccept;
        receipt.operation = "upsert";
        receipt.outcome = MemoryWriteReceiptOutcome::Rejected;
        receipt.layer = "project";
        receipt.error_code = StableWriteErrorCode(error);
        if (write_receipt_sink_ != nullptr) write_receipt_sink_->OnMemoryWriteReceipt(receipt);
    };
    if (!generate_enabled()) {
        reject_receipt("本场记忆学习未开启");
        return std::unexpected("本场记忆学习未开启");
    }
    auto candidate = GetCandidate(id);
    if (!candidate.has_value()) {
        const std::string error = "找不到候选: " + id;
        reject_receipt(error);
        return std::unexpected(error);
    }

    // inferred 不准入正式库(规格:inferred 只进候选区)。
    if (candidate->confidence == "inferred") {
        const std::string error = "候选置信度是 inferred,先 /memory edit 改实或直接 reject";
        reject_receipt(error);
        return std::unexpected(error);
    }
    // feedback 只收用户明说的纠正(规格:模型推断不得直写 feedback)。
    if (candidate->kind == MemoryKind::Feedback && candidate->confidence != "user-stated") {
        const std::string error = "feedback 候选只收用户明说的纠正(confidence 须为 user-stated)";
        reject_receipt(error);
        return std::unexpected(error);
    }
    // fact 须有可核验证据。
    if (candidate->kind == MemoryKind::Fact && candidate->paths.empty()) {
        const std::string error = "fact 候选缺证据路径,先 /memory edit 补 paths 或直接 reject";
        reject_receipt(error);
        return std::unexpected(error);
    }

    SaveRequest request;
    request.kind = candidate->kind;
    request.title = candidate->title;
    request.summary = candidate->summary;
    request.content = candidate->content;
    request.keywords = candidate->keywords;
    request.paths = candidate->paths;
    request.confidence = candidate->confidence;
    request.occurred_at = candidate->occurred_at;
    request.source_session = source_session_;
    // 候选的 paths 同时充当证据(schema 2:fact 须有可核验证据)。
    for (const std::string& path : candidate->paths) {
        request.evidence.push_back(MemoryEvidence{path, std::string()});
    }
    auto queued = EnqueueSave(request, /*user_initiated=*/false,
                              MemoryWriteSource::CandidateAccept);
    if (!queued.has_value()) return queued;

    candidates::RemoveCandidateFile(memory_dir_, id);
    return queued;
}

std::expected<void, std::string> ProjectMemory::EditCandidate(const std::string& id, const std::string& title,
                                                              const std::string& content) {
    return candidates::EditCandidate(memory_dir_, id, title, content);
}

std::expected<void, std::string> ProjectMemory::RejectCandidate(const std::string& id, std::string reason) {
    return candidates::RejectCandidate(memory_dir_, id, std::move(reason));
}

void ProjectMemory::SetRetrievalHints(std::vector<std::string> hints) {
    retrieval_hints_ = std::move(hints);
}

// ---- show/open 与编辑会话(存储件住 topic_store,编辑器交互住 editor) ----

std::expected<std::pair<std::string, fs::path>, std::string> ProjectMemory::ReadTopicForShow(
    const std::string& id) const {
    return store::ReadTopicForShow(memory_dir_, user_memory_dir(), id);
}

std::expected<ProjectMemory::TopicEditSession, std::string> ProjectMemory::BeginTopicEdit(
    const std::string& id) const {
    return store::BeginTopicEdit(memory_dir_, user_memory_dir(), id);
}

std::expected<void, std::string> ProjectMemory::CommitTopicEdit(const TopicEditSession& session) const {
    return store::CommitTopicEdit(session);
}

std::expected<void, std::string> ProjectMemory::EditTopicInEditor(const std::string& id) const {
    auto session = BeginTopicEdit(id);
    if (!session.has_value()) return std::unexpected(session.error());

    // 编辑器的选择与进程交互走外部管理适配(editor_adapter):门面不再
    // 直接起编辑器进程。
    const std::string program = editor::PickEditorProgram();
    const auto ran = editor::RunEditorForFile(program, session->scratch);
    if (ran.spawn_failed || ran.timed_out) {
        std::error_code ec;
        fs::remove(session->scratch, ec);
        return std::unexpected(ran.timed_out ? "编辑器超时未退出,原件未动"
                                             : "编辑器没跑起来($VISUAL/$EDITOR): " + ran.spawn_error);
    }
    return CommitTopicEdit(*session);
}

std::expected<void, std::string> ProjectMemory::OpenIndexInEditor() const {
    const std::string program = editor::PickEditorProgram();
    const fs::path index = memory_dir_ / "index.md";
    std::error_code ec;
    if (!fs::exists(index, ec)) {
        auto built = RebuildMemoryIndex(memory_dir_);
        if (!built.has_value()) return built;
    }
    const auto ran = editor::RunEditorForFile(program, index);
    if (ran.spawn_failed) {
        return std::unexpected("编辑器没跑起来($VISUAL/$EDITOR): " + ran.spawn_error);
    }
    if (ran.timed_out) return std::unexpected("编辑器超时未退出");
    // index 是派生物,手改只为眼看;顺手重建一次,让它跟主题对齐。
    return RebuildMemoryIndex(memory_dir_);
}

ProjectMemory::MigrationPlan ProjectMemory::PlanMigration() const {
    return store::PlanSchemaMigration(memory_dir_);
}

std::expected<ProjectMemory::MigrationResult, std::string> ProjectMemory::RunMigration() const {
    return store::RunSchemaMigration(memory_dir_);
}

// P0-4:全局记忆目录的健康自检(/doctor memory 的引擎体)。
std::vector<std::string> CheckGlobalMemoryHealth(const fs::path& home_lubancode) {
    // §9.3/P0-4:只读为主,复紧幂等。
    std::vector<std::string> lines;
    const fs::path home = AbsoluteNormal(home_lubancode);
    if (home.empty()) {
        lines.push_back("[!!] 找不到 LubanCode 主目录,全局记忆无从检查");
        return lines;
    }
    const fs::path user_root = home / "memory" / "user";
    std::error_code ec;
    if (!fs::exists(user_root, ec)) {
        lines.push_back("[ok] 全局记忆目录尚未创建(还没有全局主题)");
    } else {
        if (trajectory::ContainsSymlinkOrReparse(home, user_root)) {
            lines.push_back("[!!] 全局记忆路径上夹着 symlink/reparse,可能越出主目录: " +
                            PathUtf8(user_root));
        } else if (!trajectory::IsContainedCanonicalPath(user_root, home)) {
            lines.push_back("[!!] 全局记忆目录越出主目录: " + PathUtf8(user_root));
        } else {
            lines.push_back("[ok] 全局记忆目录在主目录内,无 symlink 逃逸");
        }
        // user-only 复紧(POSIX 0700 / Windows PROTECTED DACL):设不住大声报。
        if (fs::is_directory(user_root, ec) && !trajectory::HardenDirectoryUserOnly(user_root)) {
            lines.push_back("[! ] 全局记忆目录收紧 user-only 失败,请检查文件系统权限: " +
                            PathUtf8(user_root));
        } else {
            lines.push_back("[ok] 全局记忆目录 user-only 权限已核(0700/PROTECTED DACL)");
        }
    }
    std::size_t failed_jobs = 0;
    fs::directory_iterator failed_it(home / "memory-jobs" / "failed", ec);
    if (!ec) {
        for (const auto& item : failed_it) {
            if (item.is_regular_file(ec) && item.path().extension() == ".json") ++failed_jobs;
        }
    }
    if (failed_jobs == 0) {
        lines.push_back("[ok] memory job 无失败积压");
    } else {
        lines.push_back("[! ] memory job 失败积压 " + std::to_string(failed_jobs) +
                        " 笔(memory-jobs/failed,各带 .error.txt 回执)");
    }
    // SV-01:所有权裁决隔离的陈锁/旧格式锁留证(memory-jobs 下的
    // worker.lock.stale-*)。有数 = 近期有 worker 暴毙或旧版本残留,值得
    // 看一眼再手清。
    std::size_t quarantined_locks = 0;
    ec.clear();
    fs::directory_iterator stale_it(home / "memory-jobs", ec);
    if (!ec) {
        for (const auto& item : stale_it) {
            if (PathUtf8(item.path().filename()).starts_with("worker.lock.stale-")) ++quarantined_locks;
        }
    }
    if (quarantined_locks == 0) {
        lines.push_back("[ok] memory worker 锁无隔离残迹");
    } else {
        lines.push_back("[! ] memory worker 锁隔离残迹 " + std::to_string(quarantined_locks) +
                        " 处(worker.lock.stale-*;worker 暴毙或旧版残留的留证,可查后手删)");
    }
    // 修复单 §五 C:doctor 也报待写账与 worker 事故——全只读,不拉进程。
    const std::size_t pending_jobs = queue::CountPendingJobs(home);
    if (pending_jobs == 0) {
        lines.push_back("[ok] memory job 无待写积压");
    } else {
        lines.push_back("[! ] memory job 待写 " + std::to_string(pending_jobs) +
                        " 笔(跨工作区;/memory jobs 按工作区看明细)");
    }
    for (const std::string& incident : queue::MemorySupervisorDiagnostics()) {
        lines.push_back(incident);
    }
    return lines;
}

}  // namespace lubancode::memory
