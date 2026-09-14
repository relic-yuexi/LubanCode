// QQ 接入单 Q2:ChannelManager 出站投递面(channel.send 受理/回执结算/
// 分型/超时/代次隔离/重复回执)。§七第三、四、五项的宿主侧合同。
#include <doctest/doctest.h>

#include <filesystem>
#include <vector>

#include "channel/manager.hpp"
#include "fake_channel_sidecar.hpp"

using namespace lubancode::channel;
using lubancode::test_support::FakeChannelSidecar;
using Outcome = ChannelManager::ChannelDeliveryOutcome;

namespace {

std::filesystem::path MakeStateRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-manager-send-" + std::string(tag));
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

ChannelAccountUserConfig OpenAccount() {
    ChannelAccountUserConfig config;
    config.enabled = true;
    config.transport = "websocket";
    config.secret_env = "QQBOT_SECRET";
    config.dm_policy = DmPolicy::Open;
    return config;
}

// 起 account 到 Running 并推进一轮 Pump。
struct RunningAccount {
    std::filesystem::path root;
    std::int64_t now = 1724700000000;
    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    std::unique_ptr<ChannelManager> manager;

    explicit RunningAccount(const char* tag) : root(MakeStateRoot(tag)) {
        ChannelManagerOptions options;
        options.state_root = root;
        options.now_ms = [this] { return now; };
        options.alive_checker = [](unsigned long) { return true; };
        options.send_timeout_ms = 30'000;
        manager = std::make_unique<ChannelManager>(std::move(options));
        REQUIRE(manager->AddAccount("qqbot", "main", OpenAccount(), &transport).status ==
                ChannelManager::AddAccountResult::Status::Ok);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
        REQUIRE(manager->Snapshot("qqbot", "main")->state == ChannelAccountState::Running);
    }

    ChannelManager::ChannelSendRequest Send(std::string delivery_id) {
        ChannelManager::ChannelSendRequest request;
        request.conversation_id = "dm-owner";
        request.text = "回复正文";
        request.reply_to_message_id = "m-in-1";
        request.client_delivery_id = std::move(delivery_id);
        return request;
    }
};

std::size_t CountOutcomes(const std::vector<Outcome>& outcomes, Outcome::Status status) {
    std::size_t count = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.status == status) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("SendReply:受理即发 channel.send,回执 Accepted 带 provider id") {
    RunningAccount fixture("accept");
    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-a1")).has_value());
    // 帧已写给 sidecar;Pump 收回执。
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    REQUIRE(fixture.sidecar.sent_messages()[0].client_id == "dl-a1");
    REQUIRE(fixture.sidecar.sent_messages()[0].params["conversation"]["id"] == "dm-owner");
    REQUIRE(fixture.sidecar.sent_messages()[0].params["reply_to_message_id"] == "m-in-1");
    fixture.manager->Pump("qqbot", "main");
    const auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
    REQUIRE(outcomes.size() == 1);
    REQUIRE(outcomes[0].status == Outcome::Status::Accepted);
    REQUIRE_FALSE(outcomes[0].provider_message_id.empty());
    REQUIRE(outcomes[0].client_delivery_id == "dl-a1");
    // 已结算的 delivery 不再受理(终态不翻转)。
    REQUIRE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-a1")).has_value());
}

TEST_CASE("SendReply:参数缺与账号不在 Running 都拒") {
    RunningAccount fixture("reject");
    ChannelManager::ChannelSendRequest bad = fixture.Send("");
    REQUIRE(fixture.manager->SendReply("qqbot", "main", bad).has_value());
    bad = fixture.Send("dl-x");
    bad.text.clear();
    REQUIRE(fixture.manager->SendReply("qqbot", "main", bad).has_value());
    REQUIRE(fixture.sidecar.sent_messages().empty());
    REQUIRE(fixture.manager->SendReply("qqbot", "other", fixture.Send("dl-y")).has_value());
}

TEST_CASE("限频分型:RateLimited 可重试;拒绝分 reply_window_expired/platform_reject") {
    RunningAccount fixture("classify");
    SUBCASE("rate_limited") {
        fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::RateLimitedFirst);
        fixture.sidecar.set_rate_limited_first(1);
        REQUIRE_FALSE(
            fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-r1")).has_value());
        fixture.manager->Pump("qqbot", "main");
        const auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
        REQUIRE(CountOutcomes(outcomes, Outcome::Status::RateLimited) == 1);
        REQUIRE(outcomes[0].error_code == "rate_limited");
        // 同 delivery 重试(适配器/泵层同 client_id)——第二次成功。
        REQUIRE_FALSE(
            fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-r1")).has_value());
        fixture.manager->Pump("qqbot", "main");
        const auto second = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
        REQUIRE(CountOutcomes(second, Outcome::Status::Accepted) == 1);
    }
    SUBCASE("回复窗口过期") {
        fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::PermanentReject);
        fixture.sidecar.set_reject_detail("msg_id expired");
        REQUIRE_FALSE(
            fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-w1")).has_value());
        fixture.manager->Pump("qqbot", "main");
        const auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
        REQUIRE(outcomes.size() == 1);
        REQUIRE(outcomes[0].status == Outcome::Status::Rejected);
        REQUIRE(outcomes[0].error_code == "reply_window_expired");
    }
    SUBCASE("平台明确拒绝") {
        fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::PermanentReject);
        REQUIRE_FALSE(
            fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-p1")).has_value());
        fixture.manager->Pump("qqbot", "main");
        const auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
        REQUIRE(outcomes[0].status == Outcome::Status::Rejected);
        REQUIRE(outcomes[0].error_code == "platform_reject");
    }
    SUBCASE("令牌失效") {
        fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::LoginRequired);
        REQUIRE_FALSE(
            fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-l1")).has_value());
        fixture.manager->Pump("qqbot", "main");
        const auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
        REQUIRE(outcomes[0].status == Outcome::Status::AuthFailed);
        REQUIRE(outcomes[0].error_code == "auth_failed");
    }
}

TEST_CASE("超时:无回执的在途 send 裁决 delivery_unknown,停自动重发") {
    RunningAccount fixture("timeout");
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::Silent);
    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-t1")).has_value());
    fixture.now += 10'000;
    fixture.manager->Pump("qqbot", "main");
    REQUIRE(fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main").empty());
    fixture.now += 21'000;  // 过 30s 帽
    fixture.manager->Pump("qqbot", "main");
    const auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
    REQUIRE(outcomes.size() == 1);
    REQUIRE(outcomes[0].status == Outcome::Status::Unknown);
    REQUIRE(outcomes[0].error_code == "delivery_unknown");
    REQUIRE_FALSE(fixture.manager->HasPendingSend("qqbot", "main", "dl-t1"));
    // 已结算:同 delivery 不再受理(不虚 exactly-once)。
    REQUIRE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-t1")).has_value());
}

TEST_CASE("重复回执只结一次;旧代次回执隔离") {
    RunningAccount fixture("dup-stale");
    // 受理一笔。
    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-d1")).has_value());
    fixture.manager->Pump("qqbot", "main");
    REQUIRE(fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main").size() == 1);
    // 重复 delivery.receipt 通知:已结算,只留诊断。
    fixture.sidecar.EmitDeliveryReceipt("dl-d1", "delivered");
    fixture.manager->Pump("qqbot", "main");
    REQUIRE(fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main").empty());

    // 旧代次:受理后重启账号(代次 +1),迟到的 send 应答不结算。
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::Silent);
    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-s1")).has_value());
    REQUIRE(fixture.manager->RestartAccount("qqbot", "main").has_value());
    fixture.manager->Pump("qqbot", "main");  // 握手 + start + 迟到的 send 应答
    fixture.manager->Pump("qqbot", "main");
    for (const auto& outcome :
         fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main")) {
        REQUIRE(outcome.client_delivery_id != "dl-s1");
    }
    // 陈旧回执走 delivery.receipt 的同款裁决:不结算。
    fixture.sidecar.EmitDeliveryReceipt("dl-s1", "delivered");
    fixture.manager->Pump("qqbot", "main");
    for (const auto& outcome :
         fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main")) {
        REQUIRE(outcome.client_delivery_id != "dl-s1");
    }
}

TEST_CASE("delivery.receipt 通知与在途 send 关联:delivered/failed/rate") {
    RunningAccount fixture("receipt");
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::Silent);
    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-k1")).has_value());
    fixture.sidecar.EmitDeliveryReceipt("dl-k1", "delivered");
    fixture.manager->Pump("qqbot", "main");
    auto outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
    REQUIRE(outcomes.size() == 1);
    REQUIRE(outcomes[0].status == Outcome::Status::Accepted);
    REQUIRE_FALSE(outcomes[0].provider_message_id.empty());

    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-k2")).has_value());
    fixture.sidecar.EmitDeliveryReceipt("dl-k2", "failed", "sender blocked");
    fixture.manager->Pump("qqbot", "main");
    outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
    REQUIRE(outcomes.size() == 1);
    REQUIRE(outcomes[0].status == Outcome::Status::Rejected);

    REQUIRE_FALSE(fixture.manager->SendReply("qqbot", "main", fixture.Send("dl-k3")).has_value());
    fixture.sidecar.EmitDeliveryReceipt("dl-k3", "failed", "rate limit hit");
    fixture.manager->Pump("qqbot", "main");
    outcomes = fixture.manager->DrainChannelDeliveryOutcomes("qqbot", "main");
    REQUIRE(outcomes.size() == 1);
    REQUIRE(outcomes[0].status == Outcome::Status::RateLimited);
}

TEST_CASE("状态迁移落 account-status.json,只读投影读得回") {
    RunningAccount fixture("statusfile");
    const auto status =
        ChannelManager::ReadAccountStatusFile(fixture.root / "qqbot" / "main");
    REQUIRE(status.present);
    REQUIRE(status.channel_id == "qqbot");
    REQUIRE(status.account_id == "main");
    REQUIRE(status.state == "running");
    REQUIRE(status.generation == 2);  // AddAccount(1) + StartAccount(+1)
    // 零副作用:不存在的目录给 present=false。
    REQUIRE_FALSE(ChannelManager::ReadAccountStatusFile(fixture.root / "nope").present);
}
