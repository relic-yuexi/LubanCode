#include "sdk/adapters.hpp"

#include <filesystem>
#include <set>
#include <utility>

#include "tools/path_utils.hpp"

namespace lubancore::detail {
namespace {
namespace api = lubancode::api;
namespace tools = lubancode::tools;
using Json = nlohmann::json;

class BackendAdapter final : public api::Backend {
public:
    explicit BackendAdapter(std::unique_ptr<lubancore::Backend> backend) : backend_(std::move(backend)) {}
    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* cancel) override {
        try {
            ModelRequest in;
            in.model = request.model;
            in.system = request.system;
            in.max_output_tokens = request.max_tokens;
            for (const auto& source : request.messages) {
                Message message;
                switch (source.role) {
                    case api::Role::User: message.role = "user"; break;
                    case api::Role::Assistant: message.role = "assistant"; break;
                    case api::Role::System: message.role = "system"; break;
                    case api::Role::Tool: message.role = "tool"; break;
                }
                for (const auto& block : source.content) {
                    if (const auto* text = std::get_if<api::TextBlock>(&block)) message.text += text->text;
                    else if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
                        message.tool_calls.push_back({call->id, call->name, call->input.dump()});
                    } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                        for (const auto& rich : result->blocks) {
                            if (!std::holds_alternative<tools::TextContent>(rich)) {
                                return std::unexpected(api::Error{api::ErrorKind::Api,
                                    "sdk.backend.unsupported_content: custom backend accepts text only"});
                            }
                        }
                        if (result->structured_content.has_value()) {
                            return std::unexpected(api::Error{api::ErrorKind::Api,
                                "sdk.backend.unsupported_content: structured tool result"});
                        }
                        message.tool_replies.push_back({result->tool_use_id, result->content, result->is_error});
                    } else {
                        return std::unexpected(api::Error{api::ErrorKind::Api,
                            "sdk.backend.unsupported_content: custom backend accepts text/tool history only"});
                    }
                }
                in.messages.push_back(std::move(message));
            }
            for (const auto& tool : request.tools) {
                in.tools.push_back({tool.name, tool.description, tool.input_schema.dump()});
            }
            if (cancel && cancel->load()) return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
            auto reply = backend_->Generate(in, Cancellation{cancel});
            if (cancel && cancel->load()) return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
            if (!reply) return std::unexpected(api::Error{api::ErrorKind::Api, reply.error().code + ": " + reply.error().message});
            if (!lubancode::platform::IsValidUtf8(reply->text)) {
                return std::unexpected(api::Error{api::ErrorKind::Parse, "sdk.backend.invalid_utf8"});
            }
            std::set<std::string> ids;
            for (const auto& call : reply->tool_calls) {
                auto input = Json::parse(call.input_json, nullptr, false);
                if (call.id.empty() || call.name.empty() || !ids.insert(call.id).second || !input.is_object()) {
                    return std::unexpected(api::Error{api::ErrorKind::Parse, "sdk.backend.invalid_tool_call"});
                }
            }
            emit(api::MessageStart{"", request.model});
            if (!reply->text.empty()) emit(api::TextDelta{reply->text});
            int index = 0;
            for (const auto& call : reply->tool_calls) {
                emit(api::ToolUseStart{index, call.id, call.name});
                emit(api::ToolUseInputDelta{index, call.input_json});
                emit(api::ContentBlockDone{index, call.id});
                ++index;
            }
            api::MessageDone done;
            done.stop_reason = reply->tool_calls.empty() ? "end_turn" : "tool_use";
            if (reply->usage) {
                done.usage_reported = true;
                done.usage.input_tokens = reply->usage->input_tokens;
                done.usage.output_tokens = reply->usage->output_tokens;
            }
            emit(done);
            return {};
        } catch (const std::exception& error) {
            return std::unexpected(api::Error{api::ErrorKind::Api, std::string("sdk.backend.exception: ") + error.what()});
        } catch (...) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "sdk.backend.exception"});
        }
    }
private:
    std::unique_ptr<lubancore::Backend> backend_;
};

class LocalTool final : public tools::Tool {
public:
    LocalTool(std::unique_ptr<tools::Tool> inner, std::string cwd)
        : inner_(std::move(inner)), cwd_(std::move(cwd)) {}
    std::string name() const override { return inner_->name(); }
    std::string description() const override {
        return name() == "run_command" ? "Run a foreground command in the session cwd. Background jobs are unavailable."
                                       : inner_->description();
    }
    Json input_schema() const override {
        auto schema = inner_->input_schema();
        if (name() == "run_command" && schema.contains("properties")) {
            schema["properties"].erase("run_in_background");
            schema["properties"].erase("max_runtime_ms");
        }
        return schema;
    }
    bool needs_confirm() const override { return inner_->needs_confirm(); }
    tools::ApprovalClass approval_class() const override { return inner_->approval_class(); }
    tools::EffectClass effect_class() const override { return inner_->effect_class(); }
    tools::Idempotency idempotency() const override { return inner_->idempotency(); }
    tools::RecoveryCapability recovery_capability() const override { return inner_->recovery_capability(); }
    Result execute(const Json& input) override { return execute(input, {}); }
    Result execute(const Json& input, const tools::ToolExecutionContext& context) override {
        auto effective = input;
        const bool command = name() == "run_command";
        if (command && input.contains("run_in_background") && input.at("run_in_background") != false) {
            return Result::Error("sdk.tool.unsupported: background command jobs are not enabled");
        }
        const char* key = command ? "cwd" : "path";
        if (effective.contains(key) && effective.at(key).is_string()) {
            const auto value = effective.at(key).get<std::string>();
            if (value.find('\0') != std::string::npos) return Result::Error("sdk.tool.invalid_path: NUL");
            auto path = tools::Utf8ToPath(value);
            if (path.is_relative()) effective[key] = tools::PathToUtf8(tools::Utf8ToPath(cwd_) / path);
        } else if (command && !effective.contains(key)) {
            effective[key] = cwd_;
        }
        // Empty/null command cwd must not fall through to process-global cwd.
        if (command && effective.contains(key) && (effective.at(key).is_null() || effective.at(key) == "")) {
            effective[key] = cwd_;
        }
        return inner_->execute(effective, context);
    }
private:
    std::unique_ptr<tools::Tool> inner_;
    std::string cwd_;
};

class CustomTool final : public tools::Tool {
public:
    CustomTool(lubancore::Tool tool, Json schema, std::string cwd)
        : tool_(std::move(tool)), schema_(std::move(schema)), cwd_(std::move(cwd)) {}
    std::string name() const override { return tool_.name; }
    std::string description() const override { return tool_.description; }
    Json input_schema() const override { return schema_; }
    bool needs_confirm() const override { return tool_.requires_approval; }
    tools::ApprovalClass approval_class() const override {
        return needs_confirm() ? tools::ApprovalClass::External : tools::ApprovalClass::None;
    }
    Result execute(const Json& input) override { return execute(input, {}); }
    Result execute(const Json& input, const tools::ToolExecutionContext& context) override {
        try {
            auto result = tool_.execute(input.dump(), ToolContext{cwd_, Cancellation{context.cancel}});
            if (!result) return Result::Error(result.error().code + ": " + result.error().message);
            return Result{std::move(result->text), result->is_error};
        } catch (const std::exception& error) {
            return Result::Error(std::string("sdk.tool.exception: ") + error.what());
        } catch (...) { return Result::Error("sdk.tool.exception"); }
    }
private:
    lubancore::Tool tool_;
    Json schema_;
    std::string cwd_;
};
} // namespace

std::unique_ptr<lubancode::api::Backend> AdaptBackend(std::unique_ptr<Backend> backend) {
    return std::make_unique<BackendAdapter>(std::move(backend));
}
std::unique_ptr<lubancode::tools::Tool> BindLocalTool(std::unique_ptr<lubancode::tools::Tool> tool, std::string cwd) {
    return std::make_unique<LocalTool>(std::move(tool), std::move(cwd));
}
Result<std::unique_ptr<lubancode::tools::Tool>> AdaptTool(Tool tool, std::string cwd) {
    auto schema = Json::parse(tool.input_schema_json, nullptr, false);
    if (tool.name.empty() || tool.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        !lubancode::platform::IsValidUtf8(tool.description) || !tool.execute || !schema.is_object() || !schema.contains("type") ||
        !schema.at("type").is_string() || schema.at("type").get<std::string>() != "object") {
        return std::unexpected(Error{"sdk.tool.invalid_definition", tool.name});
    }
    return std::make_unique<CustomTool>(std::move(tool), std::move(schema), std::move(cwd));
}
} // namespace lubancore::detail
