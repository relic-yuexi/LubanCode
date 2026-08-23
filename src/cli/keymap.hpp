// Action / Keymap 层(交互抛光总账第一步的"三层地基"之一):
// 把"按了什么键"与"要干什么事"拆开——语义动作(ActionId,如
// chat.search_history)是可测试的名词;键和弦(KeyChord,如 Ctrl+R)只是
// 皮;Keymap 把两者按作用域(KeyScope)接起来,支持改绑、冲突检查与
// 用户配置落盘。终端层(console_input)与面板/转录各处只查这层拿动作,
// 不再各自硬编码键位。
//
// 规矩(规格"总规矩"2/10):
//   - 动作名按语义取(chat.search_history / agent.stop 这类),VK_ 常量与
//     CSI 序列绝不漏到配置层——配置文件里只写动作名 + 和弦文本。
//   - 同一作用域内一枚和弦至多绑一个动作(冲突拒绝保存);不同作用域可
//     复用(Composer 的 Ctrl+R 与 Search 的 Ctrl+R 各归各)。
//   - 用户改绑只写用户级 ~/.lubancode/keymap.json,项目配置不许暗改全局
//     键位——这个模块压根不读项目目录,天然堵死。
//   - 核心编辑键(Enter 提交、Backspace、方向键光标移动)不进可改绑表:
//     它们是编辑器的一部分,不是语义动作;表里只收"可发现、可换皮"的
//     那批。既有安全键(Ctrl+C/Ctrl+D)同理不入表。
//
// 纯逻辑:不碰 Win32/POSIX/终端,可脱离控制台单测(tests/unit/cli/test_keymap.cpp)。
// platform::KeyInput -> KeyChord 的转换也收在这层(ChordFromKeyInput),
// platform 的语义按键枚举(KeyInput::Kind)是它唯一认的"物理世界"。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/console.hpp"

namespace lubancode::cli::keymap {

// ---------------------------------------------------------------------------
// 键和弦:物理键 + 修饰键
// ---------------------------------------------------------------------------

struct KeyChord {
    enum class Key : std::uint8_t {
        Char,      // ch 有效(可打印字符,按 Unicode 码点)
        Enter,
        Tab,
        ShiftTab,
        Esc,
        Backspace,
        Delete,
        Up,
        Down,
        Left,
        Right,
        Home,
        End,
        PageUp,
        PageDown,
    };
    Key key = Key::Char;
    char32_t ch = 0;  // key == Char 时有效
    bool ctrl = false;
    bool alt = false;
    bool shift = false;  // Char 的 shift 由大小写体现,一般不单设;Tab/方向键才用

    friend bool operator==(const KeyChord& a, const KeyChord& b) {
        return a.key == b.key && a.ch == b.ch && a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift;
    }
    friend bool operator!=(const KeyChord& a, const KeyChord& b) { return !(a == b); }
};

// 人读的写法:"Ctrl+R"、"Alt+V"、"Shift+Tab"、"?"、"{"。修饰键固定
// Ctrl/Alt/Shift 顺序,字母一律大写——同一枚和弦永远拼出同一串文本,
// /keymap 列表与 footer 提示共用,不做第二套格式。
std::string FormatKeyChord(const KeyChord& chord);

// 解析(大小写不敏感,各段可用 + 或 - 连接):"ctrl+r" / "Alt-V" / "?" /
// "shift+tab" / "pageup"。认不出给 nullopt。空白剥掉再拆。
std::optional<KeyChord> ParseKeyChord(std::string_view text);

// platform 语义按键 -> 和弦。Kind::None / Paste 给 nullopt(粘贴不是键);
// CtrlC/CtrlD 这类专枚举翻成带 ctrl 修饰的对应和弦,反查显示用。
std::optional<KeyChord> ChordFromKeyInput(const platform::KeyInput& input);

// ---------------------------------------------------------------------------
// 语义动作
// ---------------------------------------------------------------------------

enum class ActionId : std::uint16_t {
    None = 0,

    // ---- Composer 作用域(空闲主输入框) ----
    ChatSearchHistory,      // chat.search_history:打开提问历史反向搜索
    ChatHistoryPrev,        // chat.history_prev:上一条历史(明确别名,不受多行位置影响)
    ChatHistoryNext,        // chat.history_next:下一条历史
    ChatExternalEditor,     // chat.external_editor:草稿交外部编辑器
    ComposerStash,          // composer.stash:收起/取回草稿
    ImagePasteClipboard,    // image.paste_clipboard:剪贴板图片直贴
    HelpShow,               // help.show:当前场景按键帮助(空 composer 的 ?)

    // ---- 转录导航(空 composer 生效,不抢正文输入) ----
    TranscriptPrevUserTurn,  // transcript.prev_user_turn:{
    TranscriptNextUserTurn,  // transcript.next_user_turn:}
    TranscriptToScrollback,  // transcript.to_scrollback:[
    TranscriptViewInEditor,  // transcript.view_in_editor:v

    // ---- 搜索框作用域(Ctrl+R 打开后) ----
    SearchOlder,        // search.older:再按往更早一条
    SearchScopeCycle,   // search.scope_cycle:轮换 本会话/本项目/全部
    SearchAccept,       // search.accept:接受回 composer 继续改(Tab/Esc)
    SearchAcceptSubmit, // search.accept_submit:接受并直接提交(Enter)
    SearchCancel,       // search.cancel:取消并还原草稿(Ctrl+C)

    // ---- 面板作用域(子代理导航坞;正文非空时状态机自己放行) ----
    AgentNavUp,           // agent.nav_up
    AgentNavDown,         // agent.nav_down
    AgentView,            // agent.view:进/出查看态
    AgentBack,            // agent.back:逐层退出
    AgentStop,            // agent.stop:停止/清除当前条目
    AgentStopAllArm,      // agent.stop_all_arm:两段确认第一段
    AgentStopAllConfirm,  // agent.stop_all_confirm:两段确认第二段

    // ---- 既有固定键(入账可查可显,暂不可改绑;见 BindableAction 注释) ----
    TranscriptToggleExpand,  // transcript.toggle_expand:Ctrl+O 紧凑/详细
    TranscriptFocusView,     // transcript.focus_view:Ctrl+E 聚焦查看
    ScreenRedraw,            // screen.redraw:Ctrl+L 整屏重画
};

// 动作 <-> 配置层语义名(唯一转换口;两边都不许手抄第二份)。
const char* ActionName(ActionId action);
std::optional<ActionId> ActionFromName(std::string_view name);

// 作用域:一枚和弦在"哪个场景里"指哪个动作。查表必须带作用域——
// Ctrl+R 在 Composer 里是"打开搜索",在 Search 里是"往更早走"。
enum class KeyScope : std::uint8_t {
    Composer,  // 空闲主输入框(含空 composer 的转录导航键)
    Search,    // 历史搜索框开着的时候(整个键盘优先归它)
    Panel,     // 子代理导航坞(与 Composer 叠加,状态机裁夺谁吃键)
    Streaming, // 流式期间监听线程(Esc 打断/Shift+Tab 切档那批,暂只入账)
};

const char* ScopeName(KeyScope scope);

// ---------------------------------------------------------------------------
// 绑定表
// ---------------------------------------------------------------------------

struct BindingRecord {
    ActionId action = ActionId::None;
    KeyScope scope = KeyScope::Composer;
    KeyChord chord;         // 当前生效的和弦(默认或用户覆盖后)
    KeyChord default_chord;  // 出厂默认(复位用;无默认的动作这里是"空 Char")
    bool bindable = true;   // false = 固定键(Ctrl+O/E/L),只展示不改绑
    bool has_default = true;  // 无默认和弦的动作(stash 这类)为 false
};

// 是否可改绑:固定键(编辑器安全所需)返回 false。配置层拒绝写入这些。
bool BindableAction(ActionId action);

class Keymap {
public:
    Keymap();  // 出厂默认表

    // 查表:该作用域里这枚和弦当前绑的是哪个动作。
    ActionId Lookup(KeyScope scope, const KeyChord& chord) const;

    // 反查:这个动作当前绑哪枚和弦(供 footer/? 帮助从 keymap 取文案,
    // 用户改键后提示跟着改)。无绑定给 nullopt。
    std::optional<KeyChord> ChordFor(ActionId action) const;

    // 改绑(含冲突检查):同作用域已有别的动作占着这枚和弦就拒绝,
    // error 写明撞了谁。固定键 / 未知动作拒绝。成功后 ChordFor 立即变。
    bool SetBinding(ActionId action, KeyChord chord, std::string& error);

    // 复位单项(回出厂默认)。
    bool ResetBinding(ActionId action, std::string& error);

    // 全部绑定(含固定键),按 作用域/动作名 排序——/keymap 与 ? 帮助共用。
    std::vector<BindingRecord> AllBindings() const;

    // ---- 用户覆盖(独立文件 ~/.lubancode/keymap.json,平面 {"动作名":"和弦"}) ----
    // 解析(纯):JSON 文本 -> 动作名/和弦文本对;坏 JSON、顶层不是对象、
    // 值不是字符串的条目各记一条 error(不废整份)。
    static std::optional<std::vector<std::pair<std::string, std::string>>> ParseOverridesJson(
        const std::string& json_text, std::vector<std::string>& errors);

    // 应用一批覆盖(动作名 -> 和弦文本)。单条失败(名字不认得/和弦解析
    // 不动/冲突/固定键)只记 error 跳过,不废整份——坏配置回退该项默认
    // 并报具体项(规格"验收")。
    void ApplyOverrides(const std::vector<std::pair<std::string, std::string>>& overrides,
                        std::vector<std::string>& errors);

    // 序列化成 JSON 文本(只写与默认不同的项;没有覆盖给空串)。
    std::string SerializeOverrides() const;

private:
    struct Entry {
        ActionId action;
        KeyScope scope;
        KeyChord default_chord;
        bool bindable;
        bool has_default;
        KeyChord chord;  // 当前生效(= default 或覆盖)
    };
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// 进程级活动表:终端层/面板/帮助共用一份。启动时(cli_app)调
// LoadActiveKeymapOverrides 读用户文件;交互层改绑后(SetBinding)调
// SaveActiveKeymapOverrides 落盘。线程注意:只在主线程改(启动/命令处理),
// 读的两侧(空闲主线程、? 帮助)同在主线程;流式监听线程不查这张表
// (它的键位固定,只入账展示)。
// ---------------------------------------------------------------------------

Keymap& ActiveKeymap();

// 读 <dir>/keymap.json 应用覆盖(文件不存在 = 无覆盖,静默)。返回给
// 用户看的告警行(坏条目/坏 JSON),调用方决定打不打。
std::vector<std::string> LoadActiveKeymapOverrides(const std::string& user_lubancode_dir);

// 把活动表当前覆盖写回 <dir>/keymap.json。失败返回错误说明。
std::optional<std::string> SaveActiveKeymapOverrides(const std::string& user_lubancode_dir);

// 用户 keymap.json 的绝对路径(utf8);dir 为空给空串。
std::string KeymapOverridesPath(const std::string& user_lubancode_dir);

}  // namespace lubancode::cli::keymap
