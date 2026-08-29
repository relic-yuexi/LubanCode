// 会话层排队队列 SteeringQueue(0.28.x"排队消息在工具边界送达并可
// Shift+左键编辑")的纯逻辑测试:落队顺序与目标、投递按目标分账、编辑事务
// (版本冲突/冻结/删除)、终态目标标注、立即送状态旗、显示成行(标题随
// 状态变、三条窗口、编辑中标记)。取走即消费单(2026-08)另钉:自动发送
// 失败回队与防死循环闸、存档恢复(RestoreFromArchive)、清账告知成行
// (BuildQueueDisposalRows)。全部走局部实例,不碰全局
// SessionSteeringQueue。流式输入行的 slash 提示随 StreamSlashHintLines 删除
// 改钉编辑器 RenderState(tests/unit/cli/test_line_editor.cpp 的忙碌路用例)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/queue_model.hpp"
#include "cli/slash_commands.hpp"

using lubancode::cli::BuildQueueArchiveRows;
using lubancode::cli::BuildQueueDisposalRows;
using lubancode::cli::BuildSteeringQueueRows;
using lubancode::cli::DeliveryMode;
using lubancode::cli::IsQueuedSlashText;
using lubancode::cli::MessageTarget;
using lubancode::cli::ParseSlashCommand;
using lubancode::cli::QueueItemState;
using lubancode::cli::QueueTextAdmittedDuringBusy;
using lubancode::cli::QueueTitleMode;
using lubancode::cli::QueueViewOptions;
using lubancode::cli::QueuedMessage;
using lubancode::cli::SlashCommand;
using lubancode::cli::SlashCommandQueueableDuringBusy;
using lubancode::cli::SteeringQueue;
using Status = SteeringQueue::CommitStatus;

namespace {

// 按 id 找条目(测试断言用;找不到就给个带 id=0 的哑条目)。
QueuedMessage Find(const std::vector<QueuedMessage>& items, lubancode::cli::QueueId id) {
    for (const auto& item : items) {
        if (item.id == id) {
            return item;
        }
    }
    return QueuedMessage{};
}

}  // namespace

TEST_CASE("落队:顺序保持、目标分账;空文本拒收") {
    SteeringQueue q;
    const auto main_id = q.Enqueue(MessageTarget::Main(), "第一句");
    const auto agent_id = q.Enqueue(MessageTarget::Agent(3), "给三号的");
    const auto empty_id = q.Enqueue(MessageTarget::Main(), "");
    CHECK(main_id != 0);
    CHECK(agent_id != 0);
    CHECK(main_id != agent_id);
    CHECK(empty_id == 0);  // 空文本不落队

    const auto snapshot = q.Snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot[0].text == "第一句");        // 顺序 = 落队顺序
    CHECK(snapshot[0].target == MessageTarget::Main());
    CHECK(snapshot[1].target == MessageTarget::Agent(3));
    CHECK(snapshot[1].target.task_id == 3);
    CHECK(snapshot[0].state == QueueItemState::Queued);
    CHECK(q.size() == 2);
}

TEST_CASE("投递按目标分账:main 的取件不带走子代理的,反之亦然") {
    SteeringQueue q;
    const auto m1 = q.Enqueue(MessageTarget::Main(), "m1");
    const auto a1 = q.Enqueue(MessageTarget::Agent(2), "a1");
    const auto m2 = q.Enqueue(MessageTarget::Main(), "m2");
    const auto a2 = q.Enqueue(MessageTarget::Agent(5), "a2");
    const auto other = q.Enqueue(MessageTarget::Agent(2), "another");

    auto main_items = q.TakeDeliverable(MessageTarget::Main());
    REQUIRE(main_items.size() == 2);
    CHECK(main_items[0].id == m1);  // main 目标内部保持落队顺序
    CHECK(main_items[1].id == m2);

    auto agent2_items = q.TakeDeliverable(MessageTarget::Agent(2));
    REQUIRE(agent2_items.size() == 2);
    CHECK(agent2_items[0].id == a1);
    CHECK(agent2_items[1].id == other);

    // 只剩五号那条;取空后 HasDeliverable 对所有目标都是假。
    CHECK(q.size() == 1);
    CHECK(q.HasDeliverable(MessageTarget::Agent(5)));
    CHECK_FALSE(q.HasDeliverable(MessageTarget::Main()));
    CHECK_FALSE(q.HasDeliverable(MessageTarget::Agent(2)));
    CHECK(q.TakeFirstDeliverable(MessageTarget::Main()) == std::nullopt);
    const auto head = q.TakeFirstDeliverable(MessageTarget::Agent(5));
    REQUIRE(head.has_value());
    CHECK(head->id == a2);
    CHECK(q.empty());
}

TEST_CASE("编辑事务:取回装原文、Enter 原位替换保 id/目标/次序") {
    SteeringQueue q;
    const auto id1 = q.Enqueue(MessageTarget::Main(), "旧话一");
    const auto id2 = q.Enqueue(MessageTarget::Agent(4), "旧话二");

    auto handle = q.BeginEditLatest();
    REQUIRE(handle.has_value());
    CHECK(handle->id == id2);                    // 默认取最新落队那条
    CHECK(handle->text == "旧话二");
    CHECK(handle->target == MessageTarget::Agent(4));

    // 冻结中的条目:不可再开编辑、也不可投递。
    CHECK(q.BeginEdit(id2) == std::nullopt);
    CHECK(q.TakeDeliverable(MessageTarget::Agent(4)).empty());
    CHECK_FALSE(q.HasDeliverable(MessageTarget::Agent(4)));
    CHECK(q.editable_size() == 1);  // 冻结的不算可编辑
    CHECK(q.HasDeliverable(MessageTarget::Main()));  // 别的目标不受牵连

    CHECK(q.CommitEdit(*handle, "新话二") == Status::Ok);
    const auto snapshot = q.Snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot[1].id == id2);  // id 保住
    CHECK(snapshot[1].text == "新话二");
    CHECK(snapshot[1].target == MessageTarget::Agent(4));  // 目标保住
    CHECK(snapshot[1].version == 2);
    CHECK_FALSE(snapshot[1].edit_open);  // 解冻,可投递了
    CHECK(q.HasDeliverable(MessageTarget::Agent(4)));
    CHECK(snapshot[0].id == id1);
    CHECK(snapshot[0].text == "旧话一");  // 其余条目原样,位置不动
}

TEST_CASE("版本冲突:凭据过期后提交失败,原文不被覆盖") {
    SteeringQueue q;
    const auto id = q.Enqueue(MessageTarget::Main(), "原文");

    auto stale = q.BeginEditLatest();
    REQUIRE(stale.has_value());

    // 同一条被另一只事务改过(先取消,再开新事务提交,旧凭据变过期)。
    REQUIRE(q.CancelEdit(*stale) == Status::Ok);
    auto fresh = q.BeginEditLatest();
    REQUIRE(fresh.has_value());
    REQUIRE(q.CommitEdit(*fresh, "新文") == Status::Ok);

    CHECK(q.CommitEdit(*stale, "拿旧凭据改") == Status::Conflict);
    CHECK(Find(q.Snapshot(), id).text == "新文");  // 原文(新文)没被旧凭据覆盖

    // 已出队(送达)的条目:提交按 NotFound 报,不复活。
    auto doomed = q.BeginEditLatest();
    REQUIRE(doomed.has_value());
    REQUIRE(q.CancelEdit(*doomed) == Status::Ok);
    REQUIRE_FALSE(q.TakeDeliverable(MessageTarget::Main()).empty());
    CHECK(q.CommitEdit(*doomed, "送达后想改") == Status::NotFound);
}

TEST_CASE("Esc 还原:取消编辑放回原文;Del 删除;Remove 兜底") {
    SteeringQueue q;
    q.Enqueue(MessageTarget::Main(), "要还原的");
    auto handle = q.BeginEditLatest();
    REQUIRE(handle.has_value());
    CHECK(q.CancelEdit(*handle) == Status::Ok);
    const auto snapshot = q.Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].text == "要还原的");
    CHECK_FALSE(snapshot[0].edit_open);
    CHECK(q.CancelEdit(*handle) == Status::Conflict);  // 已解冻再取消 = 冲突

    // 删除走编辑事务。
    auto del_handle = q.BeginEditLatest();
    REQUIRE(del_handle.has_value());
    CHECK(q.DeleteMessage(*del_handle) == Status::Ok);
    CHECK(q.empty());
    CHECK(q.DeleteMessage(*del_handle) == Status::NotFound);

    const auto id2 = q.Enqueue(MessageTarget::Agent(1), "直接删");
    CHECK(q.Remove(id2));
    CHECK(q.empty());
    CHECK_FALSE(q.Remove(id2));
}

TEST_CASE("终态目标:MarkTargetGone/MarkFailed 留原位标错,不参与投递") {
    SteeringQueue q;
    const auto gone = q.Enqueue(MessageTarget::Agent(9), "给已结束代理的");
    const auto ok = q.Enqueue(MessageTarget::Agent(9), "还给机会的");
    const auto failed = q.Enqueue(MessageTarget::Main(), "投错了的");

    q.MarkTargetGone(gone, "目标子代理已结束");
    q.MarkFailed(failed, "发送通道断了");

    auto deliverable = q.TakeDeliverable(MessageTarget::Agent(9));
    REQUIRE(deliverable.size() == 1);
    CHECK(deliverable[0].id == ok);  // 只有健康的条目参与投递

    const auto snapshot = q.Snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(Find(snapshot, gone).state == QueueItemState::TargetGone);
    CHECK(Find(snapshot, gone).note == "目标子代理已结束");
    CHECK(Find(snapshot, failed).state == QueueItemState::Failed);

    // TargetGone 的条目还能取回编辑(用户要改目标或删掉)。
    auto handle = q.BeginEdit(gone);
    REQUIRE(handle.has_value());
    CHECK(handle->text == "给已结束代理的");
}

TEST_CASE("立即送状态旗:Esc 翻旗、送空后收旗;落队消息记下策略") {
    SteeringQueue q;
    CHECK_FALSE(q.immediate_delivery_requested());
    CHECK_FALSE(q.HasAnyDeliverable());
    q.Enqueue(MessageTarget::Main(), "等着送的");
    CHECK(q.HasAnyDeliverable());  // 有可送的,Esc 才翻"立即送"旗
    q.RequestImmediateDelivery();
    CHECK(q.immediate_delivery_requested());
    // 旗子翻过后落队的消息策略记 Immediate(信息性字段,投递仍只在安全点)。
    const auto late = q.Enqueue(MessageTarget::Main(), "打断后补的");
    CHECK(Find(q.Snapshot(), late).delivery == DeliveryMode::Immediate);

    q.TakeDeliverable(MessageTarget::Main());
    CHECK_FALSE(q.HasAnyDeliverable());  // 都送走了:没有可"立即送"的东西
    q.ClearImmediateDelivery();
    CHECK_FALSE(q.immediate_delivery_requested());
    // 终态条目不算可送:只有 TargetGone 时 Esc 不翻立即送旗(打断归打断)。
    const auto gone = q.Enqueue(MessageTarget::Agent(1), "目标没了");
    q.MarkTargetGone(gone, "gone");
    CHECK_FALSE(q.HasAnyDeliverable());
}

TEST_CASE("收场处置:TakeAllForDisposal 一次交出全部并清空") {
    SteeringQueue q;
    q.Enqueue(MessageTarget::Main(), "没送出的");
    q.Enqueue(MessageTarget::Agent(2), "也没送出");
    q.MarkTargetGone(q.Enqueue(MessageTarget::Agent(3), "目标没了的"), "gone");
    const auto discarded = q.TakeAllForDisposal();
    CHECK(discarded.size() == 3);
    CHECK(q.empty());
    CHECK(q.TakeAllForDisposal().empty());
    CHECK_FALSE(q.immediate_delivery_requested());
}

// ---------------------------------------------------------------------------
// 取走即消费单(2026-08):回合失败还队 + 存档恢复 + 清账告知
// ---------------------------------------------------------------------------

TEST_CASE("自动发送失败回队:回队首保 id,同一条最多自动重试一次") {
    SteeringQueue q;
    const auto id = q.Enqueue(MessageTarget::Main(), "等送达的");
    q.Enqueue(MessageTarget::Main(), "第二条");

    // 第一趟:取队头去发,那轮请求失败——还回队首,attempts 记 1。
    auto first_take = q.TakeFirstAutoSendable(MessageTarget::Main());
    REQUIRE(first_take.has_value());
    CHECK(first_take->id == id);
    q.ReturnToFront(std::move(*first_take));
    auto after_return = q.Snapshot();
    REQUIRE(after_return.size() == 2);
    CHECK(after_return[0].id == id);  // 还回队首,还在第二条前头
    CHECK(after_return[0].delivery_attempts == 1);
    CHECK(after_return[0].state == QueueItemState::Queued);  // 还是健康条目,不是 Failed

    // 第二趟:attempts=1 < 2,还肯自动再试一次(首发 + 一次重试)。
    auto second_take = q.TakeFirstAutoSendable(MessageTarget::Main());
    REQUIRE(second_take.has_value());
    CHECK(second_take->id == id);
    q.ReturnToFront(std::move(*second_take));  // 又失败,attempts=2,到顶

    // 第三趟:这条不再自动发,泵跳过它去取下一条;原条目留队等用户处置。
    auto third_take = q.TakeFirstAutoSendable(MessageTarget::Main());
    REQUIRE(third_take.has_value());
    CHECK(third_take->id != id);  // 跳过到顶的,取走第二条
    CHECK(Find(q.Snapshot(), id).delivery_attempts == 2);
    CHECK_FALSE(q.empty());  // 到顶那条还在队列里,没有无声消失

    // 终态标注不影响回还账:TargetGone/Failed 条目本就不参与投递,跳过。
    const auto failed_id = q.Enqueue(MessageTarget::Main(), "投错了的");
    q.MarkFailed(failed_id, "x");
    CHECK_FALSE(q.TakeFirstAutoSendable(MessageTarget::Main()).has_value());

    // 用户亲手改写过的条目翻篇:attempts 归零,重新参与自动发送。
    auto stale_handle = q.BeginEdit(failed_id);
    REQUIRE(stale_handle.has_value());
    REQUIRE(q.CommitEdit(*stale_handle, "改好的新话") == Status::Ok);
    CHECK(Find(q.Snapshot(), failed_id).delivery_attempts == 0);
    const auto revived = q.TakeFirstAutoSendable(MessageTarget::Main());
    REQUIRE(revived.has_value());
    CHECK(revived->id == failed_id);
}

TEST_CASE("存档恢复:RestoreFromArchive 保 id/次序/尝试次数,只收空队列") {
    std::vector<QueuedMessage> archived;
    QueuedMessage a;
    a.id = 5;
    a.target = MessageTarget::Main();
    a.text = "旧话一";
    QueuedMessage b;
    b.id = 9;
    b.target = MessageTarget::Agent(3);
    b.text = "给三号的旧话";
    QueuedMessage c;
    c.id = 11;
    c.target = MessageTarget::Main();
    c.text = "失败回还过的";
    c.delivery_attempts = 2;
    archived.push_back(a);
    archived.push_back(b);
    archived.push_back(c);

    SteeringQueue q;
    CHECK(q.RestoreFromArchive(std::move(archived)));
    auto snapshot = q.Snapshot();
    REQUIRE(snapshot.size() == 3);
    CHECK(snapshot[0].id == 5);   // 排队次序照存档
    CHECK(snapshot[0].text == "旧话一");
    CHECK(snapshot[1].target == MessageTarget::Agent(3));
    CHECK(snapshot[2].delivery_attempts == 2);

    // 回还到顶的恢复条目不自动重发(防死循环闸跨存档仍生效)……
    auto head = q.TakeFirstAutoSendable(MessageTarget::Main());
    REQUIRE(head.has_value());
    CHECK(head->id == 5);  // 跳过 attempts=2 的 11 号,先取 5 号
    // ……后续新落队的不与恢复的 id 撞号。
    const auto fresh = q.Enqueue(MessageTarget::Main(), "resume 后新排的");
    CHECK(fresh > 11);

    // 队列非空时旧档不给盖:本场自己的账优先。
    SteeringQueue busy;
    busy.Enqueue(MessageTarget::Main(), "本场的");
    std::vector<QueuedMessage> stale;
    stale.push_back(a);
    CHECK_FALSE(busy.RestoreFromArchive(std::move(stale)));
    CHECK(busy.Snapshot().size() == 1);  // 原账没动
}

TEST_CASE("清账告知:BuildQueueDisposalRows 带条数与首条预览,空清账零输出") {
    CHECK(BuildQueueDisposalRows({}).empty());

    std::vector<QueuedMessage> discarded;
    discarded.push_back(QueuedMessage{1, MessageTarget::Main(), "首条正文"});
    discarded.push_back(QueuedMessage{2, MessageTarget::Agent(3), "第二条"});
    const auto rows = BuildQueueDisposalRows(discarded);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].find("2") != std::string::npos);           // 条数
    CHECK(rows[0].find("丢弃") != std::string::npos);         // 说清是倒掉
    CHECK(rows[1].find("首条正文") != std::string::npos);     // 首条预览
    CHECK(rows[1].find("第二条") == std::string::npos);       // 只摆首条
    CHECK(rows[1].find("main") != std::string::npos);         // 目标短名

    // 首条多行只摆一行;子代理目标带 # 短名。
    std::vector<QueuedMessage> multi;
    multi.push_back(QueuedMessage{3, MessageTarget::Agent(7), "行一\n行二"});
    const auto multi_rows = BuildQueueDisposalRows(multi);
    REQUIRE(multi_rows.size() == 2);
    CHECK(multi_rows[1].find("行一") != std::string::npos);
    CHECK(multi_rows[1].find("行二") == std::string::npos);
    CHECK(multi_rows[1].find("#7") != std::string::npos);
}

TEST_CASE("退场告知:BuildQueueArchiveRows 说清随档带走,空队列零输出") {
    CHECK(BuildQueueArchiveRows({}).empty());
    std::vector<QueuedMessage> queued;
    queued.push_back(QueuedMessage{1, MessageTarget::Main(), "还排着的"});
    const auto rows = BuildQueueArchiveRows(queued);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].find("1") != std::string::npos);
    CHECK(rows[0].find("resume") != std::string::npos);  // 去处说明
    CHECK(rows[1].find("还排着的") != std::string::npos);
}

TEST_CASE("BuildSteeringQueueRows:空队列不画标题;标题随模式变") {
    CHECK(BuildSteeringQueueRows({}, QueueViewOptions{}).empty());

    const std::vector<QueuedMessage> one{QueuedMessage{1, MessageTarget::Main(), "hi"}};
    QueueViewOptions opt;
    opt.title_mode = QueueTitleMode::Boundary;
    const auto rows = BuildSteeringQueueRows(one, opt);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1] == "  \xE2\x86\xB3 hi");  // "  ↳ hi"
    CHECK(rows[0].find("工具调用后送出") != std::string::npos);
    CHECK(rows[0].find("Esc") != std::string::npos);

    opt.title_mode = QueueTitleMode::EndOfTurn;
    CHECK(BuildSteeringQueueRows(one, opt)[0].find("收尾后送出") != std::string::npos);
    opt.title_mode = QueueTitleMode::Immediate;
    CHECK(BuildSteeringQueueRows(one, opt)[0].find("打断") != std::string::npos);
    opt.title_mode = QueueTitleMode::Editing;
    CHECK(BuildSteeringQueueRows(one, opt)[0].find("编辑") != std::string::npos);
}

TEST_CASE("BuildSteeringQueueRows:目标短名、状态标记、多行正文只摆首行") {
    std::vector<QueuedMessage> items;
    items.push_back(QueuedMessage{1, MessageTarget::Main(), "第一行\n第二行"});
    items.push_back(QueuedMessage{2, MessageTarget::Agent(3), "给三号"});
    items.push_back(QueuedMessage{3, MessageTarget::Agent(9), "目标没了"});
    items.back().state = QueueItemState::TargetGone;
    items.push_back(QueuedMessage{4, MessageTarget::Main(), "投错了"});
    items.back().state = QueueItemState::Failed;

    QueueViewOptions opt;
    opt.visible_cap = 8;  // 全摆
    const auto rows = BuildSteeringQueueRows(items, opt);
    REQUIRE(rows.size() == 5);
    CHECK(rows[1].find("第一行") != std::string::npos);
    CHECK(rows[1].find("第二行") == std::string::npos);      // 只摆首行
    CHECK(rows[2].find("[#3]") != std::string::npos);         // 子代理目标短名
    CHECK(rows[2].find("给三号") != std::string::npos);
    CHECK(rows[3].find("目标已结束") != std::string::npos);
    CHECK(rows[4].find("发送失败") != std::string::npos);
    // main 目标不带短名标签(不吵)。
    CHECK(rows[1].find("main") == std::string::npos);
}

TEST_CASE("BuildSteeringQueueRows:超上限加'另有 N 条',围着编辑条目开窗") {
    std::vector<QueuedMessage> items;
    for (int i = 1; i <= 6; ++i) {
        items.push_back(QueuedMessage{static_cast<lubancode::cli::QueueId>(i), MessageTarget::Main(),
                                      "话" + std::to_string(i)});
    }
    QueueViewOptions opt;
    opt.visible_cap = 3;
    // 没在编辑:摆最新 3 条(话4-话6);行序 = 标题、"另有 3 条"、条目。
    auto rows = BuildSteeringQueueRows(items, opt);
    REQUIRE(rows.size() == 5);
    CHECK(rows[1].find("3") != std::string::npos);
    CHECK(rows[2].find("话4") != std::string::npos);
    CHECK(rows[4].find("话6") != std::string::npos);

    // 编辑第 2 条(index 1):窗口围着它开,话1-话3 在窗里。
    items[1].edit_open = true;
    rows = BuildSteeringQueueRows(items, opt);
    REQUIRE(rows.size() == 5);
    CHECK(rows[2].find("话1") != std::string::npos);
    CHECK(rows[3].find("话2") != std::string::npos);
    CHECK(rows[3].find("编辑中") != std::string::npos);  // 编辑条目带标记
    CHECK(rows[4].find("话3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 问题二(2026-08-29 实测:忙碌期排队的 /context 被包成 [用户排队消息] 送
// 模型,轮末 ProcessLine 再也看不见它):slash 身份保留 + 工具边界让路 +
// 提交门。跨 AgentLoop 的"工具边界早于轮末"回归在
// tests/unit/agent/test_queue_slash_boundary.cpp。
// ---------------------------------------------------------------------------

TEST_CASE("slash 身份:IsQueuedSlashText 与 ProcessLine 同一颗 ParseSlashCommand") {
    CHECK(IsQueuedSlashText("/context"));
    CHECK(IsQueuedSlashText("/HELP"));            // 命令词大小写不敏感
    CHECK(IsQueuedSlashText("  /todos  "));       // 前后空白剥掉
    CHECK(IsQueuedSlashText("/think high"));      // 带参数仍是完整命令
    CHECK(IsQueuedSlashText("/不认得的词"));       // Unknown 也是 slash:轮末交给本地分派器打"不认得"
    CHECK(IsQueuedSlashText("/context\n还有一行"));  // 多行整段:命令词认不认得都先按 slash 归宿走
    CHECK_FALSE(IsQueuedSlashText("普通正文"));
    CHECK_FALSE(IsQueuedSlashText("正文里带 /context 词"));  // 不以 / 起头
    CHECK_FALSE(IsQueuedSlashText(""));
    CHECK_FALSE(IsQueuedSlashText("   "));
}

TEST_CASE("工具边界让路:TakeDeliverable 跳过 slash,文字照旧按序;slash 留给轮末泵") {
    SteeringQueue q;
    q.Enqueue(MessageTarget::Main(), "文字一");
    const auto slash_id = q.Enqueue(MessageTarget::Main(), "/context");
    q.Enqueue(MessageTarget::Main(), "文字二");
    q.Enqueue(MessageTarget::Agent(3), "/todos");  // 子代理目标的 slash 同样不进边界(兜旧档漏网)

    // 工具边界:只取走普通文字,slash 两枚都留下。
    auto batch = q.TakeDeliverable(MessageTarget::Main());
    REQUIRE(batch.size() == 2);
    CHECK(batch[0].text == "文字一");  // 文字之间保持落队顺序
    CHECK(batch[1].text == "文字二");

    auto snapshot = q.Snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot[0].id == slash_id);  // /context 原样留队,没被边界抢走
    CHECK(snapshot[1].text == "/todos");
    CHECK(q.HasDeliverable(MessageTarget::Main()));    // 只剩 slash 仍是"还有没办完的事"
    CHECK(q.HasDeliverable(MessageTarget::Agent(3)));  // 子代理目标同理

    // 轮末泵:slash 也是待办,TakeFirstAutoSendable 照取队头。
    auto head = q.TakeFirstAutoSendable(MessageTarget::Main());
    REQUIRE(head.has_value());
    CHECK(head->id == slash_id);
    // 取走的这条经 ProcessLine 开头那颗 ParseSlashCommand 认得——轮末走的
    // 是本地分派,不是模型。
    CHECK(ParseSlashCommand(head->text).command == SlashCommand::Context);
    CHECK_FALSE(q.HasDeliverable(MessageTarget::Main()));  // main 的办完了

    // TakeFirstDeliverable 的让路规矩与批量版一致。
    CHECK(q.TakeFirstDeliverable(MessageTarget::Agent(3)) == std::nullopt);
    CHECK(q.size() == 1);
}

TEST_CASE("提交门:白名单内放行,白名单外与子代理目标的 slash 明拒") {
    // 普通文字恒放行(不论目标——给子代理递话正是队列的本职)。
    CHECK(QueueTextAdmittedDuringBusy("普通排队文字", MessageTarget::Main()));
    CHECK(QueueTextAdmittedDuringBusy("给三号的补充", MessageTarget::Agent(3)));
    // 单子点名的可排队样例 + 同类只读/维护面。
    CHECK(QueueTextAdmittedDuringBusy("/context", MessageTarget::Main()));
    CHECK(QueueTextAdmittedDuringBusy("/help", MessageTarget::Main()));
    CHECK(QueueTextAdmittedDuringBusy("/todos", MessageTarget::Main()));
    CHECK(QueueTextAdmittedDuringBusy("/compact 重点保住收尾清单", MessageTarget::Main()));
    CHECK(QueueTextAdmittedDuringBusy("/think high", MessageTarget::Main()));
    CHECK(QueueTextAdmittedDuringBusy("/effort low", MessageTarget::Main()));   // /think 别名
    CHECK(QueueTextAdmittedDuringBusy("/bg", MessageTarget::Main()));           // /background 别名
    CHECK(QueueTextAdmittedDuringBusy("/sessions all", MessageTarget::Main()));
    // 菜单/向导类:轮末自动弹 ReadLine,用户不在场。
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/model", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/provider add x", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/peers", MessageTarget::Main()));
    // 换场/毁档类:排队的旧命令不该悄悄改会话去向。
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/exit", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/quit", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/clear", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/resume 1", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/init", MessageTarget::Main()));
    // 起工作/发模型类:那是排一轮活,不是本地命令。
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/plan", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/workflow run x", MessageTarget::Main()));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/image a.png", MessageTarget::Main()));
    // Unknown(含 workflow alias)一律不排:不认得的命令不进队列。
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/不认得", MessageTarget::Main()));
    // 本地命令不投子代理:哪一档白名单内的都不行。
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/context", MessageTarget::Agent(3)));
    CHECK_FALSE(QueueTextAdmittedDuringBusy("/help", MessageTarget::Agent(3)));
    // 枚举口径直钉:Image/Unknown/Exit 不在白名单,Context/Help 在。
    CHECK_FALSE(SlashCommandQueueableDuringBusy(SlashCommand::Image));
    CHECK_FALSE(SlashCommandQueueableDuringBusy(SlashCommand::Unknown));
    CHECK_FALSE(SlashCommandQueueableDuringBusy(SlashCommand::Exit));
    CHECK(SlashCommandQueueableDuringBusy(SlashCommand::Context));
    CHECK(SlashCommandQueueableDuringBusy(SlashCommand::Help));
}

TEST_CASE("队列区成行:排队 slash 带'轮末执行'标记,普通文字不带") {
    std::vector<QueuedMessage> items;
    items.push_back(QueuedMessage{1, MessageTarget::Main(), "/context"});
    items.push_back(QueuedMessage{2, MessageTarget::Main(), "普通话"});
    QueueViewOptions opt;
    opt.visible_cap = 4;
    const auto rows = BuildSteeringQueueRows(items, opt);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].find("轮末执行") != std::string::npos);  // slash 条目明说归宿
    CHECK(rows[1].find("/context") != std::string::npos);
    CHECK(rows[2].find("轮末执行") == std::string::npos);  // 普通文字不带
    CHECK(rows[2].find("普通话") != std::string::npos);
}
