#include "app/memory_ledger_bridge.hpp"

#include <filesystem>
#include <system_error>

#include "platform/log_sink.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::app {

namespace {

namespace fs = std::filesystem;

// 合同 §四:≤512B 的小内容允许内联(snapshot_inline/snapshotInline),
// 其余走内容寻址 blob。
constexpr std::size_t kSnapshotInlineLimit = 512;

std::string PathUtf8Text(const fs::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

}  // namespace

MemoryLedgerBridge::MemoryLedgerBridge(runtime::TrajectorySessionLedger& ledger) : ledger_(ledger) {}

// ---------------------------------------------------------------------------
// v3 场:召回注入——快照消息 -> 链接纳 -> 事实行,三步按序落稳
// ---------------------------------------------------------------------------

std::expected<void, std::string> MemoryLedgerBridge::RecordRecallInjection(
    const memory::InjectedMemoryRecord& record) {
    if (auto* v3_writer = ledger_.v3_main_writer()) {
        return RecordRecallInjectionV3(*v3_writer, record);
    }
    // ---- v2 老路(旧档消费期保留,退役归 T02-B) ----
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

std::expected<void, std::string> MemoryLedgerBridge::RecordRecallInjectionV3(
    trajectory::v3::V3Writer& writer, const memory::InjectedMemoryRecord& record) {
    // 派工冻结:父账只落事实,不写隐藏消息、不接纳进链——这段正文发给了
    // 孩子,父模型没见过;链上冒领就是"模型收到了"的假账。子账侧的完整
    // 隐藏消息/采用链归 §4.71(T13-M),本桥不越权。
    if (!record.target_run_id.empty()) {
        nlohmann::json payload{
            {"memoryId", record.memory_id},
            {"memoryLevel", record.memory_level},
            {"memorySchema", static_cast<std::uint64_t>(record.memory_schema)},
            {"memoryUpdatedAt", record.memory_updated_at},
            {"contentSha256", record.content_sha256},
            {"targetRunId", record.target_run_id},
            {"sourceEvidenceRefs", record.source_evidence_refs},
            {"injectedBytes", static_cast<std::uint64_t>(record.injected_bytes)},
        };
        std::string snapshot_ref;
        std::string snapshot_inline;
        if (record.content.size() > kSnapshotInlineLimit) {
            const fs::path artifact_root = ledger_.session_dir() / "artifacts";
            trajectory::BlobStore blobs(artifact_root);
            auto stored =
                blobs.Store(record.content, "text/plain", trajectory::Durability::ProcessCrash);
            if (!stored.has_value()) {
                return std::unexpected("memory.recall_snapshot_failed: " + stored.error());
            }
            if (stored->sha256 != record.content_sha256) {
                return std::unexpected("memory.recall_snapshot_failed: 快照指纹与正文对不上");
            }
            std::error_code ec;
            const fs::path relative =
                fs::relative(blobs.PathFor(stored->sha256), ledger_.session_dir(), ec);
            if (ec) {
                return std::unexpected("memory.recall_snapshot_failed: 快照路径解析失败: " +
                                       ec.message());
            }
            snapshot_ref = PathUtf8Text(relative);
        } else {
            snapshot_inline = record.content;
        }
        if (!snapshot_ref.empty()) payload["snapshotRef"] = snapshot_ref;
        if (!snapshot_inline.empty()) payload["snapshotInline"] = snapshot_inline;
        trajectory::v3::EventDraft draft;
        draft.kind = trajectory::v3::EventKindV3::MemoryRecallInjected;
        draft.payload = std::move(payload);
        const auto receipt = writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
        if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            return std::unexpected("memory.recall_snapshot_failed: memory.recall.injected 落账失败: " +
                                   receipt.error_code);
        }
        return {};
    }

    // 主会话注入:三步按序——快照消息(context_runtime 的隐藏 user,正文
    // 就是当时实际注入的选段)→ AdmitMessages 接纳进链(此后 model.
    // request.prepared 的 inputMessageRefs 沿链自然带上)→ memory.recall.
    // injected 事实(memoryId/revision/hash/messageRef)。前两步落不稳,
    // 本次不注入该条(调用方按 optional 放行);第三步是已发生事实的
    // 收口行,落不稳时注入仍然成立(账上有快照与接纳),只记缺口日志,
    // 不倒过来撤注入——链已长出,回滚不出第二套真相。
    std::string turn_id = record.turn_id;
    if (turn_id.empty()) {
        // 老调用方没递回合号:writer 自家号池补一枚(zero-pad 格式与宿主
        // turn-<n> 不撞名)。注入本体不丢;审计链弱一档,不造假。
        turn_id = writer.NewTurnId();
    }

    trajectory::v3::MessageDraft message;
    message.message_id_override = writer.NewMessageId();
    message.turn_id = turn_id;
    message.purpose = trajectory::v3::MessagePurpose::Conversation;
    // origin 不是 Human:重放/消费侧不许把这行当人类输入再触发召回 Hook。
    message.origin = trajectory::v3::MessageOrigin::ContextRuntime;
    message.display = trajectory::v3::DisplayMode::Hidden;
    message.message = nlohmann::json{{"role", "user"}, {"content", record.content}};
    const auto committed =
        writer.AppendMessage(std::move(message), trajectory::Durability::ProcessCrash);
    if (committed.status != trajectory::v3::WriteReceipt::Status::Committed) {
        return std::unexpected("memory.recall_snapshot_failed: 快照消息落账失败: " +
                               committed.error_code);
    }
    const auto admitted =
        writer.AdmitMessages({committed.id}, trajectory::Durability::ProcessCrash);
    if (admitted.status != trajectory::v3::WriteReceipt::Status::Committed) {
        // 快照消息还在盘上但没有进链:惰性行,链投影不含它,不是假账;
        // 本次照旧不注入(§9.2 注了却无账)。
        return std::unexpected("memory.recall_snapshot_failed: context.input.applied 落账失败: " +
                               admitted.error_code);
    }

    nlohmann::json payload{
        {"memoryId", record.memory_id},
        {"memoryLevel", record.memory_level},
        {"memorySchema", static_cast<std::uint64_t>(record.memory_schema)},
        {"memoryUpdatedAt", record.memory_updated_at},
        {"contentSha256", record.content_sha256},
        {"messageRef", committed.id},
        {"sourceEvidenceRefs", record.source_evidence_refs},
        {"injectedBytes", static_cast<std::uint64_t>(record.injected_bytes)},
    };
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::MemoryRecallInjected;
    draft.turn_id = turn_id;
    draft.payload = std::move(payload);
    const auto receipt = writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
        // 事实行收口失败:注入已成立(快照+接纳都在账上),只留缺口日志,
        // 不撤销、不冒充失败。
        platform::LogSink::Instance().Error(
            "memory", "memory.recall.injected 事实行落不稳,注入本体已在账: " + receipt.error_code);
    }
    return {};
}

std::string MemoryLedgerBridge::current_session_id() const {
    return ledger_.session_id();
}

// ---------------------------------------------------------------------------
// 写入因果边:requested 只记发起,排队/落盘各有各的账
// ---------------------------------------------------------------------------

std::string MemoryLedgerBridge::RecordSaveRequested(const memory::SaveLedgerNote& note) {
    if (auto* v3_writer = ledger_.v3_main_writer()) {
        return RecordSaveRequestedV3(*v3_writer, note);
    }
    // ---- v2 老路 ----
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

std::string MemoryLedgerBridge::RecordSaveRequestedV3(trajectory::v3::V3Writer& writer,
                                                      const memory::SaveLedgerNote& note) {
    // requested 只是因果边:这笔写最终排没排上队看 memory.write.receipted
    // (MemoryTurnLedger 的 v3 写口),落没落盘看 workspace lifecycle 的
    // memory.save.committed(worker 的回执)——三态按真实回执分账,这里
    // 不冒充任何后态。
    nlohmann::json payload{
        {"request",
         nlohmann::json{{"operation", note.operation},
                        {"layer", note.layer},
                        {"kind", note.kind},
                        {"memoryId", note.memory_id},
                        {"title", note.title}}},
        {"sourceSession", note.source_session},
        {"originator", note.originator},
    };
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::MemorySaveRequested;
    draft.payload = std::move(payload);
    const auto receipt = writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
        platform::LogSink::Instance().Error(
            "memory", "memory.save.requested 落不稳,本笔写走无轨迹兜底引用: " + receipt.error_code);
        return std::string();
    }
    return "workspace_key=" + ledger_.workspace_key() + "/session_id=" + writer.session_id() +
           "/run_id=" + writer.run_id() + "/event_id=" + receipt.id;
}

}  // namespace lubancode::app
