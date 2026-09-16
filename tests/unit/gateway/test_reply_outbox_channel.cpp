// QQ 接入单 Q2 §七:DurableReplyOutbox 的渠道族——QQ target 入箱/拆段/
// 稳定 deliveryId/item.attempt/item.sent/delivery_unknown/failed 与重开投影。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "gateway/reply_outbox.hpp"

using namespace lubancode::gateway;

namespace {

struct OutboxDir {
    std::filesystem::path root;

    explicit OutboxDir(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-outbox-channel-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    DurableReplyOutbox::Paths Paths() const {
        DurableReplyOutbox::Paths paths;
        paths.log_file = root / "outbox.jsonl";
        paths.replies_dir = root / "replies";
        paths.published_dir = root / "out";
        return paths;
    }
};

DurableReplyOutbox::ChannelTarget Target() {
    DurableReplyOutbox::ChannelTarget target;
    target.channel_id = "qqbot";
    target.account_id = "main";
    target.conversation_id = "dm-owner";
    target.reply_to_message_id = "m-in-1";
    target.source_ref = "ingress:qqbot:main:7";
    return target;
}

}  // namespace

TEST_CASE("拆段:短文单段;长文按 UTF-8 边界多段且不撕半个字符") {
    REQUIRE(SplitReplySegments("你好", 0).empty());       // 非法帽明败
    REQUIRE(SplitReplySegments("hello", 100).size() == 1);
    // 中文长文(每字 3 字节):2000 帽 → 4 段内。
    std::string chinese;
    for (int i = 0; i < 2000; ++i) {
        chinese += "汉";
    }
    const auto segments = SplitReplySegments(chinese, 2000);
    REQUIRE(segments.size() >= 3);
    REQUIRE(segments.size() <= 4);
    std::string rejoined;
    for (const auto& segment : segments) {
        rejoined += segment;
    }
    REQUIRE(rejoined == chinese);  // 拆段无损
    // 帽边界不落在多字节序列中间:每段起字节是字符首字节(切点处
    // text[end] 为 lead byte);段的完整性由无损重组背书。
    for (const auto& segment : segments) {
        REQUIRE((static_cast<unsigned char>(segment.front()) & 0xC0) != 0x80);
    }
}

TEST_CASE("渠道入箱:target 字段落账,单段 deliveryId 定式;本地族不混") {
    OutboxDir dir("enqueue");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    const auto receipt = outbox.EnqueueChannel("sel-t1", "回复正文", "s1", "turn-1",
                                               Target(), 1000);
    REQUIRE(receipt.accepted);
    REQUIRE(receipt.delivery_ids.size() == 1);
    const std::string expected = MakeDeliveryId(
        "sel-t1", MakeChannelDeliveryTarget("qqbot", "main", "dm-owner"), 1);
    REQUIRE(receipt.delivery_ids[0] == expected);
    const auto item = outbox.Find(expected);
    REQUIRE(item.has_value());
    REQUIRE(item->state == "pending");
    REQUIRE(item->target_channel_id == "qqbot");
    REQUIRE(item->target_account_id == "main");
    REQUIRE(item->target_conversation_id == "dm-owner");
    REQUIRE(item->target_reply_to_message_id == "m-in-1");
    // A05:msg_seq 不再由 ordinal 顶替——入箱时未分配(0),首次网络发送
    // 前 AssignChannelSendIdentity 持久发号(见身份分配册)。
    REQUIRE(item->target_msg_seq == 0);
    REQUIRE(item->source_ref == "ingress:qqbot:main:7");
    REQUIRE(item->ordinal == 1);
    REQUIRE(outbox.PendingChannelItems().size() == 1);
    // 幂等重入:同 selection 同正文 → duplicate,不另入。
    const auto again = outbox.EnqueueChannel("sel-t1", "回复正文", "s1", "turn-1",
                                             Target(), 2000);
    REQUIRE(again.duplicate);
    REQUIRE_FALSE(again.accepted);
    REQUIRE(again.delivery_ids == receipt.delivery_ids);
    REQUIRE(outbox.PendingChannelItems().size() == 1);
}

TEST_CASE("渠道投递态:attempt 先账,sent 幂等,unknown/failed 终态不翻转") {
    OutboxDir dir("states");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    const auto receipt = outbox.EnqueueChannel("sel-t2", "正文", "s1", "turn-2",
                                               Target(), 1000);
    const std::string id = receipt.delivery_ids[0];
    REQUIRE(outbox.RecordAttempt(id, 1100));
    REQUIRE(outbox.Find(id)->state == "sending");
    REQUIRE(outbox.Find(id)->attempts == 1);
    REQUIRE(outbox.RecordAttempt(id, 1200));  // 重驱动:再记一笔
    REQUIRE(outbox.Find(id)->attempts == 2);
    REQUIRE(outbox.MarkSent(id, "om_9", 1300));
    REQUIRE(outbox.Find(id)->state == "sent");
    REQUIRE(outbox.Find(id)->provider_message_id == "om_9");
    // 重复回执幂等;终态后不再 attempt/failed。
    REQUIRE(outbox.MarkSent(id, "om_9", 1400));
    REQUIRE_FALSE(outbox.RecordAttempt(id, 1500));
    REQUIRE_FALSE(outbox.MarkChannelFailed(id, "rate_limited", 1600));
    REQUIRE(outbox.Find(id)->state == "sent");
    // 未知超时态:另一枚。
    const auto receipt2 = outbox.EnqueueChannel("sel-t3", "正文2", "s1", "turn-3",
                                                Target(), 1000);
    const std::string id2 = receipt2.delivery_ids[0];
    REQUIRE(outbox.RecordAttempt(id2, 1100));
    REQUIRE(outbox.MarkOutcomeUnknown(id2, 5000));
    REQUIRE(outbox.Find(id2)->state == "delivery_unknown");
    REQUIRE(outbox.Find(id2)->delivery_error == "channel.delivery_unknown");
    // unknown 是终态:不再翻 failed/sent。
    REQUIRE_FALSE(outbox.MarkChannelFailed(id2, "rate_limited", 6000));
    REQUIRE_FALSE(outbox.MarkSent(id2, "om_x", 7000));
}

TEST_CASE("本地投递跳过渠道族:DeliverPending 不给渠道项发本地文件") {
    OutboxDir dir("mixed");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    REQUIRE(outbox.EnqueueChannel("sel-c1", "渠道正文", "s1", "turn-c", Target(), 1000)
                .accepted);
    const auto local = outbox.Enqueue("sel-l1", "本地正文", "s1", "turn-l", 1000);
    REQUIRE(local.accepted);
    const auto result = outbox.DeliverPending(2000);
    REQUIRE(result.delivered == 1);  // 只本地那枚
    REQUIRE(outbox.Find(local.delivery_id)->state == "delivered");
    const auto channel_item = outbox.Find(outbox.PendingChannelItems()[0].delivery_id);
    REQUIRE(channel_item->state == "pending");  // 渠道枚未被本地路径碰
}

TEST_CASE("重开投影:渠道态/attempt 计数/target 字段从账重放;正文从段原件读回") {
    OutboxDir dir("reopen");
    std::string sent_id;
    std::string sending_id;
    {
        DurableReplyOutbox outbox;
        REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
        const auto a = outbox.EnqueueChannel("sel-r1", "第一段", "s1", "turn-r1",
                                             Target(), 1000);
        const auto b = outbox.EnqueueChannel("sel-r2", "第二段", "s1", "turn-r2",
                                             Target(), 1000);
        sent_id = a.delivery_ids[0];
        sending_id = b.delivery_ids[0];
        REQUIRE(outbox.RecordAttempt(sent_id, 1100));
        REQUIRE(outbox.MarkSent(sent_id, "om_1", 1200));
        REQUIRE(outbox.RecordAttempt(sending_id, 1300));
    }
    DurableReplyOutbox reopened;
    REQUIRE(DurableReplyOutbox::Open(&reopened, dir.Paths()).ok);
    REQUIRE(reopened.Find(sent_id)->state == "sent");
    REQUIRE(reopened.Find(sent_id)->provider_message_id == "om_1");
    REQUIRE(reopened.Find(sent_id)->attempts == 1);
    REQUIRE(reopened.Find(sending_id)->state == "sending");
    REQUIRE(reopened.Find(sending_id)->attempts == 1);
    REQUIRE(reopened.Find(sending_id)->source_ref == "ingress:qqbot:main:7");
    // 冻结正文:内存没有(重开后)从段原件读回并核 hash。
    std::string text;
    REQUIRE(reopened.LoadChannelItemText(sending_id, &text));
    REQUIRE(text == "第二段");
}

TEST_CASE("写盘失败:账 broken 后渠道态推进全拒") {
    OutboxDir dir("broken");
    DurableReplyOutbox outbox;
    // log 文件位置放一个目录:首笔提交开不了写者 → broken。
    DurableReplyOutbox::Paths paths = dir.Paths();
    std::error_code ec;
    std::filesystem::create_directories(paths.log_file, ec);
    REQUIRE(DurableReplyOutbox::Open(&outbox, paths).ok);
    const auto receipt = outbox.EnqueueChannel("sel-b1", "正文", "s1", "turn-b",
                                               Target(), 1000);
    REQUIRE(receipt.error_code == "outbox.append_failed");
    REQUIRE_FALSE(outbox.RecordAttempt("whatever", 1));
    REQUIRE_FALSE(outbox.MarkSent("whatever", "om", 1));
    REQUIRE(outbox.broken());
}

// ---------------------------------------------------------------------------
// Q4:出站附件(冻结正文 + 附件引用)
// ---------------------------------------------------------------------------

TEST_CASE("Q4 附件入箱:长文末段带附件字段,幂等重入不重写") {
    OutboxDir dir("attach");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    // 附件原件(冻结引用的产物文件)。
    const std::filesystem::path product = dir.root / "product.txt";
    { std::ofstream stream(product); stream << "完整产物正文,超过一段的全文在附件里"; }

    // 长文:12000 字节(汉字×2000 对)→ 7 文本段(UTF-8 边界回退到
    // 1998/段,末段 12 字节)+ 1 纯附件末段(QQ msg_type=7 不带 content,
    // 正文全在前面的段里,谁也不吃掉谁)。
    std::string long_text;
    for (int i = 0; i < 2000; ++i) {
        long_text += "汉字";
    }
    DurableReplyOutbox::ChannelAttachment attachment;
    attachment.local_path = product.generic_string();
    attachment.file_name = "sel-a1.txt";
    attachment.mime_type = "text/plain";
    attachment.size_bytes = 15;
    const auto receipt = outbox.EnqueueChannel("sel-a1", long_text, "s1", "turn-a",
                                               Target(), 1000, &attachment);
    REQUIRE(receipt.accepted);
    REQUIRE(receipt.delivery_ids.size() == 8);
    // 末段(纯附件)带附件字段,其余文本段不带。
    for (std::size_t i = 0; i + 1 < receipt.delivery_ids.size(); ++i) {
        CHECK(outbox.Find(receipt.delivery_ids[i])->attachment_local_path.empty());
    }
    const auto last = outbox.Find(receipt.delivery_ids.back());
    CHECK(last->attachment_local_path == product.generic_string());
    CHECK(last->attachment_file_name == "sel-a1.txt");
    CHECK(last->attachment_mime_type == "text/plain");
    CHECK(last->attachment_size_bytes == 15);
    CHECK(last->reply_text.empty());  // 附件段零正文
    CHECK_FALSE(last->attachment_sha256.empty());  // 入箱时算定

    // 幂等重入:同 selection 同附件 → duplicate,字段不重复落。
    const auto again = outbox.EnqueueChannel("sel-a1", long_text, "s1", "turn-a",
                                             Target(), 2000, &attachment);
    REQUIRE(again.duplicate);
    REQUIRE(again.delivery_ids == receipt.delivery_ids);
    // 短文带附件:1 文本段 + 1 附件段。
    const auto single = outbox.EnqueueChannel("sel-a2", "短文", "s1", "turn-a2",
                                              Target(), 1000, &attachment);
    REQUIRE(single.accepted);
    REQUIRE(single.delivery_ids.size() == 2);
    CHECK_FALSE(outbox.Find(single.delivery_ids[1])->attachment_local_path.empty());
}

// ---------------------------------------------------------------------------
// A05/A06:QQ 发送身份的持久分配——同 (账号,锚) 跨回复共用发号器;
// 冻结幂等;改锚先裁决;重启(重开)发号不归零。
// ---------------------------------------------------------------------------

TEST_CASE("身份分配:同锚跨 delivery 单调发号;幂等复用;主动消息 seq=0") {
    OutboxDir dir("identity_alloc");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    // 同一封来信(m-in-1)下的两枚回复段 + 一枚配对提示——三枚各占一号,
    // 不因 ordinal 都是 1 而撞号(旧病的判据)。
    const auto first = outbox.AssignChannelSendIdentity("dl-a", "main", "m-in-1", "sha-a", 1000);
    REQUIRE(first.ok);
    CHECK(first.identity.anchor_msg_id == "m-in-1");
    CHECK(first.identity.msg_seq == 1);
    const auto second = outbox.AssignChannelSendIdentity("dl-b", "main", "m-in-1", "sha-b", 1100);
    REQUIRE(second.ok);
    CHECK(second.identity.msg_seq == 2);
    const auto third = outbox.AssignChannelSendIdentity("dl-c", "main", "m-in-1", "sha-c", 1200);
    REQUIRE(third.ok);
    CHECK(third.identity.msg_seq == 3);
    // 幂等:同 delivery 同锚 → 旧身份(零新账)。
    const auto retry = outbox.AssignChannelSendIdentity("dl-a", "main", "m-in-1", "sha-a", 1300);
    REQUIRE(retry.ok);
    CHECK(retry.identity.msg_seq == 1);
    // 同 delivery 换锚 → 冲突(必须先裁决,A06)。
    const auto conflict = outbox.AssignChannelSendIdentity("dl-a", "main", "m-in-2", "sha-a", 1400);
    REQUIRE_FALSE(conflict.ok);
    CHECK(conflict.error_code == "identity_anchor_conflict");
    // 裁决后新锚 → 新号(旧身份留在账里,可追溯)。
    REQUIRE(outbox.RetireChannelSendIdentity("dl-a", "reply_window_expired_reanchor", 1500));
    const auto reanchored =
        outbox.AssignChannelSendIdentity("dl-a", "main", "m-in-2", "sha-a", 1600);
    REQUIRE(reanchored.ok);
    CHECK(reanchored.identity.anchor_msg_id == "m-in-2");
    CHECK(reanchored.identity.msg_seq == 1);  // 新锚独立计数(m-in-2 的 1 号)
    // 主动消息(空锚):seq 恒 0,身份可冻结可复用。
    const auto active = outbox.AssignChannelSendIdentity("dl-act", "main", "", "sha-act", 1700);
    REQUIRE(active.ok);
    CHECK(active.identity.msg_seq == 0);
    const auto active_retry =
        outbox.AssignChannelSendIdentity("dl-act", "main", "", "sha-act", 1800);
    REQUIRE(active_retry.ok);
    CHECK(active_retry.identity.msg_seq == 0);
    // 账号隔离:另一账号同锚各自从 1 起。
    const auto other =
        outbox.AssignChannelSendIdentity("dl-x", "alt", "m-in-1", "sha-x", 1900);
    REQUIRE(other.ok);
    CHECK(other.identity.msg_seq == 1);
    // 同 delivery 同锚但摘要变了 → 账坏明拒(冻结正文不许换)。
    const auto mismatch =
        outbox.AssignChannelSendIdentity("dl-b", "main", "m-in-1", "sha-other", 2000);
    REQUIRE_FALSE(mismatch.ok);
    CHECK(mismatch.error_code == "identity_payload_mismatch");
    // 只读投影与 item 视图同步。
    const auto live = outbox.FindLiveChannelSendIdentity("dl-b");
    REQUIRE(live.has_value());
    CHECK(live->msg_seq == 2);
}

TEST_CASE("身份分配:重开发号从账重放不归零;旧账 ordinal 顶替值折成身份") {
    OutboxDir dir("identity_reopen");
    // 先手写一笔旧式账行(升级前格式:targetMsgSeq 由 ordinal 顶替)——
    // 升级路径的折算判据。
    {
        std::error_code ec;
        std::filesystem::create_directories(dir.root / "replies", ec);
        std::ofstream stream(dir.Paths().log_file, std::ios::app);
        stream << R"({"type":"item.enqueued","schemaVersion":1,"deliveryId":"dl-legacy-item",)"
                  R"("selectionId":"sel-legacy","deliveryTarget":"channel:qqbot:main:dm-owner",)"
                  R"("ordinal":1,"replySha256":"sha-legacy","sessionId":"s1","turnId":"turn-l",)"
                  R"("enqueuedAtMs":900,"targetChannelId":"qqbot","targetAccountId":"main",)"
                  R"("targetConversationId":"dm-owner","targetReplyToMessageId":"m-in-1",)"
                  R"("targetMsgSeq":1,"sourceRef":"ingress:qqbot:main:7"})"
               << "\n";
    }
    std::string new_id = "dl-new";
    {
        DurableReplyOutbox outbox;
        REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
        // 开账即折算:旧账 targetMsgSeq=1 已被记为 m-in-1 的 1 号。
        const auto legacy_live = outbox.FindLiveChannelSendIdentity("dl-legacy-item");
        REQUIRE(legacy_live.has_value());
        CHECK(legacy_live->msg_seq == 1);
        CHECK(legacy_live->anchor_msg_id == "m-in-1");
        // 新分配从 max(旧账)=1 之上起:2 号,永不与在途旧号相撞。
        const auto assigned =
            outbox.AssignChannelSendIdentity(new_id, "main", "m-in-1", "sha-n", 1100);
        REQUIRE(assigned.ok);
        CHECK(assigned.identity.msg_seq == 2);
    }
    DurableReplyOutbox reopened;
    REQUIRE(DurableReplyOutbox::Open(&reopened, dir.Paths()).ok);
    // 显式身份行重放:dl-new 的 2 号仍在(幂等复用,重启不归零)。
    const auto again =
        reopened.AssignChannelSendIdentity(new_id, "main", "m-in-1", "sha-n", 2000);
    REQUIRE(again.ok);
    CHECK(again.identity.msg_seq == 2);
    // 下一枚 3 号:发号计数跨重开连续。
    const auto next =
        reopened.AssignChannelSendIdentity("dl-next", "main", "m-in-1", "sha-next", 2100);
    REQUIRE(next.ok);
    CHECK(next.identity.msg_seq == 3);
    // PendingChannelItems 视图带上 live 身份(泵读到的恒是冻结值)。
    const auto items = reopened.PendingChannelItems();
    bool saw_legacy = false;
    for (const auto& item : items) {
        if (item.delivery_id != "dl-legacy-item") {
            continue;
        }
        saw_legacy = true;
        CHECK(item.target_msg_seq == 1);
        CHECK(item.target_reply_to_message_id == "m-in-1");
    }
    CHECK(saw_legacy);
    // 裁决后视图归零(下一轮分配换新身份)。
    REQUIRE(reopened.RetireChannelSendIdentity("dl-legacy-item", "test", 2200));
    const auto retired = reopened.FindLiveChannelSendIdentity("dl-legacy-item");
    REQUIRE_FALSE(retired.has_value());
}

TEST_CASE("身份分配:账写不进 → 不发号(内存计数不动,重试同号)") {
    OutboxDir dir("identity_broken");
    DurableReplyOutbox outbox;
    DurableReplyOutbox::Paths paths = dir.Paths();
    std::error_code ec;
    std::filesystem::create_directories(paths.log_file, ec);  // 目录占住账文件
    REQUIRE(DurableReplyOutbox::Open(&outbox, paths).ok);
    const auto failed =
        outbox.AssignChannelSendIdentity("dl-f", "main", "m-in-1", "sha", 1000);
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.error_code == "outbox.append_failed");
    REQUIRE_FALSE(outbox.RetireChannelSendIdentity("dl-f", "test", 1100));
}

TEST_CASE("Q4 附件入箱:纯附件回复(空正文)也是合法单段") {
    OutboxDir dir("attach-only");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    const std::filesystem::path product = dir.root / "only.txt";
    { std::ofstream stream(product); stream << "file body"; }
    DurableReplyOutbox::ChannelAttachment attachment;
    attachment.local_path = product.generic_string();
    attachment.file_name = "only.txt";
    attachment.mime_type = "text/plain";
    attachment.size_bytes = 9;
    const auto receipt = outbox.EnqueueChannel("sel-a3", "", "s1", "turn-a3",
                                               Target(), 1000, &attachment);
    REQUIRE(receipt.accepted);
    REQUIRE(receipt.delivery_ids.size() == 1);
    const auto item = outbox.Find(receipt.delivery_ids[0]);
    CHECK(item->attachment_local_path == product.generic_string());
    // 无附件的空正文照旧明败(不造空段)。
    const auto invalid = outbox.EnqueueChannel("sel-a4", "", "s1", "turn-a4",
                                               Target(), 1000);
    CHECK_FALSE(invalid.accepted);
    CHECK(invalid.error_code == "outbox.segment_invalid");
}

TEST_CASE("Q4 附件入箱:原件读不了明败;重开投影带附件字段") {
    OutboxDir dir("attach-missing");
    DurableReplyOutbox outbox;
    REQUIRE(DurableReplyOutbox::Open(&outbox, dir.Paths()).ok);
    DurableReplyOutbox::ChannelAttachment missing;
    missing.local_path = (dir.root / "gone.txt").generic_string();
    missing.file_name = "gone.txt";
    missing.mime_type = "text/plain";
    const auto receipt = outbox.EnqueueChannel("sel-a5", "正文", "s1", "turn-a5",
                                               Target(), 1000, &missing);
    REQUIRE_FALSE(receipt.accepted);
    CHECK(receipt.error_code == "outbox.attachment_unreadable");

    // 重开投影:附件字段从账行读回(旧账行无新键 = 空)。
    const std::filesystem::path product = dir.root / "p.txt";
    { std::ofstream stream(product); stream << "产物"; }
    DurableReplyOutbox::ChannelAttachment attachment;
    attachment.local_path = product.generic_string();
    attachment.file_name = "p.txt";
    attachment.mime_type = "text/plain";
    attachment.size_bytes = 6;
    std::vector<std::string> delivery_ids;
    {
        DurableReplyOutbox first;
        REQUIRE(DurableReplyOutbox::Open(&first, dir.Paths()).ok);
        const auto ok = first.EnqueueChannel("sel-a6", "正文", "s1", "turn-a6",
                                             Target(), 1000, &attachment);
        REQUIRE(ok.accepted);
        delivery_ids = ok.delivery_ids;
    }
    DurableReplyOutbox reopened;
    REQUIRE(DurableReplyOutbox::Open(&reopened, dir.Paths()).ok);
    // 附件挂在末段(纯附件段),不是首段文本段。
    const auto item = reopened.Find(delivery_ids.back());
    REQUIRE(item.has_value());
    CHECK(item->attachment_local_path == product.generic_string());
    CHECK(item->attachment_file_name == "p.txt");
    CHECK(item->attachment_size_bytes == 6);
    // 对照:既有无附件项的字段为空(旧账兼容)。
    CHECK(outbox.Find(outbox.EnqueueChannel("sel-a7", "普通", "s1", "t7",
                                            Target(), 1000)
                          .delivery_ids[0])
              ->attachment_local_path.empty());
}
