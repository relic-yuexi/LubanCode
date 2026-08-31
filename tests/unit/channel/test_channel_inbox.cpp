// 多渠道消息接入单阶段 2:ChannelInbox 册——每账号/每会话水位、sender
// 速率窗、同正文短窗、conversation 间轮转 FIFO。规矩:满不默丢,给决策。
#include <doctest/doctest.h>

#include "channel/inbox.hpp"

using namespace lubancode::channel;

namespace {

InboxLimits TightLimits() {
    InboxLimits limits;
    limits.max_pending_total = 3;
    limits.max_pending_per_conversation = 2;
    limits.sender_rate_window_ms = 60'000;
    limits.sender_rate_max = 2;
    limits.same_content_window_ms = 5'000;
    return limits;
}

}  // namespace

TEST_CASE("总量帽:满了拒,给 queue_full,不默丢") {
    ChannelInbox inbox(TightLimits());
    const auto a = inbox.Enqueue(1, "c1", "s1", "d1", 1000);
    const auto b = inbox.Enqueue(2, "c2", "s1", "d2", 1000);
    CHECK(a.status == ChannelInbox::EnqueueResult::Status::Accepted);
    CHECK(b.status == ChannelInbox::EnqueueResult::Status::Accepted);
    // 第三个:same_content 窗不拦(digest 不同),但 sender 速率窗先拦?
    // s1 已发 2 条 = rate_max。换 sender 绕开速率,顶总量。
    const auto c = inbox.Enqueue(3, "c3", "s2", "d3", 1000);
    CHECK(c.status == ChannelInbox::EnqueueResult::Status::Accepted);
    CHECK(inbox.pending_total() == 3);
    const auto d = inbox.Enqueue(4, "c1", "s3", "d4", 1000);
    CHECK(d.status == ChannelInbox::EnqueueResult::Status::QueueFull);
    CHECK(d.reason == "queue_full");
    CHECK(inbox.pending_total() == 3);  // 拒了的没进队,也没悄悄挤掉谁
}

TEST_CASE("每会话帽先于总量帽,细原因可区分") {
    ChannelInbox inbox(TightLimits());
    CHECK(inbox.Enqueue(1, "c1", "s1", "d1", 1000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
    CHECK(inbox.Enqueue(2, "c1", "s2", "d2", 1000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
    // c1 到帽(2),总量还没到(3)。
    const auto third = inbox.Enqueue(3, "c1", "s3", "d3", 1000);
    CHECK(third.status == ChannelInbox::EnqueueResult::Status::QueueFull);
    CHECK(third.reason == "queue_full_conversation");
    CHECK(inbox.pending_for("c1") == 2);
}

TEST_CASE("sender 速率窗:窗口滑走后放行") {
    ChannelInbox inbox(TightLimits());
    CHECK(inbox.Enqueue(1, "c1", "s1", "d1", 1000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
    CHECK(inbox.Enqueue(2, "c2", "s1", "d2", 2000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
    // s1 在 60s 窗内已发 2 条。
    const auto limited = inbox.Enqueue(3, "c3", "s1", "d3", 3000);
    CHECK(limited.status == ChannelInbox::EnqueueResult::Status::SenderRateLimited);
    CHECK(limited.reason == "sender_rate_limited");
    // 窗口滑走:61 秒后再来。
    const auto later = inbox.Enqueue(4, "c3", "s1", "d4", 3000 + 61'000);
    CHECK(later.status == ChannelInbox::EnqueueResult::Status::Accepted);
}

TEST_CASE("同正文短窗:同 sender 同会话同 digest 拒,异会话放行") {
    ChannelInbox inbox(TightLimits());
    CHECK(inbox.Enqueue(1, "c1", "s1", "same", 1000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
    const auto dup = inbox.Enqueue(2, "c1", "s1", "same", 2000);
    CHECK(dup.status == ChannelInbox::EnqueueResult::Status::DuplicateContent);
    CHECK(dup.reason == "duplicate_content");
    // 换会话不算重发风暴的另一份。
    CHECK(inbox.Enqueue(3, "c2", "s1", "same", 2000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
    // 换 sender 也不是。
    CHECK(inbox.Enqueue(4, "c1", "s2", "same", 2000).status ==
          ChannelInbox::EnqueueResult::Status::Accepted);
}

TEST_CASE("会话内 FIFO,会话间轮转不饿") {
    InboxLimits limits;
    limits.max_pending_total = 100;
    limits.max_pending_per_conversation = 100;
    limits.sender_rate_max = 0;          // 关速率窗
    limits.same_content_window_ms = 0;   // 关短窗
    ChannelInbox inbox(limits);
    for (int i = 0; i < 3; ++i) {
        // c1 先灌三条,c2 后灌三条。
        REQUIRE(inbox.Enqueue(10 + i, "c1", "s1", std::string("c1-") + std::to_string(i), 1000 + i)
                    .status == ChannelInbox::EnqueueResult::Status::Accepted);
        REQUIRE(inbox.Enqueue(20 + i, "c2", "s1", std::string("c2-") + std::to_string(i), 1000 + i)
                    .status == ChannelInbox::EnqueueResult::Status::Accepted);
    }
    // 轮转:c1 -> c2 -> c1 -> c2 ...
    const auto a = inbox.TakeNext();
    const auto b = inbox.TakeNext();
    const auto c = inbox.TakeNext();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    CHECK(a->conversation_id == "c1");
    CHECK(a->sid == 10);
    CHECK(b->conversation_id == "c2");
    CHECK(b->sid == 20);
    CHECK(c->conversation_id == "c1");
    CHECK(c->sid == 11);
    // 指定会话直取:FIFO。
    const auto direct = inbox.TakeNextFor("c2");
    REQUIRE(direct.has_value());
    CHECK(direct->sid == 21);
    // 取空后无活。
    while (inbox.TakeNext().has_value()) {
    }
    CHECK(inbox.pending_total() == 0);
    CHECK_FALSE(inbox.TakeNext().has_value());
}
