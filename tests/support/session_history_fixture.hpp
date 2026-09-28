#pragma once

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_http_server.hpp"

namespace lubancode::test_support::session_history {
namespace fs = std::filesystem;
using nlohmann::json;

inline constexpr const char* kTool = "history_probe";
inline constexpr const char* kCall = "HISTORY_A_CALL";
inline constexpr const char* kArgument = "HISTORY_A_ARGUMENT";
inline constexpr const char* kToolResult = "HISTORY_A_TOOL_RESULT";
inline constexpr const char* kFirst = "HISTORY_A_USER_ONE";
inline constexpr const char* kFirstAnswer = "HISTORY_A_ANSWER_ONE";
inline constexpr const char* kSecond = "HISTORY_A_USER_TWO";
inline constexpr const char* kSecondAnswer = "HISTORY_A_ANSWER_TWO";
inline constexpr const char* kThird = "HISTORY_A_USER_THREE";
inline constexpr const char* kThirdAnswer = "HISTORY_A_ANSWER_THREE";
inline constexpr const char* kOther = "HISTORY_B_USER";
inline constexpr const char* kOtherAnswer = "HISTORY_B_ANSWER";

inline std::string Utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline FakeHttpResponse Reply(const json& delta, const char* reason) {
    FakeHttpResponse response;
    response.headers.emplace_back("Content-Type", "text/event-stream");
    for (const auto& frame : std::vector<json>{
             {{"id", "history-reply"}, {"choices", json::array({
                  {{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}})}},
             {{"id", "history-reply"}, {"choices", json::array({
                  {{"index", 0}, {"delta", json::object()}, {"finish_reason", reason}}})}},
             {{"id", "history-reply"}, {"choices", json::array()},
              {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 8}, {"total_tokens", 28}}}}}) {
        response.body += "data: " + frame.dump() + "\n\n";
    }
    response.body += "data: [DONE]\n\n";
    return response;
}

struct Fixture {
    fs::path root;
    FakeHttpServer model;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("session-history-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++serial));
        fs::create_directories(root / "project");
        fs::create_directories(root / "resources");
        const json function = {{"name", kTool},
                               {"arguments", json({{"text", kArgument}}).dump()}};
        const json tool_call = {{"index", 0}, {"id", kCall}, {"type", "function"},
                                {"function", function}};
        model.Enqueue(Reply({{"role", "assistant"},
                             {"tool_calls", json::array({tool_call})}}, "tool_calls"));
        for (const char* answer : {kFirstAnswer, kSecondAnswer, kOtherAnswer, kThirdAnswer}) {
            model.Enqueue(Reply({{"role", "assistant"}, {"content", answer}}, "stop"));
        }
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Cwd() const { return Utf8(root / "project"); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(model.port()); }

    std::string RecordedCallId(const std::string& session_id) const {
        std::vector<json> records;
        for (const auto& entry : fs::recursive_directory_iterator(root / "data")) {
            if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") continue;
            std::ifstream input(entry.path(), std::ios::binary);
            REQUIRE(input.good());
            for (std::string line; std::getline(input, line);) {
                if (line.empty()) continue;
                const auto record = json::parse(line);
                if (record.is_object() && record.value("sessionId", "") == session_id) {
                    records.push_back(record);
                }
            }
        }
        std::string action_id, assistant_ref;
        int mappings = 0;
        for (const auto& record : records) {
            if (record.value("type", "") != "event" ||
                record.value("kind", "") != "tool.execution.pending") continue;
            const auto& payload = record.at("payload");
            if (payload.value("provider_tool_call_id", "") != kCall) continue;
            action_id = record.at("actionId").get<std::string>();
            CHECK_FALSE(action_id.empty());
            CHECK(payload.value("tool_call_id", "") == action_id);
            assistant_ref = payload.at("assistantMessageRef").get<std::string>();
            ++mappings;
        }
        REQUIRE(mappings == 1);
        int declarations = 0;
        for (const auto& record : records) {
            if (record.value("type", "") != "message" ||
                record.value("messageId", "") != assistant_ref) continue;
            const auto& message = record.at("message");
            CHECK(message.value("role", "") == "assistant");
            for (const auto& call : message.at("tool_calls")) {
                if (call.value("id", "") != kCall) continue;
                CHECK(call["function"].value("name", "") == kTool);
                CHECK(json::parse(call["function"].value("arguments", "{}")) ==
                      json({{"text", kArgument}}));
                ++declarations;
            }
        }
        REQUIRE(declarations == 1);
        return action_id;
    }
};

inline std::string MessageText(const json& message) {
    if (!message.contains("content") || message["content"].is_null()) return {};
    if (message["content"].is_string()) return message["content"].get<std::string>();
    std::string text;
    REQUIRE(message["content"].is_array());
    for (const auto& part : message["content"]) {
        if (part.value("type", "") == "text") text += part.value("text", "");
    }
    return text;
}

// Read the actual HTTP wire, not the host's history view or its final reply.
// Keep the marked messages, call and result in order; duplicates remain visible.
inline void CheckRequest(const FakeHttpRequest& request,
                         const std::vector<std::string>& expected, bool other = false,
                         const std::string& expected_call_id = kCall) {
    CHECK(request.method == "POST");
    const auto body = json::parse(request.body);
    CHECK(body.value("model", "") == "history-model");
    REQUIRE(body.contains("messages"));
    REQUIRE(body["messages"].is_array());
    std::vector<std::string> observed;
    for (const auto& message : body["messages"]) {
        const auto role = message.value("role", "");
        const auto text = MessageText(message);
        if (role == "user" || role == "assistant") {
            int matched_messages = 0;
            for (const char* marker : {kFirst, kFirstAnswer, kSecond, kSecondAnswer,
                                       kThird, kThirdAnswer, kOther, kOtherAnswer}) {
                const auto position = text.find(marker);
                if (position != std::string::npos) {
                    const std::string token(marker);
                    CHECK(role == (token.find("_USER") != std::string::npos ? "user" : "assistant"));
                    CHECK(text.find(marker, position + token.size()) == std::string::npos);
                    observed.emplace_back(marker);
                    ++matched_messages;
                }
            }
            CHECK(matched_messages <= 1);
        }
        if (message.contains("tool_calls")) {
            REQUIRE(message["tool_calls"].is_array());
            for (const auto& call : message["tool_calls"]) {
                CHECK(role == "assistant");
                CHECK(call.value("id", "") == expected_call_id);
                CHECK(call["function"].value("name", "") == std::string(kTool));
                CHECK(json::parse(call["function"].value("arguments", "{}")) ==
                      json({{"text", kArgument}}));
                observed.emplace_back(kCall);
            }
        }
        if (role == "tool") {
            CHECK(message.value("tool_call_id", "") == expected_call_id);
            CHECK(text.find(kToolResult) != std::string::npos);
            observed.emplace_back(kToolResult);
        }
    }
    CHECK(observed == expected);
    CHECK(request.body.find(other ? "HISTORY_A_" : "HISTORY_B_") == std::string::npos);
}

inline void CheckRequests(const Fixture& fixture, const std::string& session_id) {
    const auto requests = fixture.model.requests();
    REQUIRE(requests.size() == 5);
    CheckRequest(requests[0], {kFirst});
    CheckRequest(requests[1], {kFirst, kCall, kToolResult});
    CheckRequest(requests[2], {kFirst, kCall, kToolResult, kFirstAnswer, kSecond});
    CheckRequest(requests[3], {kOther}, true);
    // Live turns retain provider IDs. V3 resume uses the durable action ID on
    // both sides of the pair; derive it from the stored provider binding instead
    // of accepting arbitrary IDs or assuming an action-number allocation order.
    const auto resumed_call_id = fixture.RecordedCallId(session_id);
    CheckRequest(requests[4], {kFirst, kCall, kToolResult, kFirstAnswer,
                                kSecond, kSecondAnswer, kThird}, false, resumed_call_id);
}
}  // namespace lubancode::test_support::session_history
