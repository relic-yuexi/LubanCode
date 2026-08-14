// "ask_user 被子代理状态遮挡"的 repaint 协调层测试:
//   1. StreamFooterSuspendScope 升级成的全局挂起计数——嵌套、逐层退场、
//      异常早退析构,账目都要对得平;
//   2. 挂起钩子(SetRepaintSuspendHideHook)只在最外层进入时响一次;
//   3. 可控 ticker(AgentStatusPainter::Tick 手工泵一拍)——挂起期间零状态
//      输出;板上全是完成态且内容没变时,静默跳过整拍重画;
//   4. 输入所有权——ConsoleReadMutex 的互斥规约(菜单持读权时监听侧
//      try_lock 拿不到)+ RepaintSuspendActive 门(挂起期间监听让出)。
//
// 真终端(Windows Terminal/VS Code/conhost)上的菜单不被状态块插入、
// 恢复点落在菜单结果之后,属刮屏手测,见交活清单;这里钉的是纯状态机。

#include <doctest/doctest.h>

#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/agent_status.hpp"
#include "cli/console_input.hpp"
#include "cli/live_transcript.hpp"
#include "cli/theme.hpp"

using lubancode::cli::AgentStatusBoard;
using lubancode::cli::AgentStatusPainter;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::ConsoleReadMutex;
using lubancode::cli::RepaintSuspendActive;
using lubancode::cli::StreamFooterPaintScope;
using lubancode::cli::StreamFooterSuspendScope;
using lubancode::cli::StreamFooterSuspendDepthForTest;

namespace {

// 把 std::cout 引到内存里,数 Tick 落了多少字节。Tick 内部自拿
// StdoutWriteMutex,跟作用域对象的构造/析构天然错开,不会自锁。
class CoutCapture {
public:
    CoutCapture() : old_(std::cout.rdbuf(buffer_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }

    std::size_t size() const { return buffer_.str().size(); }
    std::string text() const { return buffer_.str(); }

    CoutCapture(const CoutCapture&) = delete;
    CoutCapture& operator=(const CoutCapture&) = delete;

private:
    std::ostringstream buffer_;
    std::streambuf* old_;
};

}  // namespace

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

// ---- 挂起钩子:最外层进入响一次 ---------------------------------------------

TEST_CASE("挂起钩子:最外层进入响一次,嵌套不再响,退干净后下一场再响") {
    int calls = 0;
    lubancode::cli::SetRepaintSuspendHideHook([&calls] { ++calls; });
    {
        StreamFooterSuspendScope outer;
        CHECK(calls == 1);
        {
            StreamFooterSuspendScope inner;
            CHECK(calls == 1);  // 嵌套进入不重复收块
        }
        CHECK(calls == 1);
    }
    {
        StreamFooterSuspendScope next_menu;
        CHECK(calls == 2);  // 下一场菜单开屏再收一次
    }
    CHECK(calls == 2);
    lubancode::cli::SetRepaintSuspendHideHook({});  // 清理,别漏给后面的用例
}

// ---- 可控 ticker:挂起期间零输出 / 完成态静默 -------------------------------

TEST_CASE("可控 ticker:板上没条目时一拍零输出") {
    AgentStatusBoard board;
    const auto theme = BuiltinTheme("dark");
    AgentStatusPainter painter(board, theme, /*enabled=*/false);  // 不起线程,手工泵
    CoutCapture capture;
    painter.Tick();
    CHECK(capture.size() == 0);
}

TEST_CASE("可控 ticker:运行中条目每拍都画,画完问题恰逢下一拍也有输出") {
    AgentStatusBoard board;
    const int id = board.Start("跑一个前台子代理");
    const auto theme = BuiltinTheme("dark");
    AgentStatusPainter painter(board, theme, /*enabled=*/false);
    CoutCapture capture;
    painter.Tick();
    CHECK(capture.text().find("跑一个前台子代理") != std::string::npos);
    painter.Tick();  // Running 态耗时在跳,内容每拍都变,不静默
    CHECK(capture.size() > 0);
    (void)id;
}

TEST_CASE("可控 ticker:挂起期间零状态输出——菜单占屏,ticker 一拍不落笔") {
    AgentStatusBoard board;
    board.Start("仍在跑的后台子代理");
    const auto theme = BuiltinTheme("dark");
    AgentStatusPainter painter(board, theme, /*enabled=*/false);
    // 菜单开屏前 ticker 正常画得出块(有输出),证明不是 painter 哑了。
    {
        CoutCapture before;
        painter.Tick();
        REQUIRE(before.size() > 0);
    }
    {
        StreamFooterSuspendScope menu;  // 交互菜单取得屏面所有权
        for (int beat = 0; beat < 3; ++beat) {
            CoutCapture during;
            painter.Tick();  // "画完问题恰逢下一拍"——恰逢三拍
            CHECK(during.size() == 0);
        }
    }
    // 菜单退出(作用域收场)后恢复:计数归零,ticker 照常落笔。
    CHECK(StreamFooterSuspendDepthForTest() == 0);
    {
        CoutCapture after;
        painter.Tick();
        CHECK(after.size() > 0);
    }
}

TEST_CASE("可控 ticker:完成态且内容没变时静默,有新条目才再画") {
    AgentStatusBoard board;
    const int id = board.Start("跑完的前台子代理");
    board.RecordToolCall(id);
    board.Finish(id, /*success=*/true);
    const auto theme = BuiltinTheme("dark");
    AgentStatusPainter painter(board, theme, /*enabled=*/false);
    {
        CoutCapture first;
        painter.Tick();
        REQUIRE(first.size() > 0);  // 首拍画出来(此刻块还没在屏上)
    }
    {
        CoutCapture second;
        painter.Tick();  // 完成摘要不会再变——不每 400ms 死缠屏幕
        CHECK(second.size() == 0);
        painter.Tick();
        CHECK(second.size() == 0);
    }
    board.Start("又收尾一条");  // 内容变了(新条目),照常画
    {
        CoutCapture third;
        painter.Tick();
        CHECK(third.text().find("又收尾一条") != std::string::npos);
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
