// ChannelRouter 单测(多渠道消息接入单阶段 3):准入链、session key、
// binding 匹配与冲突诊断、工具策略、memory 默认隔离。
//
// 真源:docs/architecture/channels/configuration.md §7-8;TODO §14/§16。
// 纯函数件,不起 IO、不碰线程;pairing 用内存假账。

#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "channel/channel_config.hpp"
#include "channel/channel_router.hpp"
#include "channel/types.hpp"

using namespace lubancode::channel;

namespace {

class FakePairing : public PairingAdmission {
public:
    std::map<std::string, bool> approved;
    std::vector<std::string> requested;

    bool IsSenderApproved(const std::string& sender_id) const override {
        const auto hit = approved.find(sender_id);
        return hit != approved.end() && hit->second;
    }
    std::optional<std::string> RequestCode(const std::string& sender_id,
                                           std::int64_t /*now_ms*/) override {
        requested.push_back(sender_id);
        return "CODE1234";
    }
};

ChannelInboundEvent MakeEvent(ConversationKind kind, const std::string& conversation_id,
                              const std::string& sender_id, bool mentions_bot = false) {
    ChannelInboundEvent event;
    event.delivery_id = "d-1";
    event.provider_event_id = "pe-1";
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.conversation.kind = kind;
    event.conversation.id = conversation_id;
    event.sender.id = sender_id;
    event.sender.display_name = sender_id;
    event.message_id = "m-1";
    event.hints.mentions_bot = mentions_bot;
    ChannelPart part;
    part.type = ChannelPartType::Text;
    part.text = "hello";
    event.parts.push_back(part);
    return event;
}

ChannelAccountUserConfig MakeAccount() {
    ChannelAccountUserConfig account;
    account.enabled = true;
    account.dm_policy = DmPolicy::Allowlist;
    account.allow_from = {"owner"};
    account.group_policy = GroupPolicy::Allowlist;
    account.group_allow_from = {"group-1", "g-1"};
    account.require_mention = true;
    return account;
}

RouteDecision Route(const ChannelInboundEvent& event, const ChannelAccountUserConfig& account,
                    const std::vector<ChannelBindingConfig>* bindings = nullptr,
                    PairingAdmission* pairing = nullptr) {
    RouteInput input;
    input.event = &event;
    input.account = &account;
    input.bindings = bindings;
    input.pairing = pairing;
    input.now_ms = 1000;
    return RouteChannelEvent(input);
}

}  // namespace

TEST_CASE("准入链:bot 默认拒绝,allow_bots 明开才收") {
    ChannelAccountUserConfig account = MakeAccount();
    ChannelInboundEvent event = MakeEvent(ConversationKind::Direct, "dm-1", "other-bot");
    event.sender.is_bot = true;

    auto decision = Route(event, account);
    CHECK(decision.status == RouteDecision::Status::Rejected);
    CHECK(decision.reason == "bot_rejected");

    // 明开 allow_bots 后,bot 的 DM 走 dm_policy 正常判(这里用 Open,
    // 让 bot 不必进 allowlist)。
    account.allow_bots = true;
    account.dm_policy = DmPolicy::Open;
    decision = Route(event, account);
    CHECK(decision.status == RouteDecision::Status::Admitted);
}

TEST_CASE("准入链:DM 四档 policy") {
    ChannelAccountUserConfig account = MakeAccount();
    const ChannelInboundEvent from_owner = MakeEvent(ConversationKind::Direct, "dm-1", "owner");
    const ChannelInboundEvent from_stranger = MakeEvent(ConversationKind::Direct, "dm-1", "stranger");

    SUBCASE("disabled 全拒") {
        account.dm_policy = DmPolicy::Disabled;
        CHECK(Route(from_owner, account).status == RouteDecision::Status::Rejected);
        CHECK(Route(from_stranger, account).reason == "dm_disabled");
    }
    SUBCASE("allowlist 只收名单") {
        CHECK(Route(from_owner, account).status == RouteDecision::Status::Admitted);
        CHECK(Route(from_stranger, account).reason == "dm_not_in_allowlist");
    }
    SUBCASE("open 全收") {
        account.dm_policy = DmPolicy::Open;
        CHECK(Route(from_owner, account).status == RouteDecision::Status::Admitted);
        CHECK(Route(from_stranger, account).status == RouteDecision::Status::Admitted);
    }
    SUBCASE("pairing:已批准放行,未知申请 code,没 pairing 账 fail closed") {
        account.dm_policy = DmPolicy::Pairing;
        FakePairing pairing;
        pairing.approved["owner"] = true;
        auto approved = Route(from_owner, account, nullptr, &pairing);
        CHECK(approved.status == RouteDecision::Status::Admitted);

        auto stranger = Route(from_stranger, account, nullptr, &pairing);
        CHECK(stranger.status == RouteDecision::Status::PendingPairing);
        CHECK(stranger.reason == "pairing_pending");
        CHECK(stranger.pairing_code == "CODE1234");
        CHECK(pairing.requested.size() == 1);

        // 没接 pairing 账:不悄悄放行。
        auto orphan = Route(from_stranger, account);
        CHECK(orphan.status == RouteDecision::Status::Rejected);
        CHECK(orphan.reason == "pairing_unavailable");
    }
}

TEST_CASE("准入链:群聊 allowlist 与 require_mention") {
    ChannelAccountUserConfig account = MakeAccount();
    const ChannelInboundEvent in_group = MakeEvent(ConversationKind::Group, "group-1", "alice");
    const ChannelInboundEvent other_group = MakeEvent(ConversationKind::Group, "group-2", "alice");

    SUBCASE("名单外群拒") {
        CHECK(Route(other_group, account).reason == "group_not_in_allowlist");
    }
    SUBCASE("名单内但没 @ 不触发") {
        CHECK(Route(in_group, account).reason == "mention_required");
    }
    SUBCASE("@ 命中或回复消息放行") {
        ChannelInboundEvent mentioned = in_group;
        mentioned.hints.mentions_bot = true;
        CHECK(Route(mentioned, account).status == RouteDecision::Status::Admitted);

        ChannelInboundEvent reply = in_group;
        reply.hints.is_reply = true;
        CHECK(Route(reply, account).status == RouteDecision::Status::Admitted);
    }
    SUBCASE("disabled 全拒") {
        account.group_policy = GroupPolicy::Disabled;
        CHECK(Route(in_group, account).reason == "group_disabled");
    }
    SUBCASE("open 群仍要 @") {
        account.group_policy = GroupPolicy::Open;
        CHECK(Route(in_group, account).reason == "mention_required");
        account.require_mention = false;
        CHECK(Route(in_group, account).status == RouteDecision::Status::Admitted);
    }
}

TEST_CASE("session key:DM/群聊/group scope 四档") {
    const ChannelInboundEvent dm = MakeEvent(ConversationKind::Direct, "dm-1", "owner");
    CHECK(MakeChannelSessionKey("qqbot", "main", dm.conversation, dm.sender.id,
                                GroupSessionScope::Group) ==
          "channel:qqbot:main:direct:dm-1");

    const ChannelInboundEvent group = MakeEvent(ConversationKind::Group, "g-1", "alice");
    CHECK(MakeChannelSessionKey("qqbot", "main", group.conversation, "alice",
                                GroupSessionScope::Group) == "channel:qqbot:main:group:g-1");
    CHECK(MakeChannelSessionKey("qqbot", "main", group.conversation, "alice",
                                GroupSessionScope::GroupSender) ==
          "channel:qqbot:main:group:g-1:sender:alice");

    ChannelInboundEvent threaded = group;
    threaded.conversation.thread_id = "t-9";
    CHECK(MakeChannelSessionKey("qqbot", "main", threaded.conversation, "alice",
                                GroupSessionScope::GroupThread) ==
          "channel:qqbot:main:group:g-1:thread:t-9");
    CHECK(MakeChannelSessionKey("qqbot", "main", threaded.conversation, "alice",
                                GroupSessionScope::GroupThreadSender) ==
          "channel:qqbot:main:group:g-1:thread:t-9:sender:alice");

    // 路由真跑:group_scope=group_sender 时两位 sender 各进各场。
    ChannelAccountUserConfig account = MakeAccount();
    account.group_scope = "group_sender";
    const auto alice = Route(MakeEvent(ConversationKind::Group, "g-1", "alice", true), account);
    const auto bob = Route(MakeEvent(ConversationKind::Group, "g-1", "bob", true), account);
    REQUIRE(alice.status == RouteDecision::Status::Admitted);
    REQUIRE(bob.status == RouteDecision::Status::Admitted);
    CHECK(alice.session_key != bob.session_key);
    CHECK(alice.session_key == "channel:qqbot:main:group:g-1:sender:alice");
}

TEST_CASE("binding:具体压过宽,agent 与工具策略随档") {
    ChannelAccountUserConfig account = MakeAccount();
    account.agent = "account-agent";

    std::vector<ChannelBindingConfig> bindings;
    ChannelBindingConfig channel_level;
    channel_level.agent = "channel-agent";
    {
        ChannelBindingConversationMatch match_conversation;
        match_conversation.kind = "direct";
        match_conversation.id = "dm-1";
        channel_level.match.conversation = match_conversation;
        channel_level.policy.tools.deny = {"shell"};
    }
    bindings.push_back(channel_level);

    // conversation 档命中:agent 与工具策略都取它。
    const auto decision = Route(MakeEvent(ConversationKind::Direct, "dm-1", "owner"), account,
                                &bindings);
    REQUIRE(decision.status == RouteDecision::Status::Admitted);
    CHECK(decision.agent == "channel-agent");
    CHECK(decision.agent_source == "binding_conversation");
    CHECK(decision.tools.deny.size() == 1);
    CHECK(decision.tools.Allows("read_file"));
    CHECK_FALSE(decision.tools.Allows("shell"));

    // 别的 DM:conversation 档不中,回落账号默认。
    const auto other = Route(MakeEvent(ConversationKind::Direct, "dm-2", "owner"), account,
                             &bindings);
    CHECK(other.agent == "account-agent");
    CHECK(other.agent_source == "account_default");
    CHECK(other.tools.allow.empty());
}

TEST_CASE("binding:allow 名单收窄工具面") {
    ChannelAccountUserConfig account = MakeAccount();
    std::vector<ChannelBindingConfig> bindings;
    ChannelBindingConfig binding;
    binding.match.account = "main";
    binding.policy.tools.allow = {"read_file", "search"};
    binding.policy.tools.deny = {"search"};
    bindings.push_back(binding);

    const auto decision = Route(MakeEvent(ConversationKind::Direct, "dm-1", "owner"), account,
                                &bindings);
    REQUIRE(decision.status == RouteDecision::Status::Admitted);
    CHECK(decision.tools.Allows("read_file"));
    CHECK_FALSE(decision.tools.Allows("search"));   // deny 压过 allow
    CHECK_FALSE(decision.tools.Allows("write_file"));  // 不在 allow 名单
}

TEST_CASE("binding:同档命中两条报冲突,不按配置次序碰运气") {
    ChannelAccountUserConfig account = MakeAccount();
    std::vector<ChannelBindingConfig> bindings;
    ChannelBindingConfig first;
    first.agent = "a-one";
    {
        ChannelBindingConversationMatch conversation;
        conversation.kind = "direct";
        conversation.id = "dm-1";
        first.match.conversation = conversation;
    }
    ChannelBindingConfig second = first;
    second.agent = "a-two";
    bindings.push_back(first);
    bindings.push_back(second);

    const auto decision = Route(MakeEvent(ConversationKind::Direct, "dm-1", "owner"), account,
                                &bindings);
    CHECK(decision.status == RouteDecision::Status::Rejected);
    CHECK(decision.reason == "binding_conflict");

    // 宽窄两档并存不算冲突:窄的赢。
    ChannelBindingConfig wide;
    wide.match.account = "main";
    wide.agent = "wide-agent";
    bindings.pop_back();
    bindings.push_back(wide);
    const auto resolved = Route(MakeEvent(ConversationKind::Direct, "dm-1", "owner"), account,
                                &bindings);
    REQUIRE(resolved.status == RouteDecision::Status::Admitted);
    CHECK(resolved.agent == "a-one");
}

TEST_CASE("binding:thread 档最具体") {
    ChannelAccountUserConfig account = MakeAccount();
    std::vector<ChannelBindingConfig> bindings;
    ChannelBindingConfig conversation_binding;
    conversation_binding.agent = "conv-agent";
    {
        ChannelBindingConversationMatch conversation;
        conversation.kind = "group";
        conversation.id = "g-1";
        conversation_binding.match.conversation = conversation;
    }
    ChannelBindingConfig thread_binding = conversation_binding;
    thread_binding.agent = "thread-agent";
    thread_binding.match.thread_id = "t-9";
    bindings.push_back(conversation_binding);
    bindings.push_back(thread_binding);

    ChannelInboundEvent event = MakeEvent(ConversationKind::Group, "g-1", "alice", true);
    event.conversation.thread_id = "t-9";
    const auto decision = Route(event, account, &bindings);
    REQUIRE(decision.status == RouteDecision::Status::Admitted);
    CHECK(decision.agent == "thread-agent");

    // thread 不匹配的 binding 不算命中(thread_id 限定没对上)。
    ChannelInboundEvent other_thread = event;
    other_thread.conversation.thread_id = "t-8";
    CHECK(Route(other_thread, account, &bindings).agent == "conv-agent");
}

TEST_CASE("memory:owner/非 owner/group 三档默认隔离") {
    ChannelAccountUserConfig account = MakeAccount();
    std::vector<ChannelBindingConfig> bindings;
    ChannelBindingConfig binding;
    binding.match.account = "main";
    binding.policy.memory = ChannelBindingMemoryPolicy{.user = true, .project = true};
    bindings.push_back(binding);

    SUBCASE("owner DM 按 binding 明开") {
        const auto decision =
            Route(MakeEvent(ConversationKind::Direct, "dm-1", "owner"), account, &bindings);
        REQUIRE(decision.status == RouteDecision::Status::Admitted);
        CHECK(decision.memory.user_memory);
        CHECK(decision.memory.project_memory);
        CHECK(decision.memory.source == "owner_binding");
    }
    SUBCASE("非 owner DM:binding 想开也压回关") {
        const auto decision =
            Route(MakeEvent(ConversationKind::Direct, "dm-1", "stranger"), account, &bindings);
        REQUIRE(account.dm_policy == DmPolicy::Allowlist);
        CHECK(decision.status == RouteDecision::Status::Rejected);  // 名单外,先挡在准入
        account.dm_policy = DmPolicy::Open;
        const auto open_dm = Route(MakeEvent(ConversationKind::Direct, "dm-1", "stranger"),
                                   account, &bindings);
        REQUIRE(open_dm.status == RouteDecision::Status::Admitted);
        CHECK_FALSE(open_dm.memory.user_memory);
        CHECK_FALSE(open_dm.memory.project_memory);
        CHECK(open_dm.memory.source == "default_closed");
    }
    SUBCASE("group:全关,binding 明开也不放") {
        ChannelBindingConfig group_binding = binding;
        {
            ChannelBindingConversationMatch conversation;
            conversation.kind = "group";
            conversation.id = "g-1";
            group_binding.match.conversation = conversation;
        }
        bindings.push_back(group_binding);
        const auto decision =
            Route(MakeEvent(ConversationKind::Group, "g-1", "alice", true), account, &bindings);
        REQUIRE(decision.status == RouteDecision::Status::Admitted);
        CHECK_FALSE(decision.memory.user_memory);
        CHECK_FALSE(decision.memory.project_memory);
    }
    SUBCASE("无 binding 的 owner DM 也默认关") {
        const auto decision =
            Route(MakeEvent(ConversationKind::Direct, "dm-1", "owner"), account, nullptr);
        REQUIRE(decision.status == RouteDecision::Status::Admitted);
        CHECK_FALSE(decision.memory.user_memory);
        CHECK(decision.memory.source == "default_closed");
    }
}

TEST_CASE("provenance:宿主真账随决策带出,不信 sidecar 的 is_owner") {
    ChannelInboundEvent event = MakeEvent(ConversationKind::Direct, "dm-1", "owner");
    event.sender.is_owner = true;  // sidecar 声称——不算数
    const auto decision = Route(event, MakeAccount());
    REQUIRE(decision.status == RouteDecision::Status::Admitted);
    CHECK(decision.provenance.origin == MessageOrigin::ExternalChannel);
    CHECK(decision.provenance.channel_id == "qqbot");
    CHECK(decision.provenance.account_id == "main");
    CHECK(decision.provenance.sender_id == "owner");
    CHECK(decision.provenance.conversation_id == "dm-1");
    CHECK(decision.provenance.provider_message_id == "m-1");
    // memory 判定看宿主 allowlist(owner 在名单):is_owner 的声称不影响准入,
    // 但也没给它换来源档。
    CHECK(decision.memory.source == "default_closed");
}

TEST_CASE("空 conversation id 拒:没法定 session") {
    const auto decision = Route(MakeEvent(ConversationKind::Direct, "", "owner"), MakeAccount());
    CHECK(decision.status == RouteDecision::Status::Rejected);
    CHECK(decision.reason == "bad_conversation");
}
