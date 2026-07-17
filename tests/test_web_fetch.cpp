// web_fetch 的纯函数部分:HTML 剥离器(标签/script/style 剔除、实体还原、
// 空白折叠、中文)、UTF-8 安全截断、非法 UTF-8 清洗。不碰网络——真抓网页
// 那条路留给集成验证。

#include <doctest/doctest.h>

#include <string>

#include "tools/web_fetch.hpp"

using lubancode::tools::SanitizeUtf8;
using lubancode::tools::StripHtml;
using lubancode::tools::TruncateUtf8;
using lubancode::tools::WebFetchTool;

TEST_CASE("StripHtml: 普通标签剥掉,正文保留") {
    const std::string html = "<html><body><p>hello <b>world</b></p></body></html>";
    const std::string text = StripHtml(html);
    CHECK(text.find("hello world") != std::string::npos);
    CHECK(text.find("<") == std::string::npos);
    CHECK(text.find("body") == std::string::npos);
}

TEST_CASE("StripHtml: script/style 整块连内容一起剔掉") {
    const std::string html =
        "<p>before</p><script type=\"text/javascript\">var x = '<p>fake</p>';</script>"
        "<style>.a { color: red; }</style><p>after</p>";
    const std::string text = StripHtml(html);
    CHECK(text.find("before") != std::string::npos);
    CHECK(text.find("after") != std::string::npos);
    CHECK(text.find("var x") == std::string::npos);
    CHECK(text.find("color") == std::string::npos);
    CHECK(text.find("fake") == std::string::npos);
}

TEST_CASE("StripHtml: 大小写混着写的 SCRIPT/Style 同样剔掉") {
    const std::string html = "<SCRIPT>alert(1)</Script><STYLE>b{}</style><p>正文</p>";
    const std::string text = StripHtml(html);
    CHECK(text.find("alert") == std::string::npos);
    CHECK(text.find("b{}") == std::string::npos);
    CHECK(text.find("正文") != std::string::npos);
}

TEST_CASE("StripHtml: 注释剔掉") {
    const std::string text = StripHtml("<p>a</p><!-- 注释里有 <p>标签</p> -->b");
    CHECK(text.find("注释") == std::string::npos);
    CHECK(text.find("a") != std::string::npos);
    CHECK(text.find("b") != std::string::npos);
}

TEST_CASE("StripHtml: 常见实体还原") {
    const std::string text = StripHtml("a &amp; b &lt;tag&gt; &quot;q&quot; c&nbsp;d");
    CHECK(text.find("a & b") != std::string::npos);
    CHECK(text.find("<tag>") != std::string::npos);
    CHECK(text.find("\"q\"") != std::string::npos);
    CHECK(text.find("c d") != std::string::npos);
}

TEST_CASE("StripHtml: 认不出的实体原样保留") {
    const std::string text = StripHtml("a &hellip; b");
    CHECK(text.find("&hellip;") != std::string::npos);
}

TEST_CASE("StripHtml: 块级标签换成换行,行内标签不产生换行") {
    const std::string text = StripHtml("<h1>标题</h1><p>第一段有 <em>强调</em> 词</p><p>第二段</p>");
    CHECK(text.find("标题\n") != std::string::npos);
    CHECK(text.find("第一段有 强调 词") != std::string::npos);
    CHECK(text.find("第一段有 强调 词\n") != std::string::npos);
    CHECK(text.find("第二段") != std::string::npos);
}

TEST_CASE("StripHtml: 连续空白折叠,段落间最多空一行") {
    const std::string html = "<p>a    b\t\tc</p>\n\n\n\n<div></div><div></div><div></div><p>d</p>";
    const std::string text = StripHtml(html);
    CHECK(text.find("a b c") != std::string::npos);
    // 一堆空块级标签堆出来的换行,折叠后不超过两个连续换行。
    CHECK(text.find("\n\n\n") == std::string::npos);
    CHECK(text.find("d") != std::string::npos);
}

TEST_CASE("StripHtml: 中文正文原样保留") {
    const std::string text = StripHtml("<div class=\"x\">武松打虎,<span>景阳冈</span>上。</div>");
    CHECK(text.find("武松打虎,景阳冈上。") != std::string::npos);
}

TEST_CASE("StripHtml: 没闭合的残破标签不至于把后面正文全吞掉") {
    const std::string text = StripHtml("<p>好句<broken");
    CHECK(text.find("好句") != std::string::npos);
}

TEST_CASE("TruncateUtf8: ASCII 恰好截在边界") {
    CHECK(TruncateUtf8("hello", 3) == "hel");
    CHECK(TruncateUtf8("hello", 10) == "hello");
    CHECK(TruncateUtf8("hello", 5) == "hello");
}

TEST_CASE("TruncateUtf8: 不吐半个中文字符") {
    const std::string text = "汉字";  // 每个字 3 字节,共 6 字节
    CHECK(TruncateUtf8(text, 6) == "汉字");
    CHECK(TruncateUtf8(text, 5) == "汉");
    CHECK(TruncateUtf8(text, 4) == "汉");
    CHECK(TruncateUtf8(text, 3) == "汉");
    CHECK(TruncateUtf8(text, 2) == "");
}

TEST_CASE("SanitizeUtf8: 合法 UTF-8(含中文)原样保留") {
    const std::string text = "hello 世界";
    CHECK(SanitizeUtf8(text) == text);
}

TEST_CASE("SanitizeUtf8: 非法字节替换成问号") {
    std::string bad = "ok";
    bad += static_cast<char>(0xC4);  // GBK "中" 的首字节,后面不是合法续字节
    bad += static_cast<char>(0xE3);
    const std::string cleaned = SanitizeUtf8(bad);
    CHECK(cleaned.find("ok") == 0);
    CHECK(cleaned.find('?') != std::string::npos);
    // 清洗后的结果塞进 JSON 不该抛异常。
    CHECK_NOTHROW([&] {
        nlohmann::json j;
        j["text"] = cleaned;
        (void)j.dump();
    }());
}

TEST_CASE("WebFetchTool: 参数校验(缺 url / 坏协议 / 坏 max_bytes)不碰网络直接报错") {
    WebFetchTool tool;
    CHECK(tool.execute(nlohmann::json::object()).is_error);
    CHECK(tool.execute({{"url", "ftp://example.com"}}).is_error);
    CHECK(tool.execute({{"url", "https://example.com"}, {"max_bytes", -1}}).is_error);
}

TEST_CASE("WebFetchTool: 元信息") {
    WebFetchTool tool;
    CHECK(tool.name() == "web_fetch");
    CHECK_FALSE(tool.needs_confirm());
    const auto schema = tool.input_schema();
    CHECK(schema["required"] == nlohmann::json::array({"url"}));
}
