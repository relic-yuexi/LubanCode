// 底栏一本帧账(0.29.x):BottomChromeFrame 的行序/高度/指纹纯逻辑。空
// 闲 composer 与流式 footer 共认这一份——两条路的行序契约钉死在这儿,
// 谁也不许多拼一套。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/bottom_chrome.hpp"

using namespace lubancode::cli;

namespace {
bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}
}  // namespace

TEST_CASE("帧账:总行数 = 队列+横线+输入+状态+坞+提示") {
    BottomChromeFrame frame;
    CHECK(frame.TotalRows() == 4);  // 空队列 + 横线2 + 输入1 + 状态1 + 空坞 + 空提示
    frame.queue_rows = {"待发送消息 2 条", "> 第一条", "> 第二条"};
    frame.agent_dock_rows = {"↑/↓ 选择", "● main", "○ agent #1"};
    frame.transient_rows = {"  /help  列出命令"};
    frame.composer_rows = 3;  // 多行 composer
    CHECK(frame.TotalRows() == 3 + 3 + 2 + 3 + 1 + 1);
    // 坞首行 = 队列之后、框与状态之下(相对帧顶)。
    CHECK(frame.AgentDockFirstRow() == 3 + 3 + 2 + 1);
}

TEST_CASE("帧账:指纹认内容——行变/选择变必变,重排不变瞎报") {
    BottomChromeFrame a;
    a.queue_rows = {"队列"};
    a.agent_dock_rows = {"● main", "○ agent #1"};
    a.selected_task_id = 3;
    a.revision = BottomChromeRevision(a);
    BottomChromeFrame b = a;
    b.revision = BottomChromeRevision(b);
    CHECK(BottomChromeFingerprint(a) == BottomChromeFingerprint(b));
    CHECK(a.revision == b.revision);

    b.agent_dock_rows[1] = "○ agent #1  完成";  // 状态变了
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    b = a;
    b.selected_task_id = 0;  // 选择变了
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    b = a;
    b.composer_rows = 2;  // 单行变多行
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
}

TEST_CASE("帧账:指纹把各分区隔开,行内容跨区撞车也不误判相等") {
    BottomChromeFrame a;
    a.queue_rows = {"同一行字"};
    BottomChromeFrame b;
    b.agent_dock_rows = {"同一行字"};
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    CHECK(Contains(BottomChromeFingerprint(a), "q:"));
    CHECK(Contains(BottomChromeFingerprint(a), "d:"));
}
