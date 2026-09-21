// M8:LineFramer 是纯函数式的增量分帧器(mcp/transport.hpp),不碰任何 IO——
// 单测只管喂字节、查吐出来的行,覆盖三种关键情形:一条消息被拆成好几段
// 喂进来、好几条消息挤在一次读到的数据里、残行留到下一次 Feed() 才凑齐。

#include <doctest/doctest.h>

#include "mcp/transport.hpp"

using namespace lubancode;

TEST_CASE("LineFramer: 一次 Feed 里一条完整的行") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("hello\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "hello");
}

TEST_CASE("LineFramer: 一次 Feed 里挤了好几条消息") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("line1\nline2\nline3\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "line1");
    CHECK(lines[1] == "line2");
    CHECK(lines[2] == "line3");
}

TEST_CASE("LineFramer: 一条消息被拆成好几段喂进来") {
    mcp::LineFramer framer;
    auto first = framer.Feed("hel");
    CHECK(first.empty());  // 还没凑齐一整行
    auto second = framer.Feed("lo wor");
    CHECK(second.empty());
    auto third = framer.Feed("ld\n");
    REQUIRE(third.size() == 1);
    CHECK(third[0] == "hello world");
}

TEST_CASE("LineFramer: 残行留在缓冲里,下一条完整消息带出上一次的残行") {
    mcp::LineFramer framer;
    auto first = framer.Feed("complete1\npartial-tai");
    REQUIRE(first.size() == 1);
    CHECK(first[0] == "complete1");

    auto second = framer.Feed("l\ncomplete2\n");
    REQUIRE(second.size() == 2);
    CHECK(second[0] == "partial-tail");
    CHECK(second[1] == "complete2");
}

TEST_CASE("LineFramer: \\r\\n 和裸 \\n 都认,统一去掉行尾 \\r") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("with-cr\r\nwithout-cr\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "with-cr");
    CHECK(lines[1] == "without-cr");
}

TEST_CASE("LineFramer: 空行(连续两个换行符)吐出一个空字符串") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("a\n\nb\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "a");
    CHECK(lines[1].empty());
    CHECK(lines[2] == "b");
}

TEST_CASE("LineFramer: 没有换行符,什么都不吐,残留在缓冲里等下一次") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("no-newline-yet");
    CHECK(lines.empty());
    const auto more = framer.Feed("-still-nothing");
    CHECK(more.empty());
    const auto finally = framer.Feed("\n");
    REQUIRE(finally.size() == 1);
    CHECK(finally[0] == "no-newline-yet-still-nothing");
}

TEST_CASE("LineFramer: 单行超过 8MB 上限,置 overflowed、清空缓冲、不再吐行") {
    mcp::LineFramer framer;
    const std::string chunk(1024 * 1024, 'x');
    for (int i = 0; i < 9; ++i) {
        framer.Feed(chunk);  // 一直不给换行
    }
    CHECK(framer.overflowed());

    // 报废之后,正常行也不吐了(传输层看 overflowed() 断连)。
    const auto lines = framer.Feed("normal\n");
    CHECK(lines.empty());
}

TEST_CASE("LineFramer: 超限前已凑齐的完整行照常交出,残行丢弃") {
    mcp::LineFramer framer;
    std::string input = "good-line\n";
    input += std::string(9 * 1024 * 1024, 'y');  // 没有换行的超长残行
    const auto lines = framer.Feed(input);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "good-line");
    CHECK(framer.overflowed());
}

TEST_CASE("LineFramer: 正常行量不触发 overflowed") {
    mcp::LineFramer framer;
    for (int i = 0; i < 1000; ++i) {
        framer.Feed("line-" + std::to_string(i) + "\n");
    }
    CHECK_FALSE(framer.overflowed());
}

// ---- SV-04 资源合同:上限随协议与切片漂移的病灶与收敛 ----
// 合同三件事(见 mcp/transport.hpp):上限管剥掉行尾 \r 后的单行长度;
// Feed 怎么切不影响准入结论;超限即报废、同批先前合规行照常交出。

TEST_CASE("LineFramer: 超限行恰与换行同批到达也拒绝——切片漂移已封死") {
    // 静态反例:第一拍喂恰在上限的残行,第二拍补 "x\n"。旧实现先吐出
    // 上限+1 字节的完整行、再清残缓冲,overflowed 仍为假;第二拍若只到
    // "x" 却触发 overflow——同一消息,准入只随管道切片变。
    mcp::LineFramer framer;
    const std::string at_cap(mcp::LineFramer::kMaxLineBytes, 'x');
    CHECK(framer.Feed(at_cap).empty());  // 残行恰在上限:不判死,等换行
    CHECK_FALSE(framer.overflowed());

    const auto lines = framer.Feed("x\n");  // 凑齐:上限+1,拒绝
    CHECK(lines.empty());
    CHECK(framer.overflowed());

    // 反向切片:第二拍先只到 "x"(无换行),也判死——两种切法同一结论。
    mcp::LineFramer twin;
    CHECK(twin.Feed(at_cap).empty());
    CHECK(twin.Feed("x").empty());
    CHECK(twin.overflowed());
}

TEST_CASE("LineFramer: 恰在上限的单行通过——整片/劈在边界/逐字节凑尾结论一致") {
    const std::string line(mcp::LineFramer::kMaxLineBytes, 'x');

    // 整片喂入。
    {
        mcp::LineFramer framer;
        const auto lines = framer.Feed(line + "\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].size() == mcp::LineFramer::kMaxLineBytes);
        CHECK_FALSE(framer.overflowed());
    }
    // 劈在边界:残行攒到恰在上限,再补换行。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(line).empty());
        const auto lines = framer.Feed("\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].size() == mcp::LineFramer::kMaxLineBytes);
        CHECK_FALSE(framer.overflowed());
    }
    // 逐字节凑尾:主体一次喂,末尾几个字节逐个喂(含换行)。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(std::string_view(line).substr(0, line.size() - 3)).empty());
        CHECK(framer.Feed("x").empty());
        CHECK(framer.Feed("x").empty());
        CHECK(framer.Feed("x").empty());
        const auto lines = framer.Feed("\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].size() == mcp::LineFramer::kMaxLineBytes);
        CHECK_FALSE(framer.overflowed());
    }
}

TEST_CASE("LineFramer: 超上限一字的单行拒绝——整片/劈在边界/逐字节凑尾结论一致") {
    const std::string line(mcp::LineFramer::kMaxLineBytes + 1, 'x');

    // 整片喂入:凑齐即拒。
    {
        mcp::LineFramer framer;
        const auto lines = framer.Feed(line + "\n");
        CHECK(lines.empty());
        CHECK(framer.overflowed());
    }
    // 劈在边界:残行攒到上限+1,换行未到先判死。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(line).empty());
        CHECK(framer.overflowed());
        CHECK(framer.Feed("\n").empty());  // 报废后不再吐行
    }
    // 逐字节凑尾:最后一字落账即判死。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(std::string_view(line).substr(0, line.size() - 1)).empty());
        CHECK_FALSE(framer.overflowed());  // 恰在上限:还不判死
        CHECK(framer.Feed("x").empty());
        CHECK(framer.overflowed());
    }
}

TEST_CASE("LineFramer: 恰在上限且以 \\r\\n 收尾——攒行途中不误判(末位 \\r 按最小可能行长核界)") {
    // 行 = 上限个 'x' + "\r\n":剥 \r 后恰在上限,必须通过。逐字节喂时
    // 缓冲一度到上限+1(末位 \r),残行核界若把这算超限,就跟整片喂漂移了。
    const std::string body(mcp::LineFramer::kMaxLineBytes, 'x');

    // 整片喂入。
    {
        mcp::LineFramer framer;
        const auto lines = framer.Feed(body + "\r\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].size() == mcp::LineFramer::kMaxLineBytes);
        CHECK_FALSE(framer.overflowed());
    }
    // 逐字节凑尾:末位 \r 单独一拍,缓冲 = 上限+1。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(std::string_view(body).substr(0, body.size() - 3)).empty());
        CHECK(framer.Feed("x").empty());
        CHECK(framer.Feed("x").empty());
        CHECK(framer.Feed("x").empty());
        CHECK(framer.Feed("\r").empty());  // 缓冲上限+1,末位 \r:不判死
        CHECK_FALSE(framer.overflowed());
        const auto lines = framer.Feed("\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].size() == mcp::LineFramer::kMaxLineBytes);
        CHECK_FALSE(framer.overflowed());
    }
}

TEST_CASE("LineFramer: 剥 \\r 后仍超上限的行——换行前即判死,与整片喂结论一致") {
    // 行 = 上限+1 个 'x' + "\r\n":剥 \r 后仍超一字,拒绝。
    const std::string line(mcp::LineFramer::kMaxLineBytes + 1, 'x');

    // 整片喂入。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(line + "\r\n").empty());
        CHECK(framer.overflowed());
    }
    // 逐字节凑尾:攒到上限+1 个 'x' 时缓冲无 \r,直接判死。
    {
        mcp::LineFramer framer;
        CHECK(framer.Feed(std::string_view(line).substr(0, line.size() - 1)).empty());
        CHECK(framer.Feed("x").empty());
        CHECK(framer.overflowed());
        CHECK(framer.Feed("\r\n").empty());  // 报废后不再吐行
    }
}

TEST_CASE("LineFramer: 多条均合规消息同批到达全部解出——上限管单行,不管合计") {
    mcp::LineFramer framer;
    const std::string line(3 * 1024 * 1024, 'a');  // 单行 3MB,合规
    std::string batch;
    for (int i = 0; i < 3; ++i) {
        batch += line;
        batch += '\n';  // 合计 9MB,超过单行上限,但不是总量帽
    }
    const auto lines = framer.Feed(batch);
    REQUIRE(lines.size() == 3);
    CHECK_FALSE(framer.overflowed());
}

TEST_CASE("LineFramer: 同批里超限行只判死它自己,先前凑齐的合规行照常交出") {
    mcp::LineFramer framer;
    std::string batch = "good1\ngood2\n";
    batch += std::string(mcp::LineFramer::kMaxLineBytes + 1, 'z');
    batch += "\nafter\n";  // 判死之后这行不该再出来
    const auto lines = framer.Feed(batch);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "good1");
    CHECK(lines[1] == "good2");
    CHECK(framer.overflowed());
    CHECK(framer.Feed("more\n").empty());  // 报废后不再吐行
}
