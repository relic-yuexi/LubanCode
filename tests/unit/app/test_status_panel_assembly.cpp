// 状态面板数据拼装(骨架拆解反弹·问题 2)单测:字段映射与条件段
// (工具后端档只在非 json 档挂、REC 空录制器收起、goal/loop 空指针空段)
// 原先铺在 TerminalSessionController::Run 的循环顶,现在填一份 inputs 就
// 能钉——不必起会话。
#include <doctest/doctest.h>

#include <optional>
#include <string>

#include "app/status_panel_assembly.hpp"
#include "cli/context_tracker.hpp"
#include "config/config.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

TEST_CASE("字段映射:model/provider/effort/context/cache 注记原样过") {
    lubancode::cli::ContextTracker tracker(1000);
    tracker.set_window_tokens(2000);  // 窗口改过,数字跟 tracker 走

    std::string model = "test-model";
    std::string provider = "test-provider";
    std::string effort = "medium";
    std::string ptc_text = "programmatic(auto)";

    lubancode::config::ConfigResult config_result;
    lubancode::app::StatusPanelInputs inputs;
    inputs.current_model = &model;
    inputs.active_provider = &provider;
    inputs.current_think = &effort;
    inputs.ptc_resolution = &ptc_text;
    inputs.context_tracker = &tracker;
    inputs.config_result = &config_result;

    const lubancode::cli::StatusPanelData data =
        lubancode::app::BuildStatusPanelData(inputs, lubancode::config::ToolCallingMode::Programmatic);
    CHECK(data.model == "test-model");
    CHECK(data.provider == "test-provider");  // env 没覆盖:照 active_provider
    CHECK(data.effort == "medium");
    CHECK(data.window_tokens == 2000);
    CHECK(data.cwd.empty() == false);  // 现场取,非空
    // json 默认档之外,工具后端段挂上。
    CHECK(data.tools == "programmatic(auto)");
    // 没配 goal/loop:状态段收起。
    CHECK(data.goal_loop.empty());
    CHECK(data.rec.empty());  // 没在录:REC 收起
    CHECK(data.plan_mode.empty());  // 非 Plan 档:plan 段收起
}

TEST_CASE("工具后端档:json 默认时留空(状态行零变化)") {
    std::string ptc_text = "programmatic(auto)";
    lubancode::app::StatusPanelInputs inputs;
    inputs.ptc_resolution = &ptc_text;
    const lubancode::cli::StatusPanelData data =
        lubancode::app::BuildStatusPanelData(inputs, lubancode::config::ToolCallingMode::Json);
    CHECK(data.tools.empty());
}

TEST_CASE("空录制器:REC 段收起,与没传等价") {
    const std::optional<lubancode::skills::WorkflowRecorder> no_recorder;
    lubancode::app::StatusPanelInputs with_recorder;
    with_recorder.recorder = &no_recorder;
    lubancode::app::StatusPanelInputs without_recorder;
    const lubancode::cli::StatusPanelData a =
        lubancode::app::BuildStatusPanelData(with_recorder, lubancode::config::ToolCallingMode::Json);
    const lubancode::cli::StatusPanelData b =
        lubancode::app::BuildStatusPanelData(without_recorder, lubancode::config::ToolCallingMode::Json);
    CHECK(a.rec == b.rec);
    CHECK(a.rec.empty());
}

}  // namespace
