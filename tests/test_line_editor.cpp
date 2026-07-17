// LineEditorCore 是逐键输入编辑器的核心层,吃抽象 KeyEvent、吐 RenderState,
// 不碰任何 Win32 API / 真实控制台——这里全部喂 KeyEvent 序列断言
// RenderState,一步都不碰控制台,能在任何环境下跑。

#include <doctest/doctest.h>

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

TEST_CASE("ConfirmModePromptPrefix: 三档对应三种提示符前缀") {
    CHECK(ConfirmModePromptPrefix(ConfirmMode::Confirm).empty());
    CHECK(ConfirmModePromptPrefix(ConfirmMode::Auto) == "[auto] ");
    CHECK(ConfirmModePromptPrefix(ConfirmMode::Yolo) == "[yolo] ");
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

TEST_CASE("ComputeEditLineWindow: 超宽时不切半个宽字符") {
    // 全是宽字符(每个占 2 列),content_width 给奇数,窗口不该出现半个字宽。
    const std::u32string line = U"一二三四五六七八九十";  // 10 个汉字,宽度各 2,总宽 20
    const EditLineWindow window = ComputeEditLineWindow(line, 5, 5);
    CHECK(DisplayWidth(window.text) <= 5);
    CHECK(DisplayWidth(window.text) % 2 == 0);  // 全宽字符窗口,显示宽度必是偶数,没有切半个字
}
