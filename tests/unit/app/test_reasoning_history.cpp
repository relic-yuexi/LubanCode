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

TEST_CASE("history default/all/off 对所有模型可选,关思考不拒绝选择") {
    using Mode = api::ReasoningHistoryMode;
    for (const auto& reasoning : {OptionalKeepReasoning(), ServerFixedReasoning(),
                                  NoPreservedReasoning(), api::ReasoningConfig{}}) {
        for (const auto mode : {Mode::ProviderDefault, Mode::All, Mode::Disabled}) {
            for (const std::string effort : {"", "high", "none"}) {
                const auto decision = app::DecideThinkHistorySwitch(Mode::All, mode, reasoning, effort);
                CHECK(decision.applied);
                CHECK(decision.mode == mode);
                CHECK_FALSE(decision.notes.empty());
            }
        }
    }
}

TEST_CASE("切模型与关闭思考不改用户历史回传选择") {
    using Mode = api::ReasoningHistoryMode;
    auto think = std::make_shared<std::string>("none");
    config::ModelCatalogEntry entry;
    entry.reasoning = NoPreservedReasoning();
    for (const auto mode : {Mode::ProviderDefault, Mode::All, Mode::Disabled}) {
        auto history = std::make_shared<Mode>(mode);
        CHECK_FALSE(app::RevalidateThinkHistoryMode(history, think, &entry));
        CHECK(*history == mode);
        CHECK_FALSE(app::RevalidateThinkHistoryMode(history, think, nullptr));
        CHECK(*history == mode);
    }
}
