// "ask_user 被子代理状态遮挡"的 repaint 协调层测试:
//   1. StreamFooterSuspendScope 升级成的全局挂起计数——嵌套、逐层退场、
//      异常早退析构,账目都要对得平;
//   2. 屏幕事务(PaintScope)也算挂起,退出即恢复;
//   3. 输入所有权——ConsoleReadMutex 的互斥规约(菜单持读权时监听侧
//      try_lock 拿不到)+ RepaintSuspendActive 门(挂起期间监听让出)。
//
// 旧 AgentStatusBoard/AgentStatusPainter 已删:子代理状态(前台+后台)全在
// AgentTool 统一台账里,流式 footer 的代理面板一处画,挂起语义由 footer 的
// suspend/paint 计数继续承担(挂起期间 RedrawStreamFooterLocked 空操作、
// turn_runner 的心跳线程每拍先查 RepaintSuspendedLocked)。菜单不被面板
// 插入、恢复点落在菜单结果之后,属刮屏手测,见交活清单;这里钉的是纯状态机。

#include <doctest/doctest.h>

#include <mutex>
#include <stdexcept>

#include "cli/console_input.hpp"
#include "cli/theme.hpp"

using lubancode::cli::ConsoleReadMutex;
using lubancode::cli::RepaintSuspendActive;
using lubancode::cli::StreamFooterPaintScope;
using lubancode::cli::StreamFooterSuspendScope;
using lubancode::cli::StreamFooterSuspendDepthForTest;

// ---- 挂起计数:嵌套 / 逐层退场 / 异常早退 ----------------------------------

TEST_CASE("挂起计数:嵌套逐层加、逐层退,账目对得平") {
    CHECK(StreamFooterSuspendDepthForTest() == 0);
    CHECK_FALSE(RepaintSuspendActive());
    {
        StreamFooterSuspendScope outer;
        CHECK(StreamFooterSuspendDepthForTest() == 1);
        CHECK(RepaintSuspendActive());
        {
            StreamFooterSuspendScope middle;
            CHECK(StreamFooterSuspendDepthForTest() == 2);
            {
                StreamFooterSuspendScope inner;
                CHECK(StreamFooterSuspendDepthForTest() == 3);
            }
            CHECK(StreamFooterSuspendDepthForTest() == 2);
            CHECK(RepaintSuspendActive());  // 内层退了,外层没退干净前仍是挂起
        }
        CHECK(StreamFooterSuspendDepthForTest() == 1);
    }
    CHECK(StreamFooterSuspendDepthForTest() == 0);
    CHECK_FALSE(RepaintSuspendActive());
}

TEST_CASE("挂起计数:作用域内抛异常,析构把账退干净") {
    CHECK(StreamFooterSuspendDepthForTest() == 0);
    bool threw = false;
    try {
        StreamFooterSuspendScope outer;
        StreamFooterSuspendScope inner;
        throw std::runtime_error("菜单画到一半炸了");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(StreamFooterSuspendDepthForTest() == 0);
    CHECK_FALSE(RepaintSuspendActive());
    // 对账之后新作用域照常工作(上一场的异常没有留下半枚挂起)。
    {
        StreamFooterSuspendScope again;
        CHECK(StreamFooterSuspendDepthForTest() == 1);
    }
    CHECK(StreamFooterSuspendDepthForTest() == 0);
}

TEST_CASE("挂起判定:屏幕事务(PaintScope)也算挂起,退出即恢复") {
    CHECK_FALSE(RepaintSuspendActive());  // 事务不占输入,只占屏面
    {
        StreamFooterPaintScope paint(true);
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        CHECK(lubancode::cli::RepaintSuspendedLocked());
    }
    {
        StreamFooterPaintScope inactive(false);
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        CHECK_FALSE(lubancode::cli::RepaintSuspendedLocked());
    }
}

// ---- 输入所有权 ----------------------------------------------------------

TEST_CASE("输入所有权:菜单持读权时监听侧 try_lock 拿不到,退出后恢复") {
    {
        std::unique_lock<std::mutex> menu(ConsoleReadMutex());  // ReadChoiceMenu 全程攥着它
        std::unique_lock<std::mutex> listener(ConsoleReadMutex(), std::try_to_lock);
        CHECK_FALSE(listener.owns_lock());  // 监听线程抢不到,一枚键都消费不了
    }
    {
        std::unique_lock<std::mutex> listener(ConsoleReadMutex(), std::try_to_lock);
        CHECK(listener.owns_lock());  // 菜单退出,读权回到监听侧
    }
}

TEST_CASE("输入所有权:挂起计数>0 期间监听侧按 RepaintSuspendActive 让出") {
    CHECK_FALSE(RepaintSuspendActive());
    {
        StreamFooterSuspendScope menu;
        CHECK(RepaintSuspendActive());  // 监听线程看到真值,连空窗期都不读键
    }
    CHECK_FALSE(RepaintSuspendActive());
}
