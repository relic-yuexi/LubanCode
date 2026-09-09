// context_window_panel.hpp 的纯逻辑实现:窗口候选(§四)、思考档位裁决
// (§五)、按键状态机与帧拼装。不碰终端、不碰配置;TTY 宿主在
// context_window_panel.cpp(归 lubancode_app)。
#include "cli/context_window_panel.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth(窄终端截行)

namespace lubancode::cli {

namespace {

// ASCII 大小写不敏感的串等价(与 settings_commands 的 provider 档位比对、
// config::ThinkLevelDeclared 同一待遇:High/high 不算两档)。
bool AsciiIEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (x != y) {
            return false;
        }
    }
    return true;
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 窗口候选(§四合同)
// ---------------------------------------------------------------------------

ContextWindowCandidates BuildContextWindowCandidates(std::optional<std::size_t> declared_limit,
                                                     std::size_t current_window) {
    ContextWindowCandidates out;
    out.current_window = current_window;
    std::vector<std::size_t> values;
    // 0 不是有效的模型上限,按未知处理,不生成 0-token 候选。
    if (declared_limit.has_value() && *declared_limit > 0) {
        out.limit_known = true;
        out.declared_limit = *declared_limit;
        // 小窗口也提供较低预算,所有新增档位均受声明上限约束。
        for (const std::size_t common : kContextWindowCommonCandidates) {
            if (common <= *declared_limit) {
                values.push_back(common);
            }
        }
        // 大窗口自动延伸 2M/4M/8M…;先除再乘避免 size_t 溢出。
        for (std::size_t common = 1000000; common <= *declared_limit / 2;) {
            common *= 2;
            values.push_back(common);
        }
        // 小于最小常用档的旧模型仍可缩小预算;1 token 已无更小正数。
        if (values.empty() && *declared_limit > 1) {
            values.push_back(*declared_limit / 2);
        }
        // 保留厂商声明的原始整数,1048576 不改写为 1000000。
        values.push_back(*declared_limit);
        out.current_over_limit = current_window > *declared_limit;
    } else {
        // 能力未知(§4.2 第 6 条):保守展示,只有当前值,标未验证;不凭
        // 名字猜档位,不提供猜测的高档。
        out.unverified = true;
    }
    // 当前值必在候选(§4.2 第 4/5 条):超限照实保留(标异常),未改这
    // 项可维持既有预算。0 = 运行态没有可用预算(异常态),不硬塞进候选。
    if (current_window > 0) {
        values.push_back(current_window);
    }
    if (values.empty()) {
        // 声明缺失且无当前值:给最保守的一档(默认窗口口径),保面板能开。
        values.push_back(200000);
        out.unverified = true;
        out.limit_known = false;
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    out.values = std::move(values);
    return out;
}

std::string FormatContextWindowLabel(std::size_t tokens) {
    if (tokens >= 1000000 && tokens % 1000000 == 0) {
        return std::to_string(tokens / 1000000) + "M";
    }
    if (tokens >= 1000 && tokens % 1000 == 0) {
        return std::to_string(tokens / 1000) + "K";
    }
    // 1048576 这类真值原样显示,不折成 1M(§4.2 第 7 条)。
    return std::to_string(tokens);
}

// ---------------------------------------------------------------------------
// 思考档位(§五合同):集中裁决的唯一函数
// ---------------------------------------------------------------------------

std::string FormatThinkEffortLabel(const std::string& effort) {
    if (effort.empty()) {
        return tr("cw_panel.provider_default");
    }
    if (ToLowerAscii(effort) == "none") {
        return tr("cw_panel.disabled");
    }
    std::string label = effort;
    if (!label.empty() && label[0] >= 'a' && label[0] <= 'z') {
        label[0] = static_cast<char>(label[0] - 'a' + 'A');
    }
    return label;
}

namespace {

// 声明档的保序去重插入(ASCII 大小写不敏感)。
void PushUniqueLevel(std::vector<std::string>& levels, const std::string& level) {
    if (level.empty()) {
        return;  // 空串不是档位,是"未发送参数"状态,不进档位表
    }
    for (const std::string& existing : levels) {
        if (AsciiIEqual(existing, level)) {
            return;
        }
    }
    levels.push_back(level);
}

bool DeclaresLevel(const std::vector<std::string>& levels, const std::string& level) {
    for (const std::string& existing : levels) {
        if (AsciiIEqual(existing, level)) {
            return true;
        }
    }
    return false;
}

}  // namespace

ThinkEffortCapability ResolveThinkEffortCapability(const lubancode::config::ModelCatalogEntry* entry,
                                                   const std::vector<std::string>& provider_levels,
                                                   const std::string& current_effort) {
    ThinkEffortCapability out;
    out.current_effort = current_effort;

    // 目录明说不吃推理参数(declined,如只出图的模型):行不可调,说明
    // "Not supported"(§5.2:"未声明能力"和"不支持"必须分开——这里是不支持)。
    if (entry != nullptr && entry->reasoning.declined) {
        out.control = ThinkEffortControl::NotSupported;
        return out;
    }

    // 声明档(§5.1):目录条目的 supported_think_levels 与 reasoning.
    // supported_efforts 合并(两处同一件事的两种形状,保序去重);目录没有
    // 再看 provider 配置声明(Effort 诊断单的主路),标未验证。
    std::vector<std::string> levels;
    bool from_provider = false;
    if (entry != nullptr) {
        for (const auto& level : entry->supported_think_levels) {
            PushUniqueLevel(levels, level.effort);
        }
        for (const std::string& effort : entry->reasoning.supported_efforts) {
            PushUniqueLevel(levels, effort);
        }
    }
    if (levels.empty()) {
        for (const std::string& level : provider_levels) {
            PushUniqueLevel(levels, level);
        }
        from_provider = !levels.empty();
    }

    // Disabled(关思考)只在声明可关时提供(§5.2):none 在声明表里,或
    // reasoning 声明了开关(supports_toggle / 方言有开关形状)。目录声明
    // 关不掉(always_think/off_unsupported)的模型不给——哪怕档位可调,
    // 也不出现 Disabled 选项。不把 minimal、低预算或空串当关闭。
    bool off_allowed = false;
    if (entry != nullptr) {
        if (DeclaresLevel(levels, "none")) {
            off_allowed = true;
        }
        if (entry->reasoning.supports_toggle) {
            off_allowed = true;
        }
        if (!entry->reasoning.dialect.toggle.empty() && entry->reasoning.dialect.toggle != "none") {
            off_allowed = true;
        }
    }

    // 可调性裁决(§5.2):有声明档或声明了开关 = Adjustable;目录声明思考
    // 关不掉且没有任何档位 = AlwaysOn(行不可调,说明"始终开启");目录外
    // 且 provider 未声明 = Unknown(能力未知,不猜档位,行不可调)。
    if (!levels.empty() || off_allowed) {
        out.control = ThinkEffortControl::Adjustable;
    } else if (entry != nullptr &&
               lubancode::config::ClassifyThinkOffDeclaration(entry) ==
                   lubancode::config::ThinkOffDeclaration::DeclaredUnsupported) {
        out.control = ThinkEffortControl::AlwaysOn;
        return out;
    } else if (entry == nullptr && provider_levels.empty()) {
        out.control = ThinkEffortControl::Unknown;
        return out;
    } else {
        // 有条目、没档位、没开关:推理能力声明过但档位未声明(预算型等)。
        // 不新编档位与 token 映射(§5.2),按两态诚实展示:可关(若声明)
        // 已在上面判过,这里只剩"发当前值 / 不发参数"两态或单态。
        out.control = ThinkEffortControl::Adjustable;
    }

    // 候选组装。展示值与请求值分开(§5.2):value 发请求,label 给人看。
    const std::string default_think = entry != nullptr ? entry->default_think : std::string();
    for (const std::string& level : levels) {
        ThinkEffortOption option;
        option.value = level;
        option.label = FormatThinkEffortLabel(level);
        // (Default) 只在解析得明确默认时标(§5.3):不做全局假设。
        option.is_default = !default_think.empty() && AsciiIEqual(level, default_think);
        option.unverified = from_provider;
        out.options.push_back(std::move(option));
    }
    // 目录默认档不在声明表里也补出来(ComputeCatalogApplication 切模型时
    // 会应用它,是真实可落地的值)。
    if (!default_think.empty() && !DeclaresLevel(levels, default_think)) {
        ThinkEffortOption option;
        option.value = default_think;
        option.label = FormatThinkEffortLabel(default_think);
        option.is_default = true;
        option.unverified = from_provider;
        out.options.push_back(std::move(option));
    }
    // Disabled:none 已在声明表里时上面已加(标签走 Disabled);否则在
    // 声明了开关时补一枚。
    if (off_allowed && !DeclaresLevel(levels, "none")) {
        ThinkEffortOption option;
        option.value = "none";
        option.label = FormatThinkEffortLabel("none");
        option.unverified = from_provider;
        out.options.push_back(std::move(option));
    }
    // "不发送参数"(Provider default)是正式状态(§5.3):当前正空着,或
    // 模型没有任何声明档(它是该模型诚实的另一态)时纳入候选;有声明档
    // 且当前非空时不硬塞第五个选项。
    const bool empty_current = current_effort.empty();
    if (empty_current || levels.empty()) {
        ThinkEffortOption option;
        option.value = "";
        option.label = FormatThinkEffortLabel("");
        out.options.push_back(std::move(option));
    }
    // 当前值兜底(§5.2):不在候选里就追加为未验证档,照实展示不偷偷换档;
    // 用户改选时只在真实候选里挑,不把透传字符串标成真实支持。
    if (!empty_current) {
        bool present = false;
        for (const ThinkEffortOption& option : out.options) {
            if (AsciiIEqual(option.value, current_effort)) {
                present = true;
                break;
            }
        }
        if (!present) {
            ThinkEffortOption option;
            option.value = current_effort;
            option.label = FormatThinkEffortLabel(current_effort);
            option.unverified = true;
            out.options.push_back(std::move(option));
        }
    }
    return out;
}

std::size_t FindThinkEffortIndex(const ThinkEffortCapability& capability, const std::string& value) {
    for (std::size_t i = 0; i < capability.options.size(); ++i) {
        if (AsciiIEqual(capability.options[i].value, value)) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

// ---------------------------------------------------------------------------
// 面板状态机
// ---------------------------------------------------------------------------

ContextWindowPanelCore::ContextWindowPanelCore(std::size_t window_count, std::size_t effort_count,
                                               std::size_t window_start, std::size_t effort_start)
    : window_count_(window_count), effort_count_(effort_count), window_start_(window_start),
      effort_start_(effort_start) {
    state_.window_index = window_count_ > 0 ? (window_start < window_count_ ? window_start : 0) : 0;
    state_.effort_index = effort_count_ > 0 ? (effort_start < effort_count_ ? effort_start : 0) : 0;
}

const ContextWindowPanelCore::State& ContextWindowPanelCore::HandleKey(const KeyEvent& event) {
    if (state_.submitted || state_.cancelled) {
        return state_;
    }
    switch (event.kind) {
        case KeyKind::Up:
        case KeyKind::Down:
        case KeyKind::Tab:
        case KeyKind::ShiftTab:
            // 上下在两行间切换(本面板恒两行,循环切)。
            state_.focus = state_.focus == 0 ? 1u : 0u;
            break;
        case KeyKind::Left:
            // 左右循环切当前行的候选(§三:只有一个候选时不改变状态)。
            if (state_.focus == 0) {
                if (window_count_ > 1) {
                    state_.window_index =
                        state_.window_index == 0 ? window_count_ - 1 : state_.window_index - 1;
                }
            } else if (effort_count_ > 1) {
                state_.effort_index =
                    state_.effort_index == 0 ? effort_count_ - 1 : state_.effort_index - 1;
            }
            break;
        case KeyKind::Right:
            if (state_.focus == 0) {
                if (window_count_ > 1) {
                    state_.window_index = (state_.window_index + 1) % window_count_;
                }
            } else if (effort_count_ > 1) {
                state_.effort_index = (state_.effort_index + 1) % effort_count_;
            }
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
            state_.submitted = true;
            break;
        case KeyKind::Esc:
        case KeyKind::CtrlC:
        case KeyKind::CtrlD:
            // Ctrl+C 按面板取消处理(§二建议),不退程序。
            state_.cancelled = true;
            break;
        default:
            break;
    }
    return state_;
}

// ---------------------------------------------------------------------------
// 渲染(§三布局)
// ---------------------------------------------------------------------------

ContextWindowPanelFrame BuildContextWindowPanelFrame(const ContextWindowPanelView& view, int width) {
    ContextWindowPanelFrame frame;
    const int usable = width > 4 ? width - 2 : 40;
    const auto push = [&frame, usable](std::string line) {
        frame.lines.push_back(TruncateUtf8ToDisplayWidth(std::move(line), usable));
    };

    // 值行/注释行的拼装:可调且有多个候选画 "< X >",否则画 "X"(附注),
    // 不画暗示可切换的活动箭头(§三)。
    const auto window_value_text = [&]() {
        const std::size_t index = view.window_index < view.window.values.size() ? view.window_index : 0;
        std::string label = FormatContextWindowLabel(view.window.values[index]);
        if (view.window.current_over_limit &&
            view.window.values[index] == view.window.current_window) {
            label += " (" + std::string(tr("cw_panel.over_limit")) + ")";
        }
        if (view.window.unverified && view.window.values[index] == view.window.current_window) {
            label += " (" + std::string(tr("cw_panel.unverified")) + ")";
        }
        return label;
    };
    const bool window_cyclable = view.window.values.size() > 1;

    const auto effort_value_text = [&]() -> std::string {
        if (view.effort.options.empty()) {
            // 不可调行:仍展示当前值(或 Provider default 态),附不可调缘由。
            const std::string current = view.effort.current_effort.empty()
                                            ? FormatThinkEffortLabel("")
                                            : FormatThinkEffortLabel(view.effort.current_effort);
            std::string note;
            switch (view.effort.control) {
                case ThinkEffortControl::NotSupported:
                    note = tr("cw_panel.not_supported");
                    break;
                case ThinkEffortControl::AlwaysOn:
                    note = tr("cw_panel.always_on");
                    break;
                case ThinkEffortControl::Unknown:
                case ThinkEffortControl::Adjustable:
                    note = tr("cw_panel.unknown_caps");
                    break;
            }
            return current + "  --  " + note;
        }
        const std::size_t index =
            view.effort_index < view.effort.options.size() ? view.effort_index : 0;
        const ThinkEffortOption& option = view.effort.options[index];
        std::string label = option.label;
        if (option.is_default) {
            label += " " + std::string(tr("cw_panel.default_suffix"));
        }
        if (option.unverified) {
            label += " (" + std::string(tr("cw_panel.unverified")) + ")";
        }
        return label;
    };
    const bool effort_cyclable = view.effort.options.size() > 1;

    push(view.model_title);
    push("");
    push(std::string(view.focus == 0 ? "> " : "  ") + std::string(tr("cw_panel.context_label")));
    push(view.window.limit_known
             ? "  " + trf("cw_panel.context_limit", std::to_string(view.window.declared_limit))
             : "  " + std::string(tr("cw_panel.context_desc")));
    push(window_cyclable ? "  < " + window_value_text() + " >" : "  " + window_value_text());
    push("");
    push(std::string(view.focus == 1 ? "> " : "  ") + std::string(tr("cw_panel.effort_label")));
    push("  " + std::string(tr("cw_panel.effort_desc")));
    push(effort_cyclable ? "  < " + effort_value_text() + " >" : "  " + effort_value_text());
    push("");

    // Selected 行:未操作时与当前生效值相同;变更后是草稿,附 Unsaved
    // 提醒,不误称已生效(§三)。档位用平名(不带 (Default)/未验证尾巴,
    // 与需求示例 "Xhigh thinking" 同口径)。
    const auto effort_selected_text = [&]() {
        if (view.effort.options.empty()) {
            return view.effort.current_effort.empty() ? FormatThinkEffortLabel("")
                                                      : FormatThinkEffortLabel(view.effort.current_effort);
        }
        const std::size_t index =
            view.effort_index < view.effort.options.size() ? view.effort_index : 0;
        return view.effort.options[index].label;
    };
    {
        const std::size_t window_index =
            view.window_index < view.window.values.size() ? view.window_index : 0;
        std::string selected = trf("cw_panel.selected",
                                   FormatContextWindowLabel(view.window.values[window_index]),
                                   effort_selected_text());
        if (view.window_index != view.original_window_index ||
            view.effort_index != view.original_effort_index) {
            selected += "  [" + std::string(tr("cw_panel.unsaved")) + "]";
        }
        push(std::move(selected));
    }
    push("");
    push(tr("cw_panel.footer_nav"));
    push(tr("cw_panel.footer_save"));
    return frame;
}

}  // namespace lubancode::cli
