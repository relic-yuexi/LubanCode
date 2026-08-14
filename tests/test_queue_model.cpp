// 会话层排队队列 SteeringQueue(0.28.x"排队消息在工具边界送达并可
// Shift+左键编辑")的纯逻辑测试:落队顺序与目标、投递按目标分账、编辑事务
// (版本冲突/冻结/删除)、终态目标标注、立即送状态旗、显示成行(标题随
// 状态变、三条窗口、编辑中标记)。全部走局部实例,不碰全局
// SessionSteeringQueue;StreamSlashHintLines(流式输入行 slash 提示)的
// 用例沿用旧文件(接线层纯函数,仍钉在这边)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/line_editor.hpp"
#include "cli/queue_model.hpp"

using lubancode::cli::BuildSteeringQueueRows;
using lubancode::cli::DeliveryMode;
using lubancode::cli::MessageTarget;
using lubancode::cli::QueueItemState;
using lubancode::cli::QueueTitleMode;
using lubancode::cli::QueueViewOptions;
using lubancode::cli::QueuedMessage;
using lubancode::cli::SteeringQueue;
using lubancode::cli::StreamSlashHintLines;
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
// StreamSlashHintLines(流式输入行的 slash 提示,接线层纯函数):门槛、前缀
// 过滤、封顶 6 行 + 汇总行。候选表是自造的,不依赖 AllSlashCommands 的
// 真实清单(那份会随版本长,断言数字会飘)。
// ---------------------------------------------------------------------------

namespace {

std::vector<lubancode::cli::CompletionCandidate> HintCandidates() {
    using lubancode::cli::CompletionCandidate;
    return {
        {"/help", "看帮助"},
        {"/model", "换模型"},
        {"/record", "录制技能"},
        {"/retry", "重试一轮"},
        {"/read", "读文件"},
        {"/refresh", "刷新"},
        {"/remove", "删掉"},
        {"/recordx", "多一个凑数"},
    };
}

}  // namespace

TEST_CASE("StreamSlashHintLines:'/':全列,封顶 6 行 + 一行汇总,不带选中标记") {
    const auto lines = StreamSlashHintLines(HintCandidates(), "/");
    REQUIRE(lines.size() == 7);  // 6 行候选 + 1 行"共 8 个命令"
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(lines[i].rfind("  ", 0) == 0);   // 没有选中态,一律两空格起头
        CHECK(lines[i].rfind("> ", 0) != 0);
    }
    CHECK(lines[0].find("/help") != std::string::npos);
    CHECK(lines[5].find("/refresh") != std::string::npos);  // 第 7、8 个不逐条摆
    CHECK(lines[5].find("/remove") == std::string::npos);
    CHECK(lines[6].find("8") != std::string::npos);         // 汇总行带总数
    // 说明跟着命令行走
    CHECK(lines[0].find("看帮助") != std::string::npos);
}

TEST_CASE("StreamSlashHintLines:恰好 6 个命中,正好 6 行,不加汇总行") {
    using lubancode::cli::CompletionCandidate;
    std::vector<CompletionCandidate> six;
    for (int i = 0; i < 6; ++i) {
        six.push_back(CompletionCandidate{"/a" + std::to_string(i), "说明"});
    }
    const auto lines = StreamSlashHintLines(six, "/");
    REQUIRE(lines.size() == 6);
    CHECK(lines[5].find("/a5") != std::string::npos);
}

TEST_CASE("StreamSlashHintLines:'/re' 前缀过滤,大小写不敏感") {
    const auto lines = StreamSlashHintLines(HintCandidates(), "/re");
    // 命中 /record /retry /read /refresh /remove /recordx,恰好 6 个,不加汇总行
    REQUIRE(lines.size() == 6);
    CHECK(lines[0].find("/record") != std::string::npos);
    for (const auto& line : lines) {
        CHECK(line.find("/help") == std::string::npos);
        CHECK(line.find("/model") == std::string::npos);
    }

    const auto upper = StreamSlashHintLines(HintCandidates(), "/RE");
    REQUIRE(upper.size() == lines.size());
    CHECK(upper[0].find("/record") != std::string::npos);  // 命令名照原样摆
}

TEST_CASE("StreamSlashHintLines:无匹配、含空格、非 slash、空 buffer 一律为空") {
    const auto cands = HintCandidates();
    CHECK(StreamSlashHintLines(cands, "/xyz").empty());        // 无匹配
    CHECK(StreamSlashHintLines(cands, "/record start").empty());  // 敲了空格,进了参数区
    CHECK(StreamSlashHintLines(cands, "hello").empty());       // 非 slash 正文
    CHECK(StreamSlashHintLines(cands, "").empty());            // 空 buffer
    CHECK(StreamSlashHintLines({}, "/re").empty());            // 没有候选表也是空
}
