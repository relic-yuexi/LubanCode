// 跨会话传话:信封解析、去重、限速、队列上限、默认权限档,外加接收决定
// 随队列项原子冻结的合同(SV-02:正文与 Hold 决定同一临界区一次发布,
// 主线程 Drain 夹不走"未定案"的扣信)。时钟注入;并发册用栅栏钉交错,
// 不靠 sleep 赌时序。

#include <doctest/doctest.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    CHECK(mailbox.Offer(MakeEnvelope("m1"), 1000, /*held=*/false).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m1"), 1001, /*held=*/false).status == PeerOfferStatus::Duplicate);
    CHECK(mailbox.pending() == 1);
    const auto drained = mailbox.Drain();
    REQUIRE(drained.size() == 1);
    CHECK(drained[0].envelope.message_id == "m1");
    CHECK(mailbox.pending() == 0);
}

TEST_CASE("接收决定随队列项冻结:Drain 出来的每封带着自己的 hold 档,顺序不倒") {
    PeerMailbox mailbox;
    CHECK(mailbox.Offer(MakeEnvelope("m1", "直收"), 1000, /*held=*/false).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m2", "扣住"), 1001, /*held=*/true).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m3", "再扣"), 1002, /*held=*/true).status == PeerOfferStatus::Accepted);
    const auto drained = mailbox.Drain();
    REQUIRE(drained.size() == 3);
    CHECK(drained[0].envelope.message_id == "m1");
    CHECK(drained[0].held == false);
    CHECK(drained[1].envelope.message_id == "m2");
    CHECK(drained[1].held);
    CHECK(drained[2].envelope.message_id == "m3");
    CHECK(drained[2].held);
}

TEST_CASE("重试回执沿用首收冻结的决定:扣住的信重发不误报可读") {
    PeerMailbox mailbox;
    // 首收扣住:回执 held,决定随去重账冻结。
    const PeerOfferResult first = mailbox.Offer(MakeEnvelope("m1", "扣住的正文"), 1000, /*held=*/true);
    CHECK(first.status == PeerOfferStatus::Accepted);
    CHECK(first.held);
    REQUIRE(mailbox.Drain().size() == 1);
    // 取走之后重试同一 message_id:不重复入队,回执仍是首收的 held。
    const PeerOfferResult retry_held = mailbox.Offer(MakeEnvelope("m1", "扣住的正文"), 1001, /*held=*/true);
    CHECK(retry_held.status == PeerOfferStatus::Duplicate);
    CHECK(retry_held.held);  // 旧账是"扣住",不回 delivered
    CHECK(mailbox.pending() == 0);  // 没有第二封入队
    // 直收的信重试同理:回执 delivered。
    const PeerOfferResult direct = mailbox.Offer(MakeEnvelope("m2", "直收的正文"), 1002, /*held=*/false);
    CHECK(direct.status == PeerOfferStatus::Accepted);
    CHECK_FALSE(direct.held);
    REQUIRE(mailbox.Drain().size() == 1);
    const PeerOfferResult retry_direct = mailbox.Offer(MakeEnvelope("m2", "直收的正文"), 1003, /*held=*/false);
    CHECK(retry_direct.status == PeerOfferStatus::Duplicate);
    CHECK_FALSE(retry_direct.held);
}

TEST_CASE("限速:同一发送方窗口内超过上限被拦,别的发送方不受连坐") {
    PeerMailbox mailbox(/*capacity=*/16, /*rate_limit=*/3, /*rate_window_seconds=*/30, /*dup_text_window_seconds=*/0);
    for (int i = 0; i < 3; ++i) {
        CHECK(mailbox.Offer(MakeEnvelope("a" + std::to_string(i), "t" + std::to_string(i)), 1000 + i, /*held=*/false)
                  .status == PeerOfferStatus::Accepted);
    }
    CHECK(mailbox.Offer(MakeEnvelope("a3", "t3"), 1003, /*held=*/false).status == PeerOfferStatus::RateLimited);
    // 另一个发送方照收。
    CHECK(mailbox.Offer(MakeEnvelope("b0", "t3", "peer-b"), 1003, /*held=*/false).status ==
          PeerOfferStatus::Accepted);
    // 窗口滑过去,又能收了。
    CHECK(mailbox.Offer(MakeEnvelope("a4", "t4"), 1000 + 31, /*held=*/false).status == PeerOfferStatus::Accepted);
}

TEST_CASE("相同正文短窗去重:同一发送方重发同一句话不重复入队") {
    PeerMailbox mailbox(16, 10, 30, 10);
    CHECK(mailbox.Offer(MakeEnvelope("m1", "一样的正文"), 1000, /*held=*/false).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m2", "一样的正文"), 1002, /*held=*/false).status ==
          PeerOfferStatus::DuplicateText);
    CHECK(mailbox.pending() == 1);
    // 正文变了或窗口过了,都算新信。
    CHECK(mailbox.Offer(MakeEnvelope("m3", "别的正文"), 1003, /*held=*/false).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m4", "一样的正文"), 1000 + 11, /*held=*/false).status ==
          PeerOfferStatus::Accepted);
    // 换个发送方发同一句,不算重复。
    CHECK(mailbox.Offer(MakeEnvelope("m5", "别的正文", "peer-c"), 1004, /*held=*/false).status ==
          PeerOfferStatus::Accepted);
}

TEST_CASE("队列硬上限:满了再收报 QueueFull") {
    PeerMailbox mailbox(/*capacity=*/2, 10, 30, 0);
    CHECK(mailbox.Offer(MakeEnvelope("m1", "t1"), 1000, /*held=*/false).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m2", "t2"), 1000, /*held=*/false).status == PeerOfferStatus::Accepted);
    CHECK(mailbox.Offer(MakeEnvelope("m3", "t3"), 1000, /*held=*/false).status == PeerOfferStatus::QueueFull);
    CHECK(mailbox.pending() == 2);
    // 被上限拦下的信没进任何账:腾出位子再收同一封,照常入队。
    CHECK(mailbox.Drain().size() == 2);
    CHECK(mailbox.Offer(MakeEnvelope("m3", "t3"), 1001, /*held=*/false).status == PeerOfferStatus::Accepted);
}

// ---------------------------------------------------------------------------
// SV-02 合同夹具(正文与 Hold 决定原子入队):旧账是传输线程先 Offer 正文、
// 再拿另一把锁记 held_ids_,主线程 Drain 夹在中间就把扣信当 delivered 放
// 行。交错全由栅栏钉死,不靠 sleep 赌时序——手艺同 test_workflow_parallel.cpp
// (仓里没有 std::barrier 先例,mutex+condvar 手搓)。
// ---------------------------------------------------------------------------

// 栅栏:两方齐到才放行(把"Offer 与 Drain 真同时在跑"钉成事实)。先例
// (test_workflow_parallel.cpp)那只闸一次就完;本册要轮轮复用,带上代数
// 计数,上一轮放行过后重新关门。
class Barrier {
public:
    explicit Barrier(int parties) : parties_(parties) {}

    void Arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        const int generation = generation_;
        if (++arrived_ >= parties_) {
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return generation_ != generation; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int arrived_ = 0;
    int generation_ = 0;
    const int parties_;
};

TEST_CASE("原子发布:传输线程 Offer 扣信与主线程 Drain 轮轮抢闸,扣住的决定一封不丢") {
    constexpr int kRounds = 256;
    // 限速放宽到装得下全部轮次;正文每封不同,别让正文去重抢戏。
    PeerMailbox mailbox(/*capacity=*/256, /*rate_limit=*/300, /*rate_window_seconds=*/30,
                        /*dup_text_window_seconds=*/0);
    Barrier gate(2);

    std::vector<PeerOfferStatus> offered;  // 传输线程只记账,断言全在 join 之后
    std::vector<PeerIncoming> drained;     // 只由主线程碰
    std::thread transport([&] {
        for (int i = 0; i < kRounds; ++i) {
            gate.Arrive();  // 与主线程对齐后放行,Offer/Drain 真抢同一把锁
            PeerEnvelope envelope = MakeEnvelope("race-" + std::to_string(i), "竞_" + std::to_string(i));
            offered.push_back(mailbox.Offer(std::move(envelope), 1000 + i, /*held=*/true).status);
        }
    });
    for (int i = 0; i < kRounds; ++i) {
        gate.Arrive();
        for (auto& incoming : mailbox.Drain()) {
            drained.push_back(std::move(incoming));
        }
    }
    transport.join();
    for (auto& incoming : mailbox.Drain()) {  // 收尾:残信一并取走
        drained.push_back(std::move(incoming));
    }

    // 每轮一封,顶多晚一轮被取走:一封不丢、一封不多。
    REQUIRE(drained.size() == kRounds);
    for (const auto& status : offered) {
        CHECK(status == PeerOfferStatus::Accepted);
    }
    // 不管哪一轮被夹走,扣住的决定必须随信到手——正文先落队、决定后补
    // 账的旧病,在这里现形。
    for (const auto& incoming : drained) {
        CHECK(incoming.held);
    }
}

TEST_CASE("默认权限档:五档强类型，非默认与跨项目都保守 hold") {
    using lubancode::ApprovalMode;
    CHECK(DefaultReceiveTier(ApprovalMode::Default, ApprovalMode::Default, false) ==
          PeerPermissionTier::Accept);
    const std::vector<ApprovalMode> non_default{ApprovalMode::AcceptEdits, ApprovalMode::Yolo,
                                                ApprovalMode::Auto, ApprovalMode::DontAsk};
    for (const auto mode : non_default) {
        CHECK(DefaultReceiveTier(mode, ApprovalMode::Default, false) == PeerPermissionTier::Hold);
        CHECK(DefaultReceiveTier(ApprovalMode::Default, mode, false) == PeerPermissionTier::Hold);
    }
    CHECK(DefaultReceiveTier(ApprovalMode::Default, ApprovalMode::Default, true) ==
          PeerPermissionTier::Hold);
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
