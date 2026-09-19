// 审批通道(按代理状态投影单 P2:确认菜单改"提交审批请求→UI 显示→
// 独立响应通道返回决策")的回归册:
//   1) 挂起与裁定:Submit 拿 future,TakeForViewer 取走、presenter 出裁定、
//      Resolve 送回——工具线程等 future 恰好拿到裁定值;
//   2) 页归属:当前查看页不是 owner 时取不走(不显屏),HasPendingOutside
//      为真(通知位标记);切回那页才取得到;
//   3) 打断收口:DenyAllPending 把悬着的请求按拒绝送回,future 不悬死;
//   4) 无服务者:ClearServer 后 Submit 落空(调用方就地问的旧路);
//   5) 不重复问:取出未决的那笔不再被第二次取走。

#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <thread>

#include "cli/approval_channel.hpp"

using namespace lubancode;

TEST_CASE("ApprovalChannel: 提交→取走→裁定→future 拿到决定") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();

    bool presenter_ran = false;
    auto submitted = channel.Submit(/*owner_task_id=*/0, "write_file",
                                    [&presenter_ran]() -> bool {
                                        presenter_ran = true;
                                        return true;
                                    });
    REQUIRE(submitted.has_value());
    CHECK(channel.PendingCount() == 1);

    const auto taken = channel.TakeForViewer(0);
    REQUIRE(taken.has_value());
    CHECK(taken->owner_task_id == 0);
    CHECK(taken->tool_name == "write_file");
    CHECK_FALSE(channel.TakeForViewer(0).has_value());  // 取走后不再有可取的

    const bool allowed = taken->presenter();
    channel.Resolve(taken->id, allowed);
    CHECK(presenter_ran);
    REQUIRE(submitted->valid());
    CHECK(submitted->wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(submitted->get() == true);
    CHECK(channel.PendingCount() == 0);
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel: 页归属——别页的请求取不走,通知位亮") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();

    auto submitted = channel.Submit(0, "edit_file", []() { return false; });
    REQUIRE(submitted.has_value());
    // 正看 sub(5):main 的请求不显屏。
    CHECK_FALSE(channel.TakeForViewer(5).has_value());
    CHECK(channel.HasPendingOutside(5));
    CHECK_FALSE(channel.HasPendingOutside(0));
    // 切回 main:可取。
    const auto taken = channel.TakeForViewer(0);
    REQUIRE(taken.has_value());
    channel.Resolve(taken->id, false);
    CHECK(submitted->get() == false);
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel: 打断收口——DenyAllPending 不悬死") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    auto first = channel.Submit(0, "write_file", []() { return true; });
    auto second = channel.Submit(0, "run_command", []() { return true; });
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    channel.DenyAllPending();
    CHECK(first->get() == false);
    CHECK(second->get() == false);
    CHECK(channel.PendingCount() == 0);
    // 通道照常可用(收口不废通道)。
    auto third = channel.Submit(0, "read_file", []() { return true; });
    REQUIRE(third.has_value());
    const auto taken = channel.TakeForViewer(0);
    REQUIRE(taken.has_value());
    channel.Resolve(taken->id, true);
    CHECK(third->get());
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel: 无服务者——Submit 落空,调用方就地问") {
    cli::ApprovalChannel channel;  // 未 RegisterServer
    CHECK_FALSE(channel.Submit(0, "write_file", []() { return true; }).has_value());
}

TEST_CASE("ApprovalChannel: 取出在答的那笔不被重复取走") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    auto submitted = channel.Submit(0, "write_file", []() { return true; });
    REQUIRE(submitted.has_value());
    const auto taken = channel.TakeForViewer(0);
    REQUIRE(taken.has_value());
    // presenter 还没跑完(在答):同一笔不再被取——没有 presenter 可跑。
    CHECK_FALSE(channel.TakeForViewer(0).has_value());
    channel.Resolve(taken->id, true);
    CHECK(submitted->get());
    channel.ClearServer();
}

// ---- P3(按代理状态投影单):审批的目标绑定收口 -------------------------------

TEST_CASE("ApprovalChannel P3: owner 绑定——子代理页的请求只在那一页开菜单") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    auto sub_approval = channel.Submit(/*owner=*/7, "write_file", []() { return true; });
    auto main_approval = channel.Submit(/*owner=*/0, "run_command", []() { return false; });
    REQUIRE(sub_approval.has_value());
    REQUIRE(main_approval.has_value());

    // 正看 main:子代理 #7 的请求不显屏,只进通知位;main 的照常可取。
    const auto labels = channel.PendingOwnerLabelsOutside(0);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0] == "#7");
    const auto for_main = channel.TakeForViewer(0);
    REQUIRE(for_main.has_value());
    CHECK(for_main->owner_task_id == 0);
    channel.Resolve(for_main->id, false);
    CHECK(main_approval->get() == false);

    // 切到 #7:那笔才开菜单。确认只解这一笔——另一笔早已裁定,互不误答。
    const auto for_sub = channel.TakeForViewer(7);
    REQUIRE(for_sub.has_value());
    CHECK(for_sub->owner_task_id == 7);
    channel.Resolve(for_sub->id, true);
    CHECK(sub_approval->get() == true);
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel P3: 任务退场——DenyPendingForOwner 只拒那页的悬账") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    auto of_seven = channel.Submit(7, "write_file", []() { return true; });
    auto of_eight = channel.Submit(8, "edit_file", []() { return true; });
    REQUIRE(of_seven.has_value());
    REQUIRE(of_eight.has_value());

    // #7 被停止/清除:它悬着的审批按拒收口(旧审批按钮随任务退场失效),
    // #8 的不受牵连。
    channel.DenyPendingForOwner(7);
    CHECK(of_seven->get() == false);
    CHECK(channel.PendingCount() == 1);
    const auto taken = channel.TakeForViewer(8);
    REQUIRE(taken.has_value());
    channel.Resolve(taken->id, true);
    CHECK(of_eight->get() == true);
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel P3: 回合收口——DenyPendingForTurn 按轮号拒悬账") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    auto this_turn = channel.Submit(0, "write_file", []() { return true; }, /*gen=*/3, "turn-甲");
    auto other_turn = channel.Submit(0, "read_file", []() { return true; }, /*gen=*/3, "turn-乙");
    REQUIRE(this_turn.has_value());
    REQUIRE(other_turn.has_value());

    channel.DenyPendingForTurn("turn-甲");
    CHECK(this_turn->get() == false);
    CHECK(channel.PendingCount() == 1);
    const auto taken = channel.TakeForViewer(0);
    REQUIRE(taken.has_value());
    channel.Resolve(taken->id, true);
    CHECK(other_turn->get() == true);
    // 空轮号(未绑定)是 no-op,不误伤。
    channel.DenyPendingForTurn(std::string());
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel P3: 换代收口——DenyStaleGenerations 拒旧世代,未绑定的不掺和") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    auto stale = channel.Submit(0, "write_file", []() { return true; }, /*gen=*/2, "t1");
    auto fresh = channel.Submit(0, "read_file", []() { return true; }, /*gen=*/3, "t2");
    auto unbound = channel.Submit(0, "edit_file", []() { return true; });  // 单测/旧路:不绑定
    REQUIRE(stale.has_value());
    REQUIRE(fresh.has_value());
    REQUIRE(unbound.has_value());

    // /clear、/resume 换代到 3:旧世代(gen=2)的悬账整批拒收;同世代与
    // 未绑定的照常。
    channel.DenyStaleGenerations(3);
    CHECK(stale->get() == false);
    CHECK(channel.PendingCount() == 2);
    CHECK(channel.TakeForViewer(0).has_value());
    CHECK(channel.TakeForViewer(0).has_value());
    CHECK(fresh->get() == true);
    CHECK(unbound->get() == true);
    channel.ClearServer();
}

TEST_CASE("ApprovalChannel P3: 通知位标签——别页悬账的页名去重列出") {
    cli::ApprovalChannel channel;
    channel.RegisterServer();
    REQUIRE(channel.Submit(0, "write_file", []() { return true; }).has_value());
    REQUIRE(channel.Submit(7, "edit_file", []() { return true; }).has_value());
    REQUIRE(channel.Submit(7, "read_file", []() { return true; }).has_value());

    // 正看 #7:main 那笔是"别页",#7 自己的两笔不进通知位。
    auto labels = channel.PendingOwnerLabelsOutside(7);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0] == "main");
    CHECK(channel.HasPendingOutside(7));

    // 正看 main:#7 的两笔同页,标签去重只剩一枚。
    labels = channel.PendingOwnerLabelsOutside(0);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0] == "#7");
    channel.ClearServer();
}
