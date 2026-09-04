// 审批档位边界合同(收口审计单 P1:四套审批枚举收口):
//   公共 ApprovalMode 是唯一业务值域;cli::ConfirmMode 只持显示状态,与公共
//   值域之间只许走 ToApprovalMode/ToConfirmMode 这一组具名桥;Runtime/Agent
//   直接吃公共枚举;持久化与协议边界一律按 machine name 解析。
//
// 表驱动:五档 × 各转换边界往返。表序刻意打乱(与 ApprovalMode、ConfirmMode
// 两枚枚举的声明序都不同)——映射只认名字与能力,不认声明次序;谁要是再拿
// static_cast 桥枚举,打乱声明序时这张表先红,不静默换档。
//
// 本册在收口前的旧代码上无法编译(具名桥与公共枚举直入的签名不存在),
// 即先红;实现落账后全绿。
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/permission_mode.hpp"
#include "api/types.hpp"
#include "approval_mode.hpp"
#include "cli/line_editor.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 五档表:顺序 = yolo, default, dont_ask, auto, accept_edits——刻意与两枚
// 枚举的声明序(Default/AcceptEdits/Yolo/Auto/DontAsk 与 Confirm/…)都不同。
struct ModeRow {
    const char* machine_name;
    ApprovalMode canonical;
    cli::ConfirmMode display;
};

const std::vector<ModeRow>& ModeTable() {
    static const std::vector<ModeRow> rows = {
        {"yolo", ApprovalMode::Yolo, cli::ConfirmMode::Yolo},
        {"default", ApprovalMode::Default, cli::ConfirmMode::Confirm},
        {"dont_ask", ApprovalMode::DontAsk, cli::ConfirmMode::DontAsk},
        {"auto", ApprovalMode::Auto, cli::ConfirmMode::Auto},
        {"accept_edits", ApprovalMode::AcceptEdits, cli::ConfirmMode::AcceptEdits},
    };
    return rows;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) machine name 往返:公共值域 <-> 稳定名 <-> CLI 显示档。
// ---------------------------------------------------------------------------

TEST_CASE("五档 machine name 往返:名字是唯一跨枚举通道,不认声明序") {
    for (const ModeRow& row : ModeTable()) {
        CAPTURE(row.machine_name);
        // 公共值域 <-> 稳定名(持久化/协议边界)。
        CHECK(std::string(ApprovalModeMachineName(row.canonical)) == row.machine_name);
        CHECK(ParseApprovalMode(row.machine_name) == row.canonical);
        // CLI 显示档 <-> 稳定名。
        CHECK(std::string(cli::ConfirmModeMachineName(row.display)) == row.machine_name);
        CHECK(cli::ParseConfirmMode(row.machine_name) == row.display);
        // CLI 显示档 <-> 公共值域:只走具名桥,往返闭合。
        CHECK(cli::ToApprovalMode(row.display) == row.canonical);
        CHECK(cli::ToConfirmMode(row.canonical) == row.display);
        // 再绕一整圈也不许漂。
        CHECK(cli::ToConfirmMode(cli::ToApprovalMode(row.display)) == row.display);
    }
    // 旧协议兼容别名:confirm 收正为 default。
    CHECK(ParseApprovalMode("confirm") == ApprovalMode::Default);
    CHECK(cli::ParseConfirmMode("confirm") == cli::ConfirmMode::Confirm);
    // 未知值:严格解析不认,宽容解析保守退 default。
    CHECK_FALSE(ParseApprovalMode("ultra").has_value());
    CHECK(ParseApprovalModeOrDefault("ultra") == ApprovalMode::Default);
    CHECK_FALSE(cli::ParseConfirmMode("ultra").has_value());
}

// ---------------------------------------------------------------------------
// 2) Agent 能力投影:按能力集合说话,不按枚举大小排强弱。
// ---------------------------------------------------------------------------

TEST_CASE("能力投影:五档的自动能力集合与 may_prompt") {
    using agent::AutomaticCapability;
    const auto caps = [](std::uint8_t automatic, bool may_prompt) {
        return agent::AgentPermissionCapabilities{automatic, may_prompt};
    };
    // 顺序同样打乱,与枚举声明序不同。
    const std::vector<std::pair<ApprovalMode, agent::AgentPermissionCapabilities>> rows = {
        {ApprovalMode::DontAsk, caps(0, false)},
        {ApprovalMode::Yolo, caps(static_cast<std::uint8_t>(AutomaticCapability::FileEdit) |
                                      static_cast<std::uint8_t>(AutomaticCapability::SafeCommand) |
                                      static_cast<std::uint8_t>(AutomaticCapability::All),
                                  true)},
        {ApprovalMode::Default, caps(0, true)},
        {ApprovalMode::Auto, caps(static_cast<std::uint8_t>(AutomaticCapability::FileEdit) |
                                      static_cast<std::uint8_t>(AutomaticCapability::SafeCommand),
                                  true)},
        {ApprovalMode::AcceptEdits, caps(static_cast<std::uint8_t>(AutomaticCapability::FileEdit), true)},
    };
    for (const auto& [mode, expected] : rows) {
        CAPTURE(ApprovalModeMachineName(mode));
        const agent::AgentPermissionCapabilities got = agent::PermissionCapabilities(mode);
        CHECK(got.automatic == expected.automatic);
        CHECK(got.may_prompt == expected.may_prompt);
    }
}

TEST_CASE("父子求交按能力集合:交出什么档与声明序无关") {
    using M = ApprovalMode;
    // Yolo 的 All 交 Default 的空 = Default(Yolo 的 may_prompt=true 不封死后代)。
    CHECK(agent::IntersectPermissionModes(M::Yolo, M::Default) == M::Default);
    // DontAsk 禁止询问:并进去就封口。
    CHECK(agent::IntersectPermissionModes(M::DontAsk, M::Default) == M::DontAsk);
    CHECK(agent::IntersectPermissionModes(M::Default, M::DontAsk) == M::DontAsk);
    // Auto 交 AcceptEdits = 只剩文件编辑。
    CHECK(agent::IntersectPermissionModes(M::Auto, M::AcceptEdits) == M::AcceptEdits);
    // 同档自交不变(五档全表,乱序)。
    for (const M mode : {M::Auto, M::Default, M::Yolo, M::DontAsk, M::AcceptEdits}) {
        CAPTURE(ApprovalModeMachineName(mode));
        CHECK(agent::IntersectPermissionModes(mode, mode) == mode);
    }
}

// ---------------------------------------------------------------------------
// 3) Runtime 裁定吃公共枚举:五档 × 工具类别,表驱动钉语义。
// ---------------------------------------------------------------------------

TEST_CASE("权限裁定:公共五档直接进 PermissionContext,档义不随枚举换位") {
    struct VerdictRow {
        ApprovalMode mode;
        tools::ApprovalClass approval_class;
        runtime::PermissionVerdict::Action expected;
    };
    const std::vector<VerdictRow> rows = {
        // 顺序打乱:与两枚旧枚举的声明序都不同。
        {ApprovalMode::Yolo, tools::ApprovalClass::FileEdit, runtime::PermissionVerdict::Action::Allow},
        {ApprovalMode::Yolo, tools::ApprovalClass::Command, runtime::PermissionVerdict::Action::Allow},
        {ApprovalMode::Default, tools::ApprovalClass::FileEdit, runtime::PermissionVerdict::Action::Ask},
        {ApprovalMode::Default, tools::ApprovalClass::Command, runtime::PermissionVerdict::Action::Ask},
        {ApprovalMode::DontAsk, tools::ApprovalClass::FileEdit, runtime::PermissionVerdict::Action::Deny},
        {ApprovalMode::DontAsk, tools::ApprovalClass::Command, runtime::PermissionVerdict::Action::Deny},
        {ApprovalMode::AcceptEdits, tools::ApprovalClass::FileEdit, runtime::PermissionVerdict::Action::Allow},
        {ApprovalMode::AcceptEdits, tools::ApprovalClass::Command, runtime::PermissionVerdict::Action::Ask},
        {ApprovalMode::Auto, tools::ApprovalClass::FileEdit, runtime::PermissionVerdict::Action::Allow},
        {ApprovalMode::Auto, tools::ApprovalClass::Command, runtime::PermissionVerdict::Action::Allow},
    };
    const runtime::ToolHookDecision no_hook;
    for (const VerdictRow& row : rows) {
        CAPTURE(ApprovalModeMachineName(row.mode));
        runtime::PermissionContext context;
        context.mode = row.mode;
        nlohmann::json input = nlohmann::json::object();
        if (row.approval_class == tools::ApprovalClass::Command) {
            // Auto 档的命令放行按安全分类:"git status" 属 Safe(与既有
            // unit.runtime.turn_runtime 的探针同一句、同一 shell,确定性有账)。
            input["command"] = "git status --short";
            input["shell"] = "powershell";
        }
        const auto verdict = runtime::EvaluatePermission(context, no_hook, row.approval_class,
                                                         "probe_tool", input);
        CHECK(verdict.action == row.expected);
    }
}
