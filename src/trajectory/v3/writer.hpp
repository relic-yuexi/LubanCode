// v3 会话写者(P1 写入侧核心):单写者发号、哈希链、两类行落盘、
// system 版本链、请求输入留档、流式片段与 assistant 定稿。
//
// 合同见 docs/architecture/trajectory-v3-schema.md。写盘复用 v2
// JournalWriter(canonical JSON + Durability 三档);哈希链承继 v2 算法。
//
// 崩溃窗口语义(§4.3/§4.8):
//   - SwitchSystem 三步(change 事件 -> 新 system -> context.system.applied)
//     走完,内存才换根;中途崩溃 Continue 恢复后仍用旧根,变更显示未完成。
//   - compact 的原子提交在 compact.hpp(CompactSession);writer 只提供
//     底层行写与链状态。
//
// 不在本棒:工具执行细节、hook 效果、subagent、读取侧投影(P2)。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/schema3.hpp"

namespace lubancode::trajectory::v3 {

// ---------------------------------------------------------------------------
// 基础类型
// ---------------------------------------------------------------------------

struct WriteReceipt {
    enum class Status { Committed, Rejected, IoFailed } status = Status::Rejected;
    std::string id;  // messageId 或 eventId
    std::uint64_t seq = 0;
    std::string line_hash;
    std::string error_code;     // 稳定错误码
    std::string error_message;  // 人话(io 细节/缺哪个字段)
};

// 时间注入(单测喂固定钟)。
struct V3Clock {
    virtual ~V3Clock() = default;
    virtual std::int64_t WallMs() const;
};

struct V3WriterOptions {
    std::string writer_version = "trajectory-v3-writer-1";
    // 注入提交失败(测试专用;生产恒空):返回稳定码则该枚提交按 IoFailed
    // 收(writer 句柄随后 broken)。锁内调用,须廉价无副作用。
    std::function<std::optional<std::string>()> inject_io_failure;
};

// 链节点(schema 文档 §2.4)。
struct ChainNode {
    std::string message_ref;
    std::optional<std::string> prev_message_ref;  // 根为 null

    nlohmann::json ToJson() const {
        nlohmann::json node = nlohmann::json::object();
        node["messageRef"] = message_ref;
        node["prevMessageRef"] = prev_message_ref.has_value()
                                     ? nlohmann::json(*prev_message_ref)
                                     : nlohmann::json(nullptr);
        return node;
    }
};

// 恢复/重放后的上下文视图快照。
struct ContextView {
    std::string context_id = "main";
    std::uint64_t revision = 0;
    std::vector<ChainNode> chain;      // 根为当前 system,根到尾有序
    std::string system_message_ref;    // == chain.front().message_ref
    // 未完成 compact(无终态)按源上下文恢复,内部回合不冒充主链。
    std::vector<std::string> open_compact_ids;
};

// ---------------------------------------------------------------------------
// 写草稿:调用方填语义字段,writer 发 seq/timestamp/id/hash
// ---------------------------------------------------------------------------

struct MessageDraft {
    std::optional<std::string> turn_id;  // system/摘要给 nullopt(落 null)
    std::optional<std::string> parent_turn_id;
    std::optional<std::string> step_id;
    std::optional<std::string> request_id;
    std::optional<std::string> action_id;
    std::optional<std::string> compact_id;
    MessagePurpose purpose = MessagePurpose::Conversation;
    MessageOrigin origin = MessageOrigin::Human;
    std::optional<DisplayMode> display;
    nlohmann::json message = nlohmann::json::object();
    std::optional<std::string> caused_by_event_ref;
    std::optional<std::string> source_message_ref;
    std::optional<nlohmann::json> system_meta;
    std::optional<CompletionStatus> completion_status;
    std::optional<std::string> provider;
    std::optional<std::string> wire;
    std::optional<std::string> model;
    std::optional<nlohmann::json> response_model;  // json: string 或 null
    std::optional<std::string> provider_config_ref;
    std::optional<std::string> model_profile_ref;
    std::optional<nlohmann::json> usage;  // json: object 或 null
};

struct EventDraft {
    EventKindV3 kind = EventKindV3::SessionStarted;
    std::optional<OpStatus> status;  // 须匹配 §2.2 映射
    std::optional<std::string> turn_id;
    std::optional<std::string> parent_turn_id;
    std::optional<std::string> step_id;
    std::optional<std::string> request_id;
    std::optional<std::string> action_id;
    std::optional<std::string> compact_id;
    std::optional<std::string> command_id;
    std::optional<std::string> hook_dispatch_id;
    std::optional<std::string> task_id;
    std::optional<std::string> title_generation_id;
    nlohmann::json payload = nlohmann::json::object();
    std::optional<std::vector<nlohmann::json>> effects;
    std::optional<std::vector<nlohmann::json>> effect_refs;
};

// ---------------------------------------------------------------------------
// V3Writer
// ---------------------------------------------------------------------------

class V3Writer {
public:
    V3Writer() = default;
    V3Writer(V3Writer&&) noexcept;
    V3Writer& operator=(V3Writer&&) noexcept;
    V3Writer(const V3Writer&) = delete;
    V3Writer& operator=(const V3Writer&) = delete;
    ~V3Writer();

    // 开新卷:首行 system(seq=1、turnId=null、systemMeta.cause=initial)
    // + session.started(revision 1 单节点链)。system_content 须是拼装后
    // 完整正文(§4.3:恢复不重拼)。已存在文件拒开(create-new)。
    static std::expected<V3Writer, std::string> Start(
        const std::filesystem::path& jsonl_path, std::string_view session_id,
        std::string_view run_id, std::string_view system_content,
        nlohmann::json system_extra = nlohmann::json::object(),
        V3WriterOptions options = V3WriterOptions{}, const V3Clock* clock = nullptr);

    // 续卷:逐行验(严格解析 + 语义校验 + seq 连续 + 哈希衔接),重放链
    // 状态到内存视图;验不过拒开(错误码 v3writer.*)。尾行截断明报拒开
    // (删尾修复归读取侧 §4.60,写入侧不偷偷裁)。
    static std::expected<V3Writer, std::string> Continue(
        const std::filesystem::path& jsonl_path, V3WriterOptions options = V3WriterOptions{},
        const V3Clock* clock = nullptr);

    // ---- 底层两类行 ----

    WriteReceipt AppendMessage(MessageDraft draft, Durability durability);
    WriteReceipt AppendEvent(EventDraft draft, Durability durability);

    // ---- 上下文链 ----

    // 把已落稳的消息按序接纳进当前上下文:逐枚追加链节点并提交
    // context.input.applied(appendedChain)。message_ids 必须都已在账上。
    // revision 一次 +1(一批一条提交事件,§4.30"一组节点先全部落稳再提交")。
    WriteReceipt AdmitMessages(std::vector<std::string> message_ids,
                               Durability durability = Durability::PowerLoss);

    // system 版本切换三步(§4.3):system.change -> 新 system 消息 ->
    // context.system.applied(链根换新 system,后续节点重接)。
    // 三步全过内存才换根;第 2 步后崩溃,恢复仍用旧根(变更未完成)。
    // settings_version/soul 等缘由进 change 元数据;system_changed=false
    // 表示设置变了但正文未变,仍走三步留档,不制造假版本差异(链重接)。
    struct SwitchSystemResult {
        WriteReceipt change_event;
        WriteReceipt system_message;
        WriteReceipt apply_event;  // context.system.applied
    };
    SwitchSystemResult SwitchSystem(std::string_view new_system_content,
                                    nlohmann::json change_payload,  // cause/settingsVersion/...
                                    MessageOrigin origin = MessageOrigin::SessionRuntime,
                                    Durability durability = Durability::PowerLoss);

    // ---- 请求输入留档(§4.4) ----

    // model.request.prepared。引用先落稳才许发:system_message_ref 与
    // input_message_refs 里每个 id 必须已在账上,否则 Rejected
    // (v3writer.dangling_ref),请求不得发出。
    WriteReceipt PrepareRequest(std::string_view request_id, std::string_view turn_id,
                                std::string_view step_id, std::string_view purpose,
                                std::string_view system_message_ref,
                                const std::vector<std::string>& input_message_refs,
                                nlohmann::json provider_snapshot,  // provider/model/wire/参数/工具定义引用
                                std::optional<std::string> compact_id = std::nullopt,
                                Durability durability = Durability::ProcessCrash);

    // ---- 流式(§4.43) ----

    // 响应开始:为目标 assistant 预留 messageId(事件引用它,最终 message
    // 才以该 id 成行)。provider/wire/model 流式开始前即可固定。
    WriteReceipt BeginStreamResponse(std::string_view request_id, std::string_view stream_id,
                                     std::string_view turn_id, std::string_view step_id,
                                     std::string_view reserved_message_id,
                                     Durability durability = Durability::ProcessCrash);

    // 片段批次:流内递增 sequence + deltaType(text/reasoning/tool_args/
    // signature/block_start/block_end),批次带首尾序号。默认 ProcessCrash
    // (批内 Buffered 由调用方攒批,终态必须 PowerLoss)。
    WriteReceipt AppendStreamDelta(std::string_view request_id, std::string_view stream_id,
                                   std::string_view message_id, std::uint64_t sequence,
                                   std::string_view delta_type, nlohmann::json content,
                                   Durability durability = Durability::ProcessCrash);

    // 收齐:model.response.completed + 完整 assistant message(预留 id)+
    // 接纳进上下文(purpose=conversation 时)。finishReason 由调用方按
    // 协议判定;length 截断给 completion_status=truncated。
    WriteReceipt CompleteStreamResponse(std::string_view request_id, std::string_view stream_id,
                                        std::string_view turn_id, std::string_view step_id,
                                        std::string_view message_id, nlohmann::json message,
                                        std::string_view provider, std::string_view wire,
                                        std::string_view model, nlohmann::json response_model,
                                        nlohmann::json usage, std::string_view finish_reason,
                                        MessagePurpose purpose = MessagePurpose::Conversation,
                                        std::optional<std::string> compact_id = std::nullopt,
                                        std::optional<CompletionStatus> completion_status =
                                            std::nullopt,
                                        Durability durability = Durability::PowerLoss);

    // Esc 中断(§4.63):model.response.cancelled(记录接收水位)+ 已收到
    // 内容组装成正式 assistant(completionStatus=interrupted;usage 缺实报
    // 给 null 不补零;未收齐的调用/签名不伪造完整)。
    WriteReceipt InterruptStreamResponse(std::string_view request_id, std::string_view stream_id,
                                         std::string_view turn_id, std::string_view step_id,
                                         std::string_view message_id, nlohmann::json message,
                                         std::string_view provider, std::string_view wire,
                                         std::string_view model, std::uint64_t received_through,
                                         std::optional<nlohmann::json> usage,
                                         MessagePurpose purpose = MessagePurpose::Conversation,
                                         std::optional<std::string> compact_id = std::nullopt,
                                         Durability durability = Durability::PowerLoss);

    // ---- 身份发号(领域 ID 由调用方/领域层用) ----

    std::string NewMessageId();
    std::string NewEventId();
    std::string NewTurnId();     // turn-<n>,内部回合同池发号(§4.6)
    std::string NewStepId();     // step-<n>
    std::string NewRequestId();  // request-<n>
    std::string NewStreamId();   // stream-<n>
    std::string NewCompactId();  // compact-<n>

    // ---- 观测 ----

    const std::filesystem::path& path() const;
    std::uint64_t next_seq() const;
    std::string last_line_hash() const;
    bool broken() const;
    const ContextView& context() const;  // 当前内存视图(链/版本/当前 system)
    // 本账上是否已有该 messageId(PrepareRequest 引用先落稳的判据)。
    bool HasMessageId(std::string_view message_id) const;
    const std::string& session_id() const;
    const std::string& run_id() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit V3Writer(std::unique_ptr<Impl> impl);
};

// ---------------------------------------------------------------------------
// 验卷(P2 读取侧的地基;writer::Continue 与校验脚本共用语义)
// ---------------------------------------------------------------------------

struct V3VerifyReport {
    bool ok = false;
    bool truncated_tail = false;  // 尾行缺 '\n'(崩溃截断)
    std::uint64_t lines = 0;
    std::string error_code;
    std::string message;
    ContextView context;  // 验过后重放的内存视图
};

// 逐行验:严格解析、语义校验、seq 从 1 连续、prevHash/lineHash 衔接;
// 四类提交事件重放链状态。失败给首错。
V3VerifyReport VerifyV3File(const std::filesystem::path& path);

// 单行级:解析 + 校验 + 哈希衔接判定(prev_hash 给定,重算 lineHash)。
// 给 fixture 测试与验卷共用。
std::optional<Schema3Error> VerifyLine(const nlohmann::json& line, std::string_view prev_hash,
                                       std::uint64_t expect_seq);

}  // namespace lubancode::trajectory::v3
