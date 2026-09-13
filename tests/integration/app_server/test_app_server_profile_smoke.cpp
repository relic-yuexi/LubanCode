// 部署档生产装配的真进程冒烟(工业化多协议接入单 P1 退出条件)。
//
// 与 test_app_server_smoke.cpp(纯握手)的差别:这里钉的是"真实生产启动
// 命令按两份 Profile 跑通"——真 exe + 假模型后端(本机回环 FakeHttpServer
// 扮 chat wire)+ 真 MCP 服务(python 夹具 mcp_test_server.py)走一次多轮
// 工具调用,记录工具实际执行次数。不是注入 fake registry 的单测。
//
// 四个用例:
//   1. minimal-tools:--app-server-profile 点名最小工具档。两幕假模型
//      (幕1 回 echo tool_call,幕2 回终答文本)——断言模型请求里只见
//      档点名的两只 MCP 工具(不见终端 run_command 全家)、echo 真执行
//      (幕2 请求里带工具结果)、turn/completed 带 executionStatus/
//      finalMessageRefs/usageReported、可选降级写入 thread/started;
//   2. zero-tools:零工具档——模型请求无 tools 定义,一回终答;
//   3. 档点名服务里不存在的工具(thread 复验):thread/start 明拒,
//      进程不炸;
//   4. 坏档(不存在的档名):进程拒绝启动,退出码非 0。
//
// 缺 lubancode 可执行文件或缺 python 的环境整案跳过(与 smoke 册、
// test_mcp_client.cpp 同款口径)。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
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

// 测试可执行文件旁边的 build 树根(test_app_server_smoke.cpp 同款)。
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

// 真夹具用哪个 python(test_mcp_client.cpp 同款:Windows 是 python.exe,
// Linux/macOS 惯例 python3)。
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

// chat SSE 响应:一串 data: 帧拼一份 body。
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

// 幕1:模型点名调 echo(工具真执行的全链起点)。
std::vector<std::string> ToolCallFrames() {
    return {
        R"({"id":"c1","choices":[{"index":0,"delta":{"role":"assistant","tool_calls":[{"index":0,"id":"call-echo-1","type":"function","function":{"name":"mcp__tools-approved__echo","arguments":"{\"text\":\"PING\"}"}}]},"finish_reason":null}]})",
        R"({"id":"c1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})",
    };
}

// 幕2:终答文本 + usage(空 choices 的 usage 帧,include_usage 惯例)。
std::vector<std::string> FinalTextFrames() {
    return {
        R"({"id":"c2","choices":[{"index":0,"delta":{"role":"assistant","content":"smoke-final-answer"},"finish_reason":null}]})",
        R"({"id":"c2","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})",
        R"({"id":"c2","choices":[],"usage":{"prompt_tokens":11,"completion_tokens":7,"total_tokens":18}})",
    };
}

// 一次性纯文本幕(zero-tools 档用)。
std::vector<std::string> SingleTextFrames() {
    return {
        R"({"id":"c3","choices":[{"index":0,"delta":{"role":"assistant","content":"zero-tools-answer"},"finish_reason":null}]})",
        R"({"id":"c3","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})",
    };
}

// 冒烟场:临时 home(全局 config 注入)+ 部署档 + 交互进程 + 假模型。
struct ProfileSmoke {
    fs::path temp_root;
    fs::path home_dir;
    fs::path deployment_path;
    lubancode::test_support::FakeHttpServer model;
    std::unique_ptr<lubancode::test_support::InteractiveProcess> proc;
    std::vector<json> events;      // 服务端事件(params 层)
    std::vector<json> responses;   // 服务端响应(全信封)

    ProfileSmoke() {
        auto root = fs::temp_directory_path();
        root /= "lubancode_profile_smoke";
        std::error_code ec;
        fs::remove_all(root, ec);
        temp_root = root;
        home_dir = root / "home";
        fs::create_directories(home_dir / ".lubancode", ec);
        // P2(应用Worker接入单 §五):agentRef 必须解析到可用档案——冒烟场
        // 在材料根(~/.lubancode 同构的临时树)里种一份最小档案,档名指它。
        WriteAgentMaterials();
    }

    // Agent 档案 + 它点名的 Prompt Profile 业务正文(P2 装配吃这两层)。
    void WriteAgentMaterials() {
        std::error_code ec;
        const fs::path agents_dir = home_dir / ".lubancode" / "agents";
        fs::create_directories(agents_dir, ec);
        const fs::path profile_core =
            home_dir / ".lubancode" / "prompts" / "profiles" / "smoke" / "core";
        fs::create_directories(profile_core, ec);
        {
            std::ofstream out(agents_dir / "smoke-assistant.yaml", std::ios::binary);
            out << "schema: 1\n"
                   "name: smoke-assistant\n"
                   "description: profile 冒烟场的最小档案。\n"
                   "prompt:\n"
                   "  profile: smoke\n";
        }
        {
            std::ofstream out(profile_core / "10-identity.md", std::ios::binary);
            out << "# 身份\n\n你是 SMOKE-PROFILE-IDENTITY,部署档点名的业务档案正文。\n";
        }
    }

    ~ProfileSmoke() {
        // 进程析构自会杀树;残留目录按保留期思路清一道(孤儿目录不认作
        // 已受理,直接回收)。
        if (proc != nullptr) {
            proc.reset();
        }
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    // 全局 config:chat wire 指假模型;MCP 配 tools-approved(python 夹具)
    // 与 flaky(不存在命令——可选降级变体)。键名沿 config.json 顶层合同
    //(mcpServers,驼峰)。
    void WriteGlobalConfig(bool with_flaky) {
        json mcp;
        json approved;
        approved["command"] = kPythonCmd;
        approved["args"] = json::array({McpFixturePath()});
        mcp["tools-approved"] = approved;
        if (with_flaky) {
            json flaky;
            flaky["command"] = "definitely-no-such-command-xyz";
            mcp["flaky"] = flaky;
        }
        json config = {{"wire", "chat"},
                       {"base_url", "http://127.0.0.1:" + std::to_string(model.port())},
                       {"model", "fake-chat-model"},
                       {"api_key", "sk-smoke"},
                       {"mcpServers", std::move(mcp)}};
        std::ofstream out(home_dir / ".lubancode" / "config.json", std::ios::binary);
        out << config.dump();
    }

    void WriteDeployment(const std::string& profile_json) {
        deployment_path = temp_root / "deployment.json";
        std::ofstream out(deployment_path, std::ios::binary);
        out << profile_json;
    }

    // 起真 exe。binary_profile=true 时带 --app-server-profile(缺 --yes 的
    // 审批悬停由 --yes 显式全放解决——与终端同语义)。
    bool SpawnServer(const std::string& binary, bool with_profile_flag, std::string* error) {
        std::vector<std::string> argv{binary, "app-server", "--yes"};
        if (with_profile_flag) {
            argv.push_back("--app-server-profile");
            argv.push_back(lubancode::platform::PathToUtf8(deployment_path));
        }
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

    // 等事件/响应:收行直到谓词命中(事件账持续累积),超时 false。
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
};

// 一段部署档 JSON(inline 造,基线取 P0 golden 的形状)。
std::string MinimalToolsDeployment(bool with_flaky) {
    json disabled = json::array({"process.exec", "filesystem.write", "browser", "subagents", "ptc",
                                 "memory.read", "memory.write", "goal", "loop", "workflow"});
    json servers = json::array({"tools-approved"});
    if (with_flaky) {
        servers.push_back("flaky");
    }
    json deployment = {
        {"schemaVersion", 1},
        {"service", {{"mode", "managed"},
                     {"listeners", {{"stdio", {{"enabled", true}}}}},
                     {"defaultProfile", "smoke"}}},
        {"harnessProfiles",
         {{"smoke",
           {{"agentRef", "smoke-assistant"},
            {"features", {{"default", "disabled"}, {"enabled", json::array({"mcp"})}, {"disabled", disabled}}},
            {"components", {{"mcpServers", servers}}},
            {"tools",
             {{"mode", "only"},
              {"allow", json::array({"mcp:tools-approved:echo", "mcp:tools-approved:describe"})},
              {"deny", json::array()}}},
            {"exposure", {{"default", "direct"}}},
            {"limits", {{"stepsPerInput", 6}, {"wallTimeMs", 30000}, {"toolCallsPerInput", 8}, {"maxOutputTokens", 1200}}}}}}}};
    return deployment.dump();
}

std::string ZeroToolsDeployment() {
    json disabled = json::array({"process.exec", "filesystem.write", "browser", "subagents", "ptc",
                                 "memory.read", "memory.write", "goal", "loop", "workflow"});
    json deployment = {
        {"schemaVersion", 1},
        {"service", {{"mode", "managed"},
                     {"listeners", {{"stdio", {{"enabled", true}}}}},
                     {"defaultProfile", "smoke"}}},
        {"harnessProfiles",
         {{"smoke",
           {{"agentRef", "smoke-assistant"},
            {"features", {{"default", "disabled"}, {"enabled", json::array()}, {"disabled", disabled}}},
            {"components", json::object()},
            {"tools", {{"mode", "none"}, {"deny", json::array()}}},
            {"exposure", {{"default", "direct"}}},
            {"limits", {{"stepsPerInput", 6}, {"wallTimeMs", 30000}, {"toolCallsPerInput", 8}, {"maxOutputTokens", 1200}}}}}}}};
    return deployment.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// 用例 1:minimal-tools 档,多轮工具调用真跑通
// ---------------------------------------------------------------------------
TEST_CASE("profile 冒烟:minimal-tools——真 exe+假模型+真 MCP,多轮工具调用") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty() || !PythonAvailable()) {
        return; // 缺主程序或缺 python:跳过,不冒充通过
    }
    ProfileSmoke smoke;
    smoke.WriteGlobalConfig(/*with_flaky=*/true);
    smoke.WriteDeployment(MinimalToolsDeployment(/*with_flaky=*/true));
    // 假模型两幕:幕1 点名 echo,幕2 终答。
    smoke.model.Enqueue(SseResponse(ToolCallFrames()));
    smoke.model.Enqueue(SseResponse(FinalTextFrames()));

    std::string spawn_error;
    REQUIRE(smoke.SpawnServer(binary, /*with_profile_flag=*/true, &spawn_error));

    // 握手 → thread/start(turn/started 带 threadId;可选降级 flaky 记账)。
    REQUIRE(smoke.Send(R"({"id":1,"method":"initialize","params":{"clientName":"p1-smoke"}})"));
    REQUIRE(smoke.Send(R"({"method":"initialized"})"));
    REQUIRE(smoke.Send(R"({"id":2,"method":"thread/start","params":{}})"));
    REQUIRE(smoke.PumpUntil([&] { return smoke.FindResponse(2) != nullptr; }, 30000));
    const json* thread_response = smoke.FindResponse(2);
    REQUIRE(thread_response != nullptr);
    if (!thread_response->contains("result")) {
        // 诊断落账:错误响应全文与服务端 stderr,排障不猜。
        MESSAGE("thread/start 错误响应: ", thread_response->dump());
        MESSAGE("服务端 stderr: ", smoke.proc->StderrText());
    }
    REQUIRE(thread_response->contains("result"));
    const std::string thread_id = (*thread_response)["result"].value("threadId", std::string());
    REQUIRE_FALSE(thread_id.empty());
    // 可选降级必须写结果:flaky(档点名、tools.allow 未引用)起服失败,
    // 如实进响应(与 thread/started 事件)。
    REQUIRE((*thread_response)["result"].contains("degradedComponents"));

    // turn/start:等 turn/completed 事件(交互驱动保住 stdin 写端,
    // 回合完才走下一步——这就是"真进程协议测试"与一把梭喂完关管的差别)。
    REQUIRE(smoke.Send(json{{"id", 3},
                            {"method", "turn/start"},
                            {"params", json{{"threadId", thread_id}, {"text", "请调用 echo 说 PING"}}}}
                               .dump()));
    REQUIRE(smoke.PumpUntil(
        [&] {
            const json* completed = smoke.FindEvent("turn/completed");
            return completed != nullptr;
        },
        60000));
    const json* completed = smoke.FindEvent("turn/completed");
    REQUIRE(completed != nullptr);
    const json& params = (*completed)["params"];
    CHECK(params.value("status", std::string()) == "success");
    CHECK(params.value("executionStatus", std::string()) == "success");
    CHECK(params.contains("finalMessageRefs"));
    CHECK(params["finalMessageRefs"].is_array());
    CHECK_FALSE(params["finalMessageRefs"].empty());
    CHECK(params.contains("usageReported"));   // 字段在;值随 provider 报没报
    CHECK(params.value("stepsUsed", 0) >= 2);  // 至少两步:工具步 + 终答步
    CHECK_FALSE(params.contains("resultEnvelopePersisted"));  // 落稳了不另发字段

    // ---- 工具实际执行次数与请求面(模型的真账)----
    const std::vector<lubancode::test_support::FakeHttpRequest> requests = smoke.model.requests();
    REQUIRE(requests.size() == 2);  // 多轮:工具结果触发第二次模型请求
    const json first_body = json::parse(requests[0].body, nullptr, false);
    const json second_body = json::parse(requests[1].body, nullptr, false);
    REQUIRE_FALSE(first_body.is_discarded());
    REQUIRE_FALSE(second_body.is_discarded());

    // 第一笔:tools 数组恰是档点名的两只(echo+describe 的 MCP wire 名),
    // 绝无终端全工具(run_command/write_file 不见面)。
    REQUIRE(first_body.contains("tools"));
    std::vector<std::string> tool_names;
    for (const auto& tool : first_body["tools"]) {
        if (tool.contains("function") && tool["function"].contains("name")) {
            tool_names.push_back(tool["function"]["name"].get<std::string>());
        }
    }
    REQUIRE(tool_names.size() == 2);
    bool has_echo = false, has_describe = false;
    for (const std::string& name : tool_names) {
        has_echo = has_echo || name == "mcp__tools-approved__echo";
        has_describe = has_describe || name == "mcp__tools-approved__describe";
    }
    CHECK(has_echo);
    CHECK(has_describe);

    // P2(AW-05 首棒):系统提示来自档案业务正文——档点名的 Prompt Profile
    // 的 core 身份段在场,编码助手默认人格不混入;宿主能力段(mcp)按
    // 实际工具面照注。chat wire 的 system 落消息流头一条。
    REQUIRE(first_body.contains("messages"));
    REQUIRE(first_body["messages"].is_array());
    REQUIRE_FALSE(first_body["messages"].empty());
    REQUIRE(first_body["messages"][0].contains("content"));
    const std::string system_text = first_body["messages"][0]["content"].get<std::string>();
    CHECK(system_text.find("SMOKE-PROFILE-IDENTITY") != std::string::npos);
    CHECK(system_text.find("命令行 AI 编程助手") == std::string::npos);
    CHECK(system_text.find("外接工具(MCP)") != std::string::npos);  // mcp 能力段按工具面照注

    // 第二笔:幕1 的 echo 真执行了——工具结果(PING 回显)进了下一轮请求。
    const std::string second_body_text = requests[1].body;
    CHECK(second_body_text.find("PING") != std::string::npos);

    // 干净收线。
    REQUIRE(smoke.Send(R"({"id":9,"method":"shutdown","params":{}})"));
    int exit_code = -1;
    REQUIRE(smoke.proc->Wait(15000, &exit_code));
    CHECK(exit_code == 0);
}

// ---------------------------------------------------------------------------
// 用例 2:zero-tools 档——模型请求无工具定义
// ---------------------------------------------------------------------------
TEST_CASE("profile 冒烟:zero-tools——模型请求零工具,零 MCP 启动") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty() || !PythonAvailable()) {
        return;
    }
    ProfileSmoke smoke;
    // config 仍配 tools-approved(零工具档不得因配置在场就启动它)。
    smoke.WriteGlobalConfig(/*with_flaky=*/false);
    smoke.WriteDeployment(ZeroToolsDeployment());
    smoke.model.Enqueue(SseResponse(SingleTextFrames()));

    std::string spawn_error;
    REQUIRE(smoke.SpawnServer(binary, /*with_profile_flag=*/true, &spawn_error));
    REQUIRE(smoke.Send(R"({"id":1,"method":"initialize","params":{"clientName":"p1-smoke"}})"));
    REQUIRE(smoke.Send(R"({"method":"initialized"})"));
    REQUIRE(smoke.Send(R"({"id":2,"method":"thread/start","params":{}})"));
    REQUIRE(smoke.PumpUntil([&] { return smoke.FindResponse(2) != nullptr; }, 30000));
    const json* thread_response = smoke.FindResponse(2);
    REQUIRE(thread_response != nullptr);
    const std::string thread_id = (*thread_response)["result"].value("threadId", std::string());

    REQUIRE(smoke.Send(json{{"id", 3},
                            {"method", "turn/start"},
                            {"params", json{{"threadId", thread_id}, {"text", "零工具问答"}}}}
                               .dump()));
    REQUIRE(smoke.PumpUntil([&] { return smoke.FindEvent("turn/completed") != nullptr; }, 60000));
    const json* completed = smoke.FindEvent("turn/completed");
    REQUIRE(completed != nullptr);
    CHECK((*completed)["params"].value("executionStatus", std::string()) == "success");

    // 一笔模型请求,无 tools 定义(零工具会话:合同 §2.3)。
    const std::vector<lubancode::test_support::FakeHttpRequest> requests = smoke.model.requests();
    REQUIRE(requests.size() == 1);
    const json body = json::parse(requests[0].body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    CHECK_FALSE(body.contains("tools"));

    REQUIRE(smoke.Send(R"({"id":9,"method":"shutdown","params":{}})"));
    int exit_code = -1;
    REQUIRE(smoke.proc->Wait(15000, &exit_code));
    CHECK(exit_code == 0);
}

// ---------------------------------------------------------------------------
// 用例 3:档点名服务里不存在的工具——thread/start 明拒(缺工具明拒)
// ---------------------------------------------------------------------------
TEST_CASE("profile 冒烟:档点名不存在的工具,thread/start 明拒不炸") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty() || !PythonAvailable()) {
        return;
    }
    ProfileSmoke smoke;
    smoke.WriteGlobalConfig(/*with_flaky=*/false);
    std::string deployment_json = MinimalToolsDeployment(/*with_flaky=*/false);
    // 把 describe 换成服务里没有的工具名——握手后复验失败,整场明拒。
    const std::size_t pos = deployment_json.find("mcp:tools-approved:describe");
    REQUIRE(pos != std::string::npos);
    deployment_json.replace(pos, std::string("mcp:tools-approved:describe").size(),
                            "mcp:tools-approved:no_such_tool");
    smoke.WriteDeployment(deployment_json);

    std::string spawn_error;
    REQUIRE(smoke.SpawnServer(binary, /*with_profile_flag=*/true, &spawn_error));
    REQUIRE(smoke.Send(R"({"id":1,"method":"initialize","params":{"clientName":"p1-smoke"}})"));
    REQUIRE(smoke.Send(R"({"method":"initialized"})"));
    REQUIRE(smoke.Send(R"({"id":2,"method":"thread/start","params":{}})"));
    REQUIRE(smoke.PumpUntil([&] { return smoke.FindResponse(2) != nullptr; }, 30000));
    const json* thread_response = smoke.FindResponse(2);
    REQUIRE(thread_response != nullptr);
    // 明拒:错误响应(不是带回 threadId 的成功响应)。
    CHECK(thread_response->contains("error"));
    CHECK_FALSE((*thread_response).contains("result"));
    // 零模型请求:没装配成就没进回合,更没碰模型。
    CHECK(smoke.model.requests().empty());

    // 进程还活着(坏档拒绝会话,不崩服务)。
    REQUIRE(smoke.Send(R"({"id":9,"method":"shutdown","params":{}})"));
    int exit_code = -1;
    REQUIRE(smoke.proc->Wait(15000, &exit_code));
    CHECK(exit_code == 0);
}

// ---------------------------------------------------------------------------
// 用例 4:坏档(不存在的档名)——进程拒绝启动
// ---------------------------------------------------------------------------
TEST_CASE("profile 冒烟:坏档拒绝启动,退出码非 0") {
    const std::string binary = FindLubancodeBinary();
    if (binary.empty()) {
        return;
    }
    ProfileSmoke smoke;
    // 档名不存在:service.defaultProfile 指 no-such,解析在启动即拒。
    smoke.WriteDeployment(R"({"schemaVersion":1,"service":{"defaultProfile":"no-such"}})");

    std::string spawn_error;
    REQUIRE(smoke.SpawnServer(binary, /*with_profile_flag=*/true, &spawn_error));
    int exit_code = 0;
    REQUIRE(smoke.proc->Wait(15000, &exit_code));
    CHECK(exit_code != 0);
    // stderr 有人话(部署档解析失败),不静默。
    CHECK(smoke.proc->StderrText().find("部署档") != std::string::npos);
}
