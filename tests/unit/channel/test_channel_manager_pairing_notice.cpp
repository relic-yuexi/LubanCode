// QQ 接入单 Q1b:manager 的 PendingPairing 提示链路册(假 sidecar 回路)。
//   - 未配对来信 → PendingPairing 裁决 → 提示入队(带码/会话/被动回复锚),
//     原信不进执行队列(ingress 落 rejected:pairing_pending);
//   - 提示限频:同 sender 冷却窗内第二封不重发;重启(重建 manager)不
//     重发——持久已提示账;
//   - 被拒 sender 不再收提示;
//   - 批准后新来信放行(原消息不自动执行,须重发)。
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

#include "channel/manager.hpp"
#include "fake_channel_sidecar.hpp"

using namespace lubancode::channel;
using lubancode::test_support::FakeChannelSidecar;

namespace {

std::filesystem::path MakeStateRoot(const char* test_name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-pairing-notice-" + std::string(test_name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

class FakeTransport final : public ChannelBridgeTransport {
public:
    explicit FakeTransport(FakeChannelSidecar& sidecar) : sidecar_(sidecar) {}
    void WriteToSidecar(const std::byte* data, std::size_t size) override {
        sidecar_.FeedFromHost(data, size);
    }
    std::vector<std::byte> DrainFromSidecar() override { return sidecar_.DrainToHost(); }

private:
    FakeChannelSidecar& sidecar_;
};

ChannelAccountUserConfig PairingAccount() {
    ChannelAccountUserConfig config;
    config.enabled = true;
    config.transport = "websocket";
    config.secret_env = "QQBOT_SECRET";
    config.dm_policy = DmPolicy::Pairing;  // QQ 模板策略
    config.group_policy = GroupPolicy::Disabled;
    config.allow_bots = false;
    return config;
}

ChannelInboundEvent MakeStrangerDm(const std::string& delivery_id, const std::string& sender,
                                   const std::string& text = "你好") {
    ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = "pe-" + delivery_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = 1724700000000;
    event.provider_at_ms = 1724699999000;
    event.conversation.kind = ConversationKind::Direct;
    event.conversation.id = "dm-" + sender;
    event.sender.id = sender;
    event.sender.display_name = "陌生人";
    event.message_id = "m-" + delivery_id;
    ChannelPart part;
    part.type = ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

// 可变钟的装配:now 由测试递进;Rebuild = 同 root 重建(模拟重启,
// 盘上的 pairing/notice 账保留)。
struct NoticeFixture {
    std::filesystem::path root;
    std::int64_t now = 1724700000000;
    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    std::unique_ptr<ChannelManager> manager;

    explicit NoticeFixture(const char* tag) : root(MakeStateRoot(tag)) { Rebuild(); }

    void Rebuild() {
        ChannelManagerOptions options;
        options.state_root = root;
        options.now_ms = [this] { return now; };
        options.alive_checker = [](unsigned long) { return true; };
        manager = std::make_unique<ChannelManager>(std::move(options));
        REQUIRE(manager
                    ->AddAccount("qqbot", "main", PairingAccount(), &transport)
                    .status == ChannelManager::AddAccountResult::Status::Ok);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
        REQUIRE(manager->Snapshot("qqbot", "main")->state == ChannelAccountState::Running);
    }

    // 喂一封私信并泵完入账。
    void Deliver(const ChannelInboundEvent& event) {
        sidecar.EmitInboundEvent(event);
        manager->Pump("qqbot", "main");
    }
};

}  // namespace

TEST_CASE("Q1b 未配对来信:提示入队带码带锚,原信不进执行队列") {
    NoticeFixture fixture("queue");
    fixture.Deliver(MakeStrangerDm("d1", "stranger-1", "帮我干活"));

    // 原信不进执行队列(零模型路径),ingress 落 rejected:pairing_pending。
    CHECK_FALSE(fixture.manager->TakeNextWork("qqbot", "main").has_value());
    // 提示入队:一枚,带码与被动回复锚。
    const auto notices = fixture.manager->DrainPendingPairingNotices("qqbot", "main");
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].code.size() == kPairingCodeLength);
    CHECK(notices[0].conversation_id == "dm-stranger-1");
    CHECK(notices[0].reply_to_message_id == "m-d1");
    CHECK(notices[0].sender_id == "stranger-1");
    CHECK(notices[0].text.find(notices[0].code) != std::string::npos);
    CHECK(notices[0].text.find("lubancode channel pairing approve qqbot main") != std::string::npos);
    // 排水即取走:第二次 drain 空。
    CHECK(fixture.manager->DrainPendingPairingNotices("qqbot", "main").empty());
    // 配对账上有一枚 pending。
    CHECK(fixture.manager->PendingPairings("qqbot", "main").size() == 1);
}

TEST_CASE("Q1b 提示限频:冷却窗内第二封不重发;重启不重发") {
    NoticeFixture fixture("rate_limit");
    fixture.Deliver(MakeStrangerDm("d1", "stranger-1"));
    REQUIRE(fixture.manager->DrainPendingPairingNotices("qqbot", "main").size() == 1);

    // 窗内第二封(过 30s code 冷却,不过 5min 提示冷却):有新码,无新提示。
    fixture.now += kPairingRequestCooldownMs + 1000;
    fixture.Deliver(MakeStrangerDm("d2", "stranger-1"));
    CHECK(fixture.manager->DrainPendingPairingNotices("qqbot", "main").empty());

    // 重启(重建 manager,同 root):持久已提示账仍在,同窗内第三封也不重发。
    fixture.Rebuild();
    fixture.now += kPairingRequestCooldownMs + 1000;
    fixture.Deliver(MakeStrangerDm("d3", "stranger-1"));
    CHECK(fixture.manager->DrainPendingPairingNotices("qqbot", "main").empty());

    // 冷却窗过了:新来信出新提示(带新码)。
    fixture.now += kPairingNoticeCooldownMs;
    fixture.Deliver(MakeStrangerDm("d4", "stranger-1"));
    const auto notices = fixture.manager->DrainPendingPairingNotices("qqbot", "main");
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].reply_to_message_id == "m-d4");
}

TEST_CASE("Q1b 被拒 sender:不再收提示,也不再发新码") {
    NoticeFixture fixture("rejected");
    fixture.Deliver(MakeStrangerDm("d1", "spammer"));
    const auto notices = fixture.manager->DrainPendingPairingNotices("qqbot", "main");
    REQUIRE(notices.size() == 1);
    std::string error;
    REQUIRE(fixture.manager->RejectPairing("qqbot", "main", notices[0].code, &error).has_value());

    // 拒过之后:同 sender 再来信,零提示零新码。
    fixture.now += kPairingNoticeCooldownMs + kPairingRequestCooldownMs;
    fixture.Deliver(MakeStrangerDm("d2", "spammer"));
    CHECK(fixture.manager->DrainPendingPairingNotices("qqbot", "main").empty());
    CHECK(fixture.manager->PendingPairings("qqbot", "main").empty());
}

TEST_CASE("Q1b 批准后放行:原消息不自动执行,新来信进执行队列") {
    NoticeFixture fixture("approve");
    fixture.Deliver(MakeStrangerDm("d1", "stranger-1", "第一句"));
    const auto notices = fixture.manager->DrainPendingPairingNotices("qqbot", "main");
    REQUIRE(notices.size() == 1);
    std::string error;
    const auto approved =
        fixture.manager->ApprovePairing("qqbot", "main", notices[0].code, &error);
    REQUIRE(approved.has_value());
    CHECK(*approved == "stranger-1");

    // 第一封信已被裁决 rejected:不因批准而自动执行。
    CHECK_FALSE(fixture.manager->TakeNextWork("qqbot", "main").has_value());

    // 用户重发:进执行队列(Admitted)。
    fixture.Deliver(MakeStrangerDm("d2", "stranger-1", "重发的指令"));
    const auto work = fixture.manager->TakeNextWork("qqbot", "main");
    REQUIRE(work.has_value());
    CHECK(work->route.status == RouteDecision::Status::Admitted);
    CHECK(work->sender_id == "stranger-1");
}

TEST_CASE("Q1b 按身份批准:控制入口的身份路走通") {
    NoticeFixture fixture("by_sender");
    fixture.Deliver(MakeStrangerDm("d1", "stranger-9"));
    REQUIRE(fixture.manager->DrainPendingPairingNotices("qqbot", "main").size() == 1);
    std::string error;
    const auto approved = fixture.manager->ApprovePairingBySender("qqbot", "main", "stranger-9",
                                                                 &error);
    REQUIRE(approved.has_value());
    CHECK(*approved == "stranger-9");
    // 批准持久:重启后仍放行。
    fixture.Rebuild();
    fixture.Deliver(MakeStrangerDm("d2", "stranger-9", "重启后的来信"));
    const auto work = fixture.manager->TakeNextWork("qqbot", "main");
    REQUIRE(work.has_value());
    CHECK(work->route.status == RouteDecision::Status::Admitted);
    CHECK(fixture.manager->DrainPendingPairingNotices("qqbot", "main").empty());
}
