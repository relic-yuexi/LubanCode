#include "sdk/adapters.hpp"
#if defined(LUBANCORE_PRIVATE_OPENING_TEST_HOOKS)
#include "sdk/backend_owner_test_hooks.hpp"
#endif
#include "sdk/callback_scope.hpp"
#include "api/usage_event_projection.hpp"

#include <filesystem>
#include <set>
#include <new>
#include <utility>

#include "tools/path_utils.hpp"

namespace lubancore::detail {
#if defined(LUBANCORE_PRIVATE_OPENING_TEST_HOOKS)
namespace testing {
namespace {
thread_local BackendOwnerObserverHandle backend_owner_observer;
thread_local bool fail_backend_owner_allocation = false;
}
bool ReplaceBackendOwnerAllocationFailure(bool replacement) noexcept {
    return std::exchange(fail_backend_owner_allocation, replacement);
}
void ObserveBackendOwner(const std::shared_ptr<Backend>& owner) {
    const auto observer = backend_owner_observer;
    if (observer && *observer) (*observer)(owner);
}
BackendOwnerObserverHandle ReplaceBackendOwnerObserver(
    BackendOwnerObserverHandle replacement) noexcept {
    return std::exchange(backend_owner_observer, std::move(replacement));
}
std::unique_ptr<lubancode::api::Backend> AdaptObservedBackend(std::shared_ptr<Backend> backend) {
    return AdaptBackend(std::move(backend));
}
}
#endif
namespace {
namespace api = lubancode::api;
namespace tools = lubancode::tools;
using Json = nlohmann::json;

bool ValidSamplingValue(const std::string& value) {
    return value.size() <= 256 && value.find('\0') == std::string::npos &&
           lubancode::platform::IsValidUtf8(value);
}

// Both projection and Generate consume this conversion. In particular, these
// JSON-valued public fields are strings: never parse them back into a guessed
// provider shape, or invent content that the public backend never receives.
std::expected<ModelRequest, api::Error> ConvertRequest(const api::Request& request) {
    if (!ValidSamplingValue(request.reasoning_effort)) {
        return std::unexpected(api::Error{api::ErrorKind::Api, "sdk.backend.invalid_reasoning_effort"});
    }
    ModelRequest in;
    in.model = request.model;
    in.system = request.system;
    in.max_output_tokens = request.max_tokens;
    in.reasoning_effort = request.reasoning_effort;
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
    return in;
}

api::ModelInputSnapshot ProjectModelInput(const ModelRequest& request) {
    Json input = {{"system", request.system}, {"messages", Json::array()}, {"tools", Json::array()}};
    for (const auto& message : request.messages) {
        Json out = {{"role", message.role}, {"text", message.text},
                    {"tool_calls", Json::array()}, {"tool_replies", Json::array()}};
        for (const auto& call : message.tool_calls) {
            out["tool_calls"].push_back({{"id", call.id}, {"name", call.name}, {"input_json", call.input_json}});
        }
        for (const auto& reply : message.tool_replies) {
            out["tool_replies"].push_back({{"call_id", reply.call_id}, {"text", reply.text}, {"is_error", reply.is_error}});
        }
        input["messages"].push_back(std::move(out));
    }
    for (const auto& tool : request.tools) {
        input["tools"].push_back({{"name", tool.name}, {"description", tool.description},
                                 {"input_schema_json", tool.input_schema_json}});
    }
    return {std::move(input), api::kSdkModelRequestInputScope, api::kSdkGenerateOutputLimitScope};
}

class BackendAdapter final : public api::Backend {
public:
    explicit BackendAdapter(std::shared_ptr<lubancore::Backend> backend) : backend_(std::move(backend)) {}
    std::expected<std::optional<api::ModelInputSnapshot>, std::string>
    PrepareModelInput(const api::Request& request) const override {
        try {
            auto converted = ConvertRequest(request);
            if (!converted) return std::unexpected(converted.error().message);
            return std::optional<api::ModelInputSnapshot>{ProjectModelInput(*converted)};
        } catch (const std::exception& error) {
            return std::unexpected(std::string("sdk.backend.exception: ") + error.what());
        } catch (...) {
            return std::unexpected("sdk.backend.exception");
        }
    }
    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* cancel) override {
        bool received_reply = false;
        try {
            auto in = ConvertRequest(request);
            if (!in) return std::unexpected(in.error());
            if (cancel && cancel->load()) return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
            auto reply = [&] {
                CallbackScope callback;
                return backend_->Generate(*in, Cancellation{cancel});
            }();
            received_reply = reply.has_value();
            // Capture all returned numeric facts before copying bounded host
            // metadata. If a later material copy/validation fails, the shared
            // assembler has already received the nonterminal numeric snapshot.
            api::UsageSnapshot numeric;
            if (reply && reply->usage) {
                numeric.usage_reported=true;
                numeric.usage.input_tokens=reply->usage->input_tokens;
                numeric.usage.output_tokens=reply->usage->output_tokens;
                numeric.usage.cache_read_tokens=reply->usage->cache_read_tokens;
                numeric.usage.cache_creation_tokens=reply->usage->cache_creation_tokens;
                numeric.usage.output_reasoning_tokens=reply->usage->output_reasoning_tokens;
                emit(numeric);
            }
            std::optional<api::usage_wire::Snapshot> material;
            std::optional<std::string> provider_response_id;
            std::optional<std::string_view> material_error;
            if (reply) {
                if (reply->provider_response_id) {
                    if (!api::usage_observation::TextFits(*reply->provider_response_id,
                            ::lubancore::usage::v1::kMaxResponseIdBytes,true))
                        material_error="usage.response_id.invalid";
                    else {
                        provider_response_id=reply->provider_response_id;
                        emit(api::ProviderResponseIdentity{*provider_response_id});
                    }
                }
                if (reply->usage_observation) {
                    if (!reply->usage) material_error="usage.material.without_numbers";
                    else {
                        const std::array<std::int64_t,::lubancore::usage::v1::kFieldCount> values{
                            reply->usage->input_tokens,reply->usage->output_tokens,
                            reply->usage->cache_read_tokens,reply->usage->cache_creation_tokens,
                            reply->usage->output_reasoning_tokens};
                        const auto valid=api::usage_observation::Validate(*reply->usage_observation,values);
                        if (!valid) material_error=valid.error();
                        else {
                            material=api::usage_wire::Snapshot{values,std::move(*reply->usage_observation)};
                            emit(api::usage_wire::Nonterminal(*material,provider_response_id));
                        }
                    }
                } else if (reply->usage) {
                    auto legacy = api::usage_wire::LegacyBackend(numeric.usage);
                    if (!legacy) material_error = legacy.error();
                    else {
                        material = std::move(*legacy);
                        emit(api::usage_wire::Nonterminal(*material, provider_response_id));
                    }
                }
            }
            // Cancellation keeps its existing outcome even if optional returned
            // material is invalid. No body or success terminal has been emitted.
            if (cancel && cancel->load()) return std::unexpected(api::Error{api::ErrorKind::Cancelled,"cancelled"});
            if (material_error) return std::unexpected(api::Error{api::ErrorKind::Api,std::string(*material_error)});
            if (!reply) return std::unexpected(api::Error{api::ErrorKind::Api, reply.error().code + ": " + reply.error().message});
            if (!ValidSamplingValue(reply->stop_reason)) {
                return std::unexpected(api::Error{api::ErrorKind::Parse, "sdk.backend.invalid_stop_reason"});
            }
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
            done.stop_reason = reply->stop_reason.empty()
                ? (reply->tool_calls.empty() ? "end_turn" : "tool_use") : reply->stop_reason;
            if (material) api::usage_wire::Apply(done,*material,provider_response_id);
            else {
                done.usage=numeric.usage;
                done.usage_reported=numeric.usage_reported;
                done.provider_response_id=provider_response_id;
            }
            emit(done);
            return {};
        } catch (const std::exception& error) {
            if (received_reply && cancel && cancel->load()) return std::unexpected(api::Error{api::ErrorKind::Cancelled,"cancelled"});
            return std::unexpected(api::Error{api::ErrorKind::Api, std::string("sdk.backend.exception: ") + error.what()});
        } catch (...) {
            if (received_reply && cancel && cancel->load()) return std::unexpected(api::Error{api::ErrorKind::Cancelled,"cancelled"});
            return std::unexpected(api::Error{api::ErrorKind::Api, "sdk.backend.exception"});
        }
    }
private:
    std::shared_ptr<lubancore::Backend> backend_;
};

class LocalTool final : public tools::Tool {
public:
    LocalTool(std::unique_ptr<tools::Tool> inner, std::string cwd, bool command_jobs)
        : inner_(std::move(inner)), cwd_(std::move(cwd)), command_jobs_(command_jobs) {}
    std::string name() const override { return inner_->name(); }
    std::string description() const override {
        return name() == "run_command" ? (command_jobs_
            ? "Run a command in the session cwd. execution_mode=session_job explicitly requests an owned Job; foreground is the default."
            : "Run a foreground command in the session cwd. Background jobs are unavailable.")
                                       : inner_->description();
    }
    Json input_schema() const override {
        auto schema = inner_->input_schema();
        if (name() == "run_command" && schema.contains("properties")) {
            schema["properties"].erase("run_in_background");
            schema["properties"].erase("max_runtime_ms");
            if (command_jobs_) {
                schema["properties"]["execution_mode"] = {{"type", "string"}, {"enum", Json::array({"foreground", "session_job"})}};
                schema["properties"]["job_budget_ms"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 86400000}};
            }
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
        // Remote fetch has no filesystem path binding. Preserve its own URL
        // validation and the invocation's cancellation/identity context exactly.
        if (name() == "web_fetch") return inner_->execute(input, context);
        auto effective = input;
        const bool command = name() == "run_command";
        const bool search = name() == "search";
        if (search) {
            // The CLI parser's ambient cwd/null defaults are intentionally kept
            // there. SDK binding supplies an explicit session root every time.
            if (!input.is_object()) return Result::Error("sdk.tool.invalid_input: expected object");
            const auto path = input.find("path");
            if (path != input.end() && !path->is_null() && !path->is_string()) {
                return Result::Error("sdk.tool.invalid_path: expected string or null");
            }
            for (const char* field : {"mode", "pattern", "glob"}) {
                const auto value = input.find(field);
                if (value == input.end() || value->is_null()) continue;
                if (!value->is_string()) return Result::Error("sdk.tool.invalid_input: expected string");
                const auto text = value->get<std::string>();
                if (text.find('\0') != std::string::npos || !lubancode::platform::IsValidUtf8(text)) {
                    return Result::Error("sdk.tool.invalid_input: NUL or invalid UTF-8");
                }
            }
            if (path == input.end() || path->is_null() || *path == "") effective["path"] = cwd_;
        }
        if (command) {
            if (!input.is_object()) return Result::Error("sdk.job.invalid_input");
            // Preserve the old foreground parser, including explicit false
            // and its ignored-but-validated max_runtime_ms. The owned Job
            // normalizer rejects either CLI field instead of changing its meaning.
            if (input.contains("run_in_background") && input.at("run_in_background") != false)
                return Result::Error("sdk.job.detached_unsupported");
            if ((!command_jobs_ && (input.contains("execution_mode") || input.contains("job_budget_ms"))) ||
                (input.contains("execution_mode") && input.at("execution_mode") != "foreground") || input.contains("job_budget_ms"))
                return Result::Error("sdk.job.main_admission_required: command Jobs require the actual main declaration");
        }
        const char* key = command ? "cwd" : "path";
        if (effective.contains(key) && effective.at(key).is_string()) {
            const auto value = effective.at(key).get<std::string>();
            if (value.find('\0') != std::string::npos) return Result::Error("sdk.tool.invalid_path: NUL");
            if (search && !lubancode::platform::IsValidUtf8(value)) return Result::Error("sdk.tool.invalid_path: invalid UTF-8");
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
    bool command_jobs_ = false;
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

namespace {
struct BackendOwnerStorage {
    std::unique_ptr<Backend> backend;
    ~BackendOwnerStorage() {
        CallbackScope callback;
        backend.reset();
    }
};
#if defined(LUBANCORE_PRIVATE_OPENING_TEST_HOOKS)
template<class T> struct BackendOwnerAllocator {
    using value_type = T;
    BackendOwnerAllocator() noexcept = default;
    template<class U> BackendOwnerAllocator(const BackendOwnerAllocator<U>&) noexcept {}
    T* allocate(std::size_t count) {
        if (std::exchange(testing::fail_backend_owner_allocation, false)) throw std::bad_alloc{};
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* p, std::size_t count) noexcept { std::allocator<T>{}.deallocate(p, count); }
    template<class U> bool operator==(const BackendOwnerAllocator<U>&) const noexcept { return true; }
};
#endif
}
std::shared_ptr<Backend> OwnBackend(std::unique_ptr<Backend>& backend) {
    if (!backend) return {};
    // Allocation can throw while the original unique owner remains untouched.
#if defined(LUBANCORE_PRIVATE_OPENING_TEST_HOOKS)
    auto storage = std::allocate_shared<BackendOwnerStorage>(BackendOwnerAllocator<BackendOwnerStorage>{});
#else
    auto storage = std::make_shared<BackendOwnerStorage>();
#endif
    storage->backend = std::move(backend);
    return std::shared_ptr<Backend>(storage, storage->backend.get());
}

std::unique_ptr<lubancode::api::Backend> AdaptBackend(std::shared_ptr<Backend> backend) {
    return std::make_unique<BackendAdapter>(std::move(backend));
}
std::unique_ptr<lubancode::tools::Tool> BindLocalTool(std::unique_ptr<lubancode::tools::Tool> tool, std::string cwd, bool command_jobs) {
    return std::make_unique<LocalTool>(std::move(tool), std::move(cwd), command_jobs);
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
