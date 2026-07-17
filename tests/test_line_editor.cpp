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
}

TEST_CASE("LineEditorCore: Tab 补全,唯一匹配直接补全整名 + 空格") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/he");
    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line == U"/help ");
    CHECK(state.cursor == state.line.size());
}

TEST_CASE("LineEditorCore: Tab 补全,多个匹配先补公共前缀,再按 Tab 轮转候选") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/c");  // 匹配 /config、/clear,公共前缀就是 "/c" 本身

    RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    // 公共前缀跟当前输入一样长,没法再往前补,直接进入轮转,给出第一个候选。
    const bool first_is_config_or_clear = (state.line == U"/config ") || (state.line == U"/clear ");
    CHECK(first_is_config_or_clear);
    const std::u32string first_candidate = state.line;

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    CHECK(state.line != first_candidate);  // 轮转到另一个候选
    const bool second_is_config_or_clear = (state.line == U"/config ") || (state.line == U"/clear ");
    CHECK(second_is_config_or_clear);

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

    state = editor.HandleKey(KeyEvent::Simple(KeyKind::Tab));
    const bool is_first_candidate = (state.line == U"/foobar ") || (state.line == U"/foobaz ");
    CHECK(is_first_candidate);  // 再按一下 Tab,才真正开始轮转
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
    CHECK(state.hint_line.empty());
}

TEST_CASE("LineEditorCore: 行以 / 开头时,实时提示行列出匹配的命令") {
    LineEditorCore editor(SampleCandidates());
    editor.BeginLine();
    TypeString(editor, "/c");
    const RenderState state = editor.CurrentRenderState();
    CHECK_FALSE(state.hint_line.empty());
    CHECK(state.hint_line.find("/config") != std::string::npos);
    CHECK(state.hint_line.find("/clear") != std::string::npos);
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
