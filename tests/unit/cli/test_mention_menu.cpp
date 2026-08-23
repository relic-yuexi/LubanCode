// @ 提及菜单(交互抛光总账第三批)的纯逻辑测试 + 编辑器草稿归一 +
// Backspace 整枚删(编辑器核心侧):

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/console_input.hpp"  // NormalizeEditorDraft
#include "cli/line_editor.hpp"
#include "cli/mention_menu.hpp"

using namespace lubancode::cli;

namespace {

std::vector<FileMentionEntry> MakeEntries() {
    return {
        {"src", true},
        {"src/main.cpp", false},
        {"src/cli/console_input.cpp", false},
        {"tests/test_main.cpp", false},
        {"README.md", false},
        {"docs/带 空格 的目录", true},
    };
}

std::u32string U32(const std::string& text) { return Utf8ToUtf32(text); }

}  // namespace

TEST_CASE("FindMentionToken:光标在词元内/尾都认,<带空格>形式也不漏") {
    const std::u32string line = U32("看看 @src/ma 与 @<docs/带 空格 的目录>/ 好");
    // 光标在 "@src/ma" 尾(码点下标 10:@ 在 3,词元半开尾在 10)。
    auto token = FindMentionToken(line, 10);
    REQUIRE(token.has_value());
    CHECK(token->start == 3);
    CHECK(token->end == 10);
    CHECK(!token->bracketed);
    CHECK(token->query == "src/ma");

    // 光标在带空格 <> 词元中间(闭角之前)——回溯法断不了,前向解析必须认。
    const std::u32string bracketed = U32("@<docs/带 空格 的目录>/ rest");
    token = FindMentionToken(bracketed, 8);  // 光标落在"带"之后
    REQUIRE(token.has_value());
    CHECK(token->bracketed);
    CHECK(token->query == "docs/带 空格 的目录");

    // 光标在普通文字上:不是提及。
    CHECK_FALSE(FindMentionToken(U32("普通正文"), 3).has_value());
    // 邮箱样式的 '@'(前面不是空白)不算提及。
    CHECK_FALSE(FindMentionToken(U32("mailto a@b.com"), 8).has_value());
    // 光标压着 '@'(刚敲下)也算——空查询,给根层清单。
    token = FindMentionToken(U32("看 @"), 2);
    REQUIRE(token.has_value());
    CHECK(token->query.empty());
}

TEST_CASE("FuzzyMatchMentions:子序列命中,边界加权,根层条目可空查") {
    const auto entries = MakeEntries();
    auto hits = FuzzyMatchMentions(entries, "main");
    REQUIRE_FALSE(hits.empty());
    // "src/main.cpp" 与 "tests/test_main.cpp" 都命中;前者连续 + 无干扰,
    // 分更高,排第一。
    CHECK(entries[hits[0]].relative_path == "src/main.cpp");
    bool saw_test_main = false;
    for (const std::size_t h : hits) {
        if (entries[h].relative_path == "tests/test_main.cpp") {
            saw_test_main = true;
        }
    }
    CHECK(saw_test_main);

    // 目录直击:@src/cli 应把 src/cli/console_input.cpp 排前,目录 src 也在。
    hits = FuzzyMatchMentions(entries, "src/cli");
    REQUIRE_FALSE(hits.empty());
    CHECK(entries[hits[0]].relative_path == "src/cli/console_input.cpp");

    // 查不出:子序列断了就没有。
    CHECK(FuzzyMatchMentions(entries, "zzz").empty());

    // 空查询:全部按字典序,截 limit。
    CHECK(FuzzyMatchMentions(entries, "", 3).size() == 3);
}

TEST_CASE("MentionInsertionString:无空白裸写,带空白/角括号用 <> 包") {
    FileMentionEntry file{"src/main.cpp", false};
    FileMentionEntry dir{"src", true};
    FileMentionEntry spaced{"docs/带 空格", true};
    CHECK(MentionInsertionString(file) == "@src/main.cpp");
    CHECK(MentionInsertionString(dir) == "@src/");
    CHECK(MentionInsertionString(spaced) == "@<docs/带 空格/>");
}

TEST_CASE("ReplaceMentionToken:只换词元,词元前后一字不动") {
    const std::u32string line = U32("前面 @src/ma 后面");
    auto token = FindMentionToken(line, 10);  // 光标在词元尾
    REQUIRE(token.has_value());
    const std::u32string replaced = ReplaceMentionToken(line, *token, "@src/main.cpp ");
    CHECK(Utf32ToUtf8(replaced) == "前面 @src/main.cpp  后面");
}

TEST_CASE("ExtractTextMentions:词首 @ 才收,<带空格> 支持,去重") {
    const std::string text = "看 @src/main.cpp 与 @<docs/带 空格> 还有 @src/main.cpp,邮箱 a@b.com 不算";
    const auto mentions = ExtractTextMentions(text);
    REQUIRE(mentions.size() == 2);  // 重复的 @src/main.cpp 去重
    CHECK(mentions[0] == "src/main.cpp");
    CHECK(mentions[1] == "docs/带 空格");
}

TEST_CASE("编辑器:Backspace 在词元尾整枚删,词元中间普通退格") {
    LineEditorCore editor;
    editor.BeginLine(/*composer=*/true);
    editor.LoadText(U32("看 @src/main.cpp"));
    // 光标默认落末尾(词元尾):Backspace 整枚删。
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(Utf32ToUtf8(state.line) == "看 ");

    // 光标移到词元中间(比如 "main" 的 a 之后):普通退格删一个字。
    editor.LoadText(U32("看 @src/main.cpp"));
    // "看 "=2 码点,@ 在 2;@src/ma 长 7,光标落在 2+7=9(第 9 个码点前)。
    editor.LoadTextWithCursor(U32("看 @src/main.cpp"), 9);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(Utf32ToUtf8(state.line) == "看 @src/min.cpp");  // 删掉 'a',词元留着

    // @<带 空格> 形式:光标在闭角后整枚删。
    editor.LoadText(U32("附 @<docs/带 空格>/"));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(Utf32ToUtf8(state.line) == "附 @<docs/带 空格>");
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(Utf32ToUtf8(state.line) == "附 ");
}

TEST_CASE("NormalizeEditorDraft:CRLF 归一,行尾补的一个换行剥掉") {
    CHECK(NormalizeEditorDraft("a\r\nb\r\n") == "a\nb");
    CHECK(NormalizeEditorDraft("a\rb") == "a\nb");
    CHECK(NormalizeEditorDraft("末尾两个换行\n\n") == "末尾两个换行\n");  // 只剥一个
    CHECK(NormalizeEditorDraft("") == "");
    CHECK(NormalizeEditorDraft("单个") == "单个");
}

TEST_CASE("BuildMentionMenuLines:头行 + 选中标记 + 目录图标") {
    const auto entries = MakeEntries();
    const auto hits = FuzzyMatchMentions(entries, "src");
    REQUIRE_FALSE(hits.empty());
    const auto lines = BuildMentionMenuLines(entries, hits, 0, 80);
    REQUIRE(lines.size() >= 2);
    CHECK(lines[0].find("@") != std::string::npos);  // 头行带提示
    CHECK(lines[1].rfind("❯ ", 0) == 0);             // 第一条选中
    const auto unselected = BuildMentionMenuLines(entries, hits, -1, 80);
    CHECK(unselected[1].rfind("  ", 0) == 0);
    // 无命中:一行空结果。
    const auto none = BuildMentionMenuLines(entries, {}, 0, 80);
    REQUIRE(none.size() == 2);
    CHECK(none[1].find("没有命中") != std::string::npos);
}
