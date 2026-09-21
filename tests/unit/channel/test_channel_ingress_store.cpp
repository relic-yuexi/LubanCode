// 多渠道消息接入单阶段 2:ChannelIngressStore 册——append-only journal、
// 三级去重、replay 容错、dead letter。含阶段 2 验收剧本:
// "ack 前杀宿主,重启后重送,只留一枚 durable 事件"。
// 唯一真源 docs/architecture/channels/message-contracts.md §3-4。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

// QQBot 静默失败单 P1:最近来信链的只读投影——不手翻 JSONL 就能看见
// 每枚来信的最终状态、死信原因与时间。
// ---- SV-05:三套重放规则的合同测试 ------------------------------------------
// 同一本账(合法行 + 各形坏行)喂三条读取路径(恢复器 Open、状态页投影
// ReadChannelIngressProjection、最近链 ReadChannelIngressRecentChain),事件
// 数量与终态必须一致。此前三处各写一套 JSON/schema/state 分支:缺 schema
// 的 tr 在状态页凭空建项、未知 schema 的 tr 改写合法 sid 的终态、缺 event
// 的 evt 计入事件数——恢复器却统统跳过,同一本账三处三个结论。先钉病灶,
// 收敛(行合同 + sid 折叠核)后转绿。
TEST_CASE("SV-05 三路重放对账:坏账行在三处同命运") {
    const auto dir = MakeAccountDir("sv05_three_way");
    const auto journal = dir / "ingress" / "journal.jsonl";
    {
        auto store = OpenStore(dir);
        const auto first = store->Ingest(MakeEvent("in-1", "pe-1", "m-1"));
        REQUIRE(first.has_value());
        REQUIRE_FALSE(store->Transition(first->sid, IngressEventState::Authorized, "pass").has_value());
        REQUIRE_FALSE(store->Transition(first->sid, IngressEventState::Routed, "route_ok").has_value());
    }
    // 手工追加各形坏账行:恢复器、状态页、最近链对每一行的命运必须一致。
    {
        std::ofstream stream(journal, std::ios::app);
        stream << "{\"t\":\"tr\",\"sid\":99,\"to\":\"running\"}\n";  // 缺 schema:状态页曾凭空建 running
        stream << "{\"schema\":2,\"t\":\"tr\",\"sid\":1,\"to\":\"delivered\"}\n";  // 未知 schema:曾改写合法 sid 终态
        stream << "{\"schema\":1,\"t\":\"evt\",\"sid\":6,\"dedupe\":\"k\",\"tier\":1}\n";  // 缺 event:曾计入事件数
        stream << "{\"schema\":1,\"t\":\"tr\",\"sid\":77,\"to\":\"rejected\",\"reason\":\"orphan\"}\n";  // 孤儿 tr:静默跳过
        stream << "{\"schema\":1,\"t\":\"dup\",\"sid\":1,\"reason\":\"delivery_replayed\"}\n";  // 旁注:不重建状态
        stream << "{\"schema\":1,\"t\":\"weird\",\"sid\":1}\n";  // 未知 t:坏行
        stream << "{\"schema\":1,\"t\":\"evt\",\"sid\":7,\"broken";  // 半写行:坏行
    }
    // 恢复器:只有一枚合法事件,终态 routed。
    ChannelIngressStore::OpenResult result;
    {
        auto store = ChannelIngressStore::Open(dir, "qqbot", "main", &result);
        REQUIRE(store != nullptr);
        REQUIRE_FALSE(store->write_blocked());
        const auto records = store->Records();
        REQUIRE(records.size() == 1);
        CHECK(records[0].sid == 1);
        CHECK(records[0].state == IngressEventState::Routed);
        CHECK(records[0].last_transition_reason == "route_ok");
    }
    // 状态页投影:同一份事件数与终态,没有凭空建出的项。
    const auto projection = ReadChannelIngressProjection(dir);
    CHECK(projection.events == 1);
    REQUIRE(projection.state_counts.size() == 1);
    REQUIRE(projection.state_counts.count("routed") == 1);
    CHECK(projection.state_counts.at("routed") == 1);
    // 最近链:同一份事件数与终态。
    const auto chain = ReadChannelIngressRecentChain(dir, 64);
    REQUIRE(chain.entries.size() == 1);
    CHECK(chain.entries[0].sid == 1);
    CHECK(chain.entries[0].state == "routed");
    CHECK(chain.entries[0].reason == "route_ok");
    // 恢复器的坏行计数:缺 schema tr、未知 schema tr、缺 event evt、未知 t、
    // 半写共 5 行;孤儿 tr 与 dup 不计。
    CHECK(result.skipped_lines == 5);
}

TEST_CASE("SV-05 重复 sid evt:三路首笔为准,重落行计坏行") {
    const auto dir = MakeAccountDir("sv05_dup_sid");
    const auto journal = dir / "ingress" / "journal.jsonl";
    {
        auto store = OpenStore(dir);
        REQUIRE(store->Ingest(MakeEvent("in-1", "pe-1", "m-1")).has_value());
        REQUIRE(store->Ingest(MakeEvent("in-2", "pe-2", "m-2")).has_value());
    }
    // 手工拼坏账:把第二枚 evt 行的 sid 改成 1(同 sid 重落)。
    {
        std::ifstream in(journal);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto pos = text.find("\"sid\":2");
        REQUIRE(pos != std::string::npos);
        text.replace(pos, 7, "\"sid\":1");
        std::ofstream out(journal, std::ios::trunc);
        out << text;
    }
    ChannelIngressStore::OpenResult result;
    {
        auto store = ChannelIngressStore::Open(dir, "qqbot", "main", &result);
        REQUIRE(store != nullptr);
        const auto records = store->Records();
        REQUIRE(records.size() == 1);  // 首笔为准(收敛前恢复器收两条)
        CHECK(records[0].sid == 1);
        CHECK(records[0].event.delivery_id == "in-1");
        CHECK(store->next_sid() == 2);  // 重落行不抬账序
    }
    const auto projection = ReadChannelIngressProjection(dir);
    CHECK(projection.events == 1);  // 收敛前状态页计 2
    const auto chain = ReadChannelIngressRecentChain(dir, 8);
    REQUIRE(chain.entries.size() == 1);
    CHECK(chain.entries[0].sid == 1);
    CHECK(result.skipped_lines == 1);  // 重落行是账序破裂的坏行
}

// 状态页(gateway/status.cpp 渠道栏)的 ingress_pending = queued + running、
// dead_letter 计数,都出自 ReadChannelIngressProjection 这个真实投影入口;
// 坏账行不得把状态页计数顶起来。
TEST_CASE("SV-05 状态页真实投影入口:pending/死信计数,坏行不膨胀") {
    const auto dir = MakeAccountDir("sv05_status_projection");
    const auto journal = dir / "ingress" / "journal.jsonl";
    {
        auto store = OpenStore(dir);
        const auto first = store->Ingest(MakeEvent("in-1", "pe-1", "m-1"));
        REQUIRE(first.has_value());
        for (const auto step : {IngressEventState::Authorized, IngressEventState::Routed,
                                IngressEventState::Queued}) {
            REQUIRE_FALSE(store->Transition(first->sid, step, "").has_value());
        }
        const auto second = store->Ingest(MakeEvent("in-2", "pe-2", "m-2"));
        REQUIRE(second.has_value());
        for (const auto step : {IngressEventState::Authorized, IngressEventState::Routed,
                                IngressEventState::Queued, IngressEventState::Running}) {
            REQUIRE_FALSE(store->Transition(second->sid, step, "").has_value());
        }
        const auto third = store->Ingest(MakeEvent("in-3", "pe-3", "m-3"));
        REQUIRE(third.has_value());
        REQUIRE_FALSE(store->MoveToDeadLetter(third->sid, "turn_failed", 1724700090000).has_value());
    }
    // 与 gateway/status.cpp 同一口径:pending = queued + running。
    const auto count_pending = [](const ChannelIngressProjection& ingress) {
        std::size_t pending = 0;
        if (const auto found = ingress.state_counts.find("queued");
            found != ingress.state_counts.end()) {
            pending += found->second;
        }
        if (const auto found = ingress.state_counts.find("running");
            found != ingress.state_counts.end()) {
            pending += found->second;
        }
        return pending;
    };
    const auto before = ReadChannelIngressProjection(dir);
    CHECK(before.events == 3);
    CHECK(count_pending(before) == 2);
    CHECK(before.dead_letter == 1);
    // 尾上一笔缺 schema 的 running tr:正是审查记录里的病灶样本——状态页
    // 曾对同一本账多报一枚 running。收敛后它与恢复器同命运(跳过)。
    {
        std::ofstream stream(journal, std::ios::app);
        stream << "{\"t\":\"tr\",\"sid\":99,\"to\":\"running\"}\n";
    }
    const auto after = ReadChannelIngressProjection(dir);
    CHECK(after.events == 3);
    CHECK(count_pending(after) == 2);
    CHECK(after.state_counts.size() == 3);  // queued/running/dead_letter,无凭空第四项
}

TEST_CASE("recent chain:重放最终状态/原因/死信时间,sid 降序,limit 裁剪") {
    const auto dir = MakeAccountDir("recent_chain");
    {
        auto store = OpenStore(dir);
        // sid=1:送达;sid=2、3:死信(现场病形状:turn_failed);sid=4:受理拒。
        const auto first = store->Ingest(MakeEvent("in-1", "pe-1", "m-1"));
        REQUIRE(first.has_value());
        for (const auto step : {IngressEventState::Authorized, IngressEventState::Routed,
                                IngressEventState::Queued, IngressEventState::Running,
                                IngressEventState::Replied, IngressEventState::Delivered}) {
            REQUIRE_FALSE(store->Transition(first->sid, step, "chain_walk").has_value());
        }
        const auto second = store->Ingest(MakeEvent("in-2", "pe-2", "m-2"));
        REQUIRE(second.has_value());
        REQUIRE_FALSE(store->MoveToDeadLetter(
                          second->sid, "turn_failed: gateway.turn_failed: context.unestimated",
                          1724700060000).has_value());
        const auto third = store->Ingest(MakeEvent("in-3", "pe-3", "m-3"));
        REQUIRE(third.has_value());
        REQUIRE_FALSE(store->MoveToDeadLetter(
                          third->sid, "turn_failed: gateway.turn_failed: context.unestimated",
                          1724700120000).has_value());
        const auto fourth = store->Ingest(MakeEvent("in-4", "pe-4", "m-4"));
        REQUIRE(fourth.has_value());
        REQUIRE_FALSE(store->Transition(fourth->sid, IngressEventState::Rejected, "dm_closed").has_value());
    }
    // 只读投影(store 已销毁,零写柄)。
    const auto chain = ReadChannelIngressRecentChain(dir, 8);
    CHECK(chain.ledger_present);
    CHECK(chain.dead_letter_count == 2);
    REQUIRE(chain.entries.size() == 4);
    // sid 降序:4(rejected)→3(dead_letter)→2(dead_letter)→1(delivered)。
    CHECK(chain.entries[0].sid == 4);
    CHECK(chain.entries[0].state == "rejected");
    CHECK(chain.entries[0].reason == "dm_closed");
    CHECK(chain.entries[1].sid == 3);
    CHECK(chain.entries[1].state == "dead_letter");
    CHECK(chain.entries[1].reason.find("turn_failed") != std::string::npos);
    CHECK(chain.entries[1].dead_letter_at_ms == 1724700120000);
    CHECK(chain.entries[2].sid == 2);
    CHECK(chain.entries[2].dead_letter_at_ms == 1724700060000);
    CHECK(chain.entries[3].sid == 1);
    CHECK(chain.entries[3].state == "delivered");
    CHECK(chain.entries[3].received_at_ms == 1724700000000);
    CHECK(chain.entries[3].conversation_id == "dm-1");
    CHECK(chain.entries[3].sender_id == "sender-1");
    // limit 裁剪:只留最近 2 枚。
    const auto limited = ReadChannelIngressRecentChain(dir, 2);
    REQUIRE(limited.entries.size() == 2);
    CHECK(limited.entries[0].sid == 4);
    CHECK(limited.entries[1].sid == 3);
    // 没账的目录:空投影,不建文件。
    const auto empty_dir = MakeAccountDir("recent_chain_empty");
    const auto empty = ReadChannelIngressRecentChain(empty_dir, 8);
    CHECK_FALSE(empty.ledger_present);
    CHECK(empty.entries.empty());
}
