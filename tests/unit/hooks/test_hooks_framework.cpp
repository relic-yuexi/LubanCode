// hooks 框架(Hooks 生命周期与信任协议)测试——纯函数与配置解析面:
//   1) SHA-256 definition hash(NIST 标准向量 + 字段敏感性);
//   2) protocol:stdin JSON 拼装、stdout 逐事件 schema 校验、退出码三分;
//   3) config:schema 2 解析(v2 事件键/exec form/错误形状)、来源相加合并、
//      迁移提示;
//   4) trust store:hash 信任、路径+hash 键、落盘回读、坏账本容错;
//   5) loader:分级(user/project)、未信任标记、去重;
//   6) dispatcher:真跑进程的决策矩阵(exit 0/2/1、坏 JSON、空 stdout、
//      legacy 语义、并发归并、未信任不起进程)在 test_hooks_dispatcher.cpp。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "hooks/hash.hpp"
#include "hooks/loader.hpp"
#include "hooks/protocol.hpp"
#include "hooks/trust.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// 1) SHA-256 definition hash。
// ---------------------------------------------------------------------------

TEST_CASE("Sha256Hex: NIST 标准测试向量") {
    CHECK(hooks::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hooks::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hooks::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("ComputeDefinitionHash: 命令/args/timeout/async 任一改,hash 变") {
    config::HookHandlerConfig base;
    base.command = "python";
    base.args = {"check.py"};
    base.timeout_ms = 20000;
    const std::string base_hash = hooks::ComputeDefinitionHash(base);

    config::HookHandlerConfig changed = base;
    changed.command = "python3";
    CHECK(hooks::ComputeDefinitionHash(changed) != base_hash);

    changed = base;
    changed.args = {"other.py"};
    CHECK(hooks::ComputeDefinitionHash(changed) != base_hash);

    changed = base;
    changed.timeout_ms = 30000;
    CHECK(hooks::ComputeDefinitionHash(changed) != base_hash);

    changed = base;
    changed.async = true;
    CHECK(hooks::ComputeDefinitionHash(changed) != base_hash);

    changed = base;
    changed.type = "http";
    CHECK(hooks::ComputeDefinitionHash(changed) != base_hash);

    // 同一份定义(含字段顺序)hash 稳定。
    config::HookHandlerConfig same = base;
    CHECK(hooks::ComputeDefinitionHash(same) == base_hash);
}

// ---------------------------------------------------------------------------
// 2) protocol:stdin 拼装与 stdout 逐事件 schema。
// ---------------------------------------------------------------------------

TEST_CASE("BuildStdinPayload: 公共字段 + 事件字段,agent 字段 null 语义") {
    hooks::HookContext ctx;
    ctx.session_id = "sess_1";
    ctx.turn_id = "turn_1";
    ctx.cwd = "D:/proj";
    ctx.transcript_path = "D:/proj/.lubancode/session.jsonl";
    ctx.permission_mode = "confirm";

    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::PreToolUse;
    payload.fields["tool_name"] = "run_command";
    payload.fields["tool_input"] = nlohmann::json::object({{"command", "dir"}});

    const nlohmann::json out = hooks::BuildStdinPayload(payload, ctx, "hookrun_1");
    CHECK(out["schema_version"] == 2);
    CHECK(out["hook_event_name"] == "PreToolUse");
    CHECK(out["hook_run_id"] == "hookrun_1");
    CHECK(out["session_id"] == "sess_1");
    CHECK(out["turn_id"] == "turn_1");
    CHECK(out["cwd"] == "D:/proj");
    CHECK(out["transcript_path"] == "D:/proj/.lubancode/session.jsonl");
    CHECK(out["permission_mode"] == "default");
    CHECK(out["agent_id"].is_null());
    CHECK(out["agent_type"].is_null());
    CHECK(out["tool_name"] == "run_command");
    CHECK(out["tool_input"]["command"] == "dir");
}

TEST_CASE("BuildStdinPayload: 审批档五值写稳，旧值与未知值保守归默认") {
    hooks::HookContext ctx;
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::SessionStart;
    const std::vector<std::string> stable{"default", "accept_edits", "yolo", "auto", "dont_ask"};
    for (const auto& mode : stable) {
        ctx.permission_mode = mode;
        CHECK(hooks::BuildStdinPayload(payload, ctx, "r")["permission_mode"] == mode);
    }
    ctx.permission_mode = "confirm";
    CHECK(hooks::BuildStdinPayload(payload, ctx, "r")["permission_mode"] == "default");
    ctx.permission_mode = "future_unrestricted";
    CHECK(hooks::BuildStdinPayload(payload, ctx, "r")["permission_mode"] == "default");
}

TEST_CASE("BuildStdinPayload: 子代理触发时 agent 字段带值,主代理为 null") {
    hooks::HookContext ctx;
    ctx.agent_id = "agent_7";
    ctx.agent_type = "general";
    ctx.parent_agent_id = "agent_main";
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::PostToolUse;
    const nlohmann::json out = hooks::BuildStdinPayload(payload, ctx, "r");
    CHECK(out["agent_id"] == "agent_7");
    CHECK(out["agent_type"] == "general");
    CHECK(out["parent_agent_id"] == "agent_main");
}

TEST_CASE("ParseStdoutJson: 空 stdout = 没有结构化输出,全字段缺省") {
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::PreToolUse, "");
    CHECK(parsed.ok);
    CHECK_FALSE(parsed.has_continue);
    CHECK_FALSE(parsed.has_permission_decision);
}

TEST_CASE("ParseStdoutJson: PreToolUse 合法 allow + updatedInput") {
    const std::string stdout_text = R"({
        "continue": true,
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow",
            "updatedInput": {"command": "dir /b"}
        }
    })";
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::PreToolUse, stdout_text);
    REQUIRE(parsed.ok);
    CHECK(parsed.permission == hooks::HookEventResult::Permission::Allow);
    CHECK(parsed.has_updated_input);
    CHECK(parsed.updated_input["command"] == "dir /b");
}

TEST_CASE("ParseStdoutJson: updatedInput 与 deny 同返 = schema 错") {
    const std::string stdout_text = R"({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "updatedInput": {"command": "x"}
        }
    })";
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::PreToolUse, stdout_text);
    CHECK_FALSE(parsed.ok);
    CHECK(parsed.error.find("updatedInput") != std::string::npos);
}

TEST_CASE("ParseStdoutJson: PermissionRequest 不认 updatedInput(不许借道改参)") {
    const std::string stdout_text = R"({
        "hookSpecificOutput": {
            "hookEventName": "PermissionRequest",
            "permissionDecision": "allow",
            "updatedInput": {"command": "x"}
        }
    })";
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::PermissionRequest, stdout_text);
    CHECK_FALSE(parsed.ok);
    CHECK(parsed.error.find("updatedInput") != std::string::npos);
}

TEST_CASE("ParseStdoutJson: PostToolUse 写 continue=false = 冒充撤销,schema 错") {
    const std::string stdout_text = R"({"continue": false})";
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::PostToolUse, stdout_text);
    CHECK_FALSE(parsed.ok);
    CHECK(parsed.error.find("continue=false") != std::string::npos);
}

TEST_CASE("ParseStdoutJson: hookEventName 与事件不符 = schema 错") {
    const std::string stdout_text = R"({
        "hookSpecificOutput": {"hookEventName": "Stop", "additionalContext": "hi"}
    })";
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::SessionStart, stdout_text);
    CHECK_FALSE(parsed.ok);
}

TEST_CASE("ParseStdoutJson: 坏 JSON 与非 object stdout 都报 schema 错") {
    CHECK_FALSE(hooks::ParseStdoutJson(hooks::HookEvent::PreToolUse, "not json {").ok);
    CHECK_FALSE(hooks::ParseStdoutJson(hooks::HookEvent::PreToolUse, "[1,2]").ok);
}

TEST_CASE("ParseStdoutJson: Stop 的 additionalContext + stopReason") {
    const std::string stdout_text = R"({
        "continue": false,
        "stopReason": "测试还没跑",
        "hookSpecificOutput": {"hookEventName": "Stop", "additionalContext": "请补跑 ctest"}
    })";
    const auto parsed = hooks::ParseStdoutJson(hooks::HookEvent::Stop, stdout_text);
    REQUIRE(parsed.ok);
    CHECK(parsed.has_continue);
    CHECK_FALSE(parsed.continue_flag);
    CHECK(parsed.stop_reason == "测试还没跑");
    CHECK(parsed.has_additional_context);
    CHECK(parsed.additional_context == "请补跑 ctest");
}

TEST_CASE("JudgeSingleRun: 退出码 0/2/1 三分矩阵") {
    hooks::HookOutput ok_blank;  // 空 stdout
    ok_blank.ok = true;
    auto judged = hooks::JudgeSingleRun(hooks::HookEvent::PreToolUse, 0, false, false, ok_blank, "");
    CHECK(judged.outcome == "ok");
    CHECK(judged.decision == "none");

    judged = hooks::JudgeSingleRun(hooks::HookEvent::PreToolUse, 2, false, false, ok_blank, "危险命令");
    CHECK(judged.outcome == "blocked");
    CHECK(judged.decision == "deny");
    CHECK(judged.detail.find("危险命令") != std::string::npos);

    judged = hooks::JudgeSingleRun(hooks::HookEvent::PreToolUse, 1, false, false, ok_blank, "");
    CHECK(judged.outcome == "failure");
    CHECK(judged.decision == "none");  // 未知退出码不一概当 deny,也不静默当成功

    judged = hooks::JudgeSingleRun(hooks::HookEvent::PreToolUse, 0, true, false, ok_blank, "");
    CHECK(judged.outcome == "timeout");

    judged = hooks::JudgeSingleRun(hooks::HookEvent::PreToolUse, 0, false, true, ok_blank, "");
    CHECK(judged.outcome == "spawn_failed");
}

// ---------------------------------------------------------------------------
// 子进程流的明示解码:UTF-8 优先,候选代码页命中要标注,拿不准留原始字节
// 摘要,绝不无声替换。
// ---------------------------------------------------------------------------

TEST_CASE("DecodeHookStreamBytes: UTF-8 直通,含中文与 emoji") {
    const std::string bytes = "钩子报错:文件不存在 🎉";
    const auto decoded = hooks::DecodeHookStreamBytes(bytes, {936});
    CHECK(decoded.encoding == "utf-8");
    CHECK_FALSE(decoded.from_raw_digest);
    CHECK(decoded.text == bytes);
}

#ifdef _WIN32
// 候选代码页的转码走 Win32 MultiByteToWideChar,POSIX 没有这层——同下面
// "候选页也解不动"一案的待遇,只在 Windows 编。
TEST_CASE("DecodeHookStreamBytes: GBK 字节按明示代码页解出并标注") {
    // "中文" 的 GBK(cp936)编码:D6 D0 CE C4。不是合法 UTF-8。
    const std::string gbk_bytes = std::string("\xD6\xD0\xCE\xC4", 4);
    const auto decoded = hooks::DecodeHookStreamBytes(gbk_bytes, {936});
    CHECK(decoded.encoding == "cp936");
    CHECK_FALSE(decoded.from_raw_digest);
    CHECK(decoded.text == "中文");
}
#endif

#ifdef _WIN32
TEST_CASE("DecodeHookStreamBytes: 候选页也解不动 = 原始字节摘要,不替换") {
    // 0xFF 0xFE 在 UTF-8 与 cp936 里都非法。
    const std::string bad = std::string("\xFF\xFE", 2);
    const auto decoded = hooks::DecodeHookStreamBytes(bad, {936});
    CHECK(decoded.encoding == "unknown");
    CHECK(decoded.from_raw_digest);
    CHECK(decoded.text.find("FF FE") != std::string::npos);
    // 摘要是证据,不是乱码替身:不得出现 U+FFFD。
    CHECK(decoded.text.find("\xEF\xBF\xBD") == std::string::npos);
}
#endif

TEST_CASE("DecodeHookStreamBytes: 空流按契约口径报 utf-8,文本为空") {
    const auto decoded = hooks::DecodeHookStreamBytes("", {936});
    CHECK(decoded.text.empty());
    CHECK_FALSE(decoded.from_raw_digest);
}

// ---------------------------------------------------------------------------
// 3) config:schema 2 解析 + 相加合并 + 迁移提示。
// ---------------------------------------------------------------------------

TEST_CASE("ParseHooksConfig: schema 2 事件键 + exec form + timeout 秒转毫秒") {
    const auto json = nlohmann::json::parse(R"({
        "schema_version": 2,
        "PreToolUse": [
            {
                "matcher": "run_command|write_file",
                "hooks": [
                    {
                        "type": "command",
                        "command": "python",
                        "args": ["${LUBANCODE_PROJECT_DIR}/.lubancode/hooks/check.py"],
                        "timeout": 20,
                        "statusMessage": "检查工具策略"
                    }
                ]
            }
        ],
        "PostToolUse": [
            {"matcher": "write_file", "hooks": [{"command": "fmt", "async": true, "timeout": 60}]}
        ]
    })");
    const auto result = config::ParseHooksConfig(json, "proj.json");
    REQUIRE(result.has_value());
    REQUIRE(result->events.count(hooks::HookEvent::PreToolUse) == 1);
    const auto& groups = result->events.at(hooks::HookEvent::PreToolUse);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].matcher == "run_command|write_file");
    CHECK_FALSE(groups[0].regex);
    REQUIRE(groups[0].hooks.size() == 1);
    CHECK(groups[0].hooks[0].command == "python");
    REQUIRE(groups[0].hooks[0].args.size() == 1);
    CHECK(groups[0].hooks[0].args[0] ==
          "${LUBANCODE_PROJECT_DIR}/.lubancode/hooks/check.py");  // 占位符装载期才替换
    CHECK(groups[0].hooks[0].timeout_ms == 20000);
    CHECK(groups[0].hooks[0].status_message == "检查工具策略");
    CHECK(groups[0].source_path == "proj.json");

    const auto& post = result->events.at(hooks::HookEvent::PostToolUse);
    REQUIRE(post.size() == 1);
    CHECK(post[0].hooks[0].async);
    CHECK(post[0].hooks[0].timeout_ms == 60000);
    CHECK_FALSE(result->HasLegacy());
    CHECK_FALSE(result->Empty());
}

TEST_CASE("ParseHooksConfig: 旧四类与新事件键同文件共存") {
    const auto json = nlohmann::json::parse(R"({
        "pre_tool": [{"matcher": "write_file", "command": "python check.py"}],
        "SessionEnd": [{"hooks": [{"command": "cleanup.sh"}]}]
    })");
    const auto result = config::ParseHooksConfig(json, "g.json");
    REQUIRE(result.has_value());
    CHECK(result->HasLegacy());
    REQUIRE(result->pre_tool.size() == 1);
    CHECK(result->pre_tool[0].source_path == "g.json");
    REQUIRE(result->events.count(hooks::HookEvent::SessionEnd) == 1);
}

TEST_CASE("ParseHooksConfig: 不认得的事件键报错,不假装支持") {
    const auto json = nlohmann::json::parse(R"({"Notification": [{"hooks": [{"command": "x"}]}]})");
    const auto result = config::ParseHooksConfig(json, "g.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("Notification") != std::string::npos);
}

TEST_CASE("ParseHooksConfig: handler 缺 command / type 不是 command / hooks 空数组都报错") {
    CHECK_FALSE(config::ParseHooksConfig(
                    nlohmann::json::parse(R"({"PreToolUse": [{"hooks": [{}]}]})"), "g.json").has_value());
    CHECK_FALSE(config::ParseHooksConfig(
                    nlohmann::json::parse(R"({"PreToolUse": [{"hooks": [{"type": "http", "command": "x"}]}]})"),
                    "g.json")
                    .has_value());
    CHECK_FALSE(config::ParseHooksConfig(nlohmann::json::parse(R"({"PreToolUse": [{"hooks": []}]})"), "g.json")
                    .has_value());
}

TEST_CASE("ParseHooksConfig: 没有匹配字段的事件写具体 matcher 报错") {
    // UserPromptSubmit 没有可匹配字段;Stop 也一样。
    CHECK_FALSE(config::ParseHooksConfig(
                    nlohmann::json::parse(R"({"UserPromptSubmit": [{"matcher": "x", "hooks": [{"command": "c"}]}]})"),
                    "g.json")
                    .has_value());
    CHECK_FALSE(config::ParseHooksConfig(
                    nlohmann::json::parse(R"({"Stop": [{"matcher": "x", "hooks": [{"command": "c"}]}]})"), "g.json")
                    .has_value());
    // SessionStart 有 source 字段,matcher 合法。
    CHECK(config::ParseHooksConfig(
              nlohmann::json::parse(R"({"SessionStart": [{"matcher": "resume", "hooks": [{"command": "c"}]}]})"),
              "g.json")
              .has_value());
}

TEST_CASE("ParseHooksConfig: timeout 越界(0/601/字符串)报错") {
    CHECK_FALSE(config::ParseHooksConfig(
                    nlohmann::json::parse(R"({"Stop": [{"hooks": [{"command": "c", "timeout": 0}]}]})"), "g.json")
                    .has_value());
    CHECK_FALSE(config::ParseHooksConfig(
                    nlohmann::json::parse(R"({"Stop": [{"hooks": [{"command": "c", "timeout": 601}]}]})"), "g.json")
                    .has_value());
}

namespace {

config::FileConfig MakeHooksFile(const std::string& hooks_json_text, const std::string& path) {
    const auto parsed = nlohmann::json::parse(hooks_json_text);
    config::FileConfig file;
    auto hooks_result = config::ParseHooksConfig(parsed, path);
    REQUIRE(hooks_result.has_value());
    file.hooks = std::move(*hooks_result);
    file.source_path = path;
    return file;
}

}  // namespace

TEST_CASE("MergeConfig: hooks 相加——项目级不再整段盖掉全局") {
    // 用 3 参便捷包装分别测"项目级当项目"与"全局当全局",这里直接走
    // MergeConfig 主入口:全局配了 pre_tool+SessionEnd,项目配了 post_tool
    // +PreToolUse,合并后四样都在。
    const auto global_file = MakeHooksFile(
        R"({"pre_tool": [{"matcher": "*", "command": "user-audit.sh"}],
            "SessionEnd": [{"hooks": [{"command": "user-teardown.sh"}]}]})",
        "C:/home/.lubancode/config.json");
    const auto project_file = MakeHooksFile(
        R"({"post_tool": [{"matcher": "write_file", "command": "proj-fmt.sh"}],
            "PreToolUse": [{"matcher": "run_command", "hooks": [{"command": "proj-policy.py"}]}]})",
        "D:/repo/.lubancode/config.json");

    config::LubancodeEnvValues empty_env{};
    const auto merged = config::MergeConfig(empty_env, project_file, global_file);
    REQUIRE(merged.has_value());

    REQUIRE(merged->config.hooks.pre_tool.size() == 1);
    CHECK(merged->config.hooks.pre_tool[0].command == "user-audit.sh");
    CHECK(merged->config.hooks.pre_tool[0].source_path == "C:/home/.lubancode/config.json");
    REQUIRE(merged->config.hooks.post_tool.size() == 1);
    CHECK(merged->config.hooks.post_tool[0].command == "proj-fmt.sh");
    CHECK(merged->config.hooks.post_tool[0].source_path == "D:/repo/.lubancode/config.json");

    REQUIRE(merged->config.hooks.events.count(hooks::HookEvent::SessionEnd) == 1);
    CHECK(merged->config.hooks.events.at(hooks::HookEvent::SessionEnd)[0].source_path ==
          "C:/home/.lubancode/config.json");
    REQUIRE(merged->config.hooks.events.count(hooks::HookEvent::PreToolUse) == 1);
    CHECK(merged->config.hooks.events.at(hooks::HookEvent::PreToolUse)[0].source_path ==
          "D:/repo/.lubancode/config.json");

    // 两边都有旧四类 → 一条迁移说明。
    bool has_merge_notice = false;
    for (const auto& notice : merged->deprecation_notices) {
        if (notice.find("一起生效") != std::string::npos) {
            has_merge_notice = true;
        }
    }
    CHECK(has_merge_notice);
}

TEST_CASE("MergeConfig: 只有一边有旧格式 hooks → 弃用提示(不提相加)") {
    const auto global_file = MakeHooksFile(R"({"pre_tool": [{"command": "x"}]})",
                                            "C:/home/.lubancode/config.json");
    config::LubancodeEnvValues empty_env{};
    const auto merged = config::MergeConfig(empty_env, std::nullopt, global_file);
    REQUIRE(merged.has_value());
    REQUIRE(merged->config.hooks.pre_tool.size() == 1);
    bool has_deprecation = false;
    for (const auto& notice : merged->deprecation_notices) {
        if (notice.find("旧协议已废弃") != std::string::npos) {
            has_deprecation = true;
        }
    }
    CHECK(has_deprecation);
}

// ---------------------------------------------------------------------------
// 4) trust store。
// ---------------------------------------------------------------------------

namespace {

std::string WriteTempFile(const std::string& name, const std::string& content) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-hook-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << content;
    return std::string(reinterpret_cast<const char*>(file.u8string().data()), file.u8string().size());
}

}  // namespace

TEST_CASE("HookTrustStore: 纯内存模式(无路径)记信任、撤信、禁用") {
    auto [store, err] = hooks::HookTrustStore::Load(std::nullopt);
    CHECK_FALSE(err.has_value());
    CHECK_FALSE(store.IsTrusted("D:/repo/.lubancode/config.json", "hash1"));
    store.SetTrusted("D:/repo/.lubancode/config.json", "hash1", "python check.py");
    CHECK(store.IsTrusted("D:/repo/.lubancode/config.json", "hash1"));
    // 同路径不同 hash(命令改了)= 未信任。
    CHECK_FALSE(store.IsTrusted("D:/repo/.lubancode/config.json", "hash2"));
    // 同 hash 不同路径(仓库挪了地方)= 未信任。
    CHECK_FALSE(store.IsTrusted("D:/repo2/.lubancode/config.json", "hash1"));
    CHECK(store.TrustedCommand("D:/repo/.lubancode/config.json", "hash1") == "python check.py");

    store.SetDisabled("D:/repo/.lubancode/config.json", "hash1", true);
    CHECK(store.IsDisabled("D:/repo/.lubancode/config.json", "hash1"));

    store.Untrust("D:/repo/.lubancode/config.json", "hash1");
    CHECK_FALSE(store.IsTrusted("D:/repo/.lubancode/config.json", "hash1"));
}

TEST_CASE("HookTrustStore: 落盘回读(路径 + hash + 命令快照都在)") {
    const std::string path = WriteTempFile("trust-roundtrip.json", "{}");
    {
        auto [store, err] = hooks::HookTrustStore::Load(path);
        REQUIRE_FALSE(err.has_value());
        store.SetTrusted("D:/repo/.lubancode/config.json", "abc123", "python check.py");
        store.SetDisabled("D:/repo/.lubancode/config.json", "dead00", true);
    }
    auto [store2, err2] = hooks::HookTrustStore::Load(path);
    REQUIRE_FALSE(err2.has_value());
    CHECK(store2.IsTrusted("D:/repo/.lubancode/config.json", "abc123"));
    CHECK(store2.TrustedCommand("D:/repo/.lubancode/config.json", "abc123") == "python check.py");
    CHECK(store2.IsDisabled("D:/repo/.lubancode/config.json", "dead00"));
    CHECK_FALSE(store2.IsDisabled("D:/repo/.lubancode/config.json", "ffff00"));
}

TEST_CASE("HookTrustStore: 坏 JSON 账本 = 空白重来,带警告不崩") {
    const std::string path = WriteTempFile("trust-broken.json", "{ not json !!!");
    auto [store, err] = hooks::HookTrustStore::Load(path);
    CHECK(err.has_value());
    CHECK(err->find("读不动") != std::string::npos);
    CHECK(store.trusted_count() == 0);
    CHECK_FALSE(store.IsTrusted("a", "b"));
}

TEST_CASE("HookTrustStore: 文件不存在 = 首访空白,不是错误") {
    auto [store, err] =
        hooks::HookTrustStore::Load("Z:/definitely/not/there/hook-trust.json");
    CHECK_FALSE(err.has_value());
    CHECK(store.trusted_count() == 0);
}

// ---------------------------------------------------------------------------
// 5) loader:分级、未信任、去重、占位符。
// ---------------------------------------------------------------------------

TEST_CASE("LoadHookDefinitions: user 与 project 分级,project 未信任标记") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"*", "user-audit.sh", "C:/home/.lubancode/config.json"});
    hooks.pre_tool.push_back(config::HookEntry{"*", "proj-policy.sh", "D:/repo/.lubancode/config.json"});

    auto [trust, terr] = hooks::HookTrustStore::Load(std::nullopt);
    REQUIRE_FALSE(terr.has_value());
    const auto loaded = hooks::LoadHookDefinitions(hooks, "D:/repo/.lubancode/config.json",
                                                   "C:/home/.lubancode/config.json", "D:/repo", trust);
    REQUIRE(loaded.definitions.size() == 2);
    CHECK(loaded.has_untrusted_project);
    // user 在前(source order),trusted 恒真;project 未信任。
    CHECK(loaded.definitions[0].source_kind == hooks::HookSourceKind::User);
    CHECK(loaded.definitions[0].trusted);
    CHECK(loaded.definitions[1].source_kind == hooks::HookSourceKind::Project);
    CHECK_FALSE(loaded.definitions[1].trusted);
    CHECK(loaded.definitions[1].legacy);
}

TEST_CASE("LoadHookDefinitions: 项目 hook 信任过当前 hash 就起得来") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"*", "proj-policy.sh", "D:/repo/.lubancode/config.json"});

    auto [trust, terr] = hooks::HookTrustStore::Load(std::nullopt);
    // 先用同一 hash 算法定出键,记一笔信任。
    config::HookHandlerConfig handler;
    handler.command = "proj-policy.sh";
    trust.SetTrusted("D:/repo/.lubancode/config.json", hooks::ComputeDefinitionHash(handler), "proj-policy.sh");

    const auto loaded = hooks::LoadHookDefinitions(hooks, "D:/repo/.lubancode/config.json", std::nullopt, "D:/repo",
                                                   trust);
    REQUIRE(loaded.definitions.size() == 1);
    CHECK(loaded.definitions[0].trusted);
    CHECK_FALSE(loaded.has_untrusted_project);
}

TEST_CASE("LoadHookDefinitions: ${LUBANCODE_PROJECT_DIR} 在命令与参数里替换") {
    config::HooksConfig hooks;
    config::HookMatcherGroupConfig group;
    config::HookHandlerConfig handler;
    handler.command = "python";
    handler.args = {"${LUBANCODE_PROJECT_DIR}/.lubancode/hooks/check.py"};
    group.hooks.push_back(handler);
    group.source_path = "C:/home/.lubancode/config.json";
    hooks.events[hooks::HookEvent::PreToolUse].push_back(group);

    auto [trust, terr] = hooks::HookTrustStore::Load(std::nullopt);
    const auto loaded = hooks::LoadHookDefinitions(hooks, std::nullopt, "C:/home/.lubancode/config.json",
                                                   "D:/myrepo", trust);
    REQUIRE(loaded.definitions.size() == 1);
    CHECK(loaded.definitions[0].handler.args[0] == "D:/myrepo/.lubancode/hooks/check.py");
    CHECK_FALSE(loaded.definitions[0].legacy);
    CHECK(loaded.definitions[0].trusted);  // user 来源不走信任审查
}

TEST_CASE("LoadHookDefinitions: 同事件同 hash 不同来源,去重只留一") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"*", "same-cmd.sh", "C:/home/.lubancode/config.json"});
    hooks.pre_tool.push_back(config::HookEntry{"write_file", "same-cmd.sh", "D:/repo/.lubancode/config.json"});

    auto [trust, terr] = hooks::HookTrustStore::Load(std::nullopt);
    const auto loaded = hooks::LoadHookDefinitions(hooks, "D:/repo/.lubancode/config.json",
                                                   "C:/home/.lubancode/config.json", "D:/repo", trust);
    REQUIRE(loaded.definitions.size() == 2);
    CHECK_FALSE(loaded.definitions[0].deduped);  // user 那条先排,执行
    CHECK(loaded.definitions[1].deduped);        // project 同命令,记账不执行
}

TEST_CASE("LoadHookDefinitions: legacy 四类各自映射到事件,session 不带 matcher") {
    config::HooksConfig hooks;
    hooks.session_start.push_back(config::HookEntry{"", "on-start.sh", "C:/home/.lubancode/config.json"});
    hooks.session_end.push_back(config::HookEntry{"", "on-end.sh", "C:/home/.lubancode/config.json"});
    auto [trust, terr] = hooks::HookTrustStore::Load(std::nullopt);
    const auto loaded =
        hooks::LoadHookDefinitions(hooks, std::nullopt, "C:/home/.lubancode/config.json", "D:/repo", trust);
    REQUIRE(loaded.definitions.size() == 2);
    CHECK(loaded.definitions[0].event == hooks::HookEvent::SessionStart);
    CHECK(loaded.definitions[0].matcher.empty());
    CHECK(loaded.definitions[0].handler.timeout_ms == 30000);
    CHECK(loaded.definitions[1].event == hooks::HookEvent::SessionEnd);
}
