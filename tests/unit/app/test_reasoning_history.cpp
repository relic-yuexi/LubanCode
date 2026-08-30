// /think history 的切换裁决与重校验(Kimi 保留式思考单 P1):/think 切换
// 与冲突明报各案钉在这——纯函数 DecideThinkHistorySwitch / Revalidate-
// ThinkHistoryMode 不碰终端,各案的落定档与明报语义在这里逐行对单子
// §七 P1 的六条。wire 侧的 keep 落线在 test_chat_request 与回环集成,
// 会话账的往返在 test_session_store。
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "api/reasoning.hpp"
#include "app/commands/settings_commands.hpp"
#include "config/model_catalog.hpp"

using namespace lubancode;

namespace {

// K2.6 形状:可选跨轮保留(thinking_keep),档位只当开关用。
api::ReasoningConfig OptionalKeepReasoning() {
    api::ReasoningConfig reasoning;
    reasoning.supports_toggle = true;
    reasoning.dialect.toggle = "thinking_type";
    reasoning.dialect.toggle_on = "enabled";
    reasoning.dialect.toggle_off = "disabled";
    reasoning.dialect.delta = "reasoning_content";
    reasoning.dialect.replay = "tool_episode";
    reasoning.dialect.history_control = "thinking_keep";
    reasoning.dialect.verified = true;
    return reasoning;
}

// K3/K2.7 形状:服务端固定保留,无请求字段。
api::ReasoningConfig ServerFixedReasoning() {
    api::ReasoningConfig reasoning;
    reasoning.supports_effort = true;
    reasoning.dialect.toggle = "none";
    reasoning.dialect.effort_path = "reasoning_effort";
    reasoning.dialect.replay = "always";
    reasoning.dialect.verified = true;
    return reasoning;
}

// K2.5 形状:不支持 Preserved Thinking。
api::ReasoningConfig NoPreservedReasoning() {
    api::ReasoningConfig reasoning;
    reasoning.supports_toggle = true;
    reasoning.dialect.toggle = "thinking_type";
    reasoning.dialect.toggle_on = "enabled";
    reasoning.dialect.toggle_off = "disabled";
    reasoning.dialect.replay = "never";
    reasoning.dialect.verified = true;
    return reasoning;
}

}  // namespace

TEST_CASE("能力档裁决:方言推导三档,不按模型名") {
    CHECK(api::ReasoningHistorySupportFor(OptionalKeepReasoning()) ==
          api::ReasoningHistorySupport::RequestControl);
    CHECK(api::ReasoningHistorySupportFor(ServerFixedReasoning()) ==
          api::ReasoningHistorySupport::ServerFixed);
    CHECK(api::ReasoningHistorySupportFor(NoPreservedReasoning()) ==
          api::ReasoningHistorySupport::None);
    CHECK(api::ReasoningHistorySupportFor(api::ReasoningConfig{}) ==
          api::ReasoningHistorySupport::None);  // 无方言旧 provider 不猜
}

TEST_CASE("/think history all 各案: K2.6 落账、冲突拒绝、K3 兜底、K2.5 拒绝") {
    using Mode = api::ReasoningHistoryMode;
    const auto k26 = OptionalKeepReasoning();

    // K2.6 + 开思考:落账 All,明报 keep/type 同发与 replay 升 always。
    const auto on = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::All, k26, "high");
    CHECK(on.applied);
    CHECK(on.mode == Mode::All);

    // K2.6 + 未设档位(思考默认开):照样落账。
    const auto unset = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::All, k26, "");
    CHECK(unset.applied);
    CHECK(unset.mode == Mode::All);

    // K2.6 + 思考已关:冲突,明报拒绝,不猜(P1 第 3 条)。
    const auto conflict = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::All, k26, "none");
    CHECK_FALSE(conflict.applied);
    CHECK(conflict.mode == Mode::ProviderDefault);  // 原档退回
    REQUIRE_FALSE(conflict.notes.empty());
    CHECK(conflict.notes.front().find("冲突") != std::string::npos);

    // K3/K2.7:服务端固定保留,选 all 不发字段但选择记档(切回可选模型生效)。
    const auto fixed = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::All,
                                                     ServerFixedReasoning(), "high");
    CHECK(fixed.applied);
    CHECK(fixed.mode == Mode::All);

    // K2.5:不支持 Preserved Thinking,当场拒绝(P1 第 5 条)。
    const auto refused = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::All,
                                                       NoPreservedReasoning(), "high");
    CHECK_FALSE(refused.applied);
    CHECK(refused.mode == Mode::ProviderDefault);
    REQUIRE_FALSE(refused.notes.empty());
    CHECK(refused.notes.front().find("不支持") != std::string::npos);

    // 无方言的自定义旧 provider:同 K2.5 待遇,不猜。
    const auto legacy = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::All,
                                                      api::ReasoningConfig{}, "high");
    CHECK_FALSE(legacy.applied);
}

TEST_CASE("/think history default 各案: 关得掉与关不掉都说清") {
    using Mode = api::ReasoningHistoryMode;
    const auto k26 = OptionalKeepReasoning();

    // K2.6 关回 default:落账,明报不再宣称跨轮保留、工具循环照旧。
    const auto off = app::DecideThinkHistorySwitch(Mode::All, Mode::ProviderDefault, k26, "high");
    CHECK(off.applied);
    CHECK(off.mode == Mode::ProviderDefault);
    REQUIRE_FALSE(off.notes.empty());
    CHECK(off.notes.front().find("tool_episode") != std::string::npos);

    // K3/K2.7 试图关:明报模型不支持关闭,回传仍 always(P1 第 4 条)。
    const auto fixed = app::DecideThinkHistorySwitch(Mode::All, Mode::ProviderDefault,
                                                     ServerFixedReasoning(), "high");
    CHECK(fixed.applied);
    CHECK(fixed.mode == Mode::ProviderDefault);
    REQUIRE_FALSE(fixed.notes.empty());
    CHECK(fixed.notes.front().find("不支持关闭") != std::string::npos);

    // K2.5 选 default:就是它的常态。
    const auto plain = app::DecideThinkHistorySwitch(Mode::ProviderDefault, Mode::ProviderDefault,
                                                     NoPreservedReasoning(), "high");
    CHECK(plain.applied);
    CHECK(plain.mode == Mode::ProviderDefault);
}

TEST_CASE("切模型重校验: 不支持的回落 default,合法的组合不动") {
    using Mode = api::ReasoningHistoryMode;
    auto history = std::make_shared<api::ReasoningHistoryMode>(Mode::All);
    auto think = std::make_shared<std::string>("high");

    config::ModelCatalog catalog;
    config::ModelCatalogEntry k26_entry;
    k26_entry.provider_id = "moonshot";
    k26_entry.slug = "kimi-k2.6";
    k26_entry.reasoning = OptionalKeepReasoning();
    config::ModelCatalogEntry k3_entry;
    k3_entry.provider_id = "moonshot";
    k3_entry.slug = "kimi-k3";
    k3_entry.reasoning = ServerFixedReasoning();
    config::ModelCatalogEntry k25_entry;
    k25_entry.provider_id = "moonshot";
    k25_entry.slug = "kimi-k2.5";
    k25_entry.reasoning = NoPreservedReasoning();
    catalog.models = {k26_entry, k3_entry, k25_entry};

    // K2.6(all) 切到 K3:固定开启,All 合法保留(不硬带 keep——wire 侧
    // 由方言守着),不回落。
    CHECK_FALSE(app::RevalidateThinkHistoryMode(history, think, catalog.FindBySlug("kimi-k3")));
    CHECK(*history == Mode::All);

    // K3(all) 切到 K2.5:不支持,回落 default。
    CHECK(app::RevalidateThinkHistoryMode(history, think, catalog.FindBySlug("kimi-k2.5")));
    CHECK(*history == Mode::ProviderDefault);

    // K2.5(default) 切回 K2.6 并重开 all,再把档位切 none(模拟目录默认
    // 关思考):重校验判冲突回落 default。
    *history = Mode::All;
    *think = "none";
    CHECK(app::RevalidateThinkHistoryMode(history, think, catalog.FindBySlug("kimi-k2.6")));
    CHECK(*history == Mode::ProviderDefault);

    // 合法的 K2.6(all + 开思考):不动。
    *history = Mode::All;
    *think = "high";
    CHECK_FALSE(app::RevalidateThinkHistoryMode(history, think, catalog.FindBySlug("kimi-k2.6")));
    CHECK(*history == Mode::All);

    // default 对任何模型都是合法状态:不回落、不刷屏。
    *history = Mode::ProviderDefault;
    CHECK_FALSE(app::RevalidateThinkHistoryMode(history, think, nullptr));  // 目录外模型
    CHECK(*history == Mode::ProviderDefault);
}
