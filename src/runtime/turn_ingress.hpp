// TurnIngress(多渠道消息接入单阶段 3):回合入口的统一合同。
//
// 单子 §15.2:把"RunSessionTurn(string, TurnSource)"往下一层收成结构体
// ——终端、peer、channel、后台完成各造自己的 TurnIngress,共同入口只管
// 建/恢复 session、取有效 Agent Profile、取工具与权限快照、开 turn、落
// 消息与事件、回终态。
//
//   - source:User(终端用户正文)/Incoming(peer 来信、后台回流)/
//    Channel(渠道消息,阶段 3 起)。原先这枚枚举嵌在
//    TerminalSessionController 里,渠道要造自己的 ingress,枚举随合同
//    上移到这里;控制器侧改成 using 别名,两值语义不变。
//   - provenance:宿主真账(channel/types.hpp §2),由宿主生成、模型不可
//    改、落 session JSONL;终端路径不填(回落 HumanTerminal)。
//   - allow_memory_retrieval:渠道 memory 隔离(configuration.md §8)的
//    执行位——false 时本轮不召回项目/用户记忆(host 不调 BuildTurnContext
//    的检索路,只留跳过账)。
//
// 纯合同:不 include agent/app/cli;channel 与 api 都是中立层。
#pragma once

#include <string>

#include "api/types.hpp"
#include "channel/types.hpp"

namespace lubancode::runtime {

// 回合来源(原 TerminalSessionController::TurnSource 上移,加 Channel)。
enum class TurnSource {
    User,      // 终端用户敲的正文
    Incoming,  // peer 来信/后台完成唤醒(宿主合成语义)
    Channel,   // 渠道消息(外部真人经 IM 发来)
};

// 一轮回合的入账。默认值 = 终端老路的形状。
struct TurnIngress {
    api::Message message;
    channel::MessageProvenance provenance;  // 默认 origin=HostSynthetic,终端路径按 HumanTerminal 落
    TurnSource source = TurnSource::User;
    std::string session_key;            // channel 路由的 session key;终端路空
    std::string reply_route;            // 回发路由(渠道 reply_route_id);终端路空
    std::string ingress_delivery_id;    // 渠道入站 delivery id;终端路空
    bool allow_memory_retrieval = true;  // 渠道 memory 隔离执行位
};

// 渠道事件折成 TurnIngress 的正文投影(§12.1:text 进 TextBlock,媒体按
// 投影规矩给稳定说明;本批只钉 text 与 unsupported 的说明,图片/转录归
// 阶段 4 的 ReplyAssembler/媒体路)。空文本(纯媒体消息)给一行占位说明,
// 不发空消息。
TurnIngress MakeChannelTurnIngress(const channel::ChannelInboundEvent& event,
                                   const channel::MessageProvenance& provenance,
                                   const std::string& session_key, bool allow_memory_retrieval);

}  // namespace lubancode::runtime
