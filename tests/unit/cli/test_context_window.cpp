// /context-window 面板(ContextWindow交互面板单)的纯逻辑钉子:窗口候选
// (§四合同)、思考档位裁决(§五合同)、按键状态机与帧拼装。TTY 宿主
// (RunContextWindowPanel)要真终端,归真机手测;命令接线与应用校验在
// tests/unit/app/test_context_window_command.cpp。

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "cli/context_window_panel.hpp"
#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8(窄终端截行的宽度口径)
#include "config/config.hpp"
#include "config/model_catalog.hpp"

using namespace lubancode;

namespace {

config::ModelCatalogEntry MakeEntry() {
    config::ModelCatalogEntry entry;
    entry.provider_id = "prov";
    entry.slug = "model-x";
    return entry;
}

cli::KeyEvent Key(cli::KeyKind kind) {
    return cli::KeyEvent::Simple(kind);
}

// 在候选里按值找下标(测试侧的口径对照:ASCII 大小写不敏感)。
bool HasLevel(const cli::ThinkEffortCapability& capability, const std::string& value) {
    return cli::FindThinkEffortIndex(capability, value) != static_cast<std::size_t>(-1);
}

}  // namespace

// ---------------------------------------------------------------------------
// 窗口候选(§四合同 + §4.2 示例表)
// ---------------------------------------------------------------------------

TEST_CASE("BuildContextWindowCandidates: 常用档按已知上限过滤") {
    SUBCASE("1M 上限:小窗口档至十进制百万档") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
        CHECK(out.limit_known);
        CHECK(out.declared_limit == 1000000);
        CHECK(out.values == std::vector<std::size_t>{8000, 16000, 32000, 64000, 128000,
                                                     200000, 256000, 400000, 512000, 1000000});
        CHECK_FALSE(out.current_over_limit);
        CHECK_FALSE(out.unverified);
    }
    SUBCASE("400K 上限:不出现 512K 和 1M") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{400000}, std::size_t{200000});
        CHECK(out.values == std::vector<std::size_t>{8000, 16000, 32000, 64000, 128000,
                                                     200000, 256000, 400000});
    }
    SUBCASE("200K 上限:仍能选较低预算") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{200000}, std::size_t{200000});
        CHECK(out.values == std::vector<std::size_t>{8000, 16000, 32000, 64000, 128000, 200000});
    }
    SUBCASE("128K 上限:提供多个档位且不越界") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{128000}, std::size_t{128000});
        CHECK(out.values == std::vector<std::size_t>{8000, 16000, 32000, 64000, 128000});
    }
    SUBCASE("512K 上限:常用档包含声明值且去重") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{512000}, std::size_t{512000});
        CHECK(out.values == std::vector<std::size_t>{8000, 16000, 32000, 64000, 128000,
                                                     200000, 256000, 400000, 512000});
    }
    SUBCASE("1048576 上限:1M 档留有余量,原始上限不丢") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{1048576}, std::size_t{1048576});
        REQUIRE(out.values.size() == 11);
        CHECK(out.values[out.values.size() - 2] == 1000000);
        CHECK(out.values.back() == 1048576);
        CHECK(out.declared_limit == 1048576);
        CHECK(std::is_sorted(out.values.begin(), out.values.end()));
    }
    SUBCASE("大于百万的上限:自动延伸 2M/4M,补真实上限") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{5000000}, std::size_t{1000000});
        REQUIRE(out.values.size() == 13);
        CHECK(out.values[10] == 2000000);
        CHECK(out.values[11] == 4000000);
        CHECK(out.values.back() == 5000000);
    }
    SUBCASE("小于 8K 的模型:补半窗口,保留真实上限") {
        const auto out = cli::BuildContextWindowCandidates(std::size_t{4096}, std::size_t{4096});
        CHECK(out.values == std::vector<std::size_t>{2048, 4096});
    }
    SUBCASE("极大上限:倍增不溢出,候选数量有界") {
        const auto limit = std::numeric_limits<std::size_t>::max();
        const auto out = cli::BuildContextWindowCandidates(limit, std::size_t{1000000});
        REQUIRE_FALSE(out.values.empty());
        CHECK(out.values.back() == limit);
        CHECK(out.values.front() > 0);
        CHECK(out.values.size() <= std::numeric_limits<std::size_t>::digits + 11);
        CHECK(std::is_sorted(out.values.begin(), out.values.end()));
        CHECK(std::adjacent_find(out.values.begin(), out.values.end()) == out.values.end());
    }
}

TEST_CASE("BuildContextWindowCandidates: 能力未知保守展示,当前值必在候选") {
    const auto out = cli::BuildContextWindowCandidates(std::nullopt, std::size_t{300000});
    CHECK(out.unverified);
    CHECK_FALSE(out.limit_known);
    REQUIRE(out.values.size() == 1);
    CHECK(out.values[0] == 300000);  // 只有当前值,标未验证,不猜高档
}

TEST_CASE("BuildContextWindowCandidates: 当前值超限照实保留并标异常,不夹档") {
    // 用户上次调大到 300K,目录上限 200K:照实显示,标 current_over_limit;
    // 不悄悄夹到最近一档(§4.2 第 5 条)。
    const auto out = cli::BuildContextWindowCandidates(std::size_t{200000}, std::size_t{300000});
    CHECK(out.current_over_limit);
    REQUIRE(out.values.size() == 7);
    CHECK(out.values[out.values.size() - 2] == 200000);
    CHECK(out.values.back() == 300000);
}

TEST_CASE("BuildContextWindowCandidates: 非标准当前值补进候选") {
    const auto out = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{150000});
    REQUIRE(out.values.size() == 11);
    CHECK(out.values[5] == 150000);
    CHECK(out.values[6] == 200000);
    CHECK(out.values.back() == 1000000);
    CHECK_FALSE(out.current_over_limit);
}

TEST_CASE("BuildContextWindowCandidates: 零上限按未知处理,不生成零档") {
    const auto out = cli::BuildContextWindowCandidates(std::size_t{0}, std::size_t{256000});
    CHECK(out.unverified);
    CHECK_FALSE(out.limit_known);
    CHECK(out.values == std::vector<std::size_t>{256000});
}

TEST_CASE("BuildContextWindowCandidates: 十进制档位标签可由配置解析器无损读回") {
    const auto out = cli::BuildContextWindowCandidates(std::size_t{2097152}, std::size_t{1000000});
    for (const auto value : out.values) {
        const auto parsed = config::ParseContextWindowTokens(cli::FormatContextWindowLabel(value));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == value);
    }
    const auto million = config::ParseContextWindowTokens("1M");
    REQUIRE(million.has_value());
    CHECK(*million == 1000000);
    CHECK(cli::FormatContextWindowLabel(2000000) == "2M");
}

TEST_CASE("FormatContextWindowLabel: K=1000,M=1000000,真值不硬折") {
    CHECK(cli::FormatContextWindowLabel(200000) == "200K");
    CHECK(cli::FormatContextWindowLabel(400000) == "400K");
    CHECK(cli::FormatContextWindowLabel(1000000) == "1M");
    CHECK(cli::FormatContextWindowLabel(128000) == "128K");
    // 1048576 不是 1000 的整倍数:原样显示,不改写成 1M(§4.2 第 7 条)。
    CHECK(cli::FormatContextWindowLabel(1048576) == "1048576");
    CHECK(cli::FormatContextWindowLabel(105000) == "105K");
}

// ---------------------------------------------------------------------------
// 思考档位(§五合同)
// ---------------------------------------------------------------------------

TEST_CASE("ResolveThinkEffortCapability: 目录声明档 + 明确默认标 (Default)") {
    config::ModelCatalogEntry entry = MakeEntry();
    entry.default_think = "xhigh";
    entry.supported_think_levels = {
        config::ThinkLevel{"low", ""}, config::ThinkLevel{"medium", ""}, config::ThinkLevel{"high", ""},
        config::ThinkLevel{"xhigh", ""},
    };
    const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "high");
    CHECK(out.control == cli::ThinkEffortControl::Adjustable);
    REQUIRE(out.options.size() == 4);
    CHECK(out.options[0].value == "low");
    CHECK(out.options[0].label == "Low");
    CHECK_FALSE(out.options[0].is_default);
    CHECK(out.options[3].value == "xhigh");
    CHECK(out.options[3].label == "Xhigh");
    CHECK(out.options[3].is_default);  // 默认只在解析得明确默认时标(§5.3)
    CHECK(HasLevel(out, "high"));
}

TEST_CASE("ResolveThinkEffortCapability: 默认档不在声明表也补出,照标默认") {
    config::ModelCatalogEntry entry = MakeEntry();
    entry.default_think = "medium";
    entry.reasoning.supported_efforts = {"low", "high"};
    const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "low");
    REQUIRE(out.options.size() == 3);
    CHECK(out.options[0].value == "low");
    CHECK(out.options[1].value == "high");
    CHECK(out.options[2].value == "medium");
    CHECK(out.options[2].is_default);
}

TEST_CASE("ResolveThinkEffortCapability: reasoning.supported_efforts 与 levels 合并去重") {
    config::ModelCatalogEntry entry = MakeEntry();
    entry.supported_think_levels = {config::ThinkLevel{"low", ""}, config::ThinkLevel{"High", ""}};
    entry.reasoning.supported_efforts = {"high", "xhigh"};  // High/high 大小写不敏感去重
    const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "low");
    REQUIRE(out.options.size() == 3);
    CHECK(out.options[0].value == "low");
    CHECK(out.options[1].value == "High");
    CHECK(out.options[2].value == "xhigh");
}

TEST_CASE("ResolveThinkEffortCapability: Disabled 只在声明可关时提供") {
    SUBCASE("目录声明关不掉:不提供 Disabled") {
        config::ModelCatalogEntry entry = MakeEntry();
        entry.supported_think_levels = {
            config::ThinkLevel{"low", ""}, config::ThinkLevel{"medium", ""}, config::ThinkLevel{"high", ""},
        };
        entry.capabilities["always_think"] = true;
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "high");
        CHECK(out.control == cli::ThinkEffortControl::Adjustable);
        CHECK_FALSE(HasLevel(out, "none"));  // 明确关不掉的模型不给关(§5.2)
    }
    SUBCASE("supports_toggle 声明:提供 Disabled") {
        config::ModelCatalogEntry entry = MakeEntry();
        entry.reasoning.supports_toggle = true;
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "");
        CHECK(out.control == cli::ThinkEffortControl::Adjustable);
        CHECK(HasLevel(out, "none"));
    }
    SUBCASE("none 是声明档(GLM 型):有 Disabled,不伪造别的档") {
        config::ModelCatalogEntry entry = MakeEntry();
        entry.supported_think_levels = {
            config::ThinkLevel{"none", ""}, config::ThinkLevel{"minimal", ""}, config::ThinkLevel{"high", ""},
        };
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "high");
        REQUIRE(out.options.size() == 3);
        CHECK(out.options[0].value == "none");
        CHECK(out.options[0].label == cli::tr("cw_panel.disabled"));
        CHECK(out.options[1].value == "minimal");  // minimal 是真档,不当关闭(§5.2)
        CHECK(out.options[1].label == "Minimal");
    }
    SUBCASE("关不掉且无档位声明:AlwaysOn 不可调") {
        config::ModelCatalogEntry entry = MakeEntry();
        entry.capabilities["off_unsupported"] = true;
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "");
        CHECK(out.control == cli::ThinkEffortControl::AlwaysOn);
        CHECK(out.options.empty());
    }
}

TEST_CASE("ResolveThinkEffortCapability: declined 的模型不可调(与未知分开)") {
    config::ModelCatalogEntry entry = MakeEntry();
    entry.reasoning.declined = true;
    entry.supported_think_levels = {config::ThinkLevel{"low", ""}};
    const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "low");
    CHECK(out.control == cli::ThinkEffortControl::NotSupported);
    CHECK(out.options.empty());
    CHECK(out.current_effort == "low");
}

TEST_CASE("ResolveThinkEffortCapability: 目录外 + provider 声明档 = 候选标未验证") {
    const auto out = cli::ResolveThinkEffortCapability(nullptr, {"low", "high"}, "high");
    CHECK(out.control == cli::ThinkEffortControl::Adjustable);
    REQUIRE(out.options.size() == 2);
    CHECK(out.options[0].value == "low");
    CHECK(out.options[0].unverified);  // provider 声明不是目录核实,标未验证
    CHECK(out.options[1].value == "high");
    CHECK(out.options[1].unverified);
}

TEST_CASE("ResolveThinkEffortCapability: 目录外且无声明 = Unknown,不猜档位") {
    const auto out = cli::ResolveThinkEffortCapability(nullptr, {}, "medium");
    CHECK(out.control == cli::ThinkEffortControl::Unknown);
    CHECK(out.options.empty());  // 不把透传字符串标成真实支持(§5.2)
    CHECK(out.current_effort == "medium");
}

TEST_CASE("ResolveThinkEffortCapability: effort 空 = 未发送参数的正式状态") {
    SUBCASE("有声明档且当前非空:不硬塞 Provider default 第五档") {
        config::ModelCatalogEntry entry = MakeEntry();
        entry.supported_think_levels = {config::ThinkLevel{"low", ""}, config::ThinkLevel{"high", ""}};
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "low");
        CHECK_FALSE(HasLevel(out, ""));
        REQUIRE(out.options.size() == 2);
    }
    SUBCASE("当前为空:Provider default 在候选,不自动补成某档(§5.3)") {
        config::ModelCatalogEntry entry = MakeEntry();
        entry.supported_think_levels = {config::ThinkLevel{"low", ""}, config::ThinkLevel{"high", ""}};
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "");
        CHECK(HasLevel(out, ""));
        const std::size_t idx = cli::FindThinkEffortIndex(out, "");
        CHECK(out.options[idx].label == cli::tr("cw_panel.provider_default"));
    }
    SUBCASE("无档位的模型:Provider default 是诚实两态之一") {
        config::ModelCatalogEntry entry = MakeEntry();  // 有条目但什么都没声明
        const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "high");
        CHECK(out.control == cli::ThinkEffortControl::Adjustable);
        CHECK(HasLevel(out, ""));
        CHECK(HasLevel(out, "high"));
    }
}

TEST_CASE("ResolveThinkEffortCapability: 当前非标准档不偷偷换,追加为未验证") {
    config::ModelCatalogEntry entry = MakeEntry();
    entry.supported_think_levels = {config::ThinkLevel{"low", ""}, config::ThinkLevel{"high", ""}};
    const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "turbo");
    CHECK(out.control == cli::ThinkEffortControl::Adjustable);
    REQUIRE(out.options.size() == 3);
    CHECK(out.options[2].value == "turbo");
    CHECK(out.options[2].unverified);  // 当前值照实展示为未验证,不换档
}

TEST_CASE("ResolveThinkEffortCapability: 大小写不敏感找下标") {
    config::ModelCatalogEntry entry = MakeEntry();
    entry.supported_think_levels = {config::ThinkLevel{"high", ""}};
    const auto out = cli::ResolveThinkEffortCapability(&entry, {}, "high");
    CHECK(cli::FindThinkEffortIndex(out, "HIGH") == 0);
    CHECK(cli::FindThinkEffortIndex(out, "nope") == static_cast<std::size_t>(-1));
}

// ---------------------------------------------------------------------------
// 按键状态机(§三:上下切项、左右循环、单候选无动作)
// ---------------------------------------------------------------------------

TEST_CASE("ContextWindowPanelCore: 左右循环切值,环绕") {
    cli::ContextWindowPanelCore core(3, 2, 0, 0);
    CHECK(core.state().focus == 0);
    CHECK(core.state().window_index == 0);

    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.state().window_index == 1);
    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.state().window_index == 2);
    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.state().window_index == 0);  // 环绕

    core.HandleKey(Key(cli::KeyKind::Left));
    CHECK(core.state().window_index == 2);  // 反向环绕
}

TEST_CASE("ContextWindowPanelCore: 只有一个候选时左右不改状态") {
    cli::ContextWindowPanelCore core(1, 1, 0, 0);
    core.HandleKey(Key(cli::KeyKind::Right));
    core.HandleKey(Key(cli::KeyKind::Left));
    CHECK(core.state().window_index == 0);
    CHECK(core.state().effort_index == 0);
    CHECK_FALSE(core.dirty());
}

TEST_CASE("ContextWindowPanelCore: 上下在两行间切换(含 Tab),左右只动焦点行") {
    cli::ContextWindowPanelCore core(3, 3, 0, 0);
    core.HandleKey(Key(cli::KeyKind::Down));
    CHECK(core.state().focus == 1);
    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.state().effort_index == 1);
    CHECK(core.state().window_index == 0);  // 非焦点行不动
    core.HandleKey(Key(cli::KeyKind::Up));
    CHECK(core.state().focus == 0);
    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.state().window_index == 1);
    CHECK(core.state().effort_index == 1);

    core.HandleKey(Key(cli::KeyKind::Tab));
    CHECK(core.state().focus == 1);
    core.HandleKey(Key(cli::KeyKind::ShiftTab));
    CHECK(core.state().focus == 0);
    // 两行面板:Up 再按一次回到第二行(循环)。
    core.HandleKey(Key(cli::KeyKind::Up));
    CHECK(core.state().focus == 1);
}

TEST_CASE("ContextWindowPanelCore: Enter 提交,Esc/Ctrl+C/Ctrl+D 取消,提交后键落空") {
    SUBCASE("Enter") {
        cli::ContextWindowPanelCore core(2, 2, 0, 0);
        core.HandleKey(Key(cli::KeyKind::Enter));
        CHECK(core.state().submitted);
        CHECK_FALSE(core.state().cancelled);
        core.HandleKey(Key(cli::KeyKind::Right));  // 收场后键一律落空
        CHECK(core.state().window_index == 0);
    }
    SUBCASE("Esc / CtrlC / CtrlD 都当取消(Ctrl+C 不退程序,§二)") {
        for (const cli::KeyKind kind : {cli::KeyKind::Esc, cli::KeyKind::CtrlC, cli::KeyKind::CtrlD}) {
            cli::ContextWindowPanelCore core(2, 2, 0, 0);
            core.HandleKey(Key(kind));
            CHECK(core.state().cancelled);
            CHECK_FALSE(core.state().submitted);
        }
    }
}

TEST_CASE("ContextWindowPanelCore: dirty 只在草稿偏离进场值时为真") {
    cli::ContextWindowPanelCore core(3, 3, 1, 1);
    CHECK_FALSE(core.dirty());
    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.dirty());
    core.HandleKey(Key(cli::KeyKind::Left));  // 调回原值:不再算未保存
    CHECK_FALSE(core.dirty());
    core.HandleKey(Key(cli::KeyKind::Down));
    core.HandleKey(Key(cli::KeyKind::Right));
    CHECK(core.dirty());
}

// ---------------------------------------------------------------------------
// 帧拼装(§三布局;语言默认 zh-CN,英文词条成对性在 test_i18n 的机制下)
// ---------------------------------------------------------------------------

namespace {

cli::ContextWindowPanelView MakeView() {
    cli::ContextWindowPanelView view;
    view.model_title = cli::trf("cw_panel.model_title", std::string("prov/model-x"));
    view.window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    config::ModelCatalogEntry entry;
    entry.default_think = "xhigh";
    entry.supported_think_levels = {config::ThinkLevel{"low", ""}, config::ThinkLevel{"xhigh", ""}};
    view.effort = cli::ResolveThinkEffortCapability(&entry, {}, "xhigh");
    view.window_index = static_cast<std::size_t>(
        std::find(view.window.values.begin(), view.window.values.end(), 200000) - view.window.values.begin());
    view.effort_index = 1;
    view.focus = 0;
    view.original_window_index = view.window_index;
    view.original_effort_index = 1;
    return view;
}

}  // namespace

TEST_CASE("BuildContextWindowPanelFrame: §三布局逐行对账") {
    const cli::ContextWindowPanelView view = MakeView();
    const auto frame = cli::BuildContextWindowPanelFrame(view, 80);
    // 14 行:标题、空、窗口三项、空、思考三项、空、Selected、空、两行脚注。
    REQUIRE(frame.lines.size() == 14);
    // 标题行 + 空行
    CHECK(frame.lines[0] == cli::trf("cw_panel.model_title", std::string("prov/model-x")));
    CHECK(frame.lines[1].empty());
    // 焦点在窗口行:"> " 跟随焦点(§三),高亮配合文本标记不只靠颜色。
    CHECK(frame.lines[2].rfind("> ", 0) == 0);
    CHECK(frame.lines[2].find(cli::tr("cw_panel.context_label")) != std::string::npos);
    CHECK(frame.lines[3] == "  " + cli::trf("cw_panel.context_limit", std::string("1000000")));
    // 可调且有多个候选才画 "< >"。
    CHECK(frame.lines[4] == "  < 200K >");
    CHECK(frame.lines[5].empty());
    // 思考行无焦点:两个空格起头。
    CHECK(frame.lines[6].rfind("  ", 0) == 0);
    CHECK(frame.lines[6].find(cli::tr("cw_panel.effort_label")) != std::string::npos);
    CHECK(frame.lines[7] == "  " + cli::tr("cw_panel.effort_desc"));
    // 当前档 xhigh 且是目录默认:展示 "(默认)"(§5.3)。
    CHECK(frame.lines[8].find("Xhigh") != std::string::npos);
    CHECK(frame.lines[8].find(cli::tr("cw_panel.default_suffix")) != std::string::npos);
    CHECK(frame.lines[8].rfind("  < ", 0) == 0);
    CHECK(frame.lines[9].empty());
    // Selected 行:未操作时与进场值相同,不带 Unsaved。
    CHECK(frame.lines[10].find("200K") != std::string::npos);
    CHECK(frame.lines[10].find("Xhigh") != std::string::npos);
    CHECK(frame.lines[10].find(cli::tr("cw_panel.unsaved")) == std::string::npos);
    CHECK(frame.lines[11].empty());
    CHECK(frame.lines[12] == cli::tr("cw_panel.footer_nav"));
    CHECK(frame.lines[13] == cli::tr("cw_panel.footer_save"));
}

TEST_CASE("BuildContextWindowPanelFrame: 变更后 Selected 带 Unsaved;焦点换行") {
    cli::ContextWindowPanelView view = MakeView();
    // 按值定位,候选增减不改变本测试选择 400K 的意图。
    view.window_index = static_cast<std::size_t>(
        std::find(view.window.values.begin(), view.window.values.end(), 400000) - view.window.values.begin());
    const auto frame = cli::BuildContextWindowPanelFrame(view, 80);
    CHECK(frame.lines[4] == "  < 400K >");
    CHECK(frame.lines[10].find("400K") != std::string::npos);
    CHECK(frame.lines[10].find(cli::tr("cw_panel.unsaved")) != std::string::npos);

    // 焦点切到思考行:"> " 跟着走。
    view.focus = 1;
    const auto frame2 = cli::BuildContextWindowPanelFrame(view, 80);
    CHECK(frame2.lines[2].rfind("  ", 0) == 0);
    CHECK(frame2.lines[6].rfind("> ", 0) == 0);
}

TEST_CASE("BuildContextWindowPanelFrame: 未知能力不画箭头,附不可调说明") {
    cli::ContextWindowPanelView view = MakeView();
    // 窗口能力未知:单候选,不画 "< >",当前值标未验证。
    view.window = cli::BuildContextWindowCandidates(std::nullopt, std::size_t{300000});
    view.window_index = 0;
    view.original_window_index = 0;
    // 思考能力未知:行不可调,保留说明("Capabilities unknown" 一类)。
    view.effort.control = cli::ThinkEffortControl::Unknown;
    view.effort.options.clear();
    view.effort.current_effort = "medium";
    view.effort_index = 0;
    view.original_effort_index = 0;

    const auto frame = cli::BuildContextWindowPanelFrame(view, 80);
    CHECK(frame.lines[3] == "  " + cli::tr("cw_panel.context_desc"));
    CHECK(frame.lines[4].rfind("  < ", 0) != 0);  // 不画暗示可切换的箭头(§三)
    CHECK(frame.lines[4].find(cli::tr("cw_panel.unverified")) != std::string::npos);
    CHECK(frame.lines[8].find("Medium") != std::string::npos);
    CHECK(frame.lines[8].find(cli::tr("cw_panel.unknown_caps")) != std::string::npos);
    CHECK(frame.lines[8].rfind("  < ", 0) != 0);
}

TEST_CASE("BuildContextWindowPanelFrame: 当前值超限标异常;declined 标不可调") {
    cli::ContextWindowPanelView view = MakeView();
    view.window = cli::BuildContextWindowCandidates(std::size_t{200000}, std::size_t{300000});
    view.window_index = view.window.values.size() - 1;  // 300K(超限的当前值)
    view.original_window_index = view.window_index;
    {
        const auto frame = cli::BuildContextWindowPanelFrame(view, 80);
        // 300000 是 1000 的整倍数,标签是 "300K"(K=1000 口径),不是原文数字。
        CHECK(frame.lines[4].find("300K") != std::string::npos);
        CHECK(frame.lines[4].find(cli::tr("cw_panel.over_limit")) != std::string::npos);
    }
    view.effort.control = cli::ThinkEffortControl::NotSupported;
    view.effort.options.clear();
    view.effort.current_effort = "low";
    view.effort_index = 0;
    view.original_effort_index = 0;
    const auto frame = cli::BuildContextWindowPanelFrame(view, 80);
    CHECK(frame.lines[8].find(cli::tr("cw_panel.not_supported")) != std::string::npos);
}

TEST_CASE("BuildContextWindowPanelFrame: 原始上限与十进制 1M 分别展示") {
    auto view = MakeView();
    view.window = cli::BuildContextWindowCandidates(std::size_t{1048576}, std::size_t{1000000});
    view.window_index = static_cast<std::size_t>(
        std::find(view.window.values.begin(), view.window.values.end(), 1000000) - view.window.values.begin());
    view.original_window_index = view.window_index;
    const auto frame = cli::BuildContextWindowPanelFrame(view, 100);
    CHECK(frame.lines[3].find("1048576") != std::string::npos);
    CHECK(frame.lines[3].find("1M=1000000") != std::string::npos);
    CHECK(frame.lines[4] == "  < 1M >");
    CHECK(frame.lines[10].find("1M") != std::string::npos);
}

TEST_CASE("BuildContextWindowPanelFrame: 窄终端按显示宽度截行,不溢出") {
    const cli::ContextWindowPanelView view = MakeView();
    const auto frame = cli::BuildContextWindowPanelFrame(view, 20);
    for (const std::string& line : frame.lines) {
        // 按显示宽度量(CJK 占 2 列),不是字节数;帧宽 20 时 usable = 18。
        CHECK(cli::DisplayWidthUtf8(line) <= 18);
    }
}

TEST_CASE("BuildContextWindowPanelFrame: 英文词条成对(切 en 后布局仍齐)") {
    const std::string saved = cli::CurrentLanguage();
    cli::SetLanguage("en");
    cli::ContextWindowPanelView view = MakeView();
    const auto frame = cli::BuildContextWindowPanelFrame(view, 80);
    REQUIRE(frame.lines.size() == 14);
    CHECK(frame.lines[2].find("Context Window") != std::string::npos);
    CHECK(frame.lines[6].find("Thinking Effort") != std::string::npos);
    CHECK(frame.lines[10].find("Selected:") != std::string::npos);
    CHECK(frame.lines[12] == cli::tr("cw_panel.footer_nav"));
    CHECK(cli::tr("cw_panel.footer_nav").find("Left/Right") != std::string::npos);  // 文本回退
    cli::SetLanguage(saved);
}
