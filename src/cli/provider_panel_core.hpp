// /provider 选择面板(Provider选择面板单):裸敲 /provider 或裸敲
// /provider switch 时的"挑一家"面板。两层,路数与 context_window_panel
// 同款:
//   - 纯逻辑(本头文件声明,provider_panel_core.cpp 实现,归
//     lubancode_core):条目生成(名字 + 当前端标记 + 鉴权缺失短标记)、
//     按键状态机(上下移动/Enter 确认/Esc 取消,长列表带滚动窗口)、
//     帧拼装。不碰终端、不碰配置文件,单测直接钉。
//   - TTY 宿主(provider_panel.cpp,归 lubancode_app):真终端下把帧画
//     出来、收方向键;非 TTY 不读键不挂起,退给调用方走老路。
//
// 面板只产"选了谁":确认后由 app 层(settings_commands 的
// HandleProviderCommand)把名字喂给既有 switch 执行路(鉴权预检/补救页/
// 热切换/横幅),不重写切换。带筛选词的旧选择器(cli/provider_switch)
// 照旧服务 /provider switch <拼错的名字> 与 /provider edit 两处入口,
// 两块面板各管各的门。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cli/line_editor.hpp"  // KeyEvent/KeyKind(状态机的输入)
#include "cli/theme.hpp"        // Theme(TTY 宿主的焦点行配色)
#include "config/config.hpp"    // ProviderConfig(条目生成的输入)

namespace lubancode::cli {

// 面板一屏默认显示的行数;超出进滚动窗口(上下方各有"还有几家"的省略
// 行)。与 SessionPicker 的默认可见行数同一档。
inline constexpr std::size_t kProviderPanelDefaultVisibleRows = 12;

// 面板一行的展示数据(纯数据,不含密钥明文)。鉴权只带"缺密钥"短标记:
// 齐备/无需鉴权不打标——面板是挑一家,不是鉴权体检表;细账归选中后的
// 补救页(provider_remedy)。
struct ProviderPanelEntry {
    std::string name;
    bool is_current = false;
    std::string auth_note;  // 空 = 无提示;非空 = 缺密钥短标记
};

// 把 providers 变成面板行(顺序照配置里的排列,当前端打标)。鉴权按
// ResolveProviderAuth 三态裁决:Missing 才给短标记,其余一律空。
std::vector<ProviderPanelEntry> BuildProviderPanelEntries(
    const std::vector<config::ProviderConfig>& providers, const std::string& active_provider);

// 在条目里按名字找下标(精确等值;空名与找不到都返回 npos)。进场光标
// 与补钥页返回列表时"刚才的选择仍在"都靠它定位。
std::size_t FindProviderPanelIndex(const std::vector<ProviderPanelEntry>& entries,
                                   const std::string& name);

// ---------------------------------------------------------------------------
// 按键状态机(纯逻辑,不碰终端)
// ---------------------------------------------------------------------------

// 长列表选择器:↑/↓(含 Tab/ShiftTab)移动高亮并首尾环绕,Home/End 跳
// 首尾,PageUp/PageDown 翻一屏;光标带着一个 visible_capacity 行的滚动
// 窗口走——光标出窗,窗口跟着挪(offset 是窗口首行下标,渲染用)。Enter
// 确认(空清单不算选中),Esc/Ctrl+C/Ctrl+D 取消(Ctrl+C 按面板取消处
// 理,不退程序)。submitted/cancelled 之后键一律落空。字符/粘贴不收:
// 这块面板只挑不筛,筛选归旧选择器(provider_switch)。
class ProviderPanelCore {
public:
    ProviderPanelCore(std::size_t entry_count, std::size_t visible_capacity, std::size_t start_index);

    struct State {
        std::size_t cursor = 0;  // 全列表下标
        std::size_t offset = 0;  // 滚动窗口首行下标(渲染用)
        bool submitted = false;
        bool cancelled = false;
    };

    const State& state() const { return state_; }
    const State& HandleKey(const KeyEvent& event);

private:
    // 光标出窗后把窗口拉回来(上出顶 offset 贴光标,下出底 offset 顶到
    // cursor - capacity + 1)。
    void ClampWindow();

    std::size_t entry_count_ = 0;
    std::size_t capacity_ = 1;  // 可见行数,0 按 1 算(窗口再小也留一行)
    State state_;
};

// ---------------------------------------------------------------------------
// 渲染(纯函数:面板状态 → 行组,不写配置、不切端)
// ---------------------------------------------------------------------------

// 一帧的全部材料(光标/窗口由 TTY 宿主每帧从 core 状态抄进来)。
// visible_capacity = 0 表示不限行数(整表画,无下方省略行)。
struct ProviderPanelView {
    std::vector<ProviderPanelEntry> entries;
    std::size_t cursor = 0;
    std::size_t offset = 0;
    std::size_t visible_capacity = 0;
};

struct ProviderPanelFrame {
    std::vector<std::string> lines;  // 逐行画;焦点行以 "> " 起头(TTY 层上色)
};

// 拼一帧。width 是终端列宽(窄终端按显示宽度截行)。布局:标题 + 当前端
// 行 + 空行 + 列表(窗口内逐行,高亮行 "> " 起头;窗口外的上下尾巴各给
// 一行"还有几家"的省略行)+ 空行 + footer。空清单给提示行,面板照画。
ProviderPanelFrame BuildProviderPanelFrame(const ProviderPanelView& view, int width);

// ---------------------------------------------------------------------------
// TTY 宿主(归 lubancode_app;非 TTY 不开面板)
// ---------------------------------------------------------------------------

struct ProviderPanelResult {
    bool opened = false;   // 面板真开了(非 TTY/终端不支持重画 = false)
    bool saved = false;    // Enter 提交(Esc/Ctrl+C/EOF = false)
    std::size_t index = 0;  // saved 时有效:entries 里的下标
};

// 开面板。view.visible_capacity 为 0 时按 kProviderPanelDefaultVisibleRows
// 落档。返回的下标按 view.entries 解读;确认后的应用(预检/补救/切换)
// 归调用方,这里一个配置都不碰。
ProviderPanelResult RunProviderPanel(const ProviderPanelView& view, const Theme& theme);

}  // namespace lubancode::cli
