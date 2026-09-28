#pragma once

#include "session_history_fixture.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::test_support {
struct McpCwdFixture {
    std::filesystem::path root;
    std::filesystem::path host_cwd = std::filesystem::current_path();
    FakeHttpServer model;
    std::string python;
    static constexpr const char* kTool = "mcp__location__where";

    McpCwdFixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() / ("mcp-session-cwd-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++serial));
        std::filesystem::create_directories(root / "project-a");
        std::filesystem::create_directories(root / "project-b");
        std::filesystem::create_directories(root / "resources");
#ifdef _WIN32
        const char* command = "python";
#else
        const char* command = "python3";
#endif
        const auto located = platform::RunProcess(
            {command, "-c", "import json,sys; print(json.dumps(sys.executable))"}, 10000);
        INFO(located.output);
        REQUIRE_FALSE(located.spawn_failed);
        REQUIRE_FALSE(located.timed_out);
        REQUIRE(located.exit_code == 0);
        python = nlohmann::json::parse(located.output).get<std::string>();
        REQUIRE(platform::Utf8ToPath(python).is_absolute());
        for (int i = 0; i != 4; ++i) {
            const nlohmann::json function = {{"name", kTool}, {"arguments", "{}"}};
            const nlohmann::json tool_call = {{"index", 0}, {"id", CallId(i)},
                                               {"type", "function"}, {"function", function}};
            model.Enqueue(session_history::Reply({{"role", "assistant"},
                {"tool_calls", nlohmann::json::array({tool_call})}}, "tool_calls"));
            model.Enqueue(session_history::Reply(
                {{"role", "assistant"}, {"content", "cwd-answer"}}, "stop"));
        }
    }
    ~McpCwdFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(model.port()); }
    static std::string Script() {
        return std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/session_resources_mcp.py";
    }
    static std::string CallId(int round) { return "cwd-call-" + std::to_string(round); }
    static std::string Input(int round) { return "cwd-input-" + std::to_string(round); }

    void Check() const {
        CHECK(std::filesystem::equivalent(host_cwd, std::filesystem::current_path()));
        const auto requests = model.requests();
        REQUIRE(requests.size() == 8);
        std::vector<int> pids;
        for (int round = 0; round != 4; ++round) {
            for (int step = 0; step != 2; ++step) {
                const auto body = nlohmann::json::parse(requests[round * 2 + step].body);
                CHECK(body.value("model", "") == "cwd-model");
                REQUIRE(body.contains("tools"));
                REQUIRE(body["tools"].size() == 1);
                CHECK(body["tools"][0]["function"].value("name", "") == std::string(kTool));
            }
            const auto body = nlohmann::json::parse(requests[round * 2 + 1].body);
            nlohmann::json observed;
            int matching_results = 0;
            for (const auto& message : body["messages"]) {
                if (message.value("role", "") == "tool" &&
                    message.value("tool_call_id", "") == CallId(round)) {
                    observed = nlohmann::json::parse(session_history::MessageText(message));
                    ++matching_results;
                }
            }
            REQUIRE(matching_results == 1);
            REQUIRE(observed.is_object());
            CHECK(observed.value("tool", "") == "where");
            const auto expected = root / (round == 2 ? "project-b" : "project-a");
            CHECK(std::filesystem::equivalent(platform::Utf8ToPath(observed.at("cwd").get<std::string>()), expected));
            pids.push_back(observed.at("pid").get<int>());
        }
        CHECK(pids[0] != pids[1]);
        CHECK(pids[0] != pids[2]);
        CHECK(pids[1] != pids[2]);
        // Closing the first session must not restart or close the second child.
        CHECK(pids[1] == pids[3]);
    }
};
}  // namespace lubancode::test_support
