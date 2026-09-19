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
