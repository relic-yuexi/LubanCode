#include "app/memory_ledger_bridge.hpp"

#include <filesystem>
#include <system_error>

#include "trajectory/blob_store.hpp"
#include "trajectory/recorder.hpp"

namespace lubancode::app {

namespace {

namespace fs = std::filesystem;

// 合同 §四:≤512B 的小内容允许 snapshot_inline 替代 snapshot_ref。
constexpr std::size_t kSnapshotInlineLimit = 512;

std::string PathUtf8Text(const fs::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

}  // namespace

MemoryLedgerBridge::MemoryLedgerBridge(runtime::TrajectorySessionLedger& ledger) : ledger_(ledger) {}

std::expected<void, std::string> MemoryLedgerBridge::RecordRecallInjection(
    const memory::InjectedMemoryRecord& record) {
    auto* recorder = ledger_.main();
    if (recorder == nullptr) {
        return std::unexpected("memory.recall_snapshot_failed: 轨迹账没有可用的 main stream");
    }

    // 快照正文先落稳:小内容内联,其余写 session artifacts 的内容寻址 blob
    // (同 hash 已在仓内直接复用,天然幂等)。写不稳就不许注(§9.2)。
    std::string snapshot_ref;
    std::string snapshot_inline;
    if (record.content.size() > kSnapshotInlineLimit) {
        const fs::path artifact_root = ledger_.session_dir() / "artifacts";
        trajectory::BlobStore blobs(artifact_root);
        auto stored = blobs.Store(record.content, "text/plain", trajectory::Durability::ProcessCrash);
        if (!stored.has_value()) {
            return std::unexpected("memory.recall_snapshot_failed: " + stored.error());
        }
        if (stored->sha256 != record.content_sha256) {
            return std::unexpected("memory.recall_snapshot_failed: 快照指纹与正文对不上");
        }
        std::error_code ec;
        const fs::path relative = fs::relative(blobs.PathFor(stored->sha256), ledger_.session_dir(), ec);
        if (ec) {
            return std::unexpected("memory.recall_snapshot_failed: 快照路径解析失败: " + ec.message());
        }
        snapshot_ref = PathUtf8Text(relative);
    } else {
        snapshot_inline = record.content;
    }

    nlohmann::json payload{
        {"kind", "memory_recall"},
        {"memory_level", record.memory_level},
        {"memory_id", record.memory_id},
        // schema 表的 "u" 只认无符号数;memory 侧的 int 在这里过一道。
        {"memory_schema", static_cast<std::uint64_t>(record.memory_schema)},
        {"memory_updated_at", record.memory_updated_at},
        {"content_sha256", record.content_sha256},
        {"source_evidence_refs", record.source_evidence_refs},
        {"injected_bytes", static_cast<std::uint64_t>(record.injected_bytes)},
    };
    if (!snapshot_ref.empty()) {
        payload["snapshot_ref"] = snapshot_ref;
    }
    if (!snapshot_inline.empty()) {
        payload["snapshot_inline"] = snapshot_inline;
    }

    trajectory::EventScope scope = recorder->base_scope();
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.actor = trajectory::Actor::Host;
    scope.origin = trajectory::Origin::MemoryRecall;
    scope.visibility = {trajectory::Visibility::ModelInput};
    scope.training_policy = trajectory::TrainingPolicy::Exclude;

    trajectory::EventLinks links;
    if (!record.target_run_id.empty()) {
        // 派工快照:父账上记清发给了哪只子代理(relations 键集封闭,
        // child_run_id 本就在集合里)。
        links.child_run_id = record.target_run_id;
    }

    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::ContextInjected;
    request.scope = std::move(scope);
    request.links = std::move(links);
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(request, trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        return std::unexpected("memory.recall_snapshot_failed: context.injected 落账失败: " +
                               receipt.error_code);
    }
    return {};
}

std::string MemoryLedgerBridge::current_session_id() const {
    return ledger_.session_id();
}

std::string MemoryLedgerBridge::RecordSaveRequested(const memory::SaveLedgerNote& note) {
    auto* recorder = ledger_.main();
    if (recorder == nullptr) {
        return std::string();
    }

    nlohmann::json payload{
        {"request",
         nlohmann::json{{"operation", note.operation},
                        {"layer", note.layer},
                        {"kind", note.kind},
                        {"memory_id", note.memory_id},
                        {"title", note.title}}},
        {"source_session", note.source_session},
    };

    trajectory::EventScope scope = recorder->base_scope();
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    // 谁发起的写:user 命令 / 模型工具 / 回合尾抽取,三路各有各的账。
    if (note.originator == "user_command") {
        scope.actor = trajectory::Actor::User;
        scope.origin = trajectory::Origin::ExternalUser;
    } else if (note.originator == "model_tool") {
        scope.actor = trajectory::Actor::Tool;
        scope.origin = trajectory::Origin::BuiltinTool;
    } else {
        scope.actor = trajectory::Actor::Host;
        scope.origin = trajectory::Origin::ScheduledHost;
    }
    scope.visibility = {trajectory::Visibility::HostOnly};
    scope.training_policy = trajectory::TrainingPolicy::Exclude;

    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::MemorySaveRequested;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(request, trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        return std::string();
    }
    return "workspace_key=" + ledger_.workspace_key() + "/session_id=" + ledger_.session_id() +
           "/run_id=" + recorder->base_scope().run_id + "/event_id=" + receipt.event_id;
}

}  // namespace lubancode::app
