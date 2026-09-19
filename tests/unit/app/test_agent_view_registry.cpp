// 按代理状态投影单 P1:会话级视图登记簿的状态分账回归册。
//
// 钉的桩(单子 §四/§七 P1 的验收拆解):
//   1. 身份与世代:AgentViewKey 映射(main=0 合法成员)、/clear 式换代把
//      旧册作废——任务号重用不串旧页;
//   2. 绘制闸与水位:离屏拒画、回屏放行;重铺成对协议(Take/Mark)后
//      旧修订号不重放、新修订号接续;
//   3. 收账不丢:离屏期间 ApplyToMainTurn 的变异全部落账(视图账/工具
//      配对/usage 在外层测试册盖,这里钉修订号轴);
//   4. 每页 UI 状态册:切走保留、切回还原、换代清册。

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/agent_view_registry.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_collector.hpp"

using namespace lubancode;

// 独立的登记簿实例逐案新建:不碰进程级单例,案与案不串。

TEST_CASE("P1 身份: AgentViewKey——main 是合法成员,世代与任务号各占半") {
    app::AgentViewRegistry registry;
    CHECK(registry.session_generation() >= 1);
    CHECK(registry.MainKey().is_main());
    CHECK(registry.MainKey().task_id == 0);
    CHECK(registry.KeyFor(7).task_id == 7);
    CHECK_FALSE(registry.KeyFor(7).is_main());
    CHECK(registry.MainKey() == cli::AgentViewKeyFor(registry.session_generation(), 0));
    CHECK(registry.MainKey() != registry.KeyFor(7));
    // 不同世代同任务号:不是同一页(换代后旧账对不上新键)。
    cli::AgentViewKey old_key{registry.session_generation(), 3};
    registry.BeginNewSession();
    CHECK(old_key != registry.KeyFor(3));
}

TEST_CASE("P1 闸门: 离屏拒画、回屏放行——重铺成对协议后按水位接续") {
    runtime::TurnCollector collector(runtime::ProcessIdAuthority(), "turn-gate");
    collector.StartTurn("问", 1);
    app::AgentViewRegistry registry;
    registry.BeginMainTurn(&collector, nullptr);

    const std::uint64_t first = registry.ApplyToMainTurn([&] { collector.OnTextDelta("a", false); });
    CHECK(first == 1);
    CHECK(registry.MainShouldDraw(first));  // main 在屏、修订号过水位(0)

    registry.SwitchViewed(5);
    const std::uint64_t second = registry.ApplyToMainTurn([&] { collector.OnTextDelta("b", false); });
    CHECK_FALSE(registry.MainShouldDraw(second));  // 离屏:账照走,画被拒

    // 成对重铺:切回 main(生产里换页钩子先切身份)、关闸取快照(含离屏
    // 期间的账),打印,钉水位。
    registry.SwitchViewed(0);
    const app::AgentViewRegistry::MainTurnSnapshot snapshot = registry.TakeMainLedgeForRepaint();
    REQUIRE(snapshot.live);
    REQUIRE(snapshot.view != nullptr);
    CHECK(snapshot.revision == second);
    CHECK_FALSE(registry.MainShouldDraw(snapshot.revision));  // 重铺进行中(闸关着)
    // 快照里离屏期间的内容都在——不丢账。
    std::string joined;
    for (const auto& item : snapshot.view->items) {
        joined += item.result_text;
    }
    CHECK(joined.find("ab") != std::string::npos);
    registry.MarkMainPrinted(snapshot.revision);
    CHECK(registry.MainShouldDraw(snapshot.revision) == false);  // 恰在水位上:不重放
    const std::uint64_t third = registry.ApplyToMainTurn([&] { collector.OnTextDelta("c", false); });
    CHECK(registry.MainShouldDraw(third));  // 过水位:接续活画

    registry.DetachMainTurn();
}

TEST_CASE("P1 收口: EndMainTurn 后终账在 ledge——切回重铺吃终账,live 翻假") {
    runtime::TurnCollector collector(runtime::ProcessIdAuthority(), "turn-end");
    collector.StartTurn("问", 1);
    app::AgentViewRegistry registry;
    registry.BeginMainTurn(&collector, nullptr);
    registry.SwitchViewed(2);
    registry.ApplyToMainTurn([&] { collector.OnTextDelta("x", false); });
    registry.ApplyToMainTurn([&] { collector.FinishTurn(runtime::TurnItemViewState::Succeeded, 9, 0); });
    registry.EndMainTurn(collector.view());

    const auto snapshot = registry.MainLedgeSnapshot();
    CHECK_FALSE(snapshot.live);
    REQUIRE(snapshot.view != nullptr);
    CHECK(snapshot.view->finished);
    CHECK(snapshot.view->status == runtime::TurnItemViewState::Succeeded);
    registry.DetachMainTurn();
}

TEST_CASE("P1 世代: BeginNewSession——查看页回 main,每页 UI 状态册换代清账") {
    runtime::TurnCollector collector(runtime::ProcessIdAuthority(), "turn-gen");
    app::AgentViewRegistry registry;
    registry.BeginMainTurn(&collector, nullptr);
    registry.SwitchViewed(4);

    cli::AgentUiState state;
    state.draft_text = "给 sub 的半句话";
    state.view_expanded = true;
    registry.ui_states().Save(registry.KeyFor(4), state);
    const std::uint64_t generation_before = registry.session_generation();

    registry.BeginNewSession();

    CHECK(registry.session_generation() == generation_before + 1);
    CHECK(registry.viewed_task_id() == 0);   // 查看页回 main
    CHECK(registry.view_epoch() > 0);
    // 旧世代的册已清:任务号重用拿不到旧草稿。
    CHECK(registry.ui_states().Load(registry.KeyFor(4)).draft_text.empty());
    registry.DetachMainTurn();
}

TEST_CASE("P1 每页 UI 状态: 切走保留、切回还原——滚动/展开/草稿各归各页") {
    app::AgentViewRegistry registry;
    const std::uint64_t generation = registry.session_generation();

    cli::AgentUiState main_state;
    main_state.expand_latest = true;
    main_state.draft_text = "main 的草稿";
    registry.ui_states().Save(cli::AgentViewKeyFor(generation, 0), main_state);

    cli::AgentUiState sub_state;
    sub_state.view_expanded = true;
    sub_state.follow_tail = false;
    sub_state.draft_text = "sub 的草稿";
    registry.ui_states().Save(cli::AgentViewKeyFor(generation, 6), sub_state);

    const cli::AgentUiState got_main = registry.ui_states().Load(cli::AgentViewKeyFor(generation, 0));
    CHECK(got_main.expand_latest);
    CHECK(got_main.draft_text == "main 的草稿");
    CHECK(got_main.view_expanded == false);  // 不串档

    const cli::AgentUiState got_sub = registry.ui_states().Load(cli::AgentViewKeyFor(generation, 6));
    CHECK(got_sub.view_expanded);
    CHECK_FALSE(got_sub.follow_tail);
    CHECK(got_sub.draft_text == "sub 的草稿");

    // 没记过的页给默认值(跟随末尾、全收起、无草稿)。
    const cli::AgentUiState fresh = registry.ui_states().Load(cli::AgentViewKeyFor(generation, 9));
    CHECK(fresh.follow_tail);
    CHECK(fresh.draft_text.empty());
}

TEST_CASE("P1 画笔护栏: WithMainRenderLock——递归嵌套不self锁死,空锁直走") {
    std::recursive_mutex render;
    runtime::TurnCollector collector(runtime::ProcessIdAuthority(), "turn-lock");
    app::AgentViewRegistry registry;
    registry.BeginMainTurn(&collector, &render);

    bool outer_ran = false;
    bool inner_ran = false;
    registry.WithMainRenderLock([&] {
        outer_ran = true;
        registry.WithMainRenderLock([&] { inner_ran = true; });  // 换页事务套事务:递归合法
    });
    CHECK(outer_ran);
    CHECK(inner_ran);

    // 没挂锁(无活回合)直走。
    registry.DetachMainTurn();
    bool ran_without_lock = false;
    registry.WithMainRenderLock([&] { ran_without_lock = true; });
    CHECK(ran_without_lock);
}

TEST_CASE("P1 查看页对账: SyncViewed——登记簿滞留旧页时按面板现值同步") {
    runtime::TurnCollector collector(runtime::ProcessIdAuthority(), "turn-sync");
    app::AgentViewRegistry registry;
    registry.BeginMainTurn(&collector, nullptr);
    registry.SwitchViewed(7);  // 换页钩子切到 sub

    // 空闲路 Esc 复位不经钩子:面板已回 main,登记簿还停在 7——闸门误关。
    CHECK(registry.viewed_task_id() == 7);
    registry.SyncViewed(0);
    CHECK(registry.viewed_task_id() == 0);
    const std::uint64_t revision = registry.ApplyToMainTurn([&] { collector.OnTextDelta("s", false); });
    CHECK(registry.MainShouldDraw(revision));  // 同步后 main 页放行

    // 同值同步是空操作(纪元不跳)。
    const std::uint64_t epoch_before = registry.view_epoch();
    registry.SyncViewed(0);
    CHECK(registry.view_epoch() == epoch_before);
    registry.DetachMainTurn();
}

// ---------------------------------------------------------------------------
// P2:帧令牌(FrameToken = {AgentViewKey, view_epoch, layout_revision})。
// 布局在锁外算好、写屏前在统一提交锁内再验——换页纪元与布局翻版任一
// 动过,旧帧不通过;快速 A→B→A 第一轮 A 的过期绘制同理被拦。
// ---------------------------------------------------------------------------
TEST_CASE("P2 帧令牌: 令牌当前性——换页/换代/布局翻版各令旧帧失配") {
    app::AgentViewRegistry registry;
    const cli::FrameToken main_now = registry.TokenFor(0);
    CHECK(registry.TokenCurrent(main_now));  // 此刻画 main 的帧:放行

    // 换页:A→B,A 的帧失配;B 的帧放行。快速 A→B→A:第一轮 A 的帧
    //(旧 epoch)仍失配——不得通过。
    registry.SwitchViewed(5);
    CHECK_FALSE(registry.TokenCurrent(main_now));
    const cli::FrameToken sub_now = registry.TokenFor(5);
    CHECK(registry.TokenCurrent(sub_now));
    registry.SwitchViewed(0);
    CHECK_FALSE(registry.TokenCurrent(main_now));  // 回 A 了,但旧 epoch 的帧不放行
    CHECK_FALSE(registry.TokenCurrent(sub_now));
    const cli::FrameToken main_again = registry.TokenFor(0);
    CHECK(registry.TokenCurrent(main_again));

    // 布局翻版(resize/Ctrl+L/Ctrl+O):同页同纪元,布局要素变了也失配。
    registry.BumpLayoutRevision();
    CHECK_FALSE(registry.TokenCurrent(main_again));
    CHECK(registry.TokenCurrent(registry.TokenFor(0)));  // 新版放行

    // 换代(/clear):整册作废,旧世代的令牌一律失配。
    const cli::FrameToken before_clear = registry.TokenFor(0);
    registry.BeginNewSession();
    CHECK_FALSE(registry.TokenCurrent(before_clear));
}

TEST_CASE("P2 帧令牌: 令牌身份——别页的令牌在自家页也失配") {
    app::AgentViewRegistry registry;
    const cli::FrameToken sub_token = registry.TokenFor(9);
    // 正看 main(0),画 sub 的帧:页不对,失配。
    CHECK_FALSE(registry.TokenCurrent(sub_token));
    CHECK(registry.TokenCurrent(registry.TokenFor(0)));
}
