// 两层会话标题的"账本"那一半(骨架拆解反弹·问题 2 自 TerminalSessionController
// 拆出)。原先四个标题方法(BeginSessionTitle/StartTitleRefinement/
// BackfillTitleOnResume/PollSessionTitleRefinement)里的"判断"部分收进这只
// 小类,单测不必再起一个完整 TerminalSessionController:
//   - 一场只自动起名一次(人工 /title pending、已有标题都不算);
//   - 本地标题(第一层)落盘成功才占内存标题——落不了盘就回退,不占;
//   - 代数(generation)管理:人工 /title、/clear、/resume 都翻号,在飞的
//     精炼结果落地对代,对不上就弃(人工优先);
//   - 精炼结果(第二层)采纳判定:代数对上、档子活着、落盘成功才换标题。
//
// 不在这的:第二层精炼的"发车"(要模型路由,见 controller 的
// StartTitleRefinement)、终端打印与 peer 名册同步(controller)、精炼线程
// 本体(SessionTitleRefiner,挂在本类里由 controller 驱动)。
#pragma once

#include <cstdint>
#include <string>

#include "app/session_title_refiner.hpp"
#include "sessions/session_store.hpp"

namespace lubancode::app {

class SessionTitleAccount {
public:
    // 第一层本地起名的结果:调用方据此决定打印与是否发第二层精炼。
    enum class LocalResult {
        NoNeed,       // 三道门拦下(试过/人工 pending/已有标题/档子没活)
        NoUsableText,  // 首问没剩可看的字(全空白那类):标题留空
        Set,          // 本地标题已落定(title 事件行也落了盘)
        WriteFailed,  // 标题事件行落不了盘:内存标题已回退,不留虚账
    };
    // 第二层精炼结果的采纳判定。
    enum class AdoptResult {
        Ignored,      // 失败/空标题/迟到(代数对不上)/场子没了:本地标题保住
        Adopted,      // 已换新标题(事件行落了盘),该同步 peer 名册
        WriteFailed,  // 标题事件行落不了盘:内存标题已回退
    };

    // title/pending 是 runtime::SessionRuntime 那份的引用(resume、/title、
    // /clear 都直接写它,这里不夺所有权);store 同为借用。store_broken 是
    // "建档失败过,别每轮都再撞一次"的那本账。
    SessionTitleAccount(std::string& title, bool& pending, lubancode::sessions::SessionStore& store,
                        bool& store_broken);

    // 第一层:首问建档当场起本地临时标题(零模型 token),/sessions 立刻
    // 有名字。三道门:本场已试过、人工标题待建档(pending)、已有标题。
    LocalResult BeginLocalTitle(const std::string& first_query);

    // resume 换场善后:翻代数、标记本场不走"首问自动起名"路;老档没标题
    // 就拿档里首条用户正文补一枚本地标题(零模型 token,失败安静退——不
    // 同于首问路,老档补名失败不打扰人)。
    LocalResult BackfillOnResume(const std::string& first_user_text);

    // 第二层收货的采纳判定(精炼是否完工由调用方先问 refiner())。判定
    // 次序与原先逐字对齐:失败/空标题 -> 迟到 -> 场子没了 -> 换标题(落盘
    // 失败回退)。
    AdoptResult AdoptRefined(const SessionTitleRefiner::Outcome& outcome);

    // 代数:人工 /title、/clear、resume 换场翻号;起飞精炼时记下,落地对代。
    std::uint64_t generation() const { return generation_; }
    void BumpGeneration() { ++generation_; }
    // /clear 开新场:翻代、取消在飞精炼、下一问重走本地起名。
    void ResetForNewSession();
    // 精炼器(自带后台线程;Start/TakeFinished/RequestCancel 都归它)。
    SessionTitleRefiner& refiner() { return refiner_; }
    bool auto_attempted() const { return auto_attempted_; }

private:
    // 起本地标题的共用尾段:title 占内存 -> 落事件行 -> 失败回退。
    LocalResult AdoptLocalTitle(const std::string& local, bool quiet_on_failure);

    std::string& title_;
    bool& pending_;
    lubancode::sessions::SessionStore& store_;
    bool& store_broken_;
    bool auto_attempted_ = false;  // 一场只试一次,失败安静降级
    std::uint64_t generation_ = 0;
    SessionTitleRefiner refiner_;
};

}  // namespace lubancode::app
