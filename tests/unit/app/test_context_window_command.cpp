// /context-window(ContextWindow交互面板单)的应用层钉子:面板确认后的
// 统一校验(ValidateContextWindowPanelSelection)——模型身份、两项候选
// 的新鲜度、关思考与 history all 的冲突。面板本体/候选生成/状态机在
// tests/unit/cli/test_context_window.cpp;TTY 宿主与真机交互归真机手测。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "api/reasoning.hpp"
#include "app/commands/session_commands.hpp"
#include "cli/context_window_panel.hpp"
#include "config/model_catalog.hpp"

using namespace lubancode;

namespace {

config::ModelCatalogEntry MakeReasoningEntry() {
    config::ModelCatalogEntry entry;
    entry.provider_id = "moonshot";
    entry.slug = "kimi";
    // 可选跨轮保留的方言:history_control=thinking_keep → RequestControl。
    entry.reasoning.dialect.history_control = "thinking_keep";
    entry.reasoning.supported_efforts = {"low", "medium", "high"};
    return entry;
}

app::ContextWindowPanelSelection Selection(bool window_changed, std::size_t window_tokens,
                                            bool effort_changed, const std::string& effort) {
    app::ContextWindowPanelSelection selection;
    selection.window_changed = window_changed;
    selection.window_tokens = window_tokens;
    selection.effort_changed = effort_changed;
    selection.effort_value = effort;
    return selection;
}

}  // namespace

TEST_CASE("ValidateContextWindowPanelSelection: 一切正常时放行") {
    const auto entry = MakeReasoningEntry();
    const auto window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    const auto effort = cli::ResolveThinkEffortCapability(&entry, {}, "high");
    const auto validation =
        app::ValidateContextWindowPanelSelection("moonshot", "kimi", "moonshot", "kimi", window, effort,
                                                 &entry, api::ReasoningHistoryMode::ProviderDefault,
                                                 Selection(true, std::size_t{400000}, true, "low"));
    CHECK(validation.ok);
    CHECK(validation.error.empty());
}

TEST_CASE("ValidateContextWindowPanelSelection: 无变更的选择照常放行(正常关闭)") {
    const auto entry = MakeReasoningEntry();
    const auto window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    const auto effort = cli::ResolveThinkEffortCapability(&entry, {}, "high");
    const auto validation = app::ValidateContextWindowPanelSelection(
        "moonshot", "kimi", "moonshot", "kimi", window, effort, &entry,
        api::ReasoningHistoryMode::ProviderDefault, Selection(false, 0, false, ""));
    CHECK(validation.ok);
}

TEST_CASE("ValidateContextWindowPanelSelection: 面板期间模型切换,拒绝写旧选项") {
    const auto entry = MakeReasoningEntry();
    const auto window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    const auto effort = cli::ResolveThinkEffortCapability(&entry, {}, "high");
    const auto validation = app::ValidateContextWindowPanelSelection(
        "moonshot", "kimi", "zhipu", "glm-5", window, effort, &entry,
        api::ReasoningHistoryMode::ProviderDefault, Selection(true, std::size_t{400000}, false, ""));
    CHECK_FALSE(validation.ok);
    CHECK(validation.error == "cmd.context_window.reject.model_changed");
    CHECK(validation.error_arg0 == "moonshot/kimi");  // 身份对给人看
    CHECK(validation.error_arg1 == "zhipu/glm-5");
}

TEST_CASE("ValidateContextWindowPanelSelection: 所选窗口不在最新候选,拒绝") {
    const auto entry = MakeReasoningEntry();
    // 上限 200K 的最新候选:400K 不在里面(面板打开期间目录变了)。
    const auto window = cli::BuildContextWindowCandidates(std::size_t{200000}, std::size_t{200000});
    const auto effort = cli::ResolveThinkEffortCapability(&entry, {}, "high");
    const auto validation =
        app::ValidateContextWindowPanelSelection("moonshot", "kimi", "moonshot", "kimi", window, effort,
                                                 &entry, api::ReasoningHistoryMode::ProviderDefault,
                                                 Selection(true, std::size_t{400000}, false, ""));
    CHECK_FALSE(validation.ok);
    CHECK(validation.error == "cmd.context_window.reject.window_stale");
}

TEST_CASE("ValidateContextWindowPanelSelection: 所选思考档不在最新候选或行不可调,拒绝") {
    const auto entry = MakeReasoningEntry();
    const auto window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    SUBCASE("档位被目录撤掉") {
        const auto effort = cli::ResolveThinkEffortCapability(&entry, {}, "high");
        const auto validation = app::ValidateContextWindowPanelSelection(
            "moonshot", "kimi", "moonshot", "kimi", window, effort, &entry,
            api::ReasoningHistoryMode::ProviderDefault, Selection(false, 0, true, "xhigh"));
        CHECK_FALSE(validation.ok);
        CHECK(validation.error == "cmd.context_window.reject.effort_stale");
    }
    SUBCASE("行变成不可调(目录改 declined)") {
        config::ModelCatalogEntry declined = MakeReasoningEntry();
        declined.reasoning.declined = true;
        const auto effort = cli::ResolveThinkEffortCapability(&declined, {}, "high");
        const auto validation = app::ValidateContextWindowPanelSelection(
            "moonshot", "kimi", "moonshot", "kimi", window, effort, &declined,
            api::ReasoningHistoryMode::ProviderDefault, Selection(false, 0, true, "low"));
        CHECK_FALSE(validation.ok);
        CHECK(validation.error == "cmd.context_window.reject.effort_stale");
    }
}

TEST_CASE("ValidateContextWindowPanelSelection: 关思考与 history all 冲突,拒绝(与 /think 同谓词)") {
    const auto window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    // RequestControl 方言 + 声明 none 档:Disabled 进候选,校验仍要拦
    // "关思考 + 跨轮保留"的组合。
    config::ModelCatalogEntry with_none = MakeReasoningEntry();
    with_none.supported_think_levels = {config::ThinkLevel{"none", ""}, config::ThinkLevel{"low", ""},
                                        config::ThinkLevel{"high", ""}};
    const auto effort = cli::ResolveThinkEffortCapability(&with_none, {}, "high");
    REQUIRE(cli::FindThinkEffortIndex(effort, "none") != static_cast<std::size_t>(-1));

    const auto validation =
        app::ValidateContextWindowPanelSelection("moonshot", "kimi", "moonshot", "kimi", window, effort,
                                                 &with_none, api::ReasoningHistoryMode::All,
                                                 Selection(false, 0, true, "none"));
    CHECK_FALSE(validation.ok);
    CHECK(validation.error == "cmd.context_window.reject.history_conflict");

    // history 在 default:关思考合法放行(同 /think 的边界)。
    const auto ok_validation =
        app::ValidateContextWindowPanelSelection("moonshot", "kimi", "moonshot", "kimi", window, effort,
                                                 &with_none, api::ReasoningHistoryMode::ProviderDefault,
                                                 Selection(false, 0, true, "none"));
    CHECK(ok_validation.ok);
}

TEST_CASE("ValidateContextWindowPanelSelection: ServerFixed 固定保留不与关思考冲突") {
    config::ModelCatalogEntry entry;  // 无方言:None;另造 ServerFixed
    entry.provider_id = "moonshot";
    entry.slug = "kimi-k3";
    entry.reasoning.dialect.replay = "always";  // replay=always 且无 history_control → ServerFixed
    entry.supported_think_levels = {config::ThinkLevel{"none", ""}, config::ThinkLevel{"low", ""}};
    const auto window = cli::BuildContextWindowCandidates(std::size_t{1000000}, std::size_t{200000});
    const auto effort = cli::ResolveThinkEffortCapability(&entry, {}, "low");
    REQUIRE(api::ReasoningHistorySupportFor(entry.reasoning) == api::ReasoningHistorySupport::ServerFixed);
    const auto validation =
        app::ValidateContextWindowPanelSelection("moonshot", "kimi-k3", "moonshot", "kimi-k3", window,
                                                 effort, &entry, api::ReasoningHistoryMode::All,
                                                 Selection(false, 0, true, "none"));
    CHECK(validation.ok);  // 服务端固定保留:wire 上没有请求字段,不算用户冲突
}
