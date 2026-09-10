// compact 全链(§4.5-4.8):requested -> 冻结 -> prompt -> prepared -> 候选
// -> 校验 -> 摘要 -> applied(唯一成功终态);失败三态 failed/cancelled/
// rejected。compactId 贯穿,独立内部回合(parentTurnId 挂主 turn,空闲手动
// 为 null)。
//
// 原子采用次序(§4.8,写死):核对源版本 -> 构造新链 -> 摘要消息落稳 ->
// compact.applied 按 PowerLoss 落稳 -> 内存视图由 writer 统一重放换账。
// applied 前崩溃:一切只是候选,旧上下文有效;applied 落稳后崩溃:
// resume 从 applied 重建新上下文(writer::Continue 重放)。
//
// 状态约束:一场 compact 只走一条链;同一主上下文一次只运行一个 compact
// (writer.context().open_compact_ids 非空时 Begin 拒收);Freeze 时核对源
// revision,Apply 时再次核对,变了记 rejected(source_conflict),不静默
// 拼接新尾巴(§4.8)。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::trajectory::v3 {

class CompactSession {
public:
    // 失败终态三型(§4.5):failed=模型请求失败;cancelled=用户取消;
    // rejected=校验不过/收益不足/源版本冲突/no_eligible_history。
    enum class FailKind { Failed, Cancelled, Rejected };

    struct BeginResult {
        WriteReceipt requested;  // compact.requested
        std::string compact_id;
        std::string turn_id;  // 内部回合(§4.6)
        bool began = false;   // false = 拒收(已有进行中的 compact)
        std::string error;
    };

    struct FreezeResult {
        WriteReceipt event;  // compact.started(冻结范围)或 compact.rejected
        bool eligible = true;  // false = no_eligible_history,rejected 已落
        std::string error;
    };

    struct ApplyResult {
        WriteReceipt summary;  // 摘要 user 消息(context_summary,turnId=null)
        WriteReceipt applied;  // compact.applied(唯一成功终态)
        bool ok = false;
        std::string error;
    };

    CompactSession() = default;
    CompactSession(const CompactSession&) = delete;
    CompactSession& operator=(const CompactSession&) = delete;
    CompactSession(CompactSession&&) = default;
    CompactSession& operator=(CompactSession&&) = default;
    ~CompactSession() = default;

    // 开场:compact.requested(trigger=manual/auto、reason、requirements快照)
    // 并建立内部回合。已有未终态 compact 时拒收(合并或拒收并留原因,§4.6)。
    // session 用 unique_ptr:optional<自身> 在类体内是不完整类型,编不过。
    struct BeginOutcome {
        BeginResult info;
        std::unique_ptr<CompactSession> session;  // began=false 时为空
    };
    static BeginOutcome Begin(V3Writer& writer, std::string_view trigger,
                              std::string_view reason,
                              std::optional<std::string> parent_turn_id,
                              nlohmann::json requirements_snapshot,
                              Durability durability = Durability::PowerLoss);

    // 冻结源上下文版本与压缩/保留范围(§4.5 行1"执行时冻结")。
    // removed 为空 = 没有可摘要化历史:落 compact.rejected
    // (reason=no_eligible_history),不空调压缩模型(§4.9)。
    FreezeResult Freeze(V3Writer& writer, std::vector<std::string> removed_message_refs,
                        std::vector<std::string> retained_message_refs,
                        std::vector<std::string> protected_turn_ids,
                        Durability durability = Durability::ProcessCrash);

    // 追加 compact prompt(user,purpose=compact,归内部回合;可多条,§4.5 行3)。
    // 也可经 WriteSpecialSystem 落压缩专用 system(不替换会话 system,§4.3)。
    WriteReceipt AppendPrompt(V3Writer& writer, nlohmann::json user_message,
                              Durability durability = Durability::ProcessCrash);
    WriteReceipt WriteSpecialSystem(V3Writer& writer, std::string_view system_content,
                                    Durability durability = Durability::ProcessCrash);

    // 候选产物:压缩模型实际回复(assistant,purpose=compact;§4.5 行5)。
    // 委托 writer 的流式便利或直接 AppendMessage;这里给一步式。
    // completion_status:压缩回复被输出上限截断时给 Truncated(§4.39 保存
    // 原始部分回复、标 incomplete,不 applied);缺省按完整收尾。
    WriteReceipt WriteCandidate(V3Writer& writer, nlohmann::json assistant_message,
                                std::string_view request_id, std::string_view step_id,
                                std::string_view provider, std::string_view wire,
                                std::string_view model, nlohmann::json usage,
                                Durability durability = Durability::PowerLoss,
                                std::optional<CompletionStatus> completion_status = std::nullopt);

    // 校验(§4.7):started + completed(passed 与逐项 checks)。checks 每项
    // 至少 {code, passed};失败项另带证据引用。
    WriteReceipt StartValidation(V3Writer& writer,
                                 Durability durability = Durability::ProcessCrash);
    WriteReceipt CompleteValidation(V3Writer& writer, bool passed,
                                    std::vector<nlohmann::json> checks,
                                    Durability durability = Durability::PowerLoss);

    // 生效:摘要消息落稳 -> compact.applied(PowerLoss)。
    // 提交前再次核对源版本(§4.8"源未变化");变了 rejected(source_conflict)。
    // 新链 = 当前 system + 摘要 + 有序 retained(§4.30);removed 只退出
    // 模型上下文,原档不删。
    ApplyResult Apply(V3Writer& writer, std::string_view summary_content,
                      std::uint64_t context_tokens_before, std::uint64_t context_tokens_after,
                      nlohmann::json token_metric,
                      Durability durability = Durability::PowerLoss);

    // 失败终态(§4.5):不更新 contextRevision,不显示"压缩完成";候选正文
    // 与失败检查仍在档。reason 必填。
    WriteReceipt Fail(V3Writer& writer, FailKind kind, std::string_view reason,
                      Durability durability = Durability::PowerLoss);

    const std::string& compact_id() const { return compact_id_; }
    const std::string& turn_id() const { return turn_id_; }
    std::optional<std::string> parent_turn_id() const { return parent_turn_id_; }
    std::uint64_t source_revision() const { return source_revision_; }
    bool finished() const { return finished_; }

private:
    CompactSession(std::string compact_id, std::string turn_id,
                   std::optional<std::string> parent_turn_id, std::uint64_t source_revision);
    std::string compact_id_;
    std::string turn_id_;
    std::optional<std::string> parent_turn_id_;
    std::string trigger_;
    std::uint64_t source_revision_ = 0;
    std::vector<std::string> removed_;
    std::vector<std::string> retained_;
    std::vector<std::string> protected_turns_;
    std::optional<std::string> candidate_message_id_;
    std::optional<std::string> validation_event_id_;
    bool frozen_ = false;
    bool finished_ = false;
};

}  // namespace lubancode::trajectory::v3
