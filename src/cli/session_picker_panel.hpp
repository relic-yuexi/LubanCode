// SessionPicker 的 TTY 面板宿主(会话管理器单第二步):真终端下把纯逻辑
// 控制器(cli/session_picker.hpp)画出来——标题、搜索行、Filter/Sort、
// 列表、底栏,单帧重画、resize 重探。数据由调用方(app 层)从
// agent::SessionCatalog 摘好喂进来;这里不读盘、不知道存档在哪、没有
// delete。Enter 返回选中的 id,Esc 原路返回(std::nullopt),什么盘都不碰。
//
// 与 provider_switch 同一层路数:platform 原语(RawInputScope/KeyReader/
// SetCursorPos/ClearRowHardFrom)画帧,不手写转义序列;非 TTY(管道/
// 重定向)不开面板,调用方按老规矩打短用法。绘制归 lubancode_app 库
// (要用 console_input 的锁与 EnsureStreamScreenRowsLocked),所以宿主
// 在这文件、纯逻辑在 lubancode_core——cli 不反向依赖 agent 的老规矩
// 不破。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/line_editor.hpp"
#include "cli/session_picker.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

// 一场会话的喂料(相对时间文字由调用方算好——终端层才算相对时间,协议
// 层留稳定串;epoch 秒在这层换算刚好)。
struct SessionPickerFeed {
    std::vector<SessionPickerEntry> entries;  // 当前 scope/sort 下的一页(已排好序)
    std::size_t total = 0;                    // 命中总数(底栏百分比用)
    long long now_epoch = 0;                  // 相对时间的"现在"(测试可钉)
};

// 面板退出时把查询形状带出来:调用方(接线层)看见形状变了就重查
// catalog、带着新数据再进面板(选中项按 id 留住);没变就是正常退出。
struct SessionPickerPanelResult {
    std::optional<std::string> picked_id;  // Enter 选中的 id;取消/EOF 给空
    SessionPickerScope scope = SessionPickerScope::Cwd;  // 退出时的筛选
    SessionPickerSort sort = SessionPickerSort::Updated; // 退出时的排序
    std::string selected_id;  // 退出时选中的那场(重进面板按 id 留住选中用)
};

// 开面板。非 TTY(管道/重定向)给 picked_id 空、形状原样退回。空列表/
// 搜索无命中也照样开(各有画面)。prefer_id:重进面板时想守住的选中项
// (换筛选前选中的那场;不在新命中里就落到最近一行)。
SessionPickerPanelResult RunSessionPickerPanel(const SessionPickerFeed& feed, const Theme& theme,
                                               SessionPickerScope initial_scope,
                                               SessionPickerSort initial_sort,
                                               const std::string& prefer_id = std::string(),
                                               std::size_t visible_capacity = 12);

}  // namespace lubancode::cli
