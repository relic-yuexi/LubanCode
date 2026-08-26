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

#include "agent/prompt_assembler.hpp"
#include "sessions/session_store.hpp"
#include "cli/slash_commands.hpp"
#include "runtime/command.hpp"
#include "runtime/event.hpp"
#include "runtime/session_runtime.hpp"

namespace {

lubancode::runtime::PlanToolCapability Builtin(const char* name) {
    lubancode::runtime::PlanToolCapability capability;
    capability.name = name;
    capability.origin = lubancode::runtime::PlanToolOrigin::Builtin;
    return capability;
}

lubancode::agent::SessionMeta TestMeta() {
    lubancode::agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m";
    meta.cwd = "D:\\x";
    meta.started_at = "2026-08-23 10:00:00";
    return meta;
}

lubancode::api::Message UserMessage(const std::string& text) {
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{text});
    return message;
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
    for (const char* name : {"todo_write", "write_file", "edit_file", "memory_save", "undo_file_edit", "skill"}) {
        auto capability = Builtin(name);
        capability.mutating = true;
        const auto verdict = lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability);
        CHECK_FALSE(verdict.allowed);
        CHECK(verdict.code == lubancode::runtime::kErrModeDeniedWrite);
        // 拒绝文案带 /plan off 指引(单子:UI 要给退路)。
        CHECK(verdict.reason.find("/plan off") != std::string::npos);
    }
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

TEST_CASE("plan_mode: agent 只准 Explore,general-purpose 拒") {
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
}

TEST_CASE("plan_mode: run_command 按 shell 细判,白名单内放、外拒") {
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
}

TEST_CASE("plan_shell: 与 Auto 档 Safe 分道——cd/echo/cat 这些 Plan 不放") {
    using lubancode::runtime::PlanShellVerdict;
    // Auto 档的 kGenericSafe 里有 cd/echo/cat/type;Plan 的窄表没有。
    CHECK(lubancode::runtime::ClassifyPlanShell("cd src", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("echo hi", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("type main.cpp", "cmd") == PlanShellVerdict::Unknown);
    CHECK(lubancode::runtime::ClassifyPlanShell("cat main.cpp", "cmd") == PlanShellVerdict::Unknown);
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

// ---------------------------------------------------------------------------
// session 事件:mode_v1 / plan_v1 / plan_review_v1
// ---------------------------------------------------------------------------

TEST_CASE("plan_events: mode_v1 序列化与解析往返") {
    lubancode::agent::ModeEvent event;
    event.mode = "plan";
    event.reason = "slash";
    event.revision = 3;
    const std::string line = lubancode::agent::SerializeModeEvent(event, "2026-08-23 10:00:00");
    const auto parsed = lubancode::agent::ParseModeEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->mode == "plan");
    CHECK(parsed->reason == "slash");
    CHECK(parsed->revision == 3);
    // 坏形状:认不得的档位给 nullopt(不猜)。
    CHECK_FALSE(lubancode::agent::ParseModeEvent(R"({"type":"mode_v1","mode":"yolo"})").has_value());
    CHECK_FALSE(lubancode::agent::ParseModeEvent(R"({"type":"queue","items":[]})").has_value());
}

TEST_CASE("plan_events: plan_v1 序列化与解析往返(超限稿不带 markdown)") {
    lubancode::agent::PlanEvent event;
    event.plan_id = "plan-1";
    event.revision = 2;
    event.state = "presented";
    event.sha256 = "abc123";
    event.markdown = "# 稿子";
    event.turn_id = "turn-4";
    const std::string line = lubancode::agent::SerializePlanEvent(event, "ts");
    const auto parsed = lubancode::agent::ParsePlanEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->plan_id == "plan-1");
    CHECK(parsed->revision == 2);
    CHECK(parsed->state == "presented");
    CHECK(parsed->sha256 == "abc123");
    CHECK(parsed->markdown == "# 稿子");
    CHECK(parsed->turn_id == "turn-4");
    // 没身份/没锚的行救不了。
    CHECK_FALSE(lubancode::agent::ParsePlanEvent(R"({"type":"plan_v1","plan_id":"plan-1"})").has_value());
}

TEST_CASE("plan_events: plan_review_v1 序列化与解析往返") {
    lubancode::agent::PlanReviewEvent event;
    event.plan_id = "plan-1";
    event.revision = 2;
    event.decision = "approved";
    event.execution_permission = "confirm";
    const std::string line = lubancode::agent::SerializePlanReviewEvent(event, "ts");
    const auto parsed = lubancode::agent::ParsePlanReviewEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->decision == "approved");
    CHECK(parsed->execution_permission == "confirm");
    CHECK(parsed->revision == 2);
    // decision 只认 approved/rejected/continued。
    CHECK_FALSE(lubancode::agent::ParsePlanReviewEvent(
                    R"({"type":"plan_review_v1","plan_id":"p","decision":"maybe"})").has_value());
}

TEST_CASE("plan_events: ParseSessionFile 回放——最后一条 mode 胜、计划逐稿收、审批收") {
    const std::string meta = lubancode::agent::SerializeSessionMeta(TestMeta());
    std::string content = meta + "\n";
    content += lubancode::agent::SerializeSessionMessage(UserMessage("帮我规划"), "ts1");
    content += "\n" + lubancode::agent::SerializeModeEvent({"plan", "slash", 1}, "ts2");
    content += "\n" + lubancode::agent::SerializePlanEvent({"plan-1", 1, "presented", "sha1", "# 初稿", "", "turn-1"},
                                                           "ts3");
    content += "\n" + lubancode::agent::SerializePlanEvent(
                         {"plan-1", 1, "superseded", "sha1", "# 初稿", "", "turn-1"}, "ts4");
    content += "\n" + lubancode::agent::SerializePlanEvent({"plan-1", 2, "presented", "sha2", "# 改稿", "", "turn-2"},
                                                           "ts5");
    content += "\n" + lubancode::agent::SerializePlanReviewEvent({"plan-1", 2, "approved", "confirm"}, "ts6");
    content += "\n" + lubancode::agent::SerializeModeEvent({"default", "approved", 2}, "ts7");
    const auto session = lubancode::agent::ParseSessionFile(content);
    REQUIRE(session.has_value());
    CHECK(session->last_mode_event.mode == "default");  // 最后一条胜
    CHECK(session->last_mode_event.reason == "approved");
    REQUIRE(session->plan_events.size() == 3);  // 逐稿留账(含 superseded 行)
    CHECK(session->plan_events.back().revision == 2);
    CHECK(session->last_plan_review.has_value());
    CHECK(session->last_plan_review->decision == "approved");
    CHECK(session->skipped_lines == 0);
}

TEST_CASE("plan_events: 老档没有 mode/plan 行,按空账收(向后兼容)") {
    const std::string meta = lubancode::agent::SerializeSessionMeta(TestMeta());
    std::string content = meta + "\n";
    content += lubancode::agent::SerializeSessionMessage(UserMessage("旧会话"), "ts1");
    const auto session = lubancode::agent::ParseSessionFile(content);
    REQUIRE(session.has_value());
    CHECK(session->last_mode_event.mode.empty());  // 老 session 默认 Default
    CHECK(session->plan_events.empty());
    CHECK_FALSE(session->last_plan_review.has_value());
}

TEST_CASE("plan_events: 坏 mode 行跳过,不废整场") {
    const std::string meta = lubancode::agent::SerializeSessionMeta(TestMeta());
    std::string content = meta + "\n";
    content += "{\"type\":\"mode_v1\",\"mode\":\"plan\"";  // 坏 JSON(没闭合)
    content += "\n" + lubancode::agent::SerializeModeEvent({"default", "slash", 1}, "ts2");
    const auto session = lubancode::agent::ParseSessionFile(content);
    REQUIRE(session.has_value());
    CHECK(session->last_mode_event.mode == "default");
    CHECK(session->skipped_lines == 1);
}

// ---------------------------------------------------------------------------
// SessionRuntime 的模式/计划账
// ---------------------------------------------------------------------------

TEST_CASE("session_runtime: 切档、恢复、审批三对匹配") {
    lubancode::runtime::SessionRuntime runtime({"", "wire", "ts"});  // 不落盘(sessions_dir 空)
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
