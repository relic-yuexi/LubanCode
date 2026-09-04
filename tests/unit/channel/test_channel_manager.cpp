// 多渠道消息接入单阶段 2:ChannelManager 册——多账号生命周期 + 入站水路
// 的端到端冒烟(假 sidecar 管道,不连真 IM)。覆盖:
//   - 握手/起停/状态机迁移账与 generation;
//   - 账号锁:活进程持有即拒(另一实例路径);
//   - channel.inbound:去重落账 -> ack -> 准入 -> inbox;
//   - 队列满背压:nack retry,事件留在账上不默丢;
//   - 阶段 2 验收剧本:ack 前杀宿主,重启重送只留一枚 durable 事件;
//   - IdleWakeCoordinator 经适配器接线(AnyReady 随 pending 变化);
//   - pairing 挂进 DM 水路;bot 拒绝;优雅停(锁释放)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#include "channel/manager.hpp"
#include "fake_channel_sidecar.hpp"
#include "runtime/idle_wake.hpp"

using namespace lubancode::channel;
using lubancode::test_support::FakeChannelSidecar;

namespace {

std::filesystem::path MakeStateRoot(const char* test_name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-manager-test" + std::string(test_name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// fake sidecar 挂上 manager 的 Transport。
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

// ChannelWakeCoordinator -> 真 IdleWakeCoordinator 的装配适配器
//(channel 库不能反向 include runtime,适配器住装配层;测试里就地写)。
class IdleWakeAdapter final : public ChannelWakeCoordinator {
public:
    void SetAccountWakeSource(const std::string& channel_id, const std::string& account_id,
                              std::function<bool()> ready) override {
        tokens_[channel_id + "/" + account_id] =
            coordinator_.AddSource("channel:" + channel_id + ":" + account_id, std::move(ready));
    }
    void ClearAccountWakeSource(const std::string& channel_id,
                                const std::string& account_id) override {
        tokens_.erase(channel_id + "/" + account_id);
    }
    bool AnyReady() const { return coordinator_.AnyReady(); }

private:
    lubancode::runtime::IdleWakeCoordinator coordinator_;
    std::map<std::string, lubancode::runtime::IdleWakeCoordinator::Subscription> tokens_;
};

ChannelAccountUserConfig OpenAccount() {
    ChannelAccountUserConfig config;
    config.enabled = true;
    config.transport = "websocket";
    config.secret_env = "QQBOT_SECRET";
    config.dm_policy = DmPolicy::Open;
    return config;
}

ChannelInboundEvent MakeDm(const std::string& delivery_id, const std::string& provider_event_id,
                           const std::string& text = "在吗") {
    ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = 1724700000000;
    event.provider_at_ms = 1724699999000;
    event.conversation.kind = ConversationKind::Direct;
    event.conversation.id = "dm-owner";
    event.sender.id = "owner-openid";
    event.sender.display_name = "老板";
    event.message_id = "m-" + delivery_id;
    ChannelPart part;
    part.type = ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

ChannelManagerOptions MakeOptions(const std::filesystem::path& root) {
    ChannelManagerOptions options;
    options.state_root = root;
    options.now_ms = [] { return 1724700000000; };
    options.alive_checker = [](unsigned long) { return true; };
    return options;
}

// 起 account 到 Running,逐泵推进。
ChannelManager::AddAccountResult AddAndStart(ChannelManager& manager, FakeChannelSidecar& /*sidecar*/,
                                             FakeTransport& transport,
                                             const ChannelAccountUserConfig& config = OpenAccount()) {
    auto added = manager.AddAccount("qqbot", "main", config, &transport);
    if (added.status != ChannelManager::AddAccountResult::Status::Ok) return added;
    REQUIRE_FALSE(manager.StartAccount("qqbot", "main").has_value());
    manager.Pump("qqbot", "main");  // initialize 往返 + 发 start
    manager.Pump("qqbot", "main");  // start 往返 -> Running
    return added;
}

}  // namespace

TEST_CASE("握手与起停:状态机走到 Running,迁移账带 generation") {
    const auto root = MakeStateRoot("lifecycle");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    ChannelManager manager(MakeOptions(root));

    const auto added = AddAndStart(manager, sidecar, transport);
    REQUIRE(added.status == ChannelManager::AddAccountResult::Status::Ok);
    CHECK(sidecar.handshake_completed());
    CHECK(sidecar.started());
    CHECK_FALSE(sidecar.stopped());

    const auto snapshot = manager.Snapshot("qqbot", "main");
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->state == ChannelAccountState::Running);
    CHECK(snapshot->generation == 2);  // 注册 1,起号 +1
    CHECK(snapshot->lock_held);

    // 停:stop 往返,Stopped,锁释放。
    REQUIRE_FALSE(manager.StopAccount("qqbot", "main").has_value());
    CHECK(sidecar.stopped());
    const auto stopped = manager.Snapshot("qqbot", "main");
    REQUIRE(stopped.has_value());
    CHECK(stopped->state == ChannelAccountState::Stopped);
    CHECK_FALSE(stopped->lock_held);

    // 再停幂等。
    REQUIRE_FALSE(manager.StopAccount("qqbot", "main").has_value());

    // 重启:新 generation。
    REQUIRE_FALSE(manager.StartAccount("qqbot", "main").has_value());
    manager.Pump("qqbot", "main");
    manager.Pump("qqbot", "main");
    const auto again = manager.Snapshot("qqbot", "main");
    REQUIRE(again.has_value());
    CHECK(again->state == ChannelAccountState::Running);
    CHECK(again->generation == 3);
}

TEST_CASE("协议版本不合:明败不重试,落不可恢复终态") {
    const auto root = MakeStateRoot("protocol_mismatch");
    FakeChannelSidecar sidecar;
    sidecar.ForceProtocolMismatchOnNextHandshake();
    FakeTransport transport(sidecar);
    ChannelManager manager(MakeOptions(root));

    REQUIRE(manager.AddAccount("qqbot", "main", OpenAccount(), &transport).status ==
            ChannelManager::AddAccountResult::Status::Ok);
    REQUIRE_FALSE(manager.StartAccount("qqbot", "main").has_value());
    manager.Pump("qqbot", "main");
    const auto snapshot = manager.Snapshot("qqbot", "main");
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->state == ChannelAccountState::Fatal);
    // 不可恢复:再起被拒。
    CHECK(manager.StartAccount("qqbot", "main").has_value());
}

TEST_CASE("账号锁:另一活实例持有时 AddAccount 拒绝") {
    const auto root = MakeStateRoot("lock_refused");
    // 第一只 manager 占住锁(观测注册,不接 transport)。
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    {
        ChannelManager first(MakeOptions(root));
        REQUIRE(first.AddAccount("qqbot", "main", OpenAccount(), &transport).status ==
                ChannelManager::AddAccountResult::Status::Ok);
        // 第二只 manager 同 state root:锁被活进程(本测试进程)持有。
        ChannelManager second(MakeOptions(root));
        const auto refused = second.AddAccount("qqbot", "main", OpenAccount(), &transport);
        CHECK(refused.status == ChannelManager::AddAccountResult::Status::LockRefused);
        CHECK(refused.detail.find("pid") != std::string::npos);
        CHECK(second.account_count() == 0);
        // 第一只析构释放锁后,第二只能进。
    }
    ChannelManager third(MakeOptions(root));
    CHECK(third.AddAccount("qqbot", "main", OpenAccount(), &transport).status ==
          ChannelManager::AddAccountResult::Status::Ok);
}

TEST_CASE("入站水路:来信 durable+ack+inbox;重投 duplicate 也 ack") {
    const auto root = MakeStateRoot("inbound_flow");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    ChannelManager manager(MakeOptions(root));
    AddAndStart(manager, sidecar, transport);

    // 假 sidecar 上报一封 DM。
    sidecar.EmitInboundEvent(MakeDm("in-1", "pe-1"));
    auto bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    // ack 的 request 已排队;Pump 送出去。
    manager.Pump("qqbot", "main");
    REQUIRE(sidecar.acked_delivery_ids().size() == 1);
    CHECK(sidecar.acked_delivery_ids()[0] == "in-1");

    // inbox 有一件活;取走推进 running。
    REQUIRE(manager.HasPendingWork("qqbot", "main"));
    const auto work = manager.TakeNextWork("qqbot", "main");
    REQUIRE(work.has_value());
    CHECK(work->event.delivery_id == "in-1");
    CHECK(work->conversation_id == "dm-owner");
    CHECK_FALSE(manager.HasPendingWork("qqbot", "main"));

    // 同 provider_event_id 重投:duplicate,ack 仍发,不开新账。
    sidecar.EmitInboundEvent(MakeDm("in-1", "pe-1"));
    bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    manager.Pump("qqbot", "main");
    REQUIRE(sidecar.acked_delivery_ids().size() == 2);
    CHECK(sidecar.acked_delivery_ids()[1] == "in-1");

    const auto snapshot = manager.Snapshot("qqbot", "main");
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->ingress_state_counts.at("running") == 1);  // 只一枚事件
    CHECK(manager.TakeNextWork("qqbot", "main") == std::nullopt);
}

TEST_CASE("背压:队列满不默丢——nack retry,事件留在账上") {
    const auto root = MakeStateRoot("backpressure");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    auto options = MakeOptions(root);
    options.inbox_limits.max_pending_total = 1;
    options.inbox_limits.sender_rate_max = 0;
    options.inbox_limits.same_content_window_ms = 0;
    ChannelManager manager(options);
    AddAndStart(manager, sidecar, transport);

    // 第一封进队。
    sidecar.EmitInboundEvent(MakeDm("in-1", "pe-1", "第一句"));
    auto bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    CHECK(manager.HasPendingWork("qqbot", "main"));

    // 第二封:总量到帽。durable 已落、ack 已发(耐久成功即 ack);inbox 拒
    // ——不 nack(重发只会撞去重键),事件留 rate_limited 账。
    sidecar.EmitInboundEvent(MakeDm("in-2", "pe-2", "第二句"));
    bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    manager.Pump("qqbot", "main");
    // 两封都 ack:耐久都成了,sidecar 都可以清 spool。
    REQUIRE(sidecar.acked_delivery_ids().size() == 2);
    CHECK(sidecar.acked_delivery_ids()[0] == "in-1");
    CHECK(sidecar.acked_delivery_ids()[1] == "in-2");
    // 事件没丢:ingress 账上 rate_limited 一枚,durable 两枚。
    const auto snapshot = manager.Snapshot("qqbot", "main");
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->ingress_state_counts.at("queued") == 1);
    CHECK(snapshot->ingress_state_counts.at("rate_limited") == 1);
    CHECK(snapshot->inbox_pending == 1);  // 只有第一封在队
}

TEST_CASE("阶段 2 验收剧本:ack 前杀宿主,重启重送,只留一枚 durable") {
    const auto root = MakeStateRoot("kill_host");
    std::string delivery = "in-7";
    // 第一只宿主:收信、durable 落账,ack 没泵出去就崩溃(直接析构)。
    {
        FakeChannelSidecar sidecar;
        FakeTransport transport(sidecar);
        ChannelManager manager(MakeOptions(root));
        AddAndStart(manager, sidecar, transport);
        sidecar.EmitInboundEvent(MakeDm(delivery, "pe-7", "验收"));
        auto bytes = sidecar.DrainToHost();
        manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
        // 不 Pump:ack 还在出站队列里。杀宿主。
    }
    // 第二只宿主:同 state root,sidecar 重送同一 delivery。
    {
        FakeChannelSidecar sidecar;
        FakeTransport transport(sidecar);
        ChannelManager manager(MakeOptions(root));
        AddAndStart(manager, sidecar, transport);
        sidecar.EmitInboundEvent(MakeDm(delivery, "pe-7", "验收"));
        auto bytes = sidecar.DrainToHost();
        manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
        manager.Pump("qqbot", "main");  // duplicate 的 ack 出门

        REQUIRE(sidecar.acked_delivery_ids().size() == 1);
        CHECK(sidecar.acked_delivery_ids()[0] == delivery);
        const auto snapshot = manager.Snapshot("qqbot", "main");
        REQUIRE(snapshot.has_value());
        // durable 事件只一枚(queued),没有为重投开新账。
        CHECK(snapshot->ingress_state_counts.at("queued") == 1);
        CHECK(snapshot->ingress_state_counts.size() == 1);
        // journal 里 evt 行只一行。
        const auto journal = root / "qqbot" / "main" / "ingress" / "journal.jsonl";
        std::ifstream stream(journal);
        std::string line;
        int evt_lines = 0;
        while (std::getline(stream, line)) {
            if (line.find("\"t\":\"evt\"") != std::string::npos) ++evt_lines;
        }
        CHECK(evt_lines == 1);
    }
}

TEST_CASE("pairing 挂进 DM 水路:未配对拒进,approve 后放行") {
    const auto root = MakeStateRoot("pairing_flow");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    auto config = OpenAccount();
    config.dm_policy = DmPolicy::Pairing;
    ChannelManager manager(MakeOptions(root));

    // 预置一只已批准的 sender(先直接开 pairing 账,approve 后 manager
    // 的 AddAccount replay 到 approved 记录——持久账跨实例,正是本意)。
    {
        constexpr std::int64_t kT0 = 1'000'000;
        auto pairing = PairingStore::Open(root / "qqbot" / "main", "qqbot", "main");
        REQUIRE(pairing != nullptr);
        const auto code = pairing->RequestPairing("vip-openid", kT0, [] { return "ABCD2345"; });
        REQUIRE(code.has_value());
        REQUIRE(pairing->Approve(*code, kT0 + 10).has_value());
    }

    AddAndStart(manager, sidecar, transport, config);

    // 未配对的 DM:rejected,不进 inbox,pending 挂一枚。
    sidecar.EmitInboundEvent(MakeDm("in-1", "pe-1"));
    auto bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    manager.Pump("qqbot", "main");
    {
        const auto snapshot = manager.Snapshot("qqbot", "main");
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->ingress_state_counts.at("rejected") == 1);
        CHECK(snapshot->pairing_pending == 1);
        CHECK(snapshot->pairing_approved == 1);  // replay 到预置批准
        CHECK_FALSE(manager.HasPendingWork("qqbot", "main"));
    }
    // 限速:同 sender 第二封不再开新 pending。
    sidecar.EmitInboundEvent(MakeDm("in-2", "pe-2"));
    bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    manager.Pump("qqbot", "main");
    {
        const auto snapshot = manager.Snapshot("qqbot", "main");
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->ingress_state_counts.at("rejected") == 2);
        CHECK(snapshot->pairing_pending == 1);
    }
    // 已批准的 sender:放行进 inbox。
    ChannelInboundEvent vip_event = MakeDm("in-3", "pe-3");
    vip_event.sender.id = "vip-openid";
    sidecar.EmitInboundEvent(vip_event);
    bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    CHECK(manager.HasPendingWork("qqbot", "main"));
}

TEST_CASE("bot 来信默认拒绝(allow_bots=false)") {
    const auto root = MakeStateRoot("bot_rejected");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    ChannelManager manager(MakeOptions(root));
    AddAndStart(manager, sidecar, transport);

    ChannelInboundEvent bot_event = MakeDm("in-1", "pe-1");
    bot_event.sender.is_bot = true;
    sidecar.EmitInboundEvent(bot_event);
    auto bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    manager.Pump("qqbot", "main");
    const auto snapshot = manager.Snapshot("qqbot", "main");
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->ingress_state_counts.at("rejected") == 1);
    CHECK_FALSE(manager.HasPendingWork("qqbot", "main"));
    // bot 的信也 ack 了(收下了,只是不进 Agent)。
    REQUIRE(sidecar.acked_delivery_ids().size() == 1);
}

TEST_CASE("IdleWake 接线:pending 有活 AnyReady,取空让位") {
    const auto root = MakeStateRoot("idle_wake");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    IdleWakeAdapter wake;
    auto options = MakeOptions(root);
    options.wake = &wake;
    ChannelManager manager(options);
    AddAndStart(manager, sidecar, transport);

    CHECK_FALSE(wake.AnyReady());
    sidecar.EmitInboundEvent(MakeDm("in-1", "pe-1"));
    auto bytes = sidecar.DrainToHost();
    manager.HandleBytesFromSidecar("qqbot", "main", bytes.data(), bytes.size());
    CHECK(wake.AnyReady());
    // 取走活,ready 归 false。
    REQUIRE(manager.TakeNextWork("qqbot", "main").has_value());
    CHECK_FALSE(wake.AnyReady());
}

TEST_CASE("优雅停:析构走 stop 收口,锁文件清") {
    const auto root = MakeStateRoot("destructor_stop");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    {
        ChannelManager manager(MakeOptions(root));
        AddAndStart(manager, sidecar, transport);
        REQUIRE(manager.Snapshot("qqbot", "main")->state == ChannelAccountState::Running);
    }
    // 析构已走 stop + 释放锁。
    CHECK(sidecar.stopped());
    const auto lock_file = root / "locks" / "qqbot-main.lock";
    CHECK_FALSE(std::filesystem::exists(lock_file));
}

TEST_CASE("传输故障:可重试的进 Degraded/Backoff,退避账带 retry_at") {
    const auto root = MakeStateRoot("transport_failure");
    FakeChannelSidecar sidecar;
    FakeTransport transport(sidecar);
    ChannelManager manager(MakeOptions(root));
    AddAndStart(manager, sidecar, transport);

    manager.NotifyTransportFailure("qqbot", "main", "transport_failed", "ws 断了");
    {
        const auto snapshot = manager.Snapshot("qqbot", "main");
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state == ChannelAccountState::Degraded);
        CHECK(snapshot->retry_at_ms > 0);
        CHECK(snapshot->backoff_attempt == 1);
    }
    manager.NotifyTransportFailure("qqbot", "main", "transport_failed", "又断");
    {
        const auto snapshot = manager.Snapshot("qqbot", "main");
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state == ChannelAccountState::Backoff);
        CHECK(snapshot->backoff_attempt == 2);
    }
    // 不可自愈:login_required 落 NeedsLogin。
    manager.NotifyTransportFailure("qqbot", "main", "login_required", "token 失效");
    const auto snapshot = manager.Snapshot("qqbot", "main");
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->state == ChannelAccountState::NeedsLogin);
}
