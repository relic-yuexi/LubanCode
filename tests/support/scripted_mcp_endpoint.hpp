#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "runtime/assembly/mcp.hpp"

namespace lubancode::test_support {

// 真 Client + 可控传输：装配/宿主测试共用，协议解析仍走生产代码。
// 此件须比返回的 Client 活得久，析构记录也由调用方持有。
class ScriptedMcpEndpoint : public mcp::Transport {
public:
    std::string fail_method;
    std::string silent_method;
    std::function<void(const std::string&)> on_method;
    std::vector<std::string> methods;
    nlohmann::json tools = nlohmann::json::array({
        {{"name", "zulu"}, {"description", "fixture z"}},
        {{"name", "echo"}, {"description", "fixture echo"}},
    });
    std::vector<std::string>* destruction_events = nullptr;
    std::string tag = "client";
    int shutdown_count = 0;

    runtime::assembly::McpClientLauncher Launcher() {
        return [this](const runtime::assembly::McpLaunchRequest& request)
            -> std::expected<std::unique_ptr<mcp::Client>, std::string> {
            auto client = std::make_unique<mcp::Client>(request.name);
            client_ = client.get();
            client->AttachTransportForTest(this);
            return client;
        };
    }

    bool WriteLine(const std::string& line) override {
        const auto message = nlohmann::json::parse(line);
        const std::string method = message.at("method").get<std::string>();
        methods.push_back(method);
        if (on_method) on_method(method);
        if (method == "notifications/cancelled") {
            client_->OnLine(nlohmann::json({{"jsonrpc", "2.0"}, {"id", message.at("params").at("requestId")},
                {"error", {{"code", -32800}, {"message", "fixture cancelled"}}}}).dump());
            return true;
        }
        if (!message.contains("id") || method == silent_method) return true;
        nlohmann::json response{{"jsonrpc", "2.0"}, {"id", message.at("id")}};
        if (method == fail_method) {
            response["error"] = {{"code", -32000}, {"message", "fixture failure"}};
        } else if (method == "initialize") {
            response["result"] = {{"protocolVersion", "2024-11-05"}};
        } else if (method == "tools/list") {
            response["result"] = {{"tools", tools}};
        } else {
            response["result"] = {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "fixture ok"}}})}};
        }
        client_->OnLine(response.dump());
        return true;
    }

    void Shutdown(int) override {
        if (alive_.exchange(false)) {
            ++shutdown_count;
            if (destruction_events) destruction_events->push_back(tag);
        }
    }
    bool IsAlive() const override { return alive_.load(); }
    std::string StderrTail() const override { return {}; }

private:
    mcp::Client* client_ = nullptr;
    std::atomic<bool> alive_{true};
};

}  // namespace lubancode::test_support
