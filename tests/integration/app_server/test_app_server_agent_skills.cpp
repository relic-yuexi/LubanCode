// 应用Worker接入单 P2 的整链验收(AW-05/06/08/09/10 的首棒)。
//
// 与 test_app_server_profile_smoke.cpp(P1:档面+MCP 工具)的差别:这里钉
// P2 四路新接线在真 exe 上走通——
//   1. Agent 装配(GAP-01/02):agentRef 解析到真档案,系统提示的正文来自
//      档案点名的 Prompt Profile(core 身份段),编码助手默认人格不混入;
//      宿主能力段按实际工具面照注(§5.2 覆盖合同);
//   2. Skill 装配(GAP-03):features.skills + allow 点名 skill 才进面;
//      清单段/预装正文进提示;skill 工具按需加载正文;加载技能不新增
//      shell 一类执行工具(AW-08);
//   3. MCP 完整协议链(AW-09):真 python 夹具——initialize 握手后
//      tools/list 复验、tools/call 真执行、协议错误(rich unknown kind 的
//      JSON-RPC error)翻译进工具结果回到模型;连接失败/工具缺名明拒在
//      册外另有(unit test_session_assembly + smoke 用例 3);
//   4. Lua 显式拒绝(AW-10):components.plugins 点名未接线插件,
//      thread/start 回 component_unavailable,不忽略不冒充;
//   5. agentRef 缺件(AW-06 首棒):指名档案不存在,进程拒绝启动。
//
// 缺 lubancode 可执行文件或缺 python 的环境整案跳过(与 smoke 册同口径)。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_http_server.hpp"
#include "interactive_process.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string FindLubancodeBinary() {
#ifdef LUBANCODE_BINARY_DIR
    std::error_code ec;
    const fs::path root = fs::path(LUBANCODE_BINARY_DIR);
    for (const char* name :
         {"lubancode", "lubancode.exe", "Debug/lubancode.exe", "Release/lubancode.exe"}) {
        const fs::path candidate = root / name;
        if (fs::exists(candidate, ec)) {
            return lubancode::platform::PathToUtf8(candidate);
        }
    }
#endif
    return std::string();
}

constexpr const char* kPythonCmd =
#ifdef _WIN32
    "python";
#else
    "python3";
#endif

bool PythonAvailable() {
    const auto probe = lubancode::platform::RunProcessWithStdin(
        std::vector<std::string>{kPythonCmd, "--version"}, std::string(), 10000);
    return probe.exit_code == 0;
}

std::string McpFixturePath() {
#ifdef LUBANCODE_TEST_FIXTURES_DIR
    return std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/mcp_test_server.py";
#else
    return std::string("mcp_test_server.py");
#endif
}

std::string SseBody(const std::vector<std::string>& frames) {
    std::string body;
    for (const std::string& frame : frames) {
        body += "data: " + frame + "\n\n";
    }
    body += "data: [DONE]\n\n";
    return body;
}

lubancode::test_support::FakeHttpResponse SseResponse(std::vector<std::string> frames) {
    lubancode::test_support::FakeHttpResponse response;
    response.status = 200;
    response.headers.emplace_back("Content-Type", "text/event-stream");
    response.body = SseBody(std::move(frames));
    return response;
}

// 一帧 tool_calls delta(chat wire)。call_id/name/arguments 拼进流。
std::string ToolCallFrame(const std::string& call_id, const std::string& tool,
                          const std::string& arguments_json) {
    std::string frame = R"({"id":"c1","choices":[{"index":0,"delta":{"role":"assistant","tool_calls":[)";
    frame += R"({"index":0,"id":")" + call_id + R"(","type":"function","function":{"name":")" + tool +
             R"(","arguments":")" + arguments_json + R"("}})";
    frame += R"(]},"finish_reason":null}]})";
    return frame;
}

const std::string kFinishToolCalls = R"({"id":"c1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})";

std::string TextFrame(const std::string& text) {
    return R"({"id":"c2","choices":[{"index":0,"delta":{"role":"assistant","content":")" + text +
           R"(","finish_reason":null}]})";
}

const std::string kFinishStop = R"({"id":"c2","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})";
const std::string kUsageFrame =
    R"({"id":"c2","choices":[],"usage":{"prompt_tokens":13,"completion_tokens":5,"total_tokens":18}})";

// P2 冒烟场:临时材料根(~/.lubancode 同构)+ 假模型 + 交互进程。
struct AgentSkillsField {
    fs::path temp_root;
    fs::path home_dir;
    fs::path deployment_path;
    lubancode::test_support::FakeHttpServer model;
    std::unique_ptr<lubancode::test_support::InteractiveProcess> proc;
    std::vector<json> events;
    std::vector<json> responses;

    AgentSkillsField() {
        auto root = fs::temp_directory_path();
        root /= "lubancode_agent_skills";
        std::error_code ec;
        fs::remove_all(root, ec);
        temp_root = root;
        home_dir = root / "home";
        fs::create_directories(home_dir / ".lubancode", ec);
    }

    ~AgentSkillsField() {
        proc.reset();
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void WriteFile(const fs::path& path, const std::string& content) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    // 全局 config:chat wire 指假模型;MCP 配 python 夹具。
    void WriteGlobalConfig() {
        json approved;
        approved["command"] = kPythonCmd;
        approved["args"] = json::array({McpFixturePath()});
        json config = {{"wire", "chat"},
                       {"base_url", "http://127.0.0.1:" + std::to_string(model.port())},
                       {"model", "fake-chat-model"},
                       {"api_key", "sk-agent-skills"},
                       {"mcpServers", {{"tools-approved", approved}}}};
        WriteFile(home_dir / ".lubancode" / "config.json", config.dump());
    }

    // Agent 档案(点名 Prompt Profile research + 预装技能 greet)+ Profile
    // 业务正文 + 一份 Skill 正文。
    void WriteAgentMaterials() {
        WriteFile(home_dir / ".lubancode" / "agents" / "research.yaml",
                  "schema: 1\n"
                  "name: research\n"
                  "description: 研究助理档案。\n"
                  "prompt:\n"
                  "  profile: research\n"
                  "skills:\n"
                  "  preload:\n"
                  "    - greet\n");
        WriteFile(home_dir / ".lubancode" / "prompts" / "profiles" / "research" / "core" / "10-identity.md",
                  "# 身份\n\n你是 RESEARCH-PROFILE-IDENTITY,研究档案的业务正文。\n");
        WriteFile(home_dir / ".lubancode" / "skills" / "greet" / "SKILL.md",
                  "---\nname: greet\ndescription: 问候技能,验收用。\n---\n"
                  "GREET-SKILL-BODY:见到任务先问好,再干活。\n");
    }

    void WriteDeployment(const std::string& deployment_json) {
        deployment_path = temp_root / "deployment.json";
        WriteFile(deployment_path, deployment_json);
    }

    bool SpawnServer(const std::string& binary, std::string* error) {
        std::vector<std::string> argv{binary, "app-server", "--yes", "--app-server-profile",
                                      lubancode::platform::PathToUtf8(deployment_path)};
        std::vector<std::pair<std::string, std::string>> env;
#ifdef _WIN32
        env.emplace_back("USERPROFILE", lubancode::platform::PathToUtf8(home_dir));
#else
        env.emplace_back("HOME", lubancode::platform::PathToUtf8(home_dir));
#endif
        proc = lubancode::test_support::InteractiveProcess::Spawn(argv, env, std::string(), error);
        return proc != nullptr;
    }

    bool Send(const std::string& line) { return proc->WriteLine(line); }

    bool PumpUntil(const std::function<bool()>& hit, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (hit()) {
                return true;
            }
            const std::optional<std::string> line = proc->ReadLine(200);
            if (!line.has_value()) {
                continue;
            }
            const json message = json::parse(*line, nullptr, /*allow_exceptions=*/false);
            if (message.is_discarded()) {
                continue;
            }
            responses.push_back(message);
            if (message.contains("method") && message.contains("params")) {
                events.push_back(message);
            }
        }
        return hit();
    }

    const json* FindResponse(int id) const {
        for (const json& message : responses) {
            if (message.contains("id") && message["id"] == id) {
                return &message;
            }
        }
        return nullptr;
    }

    const json* FindEvent(const std::string& method) const {
        for (const json& event : events) {
            if (event.contains("method") && event["method"] == method) {
                return &event;
            }
        }
        return nullptr;
    }

    // 握手+开 thread;thread/start 明拒时回错误响应指针。
    const json* StartThread() {
        if (!Send(R"({"id":1,"method":"initialize","params":{"clientName":"p2-agent-skills"}})")) {
            return nullptr;
        }
        Send(R"({"method":"initialized"})");
        if (!Send(R"({"id":2,"method":"thread/start","params":{}})")) {
            return nullptr;
        }
        PumpUntil([&] { return FindResponse(2) != nullptr; }, 30000);
        return FindResponse(2);
    }
};

// 部署档:agentRef research,features 放行 mcp+skills,allow = echo+skill。
std::string AgentSkillsDeployment() {
    json deployment = {
        {"schemaVersion", 1},
        {"service", {{"mode", "managed"},
                     {"listeners", {{"stdio", {{"enabled", true}}}}},
                     {"defaultProfile", "smoke"}}},
        {"harnessProfiles",
         {{"smoke",
           {{"agentRef", "research"},
            {"features",
             {{"default", "disabled"}, {"enabled", json::array({"mcp", "skills"})}, {"disabled", json::array()}}},
            {"components", {{"mcpServers", json::array({"tools-approved"})}}},
            {"tools",
             {{"mode", "only"}, {"allow", json::array({"mcp:tools-approved:echo", "skill"})}, {"deny", json::array()}}},
            {"exposure", {{"default", "direct"}}}}}}}};
    return deployment.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// 用例 1:Agent 正文 + Skill 工具 + MCP 调用整链(AW-05/AW-08/AW-09)
// ---------------------------------------------------------------------------
TEST_CASE("P2 整链:档案正文进提示,skill 工具真加载,MCP echo 真执行") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty() || !PythonAvailable()) {
        return;
    }
    AgentSkillsField field;
    field.WriteGlobalConfig();
    field.WriteAgentMaterials();
    field.WriteDeployment(AgentSkillsDeployment());
    // 假模型三幕:幕1 调 skill 工具加载 greet,幕2 调 MCP echo,幕3 终答。
    field.model.Enqueue(SseResponse({ToolCallFrame("call-skill-1", "skill", R"({\"name\":\"greet\"})"),
                                     kFinishToolCalls}));
    field.model.Enqueue(SseResponse({ToolCallFrame("call-echo-1", "mcp__tools-approved__echo",
                                                   R"({\"text\":\"PING\"})"),
                                     kFinishToolCalls}));
    field.model.Enqueue(SseResponse({TextFrame("p2-final-answer"), kFinishStop, kUsageFrame}));

    std::string spawn_error;
    REQUIRE(field.SpawnServer(binary, &spawn_error));
    const json* thread_response = field.StartThread();
    REQUIRE(thread_response != nullptr);
    if (!thread_response->contains("result")) {
        MESSAGE("thread/start 错误响应: ", thread_response->dump());
        MESSAGE("服务端 stderr: ", field.proc->StderrText());
    }
    REQUIRE(thread_response->contains("result"));
    const std::string thread_id = (*thread_response)["result"].value("threadId", std::string());
    REQUIRE_FALSE(thread_id.empty());

    REQUIRE(field.Send(json{{"id", 3},
                            {"method", "turn/start"},
                            {"params", json{{"threadId", thread_id}, {"text", "加载 greet 并调 echo"}}}}
                               .dump()));
    REQUIRE(field.PumpUntil([&] { return field.FindEvent("turn/completed") != nullptr; }, 60000));
    const json* completed = field.FindEvent("turn/completed");
    REQUIRE(completed != nullptr);
    CHECK((*completed)["params"].value("executionStatus", std::string()) == "success");

    const std::vector<lubancode::test_support::FakeHttpRequest> requests = field.model.requests();
    REQUIRE(requests.size() == 3);  // skill 步 + echo 步 + 终答步
    const json first_body = json::parse(requests[0].body, nullptr, false);
    REQUIRE_FALSE(first_body.is_discarded());

    // ---- AW-05:首请求的系统提示 ----
    REQUIRE(first_body.contains("messages"));
    REQUIRE(first_body["messages"].is_array());
    REQUIRE_FALSE(first_body["messages"].empty());
    REQUIRE(first_body["messages"][0].contains("content"));
    const std::string system_text = first_body["messages"][0]["content"].get<std::string>();
    // 档案业务正文(Profile core)在场;编码助手默认人格不混入。
    CHECK(system_text.find("RESEARCH-PROFILE-IDENTITY") != std::string::npos);
    CHECK(system_text.find("命令行 AI 编程助手") == std::string::npos);
    // 宿主能力段按实际工具面照注(mcp 段恒在,盖不掉)。
    CHECK(system_text.find("外接工具(MCP)") != std::string::npos);
    // 技能清单段:获准清单进提示材料,来源注记如实(只认材料根一处)。
    CHECK(system_text.find("greet") != std::string::npos);
    CHECK(system_text.find("部署材料根的 skills/ 目录") != std::string::npos);
    // skills.preload 预装正文(eager 注入)。
    CHECK(system_text.find("GREET-SKILL-BODY") != std::string::npos);
    // 运行环境段(宿主段)在场。
    CHECK(system_text.find("工作目录") != std::string::npos);

    // ---- 工具面:恰是档点名的一枚 MCP 工具 + skill,无任何执行类工具 ----
    REQUIRE(first_body.contains("tools"));
    std::vector<std::string> tool_names;
    for (const auto& tool : first_body["tools"]) {
        if (tool.contains("function") && tool["function"].contains("name")) {
            tool_names.push_back(tool["function"]["name"].get<std::string>());
        }
    }
    REQUIRE(tool_names.size() == 2);
    bool has_skill = false, has_echo = false;
    for (const std::string& name : tool_names) {
        has_skill = has_skill || name == "skill";
        has_echo = has_echo || name == "mcp__tools-approved__echo";
    }
    CHECK(has_skill);
    CHECK(has_echo);  // AW-08:加载技能不新增 run_command 一类(面上只有这两枚)

    // ---- 幕1 真执行:skill 工具结果(正文)进幕2 请求 ----
    CHECK(requests[1].body.find("GREET-SKILL-BODY") != std::string::npos);
    // ---- 幕2 真执行:MCP echo 回显(PING)进幕3 请求(AW-09 调用链)----
    CHECK(requests[2].body.find("PING") != std::string::npos);

    REQUIRE(field.Send(R"({"id":9,"method":"shutdown","params":{}})"));
    int exit_code = -1;
    REQUIRE(field.proc->Wait(15000, &exit_code));
    CHECK(exit_code == 0);
}

// ---------------------------------------------------------------------------
// 用例 2:MCP 协议错误链——工具回 JSON-RPC error,翻译进工具结果回模型
// ---------------------------------------------------------------------------
TEST_CASE("P2 MCP 错误链:rich(unknown kind) 的 -32602 回进下一轮请求") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty() || !PythonAvailable()) {
        return;
    }
    AgentSkillsField field;
    field.WriteGlobalConfig();
    json deployment = json::parse(AgentSkillsDeployment(), nullptr, false);
    REQUIRE_FALSE(deployment.is_discarded());
    deployment["harnessProfiles"]["smoke"]["tools"]["allow"] =
        json::array({"mcp:tools-approved:rich"});
    deployment["harnessProfiles"]["smoke"]["features"]["enabled"] = json::array({"mcp"});
    deployment["harnessProfiles"]["smoke"]["agentRef"] = "general-purpose";  // 码内内置档案
    field.WriteDeployment(deployment.dump());
    // 幕1:调 rich 给不认得的 kind——夹具回 JSON-RPC error(-32602)。
    field.model.Enqueue(SseResponse({ToolCallFrame("call-rich-1", "mcp__tools-approved__rich",
                                                   R"({\"kind\":\"bogus\"})"),
                                     kFinishToolCalls}));
    field.model.Enqueue(SseResponse({TextFrame("rich-error-final"), kFinishStop}));

    std::string spawn_error;
    REQUIRE(field.SpawnServer(binary, &spawn_error));
    const json* thread_response = field.StartThread();
    REQUIRE(thread_response != nullptr);
    REQUIRE(thread_response->contains("result"));
    const std::string thread_id = (*thread_response)["result"].value("threadId", std::string());

    REQUIRE(field.Send(json{{"id", 3},
                            {"method", "turn/start"},
                            {"params", json{{"threadId", thread_id}, {"text", "调 rich 给 bogus"}}}}
                               .dump()));
    REQUIRE(field.PumpUntil([&] { return field.FindEvent("turn/completed") != nullptr; }, 60000));

    const std::vector<lubancode::test_support::FakeHttpRequest> requests = field.model.requests();
    REQUIRE(requests.size() == 2);
    // 错误结果带着服务端的人话回到模型(协议错误不吞、不炸)。
    CHECK(requests[1].body.find("unknown kind") != std::string::npos);

    REQUIRE(field.Send(R"({"id":9,"method":"shutdown","params":{}})"));
    int exit_code = -1;
    REQUIRE(field.proc->Wait(15000, &exit_code));
    CHECK(exit_code == 0);
}

// ---------------------------------------------------------------------------
// 用例 3:点名未接线的 Lua 插件——component_unavailable,不忽略不冒充
// ---------------------------------------------------------------------------
TEST_CASE("P2 Lua 边界:点名插件 thread/start 明拒 component_unavailable") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty() || !PythonAvailable()) {
        return;
    }
    AgentSkillsField field;
    field.WriteGlobalConfig();
    json deployment = {
        {"schemaVersion", 1},
        {"service", {{"mode", "managed"},
                     {"listeners", {{"stdio", {{"enabled", true}}}}},
                     {"defaultProfile", "smoke"}}},
        {"harnessProfiles",
         {{"smoke",
           {{"agentRef", "general-purpose"},
            {"features",
             {{"default", "disabled"}, {"enabled", json::array({"plugins"})}, {"disabled", json::array()}}},
            {"components", {{"plugins", json::array({"demo.lua-tool"})}}},
            {"tools", {{"mode", "none"}, {"deny", json::array()}}},
            {"exposure", {{"default", "direct"}}}}}}}};
    field.WriteDeployment(deployment.dump());

    std::string spawn_error;
    REQUIRE(field.SpawnServer(binary, &spawn_error));
    const json* thread_response = field.StartThread();
    REQUIRE(thread_response != nullptr);
    REQUIRE(thread_response->contains("error"));
    CHECK_FALSE(thread_response->contains("result"));
    const json& error = (*thread_response)["error"];
    REQUIRE(error.contains("message"));
    CHECK(error["message"].get<std::string>().find("component_unavailable") != std::string::npos);
    if (error.contains("data") && error["data"].contains("code")) {
        CHECK(error["data"]["code"] == "component_unavailable");  // 机器可读码(additive)
    }
    // 零模型请求:组件没装上就不许碰模型。
    CHECK(field.model.requests().empty());

    REQUIRE(field.Send(R"({"id":9,"method":"shutdown","params":{}})"));
    int exit_code = -1;
    REQUIRE(field.proc->Wait(15000, &exit_code));
    CHECK(exit_code == 0);
}

// ---------------------------------------------------------------------------
// 用例 4:agentRef 指名的档案不存在——进程拒绝启动(AW-06 首棒)
// ---------------------------------------------------------------------------
TEST_CASE("P2 档案缺件:agentRef 不存在,拒绝启动不回落默认") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty()) {
        return;
    }
    AgentSkillsField field;
    field.WriteGlobalConfig();
    // 材料根里没有 no-such-agent 档案(码内内置只有 general-purpose/Explore)。
    json deployment = json::parse(AgentSkillsDeployment(), nullptr, false);
    deployment["harnessProfiles"]["smoke"]["agentRef"] = "no-such-agent";
    field.WriteDeployment(deployment.dump());

    std::string spawn_error;
    REQUIRE(field.SpawnServer(binary, &spawn_error));
    int exit_code = 0;
    REQUIRE(field.proc->Wait(15000, &exit_code));
    CHECK(exit_code != 0);
    const std::string stderr_text = field.proc->StderrText();
    CHECK(stderr_text.find("no-such-agent") != std::string::npos);
    CHECK(stderr_text.find("拒绝启动") != std::string::npos);
    CHECK(field.model.requests().empty());
}
