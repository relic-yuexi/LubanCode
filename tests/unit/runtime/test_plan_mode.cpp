// Plan 模式(只读研究硬闸单)的纯函数与合同测试:
//   - ModePolicy(EvaluateModePolicy):白名单/写盘拒/未知来源拒/agent 角色/
//     run_command shell 细判;Default 一概放行。
//   - Plan shell 分类(ClassifyPlanShell):与 Auto 档 Safe 不同表——cd/echo
//     不放,重定向/子表达式/环境赋值不放,git 只认只读子命令。
//   - <proposed_plan> 扫描(ScanProposedPlan):完整/半截/两份/代码块。
//   - session 事件(mode_v1/plan_v1/plan_review_v1)序列化与回放。
//   - /plan 二级参数解析。
//   - 提示词模式段:恒用嵌入版,plan 模板与 default 模板都在;default 明说
//     旧 Plan 已结束。
//   - 合同枚举(ClientCommandKind/ServerEventKind/ItemKind)的新值。

#include "runtime/plan_mode.hpp"

#include <doctest/doctest.h>

#include <filesystem>

#include "agent/prompt_assembler.hpp"
#include "cli/slash_commands.hpp"
#include "runtime/command.hpp"
#include "runtime/event.hpp"
#include "runtime/session_runtime.hpp"
#include "workspace/identity.hpp"

namespace {

lubancode::runtime::PlanToolCapability Builtin(const char* name) {
    lubancode::runtime::PlanToolCapability capability;
    capability.name = name;
    capability.origin = lubancode::runtime::PlanToolOrigin::Builtin;
    return capability;
}

}  // namespace

// ---------------------------------------------------------------------------
// EvaluateModePolicy
// ---------------------------------------------------------------------------

TEST_CASE("plan_mode: Default 一概放行,写盘件也放") {
    auto write = Builtin("write_file");
    write.mutating = true;
    const auto verdict = lubancode::runtime::EvaluateModePolicy(lubancode::runtime::CollaborationMode::Default, write);
    CHECK(verdict.allowed);
    CHECK(verdict.code.empty());
}

TEST_CASE("plan_mode: Plan 放行只读白名单") {
    for (const char* name : {"read_file", "search", "context_search", "context_read", "lsp", "web_search",
                             "web_fetch", "ask_user", "tool_search"}) {
        const auto verdict =
            lubancode::runtime::EvaluateModePolicy(lubancode::runtime::CollaborationMode::Plan, Builtin(name));
        CHECK(verdict.allowed);
    }
}

TEST_CASE("plan_mode: Plan 拒写盘件——todo_write/write_file/edit_file/memory_save/undo/record") {
    using lubancode::runtime::CollaborationMode;
    for (const char* name : {"todo_write", "write_file", "edit_file", "memory_save", "undo_file_edit", "worktree"}) {
        auto capability = Builtin(name);
        capability.mutating = true;
        const auto verdict = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability);
        CHECK_FALSE(verdict.allowed);
        CHECK(verdict.code == lubancode::runtime::kErrModeDeniedWrite);
        // 拒绝文案带 /plan off 指引(单子:UI 要给退路)。
        CHECK(verdict.reason.find("/plan off") != std::string::npos);
    }
}

TEST_CASE("plan_mode: Plan 放行 skill——加载技能只装说明进上下文(P2-3)") {
    using lubancode::runtime::CollaborationMode;
    // 只读档(注册处 SkillTool::effect_class 已声明 ReadOnlyLocal):放行。
    const auto allowed =
        lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, Builtin("skill"));
    CHECK(allowed.allowed);
    CHECK(allowed.code.empty());
    // 白名单表里有它(装配层据此给 plan_safe_by_default)。
    CHECK(lubancode::runtime::IsPlanAllowedBuiltinTool("skill"));
}

TEST_CASE("plan_mode: 未知来源(MCP/插件/Deferred)默认拒,annotation 不算数") {
    using lubancode::runtime::CollaborationMode;
    for (const auto origin : {lubancode::runtime::PlanToolOrigin::Mcp,
                              lubancode::runtime::PlanToolOrigin::PluginLua,
                              lubancode::runtime::PlanToolOrigin::PluginNative,
                              lubancode::runtime::PlanToolOrigin::Unknown}) {
        lubancode::runtime::PlanToolCapability capability = Builtin("some_mcp_tool");
        capability.origin = origin;
        const auto verdict = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability);
        CHECK_FALSE(verdict.allowed);
        CHECK(verdict.code == lubancode::runtime::kErrModeDeniedUnknownSource);
    }
}

TEST_CASE("plan_mode: MCP readOnlyHint 单独存在仍拒——宿主显式声明才放") {
    using lubancode::runtime::CollaborationMode;
    lubancode::runtime::PlanToolCapability capability = Builtin("mcp_readonly");
    capability.origin = lubancode::runtime::PlanToolOrigin::Mcp;
    // 没有宿主声明(plan_safe_by_default=false),annotation 是 hint 不是信任根。
    CHECK_FALSE(lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability).allowed);
    // 宿主显式声明放行(信任根是注册表)。
    capability.plan_safe_by_default = true;
    CHECK(lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability).allowed);
}

TEST_CASE("plan_mode: agent 按 Explore 与只读工具面判(P2-3)") {
    using lubancode::runtime::CollaborationMode;
    const auto capability = Builtin("agent");
    lubancode::runtime::PlanToolInput explore;
    explore.agent_role = "Explore";
    CHECK(lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, explore).allowed);
    lubancode::runtime::PlanToolInput general;
    general.agent_role = "general-purpose";
    const auto verdict = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, general);
    CHECK_FALSE(verdict.allowed);
    CHECK(verdict.code == lubancode::runtime::kErrModeDeniedAgentRole);
    // 小写 explore 也认(schema 的枚举值是 "Explore",模型可能给小写)。
    lubancode::runtime::PlanToolInput lower;
    lower.agent_role = "explore";
    CHECK(lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, lower).allowed);
    // 工具面只读的自定义 Agent(tools.allow 全为只读工具):放行。
    lubancode::runtime::PlanToolInput readonly_custom;
    readonly_custom.agent_role = "library-reviewer";
    readonly_custom.agent_tools_readonly = true;
    CHECK(lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, readonly_custom).allowed);
    // 工具面含写盘/命令工具的自定义 Agent:拒,回执说明判定口径。
    lubancode::runtime::PlanToolInput writing_custom;
    writing_custom.agent_role = "builder";
    writing_custom.agent_tools_readonly = false;
    const auto denied = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, writing_custom);
    CHECK_FALSE(denied.allowed);
    CHECK(denied.code == lubancode::runtime::kErrModeDeniedAgentRole);
    CHECK(denied.reason.find("tools.allow") != std::string::npos);
}

TEST_CASE("plan_mode: run_command 按 shell 细判,白名单内放、外拒带命中规则(P2-3)") {
    using lubancode::runtime::CollaborationMode;
    const auto capability = Builtin("run_command");
    lubancode::runtime::PlanToolInput safe;
    safe.shell_safe = true;
    CHECK(lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, safe).allowed);
    lubancode::runtime::PlanToolInput unsafe;
    unsafe.shell_safe = false;
    const auto verdict = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, unsafe);
    CHECK_FALSE(verdict.allowed);
    CHECK(verdict.code == lubancode::runtime::kErrModeDeniedShell);
    // 命中规则进回执(单子验收:"把命中的规则打印出来")。
    lubancode::runtime::PlanToolInput with_rule;
    with_rule.shell_safe = false;
    with_rule.shell_rule = "git 子命令 checkout 不在 Plan 只读子命令表";
    const auto ruled = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, with_rule);
    CHECK_FALSE(ruled.allowed);
    CHECK(ruled.reason.find("git 子命令 checkout") != std::string::npos);
}

TEST_CASE("plan_mode: 不在白名单的内置件拒(保守为纲)") {
    const auto verdict = lubancode::runtime::EvaluateModePolicy(
        lubancode::runtime::CollaborationMode::Plan, Builtin("programmatic_tool_calling"));
    CHECK_FALSE(verdict.allowed);
}

TEST_CASE("plan_mode: 枚举字符串往返") {
    using lubancode::runtime::CollaborationMode;
    CHECK(lubancode::runtime::ToString(CollaborationMode::Plan) == "plan");
    CHECK(lubancode::runtime::ToString(CollaborationMode::Default) == "default");
    CollaborationMode parsed = CollaborationMode::Default;
    CHECK(lubancode::runtime::ParseCollaborationMode("plan", parsed));
    CHECK(parsed == CollaborationMode::Plan);
    CHECK_FALSE(lubancode::runtime::ParseCollaborationMode("yolo", parsed));

    using lubancode::runtime::PlanReviewState;
    CHECK(lubancode::runtime::ToString(PlanReviewState::Presented) == "presented");
    CHECK(lubancode::runtime::ToString(PlanReviewState::Superseded) == "superseded");
    PlanReviewState state = PlanReviewState::Draft;
    CHECK(lubancode::runtime::ParsePlanReviewState("approved", state));
    CHECK(state == PlanReviewState::Approved);
    CHECK_FALSE(lubancode::runtime::ParsePlanReviewState("unknown", state));
}

// ---------------------------------------------------------------------------
// ClassifyPlanShell:与 Auto 档 Safe 不同表
// ---------------------------------------------------------------------------

TEST_CASE("plan_shell: 只读件放行") {
    using lubancode::runtime::PlanShellVerdict;
    CHECK(lubancode::runtime::ClassifyPlanShell("rg \"TODO\" src", "powershell") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("findstr /s \"err\" *.cpp", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-Content src/main.cpp", "powershell") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-ChildItem -Recurse", "powershell") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Select-String -Pattern x", "powershell") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("pwd", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-Location", "powershell") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Test-Path src", "powershell") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git status", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git log --oneline -20", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git diff HEAD~1", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git show abc123", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git ls-files", "cmd") == PlanShellVerdict::ReadOnly);
    // 版本探针(无重定向/管道/子表达式)。
    CHECK(lubancode::runtime::ClassifyPlanShell("python --version", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("node -v", "cmd") == PlanShellVerdict::ReadOnly);
    // P2-3 补的只读件:纯读命令与 git 查询子命令。
    CHECK(lubancode::runtime::ClassifyPlanShell("type main.cpp", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("cat package.json", "powershell") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("head -n 20 log.txt", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("tail log.txt", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("wc -l src/main.cpp", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("grep -n TODO src", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("dir src", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git ls-tree -r --name-only HEAD", "cmd") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git rev-parse HEAD", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git rev-list --count HEAD~5..HEAD", "cmd") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git blame src/main.cpp", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git cat-file -p abc123", "cmd") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("git show-ref", "cmd") == PlanShellVerdict::ReadOnly);
    // P2-3 补的 PowerShell 只读管道:真机实测 Get-ChildItem | Select-Object 被拦。
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-ChildItem | Select-Object Name,Length", "powershell") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-ChildItem -Recurse | Sort-Object Length | Select-Object -First 5",
                                                "powershell") == PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-Content package.json | Measure-Object -Line", "powershell") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-ChildItem | Where-Object Extension -eq .cpp", "powershell") ==
          PlanShellVerdict::ReadOnly);
    CHECK(lubancode::runtime::ClassifyPlanShell("Get-ChildItem | Format-Table Name", "powershell") ==
          PlanShellVerdict::ReadOnly);
}

TEST_CASE("plan_shell: 与 Auto 档 Safe 分道——cd/echo 这些 Plan 不放") {
    using lubancode::runtime::PlanShellVerdict;
    // Auto 档的 kGenericSafe 里有 cd/echo;Plan 的窄表没有(cd 改进程状态,
    // echo 是 Auto 档的放行,与只读无关)。type/cat 是 P2-3 后的只读件,
    // 已挪去上面的放行测试。
    CHECK(lubancode::runtime::ClassifyPlanShell("cd src", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("echo hi", "cmd") == PlanShellVerdict::Unknown);
}

TEST_CASE("plan_shell: 重定向/管道/子表达式/环境赋值一律不放") {
    using lubancode::runtime::PlanShellVerdict;
    CHECK(lubancode::runtime::ClassifyPlanShell("rg x > out.txt", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("rg x >> out.txt", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("rg x 2>&1", "cmd") == PlanShellVerdict::Unknown);
    // 管道到写命令(echo hi | out-file)。
    CHECK(lubancode::runtime::ClassifyPlanShell("rg x | sort", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("$(rm x)", "powershell") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("$env:FOO = 'bar'", "powershell") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("set FOO=bar", "cmd") == PlanShellVerdict::Unknown);
    // 引号里的重定向符不作数(引号状态机)。
    CHECK(lubancode::runtime::ClassifyPlanShell("rg \"a > b\"", "cmd") == PlanShellVerdict::ReadOnly);
}

TEST_CASE("plan_shell: git 只认只读子命令,写盘子命令与外部 diff 旗标拒") {
    using lubancode::runtime::PlanShellVerdict;
    CHECK(lubancode::runtime::ClassifyPlanShell("git add .", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("git commit -m x", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("git checkout main", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("git push", "cmd") == PlanShellVerdict::Unknown);
    // --ext-diff 可起外部 diff driver:不下 ReadOnly(单子 corner case)。
    CHECK(lubancode::runtime::ClassifyPlanShell("git diff --ext-diff", "cmd") == PlanShellVerdict::Unknown);
    // 全局选项(git -C ...)照 Unknown,保守(与 ClassifyCommand 同取舍)。
    CHECK(lubancode::runtime::ClassifyPlanShell("git -C .. status", "cmd") == PlanShellVerdict::Unknown);
}

// ---------------------------------------------------------------------------
// P2-3 契约:Git/PowerShell/cmd 三张只读命令表——只读写法放行,改状态
// 写法拦截,拦截回执带命中的规则。
// ---------------------------------------------------------------------------

TEST_CASE("plan_shell 契约: git 只读写法放行,改状态写法拦并报命中规则") {
    using lubancode::runtime::PlanShellClassification;
    using lubancode::runtime::PlanShellVerdict;
    // 只读:查询与列举。
    for (const char* command : {"git status", "git log --oneline -5", "git diff", "git show HEAD",
                                "git ls-files", "git ls-tree -r --name-only HEAD", "git rev-parse HEAD",
                                "git blame src/a.cpp", "git cat-file -t abc"}) {
        CHECK(lubancode::runtime::ClassifyPlanShell(command, "cmd") == PlanShellVerdict::ReadOnly);
    }
    // 改状态:工作树/引用/远端。
    for (const char* command : {"git add .", "git commit -m x", "git checkout -b feat", "git reset --hard",
                                "git push origin main", "git clean -fd", "git stash", "git merge feat"}) {
        const PlanShellClassification judged = lubancode::runtime::ClassifyPlanShellDetailed(command, "cmd");
        CHECK(judged.verdict == PlanShellVerdict::Unknown);
        CHECK_FALSE(judged.rule.empty());  // 回执把命中的规则说清(单子验收)
    }
    const PlanShellClassification checkout =
        lubancode::runtime::ClassifyPlanShellDetailed("git checkout main", "cmd");
    CHECK(checkout.rule.find("checkout") != std::string::npos);
    CHECK(checkout.rule.find("只读子命令") != std::string::npos);
}

TEST_CASE("plan_shell 契约: powershell 只读管道放行,写盘与脚本块拦并报命中规则") {
    using lubancode::runtime::PlanShellClassification;
    using lubancode::runtime::PlanShellVerdict;
    // 只读管道(真机实测闭门羹:Get-ChildItem ... | Select-Object)。
    for (const char* command : {"Get-ChildItem", "Get-ChildItem -Recurse -Filter *.cpp",
                                "Get-Content src/main.cpp", "Get-ChildItem | Select-Object FullName",
                                "Get-ChildItem | Where-Object Length -gt 100",
                                "Get-ChildItem | Sort-Object Name | Select-Object -First 3",
                                "Get-Content log.txt | Measure-Object -Line"}) {
        CHECK(lubancode::runtime::ClassifyPlanShell(command, "powershell") == PlanShellVerdict::ReadOnly);
    }
    // 写盘/任意代码:拦。
    const PlanShellClassification out_file =
        lubancode::runtime::ClassifyPlanShellDetailed("Remove-Item -Recurse build", "powershell");
    CHECK(out_file.verdict == PlanShellVerdict::Unknown);
    CHECK(out_file.rule.find("首词") != std::string::npos);
    // 脚本块体内是任意代码:拦,并指路无脚本块写法。
    const PlanShellClassification scriptblock =
        lubancode::runtime::ClassifyPlanShellDetailed("Get-ChildItem | Where-Object { $_.Length -gt 5 }",
                                                      "powershell");
    CHECK(scriptblock.verdict == PlanShellVerdict::Unknown);
    CHECK(scriptblock.rule.find("脚本块") != std::string::npos);
    // 引号里的 { 不算脚本块(引号状态机)。
    CHECK(lubancode::runtime::ClassifyPlanShell("Select-String -Pattern \"a { b\"", "powershell") ==
          PlanShellVerdict::ReadOnly);
}

TEST_CASE("plan_shell 契约: cmd 只读写法放行,写盘写法拦并报命中规则") {
    using lubancode::runtime::PlanShellClassification;
    using lubancode::runtime::PlanShellVerdict;
    // 只读:dir/type/findstr 与管道到只读件。
    for (const char* command : {"dir src", "type config.toml", "findstr /s /i TODO *.cpp",
                                "dir | findstr lib"} ) {
        CHECK(lubancode::runtime::ClassifyPlanShell(command, "cmd") == PlanShellVerdict::ReadOnly);
    }
    // 写盘:拦。
    for (const char* command : {"del build.log", "rd /s /q build", "copy a b", "move a b",
                                "npm install left-pad", "dotnet build"}) {
        const PlanShellClassification judged = lubancode::runtime::ClassifyPlanShellDetailed(command, "cmd");
        CHECK(judged.verdict == PlanShellVerdict::Unknown);
        CHECK_FALSE(judged.rule.empty());
    }
    const PlanShellClassification del = lubancode::runtime::ClassifyPlanShellDetailed("del x.txt", "cmd");
    CHECK(del.rule.find("首词") != std::string::npos);
}

TEST_CASE("plan_shell: 链上有一段不安全整条不安全;不认的 shell 不猜") {
    using lubancode::runtime::PlanShellVerdict;
    CHECK(lubancode::runtime::ClassifyPlanShell("rg x && del y", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("rg x", "zsh") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("", "cmd") == PlanShellVerdict::Unknown);
}

// ---------------------------------------------------------------------------
// ScanProposedPlan
// ---------------------------------------------------------------------------

TEST_CASE("plan_scan: 完整标签出计划,tag 外正文不算") {
    const auto scan = lubancode::runtime::ScanProposedPlan(
        "先说两句。\n<proposed_plan>\n# 计划\n第一步...\n</proposed_plan>\n尾注一句。");
    CHECK(scan.found);
    CHECK_FALSE(scan.ambiguous);
    CHECK_FALSE(scan.truncated);
    CHECK(scan.markdown == "\n# 计划\n第一步...\n");
}

TEST_CASE("plan_scan: 半截(有开无合)置 truncated,不弹审批") {
    const auto scan = lubancode::runtime::ScanProposedPlan("说明\n<proposed_plan>\n# 计划到一半");
    CHECK_FALSE(scan.found);
    CHECK(scan.truncated);
}

TEST_CASE("plan_scan: 两份/嵌套置 ambiguous,不弹审批") {
    const auto scan = lubancode::runtime::ScanProposedPlan(
        "<proposed_plan>a</proposed_plan> 中间话 <proposed_plan>b</proposed_plan>");
    CHECK_FALSE(scan.found);
    CHECK(scan.ambiguous);
}

TEST_CASE("plan_scan: 代码块里的标签不触发") {
    const auto scan = lubancode::runtime::ScanProposedPlan(
        "示例:\n```\n<proposed_plan>\n假计划\n</proposed_plan>\n```\n真话。");
    CHECK_FALSE(scan.found);
    CHECK_FALSE(scan.ambiguous);
    CHECK_FALSE(scan.truncated);
}

TEST_CASE("plan_scan: 没有标签是普通正文") {
    const auto scan = lubancode::runtime::ScanProposedPlan("普通回答,没有计划。");
    CHECK_FALSE(scan.found);
    CHECK_FALSE(scan.ambiguous);
    CHECK_FALSE(scan.truncated);
}

// (P0-6:mode_v1/plan_v1/plan_review_v1 旧档事件行的序列化/回放用例已删
// ——mode 与计划成品的持久账走 trajectory 的 control.mode.changed 等
// typed 事件,内存真值(ModeState/PlanDocument)的用例在前文。)

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SessionRuntime 的模式/计划账
// ---------------------------------------------------------------------------

TEST_CASE("session_runtime: 切档、恢复、审批三对匹配") {
    // P0-6 起 Options 无 sessions_dir("不落盘"的老路已删),账恒开——
    // 旧三参聚合初始化的 "ts" 会落进 WorkspaceIdentity.workspace_key,
    // 半截身份(有 key 无 checkout_root)让账本 absolute(空路径) 抛异常。
    // 照 test_session_runtime.cpp 的样子给临时根 + 兜底身份。
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "lubancode-plan-mode-runtime";
    std::error_code ec;
    std::filesystem::create_directories(tmp / "repo", ec);
    lubancode::runtime::SessionRuntime::Options runtime_options;
    runtime_options.wire_name = "wire";
    runtime_options.start_ts = "ts";
    runtime_options.trajectory_workspaces_root = tmp / "workspaces";
    runtime_options.trajectory_workspace_identity =
        lubancode::workspace::MakeFallbackIdentity(tmp / "repo");
    lubancode::runtime::SessionRuntime runtime(std::move(runtime_options));
    CHECK(runtime.collaboration_mode() == lubancode::runtime::CollaborationMode::Default);
    CHECK(runtime.SetCollaborationMode(lubancode::runtime::CollaborationMode::Plan, "slash", "confirm"));
    CHECK(runtime.mode_state().permission_before_plan == "confirm");
    CHECK(runtime.mode_state().revision == 1);
    CHECK_FALSE(runtime.SetCollaborationMode(lubancode::runtime::CollaborationMode::Plan, "slash"));  // 同档不切

    lubancode::runtime::PlanDocument plan;
    plan.plan_id = "plan-1";
    plan.revision = 1;
    plan.markdown = "# A";
    plan.content_sha256 = "sha-a";
    plan.state = lubancode::runtime::PlanReviewState::Presented;
    runtime.RecordPlanDocument(plan);
    REQUIRE(runtime.latest_plan() != nullptr);

    // 旧 dialog 迟到回答:hash 对不上 -> Stale,不落账。
    CHECK(runtime.ReviewPlan("plan-1", 1, "sha-wrong", true) ==
          lubancode::runtime::SessionRuntime::PlanReviewOutcome::Stale);
    CHECK(runtime.latest_plan()->state == lubancode::runtime::PlanReviewState::Presented);
    // 三对齐 -> Approved。
    CHECK(runtime.ReviewPlan("plan-1", 1, "sha-a", true) ==
          lubancode::runtime::SessionRuntime::PlanReviewOutcome::Approved);
    CHECK(runtime.latest_plan()->state == lubancode::runtime::PlanReviewState::Approved);

    // 新稿 supersede 旧 Presented 稿,revision 递增在调用侧;runtime 记账。
    lubancode::runtime::PlanDocument second;
    second.plan_id = "plan-1";
    second.revision = 2;
    second.markdown = "# B";
    second.content_sha256 = "sha-b";
    second.state = lubancode::runtime::PlanReviewState::Presented;
    runtime.RecordPlanDocument(second);
    REQUIRE(runtime.latest_plan() != nullptr);
    CHECK(runtime.latest_plan()->revision == 2);
    // 旧稿(已 Approved)不叫 supersede——只有 Presented 稿被顶。
    CHECK(runtime.latest_plan()->state == lubancode::runtime::PlanReviewState::Presented);
}

// ---------------------------------------------------------------------------
// /plan 二级参数
// ---------------------------------------------------------------------------

TEST_CASE("slash: /plan 解析四路") {
    using lubancode::cli::PlanCommandAction;
    CHECK(lubancode::cli::ParsePlanCommand("").action == PlanCommandAction::Enter);
    CHECK(lubancode::cli::ParsePlanCommand("   ").action == PlanCommandAction::Enter);
    const auto with_task = lubancode::cli::ParsePlanCommand("帮我设计 缓存层 的方案");
    CHECK(with_task.action == PlanCommandAction::EnterWithTask);
    CHECK(with_task.description == "帮我设计 缓存层 的方案");
    const auto single_word = lubancode::cli::ParsePlanCommand("查登录死锁");
    CHECK(single_word.action == PlanCommandAction::EnterWithTask);
    CHECK(single_word.description == "查登录死锁");
    CHECK(lubancode::cli::ParsePlanCommand("status").action == PlanCommandAction::Status);
    CHECK(lubancode::cli::ParsePlanCommand("off").action == PlanCommandAction::Off);
    CHECK(lubancode::cli::ParsePlanCommand("review").action == PlanCommandAction::Review);
    // 子词后还跟词:Invalid(不把 "status foo" 当正文)。
    CHECK(lubancode::cli::ParsePlanCommand("status foo").action == PlanCommandAction::Invalid);
    CHECK(lubancode::cli::ParsePlanCommand("off now").action == PlanCommandAction::Invalid);
    // 大小写不敏感(与 ParseSlashCommand 同规矩)。
    CHECK(lubancode::cli::ParsePlanCommand("STATUS").action == PlanCommandAction::Status);
}

TEST_CASE("slash: /plan 进命令表与帮助") {
    const auto parsed = lubancode::cli::ParseSlashCommand("/plan status");
    CHECK(parsed.command == lubancode::cli::SlashCommand::Plan);
    CHECK(parsed.args == "status");
    CHECK(lubancode::cli::ParseSlashCommand("/PLAN").command == lubancode::cli::SlashCommand::Plan);
    bool found = false;
    for (const auto& info : lubancode::cli::AllSlashCommands()) {
        if (info.name == "/plan") {
            found = true;
            // 单子:"/help 把 /plan 说明写成只读研究并提交计划"。
            CHECK(info.description.find("只读研究") != std::string::npos);
        }
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// 提示词模式段
// ---------------------------------------------------------------------------

TEST_CASE("prompt: ModeInstructionSegment 恒用嵌入版且两份模板都在") {
    const std::string plan = lubancode::agent::ModeInstructionSegment(/*plan_mode=*/true);
    CHECK_FALSE(plan.empty());
    // Plan 模板钉住的硬话(单子"Plan 模板至少钉这些话"):
    CHECK(plan.find("Plan 模式") != std::string::npos);
    CHECK(plan.find("proposed_plan") != std::string::npos);
    CHECK(plan.find("todo_write") != std::string::npos);  // "Plan 不调用它"
    CHECK((plan.find("先查环境") != std::string::npos || plan.find("先查") != std::string::npos));

    const std::string def = lubancode::agent::ModeInstructionSegment(/*plan_mode=*/false);
    CHECK_FALSE(def.empty());
    // Default 模板明说旧 Plan 指令已结束(单子:防切档残留,Codex 公开
    // issue 出过这类残留)。
    CHECK(def.find("已结束") != std::string::npos);
}

TEST_CASE("prompt: AssembleSystemPrompt 末尾注模式段,plan 与 default 不同") {
    lubancode::agent::PromptOptions options;
    options.cwd = "D:\\x";
    options.plan_mode = true;
    const std::string as_plan = lubancode::agent::AssembleSystemPrompt(options);
    options.plan_mode = false;
    const std::string as_default = lubancode::agent::AssembleSystemPrompt(options);
    CHECK(as_plan != as_default);
    // 模式段殿后(单子:mode instructions 最后,不给项目覆盖)。
    const std::string segment = lubancode::agent::ModeInstructionSegment(true);
    CHECK(as_plan.find(segment) != std::string::npos);
    CHECK(as_plan.rfind(segment) + segment.size() == as_plan.size());
    // Default 也注模式段(明令结束旧 Plan 指令)。
    const std::string default_segment = lubancode::agent::ModeInstructionSegment(false);
    CHECK(as_default.find(default_segment) != std::string::npos);
}

// ---------------------------------------------------------------------------
// 合同枚举的新值
// ---------------------------------------------------------------------------

TEST_CASE("contract: ClientCommandKind 新命令字符串往返") {
    using K = lubancode::runtime::ClientCommandKind;
    CHECK(lubancode::runtime::ToString(K::SetCollaborationMode) == "mode.set");
    CHECK(lubancode::runtime::ToString(K::ReviewPlan) == "plan.review");
    CHECK(lubancode::runtime::ToString(K::ReopenPlanReview) == "plan.review_reopen");
    K parsed = K::StartTurn;
    CHECK(lubancode::runtime::ParseClientCommandKind("mode.set", parsed));
    CHECK(parsed == K::SetCollaborationMode);
    CHECK(lubancode::runtime::ParseClientCommandKind("plan.review", parsed));
    CHECK(parsed == K::ReviewPlan);
    CHECK(lubancode::runtime::ParseClientCommandKind("plan.review_reopen", parsed));
    CHECK(parsed == K::ReopenPlanReview);
}

TEST_CASE("contract: ServerEventKind/ItemKind 新值字符串往返") {
    using K = lubancode::runtime::ServerEventKind;
    CHECK(lubancode::runtime::ToString(K::CollaborationModeChanged) == "mode.changed");
    CHECK(lubancode::runtime::ToString(K::PlanReviewRequested) == "plan.review_requested");
    CHECK(lubancode::runtime::ToString(K::PlanReviewResolved) == "plan.review_resolved");
    K parsed = K::ThreadStarted;
    CHECK(lubancode::runtime::ParseServerEventKind("mode.changed", parsed));
    CHECK(parsed == K::CollaborationModeChanged);
    CHECK(lubancode::runtime::ParseServerEventKind("plan.review_requested", parsed));
    CHECK(parsed == K::PlanReviewRequested);

    CHECK(lubancode::runtime::ToString(lubancode::runtime::ItemKind::Plan) == "plan");
    lubancode::runtime::ItemKind item = lubancode::runtime::ItemKind::Tool;
    CHECK(lubancode::runtime::ParseItemKind("plan", item));
    CHECK(item == lubancode::runtime::ItemKind::Plan);

    // 模式与审阅是 thread 层事件(不钉在某条 item 上)。
    lubancode::runtime::ServerEvent event;
    event.kind = K::PlanReviewRequested;
    CHECK(lubancode::runtime::LayerOf(event) == lubancode::runtime::EventLayer::Thread);
    event.kind = K::CollaborationModeChanged;
    CHECK(lubancode::runtime::LayerOf(event) == lubancode::runtime::EventLayer::Thread);
}
