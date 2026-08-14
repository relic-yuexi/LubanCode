// LineEditorCore 是逐键输入编辑器的核心层,吃抽象 KeyEvent、吐 RenderState,
// 不碰任何 Win32 API / 真实控制台——这里全部喂 KeyEvent 序列断言
// RenderState,一步都不碰控制台,能在任何环境下跑。

#include <doctest/doctest.h>

#include <algorithm>

#include "cli/line_editor.hpp"

using namespace lubancode::cli;

namespace {

std::vector<CompletionCandidate> SampleCandidates() {
    return {
        {"/help", "列出所有命令"},
        {"/model", "拉模型列表"},
        {"/config", "打印当前配置"},
        {"/clear", "清空对话历史"},
        {"/exit", "退出"},
    };
}

void TypeString(LineEditorCore& editor, const std::string& ascii_text) {
    for (char c : ascii_text) {
        editor.HandleKey(KeyEvent::Char(static_cast<char32_t>(static_cast<unsigned char>(c))));
    }
}

}  // namespace

TEST_CASE("LineEditorCore: 千字以内多行 paste 直接显示并按原文提交") {
    LineEditorCore editor;
    editor.BeginLine(true);

    const auto pasted = editor.HandleKey(KeyEvent::Paste("first\nsecond"));
    REQUIRE(pasted.lines.size() == 2);
    CHECK(Utf32ToUtf8(pasted.lines[0]) == "first");
    CHECK(Utf32ToUtf8(pasted.lines[1]) == "second");
    CHECK(Utf32ToUtf8(pasted.line) == "first\nsecond");

    const auto submitted = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(submitted.submitted);
    CHECK(Utf32ToUtf8(submitted.line) == "first\nsecond");
    REQUIRE(submitted.lines.size() == 2);
    CHECK(Utf32ToUtf8(submitted.lines[0]) == "first");
    CHECK(Utf32ToUtf8(submitted.lines[1]) == "second");

    editor.BeginLine(true);
    const auto history = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    REQUIRE(history.lines.size() == 2);
    CHECK(Utf32ToUtf8(history.lines[0]) == "first");
    CHECK(Utf32ToUtf8(history.lines[1]) == "second");
}

TEST_CASE("LineEditorCore: 单行 bracketed paste 仍按普通文本编辑") {
    LineEditorCore editor;
    editor.BeginLine(true);
    const auto state = editor.HandleKey(KeyEvent::Paste("plain text"));
    REQUIRE(state.lines.size() == 1);
    CHECK(Utf32ToUtf8(state.lines[0]) == "plain text");
    CHECK(Utf32ToUtf8(state.line) == "plain text");
}

TEST_CASE("LineEditorCore: 超过千字才折叠且提交回显展开全文") {
    LineEditorCore editor;
    editor.BeginLine(true);

    const std::string large(kLargePasteCharThreshold + 1, 'x');
    auto state = editor.HandleKey(KeyEvent::Paste(large));
    REQUIRE(state.lines.size() == 1);
    const std::string placeholder = Utf32ToUtf8(state.lines[0]);
    CHECK(placeholder.find("1001") != std::string::npos);
    CHECK(placeholder.find("xxx") == std::string::npos);

    const auto submitted = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(submitted.submitted);
    CHECK(Utf32ToUtf8(submitted.line) == large);
    REQUIRE(submitted.lines.size() == 1);
    CHECK(Utf32ToUtf8(submitted.lines[0]) == large);
}

TEST_CASE("LineEditorCore: 一千字边界直接显示,一千零一字折叠") {
    LineEditorCore editor;
    editor.BeginLine(true);

    const std::string boundary(kLargePasteCharThreshold, 'a');
    auto state = editor.HandleKey(KeyEvent::Paste(boundary));
    CHECK(Utf32ToUtf8(state.lines[0]) == boundary);

    editor.BeginLine(true);
    const std::string over(kLargePasteCharThreshold + 1, 'b');
    state = editor.HandleKey(KeyEvent::Paste(over));
    CHECK(Utf32ToUtf8(state.lines[0]).find("1001") != std::string::npos);
}

TEST_CASE("LineEditorCore: 紧邻的大 paste 片段合成一枚占位且提交原文不乱") {
    LineEditorCore editor;
    editor.BeginLine(true);

    const std::string first(kLargePasteCharThreshold + 1, 'x');
    auto state = editor.HandleKey(KeyEvent::Paste(first));
    state = editor.HandleKey(KeyEvent::Paste("tail"));
    REQUIRE(state.lines.size() == 1);
    const std::string merged_placeholder = Utf32ToUtf8(state.lines[0]);
    CHECK(merged_placeholder.find("1005") != std::string::npos);
    CHECK(std::count(merged_placeholder.begin(), merged_placeholder.end(), '[') == 1);
    CHECK(Utf32ToUtf8(state.line) == first + "tail");

    const auto submitted = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(submitted.submitted);
    CHECK(Utf32ToUtf8(submitted.line) == first + "tail");
    REQUIRE(submitted.lines.size() == 1);
    CHECK(Utf32ToUtf8(submitted.lines[0]) == first + "tail");
}

TEST_CASE("LineEditorCore: 紧邻的短 paste 片段仍按多行明文显示") {
    LineEditorCore editor;
    editor.BeginLine(true);

    auto state = editor.HandleKey(KeyEvent::Paste("alpha"));
    CHECK(Utf32ToUtf8(state.lines[0]) == "alpha");

    state = editor.HandleKey(KeyEvent::Paste("\nbeta"));
    REQUIRE(state.lines.size() == 2);
    CHECK(Utf32ToUtf8(state.lines[0]) == "alpha");
    CHECK(Utf32ToUtf8(state.lines[1]) == "beta");
    CHECK(Utf32ToUtf8(state.line) == "alpha\nbeta");

    const auto submitted = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(Utf32ToUtf8(submitted.line) == "alpha\nbeta");
}

TEST_CASE("LineEditorCore: ConPTY 已露出的 paste 首行可原位换成完整明文") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "for i in range(len(nums)):");

    const std::string full =
        "for i in range(len(nums)):\n"
        "    for j in range(i+1,len(nums)):\n"
        "        return [i,j]";
    const auto state = editor.HandleKey(KeyEvent::Paste(full, 26));
    REQUIRE(state.lines.size() == 3);
    CHECK(Utf32ToUtf8(state.lines[0]) == "for i in range(len(nums)):");
    CHECK(Utf32ToUtf8(state.line) == full);

    const auto submitted = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(submitted.submitted);
    CHECK(Utf32ToUtf8(submitted.line) == full);
}

TEST_CASE("LineEditorCore: 编辑键截断 paste 合并且退格整枚删除占位") {
    LineEditorCore editor;
    editor.BeginLine(true);

    const std::string large(kLargePasteCharThreshold + 1, 'x');
    const std::string other(kLargePasteCharThreshold + 2, 'y');
    editor.HandleKey(KeyEvent::Paste(large));
    auto state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(Utf32ToUtf8(state.lines[0]).empty());
    CHECK(Utf32ToUtf8(state.line).empty());

    editor.HandleKey(KeyEvent::Paste(large));
    editor.HandleKey(KeyEvent::Char(U' '));
    state = editor.HandleKey(KeyEvent::Paste(other));
    const std::string display = Utf32ToUtf8(state.lines[0]);
    CHECK(std::count(display.begin(), display.end(), '[') == 2);
    CHECK(Utf32ToUtf8(state.line) == large + " " + other);
}

TEST_CASE("LineEditorCore: 敲字符会插到光标位置,光标跟着往后走") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "abc");
    RenderState state = editor.CurrentRenderState();
    CHECK(state.line == U"abc");
    CHECK(state.cursor == 3);

    // 光标移到中间插入,验证不是简单地往末尾追加。
    editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    state = editor.HandleKey(KeyEvent::Char(U'X'));
    CHECK(state.line == U"abXc");
    CHECK(state.cursor == 3);
}

TEST_CASE("LineEditorCore: 退格删光标前一个字符") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "abc");
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(state.line == U"ab");
    CHECK(state.cursor == 2);

    // 光标在行首退格,什么都不发生。
    editor.HandleKey(KeyEvent::Simple(KeyKind::Home));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(state.line == U"ab");
    CHECK(state.cursor == 0);
}

TEST_CASE("LineEditorCore: 左右方向键、Home/End 移动光标,不越界") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "ab");

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    CHECK(state.cursor == 1);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    CHECK(state.cursor == 0);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));  // 已经在行首,不再往前
    CHECK(state.cursor == 0);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::End));
    CHECK(state.cursor == 2);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Right));  // 已经在行尾,不再往后
    CHECK(state.cursor == 2);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Home));
    CHECK(state.cursor == 0);
}

TEST_CASE("LineEditorCore: Enter 提交当前行,提交后行缓冲清空") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "hello");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(state.submitted);
    CHECK(state.line == U"hello");
    CHECK(editor.history_size() == 1);

    const RenderState after = editor.CurrentRenderState();
    CHECK(after.line.empty());
}

TEST_CASE("LineEditorCore: 空行 Enter 不计入历史") {
    LineEditorCore editor;
    editor.BeginLine();
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(editor.history_size() == 0);
}

TEST_CASE("LineEditorCore: 历史上下键翻,翻到最老一条不越界,翻回底部还原草稿") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "first");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    editor.BeginLine();
    TypeString(editor, "second");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));

    editor.BeginLine();
    TypeString(editor, "draft");  // 还没提交、正在敲的这一行

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.line == U"second");  // 最近一条

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.line == U"first");  // 再往前翻到最老一条

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.line == U"first");  // 已经是最老一条,再翻不动

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.line == U"second");

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.line == U"draft");  // 翻回底部,还原成刚才没提交的草稿
}

TEST_CASE("LineEditorCore: 翻到一半开始编辑,落回底部,再按 Up 重新从最新一条开始翻") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "first");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    editor.BeginLine();
    TypeString(editor, "second");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));

    editor.BeginLine();
    editor.HandleKey(KeyEvent::Simple(KeyKind::Up));  // 翻到 "second"
    RenderState state = editor.HandleKey(KeyEvent::Char(U'!'));
    CHECK(state.line == U"second!");  // 就地编辑历史记录

    // 再按 Up,应该重新从最新一条("second")开始翻,不是接着刚才那条继续走。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.line == U"second");

    // 翻到底部应该恢复成刚才编辑出来的 "second!",不是空草稿。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.line == U"second!");
}

TEST_CASE("LineEditorCore: Tab 补全,没有匹配的候选,行内容不变") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/zzz");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"/zzz");
    CHECK(state.hint_lines.empty());  // 没有候选匹配,提示区跟着清空
}

TEST_CASE("LineEditorCore: Tab 补全,唯一匹配直接补全整名 + 空格") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/he");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"/help ");
    CHECK(state.cursor == state.line.size());
}

namespace {

// 找出 hint_lines 里以 "> " 开头的那一行(轮转当前选中的标记行),
// 断言"恰好一行被标中",顺带把这一行内容吐出来核对候选名对不对。
std::string FindMarkedHintLine(const std::vector<std::string>& hint_lines) {
    std::string marked;
    int marked_count = 0;
    for (const auto& line : hint_lines) {
        if (line.rfind("> ", 0) == 0) {
            marked = line;
            ++marked_count;
        }
    }
    CHECK(marked_count == 1);
    return marked;
}

}  // namespace

TEST_CASE("LineEditorCore: Tab 补全,多个匹配先补公共前缀,再按 Tab 轮转候选") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/c");  // 匹配 /config、/clear,公共前缀就是 "/c" 本身

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    // 公共前缀跟当前输入一样长,没法再往前补,直接进入轮转,给出第一个候选。
    const bool first_is_config_or_clear = (state.line == U"/config ") || (state.line == U"/clear ");
    CHECK(first_is_config_or_clear);
    const std::u32string first_candidate = state.line;
    // 轮转会话开始了,两个候选都还在提示区里,当前选中那个行首标 "> "。
    REQUIRE(state.hint_lines.size() == 2);
    std::string marked = FindMarkedHintLine(state.hint_lines);
    CHECK(marked.find(Utf32ToUtf8(first_candidate.substr(0, first_candidate.size() - 1))) != std::string::npos);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line != first_candidate);  // 轮转到另一个候选
    const bool second_is_config_or_clear = (state.line == U"/config ") || (state.line == U"/clear ");
    CHECK(second_is_config_or_clear);
    REQUIRE(state.hint_lines.size() == 2);
    marked = FindMarkedHintLine(state.hint_lines);
    // 标记跟着轮转换到了当前选中的候选那一行,不是停在原地。
    CHECK(marked.find(Utf32ToUtf8(state.line.substr(0, state.line.size() - 1))) != std::string::npos);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == first_candidate);  // 只有两个候选,转一圈回到第一个
}

TEST_CASE("LineEditorCore: Tab 补全,公共前缀比已输入的还长时,第一下先补到公共前缀,不直接进入轮转") {
    // 用一组公共前缀比已输入前缀更长的候选("/foobar" "/foobaz" 共同前缀是
    // "/fooba",比已输入的 "/fo" 长),验证"先补公共前缀、再按 Tab 才轮转"
    // 这两步是分开的,不是一次 Tab 就跳进轮转。
    std::vector<CompletionCandidate> candidates = {
        {"/foobar", "候选一"},
        {"/foobaz", "候选二"},
    };
    LineEditorCore editor(std::move(candidates));
    editor.BeginLine();
    TypeString(editor, "/fo");

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"/fooba");  // 只补到公共前缀,没有直接选中某个候选
    // 两个候选都列出来了,但还没真正选中任何一个,不该有 "> " 标记。
    REQUIRE(state.hint_lines.size() == 2);
    CHECK(state.hint_lines[0].rfind("> ", 0) != 0);
    CHECK(state.hint_lines[1].rfind("> ", 0) != 0);
    CHECK(state.hint_lines[0].rfind("  ", 0) == 0);
    CHECK(state.hint_lines[1].rfind("  ", 0) == 0);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    const bool is_first_candidate = (state.line == U"/foobar ") || (state.line == U"/foobaz ");
    CHECK(is_first_candidate);  // 再按一下 Tab,才真正开始轮转
    // 这下真正选中了,该有恰好一行标 "> " 了。
    REQUIRE(state.hint_lines.size() == 2);
    const int marked_count = static_cast<int>(state.hint_lines[0].rfind("> ", 0) == 0) +
                              static_cast<int>(state.hint_lines[1].rfind("> ", 0) == 0);
    CHECK(marked_count == 1);
}

TEST_CASE("LineEditorCore: 敲字符会打断正在进行的 Tab 轮转会话") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/c");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));  // 进入轮转
    editor.HandleKey(KeyEvent::Char(U'!'));             // 打断
    // 这次 Tab 应该是全新的一次补全(以当前整行作为词),而不是接着上次轮转。
    // 当前行内容形如 "/config !" 或 "/clear !",不是 slash 命令词开头匹配,
    // 所以 Tab 不会有任何候选、行内容不变。
    const std::u32string before = editor.CurrentRenderState().line;
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == before);
}

TEST_CASE("LineEditorCore: 不以 / 开头,Tab 什么都不做,也没有提示行") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "hello");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"hello");
    CHECK(state.hint_lines.empty());
}

TEST_CASE("LineEditorCore: 行以 / 开头时,提示区一行一个候选,列出匹配的命令") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/c");
    const RenderState state = editor.CurrentRenderState();
    REQUIRE(state.hint_lines.size() == 2);  // 匹配 /config、/clear
    const std::string joined = state.hint_lines[0] + "\n" + state.hint_lines[1];
    CHECK(joined.find("/config") != std::string::npos);
    CHECK(joined.find("/clear") != std::string::npos);
    // 一行一个候选:任何单独一行不该同时出现两个候选名。
    CHECK_FALSE((state.hint_lines[0].find("/config") != std::string::npos &&
                 state.hint_lines[0].find("/clear") != std::string::npos));
    CHECK_FALSE((state.hint_lines[1].find("/config") != std::string::npos &&
                 state.hint_lines[1].find("/clear") != std::string::npos));
    // 还没开始 Tab 轮转,不该有选中标记。
    CHECK(state.hint_lines[0].rfind("  ", 0) == 0);
    CHECK(state.hint_lines[1].rfind("  ", 0) == 0);
}

TEST_CASE("LineEditorCore: 退格删到不再以 / 开头,提示区清空") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/c");
    CHECK_FALSE(editor.CurrentRenderState().hint_lines.empty());

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));  // "/"
    CHECK(state.line == U"/");
    // "/" 本身不匹配任何完整候选前缀过滤? 匹配规则是前缀匹配,"/" 匹配全部候选。
    CHECK_FALSE(state.hint_lines.empty());

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));  // 删掉 "/"
    CHECK(state.line.empty());
    CHECK(state.hint_lines.empty());  // 不再以 / 开头,提示区清空
}

TEST_CASE("LineEditorCore: 敲一个不匹配任何候选的 / 命令词,提示区清空") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/z");
    const RenderState state = editor.CurrentRenderState();
    CHECK(state.hint_lines.empty());  // /z 不匹配任何候选
}

TEST_CASE("LineEditorCore: 只有 1 个候选匹配,提示区只有 1 行,没有汇总行") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/mo");  // 只有 /model 匹配
    const RenderState state = editor.CurrentRenderState();
    REQUIRE(state.hint_lines.size() == 1);
    CHECK(state.hint_lines[0].find("/model") != std::string::npos);
}

TEST_CASE("LineEditorCore: 候选恰好 6 个,提示区正好 6 行,不加汇总行") {
    std::vector<CompletionCandidate> candidates;
    for (int i = 0; i < 6; ++i) {
        candidates.push_back(CompletionCandidate{"/a" + std::to_string(i), "候选说明 " + std::to_string(i)});
    }
    LineEditorCore editor(std::move(candidates));
    editor.BeginLine();
    TypeString(editor, "/a");  // 恰好 6 个候选全匹配

    const RenderState state = editor.CurrentRenderState();
    REQUIRE(state.hint_lines.size() == 6);  // 没有超过上限,不加汇总行
    for (int i = 0; i < 6; ++i) {
        CHECK(state.hint_lines[static_cast<std::size_t>(i)].find("/a" + std::to_string(i)) != std::string::npos);
    }
}

TEST_CASE("LineEditorCore: 候选超过 6 个,提示区最多 6 行 + 一行汇总") {
    std::vector<CompletionCandidate> candidates;
    for (int i = 0; i < 8; ++i) {
        candidates.push_back(CompletionCandidate{"/a" + std::to_string(i), "候选说明 " + std::to_string(i)});
    }
    LineEditorCore editor(std::move(candidates));
    editor.BeginLine();
    TypeString(editor, "/a");  // 8 个候选全匹配

    const RenderState state = editor.CurrentRenderState();
    REQUIRE(state.hint_lines.size() == 7);  // 6 行候选 + 1 行汇总
    for (int i = 0; i < 6; ++i) {
        CHECK(state.hint_lines[static_cast<std::size_t>(i)].find("/a" + std::to_string(i)) != std::string::npos);
    }
    const std::string& summary = state.hint_lines[6];
    CHECK(summary.find("8") != std::string::npos);  // 汇总行报出总数
    CHECK(summary.find("/a6") == std::string::npos);  // 第 7、8 个候选没有单独一行
    CHECK(summary.find("/a7") == std::string::npos);
}

TEST_CASE("LineEditorCore: ShiftTab 循环切换确认模式,三档循环") {
    LineEditorCore editor;
    CHECK(editor.confirm_mode() == ConfirmMode::Confirm);

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.mode_changed);
    CHECK(state.mode == ConfirmMode::Auto);
    CHECK(editor.confirm_mode() == ConfirmMode::Auto);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.mode == ConfirmMode::Yolo);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.mode == ConfirmMode::Confirm);
}

TEST_CASE("LineEditorCore: ShiftTab 不影响正在编辑的行内容") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "abc");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.line == U"abc");
    CHECK(state.cursor == 3);
}

TEST_CASE("NextConfirmMode: 三档顺序循环,不依赖 LineEditorCore") {
    CHECK(NextConfirmMode(ConfirmMode::Confirm) == ConfirmMode::Auto);
    CHECK(NextConfirmMode(ConfirmMode::Auto) == ConfirmMode::Yolo);
    CHECK(NextConfirmMode(ConfirmMode::Yolo) == ConfirmMode::Confirm);
}

TEST_CASE("LineEditorCore: Ctrl+C 在非空行清空当前行,继续编辑") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "abc");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlC));
    CHECK(state.cleared);
    CHECK_FALSE(state.eof_requested);
    CHECK(state.line.empty());
}

TEST_CASE("LineEditorCore: Ctrl+C 在空行请求 EOF") {
    LineEditorCore editor;
    editor.BeginLine();
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlC));
    CHECK(state.eof_requested);
    CHECK_FALSE(state.cleared);
}

TEST_CASE("LineEditorCore: Ctrl+D 总是请求 EOF") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "abc");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlD));
    CHECK(state.eof_requested);
}

TEST_CASE("LineEditorCore: Esc 在非空行清空当前行,esc_pressed 置位,不请求 EOF") {
    LineEditorCore editor;
    editor.BeginLine();
    TypeString(editor, "abc");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(state.esc_pressed);
    CHECK(state.cleared);
    CHECK(state.line.empty());
    CHECK_FALSE(state.eof_requested);
}

TEST_CASE("LineEditorCore: Esc 在空行也只是清行、置 esc_pressed,不像 Ctrl+C 那样请求 EOF") {
    LineEditorCore editor;
    editor.BeginLine();
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(state.esc_pressed);
    CHECK_FALSE(state.eof_requested);
}

TEST_CASE("CharDisplayWidth/DisplayWidth: ASCII 宽度 1,常用汉字宽度 2") {
    CHECK(CharDisplayWidth(U'a') == 1);
    CHECK(CharDisplayWidth(U'1') == 1);
    CHECK(CharDisplayWidth(U'中') == 2);
    CHECK(CharDisplayWidth(U'文') == 2);
    CHECK(CharDisplayWidth(U'!') == 1);

    CHECK(DisplayWidth(U"abc") == 3);
    CHECK(DisplayWidth(U"中文") == 4);
    CHECK(DisplayWidth(U"a中b") == 4);
    CHECK(DisplayWidth(U"") == 0);
}

TEST_CASE("LineEditorCore: 光标显示列宽随 CJK 字符按 2 算") {
    LineEditorCore editor;
    editor.BeginLine();
    editor.HandleKey(KeyEvent::Char(U'中'));
    editor.HandleKey(KeyEvent::Char(U'文'));
    RenderState state = editor.CurrentRenderState();
    CHECK(state.cursor == 2);              // 按码点算,两个字
    CHECK(state.cursor_display_col == 4);  // 按显示宽度算,每个汉字占两列

    editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    state = editor.CurrentRenderState();
    CHECK(state.cursor == 1);
    CHECK(state.cursor_display_col == 2);
}

TEST_CASE("Utf32ToUtf8: ASCII 和汉字都能正确编码回 UTF-8") {
    CHECK(Utf32ToUtf8(U"abc") == "abc");
    CHECK(Utf32ToUtf8(U"中文") == "\xe4\xb8\xad\xe6\x96\x87");
}

// 以下几个 TEST_CASE 是终端层"保证物理上永不折行"截断逻辑里,能脱离真实
// 控制台单测的纯函数部分:按显示宽度截断(UTF-32/UTF-8 两个版本)、编辑行
// 超宽时的可视窗口计算。

TEST_CASE("TruncateToDisplayWidth: 纯 ASCII,恰好边界,超宽截断") {
    CHECK(TruncateToDisplayWidth(U"hello", 10) == U"hello");   // 够宽,原样不动
    CHECK(TruncateToDisplayWidth(U"hello", 5) == U"hello");    // 恰好等宽,不截
    CHECK(TruncateToDisplayWidth(U"hello", 3) == U"hel");      // 超宽,截到刚好塞满
    CHECK(TruncateToDisplayWidth(U"hello", 0) == U"");         // 宽度给 0,空串
    CHECK(TruncateToDisplayWidth(U"hello", -1) == U"");        // 负数按 0 处理
}

TEST_CASE("TruncateToDisplayWidth: 中英混排,宽字符绝不切半个字宽") {
    // "a中b" 每个字符宽度依次是 1、2、1,累计宽度 1、3、4。
    CHECK(TruncateToDisplayWidth(U"a中b", 4) == U"a中b");  // 恰好放得下整串
    CHECK(TruncateToDisplayWidth(U"a中b", 3) == U"a中");   // 放不下最后的 b,砍掉整个字符
    CHECK(TruncateToDisplayWidth(U"a中b", 2) == U"a");     // 放不下"中"(要 2 列,只剩 1 列),整个不要
    CHECK(TruncateToDisplayWidth(U"a中b", 1) == U"a");
    CHECK(TruncateToDisplayWidth(U"中文", 3) == U"中");    // 剩 1 列放不下第二个汉字,不切半个字宽
}

TEST_CASE("TruncateUtf8ToDisplayWidth: UTF-8 版本,纯 ASCII、中英混排、边界、超宽都对") {
    CHECK(TruncateUtf8ToDisplayWidth("hello", 10) == "hello");
    CHECK(TruncateUtf8ToDisplayWidth("hello", 5) == "hello");
    CHECK(TruncateUtf8ToDisplayWidth("hello", 3) == "hel");
    CHECK(TruncateUtf8ToDisplayWidth("", 5) == "");
    CHECK(TruncateUtf8ToDisplayWidth("hello", 0) == "");

    // "a中b":UTF-8 编码,宽度同上面 UTF-32 版本的用例。
    const std::string mixed = Utf32ToUtf8(U"a中b");
    CHECK(TruncateUtf8ToDisplayWidth(mixed, 4) == Utf32ToUtf8(U"a中b"));
    CHECK(TruncateUtf8ToDisplayWidth(mixed, 3) == Utf32ToUtf8(U"a中"));
    CHECK(TruncateUtf8ToDisplayWidth(mixed, 2) == Utf32ToUtf8(U"a"));
}

// ---- 软换行 WrapToDisplayWidth / WrapUtf8ToDisplayWidth -------------------

TEST_CASE("WrapToDisplayWidth: 短文本不折,原样一段返回") {
    CHECK(WrapToDisplayWidth(U"hello", 10).size() == 1);
    CHECK(WrapToDisplayWidth(U"hello", 10)[0] == U"hello");
    CHECK(WrapToDisplayWidth(U"hello", 5)[0] == U"hello");  // 恰好等宽,不折
}

TEST_CASE("WrapToDisplayWidth: 纯 ASCII 按宽折行,每段不超过 max_width") {
    const std::vector<std::u32string> w = WrapToDisplayWidth(U"abcdefghij", 4);
    REQUIRE(w.size() == 3);
    CHECK(w[0] == U"abcd");
    CHECK(w[1] == U"efgh");
    CHECK(w[2] == U"ij");
    for (const std::u32string& seg : w) {
        CHECK(DisplayWidth(seg) <= 4);
    }
}

TEST_CASE("WrapToDisplayWidth: CJK 宽字不切半个,断在整字边界") {
    // "中文测试" 四个汉字各占 2 列,max_width=4 恰好两字一行。
    const std::vector<std::u32string> w = WrapToDisplayWidth(U"中文测试", 4);
    REQUIRE(w.size() == 2);
    CHECK(w[0] == U"中文");
    CHECK(w[1] == U"测试");
}

TEST_CASE("WrapToDisplayWidth: 宽字凑不下一整列时独占一行,不丢内容") {
    // max_width=3:一个汉字 2 列放得下,第二个会让累计到 4 超宽——各占一行。
    // 每段 2 列,不超过 3。
    const std::vector<std::u32string> w = WrapToDisplayWidth(U"中文测试", 3);
    REQUIRE(w.size() == 4);
    CHECK(w[0] == U"中");
    CHECK(w[1] == U"文");
    CHECK(w[2] == U"测");
    CHECK(w[3] == U"试");
}

TEST_CASE("WrapToDisplayWidth: 中英混排按显示宽累加折行") {
    // "a中b":a=1,中=2,b=1,总 4。max_width=3 → "a中"(3) | "b"(1)。
    const std::vector<std::u32string> w = WrapToDisplayWidth(U"a中b", 3);
    REQUIRE(w.size() == 2);
    CHECK(w[0] == U"a中");
    CHECK(w[1] == U"b");
}

TEST_CASE("WrapToDisplayWidth: 换行符强制断行") {
    const std::vector<std::u32string> w = WrapToDisplayWidth(U"ab\ncd", 10);
    REQUIRE(w.size() == 2);
    CHECK(w[0] == U"ab");
    CHECK(w[1] == U"cd");
}

TEST_CASE("WrapToDisplayWidth: 连续换行产生空段,空文本/非正宽给空 vector") {
    const std::vector<std::u32string> w = WrapToDisplayWidth(U"a\n\nb", 10);
    REQUIRE(w.size() == 3);
    CHECK(w[1].empty());  // 中间空行保留
    CHECK(WrapToDisplayWidth(U"", 5).empty());
    CHECK(WrapToDisplayWidth(U"hello", 0).empty());
    CHECK(WrapToDisplayWidth(U"hello", -1).empty());
}

TEST_CASE("WrapUtf8ToDisplayWidth: UTF-8 版本与 u32 版本结果一致") {
    const std::string text = Utf32ToUtf8(U"中文测试a");
    const std::vector<std::string> w = WrapUtf8ToDisplayWidth(text, 4);
    REQUIRE(w.size() == 3);
    CHECK(w[0] == Utf32ToUtf8(U"中文"));
    CHECK(w[1] == Utf32ToUtf8(U"测试"));
    CHECK(w[2] == "a");
}


TEST_CASE("ComputeEditLineWindow: 整行放得下时,窗口就是整行,光标列不变") {
    const EditLineWindow window = ComputeEditLineWindow(U"hello", 3, 20);
    CHECK(window.text == U"hello");
    CHECK(window.cursor_display_col == 3);
}

TEST_CASE("ComputeEditLineWindow: content_width <= 0 给空窗口") {
    const EditLineWindow window = ComputeEditLineWindow(U"hello", 2, 0);
    CHECK(window.text.empty());
    CHECK(window.cursor_display_col == 0);
}

TEST_CASE("ComputeEditLineWindow: 超宽时窗口式截断,光标始终落在窗口可见范围内") {
    // 20 个字符的行,窗口只给 5 列宽,光标在各个位置都不能被挤出窗口外。
    const std::u32string line = U"0123456789abcdefghij";  // 20 个字符,宽度各 1

    // 光标在行首:窗口应该覆盖行首。
    EditLineWindow window = ComputeEditLineWindow(line, 0, 5);
    CHECK(DisplayWidth(window.text) <= 5);
    CHECK(window.cursor_display_col <= DisplayWidth(window.text));
    CHECK(window.text.front() == U'0');  // 光标在最左边,窗口从行首开始截

    // 光标在行尾:窗口应该覆盖行尾,光标落在窗口最后一列。
    window = ComputeEditLineWindow(line, line.size(), 5);
    CHECK(DisplayWidth(window.text) <= 5);
    CHECK(window.cursor_display_col == DisplayWidth(window.text));  // 光标顶在窗口最右边
    CHECK(window.text.back() == U'j');

    // 光标在行中间:不折行(窗口宽度不超过 content_width),光标在窗口内可见。
    window = ComputeEditLineWindow(line, 10, 5);
    CHECK(DisplayWidth(window.text) <= 5);
    CHECK(window.cursor_display_col <= DisplayWidth(window.text));
    // 窗口里的字符必须在原串里连续出现(没有被稀奇古怪地拼接)。
    CHECK(Utf32ToUtf8(line).find(Utf32ToUtf8(window.text)) != std::string::npos);
}

// ---------------------------------------------------------------------------
// UI-A(0.11.0):多行 composer。BeginLine(true) 开 composer 模式(只有主提示
// 符走这一档);默认 BeginLine() 保持单行语义,上面全部老用例原样跑。
// ---------------------------------------------------------------------------

TEST_CASE("多行 composer: NewLine 在光标处插换行,劈开当前行") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "abc");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Left));  // 光标落在 b、c 之间
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    REQUIRE(state.lines.size() == 2);
    CHECK(state.lines[0] == U"ab");
    CHECK(state.lines[1] == U"c");
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 0);
    CHECK(state.line == U"ab\nc");   // 兼容字段:多行拼 '\n'
    CHECK(state.cursor == 3);        // 拼接串里 "ab\n" 之后
    CHECK_FALSE(state.submitted);    // 插换行不是提交
}

TEST_CASE("多行 composer: Enter 全发,多行拼 \\n,历史记一条") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "one");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "two");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(state.submitted);
    CHECK(state.line == U"one\ntwo");
    REQUIRE(state.lines.size() == 2);
    CHECK(state.lines[0] == U"one");  // 提交帧画的是完整内容,终端层靠它留痕
    CHECK(state.lines[1] == U"two");
    CHECK(editor.history_size() == 1);
    CHECK(editor.CurrentRenderState().line.empty());  // 提交后缓冲清空
}

TEST_CASE("多行 composer: 空/全空白 composer 按 Enter 不发送,原地不动") {
    LineEditorCore editor;
    editor.BeginLine(true);
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK_FALSE(state.submitted);
    CHECK(editor.history_size() == 0);

    // 空格、Tab 字符、换行凑一段全空白,照样不发。
    TypeString(editor, "  ");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    editor.HandleKey(KeyEvent::Char(U'\t'));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK_FALSE(state.submitted);
    CHECK(state.line == U"  \n\t");  // 内容原样在,没被清、没被发
    CHECK(editor.history_size() == 0);
}

TEST_CASE("非 composer 模式: NewLine 等同 Enter,空行照样提交(确认提示语义不回归)") {
    LineEditorCore editor;
    editor.BeginLine();  // 默认单行模式:确认提示 [y/a/N]、/model 编号选择走这档
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    CHECK(state.submitted);       // Shift+Enter 在单行读取里就是提交
    CHECK(state.line.empty());    // 空行提交(确认提示靠这个当"默认拒绝/默认选项")
    CHECK(editor.history_size() == 0);

    editor.BeginLine();
    TypeString(editor, "y");
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    CHECK(state.submitted);
    CHECK(state.line == U"y");
}

TEST_CASE("多行 composer: 左右键跨行边界") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "ab");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "cd");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Home));  // 光标 (1, 0)

    // 行首按左:落到上一行行尾。
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    CHECK(state.cursor_row == 0);
    CHECK(state.cursor_col == 2);

    // 行尾按右:落到下一行行首。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Right));
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 0);

    // 整个 composer 开头按左:不动。
    editor.HandleKey(KeyEvent::Simple(KeyKind::Up));    // 回第一行(列钳到 0)
    editor.HandleKey(KeyEvent::Simple(KeyKind::Home));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    CHECK(state.cursor_row == 0);
    CHECK(state.cursor_col == 0);
}

TEST_CASE("多行 composer: 行首退格把这一行并进上一行") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "ab");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "cd");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Home));  // (1, 0)
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    REQUIRE(state.lines.size() == 1);
    CHECK(state.lines[0] == U"abcd");
    CHECK(state.cursor_row == 0);
    CHECK(state.cursor_col == 2);  // 光标落在缝合点

    // 整个 composer 开头退格:什么都不发生。
    editor.HandleKey(KeyEvent::Simple(KeyKind::Home));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(state.lines[0] == U"abcd");
    CHECK(state.cursor_col == 0);
}

TEST_CASE("多行 composer: 上下键行间移动,列钳到目标行长度") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "abcdef");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "xy");
    // 光标 (1, 2);Up 回第一行,列保持 2。
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.cursor_row == 0);
    CHECK(state.cursor_col == 2);
    // End 到第一行行尾(6),Down 到第二行,列钳到 2。
    editor.HandleKey(KeyEvent::Simple(KeyKind::End));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 2);
}

TEST_CASE("多行 composer: 第一行再按上才翻历史,翻回底部还原多行草稿") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "old");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));  // 历史:{"old"}

    editor.BeginLine(true);
    TypeString(editor, "l1");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "l2");  // 两行草稿,光标 (1, 2)

    // 第一下 Up:只是行间移动,不翻历史。
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.cursor_row == 0);
    CHECK(state.line == U"l1\nl2");

    // 已在第一行,再 Up:翻历史,载入 "old"。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.line == U"old");
    REQUIRE(state.lines.size() == 1);

    // Down 翻回底部:多行草稿完整还原。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.line == U"l1\nl2");
    REQUIRE(state.lines.size() == 2);
    CHECK(state.lines[0] == U"l1");
    CHECK(state.lines[1] == U"l2");
}

TEST_CASE("多行 composer: 多行历史条目往返载入,行结构和光标都对") {
    LineEditorCore editor;
    editor.BeginLine(true);
    for (char32_t c : std::u32string(U"第一行")) {
        editor.HandleKey(KeyEvent::Char(c));
    }
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    for (char32_t c : std::u32string(U"第二行")) {
        editor.HandleKey(KeyEvent::Char(c));
    }
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(editor.history_size() == 1);

    editor.BeginLine(true);
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    REQUIRE(state.lines.size() == 2);
    CHECK(state.lines[0] == U"第一行");
    CHECK(state.lines[1] == U"第二行");
    CHECK(state.cursor_row == 1);                 // 载入后光标在末行末尾
    CHECK(state.cursor_col == 3);
    CHECK(state.cursor_display_col == 6);         // 三个汉字,每个两列
    CHECK(state.line == U"第一行\n第二行");

    // 原样再提交:历史又记一条,内容一致。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(state.submitted);
    CHECK(state.line == U"第一行\n第二行");
    CHECK(editor.history_size() == 2);
}

TEST_CASE("多行 composer: 多行时首行的 / 是正文,不出提示、Tab 不补全") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/he");
    CHECK_FALSE(editor.CurrentRenderState().hint_lines.empty());  // 单行时提示照旧

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    CHECK(state.hint_lines.empty());  // 一变多行,提示区立刻消失

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"/he\n");    // Tab 不补全,内容原样
    CHECK(state.hint_lines.empty());

    // 退格并回单行,提示区回来。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(state.line == U"/he");
    CHECK_FALSE(state.hint_lines.empty());
}

TEST_CASE("多行 composer: Ctrl+C 非空清空整个 composer,空则请求 EOF") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "aa");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "bb");
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlC));
    CHECK(state.cleared);
    CHECK_FALSE(state.eof_requested);
    REQUIRE(state.lines.size() == 1);  // 所有行一并清,不是只清当前行
    CHECK(state.lines[0].empty());

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlC));  // 已空,再按当 EOF
    CHECK(state.eof_requested);
}

TEST_CASE("多行 composer: Esc 清空整个 composer,esc_pressed 置位") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "aa");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "bb");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(state.esc_pressed);
    CHECK(state.cleared);
    CHECK(state.line.empty());
    CHECK_FALSE(state.eof_requested);
}

TEST_CASE("多行 composer: Home/End 作用于当前行,不是整个 composer") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "abc");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "de");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Left));  // (1, 1)
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Home));
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 0);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::End));
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 2);
}

TEST_CASE("emoji: 代理对合成的码点按宽字符算,编辑不劈半") {
    CHECK(CharDisplayWidth(U'😀') == 2);       // U+1F600,终端层由代理对拼出的码点
    CHECK(CharDisplayWidth(U'🚀') == 2);       // U+1F680
    CHECK(CharDisplayWidth(U'🧠') == 2);       // U+1F9E0
    CHECK(CharDisplayWidth(U'🪐') == 2);       // U+1FA90
    CHECK(DisplayWidth(U"a😀中") == 5);

    LineEditorCore editor;
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Char(U'中'));
    editor.HandleKey(KeyEvent::Char(char32_t{0x1F600}));
    RenderState state = editor.CurrentRenderState();
    CHECK(state.cursor == 2);              // 按码点算两个字符
    CHECK(state.cursor_display_col == 4);  // 显示宽度 2 + 2

    // 退格一下删掉整个 emoji(单个码点),不会劈出半个代理对。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(state.line == U"中");
    CHECK(state.cursor_display_col == 2);

    // 窗口截断也不切半个 emoji 宽度。
    CHECK(TruncateToDisplayWidth(U"😀😀", 3) == U"😀");
}

TEST_CASE("多行 composer: cursor 兼容字段按拼接串算,显示列按当前行算") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "ab");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    editor.HandleKey(KeyEvent::Char(U'中'));
    editor.HandleKey(KeyEvent::Char(U'c'));
    const RenderState state = editor.CurrentRenderState();
    CHECK(state.line == U"ab\n中c");
    CHECK(state.cursor == 5);              // "ab\n中c" 里末尾:2 + 1(\n) + 2
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 2);
    CHECK(state.cursor_display_col == 3);  // 当前行 "中c":2 + 1,跟第一行无关
}

TEST_CASE("ComputeEditLineWindow: 超宽时不切半个宽字符") {
    // 全是宽字符(每个占 2 列),content_width 给奇数,窗口不该出现半个字宽。
    const std::u32string line = U"一二三四五六七八九十";  // 10 个汉字,宽度各 2,总宽 20
    const EditLineWindow window = ComputeEditLineWindow(line, 5, 5);
    CHECK(DisplayWidth(window.text) <= 5);
    CHECK(DisplayWidth(window.text) % 2 == 0);  // 全宽字符窗口,显示宽度必是偶数,没有切半个字
}

// ---------------------------------------------------------------------------
// 0.16.0:slash 候选菜单 ↓↑ 直选(菜单选择态状态机)。
// ---------------------------------------------------------------------------

TEST_CASE("菜单直选: / 开头且候选非空,按 Down 进入选择态,选中第一条") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/c");  // 匹配 /config、/clear
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.selected_index == 0);
    CHECK(state.line == U"/c");  // 进菜单不改行内容
    REQUIRE(state.hint_lines.size() == 2);
    CHECK(state.hint_lines[0].rfind("> ", 0) == 0);  // 第一条标中
    CHECK(state.hint_lines[1].rfind("  ", 0) == 0);
}

TEST_CASE("菜单直选: Down/Up 循环移动,到底回头、到顶回尾") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/c");  // 两个候选
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));  // 进菜单,idx 0
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.selected_index == 1);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));  // 到底,循环回第一条
    CHECK(state.selected_index == 0);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));  // 到顶,循环到最后一条
    CHECK(state.selected_index == 1);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(state.selected_index == 0);
    // 全程行内容不动。
    CHECK(state.line == U"/c");
}

TEST_CASE("菜单直选: Enter 采纳选中命令并提交,整行换成命令名") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/mo");  // 只匹配 /model
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(state.submitted);
    CHECK(state.line == U"/model");
    CHECK(editor.history_size() == 1);
    CHECK(editor.CurrentRenderState().line.empty());  // 提交后缓冲清空
}

TEST_CASE("菜单直选: Enter 采纳时,用户已敲的参数尾巴原样拼接") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/mo abc");  // 命令词 /mo + 参数尾巴 " abc"
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(state.submitted);
    CHECK(state.line == U"/model abc");
}

TEST_CASE("菜单直选: 继续打字退出选择态,候选随新前缀刷新") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/c");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));  // 进菜单
    const RenderState state = editor.HandleKey(KeyEvent::Char(U'l'));
    CHECK(state.selected_index == -1);  // 选择态退了
    CHECK(state.line == U"/cl");        // 字符正常插入
    REQUIRE(state.hint_lines.size() == 1);  // 只剩 /clear 匹配
    CHECK(state.hint_lines[0].find("/clear") != std::string::npos);
    CHECK(state.hint_lines[0].rfind("  ", 0) == 0);  // 没有选中标记
}

TEST_CASE("菜单直选: 退格退出选择态,回普通编辑") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/cl");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(state.selected_index == -1);
    CHECK(state.line == U"/c");  // 退格正常删字符
    REQUIRE(state.hint_lines.size() == 2);  // /config、/clear 又都回来了
}

TEST_CASE("菜单直选: ESC 退出选择态但不清行(跟空闲态 ESC 清 composer 不同)") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/c");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(state.selected_index == -1);
    CHECK(state.line == U"/c");        // 行内容原封不动
    CHECK_FALSE(state.cleared);
    CHECK_FALSE(state.esc_pressed);
    CHECK_FALSE(state.hint_lines.empty());  // 候选提示还在,只是没了标记

    // 退出选择态之后再按 ESC,才是老的"清空输入"语义。
    const RenderState after = editor.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(after.cleared);
    CHECK(after.esc_pressed);
    CHECK(after.line.empty());
}

TEST_CASE("菜单直选: 选择态里按 Tab 退出菜单并走补全老路") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/mo");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"/model ");  // 唯一匹配,老规矩补全整名 + 空格
}

TEST_CASE("菜单直选: 非 slash 行按 Down 仍是翻历史,不进菜单") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "hello");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));  // 历史 {"hello"}
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Up));  // 翻到 "hello"
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.selected_index == -1);
    CHECK(state.line.empty());  // 翻回底部草稿(空),Down 的历史职责没回归
}

TEST_CASE("菜单直选: / 开头但候选为空,Down 不进菜单(仍是历史语义)") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/zzz");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.selected_index == -1);
    CHECK(state.line == U"/zzz");  // 在底部,Down 无历史可翻,原地不动
    CHECK(state.hint_lines.empty());
}

TEST_CASE("菜单直选: 多行 composer 里 / 是正文,Down 是行间移动不进菜单") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/he");
    editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
    TypeString(editor, "x");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Up));  // 光标回第一行
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.selected_index == -1);
    CHECK(state.cursor_row == 1);  // 单纯的行间移动
    CHECK(state.hint_lines.empty());
}

TEST_CASE("菜单直选: 候选超过 6 个时,选中项循环到窗口外,展示窗口跟着挪") {
    std::vector<CompletionCandidate> candidates;
    for (int i = 0; i < 8; ++i) {
        candidates.push_back(CompletionCandidate{"/a" + std::to_string(i), "说明"});
    }
    LineEditorCore editor(std::move(candidates));
    editor.BeginLine(true);
    TypeString(editor, "/a");
    editor.HandleKey(KeyEvent::Simple(KeyKind::Down));  // idx 0
    RenderState state{};
    for (int i = 0; i < 7; ++i) {
        state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));  // 一路到 idx 7
    }
    CHECK(state.selected_index == 7);
    // 窗口挪到 [2, 8),选中项 /a7 在最后一行、带 "> " 标记。
    REQUIRE(state.hint_lines.size() == 7);  // 6 行候选 + 1 行汇总
    CHECK(state.hint_lines[5].rfind("> ", 0) == 0);
    CHECK(state.hint_lines[5].find("/a7") != std::string::npos);
    CHECK(state.hint_lines[6].find("8") != std::string::npos);  // 汇总行还在
    // 再 Down 一下循环回第一条,窗口也回到头。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(state.selected_index == 0);
    CHECK(state.hint_lines[0].rfind("> ", 0) == 0);
    CHECK(state.hint_lines[0].find("/a0") != std::string::npos);
}

// ---------------------------------------------------------------------------
// UI-D(0.16.0)/ 0.17.0 键位矫正:Ctrl+O / Ctrl+E / 焦点态的按键语义翻译。
// 0.17.0 改钉:Shift+Tab 任何时候都是切档(焦点态内除外);空 composer Tab
// 进入显式焦点态,态内 Tab/Shift+Tab 移动、ESC/Enter 退出。
// ---------------------------------------------------------------------------

TEST_CASE("0.17.0: 空 composer 的 Shift+Tab 是切档,不是焦点导航") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.focus_move == 0);
    CHECK(state.mode_changed);
    CHECK(editor.confirm_mode() == ConfirmMode::Auto);
    CHECK_FALSE(state.focus_active);
}

TEST_CASE("0.17.0: 空 composer Tab 进入焦点态并请求选最近条目") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.focus_move == 1);
    CHECK(state.focus_active);
    CHECK(state.line.empty());  // 行内容不动
    CHECK(editor.focus_mode());
}

TEST_CASE("0.17.0: 焦点态内 Tab 往旧走、Shift+Tab 往新走且不切档") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));  // 进焦点态

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.focus_move == 1);
    CHECK(state.focus_active);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.focus_move == -1);
    CHECK_FALSE(state.mode_changed);  // 焦点态内 Shift+Tab 是方向键,不切档
    CHECK(editor.confirm_mode() == ConfirmMode::Confirm);
    CHECK(state.focus_active);
}

TEST_CASE("0.17.0: 焦点态 ESC 退出回编辑,不清行不转发") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK_FALSE(state.focus_active);
    CHECK_FALSE(state.cleared);
    CHECK_FALSE(state.esc_pressed);  // 不当"清行 ESC"转发给终端层
    CHECK_FALSE(editor.focus_mode());

    // 退出焦点态之后,Shift+Tab 恢复切档本职。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.mode_changed);
    CHECK(state.focus_move == 0);
}

TEST_CASE("0.17.0: 焦点态 Enter 退出回编辑,不提交") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));

    const std::size_t history_before = editor.history_size();
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK_FALSE(state.submitted);
    CHECK_FALSE(state.focus_active);
    CHECK(editor.history_size() == history_before);
}

TEST_CASE("0.17.0: 焦点态里打字直接退出焦点态并落进 composer") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));

    RenderState state = editor.HandleKey(KeyEvent::Char(U'a'));
    CHECK_FALSE(state.focus_active);
    CHECK(state.line == U"a");
}

TEST_CASE("0.17.0: 焦点态里 Ctrl+E 照旧转发聚焦查看请求,不退出焦点态") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlE));
    CHECK(state.focus_view_requested);
    CHECK(state.focus_active);
}

TEST_CASE("0.17.0: ExitFocusMode 由终端层在焦点请求没被消费时调,退回编辑态") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(editor.focus_mode());
    editor.ExitFocusMode();
    CHECK_FALSE(editor.focus_mode());
    // 退掉之后 Shift+Tab 立刻恢复切档。
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.mode_changed);
}

TEST_CASE("0.17.0: BeginLine 清掉焦点态,不跨读取残留") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(editor.focus_mode());
    editor.BeginLine(true);
    CHECK_FALSE(editor.focus_mode());
}

TEST_CASE("UI-D: composer 有内容时 Tab/Shift+Tab 维持补全/切档现职") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine(true);
    TypeString(editor, "/he");
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.focus_move == 0);
    CHECK(state.line == U"/help ");  // 还是补全
    CHECK_FALSE(state.focus_active);

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.focus_move == 0);
    CHECK(state.mode_changed);  // 还是切档
    CHECK(editor.confirm_mode() == ConfirmMode::Auto);
}

TEST_CASE("UI-D: 非 composer(确认提示等单行读取)Tab/Shift+Tab 语义不回归") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();  // 单行模式,空行
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.focus_move == 0);  // 不是焦点导航

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::ShiftTab));
    CHECK(state.focus_move == 0);
    CHECK(state.mode_changed);  // 空行也照样切档(升级前语义)
}

TEST_CASE("UI-D: Ctrl+O/Ctrl+E 只在 composer 模式下转发请求") {
    LineEditorCore editor;
    editor.BeginLine(true);
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlO));
    CHECK(state.toggle_expand_requested);
    CHECK_FALSE(state.focus_view_requested);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlE));
    CHECK(state.focus_view_requested);
    CHECK_FALSE(state.toggle_expand_requested);

    editor.BeginLine();  // 非 composer:两个键都无动作
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlO));
    CHECK_FALSE(state.toggle_expand_requested);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlE));
    CHECK_FALSE(state.focus_view_requested);
}

TEST_CASE("UI-D: Ctrl+O/Ctrl+E 不动行内容、不打断编辑") {
    LineEditorCore editor;
    editor.BeginLine(true);
    TypeString(editor, "abc");
    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlO));
    CHECK(state.line == U"abc");
    CHECK(state.cursor == 3);
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::CtrlE));
    CHECK(state.line == U"abc");
}

TEST_CASE("0.28.x LoadText:整段正文装进编辑区,光标落末尾,可继续完整编辑") {
    LineEditorCore editor;
    editor.BeginLine(true);
    // 多行正文:按 '\n' 拆行,光标落在末行末尾。
    editor.LoadText(U"第一行\n第二行");
    RenderState state = editor.CurrentRenderState();
    CHECK(state.line == U"第一行\n第二行");
    CHECK(state.lines.size() == 2);
    CHECK(state.cursor_row == 1);
    CHECK(state.cursor_col == 3);  // "第二行" 三个码点

    // 取回后是在真正的编辑器里:左右挪光标、中间插字、行首退格并线,全部
    // 与手敲的 composer 一个待遇。
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Left));
    state = editor.HandleKey(KeyEvent::Char(U'!'));
    CHECK(state.line == U"第一行\n第!二行");
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Home));
    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Backspace));  // 行首退格:并进上一行
    CHECK(state.line == U"第一行第!二行");
    CHECK(state.lines.size() == 1);

    // LoadText 顺带清掉历史浏览位:取回的不是历史,内容就是所载正文。
    editor.LoadText(U"再来一段");
    state = editor.CurrentRenderState();
    CHECK(state.line == U"再来一段");
    CHECK(state.cursor_col == 4);
}
