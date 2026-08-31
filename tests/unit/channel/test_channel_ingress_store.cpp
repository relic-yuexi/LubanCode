// 多渠道消息接入单阶段 2:ChannelIngressStore 册——append-only journal、
// 三级去重、replay 容错、dead letter。含阶段 2 验收剧本:
// "ack 前杀宿主,重启后重送,只留一枚 durable 事件"。
// 唯一真源 docs/architecture/channels/message-contracts.md §3-4。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "channel/ingress_store.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeAccountDir(const char* test_name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-ingress-test" + std::string(test_name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

ChannelInboundEvent MakeEvent(const std::string& delivery_id, const std::string& provider_event_id,
                              const std::string& message_id = "", const std::string& text = "你好") {
    ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = 1724700000000;
    event.provider_at_ms = 1724699999000;
    event.conversation.kind = ConversationKind::Direct;
    event.conversation.id = "dm-1";
    event.sender.id = "sender-1";
    event.message_id = message_id;
    ChannelPart part;
    part.type = ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

std::unique_ptr<ChannelIngressStore> OpenStore(const std::filesystem::path& dir) {
    ChannelIngressStore::OpenResult result;
    auto store = ChannelIngressStore::Open(dir, "qqbot", "main", &result);
    REQUIRE(store != nullptr);
    REQUIRE_FALSE(store->write_blocked());
    return store;
}

}  // namespace

TEST_CASE("去重键三级:provider_event_id 优先,message_id 退,指纹兜底") {
    std::string digest;
    // 一级。
    DedupeKey tier1 = ComputeDedupeKey(MakeEvent("d1", "pe-1", "m-1"), &digest);
    CHECK(tier1.tier == 1);
    CHECK(tier1.key == "p:qqbot:main:pe-1");
    CHECK(tier1.window_until_ms == 0);  // 永久键
    CHECK_FALSE(digest.empty());
    // 二级:provider_event_id 空时退 message_id。
    DedupeKey tier2 = ComputeDedupeKey(MakeEvent("d2", "", "m-2"), nullptr);
    CHECK(tier2.tier == 2);
    CHECK(tier2.key == "m:qqbot:main:dm-1:m-2");
    // 三级:两级都空走指纹(时间桶 10s)。
    DedupeKey tier3 = ComputeDedupeKey(MakeEvent("d3", "", ""), nullptr);
    CHECK(tier3.tier == 3);
    CHECK(tier3.key.rfind("f:sender-1:", 0) == 0);
    CHECK(tier3.window_until_ms > 0);  // 短窗
    // 同桶同正文同键;跨桶不同键。
    ChannelInboundEvent bucket_next = MakeEvent("d4", "", "");
    bucket_next.provider_at_ms = 1724699999000 + 10 * 1000;  // 下一桶
    CHECK(ComputeDedupeKey(bucket_next, nullptr).key != tier3.key);
}

TEST_CASE("Ingest:新事件落 durable;同 provider_event_id 重投判 duplicate") {
    const auto dir = MakeAccountDir("ingest_dedupe");
    auto store = OpenStore(dir);

    const auto first = store->Ingest(MakeEvent("in-1", "pe-1"));
    REQUIRE(first.has_value());
    CHECK(first->status == ChannelIngressStore::IngestOutcome::Status::Accepted);
    CHECK(first->sid == 1);
    CHECK(first->ack);

    // 同 provider_event_id(新 delivery_id)重投。
    const auto replay = store->Ingest(MakeEvent("in-2", "pe-1"));
    REQUIRE(replay.has_value());
    CHECK(replay->status == ChannelIngressStore::IngestOutcome::Status::Duplicate);
    CHECK(replay->sid == 1);
    CHECK(replay->ack);  // duplicate 也 ack,让 sidecar 清 spool

    // 同 delivery_id 重发(ack 前的退避重发)同样 duplicate。
    const auto same_delivery = store->Ingest(MakeEvent("in-1", "pe-1"));
    REQUIRE(same_delivery.has_value());
    CHECK(same_delivery->status == ChannelIngressStore::IngestOutcome::Status::Duplicate);

    // 账上只有一枚事件。
    CHECK(store->Records().size() == 1);
    CHECK(store->FindByDeliveryId("in-1").has_value());
    CHECK_FALSE(store->FindByDeliveryId("in-2").has_value());
}

TEST_CASE("指纹第三级:短窗内同正文同 sender 去重,窗口外放行") {
    const auto dir = MakeAccountDir("fingerprint_window");
    auto store = OpenStore(dir);

    ChannelInboundEvent first_event = MakeEvent("in-1", "", "");
    first_event.received_at_ms = 1'000'000;
    REQUIRE(store->Ingest(first_event).has_value());

    // 短窗内(同桶同 digest 同 sender):duplicate。
    ChannelInboundEvent within = MakeEvent("in-2", "", "");
    within.received_at_ms = 1'000'000 + 1000;
    const auto replay = store->Ingest(within);
    REQUIRE(replay.has_value());
    CHECK(replay->status == ChannelIngressStore::IngestOutcome::Status::Duplicate);

    // 短窗外:新事件。
    ChannelInboundEvent outside = MakeEvent("in-3", "", "");
    outside.received_at_ms = 1'000'000 + kFingerprintWindowMs + 1;
    const auto fresh = store->Ingest(outside);
    REQUIRE(fresh.has_value());
    CHECK(fresh->status == ChannelIngressStore::IngestOutcome::Status::Accepted);
    CHECK(store->Records().size() == 2);
}

TEST_CASE("状态机主线与旁路:非法迁移拒绝,终态无出边") {
    CHECK(CanIngressTransition(IngressEventState::Durable, IngressEventState::Authorized));
    CHECK(CanIngressTransition(IngressEventState::Authorized, IngressEventState::Routed));
    CHECK(CanIngressTransition(IngressEventState::Routed, IngressEventState::Queued));
    CHECK(CanIngressTransition(IngressEventState::Queued, IngressEventState::Running));
    CHECK(CanIngressTransition(IngressEventState::Running, IngressEventState::Replied));
    CHECK(CanIngressTransition(IngressEventState::Replied, IngressEventState::Delivered));
    CHECK(CanIngressTransition(IngressEventState::Delivered, IngressEventState::Archived));
    // 旁路。
    CHECK(CanIngressTransition(IngressEventState::Durable, IngressEventState::Rejected));
    CHECK(CanIngressTransition(IngressEventState::Queued, IngressEventState::RateLimited));
    // 跳步与终态出边非法。
    CHECK_FALSE(CanIngressTransition(IngressEventState::Durable, IngressEventState::Queued));
    CHECK_FALSE(CanIngressTransition(IngressEventState::Archived, IngressEventState::Delivered));
    CHECK_FALSE(CanIngressTransition(IngressEventState::RateLimited, IngressEventState::Queued));
    CHECK(IsIngressTerminalState(IngressEventState::Archived));
    CHECK(IsIngressTerminalState(IngressEventState::RateLimited));
    CHECK_FALSE(IsIngressTerminalState(IngressEventState::Running));
}

TEST_CASE("Transition 落账并更新内存态;重开后由 journal 重建") {
    const auto dir = MakeAccountDir("transition_replay");
    std::int64_t sid = 0;
    {
        auto store = OpenStore(dir);
        const auto ingest = store->Ingest(MakeEvent("in-1", "pe-1"));
        REQUIRE(ingest.has_value());
        sid = ingest->sid;
        CHECK_FALSE(store->Transition(sid, IngressEventState::Authorized, "").has_value());
        CHECK_FALSE(store->Transition(sid, IngressEventState::Routed, "").has_value());
        CHECK_FALSE(store->Transition(sid, IngressEventState::Queued, "").has_value());
        // 非法:queued -> delivered。
        CHECK(store->Transition(sid, IngressEventState::Delivered, "").has_value());
        // 未知 sid。
        CHECK(store->Transition(999, IngressEventState::Queued, "").has_value());
    }
    // 重开:状态由 tr 行重建。
    auto reopened = OpenStore(dir);
    const auto record = reopened->FindBySid(sid);
    REQUIRE(record.has_value());
    CHECK(record->state == IngressEventState::Queued);
    CHECK(reopened->StateCounts()["queued"] == 1);
    // 去重索引也在:重投同 provider_event_id 仍 duplicate。
    const auto replay = reopened->Ingest(MakeEvent("in-9", "pe-1"));
    REQUIRE(replay.has_value());
    CHECK(replay->status == ChannelIngressStore::IngestOutcome::Status::Duplicate);
    CHECK(reopened->Records().size() == 1);
}

TEST_CASE("阶段 2 验收剧本:ack 前杀宿主,重启重送只留一枚 durable 事件") {
    const auto dir = MakeAccountDir("kill_before_ack");
    // 第一只宿主:收事件、落 durable。ack 帧还没出门就崩溃(析构模拟杀)。
    {
        auto store = OpenStore(dir);
        const auto ingest = store->Ingest(MakeEvent("in-7", "pe-7", "msg-7", "验收正文"));
        REQUIRE(ingest.has_value());
        CHECK(ingest->status == ChannelIngressStore::IngestOutcome::Status::Accepted);
        // 此处不 ack、不 pump——直接掉电。
    }
    // 第二只宿主:同一 state dir 重开。
    auto host2 = OpenStore(dir);
    CHECK(host2->Records().size() == 1);
    // 假 sidecar 重送同一 delivery(ack 一直没到)。
    const auto replay = host2->Ingest(MakeEvent("in-7", "pe-7", "msg-7", "验收正文"));
    REQUIRE(replay.has_value());
    CHECK(replay->status == ChannelIngressStore::IngestOutcome::Status::Duplicate);
    CHECK(replay->ack);
    // durable 事件仍只一枚。
    CHECK(host2->Records().size() == 1);
    CHECK(host2->next_sid() == 2);  // 没有为重投新开账
    // journal 文件里 evt 行只一行。
    const auto journal = dir / "ingress" / "journal.jsonl";
    std::ifstream stream(journal);
    std::string line;
    int evt_lines = 0;
    while (std::getline(stream, line)) {
        if (line.find("\"t\":\"evt\"") != std::string::npos) ++evt_lines;
    }
    CHECK(evt_lines == 1);
}

TEST_CASE("journal 半行与坏行容错:replay 跳过,不崩,账继续") {
    const auto dir = MakeAccountDir("torn_lines");
    {
        auto store = OpenStore(dir);
        REQUIRE(store->Ingest(MakeEvent("in-1", "pe-1")).has_value());
        REQUIRE(store->Ingest(MakeEvent("in-2", "pe-2")).has_value());
    }
    // 尾上追加一行半写(没换行的烂 JSON)。
    {
        std::ofstream stream(dir / "ingress" / "journal.jsonl", std::ios::app);
        stream << "{\"schema\":1,\"t\":\"evt\",\"sid\":3,\"broken";
    }
    ChannelIngressStore::OpenResult result;
    auto store = ChannelIngressStore::Open(dir, "qqbot", "main", &result);
    REQUIRE(store != nullptr);
    CHECK_FALSE(store->write_blocked());
    CHECK(result.skipped_lines == 1);
    CHECK(store->Records().size() == 2);
    // 账还能继续写。
    const auto ingest = store->Ingest(MakeEvent("in-3", "pe-3"));
    REQUIRE(ingest.has_value());
    CHECK(ingest->status == ChannelIngressStore::IngestOutcome::Status::Accepted);
    CHECK(store->Records().size() == 3);
}

TEST_CASE("dead letter:旁路终态 + 独立账档") {
    const auto dir = MakeAccountDir("dead_letter");
    auto store = OpenStore(dir);
    const auto ingest = store->Ingest(MakeEvent("in-1", "pe-1"));
    REQUIRE(ingest.has_value());
    CHECK_FALSE(store->MoveToDeadLetter(ingest->sid, "permanent_reject", 1724700009000).has_value());
    CHECK(store->dead_letter_count() == 1);
    CHECK(store->DeadLetters().size() == 1);
    CHECK(store->DeadLetters()[0].reason == "permanent_reject");
    // 终态事件不能重复进。
    CHECK(store->MoveToDeadLetter(ingest->sid, "again", 1).has_value());
    CHECK(store->dead_letter_count() == 1);
    // dead-letter.jsonl 留档。
    CHECK(std::filesystem::exists(dir / "ingress" / "dead-letter.jsonl"));
}
