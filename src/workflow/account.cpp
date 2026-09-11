// Workflow 编排账实现。写盘纪律:事件行走 V3EventLedger(哈希链/发号/
// 只 event 行);原件(inputs/outputs/checkpoints/definition)走
// platform::AtomicWriteFile(ProcessCrashDurability 档)——事实账按需升档。
#include "workflow/account.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/directory.hpp"  // IsValidSingleSegment

namespace lubancode::workflow {

namespace {

using trajectory::v3::Durability;
using trajectory::v3::EventDraft;
using trajectory::v3::EventKindV3;
using trajectory::v3::WriteReceipt;

// JournalClock -> V3Clock 适配(测试的 fake 钟一路进事件账)。
class AccountClock : public trajectory::v3::V3Clock {
public:
    explicit AccountClock(std::shared_ptr<JournalClock> clock) : clock_(std::move(clock)) {}
    std::int64_t WallMs() const override {
        return clock_ != nullptr ? clock_->NowMs() : trajectory::v3::V3Clock::WallMs();
    }

private:
    std::shared_ptr<JournalClock> clock_;
};

std::string CanonicalSha256(const nlohmann::json& value) {
    auto dump = trajectory::CanonicalJsonDump(value);
    if (!dump.has_value()) return std::string();
    return hooks::Sha256Hex(*dump);
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::string();
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

nlohmann::json ParseJsonText(const std::string& text) {
    try {
        return nlohmann::json::parse(text);
    } catch (...) {
        return nlohmann::json();
    }
}

// 段目录名 -> 段号(seg-3 -> 3);不合形状给 nullopt。
std::optional<std::uint64_t> SegmentNumber(const std::string& name) {
    if (name.rfind("seg-", 0) != 0) return std::nullopt;
    std::uint64_t value = 0;
    for (std::size_t i = 4; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(name[i] - '0');
    }
    if (value == 0) return std::nullopt;
    return value;
}

std::string SixDigits(const char* prefix, std::uint64_t counter) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s-%06llu", prefix,
                  static_cast<unsigned long long>(counter));
    return buffer;
}

// "<prefix>-<号>" 取号("out-000003" -> 3);不合形状给 0。
std::uint64_t ParseCounter(const std::string& id, const char* prefix) {
    const std::size_t width = std::string(prefix).size();
    if (id.size() <= width || id.compare(0, width, prefix) != 0) return 0;
    std::uint64_t value = 0;
    for (std::size_t i = width; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return 0;
        value = value * 10 + static_cast<std::uint64_t>(id[i] - '0');
    }
    return value;
}

// execution id 尾缀里的 dispatch 号(<runId>-<nodeId>[-iN][-dM] 取 M;rfind
// 取最后一个 -d,定义侧 -d 结尾的字段撞不上它)。
std::optional<std::uint64_t> DispatchSuffixOf(const std::string& execution_id) {
    const std::size_t at = execution_id.rfind("-d");
    if (at == std::string::npos) return std::nullopt;
    std::uint64_t value = 0;
    bool any = false;
    for (std::size_t i = at + 2; i < execution_id.size(); ++i) {
        if (execution_id[i] < '0' || execution_id[i] > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(execution_id[i] - '0');
        any = true;
    }
    if (!any) return std::nullopt;
    return value;
}

// 段文件末行的 eventId(OpenSegment 的 sourceRef 五键用)。文件不在/读不
// 出给空串——调用方如实落空串,不伪造。
std::string LastEventIdOfSegment(const std::filesystem::path& stream) {
    std::ifstream file(stream, std::ios::binary);
    if (!file.is_open()) return std::string();
    std::string line_text;
    std::string last;
    while (std::getline(file, line_text)) {
        if (!line_text.empty()) last = line_text;
    }
    const nlohmann::json line = ParseJsonText(last);
    if (!line.is_object()) return std::string();
    const auto it = line.find("eventId");
    if (it == line.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

WorkflowAccountError MakeError(std::string stage, std::string code, std::string detail) {
    return WorkflowAccountError{std::move(stage), std::move(code), std::move(detail)};
}

}  // namespace

// ---------------------------------------------------------------------------
// 记录 JSON
// ---------------------------------------------------------------------------

nlohmann::json OutputCommitRecord::ToJson() const {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["schema"] = schema;
    j["outputId"] = output_id;
    j["workflowRunId"] = workflow_run_id;
    j["nodeId"] = node_id;
    j["nodeExecutionId"] = node_execution_id;
    j["attemptId"] = attempt_id;
    j["payload"] = payload;
    j["payloadSha256"] = payload_sha256;
    j["inputSha256"] = input_sha256;
    j["validationPassed"] = validation_passed;
    j["validation"] = validation;
    j["createdAtMs"] = created_at_ms;
    return j;
}

std::optional<OutputCommitRecord> OutputCommitRecord::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    OutputCommitRecord record;
    auto str = [&json](const char* key) {
        const auto it = json.find(key);
        return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    record.schema = str("schema");
    record.output_id = str("outputId");
    record.workflow_run_id = str("workflowRunId");
    record.node_id = str("nodeId");
    record.node_execution_id = str("nodeExecutionId");
    record.attempt_id = str("attemptId");
    record.payload = json.contains("payload") ? json.at("payload") : nlohmann::json::object();
    record.payload_sha256 = str("payloadSha256");
    record.input_sha256 = str("inputSha256");
    if (const auto it = json.find("validationPassed"); it != json.end() && it->is_boolean()) {
        record.validation_passed = it->get<bool>();
    }
    if (const auto it = json.find("validation"); it != json.end() && it->is_object()) {
        record.validation = *it;
    }
    if (const auto it = json.find("createdAtMs"); it != json.end() && it->is_number_integer()) {
        record.created_at_ms = it->get<std::int64_t>();
    }
    return record;
}

nlohmann::json CheckpointRecord::ToJson() const {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["schema"] = schema;
    j["checkpointId"] = checkpoint_id;
    j["workflowRunId"] = workflow_run_id;
    j["segmentId"] = segment_id;
    j["throughSeq"] = through_seq;
    j["storeRevision"] = store_revision;
    j["store"] = store;
    j["storeSha256"] = store_sha256;
    j["createdAtMs"] = created_at_ms;
    return j;
}

std::optional<CheckpointRecord> CheckpointRecord::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    CheckpointRecord record;
    auto str = [&json](const char* key) {
        const auto it = json.find(key);
        return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    record.schema = str("schema");
    record.checkpoint_id = str("checkpointId");
    record.workflow_run_id = str("workflowRunId");
    record.segment_id = str("segmentId");
    if (const auto it = json.find("throughSeq"); it != json.end() && it->is_number_unsigned()) {
        record.through_seq = it->get<std::uint64_t>();
    }
    if (const auto it = json.find("storeRevision"); it != json.end() && it->is_number_unsigned()) {
        record.store_revision = it->get<std::uint64_t>();
    }
    record.store = json.contains("store") ? json.at("store") : nlohmann::json::object();
    record.store_sha256 = str("storeSha256");
    if (const auto it = json.find("createdAtMs"); it != json.end() && it->is_number_integer()) {
        record.created_at_ms = it->get<std::int64_t>();
    }
    return record;
}

// ---------------------------------------------------------------------------
// WorkflowPathResolver
// ---------------------------------------------------------------------------

std::expected<WorkflowPathResolver, std::string> WorkflowPathResolver::ForRun(
    const std::filesystem::path& runs_root, std::string_view workflow_run_id) {
    if (!trajectory::IsValidSingleSegment(workflow_run_id)) {
        return std::unexpected("workflow_run_id 不是合法单段名: " + std::string(workflow_run_id));
    }
    return WorkflowPathResolver(runs_root, std::string(workflow_run_id));
}

std::vector<std::string> WorkflowPathResolver::list_segments() const {
    std::vector<std::pair<std::uint64_t, std::string>> found;
    std::error_code ec;
    if (!std::filesystem::exists(segments_dir(), ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(segments_dir(), ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) {
            ec.clear();
            continue;
        }
        ec.clear();
        const std::string name = platform::PathToUtf8(entry.path().filename());
        if (auto number = SegmentNumber(name); number.has_value()) {
            found.emplace_back(*number, name);
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    out.reserve(found.size());
    for (auto& [_, name] : found) out.push_back(std::move(name));
    return out;
}

std::optional<std::string> WorkflowPathResolver::latest_segment_id() const {
    const auto segments = list_segments();
    if (segments.empty()) return std::nullopt;
    return segments.back();
}

std::string WorkflowPathResolver::NextSegmentId(std::string_view latest) {
    if (latest.empty()) return "seg-1";
    const auto number = SegmentNumber(std::string(latest));
    return "seg-" + std::to_string((number.has_value() ? *number : 0) + 1);
}

// ---------------------------------------------------------------------------
// WorkflowRunAccount::Impl
// ---------------------------------------------------------------------------

struct WorkflowRunAccount::Impl {
    std::mutex mutex;
    WorkflowPathResolver resolver;
    std::string run_id;
    std::string workflow_id;
    std::string workflow_version;
    std::string definition_hash;
    std::string cwd;
    std::shared_ptr<JournalClock> clock;
    std::shared_ptr<AccountClock> account_clock;
    trajectory::v3::V3EventLedgerOptions ledger_options;
    trajectory::v3::V3EventLedger ledger;
    std::string segment_id;
    RecoveryState recovery;  // Resume 的重放结果(Start 后为空骨架)
    std::uint64_t next_output = 1;
    std::uint64_t next_checkpoint = 1;
    std::uint64_t next_dispatch = 1;
    std::uint64_t store_revision = 0;
    bool broken = false;
    bool terminal_written = false;

    Impl(WorkflowPathResolver paths, std::string id, Options options)
        : resolver(std::move(paths)), run_id(std::move(id)), clock(std::move(options.clock)) {
        ledger_options.inject_io_failure = std::move(options.inject_io_failure);
        account_clock = std::make_shared<AccountClock>(clock);
    }

    std::int64_t NowMs() const {
        return clock != nullptr ? clock->NowMs() : JournalClock().NowMs();
    }

    // 账面健康:显式断账真断;未开段(Resume 后、OpenSegment 前)没有
    // 写柄,不算断——只是只读。
    bool LedgerOk() const {
        if (broken) return false;
        if (segment_id.empty()) return true;
        return !ledger.broken();
    }

    // 关键事实统一提交口:失败置 broken——这本账从此拒写,调用方停明确
    // 失败态(fail-closed,§二/§五)。
    WriteReceipt Put(EventDraft draft, Durability durability) {
        WriteReceipt receipt = ledger.Append(std::move(draft), durability);
        if (receipt.status != WriteReceipt::Status::Committed) {
            broken = true;
        }
        return receipt;
    }

    NodeExecutionIdentity MintExecutionIdLocked(const std::string& node_id, int map_item_index) {
        std::string id = run_id + "-" + node_id;
        if (map_item_index >= 0) id += "-i" + std::to_string(map_item_index);
        id += "-d" + std::to_string(next_dispatch);
        next_dispatch += 1;
        NodeExecutionIdentity identity;
        identity.node_id = node_id;
        identity.node_execution_id = id;
        identity.attempt = 1;
        if (map_item_index >= 0) {
            identity.invocation_path["mapItemIndex"] = map_item_index;
        }
        return identity;
    }
};

WorkflowRunAccount::WorkflowRunAccount(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorkflowRunAccount::WorkflowRunAccount(WorkflowRunAccount&&) noexcept = default;
WorkflowRunAccount& WorkflowRunAccount::operator=(WorkflowRunAccount&&) noexcept = default;
WorkflowRunAccount::~WorkflowRunAccount() = default;

const WorkflowPathResolver& WorkflowRunAccount::paths() const { return impl_->resolver; }
const RecoveryState& WorkflowRunAccount::recovery() const { return impl_->recovery; }
const std::string& WorkflowRunAccount::run_id() const { return impl_->run_id; }
bool WorkflowRunAccount::broken() const { return impl_ == nullptr || impl_->broken; }

// ---- Start ----

std::expected<WorkflowRunAccount, WorkflowAccountError> WorkflowRunAccount::Start(
    const std::filesystem::path& runs_root, const std::string& workflow_run_id,
    const DefinitionInfo& definition, const nlohmann::json& effective_inputs, Options options) {
    auto resolver = WorkflowPathResolver::ForRun(runs_root, workflow_run_id);
    if (!resolver.has_value()) {
        return std::unexpected(
            MakeError("start_dirs", "workflow.account.bad_run_id", resolver.error()));
    }
    std::error_code ec;
    if (std::filesystem::exists(resolver->run_dir(), ec)) {
        return std::unexpected(MakeError(
            "start_dirs", "workflow.account.run_dir_exists",
            "run 目录已存在,不许覆写: " + platform::PathToUtf8(resolver->run_dir())));
    }
    for (const auto& dir : {resolver->segments_dir(), resolver->checkpoints_dir(),
                            resolver->outputs_dir(), resolver->nodes_dir(),
                            resolver->artifacts_dir(), resolver->subflows_dir()}) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return std::unexpected(MakeError("start_dirs", "workflow.account.mkdir_failed",
                                             platform::PathToUtf8(dir) + ": " + ec.message()));
        }
    }
    // 定义快照(不可变,原子落稳):图版本冻结锚。resume 只认这份,配置
    // reload 换不掉正在跑的定义(§四)。
    {
        const auto written =
            platform::AtomicWriteFile(resolver->definition_path(), definition.definition_json,
                                      platform::WriteDurability::ProcessCrashDurability);
        if (!written.has_value()) {
            return std::unexpected(MakeError("definition_snapshot", "workflow.account.write_failed",
                                             written.error().code + ": " + written.error().message));
        }
    }
    // bindings.json:选定引用的冻结形状(棒二 WorkflowService 填真绑定;
    // 字段形状本棒冻结)。
    {
        nlohmann::ordered_json bindings = nlohmann::ordered_json::object();
        bindings["schema"] = 1;
        bindings["workflowId"] = definition.workflow_id;
        bindings["definitionHash"] = definition.content_hash;
        bindings["bindings"] = nlohmann::json::object();
        const auto written =
            platform::AtomicWriteFile(resolver->bindings_path(), bindings.dump(2),
                                      platform::WriteDurability::ProcessCrashDurability);
        if (!written.has_value()) {
            return std::unexpected(MakeError("bindings_snapshot", "workflow.account.write_failed",
                                             written.error().code + ": " + written.error().message));
        }
    }

    auto impl = std::make_unique<Impl>(*resolver, workflow_run_id, std::move(options));
    impl->workflow_id = definition.workflow_id;
    impl->workflow_version = definition.workflow_version;
    impl->definition_hash = definition.content_hash;
    impl->cwd = definition.cwd;

    // 开 seg-1:首事件 = workflow.definition.loaded(开账事实)。
    {
        EventDraft opening;
        opening.kind = EventKindV3::WorkflowDefinitionLoaded;
        opening.payload = nlohmann::json{{"workflowId", definition.workflow_id},
                                         {"definitionHash", definition.content_hash},
                                         {"definitionRef", "definition.json"}};
        if (!definition.workflow_version.empty()) {
            opening.payload["workflowVersion"] = definition.workflow_version;
        }
        if (!definition.cwd.empty()) {
            opening.payload["cwd"] = definition.cwd;
        }
        auto started = trajectory::v3::V3EventLedger::Start(
            resolver->segment_stream("seg-1"), workflow_run_id, "seg-1", std::move(opening),
            impl->ledger_options, impl->account_clock.get());
        if (!started.has_value()) {
            return std::unexpected(
                MakeError("ledger_start", "workflow.account.ledger_start", started.error()));
        }
        impl->ledger = std::move(*started);
        impl->segment_id = "seg-1";
    }

    WorkflowRunAccount account(std::move(impl));
    // inputs.json(无损)+ workflow.inputs.committed:无 checkpoint 也能从
    // run 开账事实恢复 inputs,不初始化成空(§十)。
    if (!account.RecordInputs(effective_inputs)) {
        return std::unexpected(MakeError("inputs_file", "workflow.account.inputs_failed",
                                         "inputs 快照或提交事件落不稳"));
    }
    return account;
}

bool WorkflowRunAccount::RecordInputs(const nlohmann::json& effective_inputs) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto dump = trajectory::CanonicalJsonDump(effective_inputs);
    if (!dump.has_value()) {
        impl_->broken = true;
        return false;
    }
    const std::string sha = hooks::Sha256Hex(*dump);
    const auto written =
        platform::AtomicWriteFile(paths().inputs_path(), *dump,
                                  platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        impl_->broken = true;
        return false;
    }
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowInputsCommitted;
    draft.payload = nlohmann::json{{"inputsRef", "inputs.json"}, {"sha256", sha}};
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    return receipt.status == WriteReceipt::Status::Committed;
}

// ---- Resume(只读重放)----

std::expected<WorkflowRunAccount, WorkflowAccountError> WorkflowRunAccount::Resume(
    const std::filesystem::path& runs_root, const std::string& workflow_run_id, Options options) {
    auto resolver = WorkflowPathResolver::ForRun(runs_root, workflow_run_id);
    if (!resolver.has_value()) {
        return std::unexpected(
            MakeError("resume_verify", "workflow.account.bad_run_id", resolver.error()));
    }
    if (!std::filesystem::exists(resolver->definition_path())) {
        return std::unexpected(MakeError(
            "resume_verify", "workflow.account.no_definition",
            "run 目录没有定义快照: " + platform::PathToUtf8(resolver->run_dir())));
    }
    auto impl = std::make_unique<Impl>(*resolver, workflow_run_id, std::move(options));
    RecoveryState& state = impl->recovery;
    state.workflow_run_id = workflow_run_id;

    const auto segments = resolver->list_segments();
    if (segments.empty()) {
        return std::unexpected(MakeError("resume_verify", "workflow.account.no_segments",
                                         "run 目录没有任何编排段"));
    }

    std::string inputs_sha_from_event;
    std::string definition_hash_from_event;
    std::string latest_checkpoint_id;
    std::uint64_t latest_checkpoint_through = 0;
    std::set<std::string> committed_output_ids;

    for (const std::string& segment : segments) {
        const std::filesystem::path stream = resolver->segment_stream(segment);
        auto report = trajectory::v3::VerifyV3EventLedgerFile(stream);
        if (!report.ok) {
            // 坏尾修复沿独占修复合同另立(§十);这里只报缺口,不偷偷裁、
            // 不跳过已提交事实继续执行。
            return std::unexpected(MakeError(
                "resume_verify", "workflow.account.segment_verify_failed",
                "段 " + segment + ": " + report.error_code + " " + report.message));
        }
        std::ifstream file(stream, std::ios::binary);
        std::string line_text;
        while (std::getline(file, line_text)) {
            if (line_text.empty()) continue;
            const nlohmann::json line = ParseJsonText(line_text);
            if (!line.is_object()) continue;
            const std::string kind = line.value("kind", std::string());
            const nlohmann::json payload =
                line.contains("payload") && line["payload"].is_object()
                    ? line["payload"]
                    : nlohmann::json::object();
            const std::string exec_id = payload.value("nodeExecutionId", std::string());
            if (kind == "workflow.definition.loaded") {
                definition_hash_from_event = payload.value("definitionHash", std::string());
                state.workflow_id = payload.value("workflowId", std::string());
                state.workflow_version = payload.value("workflowVersion", std::string());
            } else if (kind == "workflow.inputs.committed") {
                inputs_sha_from_event = payload.value("sha256", std::string());
            } else if (kind == "workflow.node.reserved") {
                RecoveredExecution& exec = state.executions[exec_id];
                exec.node_id = payload.value("nodeId", std::string());
                exec.node_execution_id = exec_id;
                exec.attempt = payload.value("attempt", 1);
                exec.reserved = true;
                state.execution_order.push_back(exec_id);
                state.node_latest_execution[exec.node_id] = exec_id;
                state.dispatch_count += 1;  // reserve 即占调度位(预算恢复底数)
            } else if (kind == "workflow.node.dispatched") {
                RecoveredExecution& exec = state.executions[exec_id];
                exec.node_id = payload.value("nodeId", exec.node_id);
                exec.node_execution_id = exec_id;
                exec.dispatched = true;
            } else if (kind == "workflow.output.committed") {
                const std::string output_id = payload.value("outputId", std::string());
                RecoveredExecution& exec = state.executions[exec_id];
                exec.node_id = payload.value("nodeId", exec.node_id);
                exec.node_execution_id = exec_id;
                exec.output_committed = true;
                exec.output_id = output_id;
                committed_output_ids.insert(output_id);
            } else if (kind == "workflow.node.completed") {
                // outcome 只有 success|empty(schema 钉死);失败走
                // workflow.node.failed——名字不判成功,产物看 commit(§五)。
                RecoveredExecution& exec = state.executions[exec_id];
                exec.outcome = payload.value("outcome", std::string());
                exec.tokens += payload.value("tokens", std::int64_t{0});
                state.tokens_used += payload.value("tokens", std::int64_t{0});
            } else if (kind == "workflow.node.failed") {
                RecoveredExecution& exec = state.executions[exec_id];
                exec.outcome = "failed";
                exec.error_code = payload.value("errorCode", std::string());
                exec.tokens += payload.value("tokens", std::int64_t{0});
                state.tokens_used += payload.value("tokens", std::int64_t{0});
            } else if (kind == "workflow.node.cancelled") {
                RecoveredExecution& exec = state.executions[exec_id];
                exec.outcome = "cancelled";
            } else if (kind == "workflow.node.skipped") {
                RecoveredExecution& exec =
                    state.executions["skip:" + payload.value("nodeId", std::string())];
                exec.node_id = payload.value("nodeId", std::string());
                exec.outcome = "skipped";
            } else if (kind == "workflow.checkpoint.committed") {
                const std::uint64_t through = payload.value("throughSeq", std::uint64_t{0});
                if (latest_checkpoint_id.empty() || through >= latest_checkpoint_through) {
                    latest_checkpoint_id = payload.value("checkpointId", std::string());
                    latest_checkpoint_through = through;
                }
            } else if (kind == "workflow.run.completed") {
                state.run_terminal = "succeeded";
                impl->terminal_written = true;
            } else if (kind == "workflow.run.failed") {
                state.run_terminal = "failed";
                impl->terminal_written = true;
            } else if (kind == "workflow.run.cancelled") {
                state.run_terminal = "cancelled";
                impl->terminal_written = true;
            }
        }
        // 尾段水位(OpenSegment 链接源水位用;段按号升序,末轮即尾段)。
        state.tail_segment_id = segment;
        state.tail_segment_last_seq = report.last_seq;
        state.tail_segment_last_hash = report.last_line_hash;
    }

    // 定义快照核验:文件里的定义重算 hash,对不上账上记的定义身份即拒
    //(配置 reload/手改都偷换不了正在跑的定义)。
    {
        const nlohmann::json definition_json = ParseJsonText(ReadFileText(resolver->definition_path()));
        if (!definition_json.is_object()) {
            return std::unexpected(
                MakeError("definition_hash", "workflow.account.bad_definition", "定义快照解析失败"));
        }
        try {
            const WorkflowDefinition snapshot = WorkflowDefinition::FromJson(definition_json);
            const std::string recomputed = ContentHash(snapshot);
            if (recomputed != definition_hash_from_event) {
                return std::unexpected(MakeError(
                    "definition_hash", "workflow.account.definition_hash_mismatch",
                    "快照 hash " + recomputed + " != 账上 " + definition_hash_from_event));
            }
            state.definition = definition_json;
            state.definition_hash = recomputed;
        } catch (const std::exception& e) {
            return std::unexpected(
                MakeError("definition_hash", "workflow.account.bad_definition", e.what()));
        }
    }

    // inputs 无损读回 + hash 对账。
    if (!inputs_sha_from_event.empty()) {
        const std::string text = ReadFileText(resolver->inputs_path());
        if (text.empty()) {
            return std::unexpected(MakeError("inputs_file", "workflow.account.inputs_missing",
                                             "账上有 inputs 提交,inputs.json 缺失"));
        }
        if (hooks::Sha256Hex(text) != inputs_sha_from_event) {
            return std::unexpected(MakeError("inputs_file", "workflow.account.inputs_hash_mismatch",
                                             "inputs.json 内容与提交事件不符"));
        }
        state.inputs = ParseJsonText(text);
    }

    // checkpoint:按账上提交事件取最后一份,孤立候选文件不生效(§十)。
    if (!latest_checkpoint_id.empty()) {
        auto record = CheckpointRecord::FromJson(
            ParseJsonText(ReadFileText(resolver->checkpoint_path(latest_checkpoint_id))));
        if (!record.has_value() || record->checkpoint_id != latest_checkpoint_id) {
            return std::unexpected(MakeError("checkpoint_file", "workflow.account.checkpoint_bad",
                                             "checkpoint " + latest_checkpoint_id + " 读不回/身份不符"));
        }
        // 无损 Store 快照 hash 对账——不用脱敏展示值恢复(§二)。
        if (CanonicalSha256(record->store) != record->store_sha256) {
            return std::unexpected(MakeError(
                "checkpoint_file", "workflow.account.checkpoint_hash_mismatch",
                "checkpoint " + latest_checkpoint_id + " store hash 不符"));
        }
        impl->next_checkpoint = ParseCounter(latest_checkpoint_id, "cp-") + 1;
        impl->store_revision = record->store_revision;
        state.checkpoint = std::move(record);
    }

    // committed outputs:从 outputs/<id>.json 无损读回(验 payload hash)。
    for (auto& [exec_id, exec] : state.executions) {
        if (!exec.output_committed || exec.output_id.empty()) continue;
        auto record = OutputCommitRecord::FromJson(
            ParseJsonText(ReadFileText(resolver->output_path(exec.output_id))));
        if (!record.has_value() || record->output_id != exec.output_id) {
            return std::unexpected(MakeError("output_file", "workflow.account.output_bad",
                                             "产物 " + exec.output_id + " 读不回/身份不符"));
        }
        if (CanonicalSha256(record->payload) != record->payload_sha256) {
            return std::unexpected(MakeError("output_file", "workflow.account.output_hash_mismatch",
                                             "产物 " + exec.output_id + " payload hash 不符"));
        }
        if (!record->validation_passed) {
            // 校验未过的候选不该带 commit 事件(commit 门拒过的候选不落
            // commit)——出现了就是账坏了,如实拒。
            return std::unexpected(MakeError(
                "output_file", "workflow.account.commit_unvalidated",
                "产物 " + exec.output_id + " 校验未过却带了 commit"));
        }
        exec.committed_output = std::move(record->payload);
        impl->next_output = std::max(impl->next_output, ParseCounter(exec.output_id, "out-") + 1);
    }

    // 悬置候选:文件在、账上无 commit 事件(保存原件后崩溃)。被拒候选
    //(validation_passed=false 的留档件)不算悬置。
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(resolver->outputs_dir(), ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                ec.clear();
                continue;
            }
            ec.clear();
            auto record =
                OutputCommitRecord::FromJson(ParseJsonText(ReadFileText(entry.path())));
            if (!record.has_value() || record->output_id.empty()) continue;
            impl->next_output =
                std::max(impl->next_output, ParseCounter(record->output_id, "out-") + 1);
            if (committed_output_ids.count(record->output_id) > 0) continue;
            if (CanonicalSha256(record->payload) != record->payload_sha256) continue;  // 残件如实跳过
            if (!record->validation_passed) continue;  // 被拒候选留档,不采纳
            state.dangling_candidates.push_back(std::move(*record));
        }
    }

    // dispatch 号恢复:execution id 尾缀取最大 +1,新铸 id 不与旧执行撞名。
    for (const auto& [exec_id, exec] : state.executions) {
        if (auto dispatch = DispatchSuffixOf(exec_id); dispatch.has_value()) {
            impl->next_dispatch = std::max(impl->next_dispatch, *dispatch + 1);
        }
    }

    return WorkflowRunAccount(std::move(impl));
}

std::expected<std::string, WorkflowAccountError> WorkflowRunAccount::OpenSegment() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->broken) {
        return std::unexpected(MakeError("segment_open", "workflow.account.broken", "账已断"));
    }
    if (!impl_->LedgerOk()) {
        return std::unexpected(
            MakeError("segment_open", "workflow.account.ledger_broken", "当前段句柄已断"));
    }
    // 源水位:链尾 = 当前写段(Start 后)或重放出的尾段(Resume 后)。
    const bool from_current = !impl_->segment_id.empty();
    const std::string source_segment =
        from_current ? impl_->segment_id : impl_->recovery.tail_segment_id;
    const std::uint64_t source_seq =
        from_current ? impl_->ledger.last_seq() : impl_->recovery.tail_segment_last_seq;
    const std::string source_hash =
        from_current ? impl_->ledger.last_line_hash() : impl_->recovery.tail_segment_last_hash;
    const std::string source_event_id = LastEventIdOfSegment(impl_->resolver.segment_stream(source_segment));
    if (source_event_id.empty()) {
        return std::unexpected(MakeError(
            "segment_open", "workflow.account.source_unreadable",
            "尾段 " + source_segment + " 末行 eventId 读不出,水位链接不造假"));
    }
    const std::string next = WorkflowPathResolver::NextSegmentId(source_segment);
    std::error_code ec;
    if (std::filesystem::exists(impl_->resolver.segment_stream(next), ec)) {
        return std::unexpected(MakeError(
            "segment_open", "workflow.account.segment_exists",
            "段 " + next + " 已存在(另一恢复者先开了)——单写者,不覆写"));
    }
    // 链接源水位:五键指尾段末行(§3.1 跨账引用;旧段只读)。
    EventDraft opening;
    opening.kind = EventKindV3::WorkflowSegmentOpened;
    opening.payload = nlohmann::json{
        {"segmentId", next},
        {"sourceRef", nlohmann::json{{"sessionId", impl_->run_id},
                                     {"runId", source_segment},
                                     {"seq", source_seq},
                                     {"id", source_event_id},
                                     {"hash", source_hash}}},
        {"sourceThroughSeq", source_seq}};
    auto started = trajectory::v3::V3EventLedger::Start(
        impl_->resolver.segment_stream(next), impl_->run_id, next, std::move(opening),
        impl_->ledger_options, impl_->account_clock.get());
    if (!started.has_value()) {
        return std::unexpected(
            MakeError("segment_open", "workflow.account.ledger_start", started.error()));
    }
    impl_->ledger = std::move(*started);
    impl_->segment_id = next;
    return impl_->segment_id;
}

// ---- 写口 ----

std::expected<NodeExecutionIdentity, WorkflowAccountError>
WorkflowRunAccount::ReserveNodeExecution(const std::string& node_id, const std::string& node_kind,
                                         int map_item_index,
                                         const nlohmann::json& resolved_input) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    NodeExecutionIdentity identity = impl_->MintExecutionIdLocked(node_id, map_item_index);
    identity.input_sha256 = CanonicalSha256(resolved_input);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeReserved;
    draft.payload = nlohmann::json{{"nodeId", node_id},
                                   {"nodeExecutionId", identity.node_execution_id},
                                   {"nodeKind", node_kind},
                                   {"attempt", 1},
                                   {"inputHash", identity.input_sha256}};
    if (!identity.invocation_path.empty()) {
        draft.payload["invocationPath"] = identity.invocation_path;
    }
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    if (receipt.status != WriteReceipt::Status::Committed) {
        return std::unexpected(MakeError("commit_event", receipt.error_code,
                                         "workflow.node.reserved 落不稳: " + receipt.error_message));
    }
    return identity;
}

NodeExecutionIdentity WorkflowRunAccount::MintExecutionId(const std::string& node_id,
                                                          int map_item_index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->MintExecutionIdLocked(node_id, map_item_index);
}

bool WorkflowRunAccount::RecordNodeDispatched(const NodeExecutionIdentity& identity) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeDispatched;
    draft.payload = nlohmann::json{{"nodeId", identity.node_id},
                                   {"nodeExecutionId", identity.node_execution_id},
                                   {"attempt", identity.attempt}};
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    return receipt.status == WriteReceipt::Status::Committed;
}

std::expected<OutputCommitRecord, WorkflowAccountError> WorkflowRunAccount::CommitNodeOutput(
    const NodeExecutionIdentity& identity, const nlohmann::json& payload,
    const OutputValidation& validation, const std::string& adopt_output_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    OutputCommitRecord record;
    record.output_id =
        adopt_output_id.empty() ? SixDigits("out", impl_->next_output) : adopt_output_id;
    record.workflow_run_id = impl_->run_id;
    record.node_id = identity.node_id;
    record.node_execution_id = identity.node_execution_id;
    record.attempt_id = identity.attempt_id();
    record.payload = payload;
    record.payload_sha256 = CanonicalSha256(payload);
    record.input_sha256 = identity.input_sha256;
    record.validation_passed = validation.passed;
    record.validation = validation.ToJson();
    record.created_at_ms = impl_->NowMs();
    // 1) 原件先落稳(原子、无损):恢复不会捡到半份结果(§五/§二)。
    const auto written =
        platform::AtomicWriteFile(impl_->resolver.output_path(record.output_id),
                                  record.ToJson().dump(),
                                  platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        impl_->broken = true;
        return std::unexpected(MakeError("output_file", "workflow.account.write_failed",
                                         written.error().code + ": " + written.error().message));
    }
    // 2) output.commit 落稳(PowerLoss):"产物可供下游消费"的唯一依据。
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowOutputCommitted;
    draft.payload = nlohmann::json{{"nodeId", identity.node_id},
                                   {"nodeExecutionId", identity.node_execution_id},
                                   {"attempt", identity.attempt},
                                   {"outputId", record.output_id},
                                   {"outputHash", record.payload_sha256},
                                   {"outputRef", "outputs/" + record.output_id + ".json"},
                                   {"validation", validation.ToJson()}};
    if (!identity.input_sha256.empty()) {
        draft.payload["resolvedInputHash"] = identity.input_sha256;
    }
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    if (receipt.status != WriteReceipt::Status::Committed) {
        return std::unexpected(
            MakeError("commit_event", receipt.error_code,
                      "workflow.output.committed 落不稳: " + receipt.error_message));
    }
    impl_->next_output =
        std::max(impl_->next_output, ParseCounter(record.output_id, "out-") + 1);
    impl_->store_revision += 1;
    return record;
}

void WorkflowRunAccount::SaveRejectedCandidate(const NodeExecutionIdentity& identity,
                                               const nlohmann::json& payload,
                                               const OutputValidation& validation,
                                               const std::string& adopt_output_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    OutputCommitRecord record;
    record.output_id =
        adopt_output_id.empty() ? SixDigits("out", impl_->next_output) : adopt_output_id;
    record.workflow_run_id = impl_->run_id;
    record.node_id = identity.node_id;
    record.node_execution_id = identity.node_execution_id;
    record.attempt_id = identity.attempt_id();
    record.payload = payload;
    record.payload_sha256 = CanonicalSha256(payload);
    record.input_sha256 = identity.input_sha256;
    record.validation_passed = false;  // 被拒候选:不落 commit 事件、不发布
    record.validation = validation.ToJson();
    record.created_at_ms = impl_->NowMs();
    const auto written =
        platform::AtomicWriteFile(impl_->resolver.output_path(record.output_id),
                                  record.ToJson().dump(),
                                  platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        return;  // 留档失败不改变节点结局(已判失败),只少一份观察件
    }
    impl_->next_output = std::max(impl_->next_output, ParseCounter(record.output_id, "out-") + 1);
}

bool WorkflowRunAccount::RecordNodeCompleted(const NodeExecutionIdentity& identity,
                                             const std::string& outcome, std::int64_t duration_ms,
                                             std::int64_t tokens) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeCompleted;
    nlohmann::json payload{{"nodeId", identity.node_id},
                           {"nodeExecutionId", identity.node_execution_id},
                           {"attempt", identity.attempt},
                           {"outcome", outcome}};
    if (duration_ms >= 0) payload["durationMs"] = duration_ms;
    if (tokens != 0) payload["tokens"] = tokens;
    draft.payload = std::move(payload);
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    return receipt.status == WriteReceipt::Status::Committed;
}

bool WorkflowRunAccount::RecordNodeFailed(const NodeExecutionIdentity& identity,
                                          const std::string& error_code,
                                          const std::string& error_message, std::int64_t duration_ms,
                                          std::int64_t tokens) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeFailed;
    nlohmann::json payload{{"nodeId", identity.node_id},
                           {"nodeExecutionId", identity.node_execution_id},
                           {"attempt", identity.attempt},
                           {"errorCode", error_code}};
    if (!error_message.empty()) payload["errorMessage"] = error_message.substr(0, 500);
    if (duration_ms >= 0) payload["durationMs"] = duration_ms;
    if (tokens != 0) payload["tokens"] = tokens;
    draft.payload = std::move(payload);
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    return receipt.status == WriteReceipt::Status::Committed;
}

bool WorkflowRunAccount::RecordNodeCancelled(const std::string& node_id,
                                             const std::string& node_execution_id,
                                             const std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeCancelled;
    draft.payload =
        nlohmann::json{{"nodeId", node_id}, {"reason", reason.empty() ? "cancelled" : reason}};
    if (!node_execution_id.empty()) {
        draft.payload["nodeExecutionId"] = node_execution_id;
    }
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    return receipt.status == WriteReceipt::Status::Committed;
}

bool WorkflowRunAccount::RecordNodeSkipped(const std::string& node_id, const std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeSkipped;
    draft.payload =
        nlohmann::json{{"nodeId", node_id}, {"reason", reason.empty() ? "skipped" : reason}};
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    return receipt.status == WriteReceipt::Status::Committed;
}

std::expected<CheckpointRecord, WorkflowAccountError> WorkflowRunAccount::CommitCheckpoint(
    const nlohmann::json& store_json) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CheckpointRecord record;
    record.checkpoint_id = SixDigits("cp", impl_->next_checkpoint);
    record.workflow_run_id = impl_->run_id;
    record.segment_id = impl_->segment_id;
    record.through_seq = impl_->ledger.last_seq();
    record.store_revision = impl_->store_revision;
    record.store = store_json;
    record.store_sha256 = CanonicalSha256(store_json);
    record.created_at_ms = impl_->NowMs();
    // 1) 文件先不可变落稳;孤立候选文件不生效——生效判据是下一条事件(§十)。
    const auto written =
        platform::AtomicWriteFile(impl_->resolver.checkpoint_path(record.checkpoint_id),
                                  record.ToJson().dump(),
                                  platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        impl_->broken = true;
        return std::unexpected(MakeError("checkpoint_file", "workflow.account.write_failed",
                                         written.error().code + ": " + written.error().message));
    }
    // 2) workflow.checkpoint.committed(ref/hash/throughSeq/storeRevision)。
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowCheckpointCommitted;
    draft.payload = nlohmann::json{{"checkpointId", record.checkpoint_id},
                                   {"checkpointRef", "checkpoints/" + record.checkpoint_id + ".json"},
                                   {"sha256", record.store_sha256},
                                   {"throughSeq", record.through_seq},
                                   {"storeRevision", record.store_revision}};
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    if (receipt.status != WriteReceipt::Status::Committed) {
        return std::unexpected(
            MakeError("commit_event", receipt.error_code,
                      "workflow.checkpoint.committed 落不稳: " + receipt.error_message));
    }
    impl_->next_checkpoint += 1;
    return record;
}

bool WorkflowRunAccount::RecordRunCompleted(const nlohmann::json& result) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_written) return false;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowRunCompleted;
    draft.payload =
        nlohmann::json{{"resultHash", CanonicalSha256(result)}, {"resultRef", result}};
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    if (receipt.status == WriteReceipt::Status::Committed) impl_->terminal_written = true;
    return receipt.status == WriteReceipt::Status::Committed;
}

bool WorkflowRunAccount::RecordRunFailed(const std::string& error_code,
                                         const std::string& error_message) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_written) return false;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowRunFailed;
    draft.payload = nlohmann::json{{"errorCode", error_code}};
    if (!error_message.empty()) draft.payload["errorMessage"] = error_message.substr(0, 500);
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    if (receipt.status == WriteReceipt::Status::Committed) impl_->terminal_written = true;
    return receipt.status == WriteReceipt::Status::Committed;
}

bool WorkflowRunAccount::RecordRunCancelled(const std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->terminal_written) return false;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowRunCancelled;
    draft.payload = nlohmann::json{{"reason", reason.empty() ? "cancelled" : reason}};
    const auto receipt = impl_->Put(std::move(draft), Durability::PowerLoss);
    if (receipt.status == WriteReceipt::Status::Committed) impl_->terminal_written = true;
    return receipt.status == WriteReceipt::Status::Committed;
}

// ---- 非关键投影 ----

void WorkflowRunAccount::RecordNodeWaiting(const std::string& node_id,
                                           const std::string& wait_kind, const std::string& reason,
                                           const std::string& body) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->LedgerOk()) return;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeWaiting;
    nlohmann::json payload{{"nodeId", node_id},
                           {"waitKind", wait_kind},
                           {"reason", reason.empty() ? wait_kind : reason}};
    if (!body.empty()) payload["body"] = body;
    draft.payload = std::move(payload);
    (void)impl_->ledger.Append(std::move(draft), Durability::ProcessCrash);
}

void WorkflowRunAccount::RecordNodeRetrying(const NodeExecutionIdentity& identity,
                                            int max_attempts, const std::string& error_code) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->LedgerOk()) return;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowNodeRetrying;
    draft.payload = nlohmann::json{{"nodeId", identity.node_id},
                                   {"nodeExecutionId", identity.node_execution_id},
                                   {"attempt", identity.attempt},
                                   {"maxAttempts", max_attempts},
                                   {"errorCode", error_code}};
    (void)impl_->ledger.Append(std::move(draft), Durability::ProcessCrash);
}

void WorkflowRunAccount::RecordBranchStarted(const std::string& node_id,
                                             const std::vector<std::string>& branches,
                                             int concurrency) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->LedgerOk()) return;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& branch : branches) list.push_back(branch);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowBranchStarted;
    draft.payload = nlohmann::json{
        {"nodeId", node_id}, {"branches", std::move(list)}, {"concurrency", concurrency}};
    (void)impl_->ledger.Append(std::move(draft), Durability::ProcessCrash);
}

void WorkflowRunAccount::RecordJoinCompleted(const std::string& node_id, const std::string& join,
                                             int succeeded, int failed,
                                             const std::vector<std::string>& unavailable) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->LedgerOk()) return;
    nlohmann::json missing = nlohmann::json::array();
    for (const auto& id : unavailable) missing.push_back(id);
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowJoinCompleted;
    draft.payload = nlohmann::json{{"nodeId", node_id},
                                   {"join", join},
                                   {"succeeded", succeeded},
                                   {"failed", failed},
                                   {"unavailable", std::move(missing)}};
    (void)impl_->ledger.Append(std::move(draft), Durability::ProcessCrash);
}

void WorkflowRunAccount::RecordLoopIterationStarted(const std::string& node_id, int iteration) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->LedgerOk()) return;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowLoopIterationStarted;
    draft.payload = nlohmann::json{{"nodeId", node_id}, {"iteration", iteration}};
    (void)impl_->ledger.Append(std::move(draft), Durability::ProcessCrash);
}

void WorkflowRunAccount::RecordLoopIterationCompleted(const std::string& node_id, int iteration,
                                                      bool condition_met) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->LedgerOk()) return;
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowLoopIterationCompleted;
    draft.payload = nlohmann::json{
        {"nodeId", node_id}, {"iteration", iteration}, {"conditionMet", condition_met}};
    (void)impl_->ledger.Append(std::move(draft), Durability::ProcessCrash);
}

}  // namespace lubancode::workflow
