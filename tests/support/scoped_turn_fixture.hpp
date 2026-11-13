#pragma once

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/session_runtime.hpp"
#include "tools/registry.hpp"
#include "workspace/identity.hpp"

namespace lubancode::test_support::turn_scope {

inline std::vector<api::StreamEvent> ToolReply(const std::string& call_id) {
    return {api::MessageStart{"scope-message", "scope-model"},
            api::ToolUseStart{0, call_id, "scope_probe"},
            api::ToolUseInputDelta{0, "{}"}, api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}}};
}
inline std::vector<api::StreamEvent> TextReply(const std::string& text) {
    return {api::MessageStart{"scope-message", "scope-model"}, api::TextDelta{text},
            api::ContentBlockDone{0}, api::MessageDone{"end_turn", api::Usage{}}};
}

class Backend final : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> replies;
    std::vector<api::Request> requests;
    std::function<void(const api::Request&)> on_request;
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        requests.push_back(request);
        if (on_request) on_request(request);
        const auto index = requests.size() - 1;
        if (index >= replies.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "scope fixture exhausted", 0});
        }
        for (const auto& event : replies[index]) emit(event);
        return {};
    }
};

class ProbeTool final : public tools::Tool {
public:
    int calls = 0;
    std::atomic<bool>* cancel_on_call = nullptr;
    std::string name() const override { return "scope_probe"; }
    std::string description() const override { return "Read a scoped binding marker."; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    Result execute(const nlohmann::json&) override {
        ++calls;
        if (cancel_on_call) cancel_on_call->store(true);
        return {"scope-tool-result", false};
    }
};

struct AgentFixture {
    Backend backend;
    tools::ToolRegistry registry;
    ProbeTool* probe = nullptr;
    std::unique_ptr<agent::Agent> agent;
    AgentFixture() {
        auto tool = std::make_unique<ProbeTool>();
        probe = tool.get();
        registry.Register(std::move(tool));
        agent::AgentProfile profile;
        profile.request.model = "scope-model";
        profile.runtime.max_steps_per_turn = 5;
        profile.runtime.max_output_tokens = 8192;
        profile.system_prompt = "Preserve turn bindings.";
        agent = std::make_unique<agent::Agent>(backend, registry, std::move(profile));
    }
    void AddRound(const std::string& suffix) {
        backend.replies.push_back(ToolReply("scope-call-" + suffix));
        backend.replies.push_back(TextReply("scope-answer-" + suffix));
    }
};

struct TempDirectory {
    std::filesystem::path root;
    TempDirectory() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() /
            ("turn scope " + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()) + " " + std::to_string(++serial));
        std::filesystem::create_directories(root / "project");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(root, error); }
};

struct SessionFixture {
    TempDirectory directory;
    std::unique_ptr<runtime::SessionRuntime> session;
    SessionFixture() {
        runtime::SessionRuntime::Options options;
        options.wire_name = "responses";
        options.start_ts = "20260929-120000";
        options.lubancode_version = "test-scoped-turn";
        options.trajectory_workspaces_root = directory.root / "workspaces";
        options.trajectory_workspace_identity = workspace::MakeFallbackIdentity(directory.root / "project");
        options.trajectory_v3_system_content = "Preserve turn bindings.";
        session = std::make_unique<runtime::SessionRuntime>(std::move(options));
        REQUIRE(session->trajectory() != nullptr);
        REQUIRE_MESSAGE(session->trajectory_open_error().empty(), session->trajectory_open_error());
    }
};

struct EventCollector final : runtime::EventSink {
    std::vector<runtime::ServerEvent> events;
    void Emit(const runtime::ServerEvent& event) override { events.push_back(event); }
};

inline bool ContainsText(const api::Request& request, const std::string& expected) {
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block);
                text && text->text.find(expected) != std::string::npos) return true;
        }
    }
    return false;
}

}  // namespace lubancode::test_support::turn_scope
