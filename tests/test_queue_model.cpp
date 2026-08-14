// 待发消息队列的纯逻辑核(0.25.x 排队输入自然化)。键位规则照规格
// (todo"排队输入要改得自然"一节)逐条钉:空输入按上键取回最后一条、
// 上下键在待发消息间走、Enter 原位替换、Delete 删当前项、Esc 放回队列;
// 待发区摆法:一条不写汇总头、三条以内逐条摆、超上限只添一行"另有 N 条"。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/queue_model.hpp"

using lubancode::cli::BuildPendingQueueRows;
using lubancode::cli::PendingQueueCore;
using Event = PendingQueueCore::Event;

namespace {

// 把 UTF-8 解成码点逐个喂(真键盘路径上 TypeChar 收的就是码点,不是字节)。
void Type(PendingQueueCore& q, const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
        char32_t cp = c0;
        std::size_t extra = 0;
        if (c0 >= 0xF0) {
            cp = c0 & 0x07;
            extra = 3;
        } else if (c0 >= 0xE0) {
            cp = c0 & 0x0F;
            extra = 2;
        } else if (c0 >= 0xC0) {
            cp = c0 & 0x1F;
            extra = 1;
        }
        for (std::size_t k = 0; k < extra && i + 1 + k < utf8.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + 1 + k]) & 0x3F);
        }
        q.TypeChar(cp);
        i += extra + 1;
    }
}

}  // namespace

TEST_CASE("落队:敲字、Enter 追加,顺序保持;空 Enter 不落") {
    PendingQueueCore q;
    Type(q, "first");
    CHECK(q.Submit() == Event::Submitted);
    Type(q, "second");
    CHECK(q.Submit() == Event::Submitted);
    REQUIRE(q.size() == 2);
    CHECK(q.items()[0] == "first");
    CHECK(q.items()[1] == "second");
    CHECK(q.echo_text().empty());

    CHECK(q.Submit() == Event::None);  // 空 buffer 按 Enter:什么都不发生
    CHECK(q.size() == 2);
}

TEST_CASE("退格:退掉一个字符,空了再退不动") {
    PendingQueueCore q;
    Type(q, "abc");
    CHECK(q.Backspace() == Event::Edited);
    CHECK(q.echo_text() == "ab");
    q.Backspace();
    q.Backspace();
    CHECK(q.echo_text().empty());
    CHECK(q.Backspace() == Event::None);
}

TEST_CASE("取回:空输入按上键装进最后一条,待发区不再重复摆它") {
    PendingQueueCore q;
    Type(q, "one");
    q.Submit();
    Type(q, "two");
    q.Submit();

    CHECK(q.MoveUp() == Event::Edited);
    REQUIRE(q.editing());
    CHECK(q.echo_text() == "two");                       // 取回的是最新落队那条
    CHECK(q.display_items().size() == 1);                // 待发区只剩 one
    CHECK(q.display_items()[0] == "one");

    // 有字时按上键不取回(不误触)。
    q.CancelEdit();
    Type(q, "x");
    CHECK(q.MoveUp() == Event::None);
    CHECK_FALSE(q.editing());
}

TEST_CASE("改写:取回后改好按 Enter,原位替换") {
    PendingQueueCore q;
    Type(q, "aaa");
    q.Submit();
    Type(q, "bbb");
    q.Submit();
    Type(q, "ccc");
    q.Submit();

    q.MoveUp();                    // 取回 ccc(下标 2)
    q.Backspace();
    q.Backspace();
    q.Backspace();
    Type(q, "CCC");
    CHECK(q.Submit() == Event::Submitted);
    CHECK_FALSE(q.editing());
    REQUIRE(q.size() == 3);
    CHECK(q.items()[2] == "CCC");
    CHECK(q.items()[0] == "aaa");  // 其余两条原样,位置不动
    CHECK(q.items()[1] == "bbb");
}

TEST_CASE("浏览:上下键在待发消息间走,走到头钳住;改到一半的字不丢") {
    PendingQueueCore q;
    Type(q, "m1");
    q.Submit();
    Type(q, "m2");
    q.Submit();
    Type(q, "m3");
    q.Submit();

    q.MoveUp();          // -> m3(下标 2)
    CHECK(q.echo_text() == "m3");
    q.MoveUp();          // -> m2
    CHECK(q.echo_text() == "m2");
    q.MoveUp();          // -> m1(下标 0)
    CHECK(q.echo_text() == "m1");
    q.MoveUp();          // 到头钳住
    CHECK(q.echo_text() == "m1");
    CHECK(q.editing());

    q.Backspace();
    q.Backspace();       // 退成 ""
    Type(q, "M1");
    q.MoveDown();        // 挪走前先把 buffer 写回当前位
    CHECK(q.items()[0] == "M1");
    CHECK(q.echo_text() == "m2");
    q.MoveDown();
    CHECK(q.echo_text() == "m3");
    CHECK(q.MoveDown() == Event::Edited);  // 过了最后一条:退出编辑态
    CHECK_FALSE(q.editing());
    CHECK(q.echo_text().empty());
    CHECK(q.display_items().size() == 3);  // 三条都在队列里
}

TEST_CASE("Esc 放回队列:未提交的修改丢弃,原文还原") {
    PendingQueueCore q;
    Type(q, "keep");
    q.Submit();
    q.MoveUp();
    q.Backspace();
    q.Backspace();
    q.Backspace();
    q.Backspace();
    Type(q, "changed");
    CHECK(q.CancelEdit() == Event::Restored);
    CHECK_FALSE(q.editing());
    REQUIRE(q.size() == 1);
    CHECK(q.items()[0] == "keep");  // 原文放回
    CHECK(q.echo_text().empty());
    CHECK(q.CancelEdit() == Event::None);  // 非编辑态不管
}

TEST_CASE("Delete:删掉当前浏览的那条") {
    PendingQueueCore q;
    Type(q, "a");
    q.Submit();
    Type(q, "b");
    q.Submit();
    q.MoveUp();                    // 取回 b
    CHECK(q.DeleteCurrent() == Event::Deleted);
    CHECK_FALSE(q.editing());
    REQUIRE(q.size() == 1);
    CHECK(q.items()[0] == "a");
    CHECK(q.DeleteCurrent() == Event::None);  // 非编辑态不管
}

TEST_CASE("TakeAll:取走全部并清空,再取是空") {
    PendingQueueCore q;
    Type(q, "x");
    q.Submit();
    Type(q, "y");
    q.Submit();
    const std::vector<std::string> taken = q.TakeAll();
    CHECK(taken.size() == 2);
    CHECK(taken[0] == "x");
    CHECK(q.empty());
    CHECK(q.TakeAll().empty());
}

TEST_CASE("中文取回改写:码点级退格不切半个字") {
    PendingQueueCore q;
    Type(q, "\xe4\xbd\xa0\xe5\xa5\xbd");  // "你好"(按字节喂,码点完整)
    q.Submit();
    q.MoveUp();
    CHECK(q.echo_text() == "\xe4\xbd\xa0\xe5\xa5\xbd");
    q.Backspace();  // 退掉 "好"(一个码点,不是三个字节)
    CHECK(q.echo_text() == "\xe4\xbd\xa0");
}

TEST_CASE("BuildPendingQueueRows:一条不写汇总头,三条以内逐条摆") {
    CHECK(BuildPendingQueueRows({}, 3).empty());

    const auto one = BuildPendingQueueRows({"hello"}, 3);
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "  \xE2\x80\xBA hello");
    CHECK(one[0].find("1") == std::string::npos);  // 不写"待发消息 1 条"

    const auto three = BuildPendingQueueRows({"a", "b", "c"}, 3);
    REQUIRE(three.size() == 3);
    CHECK(three[0] == "  \xE2\x80\xBA a");
    CHECK(three[2] == "  \xE2\x80\xBA c");
}

TEST_CASE("BuildPendingQueueRows:超上限只添一行'另有 N 条',摆最近的") {
    const auto rows = BuildPendingQueueRows({"1", "2", "3", "4", "5"}, 3);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].find("2") != std::string::npos);       // "另有 2 条"
    CHECK(rows[1] == "  \xE2\x80\xBA 3");                // 最近的 3 条
    CHECK(rows[2] == "  \xE2\x80\xBA 4");
    CHECK(rows[3] == "  \xE2\x80\xBA 5");

    // 上限 1:只摆最新一条。
    const auto single = BuildPendingQueueRows({"a", "b"}, 1);
    REQUIRE(single.size() == 2);
    CHECK(single[1] == "  \xE2\x80\xBA b");
}
