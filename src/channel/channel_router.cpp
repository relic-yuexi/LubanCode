// ChannelRouter 实现(多渠道消息接入单阶段 3)。合同见 channel_router.hpp
// 文件头;准入次序照 configuration.md §7,一关不过就 Rejected,不建 session。
#include "channel/channel_router.hpp"

#include <algorithm>

namespace lubancode::channel {

namespace {

bool Contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

GroupSessionScope ParseGroupScope(const std::string& scope) {
    if (scope == "group_sender") return GroupSessionScope::GroupSender;
    if (scope == "group_thread") return GroupSessionScope::GroupThread;
    if (scope == "group_thread_sender") return GroupSessionScope::GroupThreadSender;
    return GroupSessionScope::Group;  // 默认/未知(配置层已拦)都按全群一场
}

const char* ConversationKindKey(ConversationKind kind) {
    return ConversationKindName(kind);
}

// binding.match 与事件逐字段比对:非空字段全中才算命中(空 = 通配)。
bool BindingMatches(const ChannelBindingConfig& binding, const ChannelInboundEvent& event) {
    const ChannelBindingMatch& match = binding.match;
    if (!match.channel.empty() && match.channel != event.channel_id) {
        return false;
    }
    if (!match.account.empty() && match.account != event.account_id) {
        return false;
    }
    if (match.conversation.has_value()) {
        if (!match.conversation->kind.empty()) {
            const auto kind = ConversationKindFromName(match.conversation->kind);
            if (!kind.has_value() || *kind != event.conversation.kind) {
                return false;
            }
        }
        if (!match.conversation->id.empty() && match.conversation->id != event.conversation.id) {
            return false;
        }
    }
    if (!match.thread_id.empty()) {
        if (!event.conversation.thread_id.has_value() ||
            *event.conversation.thread_id != match.thread_id) {
            return false;
        }
    }
    return true;
}

RouteDecision Rejected(const std::string& reason) {
    RouteDecision decision;
    decision.status = RouteDecision::Status::Rejected;
    decision.reason = reason;
    return decision;
}

}  // namespace

int BindingSpecificity(const ChannelBindingMatch& match) {
    if (match.conversation.has_value() && !match.thread_id.empty()) {
        return 4;  // conversation + thread
    }
    if (match.conversation.has_value()) {
        return 3;  // conversation
    }
    if (!match.account.empty()) {
        return 2;  // account
    }
    if (!match.channel.empty()) {
        return 1;  // channel
    }
    return 0;  // 空 match:不参与(配置层要求 binding 必有 match)
}

std::string MakeChannelSessionKey(const std::string& channel_id, const std::string& account_id,
                                  const ChannelConversation& conversation,
                                  const std::string& sender_id, GroupSessionScope scope) {
    std::string key = "channel:" + channel_id + ":" + account_id + ":" +
                      ConversationKindKey(conversation.kind) + ":" + conversation.id;
    const bool thread_scope = scope == GroupSessionScope::GroupThread ||
                              scope == GroupSessionScope::GroupThreadSender;
    if (conversation.thread_id.has_value() &&
        (thread_scope || conversation.kind == ConversationKind::Thread)) {
        key += ":thread:" + *conversation.thread_id;
    }
    if ((scope == GroupSessionScope::GroupSender ||
         scope == GroupSessionScope::GroupThreadSender) &&
        !sender_id.empty()) {
        key += ":sender:" + sender_id;
    }
    return key;
}

bool ToolRoutePolicy::Allows(const std::string& tool_name) const {
    if (Contains(deny, tool_name)) {
        return false;
    }
    if (!allow.empty() && !Contains(allow, tool_name)) {
        return false;
    }
    return true;
}

RouteDecision RouteChannelEvent(const RouteInput& input) {
    const ChannelInboundEvent& event = *input.event;
    const ChannelAccountUserConfig& account = *input.account;

    RouteDecision decision;
    decision.provenance.origin = MessageOrigin::ExternalChannel;
    decision.provenance.channel_id = event.channel_id;
    decision.provenance.account_id = event.account_id;
    decision.provenance.sender_id = event.sender.id;
    decision.provenance.conversation_id = event.conversation.id;
    decision.provenance.provider_message_id = event.message_id;

    // ---- 1) bot/self-loop 检查(§7:其他 bot 默认拒绝) ---------------------
    if (event.sender.is_bot && !account.allow_bots) {
        return Rejected("bot_rejected");
    }

    // ---- 2) DM/group policy + allowlist(§7) ------------------------------
    bool direct = event.conversation.kind == ConversationKind::Direct;
    if (direct) {
        switch (account.dm_policy) {
            case DmPolicy::Disabled:
                return Rejected("dm_disabled");
            case DmPolicy::Open:
                break;
            case DmPolicy::Allowlist:
                if (!Contains(account.allow_from, event.sender.id)) {
                    return Rejected("dm_not_in_allowlist");
                }
                break;
            case DmPolicy::Pairing: {
                if (input.pairing == nullptr) {
                    // 宿主没接 pairing 账:fail closed,不悄悄放行。
                    return Rejected("pairing_unavailable");
                }
                if (input.pairing->IsSenderApproved(event.sender.id)) {
                    break;
                }
                decision.status = RouteDecision::Status::PendingPairing;
                decision.reason = "pairing_pending";
                decision.pairing_code =
                    input.pairing->RequestCode(event.sender.id, input.now_ms)
                        .value_or(std::string());
                return decision;
            }
        }
    } else {
        switch (account.group_policy) {
            case GroupPolicy::Disabled:
                return Rejected("group_disabled");
            case GroupPolicy::Open:
                break;
            case GroupPolicy::Allowlist:
                if (!Contains(account.group_allow_from, event.conversation.id)) {
                    return Rejected("group_not_in_allowlist");
                }
                break;
        }
        // mention/reply trigger(群聊默认要 @)。
        if (account.require_mention && !event.hints.mentions_bot && !event.hints.is_reply) {
            return Rejected("mention_required");
        }
    }

    // ---- 3) 会话形状兜底:没有 conversation id 没法定位 session -----------
    if (event.conversation.id.empty()) {
        return Rejected("bad_conversation");
    }

    // ---- 4) route binding(§8:具体到宽,同档两条报冲突) -------------------
    const ChannelBindingConfig* best = nullptr;
    int best_specificity = 0;
    bool conflict = false;
    if (input.bindings != nullptr) {
        for (const ChannelBindingConfig& binding : *input.bindings) {
            if (!BindingMatches(binding, event)) {
                continue;
            }
            const int specificity = BindingSpecificity(binding.match);
            if (specificity == 0) {
                continue;  // 空 match 不参与(配置层已拦,双保险)
            }
            if (specificity > best_specificity) {
                best = &binding;
                best_specificity = specificity;
                conflict = false;
            } else if (specificity == best_specificity && best != nullptr) {
                conflict = true;  // 同档命中两条(§8:不按文件次序碰运气)
            }
        }
    }
    if (conflict) {
        return Rejected("binding_conflict");
    }

    // ---- 5) Agent 解析(binding 压过账号默认) ------------------------------
    if (best != nullptr && !best->agent.empty()) {
        decision.agent = best->agent;
        decision.agent_source = best_specificity >= 3
                                    ? "binding_conversation"
                                    : (best_specificity == 2 ? "binding_account" : "binding_channel");
    } else {
        decision.agent = account.agent;
        decision.agent_source = account.agent.empty() ? "default" : "account_default";
    }

    // ---- 6) 工具策略(§16.2:每层只收窄,deny 永远赢) ----------------------
    if (best != nullptr) {
        decision.tools.allow = best->policy.tools.allow;
        decision.tools.deny = best->policy.tools.deny;
        decision.tools.source = "binding";
    }

    // ---- 7) memory 默认(§8:安全边界,不是偏好) ---------------------------
    // owner 判定:DM 且 sender 在账号 allowlist 里(allow_from 是宿主真账;
    // pairing 批准的 sender 不自动升 owner——要开记忆须把 sender 加进
    // allow_from 并在 binding 明写,两道手都过才放)。
    const bool owner_dm = direct && Contains(account.allow_from, event.sender.id);
    bool user_memory = false;
    bool project_memory = false;
    if (owner_dm && best != nullptr && best->policy.memory.has_value()) {
        user_memory = best->policy.memory->user.value_or(false);
        project_memory = best->policy.memory->project.value_or(false);
        decision.memory.source = user_memory || project_memory ? "owner_binding" : "owner_binding_off";
    } else {
        decision.memory.source = "default_closed";
    }
    decision.memory.user_memory = user_memory;
    decision.memory.project_memory = project_memory;

    // ---- 8) session resolve(§8) -------------------------------------------
    decision.session_key =
        MakeChannelSessionKey(event.channel_id, event.account_id, event.conversation, event.sender.id,
                              direct ? GroupSessionScope::Group
                                     : ParseGroupScope(account.group_scope));

    decision.status = RouteDecision::Status::Admitted;
    decision.reason.clear();
    return decision;
}

}  // namespace lubancode::channel
