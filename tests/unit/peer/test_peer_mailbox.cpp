// 跨会话传话:信封解析、去重、限速、队列上限、默认权限档。全是纯逻辑,
// 时钟注入,不碰网络不碰线程。

#include <doctest/doctest.h>

#include <string>

#include "peers/peer_mailbox.hpp"

using namespace lubancode::peers;

namespace {

PeerEnvelope MakeEnvelope(const std::string& id, const std::string& text = "hello",
                          const std::string& sender = "peer-a") {
    PeerEnvelope envelope;
    envelope.message_id = id;
    envelope.sender_id = sender;
    envelope.sender_name = "backend";
    envelope.target_id = "peer-b";
    envelope.sent_at = 1000;
    envelope.text = text;
    return envelope;
}

}  // namespace

TEST_CASE("信封 JSON:往返无损,reply_to 空序列化成 null") {
    const PeerEnvelope envelope = MakeEnvelope("m1", "接口字段已改成 tenant_id");
    const nlohmann::json json = PeerEnvelopeToJson(envelope);
    CHECK(json["version"] == 1);
    CHECK(json["message_id"] == "m1");
    CHECK(json["sender_id"] == "peer-a");
    CHECK(json["sender_name"] == "backend");
    CHECK(json["target_id"] == "peer-b");
    CHECK(json["reply_to"].is_null());
    CHECK(json["text"] == "接口字段已改成 tenant_id");

    const auto parsed = PeerEnvelopeFromJson(json.dump());
    REQUIRE(parsed.has_value());
    CHECK(parsed->message_id == envelope.message_id);
    CHECK(parsed->text == envelope.text);
    CHECK(parsed->reply_to.has_value() == false);
}

TEST_CASE("信封 JSON:reply_to 有值时往返保留") {
    PeerEnvelope envelope = MakeEnvelope("m2");
    envelope.reply_to = "m1";
    const auto parsed = PeerEnvelopeFromJson(PeerEnvelopeToJson(envelope).dump());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->reply_to.has_value());
    CHECK(*parsed->reply_to == "m1");
}

TEST_CASE("信封解析:坏 JSON / 非对象 / 缺必填 / 版本不认,一律拒收") {
    CHECK_FALSE(PeerEnvelopeFromJson("not json").has_value());
    CHECK_FALSE(PeerEnvelopeFromJson("[1,2]").has_value());
    CHECK_FALSE(PeerEnvelopeFromJson("{}").has_value());  // 缺 message_id 等
    CHECK_FALSE(PeerEnvelopeFromJson(R"({"version":1,"message_id":"","sender_id":"a","target_id":"b","text":"t"})")
                    .has_value());  // 空 message_id
    CHECK_FALSE(PeerEnvelopeFromJson(R"({"version":99,"message_id":"m","sender_id":"a","target_id":"b","text":"t"})")
                    .has_value());  // 认不得的版本
    // sender_name / sent_at 可缺(老发送方),不算坏。
    CHECK(PeerEnvelopeFromJson(R"({"version":1,"message_id":"m","sender_id":"a","target_id":"b","text":"t"})")
              .has_value());
}

TEST_CASE("message_id 去重:同一封信只收一次") {
    PeerMailbox mailbox;
    CHECK(mailbox.Offer(MakeEnvelope("m1"), 1000) == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m1"), 1001) == PeerOfferStatus::Duplicate);
    CHECK(mailbox.pending() == 1);
    const auto drained = mailbox.Drain();
    REQUIRE(drained.size() == 1);
    CHECK(drained[0].message_id == "m1");
    CHECK(mailbox.pending() == 0);
}

TEST_CASE("限速:同一发送方窗口内超过上限被拦,别的发送方不受连坐") {
    PeerMailbox mailbox(/*capacity=*/16, /*rate_limit=*/3, /*rate_window_seconds=*/30, /*dup_text_window_seconds=*/0);
    for (int i = 0; i < 3; ++i) {
        CHECK(mailbox.Offer(MakeEnvelope("a" + std::to_string(i), "t" + std::to_string(i)), 1000 + i) ==
              PeerOfferStatus::Accepted);
    }
    CHECK(mailbox.Offer(MakeEnvelope("a3", "t3"), 1003) == PeerOfferStatus::RateLimited);
    // 另一个发送方照收。
    CHECK(mailbox.Offer(MakeEnvelope("b0", "t3", "peer-b"), 1003) == PeerOfferStatus::Accepted);
    // 窗口滑过去,又能收了。
    CHECK(mailbox.Offer(MakeEnvelope("a4", "t4"), 1000 + 31) == PeerOfferStatus::Accepted);
}

TEST_CASE("相同正文短窗去重:同一发送方重发同一句话不重复入队") {
    PeerMailbox mailbox(16, 10, 30, 10);
    CHECK(mailbox.Offer(MakeEnvelope("m1", "一样的正文"), 1000) == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m2", "一样的正文"), 1002) == PeerOfferStatus::DuplicateText);
    CHECK(mailbox.pending() == 1);
    // 正文变了或窗口过了,都算新信。
    CHECK(mailbox.Offer(MakeEnvelope("m3", "别的正文"), 1003) == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m4", "一样的正文"), 1000 + 11) == PeerOfferStatus::Accepted);
    // 换个发送方发同一句,不算重复。
    CHECK(mailbox.Offer(MakeEnvelope("m5", "别的正文", "peer-c"), 1004) == PeerOfferStatus::Accepted);
}

TEST_CASE("队列硬上限:满了再收报 QueueFull") {
    PeerMailbox mailbox(/*capacity=*/2, 10, 30, 0);
    CHECK(mailbox.Offer(MakeEnvelope("m1", "t1"), 1000) == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m2", "t2"), 1000) == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m3", "t3"), 1000) == PeerOfferStatus::QueueFull);
    CHECK(mailbox.pending() == 2);
}

TEST_CASE("默认权限档:两边都确认可直接收;任一边免确认默认 hold;cwd 相远默认 hold") {
    // 0=confirm, 1=auto, 2=yolo。
    CHECK(DefaultReceiveTier(0, 0, false) == PeerPermissionTier::Accept);
    CHECK(DefaultReceiveTier(1, 0, false) == PeerPermissionTier::Hold);
    CHECK(DefaultReceiveTier(0, 2, false) == PeerPermissionTier::Hold);
    CHECK(DefaultReceiveTier(2, 2, false) == PeerPermissionTier::Hold);
    CHECK(DefaultReceiveTier(0, 0, true) == PeerPermissionTier::Hold);
}

TEST_CASE("cwd 距离:前两段相同算近,不同算远,信息不全按远") {
    // 前"两段"= 根/盘符 + 第一级目录(Windows: D:/work;POSIX: /home/<user>)。
    CHECK_FALSE(PeerCwdFarApart("D:\\work\\proj-a\\sub", "D:/work/proj-a/other"));
    CHECK_FALSE(PeerCwdFarApart("D:\\work\\proj-a", "D:\\work\\proj-b"));  // 同在 D:/work 下,算近
    CHECK(PeerCwdFarApart("D:\\work\\proj-a", "D:\\games\\proj-b"));
    CHECK(PeerCwdFarApart("/home/alice/app", "/home/alice/app2") == false);  // /home/alice 相同
    CHECK(PeerCwdFarApart("/home/alice/app", "/home/bob/app"));
    CHECK(PeerCwdFarApart("", "/home/alice"));
    CHECK(PeerCwdFarApart("/home/alice", ""));
}

TEST_CASE("PeerDeliveryName:五档各有名") {
    CHECK(std::string(PeerDeliveryName(PeerDelivery::Delivered)) == "delivered");
    CHECK(std::string(PeerDeliveryName(PeerDelivery::Held)) == "held");
    CHECK(std::string(PeerDeliveryName(PeerDelivery::Refused)) == "refused");
    CHECK(std::string(PeerDeliveryName(PeerDelivery::Expired)) == "expired");
    CHECK(std::string(PeerDeliveryName(PeerDelivery::Unavailable)) == "unavailable");
}
