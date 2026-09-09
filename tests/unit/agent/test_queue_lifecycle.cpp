// steer/followup 显式化(SessionTurnStepAction 四层单 P3):队列条目的
// 意图/截止/过期与 queued→claimed→committed/returned 状态机。
//
//   1) 意图:Enqueue(两参)= steer 缺省;EnqueueWithIntent 落 intent/
//      deadline/target_turn;
//   2) 状态机:ClaimDeliverable 翻 Claimed(留队、不重取、冻编辑),
//      MarkCommitted 出队(committed);失败退回 ReturnToFront =
//      returned(Queued + attempts 账);
//   3) 过期:Turn 终局未消费的 steer 打标注、不再投递、CommitEdit 翻新;
//      slash 是轮末账不过期;
//   4) 断线对齐:RestoreFromArchive 把 Claimed 归回 Queued。

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cli/queue_model.hpp"

using namespace lubancode;
using cli::MessageTarget;
using cli::QueueDeadline;
using cli::QueueIntent;
using cli::QueueItemState;
using cli::SteeringQueue;

namespace {

std::vector<std::string> TextsOf(const std::vector<cli::QueuedMessage>& items) {
    std::vector<std::string> out;
    for (const auto& item : items) {
        out.push_back(item.text);
    }
    return out;
}

// 按 id 取条目的快照副本(值语义——快照是临时 vector,指针出了表达式
// 就是悬空,不许用)。
std::optional<cli::QueuedMessage> Find(const SteeringQueue& queue, cli::QueueId id) {
    for (const auto& item : queue.Snapshot()) {
        if (item.id == id) {
            return item;
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("意图:两参 Enqueue = steer 缺省;显式入队落全字段") {
    SteeringQueue queue;
    const cli::QueueId steer_id = queue.Enqueue(MessageTarget::Main(), "改用方案 B");
    const cli::QueueId followup_id = queue.EnqueueWithIntent(MessageTarget::Main(), "接着做第二件事",
                                                             QueueIntent::Followup, QueueDeadline::TurnEnd, "turn-3");
    REQUIRE(steer_id != 0);
    REQUIRE(followup_id != 0);
    const auto snapshot = queue.Snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot[0].intent == QueueIntent::Steer);
    CHECK(snapshot[0].deadline == QueueDeadline::NextStep);
    CHECK(snapshot[0].target_turn.empty());
    CHECK(snapshot[1].intent == QueueIntent::Followup);
    CHECK(snapshot[1].deadline == QueueDeadline::TurnEnd);
    CHECK(snapshot[1].target_turn == "turn-3");
}

TEST_CASE("状态机:claim 留队不重取,commit 出队;claimed 冻编辑") {
    SteeringQueue queue;
    const cli::QueueId a = queue.Enqueue(MessageTarget::Main(), "第一条");
    const cli::QueueId b = queue.Enqueue(MessageTarget::Main(), "第二条");

    // claim:条目翻 Claimed、留在队里(窗口态),副本带正文与 id。
    const auto claimed = queue.ClaimDeliverable(MessageTarget::Main());
    REQUIRE(claimed.size() == 2);
    CHECK(TextsOf(claimed) == std::vector<std::string>{"第一条", "第二条"});
    const auto after_claim = Find(queue, a);
    REQUIRE(after_claim.has_value());
    CHECK(after_claim->state == QueueItemState::Claimed);
    CHECK(queue.size() == 2);  // 没出队:取走不等于消费

    // 在途不重取:第二批 claim 空手而归。
    CHECK(queue.ClaimDeliverable(MessageTarget::Main()).empty());
    // 在途冻编辑。
    CHECK_FALSE(queue.BeginEdit(a).has_value());
    CHECK_FALSE(queue.BeginEditLatest().has_value());

    // commit:随请求进史才销账——出队。
    CHECK(queue.MarkCommitted({a, b}) == 2);
    CHECK(queue.empty());

    // 幂等:再 commit 认不得,回 0。
    CHECK(queue.MarkCommitted({a}) == 0);
}

TEST_CASE("状态机:pump 取件 claim→commit;失败退回 = returned(Queued+attempts)") {
    SteeringQueue queue;
    // 泵的取件范围是 slash 与显式 followup(§五.4/§五.5):普通 steer 缺省
    // 不走泵。这里用 followup 走全状态机。
    queue.EnqueueWithIntent(MessageTarget::Main(), "轮末话", QueueIntent::Followup, QueueDeadline::TurnEnd);

    auto head = queue.ClaimFirstAutoSendable(MessageTarget::Main());
    REQUIRE(head.has_value());
    CHECK(Find(queue, head->id)->state == QueueItemState::Claimed);

    // 失败退回:returned 归 Queued、attempts+1、原 id 保留。
    const cli::QueueId head_id = head->id;
    queue.ReturnToFront(std::move(*head));
    const auto returned = Find(queue, head_id);
    REQUIRE(returned.has_value());
    CHECK(returned->state == QueueItemState::Queued);
    CHECK(returned->delivery_attempts == 1);
    CHECK(queue.size() == 1);

    // 首次退回后还容一次自动重试(首发+重试 = kMaxAutoSendAttempts)。
    auto retry = queue.ClaimFirstAutoSendable(MessageTarget::Main());
    REQUIRE(retry.has_value());
    queue.ReturnToFront(std::move(*retry));
    CHECK(Find(queue, head_id)->delivery_attempts == 2);
    // 到顶:不再自动取。
    CHECK_FALSE(queue.ClaimFirstAutoSendable(MessageTarget::Main()).has_value());

    // 成功路:用户改写翻新(attempts 清零)→ claim → commit 出队。
    auto handle = queue.BeginEdit(head_id);
    REQUIRE(handle.has_value());
    CHECK(queue.CommitEdit(*handle, "改写后再送") == SteeringQueue::CommitStatus::Ok);
    auto redo = queue.ClaimFirstAutoSendable(MessageTarget::Main());
    REQUIRE(redo.has_value());
    CHECK(redo->delivery_attempts == 0);
    CHECK(queue.MarkCommittedOne(redo->id));
    CHECK(queue.empty());
}

TEST_CASE("过期:Turn 终局未消费的 steer 打标注,不再投,改写翻新") {
    SteeringQueue queue;
    const cli::QueueId steer_id = queue.Enqueue(MessageTarget::Main(), "没赶上的话");
    const cli::QueueId slash_id = queue.Enqueue(MessageTarget::Main(), "/context");
    queue.EnqueueWithIntent(MessageTarget::Main(), "下一轮的话", QueueIntent::Followup, QueueDeadline::TurnEnd);

    const auto expired = queue.MarkExpiredUnconsumedSteers("turn_end_no_next_step");
    REQUIRE(expired.size() == 1);
    CHECK(expired[0] == steer_id);

    // 过期的保持 Queued + 标注,不变 followup、不出队。
    const auto expired_item = Find(queue, steer_id);
    REQUIRE(expired_item.has_value());
    CHECK(expired_item->state == QueueItemState::Queued);
    CHECK(expired_item->expiry_note == "turn_end_no_next_step");
    CHECK(expired_item->intent == QueueIntent::Steer);

    // 过期 steer 不投:claim/取走/自动发送全让路。
    CHECK(queue.ClaimDeliverable(MessageTarget::Main()).empty());
    CHECK(queue.TakeDeliverable(MessageTarget::Main()).empty());
    // 但 followup 与 slash 照常可泵(slash 走泵本地执行,followup 起新轮)。
    auto head = queue.ClaimFirstAutoSendable(MessageTarget::Main());
    REQUIRE(head.has_value());
    CHECK(head->text == "/context");  // 落队顺序:slash 在前
    queue.MarkCommittedOne(head->id);
    auto next = queue.ClaimFirstAutoSendable(MessageTarget::Main());
    REQUIRE(next.has_value());
    CHECK(next->intent == QueueIntent::Followup);
    queue.MarkCommittedOne(next->id);
    // 过期的那条还躺着,等用户。
    REQUIRE(queue.size() == 1);
    CHECK(queue.Snapshot()[0].id == steer_id);

    // 用户取回改写 = 明示去留:标注清、重新排队可投。
    auto handle = queue.BeginEdit(steer_id);
    REQUIRE(handle.has_value());
    CHECK(queue.CommitEdit(*handle, "改写后的话") == SteeringQueue::CommitStatus::Ok);
    const auto renewed = Find(queue, steer_id);
    REQUIRE(renewed.has_value());
    CHECK(renewed->expiry_note.empty());
    const auto reclaimed = queue.ClaimDeliverable(MessageTarget::Main());
    REQUIRE(reclaimed.size() == 1);
    CHECK(reclaimed[0].text == "改写后的话");
}

TEST_CASE("断线对齐:RestoreFromArchive 把 Claimed 归回 Queued 并记一笔") {
    SteeringQueue queue;
    queue.Enqueue(MessageTarget::Main(), "取走到一半的话");
    const auto claimed = queue.ClaimDeliverable(MessageTarget::Main());
    REQUIRE(claimed.size() == 1);

    // 模拟断线后 resume:存档里仍是 Claimed 的条目,重开队列归回 Queued。
    SteeringQueue restored;
    REQUIRE(restored.RestoreFromArchive(queue.Snapshot()));
    const auto snapshot = restored.Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == QueueItemState::Queued);
    CHECK(snapshot[0].note.find("断线恢复") != std::string::npos);
    // 归回的条目可再投(没消费过,不算 committed)。
    const auto reclaimed = restored.ClaimDeliverable(MessageTarget::Main());
    REQUIRE(reclaimed.size() == 1);
}

TEST_CASE("子代理目标:claim/commit 状态机与 main 互不串账") {
    SteeringQueue queue;
    queue.Enqueue(MessageTarget::Agent(3), "给三号的话");
    queue.Enqueue(MessageTarget::Main(), "给主会话的话");

    const auto for_agent = queue.ClaimDeliverable(MessageTarget::Agent(3));
    REQUIRE(for_agent.size() == 1);
    CHECK(for_agent[0].text == "给三号的话");
    // main 的没动。
    CHECK(Find(queue, queue.Snapshot()[1].id)->state == QueueItemState::Queued);
    CHECK(queue.MarkCommitted({for_agent[0].id}) == 1);
    CHECK(queue.size() == 1);
}
