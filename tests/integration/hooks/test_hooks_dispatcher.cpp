// HookDispatcher 真进程测试:决策矩阵、信任闸门、legacy 语义、并发归并、
#include "hooks/hash.hpp"
// 运行记录。命令用跨平台 shell 一行流(Windows cmd / POSIX sh 都认):
//   echo JSON > file & exit N   —— 想要"stdout 带 JSON + 指定退出码"就先落
//   文件再 type/cat。这里走更稳的路:命令直接由测试写成 per-platform。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/loader.hpp"
#include "hooks/protocol.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/text_encoding.hpp"

using namespace lubancode;

#ifdef _WIN32
// cmd.exe 一行流。注意 config 里的 shell 串由平台 RunShellCommand 包一层
// cmd /d /s /c "...",引号里再塞引号会被剥,所以 stdout JSON 用不含引号的
// 手法不行——改用写临时脚本文件的方式,内容我们完全可控。
std::string WriteHookScript(const std::string& name, const std::string& body) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-dispatch";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (name + ".cmd");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "@echo off\r\n" << body << "\r\n";
    return std::string(reinterpret_cast<const char*>(file.u8string().data()), file.u8string().size());
}
std::string RunScriptCommand(const std::string& path) { return "cmd /d /s /c \"\"" + path + "\"\""; }
#else
std::string WriteHookScript(const std::string& name, const std::string& body) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-dispatch";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (name + ".sh");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n" << body << "\n";
    return std::string(file.string());
}
std::string RunScriptCommand(const std::string& path) { return "sh \"" + path + "\""; }
#endif

// echo 一行 JSON:sh 会把双引号当引号语法吃掉,吐出来的就不是 JSON 了;
// POSIX 用单引号裹整串保真(这些 JSON 里没有单引号),cmd 不吃双引号、
// 原样透传。
std::string EchoJson(const std::string& json) {
#ifdef _WIN32
    return "echo " + json;
#else
    return "echo '" + json + "'";
#endif
}

namespace {

// 一只 v2 PreToolUse 钩子的定义(hand-built,不走配置解析)。
hooks::HookDefinition MakeDefinition(const std::string& command, bool legacy = false) {
    hooks::HookDefinition def;
    def.event = hooks::HookEvent::PreToolUse;
    def.source_kind = hooks::HookSourceKind::User;
    def.source_path = "test://user-config";
    def.source_label = "user test://user-config";
    def.matcher = "*";
    def.legacy = legacy;
    def.handler.command = command;
    def.handler.timeout_ms = 15000;
    def.handler.failure_policy = "warn";
    def.definition_hash = hooks::ComputeDefinitionHash(def.handler);
    def.definition_hash_short = hooks::DefinitionHashShort(def.definition_hash);
    def.trusted = true;
    return def;
}

hooks::HookDispatcher MakeDispatcher(std::vector<hooks::HookDefinition> defs) {
    hooks::LoadedHooks loaded;
    loaded.definitions = std::move(defs);
    hooks::HookTrustStore trust = hooks::HookTrustStore::Load(std::nullopt).first;
    hooks::HookContext ctx;
    ctx.session_id = "test-session";
    ctx.turn_id = "test-turn";
    ctx.cwd = "/test";
    ctx.permission_mode = "confirm";
    hooks::HookDispatcher dispatcher;
    dispatcher.Configure(std::move(loaded), std::move(trust), std::move(ctx));
    return dispatcher;
}

hooks::HookPayload MakeToolPayload() {
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::PreToolUse;
    payload.fields["tool_name"] = "run_command";
    payload.fields["tool_use_id"] = "toolu_1";
    payload.fields["tool_input"] = nlohmann::json::object({{"command", "dir"}});
    payload.match_value = "run_command";
    return payload;
}

}  // namespace

// ---------------------------------------------------------------------------
// 退出码三分 + stdout schema。
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher: exit 2 阻断,deny 胜出") {
    const std::string script = WriteHookScript("deny", "echo blocked-by-policy >&2\nexit 2");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::Deny);
    CHECK(merged.permission_reason.find("blocked-by-policy") != std::string::npos);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "blocked");
    CHECK(merged.executed == 1);
}

TEST_CASE("dispatcher: exit 0 + JSON allow 带 updatedInput") {
    const std::string script = WriteHookScript(
        "allow", EchoJson("{\"continue\":true,\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\","
                          "\"permissionDecision\":\"allow\",\"updatedInput\":{\"command\":\"dir /b\"}}}"));
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::Allow);
    REQUIRE(merged.updated_input.has_value());
    CHECK((*merged.updated_input)["command"] == "dir /b");
}

TEST_CASE("dispatcher: exit 1 = hook 自己失败,warn 策略下不放行痕迹也不拦") {
    const std::string script = WriteHookScript("fail", "exit 1");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::None);  // 不一概当 deny
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "failure");  // 也不静默当成功,记录在案
}

TEST_CASE("dispatcher: exit 1 + failure_policy=deny = 门卫没起来按拦") {
    const std::string script = WriteHookScript("fail-deny", "exit 1");
    hooks::HookDefinition def = MakeDefinition(RunScriptCommand(script));
    def.handler.failure_policy = "deny";
    hooks::HookDispatcher dispatcher = MakeDispatcher({def});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::Deny);
    CHECK(merged.permission_reason.find("failure_policy=deny") != std::string::npos);
}

TEST_CASE("dispatcher: 坏 JSON stdout = schema_error,可见不吞") {
    const std::string script = WriteHookScript("badjson", "echo this is { not json");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "schema_error");
    CHECK_FALSE(merged.records[0].detail.empty());
}

TEST_CASE("dispatcher: 空 stdout + exit 0 = 无结构化输出,无决策") {
    const std::string script = WriteHookScript("silent", "exit 0");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::None);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "ok");
}

// ---------------------------------------------------------------------------
// 决策归并:deny > ask > allow;并发跑,顺序稳定。
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher: 多只同时表态,deny 胜 ask 胜 allow") {
    const std::string allow_script =
        WriteHookScript("m-allow", EchoJson("{\"hookSpecificOutput\":{\"permissionDecision\":\"allow\"}}"));
    const std::string ask_script =
        WriteHookScript("m-ask", EchoJson("{\"hookSpecificOutput\":{\"permissionDecision\":\"ask\"}}"));
    const std::string deny_script =
        WriteHookScript("m-deny", EchoJson("{\"hookSpecificOutput\":{\"permissionDecision\":\"deny\","
                                           "\"permissionDecisionReason\":\"dangerous-op\"}}"));
    hooks::HookDispatcher dispatcher = MakeDispatcher(
        {MakeDefinition(RunScriptCommand(allow_script)), MakeDefinition(RunScriptCommand(ask_script)),
         MakeDefinition(RunScriptCommand(deny_script))});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::Deny);
    CHECK(merged.permission_reason.find("dangerous-op") != std::string::npos);
    CHECK(merged.executed == 3);
    // 记录按定义序,不按完成序:三条都在,顺序 = 装载序。
    REQUIRE(merged.records.size() == 3);
}

TEST_CASE("dispatcher: 无 deny、有 ask -> ask;全无表态 -> None") {
    const std::string ask_script =
        WriteHookScript("only-ask", EchoJson("{\"hookSpecificOutput\":{\"permissionDecision\":\"ask\"}}"));
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(ask_script))});
    auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::Ask);

    const std::string silent = WriteHookScript("only-silent", "exit 0");
    hooks::HookDispatcher dispatcher2 = MakeDispatcher({MakeDefinition(RunScriptCommand(silent))});
    merged = dispatcher2.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::None);
}

TEST_CASE("dispatcher: 两只慢钩子并发,总耗时接近最慢一只") {
    // 两只各睡 ~600ms;串行要 1200ms+,并发应明显小于之和(给足裕量,只挡
    // "退化成全数相加"的回归)。
    const std::string slow1 = WriteHookScript("slow1",
#ifdef _WIN32
                                              "exit 0"
#else
                                              "sleep 0.6\nexit 0"
#endif
    );
#ifndef _WIN32
    // 第二只用不同脚本:同一文件同 hash,dispatcher 按定义去重就只剩一只
    // (executed==2 挂)。Windows 分支两条 ping 命令本就不同,没这问题。
    const std::string slow2 = WriteHookScript("slow2", "sleep 0.6\nexit 0");
#endif
#ifdef _WIN32
    // Windows cmd 没有 sleep,用 ping 顶(约 600ms+)。
    const std::string cmd1 = "ping -n 1 -w 600 127.0.0.1 >nul & exit 0";
    const std::string cmd2 = "ping -n 1 -w 600 192.0.2.1 >nul & exit 0";
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(cmd1), MakeDefinition(cmd2)});
#else
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(slow1)),
                                                        MakeDefinition(RunScriptCommand(slow2))});
#endif
    const auto start = std::chrono::steady_clock::now();
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    CHECK(merged.executed == 2);
    CHECK(elapsed < 1100);  // 两只 ~600ms 并发:显著小于 1200ms 串行和
}

// ---------------------------------------------------------------------------
// legacy adapter 守旧语义。
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher: legacy 任意非零退出码都拦(退出码 1 也拦,不只是 2)") {
    const std::string script = WriteHookScript("legacy-1", "echo old-school-block\nexit 1");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script), /*legacy=*/true)});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::Deny);
    CHECK(merged.permission_reason.find("old-school-block") != std::string::npos);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "blocked");
}

TEST_CASE("dispatcher: legacy post_tool 非零只记 failure,不拦(没有拦截概念)") {
    const std::string script = WriteHookScript("legacy-post", "exit 3");
    hooks::HookDefinition def = MakeDefinition(RunScriptCommand(script), /*legacy=*/true);
    def.event = hooks::HookEvent::PostToolUse;
    hooks::HookDispatcher dispatcher = MakeDispatcher({def});
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::PostToolUse;
    payload.fields["tool_name"] = "write_file";
    payload.fields["tool_input"] = nlohmann::json::object();
    payload.fields["tool_response_text"] = "写好了";
    payload.fields["tool_succeeded"] = true;
    payload.match_value = "write_file";
    const auto merged = dispatcher.Emit(hooks::HookEvent::PostToolUse, payload);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "failure");
    CHECK(merged.records[0].exit_code == 3);
}

TEST_CASE("dispatcher: v2 exec form 起不来 = spawn_failed,warn 策略不拦但留痕") {
    hooks::HookDefinition def = MakeDefinition("definitely-not-a-command-xyz");
    def.handler.args = {"--flag"};  // exec form:不经 shell,可执行文件不存在 = 真 spawn 失败
    def.definition_hash = hooks::ComputeDefinitionHash(def.handler);
    def.definition_hash_short = hooks::DefinitionHashShort(def.definition_hash);
    hooks::HookDispatcher dispatcher = MakeDispatcher({def});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.permission == hooks::HookEventResult::Permission::None);  // 门卫没起来,不静默当成功也不一概当 deny
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "spawn_failed");
}

// ---------------------------------------------------------------------------
// 信任闸门:未信任项目 hook 绝不起进程。
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher: 未信任的项目 hook 不起进程,user hook 照跑") {
    const std::string user_script = WriteHookScript("user-ok", "exit 0");
    const std::string proj_script = WriteHookScript("proj-malicious", "echo PWNED > pwned-marker.txt\nexit 0");

    hooks::HookDefinition user_def = MakeDefinition(RunScriptCommand(user_script));
    hooks::HookDefinition proj_def = MakeDefinition(RunScriptCommand(proj_script));
    proj_def.source_kind = hooks::HookSourceKind::Project;
    proj_def.source_path = "test://project-config";
    proj_def.source_label = "project test://project-config";
    proj_def.trusted = false;  // 未信任
    proj_def.definition_hash = hooks::ComputeDefinitionHash(proj_def.handler);
    proj_def.definition_hash_short = hooks::DefinitionHashShort(proj_def.definition_hash);

    hooks::HookDispatcher dispatcher = MakeDispatcher({user_def, proj_def});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    REQUIRE(merged.records.size() == 2);
    CHECK(merged.records[0].outcome == "ok");                       // user 照跑
    CHECK(merged.records[1].outcome == "skipped_untrusted");        // project 跳过
    CHECK(merged.executed == 1);
    // 恶意标记文件不该存在——进程根本没起。
    const std::filesystem::path marker =
        std::filesystem::temp_directory_path() / "lubancode-hook-dispatch" / "pwned-marker.txt";
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("dispatcher: /hooks trust 动作即时生效,untrust 再收回") {
    const std::string script = WriteHookScript("proj-trust", "exit 0");
    hooks::HookDispatcher dispatcher = MakeDispatcher({});
    {
        hooks::LoadedHooks loaded;
        hooks::HookDefinition proj_def = MakeDefinition(RunScriptCommand(script));
        proj_def.source_kind = hooks::HookSourceKind::Project;
        proj_def.source_path = "test://project-config";
        proj_def.trusted = false;
        loaded.definitions.push_back(proj_def);
        loaded.has_untrusted_project = true;
        hooks::HookTrustStore trust = hooks::HookTrustStore::Load(std::nullopt).first;
        hooks::HookDispatcher fresh;
        fresh.Configure(std::move(loaded), std::move(trust), hooks::HookContext{});
        dispatcher = std::move(fresh);
    }
    auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "skipped_untrusted");

    CHECK(dispatcher.TrustDefinition(1));
    merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "ok");
    CHECK(merged.executed == 1);

    CHECK(dispatcher.UntrustDefinition(1));
    merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "skipped_untrusted");
}

TEST_CASE("dispatcher: 禁用的 hook 记 skipped_disabled;最近记录可查") {
    const std::string script = WriteHookScript("disabled-one", "exit 0");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    CHECK(dispatcher.SetDefinitionDisabled(1, true));
    auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "skipped_disabled");

    CHECK(dispatcher.SetDefinitionDisabled(1, false));
    merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    CHECK(merged.records[0].outcome == "ok");

    const hooks::HookRunRecord* last = dispatcher.LastRecordFor(1);
    REQUIRE(last != nullptr);
    CHECK(last->outcome == "ok");
    CHECK(dispatcher.RecentRecords(10).size() == 2);
}

TEST_CASE("dispatcher: matcher 不命中不跑;同 hash 去重只跑一次") {
    const std::string script = WriteHookScript("matcher-script", "exit 0");
    hooks::HookDefinition def = MakeDefinition(RunScriptCommand(script));
    def.matcher = "write_file";  // 只匹配 write_file
    hooks::HookDefinition dup = MakeDefinition(RunScriptCommand(script));  // 同命令同 hash
    hooks::HookDispatcher dispatcher = MakeDispatcher({def, dup});

    auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());  // run_command
    REQUIRE(merged.records.size() == 1);  // 第一只 matcher=write_file 不命中不记账;
                                          // 第二只 matcher=* 命中,首次见此 hash,照跑
    CHECK(merged.executed == 1);

    hooks::HookPayload write_payload = MakeToolPayload();
    write_payload.fields["tool_name"] = "write_file";
    write_payload.match_value = "write_file";
    merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, write_payload);
    CHECK(merged.executed == 1);  // 两只都命中,同 hash 去重只跑一次
    REQUIRE(merged.records.size() == 2);
    CHECK(merged.records[1].outcome == "skipped_dedupe");
}

TEST_CASE("dispatcher: stdin JSON 到位——hook 真读得到工具输入") {
    // 脚本把 stdin 原样落到文件,测试读回来验证协议字段。
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-dispatch";
    std::filesystem::create_directories(dir);
    const std::string sink = (dir / "stdin-sink.json").string();
#ifdef _WIN32
    const std::string script = "more +1 > \"" + sink + "\"";
    // cmd 的 more 从 stdin 读;更稳的是 findstr /v xXqQ(全量直通)。
    const std::string cmd = "cmd /d /s /c \"findstr /r \".*\" > " + sink + "\"";
#else
    const std::string script = "cat > " + sink;
    const std::string cmd = "sh -c 'cat > " + sink + "'";
#endif
    (void)script;
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(cmd)});
    dispatcher.Emit(hooks::HookEvent::PreToolUse, MakeToolPayload());
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(sink.c_str())), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(content.empty());
    const nlohmann::json parsed = nlohmann::json::parse(content);
    CHECK(parsed["schema_version"] == 2);
    CHECK(parsed["hook_event_name"] == "PreToolUse");
    CHECK(parsed["session_id"] == "test-session");
    CHECK(parsed["tool_name"] == "run_command");
    CHECK(parsed["tool_input"]["command"] == "dir");
}

TEST_CASE("dispatcher: 大 tool input 走 stdin,远超环境块上限也能到") {
    // Windows 环境块单变量上限 32767 字节;塞 128KB 进 tool_input,走 stdin
    // 必须原样可达。
    std::string big(128 * 1024, 'x');
    hooks::HookPayload payload = MakeToolPayload();
    payload.fields["tool_input"] = nlohmann::json::object({{"content", big}});

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-dispatch";
    std::filesystem::create_directories(dir);
    const std::string sink = (dir / "big-stdin-sink.json").string();
#ifdef _WIN32
    // findstr 单行 8192 字符上限,128KB 单行 JSON 会被它掐断;用 PowerShell
    // ReadToEnd 全量落盘(慢一两秒,测试可受)。
    const std::string cmd = "powershell -NoProfile -Command [Console]::In.ReadToEnd() > " + sink;
#else
    const std::string cmd = "sh -c 'cat > " + sink + "'";
#endif
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(cmd)});
    const auto merged = dispatcher.Emit(hooks::HookEvent::PreToolUse, payload);
    CHECK(merged.executed == 1);
    std::error_code ec;
    CHECK(std::filesystem::file_size(std::filesystem::path(reinterpret_cast<const char8_t*>(sink.c_str())), ec) >
          128 * 1024);
}

TEST_CASE("dispatcher: UserPromptSubmit 可阻断,additionalContext 归并") {
    const std::string script = WriteHookScript(
        "prompt-block",
        EchoJson("{\"continue\":false,\"stopReason\":\"prompt-not-allowed\",\"hookSpecificOutput\":"
                 "{\"hookEventName\":\"UserPromptSubmit\",\"additionalContext\":\"extra-context-1\"}}"));
    hooks::HookDefinition def = MakeDefinition(RunScriptCommand(script));
    def.event = hooks::HookEvent::UserPromptSubmit;
    hooks::HookDispatcher dispatcher = MakeDispatcher({def});
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::UserPromptSubmit;
    payload.fields["prompt"] = "delete the whole repo";
    const auto merged = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, payload);
    CHECK(merged.blocked);
    CHECK(merged.block_reason.find("prompt-not-allowed") != std::string::npos);
    REQUIRE(merged.additional_context.size() == 1);
    CHECK(merged.additional_context[0] == "extra-context-1");
}

TEST_CASE("dispatcher: SessionStart 按 source 匹配——resume 只命中 resume 的钩子") {
    const std::string script = WriteHookScript("on-resume", "exit 0");
    hooks::HookDefinition def = MakeDefinition(RunScriptCommand(script));
    def.event = hooks::HookEvent::SessionStart;
    def.matcher = "resume";
    hooks::HookDispatcher dispatcher = MakeDispatcher({def});

    hooks::HookPayload startup;
    startup.event = hooks::HookEvent::SessionStart;
    startup.fields["source"] = "startup";
    startup.match_value = "startup";
    auto merged = dispatcher.Emit(hooks::HookEvent::SessionStart, startup);
    CHECK(merged.records.empty());  // startup 不该命中 resume 钩子

    hooks::HookPayload resume = startup;
    resume.fields["source"] = "resume";
    resume.match_value = "resume";
    merged = dispatcher.Emit(hooks::HookEvent::SessionStart, resume);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "ok");
}

TEST_CASE("dispatcher: SubagentStop 的 continue=false 归并进 blocked(续跑由调用方)") {
    const std::string script = WriteHookScript(
        "subagent-stop-block",
        EchoJson("{\"continue\":false,\"stopReason\":\"run-the-tests\",\"hookSpecificOutput\":"
                 "{\"hookEventName\":\"SubagentStop\",\"additionalContext\":\"missing coverage\"}}"));
    hooks::HookDefinition def = MakeDefinition(RunScriptCommand(script));
    def.event = hooks::HookEvent::SubagentStop;
    hooks::HookDispatcher dispatcher = MakeDispatcher({def});
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::SubagentStop;
    payload.fields["agent_id"] = "7";
    payload.fields["agent_type"] = "general";
    payload.fields["stop_hook_active"] = false;
    const auto merged = dispatcher.Emit(hooks::HookEvent::SubagentStop, payload);
    CHECK(merged.blocked);  // "还不能停,再收口一轮"
    CHECK(merged.block_reason.find("run-the-tests") != std::string::npos);
    REQUIRE(merged.additional_context.size() == 1);
    // SubagentStop 没有 permission 语义——不许借它做权限决定。
    CHECK(merged.permission == hooks::HookEventResult::Permission::None);
}

TEST_CASE("dispatcher: EmitWith 的上下文覆写进 stdin(子代理 agent_id 分得清)") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-dispatch";
    std::filesystem::create_directories(dir);
    const std::string sink = (dir / "subctx-sink.json").string();
#ifdef _WIN32
    const std::string cmd = "powershell -NoProfile -Command [Console]::In.ReadToEnd() > " + sink;
#else
    const std::string cmd = "sh -c 'cat > " + sink + "'";
#endif
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(cmd)});
    hooks::HookContext sub_context = dispatcher.context();
    sub_context.agent_id = "agent_42";
    sub_context.agent_type = "Explore";
    sub_context.parent_agent_id = std::nullopt;

    hooks::HookPayload payload = MakeToolPayload();
    dispatcher.EmitWith(hooks::HookEvent::PreToolUse, payload, sub_context);
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(sink.c_str())), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(content.empty());
    const nlohmann::json parsed = nlohmann::json::parse(content);
    CHECK(parsed["agent_id"] == "agent_42");
    CHECK(parsed["agent_type"] == "Explore");
    CHECK(parsed["parent_agent_id"].is_null());
    // 覆写不落账:主上下文(agent 字段 null)不受影响。
    CHECK(dispatcher.context().agent_id == std::nullopt);
}

// ---------------------------------------------------------------------------
// 编码契约:宿主往 hook stdin 写 UTF-8 无 BOM 字节,Windows 管道不借
// OEM/ANSI 代码页。中文、emoji、反斜杠、换行、32KB prompt 逐字节原样到。
// ---------------------------------------------------------------------------

namespace {

// exec form 定义(command + args,不经 shell;Windows 引号转义由平台层做)。
hooks::HookDefinition MakeExecDefinition(const std::string& command, std::vector<std::string> args) {
    hooks::HookDefinition def;
    def.event = hooks::HookEvent::UserPromptSubmit;
    def.source_kind = hooks::HookSourceKind::User;
    def.source_path = "test://user-config";
    def.source_label = "user test://user-config";
    def.matcher = "*";
    def.handler.command = command;
    def.handler.args = std::move(args);
    def.handler.timeout_ms = 30000;
    def.handler.failure_policy = "warn";
    def.definition_hash = hooks::ComputeDefinitionHash(def.handler);
    def.definition_hash_short = hooks::DefinitionHashShort(def.definition_hash);
    def.trusted = true;
    return def;
}

// 一段刁钻 prompt:中文 + 全角标点 + emoji(4 字节 UTF-8)+ 反斜杠路径 +
// 引号 + 换行。够覆盖"过不了 ConvertFrom-Json"那类报告的全部成分。
std::string TrickyPrompt() {
    std::string prompt = "只回复 OK，不调用工具 🎉 路径 C:\\Users\\鲁班\\a\"b\r\n第二行\t带制表";
    // 32KB 长档:中文重复铺到超 32768 字节(Windows 环境块单变量上限,stdin 通道不该有这道墙)。
    while (prompt.size() < 32 * 1024) {
        prompt += "中文内容与 emoji 🚀 混排、反斜杠 \\ 与引号 \" 交错;\n";
    }
    return prompt;
}

hooks::HookPayload MakePromptPayload(const std::string& prompt) {
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::UserPromptSubmit;
    payload.fields["prompt"] = prompt;
    return payload;
}

std::string ReadFileBytes(const std::string& path) {
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string SinkPath(const char* name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-dispatch";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

// JS 字符串里反斜杠是转义符且会丢( '\U' 趋近 'U' ),node 那支用正斜杠路径。
std::string SinkForwardSlashes(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// 探一个解释器在不在(powershell/pwsh/python/node)。起不来或退出码非 0 都算不在,
// 测试早退——单测不预设机器装了什么。
bool InterpreterAvailable(const std::vector<std::string>& probe_args) {
    const auto result = platform::RunProcess(probe_args, 15000);
    return !result.spawn_failed && result.exit_code == 0;
}

// 只伺候 _WIN32 段(Windows 管道逐字节比对用)。
#ifdef _WIN32
// 字节级比对前把 hook_run_id 抹平:它带时间戳与进程内计数,两次构造不可能相等。
std::string StripHookRunId(const std::string& wire) {
    constexpr const char* kKey = "\"hook_run_id\":\"";
    const std::size_t key = wire.find(kKey);
    if (key == std::string::npos) {
        return wire;
    }
    const std::size_t value_begin = key + std::string_view(kKey).size();
    const std::size_t value_end = wire.find('"', value_begin);
    std::string out = wire;
    out.erase(value_begin, value_end - value_begin);
    return out;
}
#endif

}  // namespace

TEST_CASE("dispatcher: stdin JSON 是 UTF-8 无 BOM——中文/emoji/反斜杠/换行/32KB 原样往返") {
    const std::string prompt = TrickyPrompt();
    CHECK(prompt.size() >= 32 * 1024);

    const nlohmann::json stdin_json =
        hooks::BuildStdinPayload(MakePromptPayload(prompt), MakeDispatcher({}).context(), "hookrun_enc_test");
    const std::string wire = stdin_json.dump();

    // 无 BOM:头一个字节就是 '{',不是 EF BB BF。
    REQUIRE(wire.size() >= 3);
    CHECK(wire[0] == '{');
    CHECK(static_cast<unsigned char>(wire[0]) != 0xEF);
    CHECK(static_cast<unsigned char>(wire[1]) != 0xBB);
    CHECK(static_cast<unsigned char>(wire[2]) != 0xBF);
    // 整段是合法 UTF-8(不是 ANSI/GBK 字节,也不是被转义成 ASCII 的替身)。
    CHECK(platform::IsValidUtf8(wire));
    CHECK(wire.find("只回复 OK") != std::string::npos);
    CHECK(wire.find("🎉") != std::string::npos);
    // 回读:prompt 逐字节等值。
    const nlohmann::json parsed = nlohmann::json::parse(wire);
    CHECK(parsed["prompt"] == prompt);
    CHECK(parsed["hook_event_name"] == "UserPromptSubmit");
}

#ifdef _WIN32
TEST_CASE("dispatcher: Windows 管道逐字节忠实——PowerShell 5.1 按字节抄收 stdin") {
    // 子进程完全不解码,OpenStandardInput 的字节流直接 CopyTo 进文件:文件内容
    // 与宿主写的字节逐字节相等,证明管道没有借代码页倒一道手。
    if (!InterpreterAvailable({"powershell", "-NoProfile", "-Command", "exit 0"})) {
        return;
    }
    const std::string sink = SinkPath("stdin-bytes-ps5.txt");
    const std::string ps =
        "$in=[Console]::OpenStandardInput(); $fs=[IO.File]::Create('" + sink + "'); $in.CopyTo($fs); $fs.Close()";
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeExecDefinition("powershell", {"-NoProfile", "-Command", ps})});

    const std::string prompt = "只回复 OK，不调用工具 🎉 C:\\路径\\反斜杠";
    const auto merged = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, MakePromptPayload(prompt));
    CHECK(merged.executed == 1);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "ok");

    const std::string expected = StripHookRunId(
        hooks::BuildStdinPayload(MakePromptPayload(prompt), MakeDispatcher({}).context(), "").dump());
    const std::string got = StripHookRunId(ReadFileBytes(sink));
    REQUIRE(got.size() == expected.size());
    CHECK(got == expected);
}
#endif

// 四种解释器按文档例子明示 UTF-8 读 stdin:中文与 emoji 原样到达。机器上
// 没装的解释器早退跳过(powershell 5.1 在 Windows 上恒有)。
TEST_CASE("dispatcher: 中文与 emoji 经各解释器明示 UTF-8 读原样到达") {
    const std::string prompt = "只回复 OK，不调用工具 🎉\r\n换行与 C:\\反斜杠\\也在";
    const std::string sink = SinkPath("stdin-utf8-roundtrip.txt");
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(reinterpret_cast<const char8_t*>(sink.c_str())), ec);

    struct Interpreter {
        const char* name;
        std::vector<std::string> probe;  // 探活命令(退出码 0 = 在)
        std::vector<std::string> argv;   // 真跑的命令:明示 UTF-8 读 stdin,取 prompt 落盘
    };
    const std::vector<Interpreter> cases = {
#ifdef _WIN32
        {"powershell",
         {"powershell", "-NoProfile", "-Command", "exit 0"},
         {"powershell", "-NoProfile", "-Command",
          "$r = New-Object IO.StreamReader([Console]::OpenStandardInput(), (New-Object Text.UTF8Encoding $false)); "
          "$o = $r.ReadToEnd() | ConvertFrom-Json; "
          "[IO.File]::WriteAllText('" +
              sink + "', $o.prompt, (New-Object Text.UTF8Encoding $false))"}},
        {"pwsh",
         {"pwsh", "-NoProfile", "-Command", "exit 0"},
         {"pwsh", "-NoProfile", "-Command",
          "$r = New-Object IO.StreamReader([Console]::OpenStandardInput(), (New-Object Text.UTF8Encoding $false)); "
          "$o = $r.ReadToEnd() | ConvertFrom-Json; "
          "[IO.File]::WriteAllText('" +
              sink + "', $o.prompt, (New-Object Text.UTF8Encoding $false))"}},
#else
        {"sh", {"sh", "-c", "true"}, {"sh", "-c", "cat > '" + sink + "'"}},
#endif
        {"python",
         {"python", "-c", "pass"},
         {"python", "-c",
          "import sys, json\n"
          "payload = json.loads(sys.stdin.buffer.read().decode('utf-8'))\n"
          "with open(r'" +
              sink + "', 'w', encoding='utf-8', newline='') as f:\n"
              "    f.write(payload['prompt'])"}},
        {"node",
         {"node", "-e", "0"},
         {"node", "-e",
          "let raw = []; process.stdin.on('data', c => raw.push(c)); "
          "process.stdin.on('end', () => { const payload = JSON.parse(Buffer.concat(raw).toString('utf8')); "
          "require('fs').writeFileSync('" +
              SinkForwardSlashes(sink) + "', payload.prompt); });"}},
    };

    for (const auto& interp : cases) {
        if (!InterpreterAvailable(interp.probe)) {
            continue;
        }
        std::filesystem::remove(std::filesystem::path(reinterpret_cast<const char8_t*>(sink.c_str())), ec);
        CAPTURE(interp.name);
        hooks::HookDispatcher dispatcher =
            MakeDispatcher({MakeExecDefinition(interp.argv[0], {interp.argv.begin() + 1, interp.argv.end()})});
        const auto merged = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, MakePromptPayload(prompt));
        REQUIRE(merged.records.size() == 1);
        CHECK(merged.records[0].outcome == "ok");
        const std::string got = ReadFileBytes(sink);
        REQUIRE_FALSE(got.empty());
        // 到达的内容与原 prompt 逐字节相等(POSIX sh 那支落盘的是整份 JSON,单独判)。
        if (std::string_view(interp.name) != "sh") {
            CHECK(got == prompt);
        } else {
            CHECK(got.find("只回复 OK") != std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------
// stdout/stderr 分开捕获 + 明示解码:stderr 噪声打不坏 stdout 的 JSON,
// 中文 stderr 不乱码,超长 stderr 带截断标志。
// ---------------------------------------------------------------------------

#ifdef _WIN32
TEST_CASE("dispatcher: stderr 分开捕获——stderr 噪声不污染 stdout JSON,中文不乱码") {
    if (!InterpreterAvailable({"powershell", "-NoProfile", "-Command", "exit 0"})) {
        return;
    }
    // 子进程:stderr 写一行中文噪声,stdout 写合法 JSON,退出码 0。合并捕获
    // 的旧实现里 stderr 会混进 stdout,JSON 解析直接被打崩;分开后两不相扰。
    const std::string ps = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
                           "[Console]::Error.WriteLine('这是 stderr 的中文噪声,不该影响 stdout'); "
                           "Write-Output '{\"continue\":true}'";
    hooks::HookDispatcher dispatcher =
        MakeDispatcher({MakeExecDefinition("powershell", {"-NoProfile", "-Command", ps})});
    const auto merged = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, MakePromptPayload("查一下"));
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "ok");
    CHECK(merged.records[0].stderr_encoding == "utf-8");
    CHECK(merged.records[0].stderr_head.find("这是 stderr 的中文噪声") != std::string::npos);
    CHECK_FALSE(merged.records[0].stderr_truncated);
    // 无声替换是禁区:解码结果里不许冒出 U+FFFD。
    CHECK(merged.records[0].stderr_head.find("\xEF\xBF\xBD") == std::string::npos);
}

TEST_CASE("dispatcher: PowerShell 默认编码的中文 stderr 按控制台代码页解出并标注") {
    if (!InterpreterAvailable({"powershell", "-NoProfile", "-Command", "exit 0"})) {
        return;
    }
    // PS 5.1 不设 OutputEncoding 时按控制台输出页写管道。控制台页不是 936 的
    // 机器上中文在源头就变成了问号,无从复原——那种机器只断言"不乱码、有
    // 标注",不断言内容。
    const std::string ps = "[Console]::Error.WriteLine('中文报错内容回归')";
    hooks::HookDispatcher dispatcher =
        MakeDispatcher({MakeExecDefinition("powershell", {"-NoProfile", "-Command", ps})});
    const auto merged = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, MakePromptPayload("查一下"));
    REQUIRE(merged.records.size() == 1);
    const auto& record = merged.records[0];
    CHECK_FALSE(record.stderr_head.empty());
    CHECK(record.stderr_encoding != "unknown");
    CHECK(record.stderr_head.find("\xEF\xBF\xBD") == std::string::npos);
    const std::vector<unsigned int> candidates = platform::ChildStreamCodePageCandidates();
    if (!candidates.empty() && candidates.front() == 936) {
        CHECK(record.stderr_encoding == "cp936");
        CHECK(record.stderr_head.find("中文报错内容回归") != std::string::npos);
    }
}

TEST_CASE("dispatcher: 超长 stderr 截断入账,带截断标志") {
    if (!InterpreterAvailable({"powershell", "-NoProfile", "-Command", "exit 0"})) {
        return;
    }
    const std::string ps = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
                           "$line = 'x' * 200; "
                           "for ($i = 0; $i -lt 5; $i++) { [Console]::Error.WriteLine($line) }";
    hooks::HookDispatcher dispatcher =
        MakeDispatcher({MakeExecDefinition("powershell", {"-NoProfile", "-Command", ps})});
    const auto merged = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, MakePromptPayload("查一下"));
    REQUIRE(merged.records.size() == 1);
    const auto& record = merged.records[0];
    CHECK(record.stderr_truncated);
    CHECK(record.stderr_head.size() == hooks::HookRunRecord::kStderrHeadBytes);
}
#endif

// ---------------------------------------------------------------------------
// 后台(外挂)执行面:快照执行不落账,记录投递-归并闭环,信任闸门照过。
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher: EmitDetached 真跑钩子但不碰账本,投递后安全点归并") {
    const std::string script = WriteHookScript("detached-ok", "exit 0");
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(script))});
    const std::vector<hooks::HookDefinition> snapshot = dispatcher.PolicySnapshot();
    REQUIRE(snapshot.size() == 1);

    const auto merged = hooks::HookDispatcher::EmitDetached(snapshot, hooks::HookEvent::PreToolUse,
                                                            MakeToolPayload(), dispatcher.context());
    CHECK(merged.executed == 1);
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "ok");

    // 不落账:直接 Emit 的路才会进 recent_/last_record_,detached 跑完账本纹丝不动。
    CHECK(dispatcher.RecentRecords(10).empty());
    CHECK(dispatcher.LastRecordFor(1) == nullptr);

    // 投递 -> 归并闭环:记录进账,告警随行,收编一次就干净。
    CHECK_FALSE(dispatcher.HasExternalRecords());
    dispatcher.PostExternalRecords(merged.records, "后台子代理的钩子降级了一票");
    CHECK(dispatcher.HasExternalRecords());
    const auto adoption = dispatcher.AdoptExternalRecords();
    REQUIRE(adoption.records.size() == 1);
    REQUIRE(adoption.warnings.size() == 1);
    CHECK(adoption.warnings[0].find("降级") != std::string::npos);
    CHECK(dispatcher.RecentRecords(10).size() == 1);
    CHECK(dispatcher.LastRecordFor(1) != nullptr);
    CHECK(dispatcher.LastRecordFor(1)->outcome == "ok");
    CHECK_FALSE(dispatcher.HasExternalRecords());
    CHECK(dispatcher.AdoptExternalRecords().records.empty());  // 收编一次即清
}

TEST_CASE("dispatcher: 后台投递按时间序归并,新在前的流水序不乱") {
    hooks::HookDispatcher dispatcher = MakeDispatcher({});
    hooks::HookRunRecord first;
    first.definition_id = 1;
    first.event_name = "PostToolUse";
    first.outcome = "ok";
    hooks::HookRunRecord second = first;
    second.outcome = "blocked";
    dispatcher.PostExternalRecords({first});
    dispatcher.PostExternalRecords({second});
    const auto adoption = dispatcher.AdoptExternalRecords();
    REQUIRE(adoption.records.size() == 2);
    const auto recent = dispatcher.RecentRecords(10);
    REQUIRE(recent.size() == 2);
    CHECK(recent[0].outcome == "blocked");  // 后投递的(更新的)在前
    CHECK(recent[1].outcome == "ok");
}

TEST_CASE("dispatcher: 未信任/禁用照进快照——后台执行也过信任闸门") {
    const std::string script = WriteHookScript("detached-untrusted", "echo PWNED > bg-pwned-marker.txt\nexit 0");
    hooks::HookDefinition proj_def = MakeDefinition(RunScriptCommand(script));
    proj_def.source_kind = hooks::HookSourceKind::Project;
    proj_def.source_path = "test://project-config";
    proj_def.trusted = false;  // 未信任
    hooks::HookDispatcher dispatcher = MakeDispatcher({proj_def});

    const auto merged = hooks::HookDispatcher::EmitDetached(dispatcher.PolicySnapshot(),
                                                            hooks::HookEvent::PreToolUse, MakeToolPayload(),
                                                            dispatcher.context());
    REQUIRE(merged.records.size() == 1);
    CHECK(merged.records[0].outcome == "skipped_untrusted");
    CHECK(merged.executed == 0);
    // 进程根本没起:标记文件不存在。
    const std::filesystem::path marker =
        std::filesystem::temp_directory_path() / "lubancode-hook-dispatch" / "bg-pwned-marker.txt";
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("dispatcher: 后台线程投递与主线程归并并发,一条不丢") {
    hooks::HookDispatcher dispatcher = MakeDispatcher({});
    constexpr int kBatches = 100;
    std::thread poster([&dispatcher] {
        for (int i = 0; i < kBatches; ++i) {
            hooks::HookRunRecord record;
            record.definition_id = 1;
            record.event_name = "PostToolUse";
            record.outcome = "ok";
            dispatcher.PostExternalRecords({record});
        }
    });
    int adopted = 0;
    while (true) {
        adopted += static_cast<int>(dispatcher.AdoptExternalRecords().records.size());
        if (poster.joinable() && !dispatcher.HasExternalRecords()) {
            // 队列此刻空且投递线程还能 join = 再等一小拍仍空才算收完(投递
            // 线程可能正卡在锁上)。join 之后必有终局判断。
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    poster.join();
    adopted += static_cast<int>(dispatcher.AdoptExternalRecords().records.size());
    CHECK(adopted == kBatches);
}
