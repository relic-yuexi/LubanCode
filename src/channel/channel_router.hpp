// ChannelRouter:conversation -> session/agent/policy 的路由与准入总口
//(多渠道消息接入单阶段 3)。
//
// 唯一真源 docs/architecture/channels/configuration.md §7-8(准入次序、
// session key、binding 匹配、memory 隔离默认表)与 TODO §14/§16。这里把
// 阶段 2 manager 里的"最小 DM 准入 + 群聊粗判"收成一只全账纯函数件:
//   - 准入链(§7):bot/self-loop -> DM/group policy -> sender allowlist ->
//     group allowlist -> mention/reply -> route binding -> session resolve。
//     不通过的消息不建 session、不召回记忆、不调模型——状态判 Rejected,
//     稳定 reason 交 ingress 账。
//   - session key(§8):channel:<ch>:<acct>:<kind>:<conv>[:thread:<t>];
//     群聊按 group_scope 四档展开 sender/thread 维度。
//   - binding(§8):具体到宽五档,同档命中两条报 binding_conflict,
//     不按配置次序碰运气。
//   - memory 默认(§8):owner DM 可按 binding 明开;非 owner DM 与 group
//     全关——这份默认表是安全边界,不是偏好。
//
// 纯函数件:不碰 IO、不碰线程;pairing 查询经 PairingAdmission 小口注入
//(宿主持 PairingStore,channel 库内不互相持有)。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "channel/channel_config.hpp"
#include "channel/types.hpp"

namespace lubancode::channel {

// pairing 侧的查询口(宿主拿 PairingStore 适配;router 不持有账)。
// 传空指针 = 宿主没接 pairing 账,dm_policy=pairing 的未知 sender 直接拒
//(fail closed,不悄悄放行)。
class PairingAdmission {
public:
    virtual ~PairingAdmission() = default;
    virtual bool IsSenderApproved(const std::string& sender_id) const = 0;
    // 为未知 sender 申请一次性 code;返回明文(只此一次)。宿主限速期内
    // 可回 nullopt(router 视为 PendingPairing 但无新 code,不重发提示)。
    virtual std::optional<std::string> RequestCode(const std::string& sender_id,
                                                   std::int64_t now_ms) = 0;
};

// 群聊 session scope(§8):默认 group。解析失败(配置层已拦)按 group。
enum class GroupSessionScope { Group, GroupSender, GroupThread, GroupThreadSender };

// session key(§8 的钉死拼法)。
std::string MakeChannelSessionKey(const std::string& channel_id, const std::string& account_id,
                                  const ChannelConversation& conversation,
                                  const std::string& sender_id, GroupSessionScope scope);

// ---- 路由决策 -------------------------------------------------------------

struct ToolRoutePolicy {
    // allow 非空 = 只许这些工具(再叠 deny 排除);空 = binding 没设上限。
    std::vector<std::string> allow;
    std::vector<std::string> deny;
    // 来源账:哪些 binding 出的手(空 = 无 binding 参与)。
    std::string source;

    bool Allows(const std::string& tool_name) const;
};

struct MemoryRoutePolicy {
    // §8 首版默认表:非 owner DM 与 group 全关;owner DM 按 binding 明开。
    bool user_memory = false;
    bool project_memory = false;
    std::string source;  // "default_closed" / "owner_binding" / "forced_closed"
};

struct RouteDecision {
    enum class Status {
        Admitted,
        PendingPairing,  // pairing 待批:回配对提示,不进 Agent
        Rejected,
    };
    Status status = Status::Rejected;
    // 稳定 reason:bot_rejected/dm_disabled/dm_not_in_allowlist/pairing_unavailable/
    // pairing_pending/group_disabled/group_not_in_allowlist/mention_required/
    // bad_conversation/binding_conflict。Admitted 时空。
    std::string reason;
    std::string pairing_code;  // PendingPairing 且宿主发了新 code 时非空

    // ---- Admitted 时的路由产物 ----
    std::string session_key;
    std::string agent;  // 空 = default Agent
    // agent 来源:binding_conversation/binding_account/binding_channel/account_default
    std::string agent_source;
    ToolRoutePolicy tools;
    MemoryRoutePolicy memory;
    // 宿主真账(types.hpp §2):由宿主生成,模型不可改。
    MessageProvenance provenance;
};

struct RouteInput {
    const ChannelInboundEvent* event = nullptr;
    // 事件所属账号的配置(manager 的 AccountEntry.config)。
    const ChannelAccountUserConfig* account = nullptr;
    // 渠道层 bindings(§8)。
    const std::vector<ChannelBindingConfig>* bindings = nullptr;
    // pairing 查询口;空 = 无 pairing 账(pairing 策略下未知 sender 拒)。
    PairingAdmission* pairing = nullptr;
    std::int64_t now_ms = 0;
};

// 跑一遍准入链 + 路由。纯函数:同样的输入永远同样的决策。
RouteDecision RouteChannelEvent(const RouteInput& input);

// binding 匹配的档位(§8 五档,数值越大越具体;0 = default Agent 档)。
// 仅供诊断与测试;RouteChannelEvent 内部用同一张表。
int BindingSpecificity(const ChannelBindingMatch& match);

}  // namespace lubancode::channel
