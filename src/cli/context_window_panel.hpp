// /context-window 面板(ContextWindow交互面板单):同屏调当前模型的
// 上下文窗口预算与思考强度。分两层,路数与 provider_switch / session_picker
// 同款:
//   - 纯逻辑(本头文件声明,context_window_panel_core.cpp 实现,归
//     lubancode_core):窗口候选生成(§四合同)、思考档位裁决(§五合同,
//     集中在一个解析函数)、按键状态机、帧拼装。不碰终端、不碰配置,
//     单测直接钉。
//   - TTY 宿主(context_window_panel.cpp,归 lubancode_app):真终端下把
//     帧画出来、收方向键;非 TTY 不读键不挂起,退给调用方打短说明。
//
// 面板只产"草稿":键盘改的全是面板内的索引,不写配置、不调模型;确认后
// 由 app 层(session_commands 的 HandleSlashContextWindow)重新校验再走
// 既有会话设置入口(/context 的 tracker 窗口 + /think 的 current_think),
// 不另起第二套配置系统。
//
// 窗口是本地预算,不是服务端扩容:调到 1M 不代表端点真有 1M 能力——候选
// 生成按目录声明上限过滤,能力未知时保守只显当前值并标未验证(§四)。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "cli/line_editor.hpp"  // KeyEvent/KeyKind(状态机的输入)
#include "cli/theme.hpp"             // Theme(TTY 宿主的焦点行配色)
#include "config/model_catalog.hpp"  // ModelCatalogEntry(思考能力的声明源)

namespace lubancode::cli {

// ---------------------------------------------------------------------------
// 窗口候选(§四合同)
// ---------------------------------------------------------------------------

// 常用档位按十进制 token 计数(K=1000,M=1,000,000),与
// config::ParseContextWindowTokens 同一口径。百万以上另按 2M/4M/8M…扩展。
inline constexpr std::size_t kContextWindowCommonCandidates[] = {
    8000, 16000, 32000, 64000, 128000, 200000, 256000, 400000, 512000, 1000000,
};

// 窗口候选的裁决结果。values 升序去重,当前预算必在其中(哪怕超限——
// 照实显示标异常,不悄悄夹档);limit_known=false 时只有当前值,标未验证。
struct ContextWindowCandidates {
    std::vector<std::size_t> values;
    bool limit_known = false;          // 目录声明了模型上限
    std::size_t declared_limit = 0;    // 已知上限(limit_known 才有效)
    bool current_over_limit = false;   // 当前预算超已知上限(标异常)
    bool unverified = false;           // 能力未知:候选未经声明核实
    std::size_t current_window = 0;    // 进面板时的运行态预算(裁决时快照)
};

// 纯函数:声明上限(可空 = 未知)+ 运行态当前预算 → 候选列表。
// 规则:常用档按上限过滤;补声明上限本身(非标准上限如 128K 能显示自己)
// 与当前值;去重排序。当前值超限照实保留并标 current_over_limit。
ContextWindowCandidates BuildContextWindowCandidates(std::optional<std::size_t> declared_limit,
                                                     std::size_t current_window);

// 候选标签:200K / 400K / 1M / 128K;不整除 K/M 的真值(如 1048576)原样
// 显示数字,不硬折成 1M(§4.2 第 7 条)。
std::string FormatContextWindowLabel(std::size_t tokens);

// ---------------------------------------------------------------------------
// 思考档位(§五合同)
// ---------------------------------------------------------------------------

// 一档候选:展示值与请求值分开(§5.2)——label 给人看,value 发请求。
// value 空串 = "不发送参数"(Provider default 的正式状态);"none" = 关。
struct ThinkEffortOption {
    std::string value;
    std::string label;
    bool is_default = false;   // (Default) 标记:仅解析得明确默认时置真
    bool unverified = false;   // 声明源未核实(provider 配置档/当前值兜底)
};

// 档位行的可调性。Unknown 与 NotSupported 必须分开(§5.2):前者是什么都
// 没声明(不猜),后者是目录明说不吃推理参数。
enum class ThinkEffortControl {
    Adjustable,     // 有可候选的档位(声明档/开关两态/预算模型的两态)
    NotSupported,   // 目录 declined:推理参数一律不发,不可调
    AlwaysOn,       // 目录声明思考关不掉且无档位声明:始终开启,不可调
    Unknown,        // 目录外且 provider 未声明:能力未知,不猜档位
};

struct ThinkEffortCapability {
    ThinkEffortControl control = ThinkEffortControl::Unknown;
    // control==Adjustable 时非空且含当前值(不在声明表就追加为未验证档,
    // 不偷偷换档);NotSupported/AlwaysOn/Unknown 时为空,行按不可调画。
    std::vector<ThinkEffortOption> options;
    std::string current_effort;  // 进面板时的运行态 effort(空 = 未发送)
};

// 纯函数:目录条目(可空)+ provider 配置声明档 + 当前 effort → 档位裁决。
// 声明源优先级与 /think 裸敲一致:目录 supported_think_levels 与
// reasoning.supported_efforts 合并 → provider 配置 supported_think_levels
// (标未验证)→ 都没有 = Unknown。集中在此裁决,面板不自己拼两张平级表。
// Disabled 仅在声明可关时提供(none 在声明表 / supports_toggle / 方言有
// 开关);目录声明关不掉(always_think/off_unsupported)不提供。
ThinkEffortCapability ResolveThinkEffortCapability(const lubancode::config::ModelCatalogEntry* entry,
                                                   const std::vector<std::string>& provider_levels,
                                                   const std::string& current_effort);

// 档位展示名:low → Low,xhigh → Xhigh,none → Disabled(走 i18n),
// 空串 → Provider default(走 i18n),其余首字母大写。
std::string FormatThinkEffortLabel(const std::string& effort);

// 在候选里按值找下标(ASCII 大小写不敏感,与 ThinkLevelDeclared 同待遇);
// 找不到返回 npos。
std::size_t FindThinkEffortIndex(const ThinkEffortCapability& capability, const std::string& value);

// ---------------------------------------------------------------------------
// 面板状态机(纯逻辑,不碰终端)
// ---------------------------------------------------------------------------

// 两行(窗口/思考)左右调值的按键状态机。上下(含 Tab/ShiftTab)在两行
// 间切焦点;左右循环切当前行的候选索引(只有一个候选或该行不可调时不改
// 状态);Enter 提交,Esc/Ctrl+C/Ctrl+D 取消。submitted/cancelled 之后键
// 一律落空。
class ContextWindowPanelCore {
public:
    ContextWindowPanelCore(std::size_t window_count, std::size_t effort_count, std::size_t window_start,
                           std::size_t effort_start);

    struct State {
        std::size_t focus = 0;          // 0 = 窗口行,1 = 思考行
        std::size_t window_index = 0;
        std::size_t effort_index = 0;
        bool submitted = false;
        bool cancelled = false;
    };

    const State& state() const { return state_; }
    const State& HandleKey(const KeyEvent& event);

    // 草稿是否偏离进场值(Selected 行的 Unsaved 标记用)。
    bool dirty() const { return state_.window_index != window_start_ || state_.effort_index != effort_start_; }

private:
    std::size_t window_count_ = 0;
    std::size_t effort_count_ = 0;
    std::size_t window_start_ = 0;
    std::size_t effort_start_ = 0;
    State state_;
};

// ---------------------------------------------------------------------------
// 渲染(纯函数:面板状态 → 行组,不写配置、不调模型)
// ---------------------------------------------------------------------------

// 一帧的全部材料(索引/焦点由 TTY 宿主每帧从 core 状态抄进来)。
// original_* 是进场时的索引,Selected 行的 Unsaved 标记按它与当前索引比对。
struct ContextWindowPanelView {
    std::string model_title;  // 已拼好的标题行正文(如 "模型: Kimi(家/名)")
    ContextWindowCandidates window;
    ThinkEffortCapability effort;
    std::size_t window_index = 0;
    std::size_t effort_index = 0;
    std::size_t focus = 0;
    std::size_t original_window_index = 0;
    std::size_t original_effort_index = 0;
};

struct ContextWindowPanelFrame {
    std::vector<std::string> lines;  // 逐行画;焦点行以 "> " 起头(TTY 层上色)
};

// 拼一帧。width 是终端列宽(窄终端按显示宽度截行,长模型名自然被截)。
// 布局(§三):标题 + 空行 + 两段设置项(焦点标记/说明/< 值 >)+ Selected
// 行 + 两行操作说明。可调且有多个候选才画 "< >";不可调行保留说明并给
// 不可调缘由,不画暗示可切换的箭头。
ContextWindowPanelFrame BuildContextWindowPanelFrame(const ContextWindowPanelView& view, int width);

// ---------------------------------------------------------------------------
// TTY 宿主(归 lubancode_app;非 TTY 不开面板)
// ---------------------------------------------------------------------------

struct ContextWindowPanelResult {
    bool opened = false;   // 面板真开了(非 TTY/终端不支持重画 = false)
    bool saved = false;    // Enter 提交(Esc/Ctrl+C/EOF = false)
    std::size_t window_index = 0;
    std::size_t effort_index = 0;
};

// 开面板。返回的索引按 view.window.values / view.effort.options 解读;确认
// 后的应用(校验 + 写会话状态)归调用方,这里一个配置都不碰。
ContextWindowPanelResult RunContextWindowPanel(const ContextWindowPanelView& view, const Theme& theme);

}  // namespace lubancode::cli
