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

#include <algorithm>
#include <chrono>
#include <fstream>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "config/config.hpp"  // HomeLubancodeDir:身份裁决的全局件止步
#include "platform/atomic_write.hpp"  // 原件原子写(ProcessCrashDurability=fsync 档)
#include "platform/sha256.hpp"
#include "runtime/command_service.hpp"
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
