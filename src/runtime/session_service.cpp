// SessionService 的实现(AppServer 接入 Session v3 第一棒)。
//
// 本文件是"搬运收敛":开张折算(身份裁决 + Options 组装)原样收编三处
// 重复装配(终端 interactive_session_assembly、one_shot、app_server
// HandleThreadStart),语义一个字不改;输入接纳与操作台账是持久受理的
// 共用底座(总装单 V0 §六 受理次序):原件先落稳 → accepted 行落稳
//(PowerLoss 档) → 回执;dispatched 先落稳再出队;重启沿完整来源链
// 种去重表、重建未派发 pending。三端(CLI/one-shot/app-server/Gateway)
// 同一条 SubmitInput 链,不各端各补。

#include "runtime/session_service.hpp"

#include "platform/bounded_read.hpp"
#include "platform/owned_file_path.hpp"
#include "trajectory/v3/reader.hpp"

#include <limits>
#include <map>
#include <set>

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "config/config.hpp"  // HomeLubancodeDir:身份裁决的全局件止步
#include "platform/atomic_write.hpp"  // 原件原子写(ProcessCrashDurability=fsync 档)
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/command_service.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/loop_scheduler.hpp"
#include "tools/path_utils.hpp"  // Utf8ToPath/PathToUtf8
#include "trajectory/journal.hpp"  // JournalWriter:PowerLoss 耐久原语
#include "trajectory/v3/session_switch.hpp"  // FindV3SessionStream:开关结果识别

namespace lubancode::runtime {

namespace {

constexpr const char* kOperationsFileName = "operations.jsonl";
constexpr const char* kInputArtifactsDir = "operations-inputs";
// 来源链追溯深度帽:正常 resume 链远短于此;盘损/手工构造的坏环在此
// 截断,不无限绕。
constexpr int kMaxResumeChainDepth = 64;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 台账行的内存形状(读侧统一装载;v1 旧行无 input_ref)。
struct LedgerLine {
    std::string kind;
    std::string operation_id;
    std::string input_id;
    std::string client_operation_id;
    std::string payload_hash;
    std::string input_ref;             // v2 行:相对会话目录的原件路径
    std::string origin_session_id;     // 重落行:来源场 id(审计链)
    std::string origin_operation_id;   // 重落行:来源场操作号
};

// 从一场的轨迹主账追它的 resume 来源(resume.source.attached:v2 场在
// main.jsonl,v3 场在 <sessionId>.jsonl)。先按子串筛行再 parse(全量
// 逐行 parse 太重;kind 值是行内必含的规范字节)。读不出/无来源回空。
std::string ResumeSourceOf(const std::filesystem::path& session_dir,
                           const std::string& session_id) {
    std::filesystem::path stream = session_dir / "main.jsonl";
    if (!std::filesystem::exists(stream)) {
        stream = session_dir / (session_id + ".jsonl");
    }
    std::error_code ec;
    if (session_id.empty() || !std::filesystem::exists(stream, ec) || ec) {
        return std::string();
    }
    std::ifstream in(stream, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    std::string text;
    while (std::getline(in, text)) {
        if (text.find("resume.source.attached") == std::string::npos) {
            continue;
        }
        const nlohmann::json line = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!line.is_object()) {
            continue;
        }
        // v2 账:payload.source_session_id;v3 账:payload.sourceRef.sessionId
        //(五键跨场引用,§3.1)。json 缺键断言纪律:一律 contains()。
        if (line.contains("payload") && line["payload"].is_object()) {
            const nlohmann::json& payload = line["payload"];
            if (payload.contains("source_session_id") && payload["source_session_id"].is_string()) {
                return payload["source_session_id"].get<std::string>();
            }
            if (payload.contains("sourceRef") && payload["sourceRef"].is_object() &&
                payload["sourceRef"].contains("sessionId") &&
                payload["sourceRef"]["sessionId"].is_string()) {
                return payload["sourceRef"]["sessionId"].get<std::string>();
            }
        }
    }
    return std::string();
}

// 输入原件的盘上形状(schemaVersion=1):正文与图片整份落稳,受理账行
// 只存引用——"不得只存 payloadHash 而不留可恢复正文"(总装单 §5.1)。
nlohmann::json InputArtifactJson(const std::string& operation_id, const std::string& text,
                                  const std::vector<api::ImageBlock>& images) {
    nlohmann::json artifact = nlohmann::json::object();
    artifact["schemaVersion"] = 1;
    artifact["operationId"] = operation_id;
    artifact["text"] = text;
    nlohmann::json image_array = nlohmann::json::array();
    for (const api::ImageBlock& image : images) {
        image_array.push_back(nlohmann::json{
            {"mediaType", image.media_type},
            {"data", image.data},
            {"filename", image.filename},
            {"width", image.width},
            {"height", image.height},
        });
    }
    artifact["images"] = std::move(image_array);
    return artifact;
}

// 原子写原件(fsync 档:ProcessCrashDurability = 文件 fsync + 目录 fsync,
// 与 JournalWriter 的 PowerLoss 档同强度)。
bool WriteInputArtifactDurable(const std::filesystem::path& path, const std::string& text,
                               const std::vector<api::ImageBlock>& images,
                               const std::string& operation_id) {
    const std::string bytes = InputArtifactJson(operation_id, text, images).dump();
    return platform::AtomicWriteFile(path, bytes,
                                     platform::WriteDurability::ProcessCrashDurability)
        .has_value();
}

// 读原件回队列形状。读不出/坏 JSON 回 nullopt——不猜、不凭空造正文。
std::optional<SessionService::QueuedInput> ReadInputArtifact(const std::filesystem::path& path,
                                                             const std::string& operation_id) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    const nlohmann::json artifact =
        nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>()),
                              nullptr, /*allow_exceptions=*/false);
    if (!artifact.is_object() || !artifact.contains("text") || !artifact["text"].is_string()) {
        return std::nullopt;
    }
    SessionService::QueuedInput queued;
    queued.text = artifact["text"].get<std::string>();
    if (artifact.contains("images") && artifact["images"].is_array()) {
        for (const auto& image : artifact["images"]) {
            if (!image.is_object()) {
                return std::nullopt;
            }
            api::ImageBlock block;
            if (image.contains("mediaType") && image["mediaType"].is_string()) {
                block.media_type = image["mediaType"].get<std::string>();
            }
            if (image.contains("data") && image["data"].is_string()) {
                block.data = image["data"].get<std::string>();
            }
            if (image.contains("filename") && image["filename"].is_string()) {
                block.filename = image["filename"].get<std::string>();
            }
            if (image.contains("width") && image["width"].is_number_integer()) {
                block.width = image["width"].get<int>();
            }
            if (image.contains("height") && image["height"].is_number_integer()) {
                block.height = image["height"].get<int>();
            }
            queued.images.push_back(std::move(block));
        }
    }
    queued.operation_id = operation_id;
    return queued;
}

}  // namespace

// ---------------------------------------------------------------------------
// 操作台账文件:append-only JSONL,一行一操作事实。先账后回执的关键件
// ——SubmitInput 在回执返回前按 PowerLoss 档落稳(fsync/FlushFileBuffers,
// 总装单 §5.3:关键受理要求 PowerLoss 级提交,不只 flush 字样);崩溃
// 窗口里"有账无回执"的重发按台账命中,不重复接纳。行形状:
//   schemaVersion=1(旧,无原件):
//   {"kind":"operation.accepted","operationId":"op-1","inputId":"in-1",
//    "clientOperationId":"...","payloadHash":"...","receivedAtMs":...}
//   schemaVersion=2(现行,纯追加字段,不改旧义):
//   {"schemaVersion":2,"kind":"operation.accepted",...,"inputRef":
//    "operations-inputs/op-1.json",["originSessionId":"...","originOperationId":
//    "op-9",]"receivedAtMs":...}
//   {"schemaVersion":2,"kind":"operation.dispatched","operationId":"op-1",
//    "dispatchedAtMs":...}
// 这是服务层账,不进轨迹主账(§三:新事实进 v3 主账须先补 kind/schema
// 合同,归 2.0 面)。耐久原语复用 trajectory::JournalWriter(§5.3)。
// ---------------------------------------------------------------------------
class SessionService::OperationsFile {
public:
    explicit OperationsFile(const std::filesystem::path& path) : path_(path) {}

    // 惰性建账:开张不造空文件,首笔接纳才落 operations.jsonl(与账本
    // "延迟开卷"同一取向——空场在盘上不留 0 字节残留)。
    bool ok() const { return !path_.empty(); }
    const std::filesystem::path& path() const { return path_; }

    // 落一行并按 PowerLoss 档落稳(账先行:调用方在收到 true 后才许出
    // 回执/入队/取出)。写失败 false 且此后恒 false——broken 传播,受理
    // 面随调用方停住(写盘失败停止受理,不回成功回执)。
    bool Append(const nlohmann::json& line) {
        if (broken_) {
            return false;
        }
        if (!writer_.has_value()) {
            auto opened = trajectory::JournalWriter::Open(path_,
                                                          trajectory::JournalWriter::OpenMode::Append);
            if (!opened.has_value()) {
                broken_ = true;
                return false;
            }
            writer_.emplace(std::move(*opened));
        }
        if (!writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
            broken_ = true;
            writer_.reset();  // broken 句柄弃用,后续 Append 恒 false
            return false;
        }
        return true;
    }

    // Managed new-only route. Actual detailed observations survive failure;
    // never synthesize an append receipt from the legacy bool result.
    std::expected<trajectory::JournalAppendReceipt, std::string> AppendManaged(
        const std::string& line, const std::shared_ptr<trajectory::JournalNativeIoProbe>& probe) {
        if (broken_ || managed_closed_) return std::unexpected("managed.operation.writer_unavailable");
        if (!writer_) {
            auto opened = probe ? trajectory::JournalWriter::OpenWithNativeIoProbe(path_,
                trajectory::JournalWriter::OpenMode::CreateNew, probe) :
                trajectory::JournalWriter::Open(path_, trajectory::JournalWriter::OpenMode::CreateNew);
            if (!opened) { broken_ = true; return std::unexpected(opened.error()); }
            writer_.emplace(std::move(*opened));
        }
        auto receipt = writer_->AppendLineDetailed(line, trajectory::Durability::PowerLoss);
        if (receipt.status != trajectory::JournalAppendStatus::Committed) broken_ = true;
        return receipt;
    }
    trajectory::JournalCloseReceipt CloseManagedDetailed() noexcept {
        managed_closed_ = true;
        if (writer_) return writer_->CloseDetailed();
        return {}; // No native handle was opened. First write failure stays separate.
    }

    // 只读装载(开张种账用):坏行跳过不猜(json 缺键一律 contains())。
    static std::vector<LedgerLine> ReadLines(const std::filesystem::path& path) {
        std::vector<LedgerLine> lines;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return lines;
        }
        std::string text;
        while (std::getline(in, text)) {
            if (text.empty()) {
                continue;
            }
            const nlohmann::json line = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (!line.is_object() || !line.contains("kind") || !line["kind"].is_string()) {
                continue;  // 半截尾行/坏行:跳过,不猜
            }
            LedgerLine parsed;
            parsed.kind = line["kind"].get<std::string>();
            const auto get_string = [&](const char* key) {
                return line.contains(key) && line[key].is_string() ? line[key].get<std::string>()
                                                                   : std::string();
            };
            parsed.operation_id = get_string("operationId");
            parsed.input_id = get_string("inputId");
            parsed.client_operation_id = get_string("clientOperationId");
            parsed.payload_hash = get_string("payloadHash");
            parsed.input_ref = get_string("inputRef");
            parsed.origin_session_id = get_string("originSessionId");
            parsed.origin_operation_id = get_string("originOperationId");
            lines.push_back(std::move(parsed));
        }
        return lines;
    }

private:
    bool managed_closed_ = false;
    std::filesystem::path path_;
    std::optional<trajectory::JournalWriter> writer_;
    bool broken_ = false;
};

// ---------------------------------------------------------------------------
// 开张/收口折算(三端共用)
// ---------------------------------------------------------------------------

SessionRuntime::Options SessionService::BuildRuntimeOptions(const SessionLaunchRequest& request) {
    SessionRuntime::Options options;
    options.wire_name = request.wire_name;
    options.start_ts = request.start_ts;
    options.lubancode_version = request.lubancode_version;
    options.approval_mode = request.approval_mode;
    options.trajectory_resume_at_launch = request.resume_at_launch;
    options.trajectory_require_v3_resume = request.require_v3_resume;
    options.trajectory_resume_source_session_id = request.resume_source_session_id;
    options.trajectory_workspaces_root = request.workspaces_root;
    options.trajectory_launch_cwd = request.launch_cwd;
    options.trajectory_one_shot = request.one_shot;
    options.trajectory_training_policy = request.training_policy;
    options.trajectory_v3_system_content = request.v3_system_content;
    options.trajectory_v3_opening_participant = request.v3_opening_participant;
    options.trajectory_memory_capability_factory = request.memory_capability_factory;
    options.trajectory_named_result_factory = request.named_result_factory;
    options.trajectory_recovery_capture = request.recovery_capture;
    options.trajectory_recovery_factory = request.recovery_factory;
    options.trajectory_journal_native_io_probe = request.journal_native_io_probe;
    // 身份:显式递的整份吃;否则按 cwd 四级裁决(commondir→marker→
    // config→cwd),home 递进去做全局件止步——与三端被收编前的原装配
    // 逐句对应(终端/one-shot:current_path;app-server:前端指定 cwd)。
    if (request.workspace_identity.has_value()) {
        options.trajectory_workspace_identity = *request.workspace_identity;
    } else {
        const std::filesystem::path identity_cwd =
            request.cwd_utf8.empty() ? std::filesystem::current_path() : tools::Utf8ToPath(request.cwd_utf8);
        const auto identity_home = config::StateRootDir();
        auto identity = workspace::ResolveWorkspaceIdentity(
            identity_cwd, identity_home.has_value() ? tools::Utf8ToPath(*identity_home)
                                                    : std::filesystem::path());
        if (identity.has_value()) {
            options.trajectory_workspace_identity = std::move(*identity);
        }
        // 裁决失败留空:沿旧例交账本兜底/明败,不在服务里另算一把 key。
    }
    // launch_cwd 三端收口(beta.1 反弹二):终端/app-server 旧装配沿空,
    // v3 session.started 的 launchCwd 与 v2 manifest.launch_cwd 由此缺值,
    // 列表目录列全空。空则按身份裁决起点(=实际启动 cwd)补上——这是
    // 建场时刻的真值,不是事后猜测;显式递的(one_shot)优先不覆盖。
    if (options.trajectory_launch_cwd.empty() && options.trajectory_workspace_identity.valid()) {
        options.trajectory_launch_cwd =
            tools::PathToUtf8(options.trajectory_workspace_identity.launch_cwd);
    }
    return options;
}

trajectory::CloseOutcome SessionService::CloseRuntime(SessionRuntime& runtime, const std::string& reason) {
    trajectory::CloseOutcome outcome;
    if (runtime.trajectory() == nullptr) {
        outcome.error_code = "close.no_active_session";
        outcome.message = "会话账未开,无处封口";
        return outcome;
    }
    return runtime.trajectory()->CloseSession(reason);
}

// ---------------------------------------------------------------------------
// 构造/析构
// ---------------------------------------------------------------------------

SessionService::SessionService(SessionLaunchRequest request) {
    if (request.resume_at_launch && request.journal_native_io_probe) {
        launch_error_ = "session.native_probe_resume_unsupported";
        return;
    }
    auto runtime = std::make_unique<SessionRuntime>(BuildRuntimeOptions(request));
    if (runtime->trajectory() == nullptr) {
        // 开不出账:错误说明原样透传(ledger Open 的错误串,与三端旧装配
        // 读到的同一个字符串),装配层据此失败会话启动。
        launch_error_ = runtime->trajectory_open_error();
        return;
    }
    v3_format_ = trajectory::v3::FindV3SessionStream(runtime->trajectory()->session_dir()).has_value();
    if (runtime->trajectory()->resumed_at_launch()) {
        resume_source_session_id_ = runtime->trajectory()->launch_resume_source_session_id();
    }
    runtime_ = std::move(runtime);

    // 操作台账:开张即持写句柄(会话目录已存在);resume 场顺带把直接
    // 来源场的已接纳操作种进内存表。
    operations_path_ = runtime_->trajectory()->session_dir() / kOperationsFileName;
    operations_file_ = std::make_unique<OperationsFile>(*operations_path_);
    SeedOperationLedger();
}

SessionService::SessionService(SessionLaunchRequest request, trajectory::ManagedSessionDirectory admitted,
                               trajectory::ManagedSessionCreationAudit creation)
    : admission_mode_(SessionAdmissionMode::ManagedStorageOnly) {
    // BuildRuntimeOptions must not take its legacy identity/home fallback.
    if (!request.workspace_identity || !request.workspace_identity->valid() ||
        request.workspaces_root.empty() || !request.workspaces_root.is_absolute()) {
        launch_error_ = "managed.session.invalid_launch_options";
        return;
    }
    auto runtime = std::make_unique<SessionRuntime>(BuildRuntimeOptions(request),
                                                   std::move(admitted), std::move(creation));
    if (runtime->trajectory() == nullptr) {
        launch_error_ = runtime->trajectory_open_error();
        return;
    }
    v3_format_ = runtime->trajectory()->v3_main_writer() != nullptr;
    runtime_ = std::move(runtime);
    // No operations file, SeedOperationLedger, input or execution admission here.
}

SessionService::~SessionService() {
    (void)ShutdownExecution();
    if (runtime_ != nullptr && runtime_->async_tool_runtime() != nullptr &&
        !runtime_->async_tool_runtime()->quiescent()) std::terminate();
    // Profile/tool capture destructors can still borrow service state. Destroy
    // the execution now, before any queue, mutex, operation file or ledger.
    execution_.reset();
}

void SessionService::InitializeExecution(std::unique_ptr<assembly::SessionResources> resources,
                                         agent::AgentProfile&& profile,
                                         std::optional<std::vector<api::Message>> restored_history) {
    // Validation can reject before the candidate consumes resources. Clear the
    // referenced source first, while this parameter still owns every borrow.
    // The guard also covers allocation failure before the constructor starts.
    struct SourceProfileScope {
        agent::AgentProfile& profile;
        ~SourceProfileScope() { ClearExecutionProfileBorrowers(profile); }
    } source_profile_scope{profile};
    if (admission_mode_ == SessionAdmissionMode::ManagedStorageOnly)
        throw std::logic_error(kManagedStorageOnlyError);
    {
        std::lock_guard lock(commit_mutex_);
        if (runtime_ == nullptr) throw std::logic_error("session.execution.session_unavailable");
        if (execution_ != nullptr) throw std::logic_error("session.execution.already_initialized");
        if (execution_shutdown_requested_.load()) throw std::logic_error("session.execution.stopping");
    }
    // Candidate construction/rollback can destroy user captures that query the
    // service. Neither construction nor rejected-candidate/source destruction
    // takes place under its commit mutex.
    auto candidate = std::make_unique<SessionExecution>(std::move(resources), std::move(profile),
                                                       std::move(restored_history));
    {
        std::lock_guard lock(commit_mutex_);
        if (execution_ != nullptr) throw std::logic_error("session.execution.already_initialized");
        if (execution_shutdown_requested_.load()) throw std::logic_error("session.execution.stopping");
        execution_ = std::move(candidate);
    }
}

void SessionService::RequestExecutionShutdown() {
    execution_shutdown_requested_.store(true);
    {
        std::lock_guard lock(commit_mutex_);
        // Wait for an initialization already in progress to observe the latch.
    }
    // Synchronize with first-turn publication without holding a lock while a
    // worker exits. The runtime latches shutdown even if no async owner exists.
    if (runtime_ != nullptr) runtime_->RequestAsyncToolShutdown();
}

bool SessionService::ShutdownExecution() {
    RequestExecutionShutdown();
    return runtime_ == nullptr || runtime_->ShutdownAsyncTools();
}

TrajectorySessionLedger* SessionService::trajectory() {
    return runtime_ != nullptr ? runtime_->trajectory() : nullptr;
}

const TrajectorySessionLedger* SessionService::trajectory() const {
    return runtime_ != nullptr ? runtime_->trajectory() : nullptr;
}

bool SessionService::v3_format() const {
    return v3_format_;
}

void SessionService::SeedOperationLedger() {
    if (operations_file_ == nullptr || !operations_file_->ok()) {
        return;  // 台账开不了:去重退化为内存表(接纳照常,先账后回执如实降级)
    }
    const std::filesystem::path session_dir = runtime_->trajectory()->session_dir();
    const std::string session_id = runtime_->trajectory()->session_id();
    const std::filesystem::path sessions_root = session_dir.parent_path();

    // 一场台账的装载结果:去重表条目 + "accepted 未 dispatched 且原件
    // 可读"的输入(重排/重落的料)+ 已用过的最大 op 号。
    struct LedgerScan {
        std::vector<std::pair<std::string, AcceptedOperation>> dedupe;  // key -> 条目
        struct Revive {
            std::string operation_id;
            std::string input_id;
            std::string payload_hash;
            std::string client_operation_id;
            std::string origin_session_id;    // 空 = 原生行;重落行带来源场
            std::string origin_operation_id;  // 空 = 原生行
            std::filesystem::path artifact;   // 原件绝对路径(那一场的目录下)
        };
        std::vector<Revive> revives;
        std::uint64_t max_op_number = 0;
    };
    // origin_session_id:本扫描目标场的 id——该场账里的"原生行"以此归源;
    // 重落行(origin 字段非空)仍归它自己的上源,不换成扫描场。
    const auto scan_ledger = [](const std::filesystem::path& dir,
                                const std::string& scan_session_id) {
        LedgerScan scan;
        std::unordered_set<std::string> dispatched;
        for (const LedgerLine& line : OperationsFile::ReadLines(dir / kOperationsFileName)) {
            if (line.kind == "operation.accepted") {
                AcceptedOperation accepted;
                accepted.operation_id = line.operation_id;
                accepted.input_id = line.input_id;
                accepted.payload_hash = line.payload_hash;
                accepted.input_ref = line.input_ref;
                // "op-<n>" 尾号解析:非数字尾(外来形状)不参与发号续接。
                const auto prefix = line.operation_id.rfind("op-");
                if (prefix != std::string::npos && prefix + 3 < line.operation_id.size()) {
                    const std::string number = line.operation_id.substr(prefix + 3);
                    if (number.find_first_not_of("0123456789") == std::string::npos) {
                        scan.max_op_number = std::max(
                            scan.max_op_number, static_cast<std::uint64_t>(std::stoull(number)));
                    }
                }
                if (!line.client_operation_id.empty()) {
                    scan.dedupe.emplace_back(line.client_operation_id, std::move(accepted));
                }
                scan.revives.push_back(LedgerScan::Revive{
                    line.operation_id,
                    line.input_id,
                    line.payload_hash,
                    line.client_operation_id,
                    line.origin_session_id.empty() ? scan_session_id : line.origin_session_id,
                    line.origin_operation_id.empty() ? line.operation_id : line.origin_operation_id,
                    // v1 旧行无 input_ref:原件留空 path,由 erase_if 拦下。
                    line.input_ref.empty() ? std::filesystem::path{}
                                           : dir / tools::Utf8ToPath(line.input_ref)});
            } else if (line.kind == "operation.dispatched") {
                dispatched.insert(line.operation_id);
            }
        }
        // 已派发的不重排;无原件的(v1 旧行)不重排——正文不可恢复,如实
        // 降级,不凭空造正文(去重表照种)。
        std::erase_if(scan.revives, [&](const LedgerScan::Revive& revive) {
            return dispatched.count(revive.operation_id) > 0 || revive.artifact.empty();
        });
        return scan;
    };
    // 同源归一键:一笔原始输入 = (原生场, 原生操作号)。重落行与它指向的
    // 原生行折成同一键——先到先得,后场跳过。
    const auto origin_key_of = [](const LedgerScan::Revive& revive) {
        return revive.origin_session_id + "/" + revive.origin_operation_id;
    };

    // 1) 本场台账(防御:同一进程重开同一场、或手工把账放进新场目录)。
    //    本场的未派发直接重排;本场已重落过的源不再重落第二次。
    const LedgerScan own = scan_ledger(session_dir, session_id);
    operation_counter_ = own.max_op_number;
    std::unordered_set<std::string> revived_origins;
    for (auto& [key, accepted] : own.dedupe) {
        if (operations_.count(key) == 0) {
            operations_.emplace(std::move(key), std::move(accepted));
        }
    }
    for (const auto& revive : own.revives) {
        if (!revive.origin_session_id.empty() && revive.origin_session_id != session_id) {
            revived_origins.insert(origin_key_of(revive));  // 本场重落过的上源
            continue;  // 重落行的原生输入若链上还会遇到,已由本场的这份代表
        }
        if (auto queued = ReadInputArtifact(revive.artifact, revive.operation_id)) {
            pending_inputs_.push_back(std::move(*queued));
        }
    }

    // 2) 沿完整来源链(直接来源 → 其来源 → …)由近及远:种去重表 + 收
    //    集未派发输入。多跳,不是只读上一场(§4.2"恢复沿来源链识别
    //    原键";总装单 §5.2"多次 resume 沿完整来源链查去重")。访问集 +
    //    深度帽防坏环。
    std::vector<LedgerScan::Revive> chain_revives;
    std::string cursor = resume_source_session_id_;
    std::unordered_set<std::string> visited{session_id};
    int depth = 0;
    while (!cursor.empty() && visited.count(cursor) == 0 && depth < kMaxResumeChainDepth) {
        visited.insert(cursor);
        const std::filesystem::path source_dir = sessions_root / tools::Utf8ToPath(cursor);
        const LedgerScan scan = scan_ledger(source_dir, cursor);
        for (auto& [key, accepted] : scan.dedupe) {
            if (operations_.count(key) == 0) {
                operations_.emplace(std::move(key), std::move(accepted));
            }
        }
        for (const auto& revive : scan.revives) {
            if (revived_origins.count(origin_key_of(revive)) == 0) {
                chain_revives.push_back(revive);
                revived_origins.insert(origin_key_of(revive));
            }
        }
        cursor = ResumeSourceOf(source_dir, cursor);
        ++depth;
    }

    // 3) 链上收集的未派发输入在本场重落(原件 + accepted 行,PowerLoss
    //    档,带 origin 审计)再排进 pending——重落后本场账自足,再崩再
    //    恢复时本场一跳即得,不依赖跨场对账。
    for (const auto& revive : chain_revives) {
        auto queued = ReadInputArtifact(revive.artifact, revive.operation_id);
        if (!queued.has_value()) {
            continue;  // 原件丢失/坏:不重排(已提交原件丢失属隔离案,
                       // 不跳过继续执行,归恢复对账;这里如实不排)
        }
        const std::string operation_id = "op-" + std::to_string(operation_counter_ + 1);
        const std::string input_id = "in-" + std::to_string(operation_counter_ + 1);
        const std::string input_ref =
            std::string(kInputArtifactsDir) + "/" + operation_id + ".json";
        if (!WriteInputArtifactDurable(session_dir / tools::Utf8ToPath(input_ref), queued->text,
                                       queued->images, operation_id)) {
            continue;  // 原件落不稳:不重排(写盘失败不回成功语义,下一场
                       // 构造重扫再试;去重表已种,不丢意图)
        }
        nlohmann::json line{{"schemaVersion", 2},
                            {"kind", "operation.accepted"},
                            {"operationId", operation_id},
                            {"inputId", input_id},
                            {"clientOperationId", revive.client_operation_id},
                            {"payloadHash", revive.payload_hash},
                            {"inputRef", input_ref},
                            {"originSessionId", revive.origin_session_id},
                            {"originOperationId", revive.origin_operation_id},
                            {"receivedAtMs", NowMs()}};
        if (!operations_file_->Append(line)) {
            continue;  // 账行落不稳:受理面已停(broken 传播),不重排
        }
        ++operation_counter_;
        if (!revive.client_operation_id.empty()) {
            AcceptedOperation accepted;
            accepted.operation_id = operation_id;
            accepted.input_id = input_id;
            accepted.payload_hash = revive.payload_hash;
            accepted.input_ref = input_ref;
            operations_.emplace(revive.client_operation_id, std::move(accepted));
        }
        queued->operation_id = operation_id;
        pending_inputs_.push_back(std::move(*queued));
    }
}

// ---------------------------------------------------------------------------
// 输入接纳(§4.1/§4.2 最小面)
// ---------------------------------------------------------------------------

std::string SessionService::CanonicalInputPayload(const InputRequest& input) {
    std::string canonical = input.text;
    for (const api::ImageBlock& image : input.images) {
        canonical += '\x1f';
        canonical += image.media_type;
        canonical += '\x1f';
        canonical += image.filename;
        canonical += '\x1f';
        canonical += std::to_string(image.width);
        canonical += '\x1f';
        canonical += std::to_string(image.height);
        canonical += '\x1f';
        canonical += image.data;
    }
    return canonical;
}

SessionService::InputReceipt SessionService::SubmitInput(const InputRequest& input) {
    InputReceipt receipt;
    if (admission_mode_ == SessionAdmissionMode::ManagedStorageOnly) {
        receipt.error_code = kManagedStorageOnlyError;
        return receipt;
    }
    if (runtime_ == nullptr || trajectory() == nullptr) {
        receipt.error_code = "trajectory.open_failed";
        return receipt;
    }
    const std::string payload_hash = platform::Sha256Hex(CanonicalInputPayload(input));
    std::lock_guard<std::mutex> lock(commit_mutex_);
    if (execution_shutdown_requested_.load()) {
        receipt.error_code = "session.stopping";
        return receipt;
    }
    // 幂等(§4.2):同键同载荷返回原操作;同键不同载荷 conflict。
    if (!input.client_operation_id.empty()) {
        const auto it = operations_.find(input.client_operation_id);
        if (it != operations_.end()) {
            if (it->second.payload_hash == payload_hash) {
                receipt.duplicate = true;
                receipt.operation_id = it->second.operation_id;
                receipt.input_id = it->second.input_id;
                receipt.payload_hash = payload_hash;
                return receipt;
            }
            receipt.error_code = "operation_conflict";
            receipt.payload_hash = payload_hash;
            return receipt;
        }
    }
    if (pending_inputs_.size() >= kMaxPendingInputs) {
        receipt.error_code = "queue_full";
        return receipt;
    }
    // 受理次序(总装单 §六):原件先落稳 → accepted 行落稳(PowerLoss 档)
    // → 回执。任何一步落不稳即拒绝受理,不回成功——"Append 失败仍成功"
    // 的旧病就此拔除;OperationsFile broken 后受理面持续拒绝(写盘失败
    // 停止受理)。
    const std::string operation_id = "op-" + std::to_string(operation_counter_ + 1);
    const std::string input_id = "in-" + std::to_string(operation_counter_ + 1);
    const std::string input_ref =
        std::string(kInputArtifactsDir) + "/" + operation_id + ".json";
    if (operations_file_ != nullptr && operations_file_->ok()) {
        if (!WriteInputArtifactDurable(runtime_->trajectory()->session_dir() /
                                           tools::Utf8ToPath(input_ref),
                                       input.text, input.images, operation_id)) {
            receipt.error_code = "operation.artifact_failed";
            receipt.payload_hash = payload_hash;
            return receipt;  // 原件落不稳:不发号不入队。孤立残留(若原子
                             // 写中途失败已被清理)按"原件可回收"处置。
        }
        nlohmann::json line{{"schemaVersion", 2},
                            {"kind", "operation.accepted"},
                            {"operationId", operation_id},
                            {"inputId", input_id},
                            {"clientOperationId", input.client_operation_id},
                            {"payloadHash", payload_hash},
                            {"inputRef", input_ref},
                            {"receivedAtMs", NowMs()}};
        if (!operations_file_->Append(line)) {
            receipt.error_code = "operation.append_failed";
            receipt.payload_hash = payload_hash;
            return receipt;  // 账行落不稳:不入队、不种表、不回成功。
                             // 孤立原件(op-<n>.json)留待同号重试覆盖或回收。
        }
    }
    ++operation_counter_;
    if (!input.client_operation_id.empty()) {
        AcceptedOperation accepted;
        accepted.operation_id = operation_id;
        accepted.input_id = input_id;
        accepted.payload_hash = payload_hash;
        accepted.input_ref = input_ref;
        operations_.emplace(input.client_operation_id, std::move(accepted));
    }
    // 后回执:入队 + 出回执。
    QueuedInput queued;
    queued.text = input.text;
    queued.images = input.images;
    queued.operation_id = operation_id;
    pending_inputs_.push_back(std::move(queued));
    receipt.accepted = true;
    receipt.operation_id = operation_id;
    receipt.input_id = input_id;
    receipt.payload_hash = payload_hash;
    return receipt;
}

SessionService::PendingPop SessionService::PopPendingInput() {
    PendingPop pop;
    if (admission_mode_ == SessionAdmissionMode::ManagedStorageOnly) {
        pop.status = PendingPop::Status::NotAdmitted;
        pop.error_code = kManagedStorageOnlyError;
        return pop;
    }
    std::lock_guard<std::mutex> lock(commit_mutex_);
    if (pending_inputs_.empty()) {
        pop.status = PendingPop::Status::Empty;
        return pop;
    }
    QueuedInput front = pending_inputs_.front();  // 先拷贝:落账失败不动队
    if (operations_file_ != nullptr && operations_file_->ok()) {
        nlohmann::json line{{"schemaVersion", 2},
                            {"kind", "operation.dispatched"},
                            {"operationId", front.operation_id},
                            {"dispatchedAtMs", NowMs()}};
        if (!operations_file_->Append(line)) {
            pop.status = PendingPop::Status::WriteFailed;  // 留在队首,停泵
            return pop;
        }
    }
    pending_inputs_.pop_front();
    pop.status = PendingPop::Status::Ok;
    pop.input = std::move(front);
    return pop;
}

std::size_t SessionService::pending_input_count() const {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    return pending_inputs_.size();
}

std::vector<SessionService::QueuedInput> SessionService::PendingInputsSnapshot() const {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    return {pending_inputs_.begin(), pending_inputs_.end()};
}

bool SessionService::RecordTurnFinal(const TurnFinalRecord& record) {
    if (admission_mode_ == SessionAdmissionMode::ManagedStorageOnly) return false;
    if (runtime_ == nullptr || trajectory() == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(commit_mutex_);
    if (operations_file_ == nullptr || !operations_file_->ok()) {
        return false;
    }
    // 行形状(schemaVersion=2 纯追加):operationId 是本终态归属的操作;
    // 空操作号的防御路径不落账(没经接纳的回合没有对账面,不伪造)。
    if (record.operation_id.empty()) {
        return false;
    }
    nlohmann::json refs = nlohmann::json::array();
    for (const std::string& ref : record.final_message_refs) {
        refs.push_back(ref);
    }
    nlohmann::json line{{"schemaVersion", 2},
                        {"kind", "operation.final"},
                        {"operationId", record.operation_id},
                        {"turnId", record.turn_id},
                        {"executionStatus", record.execution_status},
                        {"finalMessageRefs", std::move(refs)},
                        {"usageReported", record.usage_reported},
                        {"finalizedAtMs", NowMs()}};
    return operations_file_->Append(line);
}

std::vector<SessionService::OperationFact> SessionService::ReadOperationFacts(
    const std::filesystem::path& session_dir) {
    std::vector<OperationFact> facts;
    std::ifstream in(session_dir / kOperationsFileName, std::ios::binary);
    if (!in.is_open()) {
        return facts;  // 没建过账(空场)或目录不在:空表,调用方按 not_found 口径处理
    }
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) {
            continue;
        }
        const nlohmann::json line = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!line.is_object() || !line.contains("kind") || !line["kind"].is_string()) {
            continue;  // 半截尾行/坏行:跳过,不猜
        }
        OperationFact fact;
        fact.kind = line["kind"].get<std::string>();
        const auto get_string = [&](const char* key) {
            return line.contains(key) && line[key].is_string() ? line[key].get<std::string>()
                                                               : std::string();
        };
        fact.operation_id = get_string("operationId");
        fact.input_id = get_string("inputId");
        fact.client_operation_id = get_string("clientOperationId");
        fact.payload_hash = get_string("payloadHash");
        fact.turn_id = get_string("turnId");
        fact.execution_status = get_string("executionStatus");
        const auto get_ms = [&](const char* key) {
            return line.contains(key) && line[key].is_number_integer()
                       ? line[key].get<std::int64_t>()
                       : std::int64_t{0};
        };
        fact.received_at_ms = get_ms("receivedAtMs");
        fact.dispatched_at_ms = get_ms("dispatchedAtMs");
        fact.finalized_at_ms = get_ms("finalizedAtMs");
        fact.usage_reported = line.contains("usageReported") && line["usageReported"].is_boolean()
                                  ? line["usageReported"].get<bool>()
                                  : false;
        if (line.contains("finalMessageRefs") && line["finalMessageRefs"].is_array()) {
            for (const auto& ref : line["finalMessageRefs"]) {
                if (ref.is_string()) {
                    fact.final_message_refs.push_back(ref.get<std::string>());
                }
            }
        }
        facts.push_back(std::move(fact));
    }
    return facts;
}

std::expected<std::vector<SessionService::OperationFact>, std::string>
SessionService::ReadOperationFactsOwned(const std::string& bytes) {
    if (bytes.find('\0') != std::string::npos || !platform::IsValidUtf8(bytes))
        return std::unexpected("recovery.operations_invalid:invalid_text");
    auto raw = trajectory::RecoveryStreamLines(bytes, std::nullopt);
    if (!raw) return std::unexpected("recovery.operations_invalid:" + raw.error());
    enum class Stage { Accepted, Dispatched, Final };
    std::map<std::string, Stage> stages;
    std::set<std::string> client_keys, bound_turns;
    std::vector<OperationFact> facts;
    for (const auto& text : *raw) {
        const auto line = nlohmann::json::parse(text, nullptr, false);
        const auto bad = [] { return std::unexpected(std::string("recovery.operations_invalid")); };
        if (!line.is_object() || !line.contains("schemaVersion") ||
            (line["schemaVersion"] != 1 && line["schemaVersion"] != 2) ||
            !line["schemaVersion"].is_number_integer()) return bad();
        const auto string = [&](const char* key, bool required) {
            const auto value = line.find(key);
            return value == line.end() ? !required : value->is_string();
        };
        const auto integer = [&](const char* key) {
            const auto value = line.find(key);
            if (value == line.end()) return false;
            return value->is_number_unsigned()
                ? value->get<std::uint64_t>() <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
                : value->is_number_integer() && value->get<std::int64_t>() >= 0;
        };
        if (!string("kind", true) || !string("operationId", true)) return bad();
        OperationFact fact;
        fact.kind = line["kind"].get<std::string>();
        fact.operation_id = line["operationId"].get<std::string>();
        if (fact.operation_id.empty() || fact.operation_id.size() > 200 ||
            fact.operation_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) return bad();
        const auto prior = stages.find(fact.operation_id);
        if (fact.kind == "operation.accepted") {
            if (!string("inputId", true) || !string("clientOperationId", true) || !string("payloadHash", true) ||
                !integer("receivedAtMs") || !string("inputRef", line["schemaVersion"] == 2) ||
                !string("originSessionId", false) || !string("originOperationId", false)) return bad();
            fact.input_id = line["inputId"].get<std::string>();
            fact.client_operation_id = line["clientOperationId"].get<std::string>();
            fact.payload_hash = line["payloadHash"].get<std::string>();
            fact.received_at_ms = line["receivedAtMs"].get<std::int64_t>();
            if (prior != stages.end() || fact.input_id.empty() || fact.payload_hash.size() != 64 ||
                fact.payload_hash.find_first_not_of("0123456789abcdef") != std::string::npos ||
                (!fact.client_operation_id.empty() && !client_keys.insert(fact.client_operation_id).second)) return bad();
            stages.emplace(fact.operation_id, Stage::Accepted);
        } else if (fact.kind == "operation.dispatched") {
            if (!integer("dispatchedAtMs")) return bad();
            if (prior == stages.end() || prior->second != Stage::Accepted) return bad();
            prior->second = Stage::Dispatched;
            fact.dispatched_at_ms = line["dispatchedAtMs"].get<std::int64_t>();
        } else if (fact.kind == "operation.final") {
            if (!string("turnId", true) || !string("executionStatus", true) || !integer("finalizedAtMs") ||
                !line.contains("usageReported") || !line["usageReported"].is_boolean() ||
                !line.contains("finalMessageRefs") || !line["finalMessageRefs"].is_array()) return bad();
            if (prior == stages.end() || prior->second != Stage::Dispatched) return bad();
            prior->second = Stage::Final;
            fact.turn_id = line["turnId"].get<std::string>();
            if (!fact.turn_id.empty() && (fact.turn_id.size() > 200 ||
                fact.turn_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
                !bound_turns.insert(fact.turn_id).second)) return bad();
            fact.execution_status = line["executionStatus"].get<std::string>();
            if (fact.execution_status != "success" && fact.execution_status != "error" &&
                fact.execution_status != "cancelled" && fact.execution_status != "interrupted") return bad();
            fact.usage_reported = line["usageReported"].get<bool>();
            fact.finalized_at_ms = line["finalizedAtMs"].get<std::int64_t>();
            for (const auto& ref : line["finalMessageRefs"]) {
                if (!ref.is_string()) return bad();
                fact.final_message_refs.push_back(ref.get<std::string>());
            }
        } else return bad();
        facts.push_back(std::move(fact));
    }
    return facts;
}

SessionService::OperationLookup SessionService::LookupClientOperation(
    const std::string& client_operation_id) const {
    OperationLookup lookup;
    if (client_operation_id.empty()) {
        return lookup;
    }
    std::lock_guard<std::mutex> lock(commit_mutex_);
    const auto it = operations_.find(client_operation_id);
    if (it == operations_.end()) {
        return lookup;
    }
    lookup.found = true;
    lookup.operation_id = it->second.operation_id;
    lookup.input_id = it->second.input_id;
    lookup.payload_hash = it->second.payload_hash;
    return lookup;
}

std::expected<ManagedOperationMaterials, std::string> SessionService::ManagedScopeLocked() const {
    if (admission_mode_ != SessionAdmissionMode::ManagedStorageOnly || !trajectory())
        return std::unexpected("managed.operation.not_admitted");
    auto publication = trajectory()->managed_publication();
    auto* writer = runtime_->trajectory()->v3_main_writer(); // Const capture of the same owned writer.
    if (!publication || !writer || writer->closed() || writer->broken() ||
        publication->knowledge != trajectory::ManagedSessionOwnershipPublication::Knowledge::Committed ||
        !publication->native || !publication->native->has_value() ||
        publication->native->value().outcome != platform::WriteOutcome::CommittedDurable)
        return std::unexpected("managed.operation.owner_unavailable");
    auto marker = trajectory::CaptureManagedSessionOwnership(trajectory()->session_dir());
    if (!marker || !marker->ownership || *marker->ownership != publication->expected ||
        marker->bytes != publication->publication_bytes)
        return std::unexpected("managed.operation.owner_changed");
    auto captured = writer->CaptureJournal(128 * 1024 * 1024);
    if (!captured) return std::unexpected("managed.operation.main_unavailable");
    auto ledger = trajectory::v3::ReadV3LedgerCaptured(*captured,
        trajectory::RecoveryStreamReadLimits{128 * 1024 * 1024, 262144, 8 * 1024 * 1024});
    auto closed = captured->Close();
    if (!ledger || !closed || ledger->messages.empty() || !ledger->messages.front().system_meta)
        return std::unexpected("managed.operation.main_unavailable");
    const auto& owner = publication->expected;
    const auto& meta = *ledger->messages.front().system_meta;
    if (!meta.contains("managedSession") || !meta["managedSession"].is_object())
        return std::unexpected("managed.operation.owner_changed");
    const auto& managed = meta["managedSession"];
    const auto matches = [&](const char* key, const std::string& value) {
        return managed.contains(key) && managed[key].is_string() && managed[key] == value;
    };
    if (managed.size() != 9 || !managed.contains("schemaVersion") || !managed["schemaVersion"].is_number_integer() || managed["schemaVersion"] != 1 ||
        !matches("mode", "Managed") || !matches("tenantId", owner.tenant_id) ||
        !matches("projectId", owner.project_id) || !matches("workspaceKey", owner.workspace_key) ||
        !matches("sessionId", owner.session_id) || !managed.contains("bindingVersion") ||
        !managed["bindingVersion"].is_number_unsigned() || managed["bindingVersion"] != owner.binding_version ||
        !managed.contains("openingPolicyRevision") || !managed["openingPolicyRevision"].is_number_unsigned() ||
        managed["openingPolicyRevision"].get<std::uint64_t>() == 0 ||
        !managed.contains("creationSubject") || !managed["creationSubject"].is_object() ||
        ledger->session_id != owner.session_id || ledger->run_id != writer->run_id())
        return std::unexpected("managed.operation.owner_changed");
    const auto& creator = managed["creationSubject"];
    if (creator.size() != 4) return std::unexpected("managed.operation.owner_changed");
    for (const auto* key : {"tenantId", "userId", "actorKind", "credentialId"}) {
        if (!creator.contains(key) || !creator[key].is_string()) return std::unexpected("managed.operation.owner_changed");
        const auto value = creator[key].get<std::string>();
        if (value.empty() || value.size() > 512 || !platform::IsValidUtf8(value) ||
            std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            return std::unexpected("managed.operation.owner_changed");
    }
    if (creator["tenantId"] != owner.tenant_id ||
        (creator["actorKind"] != "user" && creator["actorKind"] != "agent" && creator["actorKind"] != "service"))
        return std::unexpected("managed.operation.owner_changed");
    ManagedOperationMaterials result; result.owner = owner; result.run_id = ledger->run_id;
    return result;
}

std::shared_ptr<const SessionService::ManagedWriteReceipt> SessionService::SubmitManagedInput(
    const InputRequest& input, const ManagedOperationAdmission& admission) {
    auto receipt = std::make_shared<ManagedWriteReceipt>();
    receipt->input.error_code = "managed.operation.publication_unconfirmed";
    std::lock_guard lock(commit_mutex_);
    if (admission_mode_ != SessionAdmissionMode::ManagedStorageOnly) {
        receipt->input.error_code = "managed.operation.not_admitted"; return receipt;
    }
    if (managed_closing_ || managed_closed_ || execution_shutdown_requested_.load()) {
        receipt->input.error_code = "session.stopping"; return receipt;
    }
    if (managed_first_failure_) {
        receipt->input.error_code = "managed.operation.storage_unconfirmed"; return receipt;
    }
    bool native_entered = false;
    try {
        auto scope = ManagedScopeLocked();
        if (!scope) { receipt->input.error_code = scope.error(); return receipt; }
        if (input.client_operation_id.empty() || !input.images.empty()) {
            receipt->input.error_code = "managed.operation.invalid_input"; return receipt;
        }
        auto intent = ManagedOperationIntentHash(admission, scope->owner, input.text);
        if (!intent) { receipt->input.error_code = intent.error(); return receipt; }
        const auto prior = operations_.find(input.client_operation_id);
        if (prior != operations_.end()) {
            if (!prior->second.managed || prior->second.managed->provenance.intent_hash != *intent) {
                receipt->input.error_code = "operation_conflict"; return receipt;
            }
            receipt->input = {false, true, {}, prior->second.operation_id, prior->second.input_id, prior->second.payload_hash};
            receipt->operation = prior->second.managed;
            receipt->knowledge = ManagedWriteReceipt::Knowledge::Committed;
            return receipt;
        }
        if (pending_inputs_.size() >= kMaxPendingInputs || operation_counter_ >= kManagedOperationInputEntries) {
            receipt->input.error_code = "queue_full"; return receipt;
        }
        const auto number = operation_counter_ + 1;
        auto prepared = PrepareManagedOperation(admission, scope->owner, scope->run_id,
            input.client_operation_id, input.text, "op-" + std::to_string(number), "in-" + std::to_string(number), NowMs());
        if (!prepared) { receipt->input.error_code = prepared.error(); return receipt; }
        const auto terminal_reserve = (pending_inputs_.size() + 1) * 1024;
        if (prepared->input_bytes.size() > kManagedOperationInputsTotalBytes - managed_input_bytes_ ||
            prepared->accepted_line.size() + 1 + terminal_reserve > kManagedOperationLedgerBytes - managed_ledger_bytes_) {
            receipt->input.error_code = "managed.operation.storage_limit"; return receipt;
        }
        auto stored = std::make_shared<const ManagedStoredOperation>(std::move(prepared->operation));
        receipt->operation = stored;
        receipt->input.operation_id = stored->provenance.operation_id;
        receipt->input.input_id = stored->provenance.input_id;
        receipt->input.payload_hash = stored->provenance.input_hash;
        AcceptedOperation accepted{stored->provenance.operation_id, stored->provenance.input_id,
            stored->provenance.input_hash, stored->input_ref, stored};
        QueuedInput queued; queued.operation_id = stored->provenance.operation_id; queued.managed = stored;
        operations_.reserve(operations_.size() + 1);
        const auto directory = trajectory()->session_dir();
        const auto input_directory = directory / kInputArtifactsDir;
        const auto input_path = directory / tools::Utf8ToPath(stored->input_ref);
        const auto ledger_path = directory / kOperationsFileName;
        if (!platform::IsUnlinkedOwnedPath(directory, input_path) || !platform::IsUnlinkedOwnedPath(directory, ledger_path)) {
            receipt->input.error_code = "managed.operation.path_rejected"; return receipt;
        }
        std::error_code error;
        std::filesystem::create_directory(platform::FileIoPath(input_directory), error);
        if (error || !platform::IsUnlinkedOwnedPath(directory, input_path)) {
            receipt->input.error_code = "managed.operation.path_rejected"; return receipt;
        }
        if (!operations_file_) {
            operations_path_ = ledger_path;
            operations_file_ = std::make_unique<OperationsFile>(ledger_path);
        }
        // Once any native publication begins, this ID is never reused, even if
        // no append witness returns or in-memory publication subsequently throws.
        operation_counter_ = number;
        native_entered = true;
        receipt->knowledge = ManagedWriteReceipt::Knowledge::Unconfirmed;
        receipt->artifact = platform::CreateImmutableFileDetailed(input_path, prepared->input_bytes,
            platform::WriteDurability::ProcessCrashDurability);
        if (!receipt->artifact->ok() || receipt->artifact->outcome != platform::WriteOutcome::CommittedDurable) {
            if (receipt->artifact->outcome == platform::WriteOutcome::NotCommitted)
                receipt->knowledge = ManagedWriteReceipt::Knowledge::Rejected;
            managed_first_failure_ = receipt; return receipt;
        }
        auto appended = operations_file_->AppendManaged(prepared->accepted_line, managed_native_probe_);
        if (!appended) { receipt->knowledge = ManagedWriteReceipt::Knowledge::Rejected; managed_first_failure_ = receipt; return receipt; }
        receipt->append = *appended;
        if (appended->status != trajectory::JournalAppendStatus::Committed ||
            appended->confirmed_durability != trajectory::Durability::PowerLoss) {
            if (appended->status == trajectory::JournalAppendStatus::RejectedBeforeIO)
                receipt->knowledge = ManagedWriteReceipt::Knowledge::Rejected;
            managed_first_failure_ = receipt; return receipt;
        }
        receipt->knowledge = ManagedWriteReceipt::Knowledge::NativeCommittedPublicationGap;
        if (managed_publication_probe_) managed_publication_probe_();
        operations_.emplace(input.client_operation_id, std::move(accepted));
        pending_inputs_.push_back(std::move(queued));
        managed_input_bytes_ += prepared->input_bytes.size();
        managed_ledger_bytes_ += prepared->accepted_line.size() + 1;
        receipt->input.accepted = true; receipt->input.error_code.clear();
        receipt->knowledge = ManagedWriteReceipt::Knowledge::Committed;
    } catch (...) {
        // No allocation or fabricated native facts in this cleanup. In
        // particular a committed append stays committed after a semantic throw.
        if (native_entered) managed_first_failure_ = receipt;
    }
    return receipt;
}

std::shared_ptr<const ManagedStoredOperation> SessionService::ManagedPendingFront() const {
    std::lock_guard lock(commit_mutex_);
    if (admission_mode_ != SessionAdmissionMode::ManagedStorageOnly || managed_closing_ || managed_closed_ ||
        managed_first_failure_ || pending_inputs_.empty()) return {};
    return pending_inputs_.front().managed;
}

std::shared_ptr<const SessionService::ManagedWriteReceipt> SessionService::RejectManagedPendingInputLocked(
    const ManagedOperationProvenance& expected, const std::string& status, const std::string& reason, std::uint64_t revision) {
    auto receipt = std::make_shared<ManagedWriteReceipt>();
    receipt->phase = ManagedWriteReceipt::Phase::Rejection;
    receipt->input.error_code = "managed.operation.publication_unconfirmed";
    if (admission_mode_ != SessionAdmissionMode::ManagedStorageOnly || managed_closed_ || managed_first_failure_ ||
        pending_inputs_.empty() || !pending_inputs_.front().managed ||
        pending_inputs_.front().managed->provenance != expected) {
        receipt->input.error_code = "managed.operation.front_changed"; return receipt;
    }
    bool native_entered = false;
    try {
        auto scope = ManagedScopeLocked();
        if (!scope) { receipt->input.error_code = scope.error(); return receipt; }
        const auto front = pending_inputs_.front().managed;
        const auto key = operations_.find(front->client_operation_id);
        if (key == operations_.end() || key->second.managed != front) {
            receipt->input.error_code = "managed.operation.front_changed"; return receipt;
        }
        auto rejected = std::make_shared<ManagedStoredOperation>(*front);
        const auto now = (std::max)(NowMs(), rejected->received_at_ms);
        auto line = PrepareManagedOperationRejection(*front, status, reason, revision, now);
        if (!line) { receipt->input.error_code = line.error(); return receipt; }
        if (line->size() + 1 > kManagedOperationLedgerBytes - managed_ledger_bytes_) {
            receipt->input.error_code = "managed.operation.storage_limit"; return receipt;
        }
        rejected->state = ManagedStoredOperation::State::RejectedBeforeDispatch;
        rejected->terminal_status = status; rejected->reason_code = reason;
        rejected->terminal_policy_revision = revision; rejected->rejected_at_ms = now;
        receipt->operation = rejected;
        receipt->input.operation_id = expected.operation_id; receipt->input.input_id = expected.input_id;
        receipt->input.payload_hash = expected.input_hash;
        native_entered = true; receipt->knowledge = ManagedWriteReceipt::Knowledge::Unconfirmed;
        auto appended = operations_file_->AppendManaged(*line, managed_native_probe_);
        if (!appended) { receipt->knowledge = ManagedWriteReceipt::Knowledge::Rejected; managed_first_failure_ = receipt; return receipt; }
        receipt->append = *appended;
        if (appended->status != trajectory::JournalAppendStatus::Committed ||
            appended->confirmed_durability != trajectory::Durability::PowerLoss) {
            if (appended->status == trajectory::JournalAppendStatus::RejectedBeforeIO)
                receipt->knowledge = ManagedWriteReceipt::Knowledge::Rejected;
            managed_first_failure_ = receipt; return receipt;
        }
        receipt->knowledge = ManagedWriteReceipt::Knowledge::NativeCommittedPublicationGap;
        if (managed_publication_probe_) managed_publication_probe_();
        key->second.managed = rejected;
        pending_inputs_.pop_front();
        managed_ledger_bytes_ += line->size() + 1;
        receipt->input.error_code.clear(); receipt->knowledge = ManagedWriteReceipt::Knowledge::Committed;
    } catch (...) { if (native_entered) managed_first_failure_ = receipt; }
    return receipt;
}

std::shared_ptr<const SessionService::ManagedWriteReceipt> SessionService::RejectManagedPendingInput(
    const ManagedOperationProvenance& expected, const std::string& status, const std::string& reason, std::uint64_t revision) {
    std::lock_guard lock(commit_mutex_);
    if (managed_closing_ || execution_shutdown_requested_.load()) {
        auto receipt = std::make_shared<ManagedWriteReceipt>(); receipt->input.error_code = "session.stopping"; return receipt;
    }
    return RejectManagedPendingInputLocked(expected, status, reason, revision);
}

std::expected<ManagedOperationMaterials, std::string> SessionService::CaptureManagedOperationMaterialsLocked() const {
    auto materials = ManagedScopeLocked();
    if (!materials) return std::unexpected(materials.error());
    materials->completion_known = managed_first_failure_ == nullptr;
    const auto directory = trajectory()->session_dir();
    const auto ledger_path = directory / kOperationsFileName;
    if (!platform::IsUnlinkedOwnedPath(directory, ledger_path)) return std::unexpected("managed.operation.path_rejected");
    std::error_code error;
    const bool exists = std::filesystem::exists(platform::FileIoPath(ledger_path), error);
    if (error) return std::unexpected("managed.operation.read_failed");
    if (exists) {
        auto bytes = platform::ReadBoundedRegularFile(platform::FileIoPath(ledger_path), kManagedOperationLedgerBytes);
        if (!bytes) return std::unexpected("managed.operation.read_failed");
        materials->operations = std::move(*bytes);
    }
    auto roster = ReadManagedOperationLedgerOwned(materials->operations, materials->owner, materials->run_id);
    if (!roster) return std::unexpected(roster.error());
    std::set<std::string> names;
    for (const auto& operation : *roster) {
        const auto path = directory / tools::Utf8ToPath(operation.input_ref);
        if (!platform::IsUnlinkedOwnedPath(directory, path)) return std::unexpected("managed.operation.path_rejected");
        auto input = platform::ReadBoundedRegularFile(platform::FileIoPath(path), kManagedOperationInputBytes);
        if (!input) return std::unexpected("managed.operation.read_failed");
        names.insert(operation.provenance.operation_id + ".json");
        materials->inputs.emplace(operation.provenance.operation_id, std::move(*input));
    }
    const auto inputs_directory = directory / kInputArtifactsDir;
    if (!platform::IsUnlinkedOwnedPath(directory, inputs_directory)) return std::unexpected("managed.operation.path_rejected");
    const bool inputs_exist = std::filesystem::exists(platform::FileIoPath(inputs_directory), error);
    if (error) return std::unexpected("managed.operation.read_failed");
    if (inputs_exist) {
        std::filesystem::directory_iterator iterator(platform::FileIoPath(inputs_directory), error), end;
        if (error) return std::unexpected("managed.operation.read_failed");
        std::size_t count = 0;
        for (; iterator != end; iterator.increment(error)) {
            if (error || ++count > kManagedOperationInputEntries ||
                !names.erase(platform::PathToUtf8(iterator->path().filename())))
                return std::unexpected("managed.operation.invalid_materials");
        }
        if (error || !names.empty()) return std::unexpected("managed.operation.invalid_materials");
    } else if (!names.empty()) return std::unexpected("managed.operation.invalid_materials");
    auto verified = ReadManagedOperationsOwned(*materials);
    if (!verified) return std::unexpected(verified.error());
    return materials;
}

std::expected<ManagedOperationMaterials, std::string> SessionService::CaptureManagedOperationMaterials() const {
    std::lock_guard lock(commit_mutex_);
    if (managed_closed_materials_) return *managed_closed_materials_;
    return CaptureManagedOperationMaterialsLocked();
}
std::shared_ptr<const SessionService::ManagedWriteReceipt> SessionService::FirstManagedWriteFailure() const {
    std::lock_guard lock(commit_mutex_); return managed_first_failure_;
}
std::optional<trajectory::JournalCloseReceipt> SessionService::ManagedOperationCloseReceipt() const {
    std::lock_guard lock(commit_mutex_); return managed_operation_close_;
}
std::optional<trajectory::CloseOutcome> SessionService::ManagedMainCloseOutcome() const {
    std::lock_guard lock(commit_mutex_); return managed_main_close_;
}
void SessionService::SetManagedOperationProbesForTest(std::shared_ptr<trajectory::JournalNativeIoProbe> native,
    std::function<void()> publication) {
    {
        std::lock_guard lock(commit_mutex_);
        if (admission_mode_ != SessionAdmissionMode::ManagedStorageOnly || operation_counter_ || managed_closing_ || managed_closed_)
            throw std::logic_error("managed.operation.probe_not_admitted");
        managed_native_probe_.swap(native); managed_publication_probe_.swap(publication);
    } // Replaced probe/capture destructors stay outside the commit mutex.
}

trajectory::CloseOutcome SessionService::CloseManaged(const std::string& reason) {
    std::lock_guard close_lock(managed_close_mutex_);
    trajectory::CloseOutcome outcome;
    {
        std::lock_guard lock(commit_mutex_);
        if (managed_close_outcome_) return *managed_close_outcome_;
        managed_closing_ = true;
        if (!runtime_) {
            outcome.error_code = "close.no_active_session";
            outcome.message = "会话账未开,无处封口";
            managed_closed_ = true; managed_close_outcome_ = outcome;
            return outcome;
        }
    }
    if (!ShutdownExecution()) outcome.error_code = "close.async_shutdown_failed";
    {
        std::lock_guard lock(commit_mutex_);
        try {
            while (!pending_inputs_.empty() && !managed_first_failure_) {
                const auto front = pending_inputs_.front().managed;
                if (!front) { outcome.error_code = "managed.operation.invalid_queue"; break; }
                const auto rejected = RejectManagedPendingInputLocked(front->provenance, "cancelled", "session.closed", 0);
                if (rejected->knowledge != ManagedWriteReceipt::Knowledge::Committed) {
                    outcome.error_code = "managed.operation.close_unconfirmed"; break;
                }
            }
            managed_closed_materials_.emplace(CaptureManagedOperationMaterialsLocked());
            if (managed_closed_materials_->has_value() && !managed_closed_materials_->value().completion_known && outcome.error_code.empty())
                outcome.error_code = "managed.operation.close_unconfirmed";
            if (!managed_closed_materials_->has_value() && outcome.error_code.empty())
                outcome.error_code = "managed.operation.close_materials_unconfirmed";
        } catch (...) {
            outcome.error_code = "managed.operation.close_unconfirmed";
        }
        if (managed_first_failure_ && outcome.error_code.empty()) outcome.error_code = "managed.operation.close_unconfirmed";
        managed_operation_close_ = operations_file_ ? operations_file_->CloseManagedDetailed() : trajectory::JournalCloseReceipt{};
        if (!managed_operation_close_->ok() && outcome.error_code.empty()) outcome.error_code = "managed.operation.close_unconfirmed";
        if (managed_closed_materials_ && managed_closed_materials_->has_value() && !outcome.error_code.empty())
            managed_closed_materials_->value().completion_known = false;
        managed_closed_ = true; // No operation writer can remain writable when the SessionLock retires.
    }
    // Always close main/retire the actual lock, even when a rejected append was
    // unknown. Preserve both failures rather than pretending a clean queue.
    auto main = CloseRuntime(*runtime_, reason);
    {
        std::lock_guard lock(commit_mutex_); managed_main_close_ = main;
        if (!main.error_code.empty() && managed_closed_materials_ && managed_closed_materials_->has_value())
            managed_closed_materials_->value().completion_known = false;
    }
    if (outcome.error_code.empty()) outcome = std::move(main);
    else {
        main.message = "managed operation storage did not close cleanly; main=" + main.error_code;
        main.error_code = std::move(outcome.error_code); main.close_quality = "incomplete";
        outcome = std::move(main);
    }
    { std::lock_guard lock(commit_mutex_); managed_close_outcome_ = outcome; }
    return outcome;
}

// ---------------------------------------------------------------------------
// typed 域命令(goal/loop/plan;原样搬自 app-server HandleTypedDomainCommand
// 的执行段,CommandService 与轨迹 command 包裹共用一份)
// ---------------------------------------------------------------------------

ClientReceipt SessionService::ExecuteDomainCommand(const std::string& command_label,
                                                   const ClientCommand& command,
                                                   goal::GoalCoordinator* goal_coordinator,
                                                   loop::LoopScheduler* loop_scheduler,
                                                   const std::string& cwd_identity,
                                                   std::int64_t now_ms) {
    if (admission_mode_ == SessionAdmissionMode::ManagedStorageOnly) {
        ClientReceipt rejected;
        rejected.accepted = false;
        rejected.error_code = kManagedStorageOnlyError;
        return rejected;
    }
    // 与 app-server 旧实现同一只进程级静态(CommandService 的 Handle* 是
    // 非常方法;无状态,静置安全)。
    static CommandService kDomainService(CommandService::Options{});
    const bool is_goal = command.kind >= ClientCommandKind::CreateGoal &&
                         command.kind <= ClientCommandKind::ClearGoal;
    const bool is_loop = command.kind >= ClientCommandKind::CreateLoopTask &&
                         command.kind <= ClientCommandKind::RunLoopTaskNow;
    // 轨迹 command 包裹(§15.7):requested 先 durable,handler 跑完落
    // 终态;与 app-server 旧实现逐字节同源(label 用协议方法名)。
    TrajectorySessionLedger* ledger = trajectory();
    const std::string command_trajectory_id =
        ledger != nullptr ? ledger->BeginCommand(command_label, command_label, "session_state")
                          : std::string();
    ClientReceipt receipt;
    if (is_goal) {
        receipt = kDomainService.HandleGoalCommand(command, goal_coordinator, cwd_identity, now_ms);
    } else if (is_loop) {
        // session_id 用账本发的 workspace session id(app-server 旧实现递的
        // record->thread_id 正是它,P0-2 起同一命名空间)。
        receipt = kDomainService.HandleLoopCommand(command, loop_scheduler, cwd_identity,
                                                   ledger != nullptr ? ledger->session_id() : std::string(),
                                                   now_ms);
    } else {
        receipt = kDomainService.HandlePlanCommand(command, runtime_.get());
    }
    if (ledger != nullptr) {
        ledger->EndCommand(command_trajectory_id, receipt.accepted,
                           receipt.accepted ? std::string() : receipt.error_code);
    }
    return receipt;
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------

trajectory::CloseOutcome SessionService::Close(const std::string& reason) {
    if (admission_mode_ == SessionAdmissionMode::ManagedStorageOnly) return CloseManaged(reason);
    if (!ShutdownExecution()) {
        trajectory::CloseOutcome outcome;
        outcome.error_code = "close.async_shutdown_failed";
        outcome.message = "会话后台工具尚未可靠收口";
        return outcome;
    }
    if (runtime_ == nullptr) {
        trajectory::CloseOutcome outcome;
        outcome.error_code = "close.no_active_session";
        outcome.message = "会话账未开,无处封口";
        return outcome;
    }
    return CloseRuntime(*runtime_, reason);
}

}  // namespace lubancode::runtime
