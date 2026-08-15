// HookDispatcher 真进程测试:决策矩阵、信任闸门、legacy 语义、并发归并、
#include "hooks/hash.hpp"
// 运行记录。命令用跨平台 shell 一行流(Windows cmd / POSIX sh 都认):
//   echo JSON > file & exit N   —— 想要"stdout 带 JSON + 指定退出码"就先落
//   文件再 type/cat。这里走更稳的路:命令直接由测试写成 per-platform。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/loader.hpp"

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
        "allow",
        "echo {\"continue\":true,\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\","
        "\"permissionDecision\":\"allow\",\"updatedInput\":{\"command\":\"dir /b\"}}}");
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
        WriteHookScript("m-allow", "echo {\"hookSpecificOutput\":{\"permissionDecision\":\"allow\"}}");
    const std::string ask_script =
        WriteHookScript("m-ask", "echo {\"hookSpecificOutput\":{\"permissionDecision\":\"ask\"}}");
    const std::string deny_script =
        WriteHookScript("m-deny", "echo {\"hookSpecificOutput\":{\"permissionDecision\":\"deny\","
                                  "\"permissionDecisionReason\":\"dangerous-op\"}}");
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
        WriteHookScript("only-ask", "echo {\"hookSpecificOutput\":{\"permissionDecision\":\"ask\"}}");
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
#ifdef _WIN32
    // Windows cmd 没有 sleep,用 ping 顶(约 600ms+)。
    const std::string cmd1 = "ping -n 1 -w 600 127.0.0.1 >nul & exit 0";
    const std::string cmd2 = "ping -n 1 -w 600 192.0.2.1 >nul & exit 0";
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(cmd1), MakeDefinition(cmd2)});
#else
    hooks::HookDispatcher dispatcher = MakeDispatcher({MakeDefinition(RunScriptCommand(slow1)),
                                                        MakeDefinition(RunScriptCommand(slow1))});
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
        "echo {\"continue\":false,\"stopReason\":\"prompt-not-allowed\",\"hookSpecificOutput\":"
        "{\"hookEventName\":\"UserPromptSubmit\",\"additionalContext\":\"extra-context-1\"}}");
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
