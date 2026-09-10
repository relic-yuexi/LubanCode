// SessionService 的实现(AppServer 接入 Session v3 第一棒)。
//
// 本文件是"搬运收敛":开张折算(身份裁决 + Options 组装)原样收编三处
// 重复装配(终端 interactive_session_assembly、one_shot、app_server
// HandleThreadStart),语义一个字不改;输入接纳与操作台账是新落的最小
// 幂等面(§4.2),先账后回执、单写者串行。

#include "runtime/session_service.hpp"

#include <chrono>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "config/config.hpp"  // HomeLubancodeDir:身份裁决的全局件止步
#include "platform/sha256.hpp"
#include "runtime/command_service.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/loop_scheduler.hpp"
#include "tools/path_utils.hpp"  // Utf8ToPath/PathToUtf8
#include "trajectory/v3/session_switch.hpp"  // FindV3SessionStream:开关结果识别

namespace lubancode::runtime {

namespace {

constexpr const char* kOperationsFileName = "operations.jsonl";

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ---------------------------------------------------------------------------
// 操作台账文件:append-only JSONL,一行一操作事实。先账后回执的关键件
// ——SubmitInput 在回执返回前 flush;崩溃窗口里"有账无回执"的重发按
// 台账命中,不重复接纳。行形状(schemaVersion=1):
//   {"kind":"operation.accepted","operationId":"op-1","inputId":"in-1",
//    "clientOperationId":"...","payloadHash":"...","receivedAtMs":...}
// 这是服务层账,不进轨迹主账(§三:新事实进 v3 主账须先补 kind/schema
// 合同,归 2.0 面)。
// ---------------------------------------------------------------------------
class SessionService::OperationsFile {
public:
    explicit OperationsFile(const std::filesystem::path& path) : path_(path) {}

    // 惰性建账:开张不造空文件,首笔接纳才落 operations.jsonl(与账本
    // "延迟开卷"同一取向——空场在盘上不留 0 字节残留)。
    bool ok() const { return !path_.empty(); }
    const std::filesystem::path& path() const { return path_; }

    // 落一行并 flush(账先行:调用方在收到 true 后才许出回执/入队)。
    bool Append(const nlohmann::json& line) {
        if (!stream_.is_open()) {
            stream_.open(path_, std::ios::app | std::ios::binary);
        }
        if (!stream_.is_open()) {
            return false;
        }
        stream_ << line.dump() << "\n";
        stream_.flush();
        return stream_.good();
    }

    // 只读装载(开张种账用):坏行跳过不猜(json 缺键一律 contains())。
    static std::vector<nlohmann::json> ReadLines(const std::filesystem::path& path) {
        std::vector<nlohmann::json> lines;
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
            if (line.is_object() && line.contains("kind")) {
                lines.push_back(line);
            }
        }
        return lines;
    }

private:
    std::filesystem::path path_;
    std::ofstream stream_;
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
    options.trajectory_resume_source_session_id = request.resume_source_session_id;
    options.trajectory_workspaces_root = request.workspaces_root;
    options.trajectory_launch_cwd = request.launch_cwd;
    options.trajectory_one_shot = request.one_shot;
    options.trajectory_training_policy = request.training_policy;
    options.trajectory_v3_system_content = request.v3_system_content;
    // 身份:显式递的整份吃;否则按 cwd 四级裁决(commondir→marker→
    // config→cwd),home 递进去做全局件止步——与三端被收编前的原装配
    // 逐句对应(终端/one-shot:current_path;app-server:前端指定 cwd)。
    if (request.workspace_identity.has_value()) {
        options.trajectory_workspace_identity = *request.workspace_identity;
    } else {
        const std::filesystem::path identity_cwd =
            request.cwd_utf8.empty() ? std::filesystem::current_path() : tools::Utf8ToPath(request.cwd_utf8);
        const auto identity_home = config::HomeLubancodeDir();
        auto identity = workspace::ResolveWorkspaceIdentity(
            identity_cwd, identity_home.has_value() ? tools::Utf8ToPath(*identity_home)
                                                    : std::filesystem::path());
        if (identity.has_value()) {
            options.trajectory_workspace_identity = std::move(*identity);
        }
        // 裁决失败留空:沿旧例交账本兜底/明败,不在服务里另算一把 key。
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

SessionService::~SessionService() = default;

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
    // 直接来源场(resume-as-new 的 source):同 workspace 的 sessions/
    // 邻居目录。单跳——多跳来源链(resume 的 resume)随 2.0 面补。
    if (!resume_source_session_id_.empty()) {
        const std::filesystem::path source_dir =
            runtime_->trajectory()->session_dir().parent_path() /
            tools::Utf8ToPath(resume_source_session_id_);
        for (const nlohmann::json& line : OperationsFile::ReadLines(source_dir / kOperationsFileName)) {
            if (line.value("kind", std::string()) != "operation.accepted") {
                continue;
            }
            const std::string key = line.value("clientOperationId", std::string());
            if (key.empty() || operations_.count(key) > 0) {
                continue;
            }
            AcceptedOperation accepted;
            accepted.operation_id = line.value("operationId", std::string());
            accepted.input_id = line.value("inputId", std::string());
            accepted.payload_hash = line.value("payloadHash", std::string());
            operations_.emplace(key, std::move(accepted));
        }
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
    if (runtime_ == nullptr || trajectory() == nullptr) {
        receipt.error_code = "trajectory.open_failed";
        return receipt;
    }
    const std::string payload_hash = platform::Sha256Hex(CanonicalInputPayload(input));
    std::lock_guard<std::mutex> lock(commit_mutex_);
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
    // 先账:操作事实落台账并 flush,回执之前完成(崩溃窗口里有账无回执,
    // 重发按台账命中,不重复执行)。
    const std::string operation_id = "op-" + std::to_string(operation_counter_ + 1);
    const std::string input_id = "in-" + std::to_string(operation_counter_ + 1);
    ++operation_counter_;
    if (operations_file_ != nullptr && operations_file_->ok()) {
        nlohmann::json line{{"schemaVersion", 1},
                            {"kind", "operation.accepted"},
                            {"operationId", operation_id},
                            {"inputId", input_id},
                            {"clientOperationId", input.client_operation_id},
                            {"payloadHash", payload_hash},
                            {"receivedAtMs", NowMs()}};
        (void)operations_file_->Append(line);  // 写失败:内存表仍记账,回执照出
    }
    if (!input.client_operation_id.empty()) {
        AcceptedOperation accepted;
        accepted.operation_id = operation_id;
        accepted.input_id = input_id;
        accepted.payload_hash = payload_hash;
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

std::optional<SessionService::QueuedInput> SessionService::PopPendingInput() {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    if (pending_inputs_.empty()) {
        return std::nullopt;
    }
    QueuedInput front = std::move(pending_inputs_.front());
    pending_inputs_.pop_front();
    return front;
}

std::size_t SessionService::pending_input_count() const {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    return pending_inputs_.size();
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
    if (runtime_ == nullptr) {
        trajectory::CloseOutcome outcome;
        outcome.error_code = "close.no_active_session";
        outcome.message = "会话账未开,无处封口";
        return outcome;
    }
    return CloseRuntime(*runtime_, reason);
}

}  // namespace lubancode::runtime
