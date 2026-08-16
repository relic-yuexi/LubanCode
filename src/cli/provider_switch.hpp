// /provider switch 的原地面板(向导重排单):TTY 下裸敲 switch 不再判 Invalid、
// 不倒总帮助,直接开一块"上下分隔线 + 列表 + 筛选行 + footer"的选择器。
// 数据(名字/默认模型/短地址/鉴权状态/当前标记)、过滤规则与按键状态机是
// 纯逻辑(可单测);终端绘制走 platform 原语,与 ReadChoiceMenu 同一层,
// 不手写转义序列。非 TTY 不开面板,由调用方给 switch 专用短用法。
//
// 选中后不在这里切——切不切、缺密钥怎么补救,归 app 层(settings_commands)
// 管;这里只负责"挑出一家"。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "cli/line_editor.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"

namespace lubancode::cli {

// 列表一行的展示数据(纯数据,不含密钥明文)。
struct ProviderSwitchEntry {
    std::string name;
    std::string model;        // 默认模型,空 = (未设置)
    std::string short_url;    // 短地址(剥协议头,不留长路径)
    std::string auth_label;   // 无需鉴权 / 可用 / 缺密钥(需要 X)/ 明文 key
    bool is_current = false;
};

// 把 providers 变成面板行。auth_label 按 ResolveProviderAuth 三态算:
// none → 无需鉴权;Ready → 可用;Missing → 缺密钥(需要 XXX)。绝不带明文。
std::vector<ProviderSwitchEntry> BuildProviderSwitchEntries(
    const std::vector<config::ProviderConfig>& providers, const std::string& active_provider);

// 纯过滤:按名字、模型、地址对 filter 做大小写不敏感的子串匹配,返回保留
// 的下标。filter 为空 = 全保留,顺序不变。
std::vector<std::size_t> FilterProviderSwitchEntries(const std::vector<ProviderSwitchEntry>& entries,
                                                     const std::string& filter);

// 挑选器的按键状态机(纯逻辑,不碰终端):字符进筛选词、Backspace 退一个、
// ↑/↓ 在过滤后的列表里移动、Enter 选中、Esc/Ctrl+C 取消。过滤词一变,光标
// 钳回首项。
class ProviderSwitchCore {
public:
    ProviderSwitchCore(std::size_t visible_count, std::size_t initial_cursor);

    struct State {
        std::string filter;
        std::size_t cursor = 0;    // 过滤后列表内的下标
        bool submitted = false;
        bool cancelled = false;
    };

    const State& state() const { return state_; }
    // visible_count 变化(过滤重算后)先调这个再继续喂键。
    void SetVisibleCount(std::size_t visible_count);
    // 初始筛选词(/provider switch <不存在的名字> 进来时带着)。
    void SetFilter(const std::string& filter) { state_.filter = filter; }
    const State& HandleKey(const KeyEvent& event);

private:
    std::size_t visible_count_ = 0;
    State state_;
};

// TTY 面板入口。providers 空 → 列表只剩"添加 provider / 取消"两项。
// start_filter:进来时就带的筛选词(/provider switch <不存在的名字> 找不到
// 时开着已筛选的列表);notice:贴在面板内的短提示(如"不存在: xxx");
// start_cursor_name:光标先停这个名字上(补钥页返回列表时"刚才的选择仍在"),
// 不在筛选结果里就退回当前 provider/首项。
// 返回:Named(带名字)/ AddNew(空列表或筛空时选了"添加")/ Cancelled。
enum class ProviderSwitchPick { Named, AddNew, Cancelled };
struct ProviderSwitchResult {
    ProviderSwitchPick pick = ProviderSwitchPick::Cancelled;
    std::string name;    // Named 时有效
    std::string filter;  // 退出面板时的筛选词(补钥页返回列表时"筛选词仍在")
};
ProviderSwitchResult RunProviderSwitchPicker(const std::vector<config::ProviderConfig>& providers,
                                              const std::string& active_provider,
                                              const std::string& start_filter, const std::string& notice,
                                              const std::string& start_cursor_name, const Theme& theme);

// 短地址:剥协议头、去结尾斜杠(127.0.0.1:8000/v1 留 /v1 一段,长路径不搬)。
std::string ShortenProviderUrl(const std::string& base_url);

}  // namespace lubancode::cli
