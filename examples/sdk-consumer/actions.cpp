#include <lubancore/core.hpp>
#include <lubancore/extensions.hpp>
#include <lubancore/results.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// This acceptance source is copied into a relocated consumer unchanged. Only
// installed public headers and the standard library may appear here.
namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace ext = sdk::extensions::v1;
namespace result = sdk::results::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
template<class T> T Take(sdk::Result<T> value, const std::string& where) {
    if (!value) throw std::runtime_error(where + ": " + value.error().code + ": " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const std::string& where) {
    if (!value) throw std::runtime_error(where + ": " + value.error().code + ": " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); Check(file.is_open(), "fixture read failed: " + Utf8(path));
    std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(!file.bad(), "fixture read failed"); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc); Check(file.is_open(), "fixture write failed");
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); file.close();
    Check(!file.fail(), "fixture write failed");
}
fs::path SessionDir(const fs::path& root, const std::string& id) {
    fs::path found;
    for (const auto& entry : fs::recursive_directory_iterator(root / "data" / "workspaces")) {
        if (!entry.is_directory() || entry.path().filename() != id || entry.path().parent_path().filename() != "sessions") continue;
        Check(found.empty(), "fixture session directory was ambiguous"); found = entry.path();
    }
    Check(!found.empty(), "fixture session directory is unavailable"); return found;
}
struct Scope {
    std::string session, operation, turn, action, wire, cwd;
    std::optional<std::uint64_t> attempt;
};
struct State {
    std::atomic<unsigned> models{0}, tools{0}, factories{0}, destroyed{0}, pre{0}, post{0};
    std::string arguments = R"({"value":"original"})", expected_arguments = arguments, marker = "ACTION_OWN";
    bool expect_error = false;
    std::function<void(const sdk::ModelRequest&)> check_reply;
    std::mutex mutex;
    std::map<std::string, unsigned> turn_models;
    std::vector<Scope> scopes;
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request) {
        ++models;
        std::string user;
        for (const auto& message : request.messages)
            if (message.role == "user" && message.tool_replies.empty() && !message.text.empty()) user = message.text;
        unsigned phase;
        { const std::lock_guard lock(mutex); phase = ++turn_models[user]; }
        if (phase == 1) return sdk::ModelReply{"", {{"wire-action", "guarded", arguments}}, sdk::Usage{2, 1}};
        Check(phase == 2, "Action retried a completed tool/model phase");
        bool seen = false;
        for (auto m = request.messages.rbegin(); m != request.messages.rend() && !seen; ++m)
            for (const auto& reply : m->tool_replies) if (reply.call_id == "wire-action") {
                Check(reply.is_error == expect_error, "Action changed known tool outcome: " + reply.text);
                if (!expect_error) Check(reply.text.find(marker + " raw") != std::string::npos, "Action lost actual raw tool reply");
                seen = true;
            }
        Check(seen, "Action reply is missing from the actual model request");
        if (check_reply) check_reply(request);
        return sdk::ModelReply{marker + " final", {}, sdk::Usage{2, 1}};
    }
    void Record(const ext::Context& context) {
        Check(!context.session_id.empty() && !context.operation_id.empty() && context.turn_id && context.action_id &&
            context.wire_call_id && context.tool_name && context.effective_cwd,
            "Action fabricated or omitted its declared owner");
        Check(*context.wire_call_id == "wire-action" && *context.tool_name == "guarded", "Action declaration crossed another call");
        Scope owned{context.session_id, context.operation_id, *context.turn_id, *context.action_id,
            *context.wire_call_id, *context.effective_cwd, std::nullopt};
        if (context.point == ext::Point::PreAction) {
            Check(!context.execution, "PreAction fabricated an execution attempt"); ++pre;
        } else {
            Check(context.point == ext::Point::PostAction && context.execution && context.execution->attempt > 0 &&
                context.execution->session_id == context.session_id && context.execution->operation_id == context.operation_id &&
                context.execution->turn_id == *context.turn_id && context.execution->action_id == *context.action_id,
                "PostAction does not name its actual finished invocation");
            owned.attempt = context.execution->attempt; ++post;
        }
        const std::lock_guard lock(mutex); scopes.push_back(std::move(owned));
    }
};
class Backend final : public sdk::Backend {
    std::shared_ptr<State> state_;
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        return state_->Generate(request);
    }
};
using Handler = std::function<sdk::Result<ext::HandlerReturn>(const ext::Context&, const ext::Input&, ext::Next)>;
class Instance final : public ext::Instance {
    Handler handler_;
    std::shared_ptr<State> state_;
public:
    Instance(Handler handler, std::shared_ptr<State> state) : handler_(std::move(handler)), state_(std::move(state)) {}
    ~Instance() override { ++state_->destroyed; }
    sdk::Result<ext::HandlerReturn> Invoke(const ext::Context& context, const ext::Input& input, ext::Next next) override {
        state_->Record(context); return handler_(context, input, std::move(next));
    }
};
ext::HandlerDefinition Definition(std::string name, ext::Point point) {
    ext::HandlerDefinition definition; definition.name = std::move(name); definition.point = point;
    definition.definition_hash = "action-native-v1"; return definition;
}
ext::Registration Registration(std::shared_ptr<State> state, std::vector<ext::HandlerDefinition> definitions, Handler handler) {
    ext::Registration registration;
    registration.manifest = {"action-fixture", "1.0.0", std::move(definitions)};
    registration.source_label = "owned Action acceptance";
    registration.factory = [state, handler = std::move(handler)](const ext::SessionContext&) -> sdk::Result<std::unique_ptr<ext::Instance>> {
        ++state->factories; return std::make_unique<Instance>(handler, state);
    };
    return registration;
}
sdk::Result<ext::HandlerReturn> Continue(ext::Next next, std::optional<std::string> candidate = std::nullopt) {
    const auto downstream = next.Call(std::move(candidate));
    if (!downstream) return std::unexpected(downstream.error());
    if (downstream->kind == ext::DownstreamOutcome::Kind::Denied)
        return ext::HandlerReturn::Denied(downstream->code, downstream->message);
    if (downstream->kind == ext::DownstreamOutcome::Kind::Failed)
        return std::unexpected(sdk::Error{downstream->code, downstream->message});
    return ext::HandlerReturn{};
}
struct Rig {
    fs::path root, cwd;
    std::unique_ptr<sdk::Runtime> runtime;
    explicit Rig(fs::path base) : root(std::move(base)) {
        fs::create_directories(root / "project"); fs::create_directories(root / "resources");
        root = fs::canonical(root); cwd = root / "project";
        runtime = Take(sdk::Runtime::Create({Utf8(root / "data"), Utf8(root / "resources")}), "Runtime");
    }
    sdk::SessionOptions Options(std::shared_ptr<State> state, fs::path project = {}) const {
        sdk::SessionOptions options; options.cwd = Utf8(project.empty() ? cwd : project);
        options.system_prompt = "Explicit Action acceptance."; options.model = "action-model";
        options.max_steps_per_turn = 4; options.approval_timeout = 10s; options.approval_mode = sdk::ApprovalMode::Yolo;
        options.backend = std::make_unique<Backend>(state);
        sdk::Tool tool; tool.name = "guarded"; tool.description = "actual acceptance effect"; tool.requires_approval = false;
        tool.input_schema_json = R"({"type":"object","properties":{"value":{"type":"string"}},"required":["value"],"additionalProperties":false})";
        const auto cwd_text = options.cwd;
        tool.execute = [state, cwd_text](const std::string& args, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(context.cwd == cwd_text && !context.cancellation.requested(), "tool effect crossed cwd or cancellation");
            Check(args == state->expected_arguments, "Action executed unadmitted arguments: " + args);
            ++state->tools; return sdk::ToolResult{state->marker + " raw", false};
        };
        options.custom_tools.push_back(std::move(tool)); return options;
    }
    std::shared_ptr<sdk::Session> Open(sdk::SessionOptions options) const { return Take(runtime->OpenSession(std::move(options)), "OpenSession"); }
};
sdk::Operation Wait(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    auto operation = Take(session->WaitResult(receipt.operation_id, 15s), "WaitResult");
    Check(operation.state != sdk::OperationState::Running && operation.state != sdk::OperationState::Accepted && operation.result_persisted,
        "Action did not persist an actual terminal: " + operation.error); return operation;
}
sdk::Operation Run(const std::shared_ptr<sdk::Session>& session, const std::string& key) { return Wait(session, Take(session->Submit(key, key), "Submit")); }
void Succeeded(const sdk::Operation& operation) { Check(operation.state == sdk::OperationState::Succeeded, "Action operation failed: " + operation.error); }
std::string Material(const sdk::results::v1::SavedSnapshot& snapshot) {
    Check(snapshot.result().metadata_state == result::ArtifactState::Verified, "saved Action metadata was not verified");
    for (const auto& channel : snapshot.result().channels) if (channel.channel == "combined") {
        Check(channel.artifact_verified && channel.text.has_value(), "saved Action display text was not verified"); return *channel.text;
    }
    throw std::runtime_error("saved Action display channel missing");
}
void Saved(const std::shared_ptr<sdk::Session>& session, const sdk::Operation& operation, const std::string& raw, bool supplement) {
    const auto rows = Take(session->ListToolResults(operation.operation_id), "ListToolResults");
    unsigned formal = 0, captured = 0;
    for (const auto& row : rows) {
        if (row.tool_name != "guarded" || !row.selected) continue;
        Check(row.identity.session_id == session->id() && row.identity.operation_id == operation.operation_id &&
            row.identity.turn_id == operation.turn_id && row.attempt > 0, "saved Action result crossed its owned operation");
        const auto text = Material(Take(session->ReadToolResult(row.identity), "ReadToolResult"));
        if (row.identity.result_id.starts_with("capture-")) { ++captured; Check(text == raw, "PostAction altered the raw capture"); }
        else if (row.identity.result_id.starts_with("res-")) {
            ++formal; Check(text.starts_with(raw), "formal Action result lost its actual raw prefix");
            Check((text.find("[Action ") != std::string::npos) == supplement, "formal Action supplement adoption differs");
            if (supplement) Check(text.find("action-native-v1") != std::string::npos && text.find("supplement") != std::string::npos,
                "formal supplement lost frozen source/text");
        } else throw std::runtime_error("unknown selected Action result identity");
    }
    Check(formal == 1 && captured == 1, "Action selected formal/raw materials are not each unique");
}
sdk::Approval Ask(const std::shared_ptr<sdk::EventStream>& stream, const sdk::Receipt& receipt) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < end) {
        auto event = Take(stream->Next(100ms), "approval event");
        if (event && event->approval && event->approval->operation_id == receipt.operation_id) return *event->approval;
    }
    throw std::runtime_error("Action did not reach the real approval table");
}
struct Gate {
    std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool release = false;
    void Enter(sdk::Cancellation cancel) {
        std::unique_lock lock(mutex); ++entered; cv.notify_all();
        while (!release && !cancel.requested()) cv.wait_for(lock, 5ms);
    }
    void Wait(unsigned count) {
        std::unique_lock lock(mutex);
        Check(cv.wait_for(lock, 20s, [&] { return entered == count; }), "Action callbacks did not overlap at their real gate");
    }
    void Release() { const std::lock_guard lock(mutex); release = true; cv.notify_all(); }
};
struct Release { std::shared_ptr<Gate> gate; ~Release() { gate->Release(); } };
} // namespace

void ActionCase(const std::string& name, const fs::path& base) {
    Rig rig(base);
    if (name == "chain") {
        auto state = std::make_shared<State>(); state->expected_arguments = R"({"value":"adopted"})";
        auto options = rig.Options(state); auto first = Definition("first", ext::Point::PreAction);
        auto second = Definition("second", ext::Point::PreAction); first.before = {"second"};
        auto post = Definition("post", ext::Point::PostAction);
        auto next_saved = std::make_shared<ext::Next>();
        options.extensions.push_back(Registration(state, {first, second, post},
            [next_saved, state](const ext::Context& context, const ext::Input& input, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                if (context.hook_name == "first") {
                    *next_saved = next;
                    return Continue(next, R"({"arguments":{"value":"adopted"}})");
                }
                if (context.hook_name == "second") {
                    Check(input.json == R"({"arguments":{"value":"adopted"}})", "Next did not feed the downstream Action");
                    return Continue(next);
                }
                std::cerr << "[sdk-action-post] input=" << input.json << " factories=" << state->factories.load()
                          << " models=" << state->models.load() << " tools=" << state->tools.load() << '\n';
                Check(input.json.find("\"outcome\":\"succeeded\"") != std::string::npos &&
                    input.json.find("\"isError\":false") != std::string::npos &&
                    input.json.find("\"errorCode\":\"\"") != std::string::npos,
                    "PostAction did not receive actual finished facts");
                auto out = Continue(next); if (out) out->effects.push_back({ext::EffectType::ResultSupplement, R"({"text":"supplement"})"}); return out;
            }));
        state->check_reply = [](const sdk::ModelRequest& request) {
            bool found = false;
            for (const auto& message : request.messages) for (const auto& reply : message.tool_replies)
                if (reply.text.find("[Action ") != std::string::npos && reply.text.find("supplement") != std::string::npos) found = true;
            Check(found, "PostAction supplement was never adopted into a real model request");
        };
        auto session = rig.Open(std::move(options)); const auto operation = Run(session, "chain"); Succeeded(operation);
        Check(state->models == 2 && state->tools == 1 && state->pre == 2 && state->post == 1, "Action chain reran or skipped actual work");
        Check(!next_saved->Call(), "saved Action Next retained a live session grant");
        { const std::lock_guard lock(state->mutex); Check(state->scopes.size() == 3 && state->scopes[0].action == state->scopes[2].action &&
            state->scopes[2].operation == operation.operation_id && state->scopes[2].turn == operation.turn_id,
            "declared and finished Action identity differ"); }
        Saved(session, operation, state->marker + " raw", true);
        Take(session->Close(), "Close"); Saved(session, operation, state->marker + " raw", true);
        Check(state->destroyed == 1, "Action instance survived checked Close");
    } else if (name == "deny") {
        auto state = std::make_shared<State>(); state->expect_error = true;
        auto options = rig.Options(state); auto outer = Definition("outer", ext::Point::PreAction);
        auto deny = Definition("deny", ext::Point::PreAction); outer.before = {"deny"};
        options.extensions.push_back(Registration(state, {outer, deny, Definition("post", ext::Point::PostAction)},
            [](const ext::Context& context, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                if (context.hook_name == "deny") return ext::HandlerReturn::Denied("fixture.action.denied", "explicit refusal");
                auto consumed = next.Call(R"({"arguments":{"value":"rewritten"}})");
                Check(consumed.has_value() && consumed->kind == ext::DownstreamOutcome::Kind::Denied, "downstream denial disappeared");
                ext::HandlerReturn out; out.output_json = R"({"arguments":{"value":"laundered"}})"; return out;
            }));
        auto session = rig.Open(std::move(options)); Succeeded(Run(session, "deny"));
        Check(state->tools == 0 && state->post == 0 && session->PendingApprovals().empty(), "denied Action crossed Started/approval/Post");
        Take(session->Close(), "Close denied");
    } else if (name == "rewrite-schema") {
        for (unsigned variant = 0; variant < 3; ++variant) {
            auto state = std::make_shared<State>(); state->expect_error = variant != 2;
            state->arguments = variant == 0 || variant == 2 ? R"({"value":7})" : R"({"value":"original"})";
            state->expected_arguments = state->arguments;
            auto options = rig.Options(state);
            if (variant != 2) options.extensions.push_back(Registration(state, {Definition("rewrite", ext::Point::PreAction)},
                [variant](const ext::Context&, const ext::Input&, ext::Next next) {
                    return Continue(next, variant == 0 ? R"({"arguments":{"value":"repaired"}})" : R"({"arguments":{"value":7}})");
                }));
            auto session = rig.Open(std::move(options)); Succeeded(Run(session, "schema-" + std::to_string(variant)));
            Check(state->tools == (variant == 2 ? 1u : 0u), "Action initial/final schema gate or legacy readonly acceptance differs");
            Take(session->Close(), "Close schema");
        }
    } else if (name == "force-ask") {
        auto state = std::make_shared<State>(); auto options = rig.Options(state);
        options.extensions.push_back(Registration(state, {Definition("ask", ext::Point::PreAction)},
            [](const ext::Context&, const ext::Input&, ext::Next next) {
                auto out = Continue(next); if (out) out->effects.push_back({ext::EffectType::AdmissionDecision, R"({"decision":"ask","reason":"explicit Action ask"})"}); return out;
            }));
        auto session = rig.Open(std::move(options)); auto events = Take(session->Subscribe(), "Subscribe");
        const auto first = Take(session->Submit("first", "first"), "Submit first ask"); const auto ticket = Ask(events, first);
        Check(state->tools == 0 && state->models == 1 && ticket.operation_id == first.operation_id &&
            ticket.tool_name == "guarded" && ticket.cwd == Utf8(rig.cwd) && !ticket.child,
            "unapproved readonly/Yolo Action crossed Started or child ownership");
        Take(session->ResolveApproval(ticket.request_id, sdk::ApprovalDecision::AcceptForSession), "approve Action"); Succeeded(Wait(session, first));
        const auto second = Take(session->Submit("second", "second"), "Submit second ask"); const auto again = Ask(events, second);
        Check(again.request_id != ticket.request_id && state->tools == 1, "ordinary temporary grant bypassed force Ask");
        Take(session->Cancel(second.operation_id), "Cancel pending Action");
        Check(Wait(session, second).state == sdk::OperationState::Cancelled && state->tools == 1, "pending Action cancellation executed a tool");
        Check(!session->ResolveApproval(again.request_id, sdk::ApprovalDecision::Accept), "late Action reply was accepted"); Take(session->Close(), "Close ask");
    } else if (name == "post-failure") {
        for (unsigned variant = 0; variant < 3; ++variant) {
            auto state = std::make_shared<State>(); auto options = rig.Options(state);
            auto post = Definition("post", ext::Point::PostAction); post.failure_policy = ext::FailurePolicy::KeepOriginal;
            post.required = variant == 1; if (variant == 2) post.failure_policy = ext::FailurePolicy::Abort;
            auto remaining = std::make_shared<std::atomic<bool>>(true);
            options.extensions.push_back(Registration(state, {post}, [remaining](const ext::Context&, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                if (remaining->exchange(false)) throw std::runtime_error("fixture post failure"); return Continue(next);
            }));
            auto session = rig.Open(std::move(options)); const auto failed = Run(session, "first");
            Check(failed.state == (variant == 0 ? sdk::OperationState::Succeeded : sdk::OperationState::Failed) && state->tools == 1 &&
                state->models == (variant == 0 ? 2u : 1u), "Post failure upgraded known work to Unknown or retried work");
            Saved(session, failed, state->marker + " raw", false);
            Succeeded(Run(session, "second")); Check(state->tools == 2, "required Post failure leaked into the next operation"); Take(session->Close(), "Close post");
        }
    } else if (name == "limits") {
        for (unsigned variant = 0; variant < 11; ++variant) {
            auto state = std::make_shared<State>(); auto options = rig.Options(state);
            const bool pre = variant < 3 || variant >= 7;
            auto definition = Definition("bounded", pre ? ext::Point::PreAction : ext::Point::PostAction); definition.required = true;
            if (variant == 8) { definition.observer = true; definition.required = false; }
            auto observer_next_rejected = std::make_shared<std::atomic<bool>>(false);
            auto definitions = std::vector<ext::HandlerDefinition>{definition};
            if (variant == 6) { auto second = Definition("second", ext::Point::PostAction); second.required = true;
                definitions.front().before = {"second"}; definitions.push_back(second); }
            options.extensions.push_back(Registration(state, std::move(definitions), [variant, observer_next_rejected](const ext::Context&, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                if (variant == 0) return Continue(next, R"({"arguments":{},"extra":true})");
                if (variant == 1) return Continue(next, R"({"arguments":{"value":"\u0000"}})");
                if (variant == 2) return Continue(next, std::string(1024 * 1024 + 1, 'x'));
                if (variant == 7) return Continue(next, "{\"arguments\":{\"value\":\"" + std::string(1, static_cast<char>(0xff)) + "\"}}");
                if (variant == 8) {
                    observer_next_rejected->store(!next.Call());
                    ext::HandlerReturn out; out.output_json = R"({"arguments":{"value":"observer-change"}})"; return out;
                }
                if (variant == 9) { const auto other = std::async(std::launch::async, [next] { return next.Call(); }).get();
                    Check(!other, "cross-thread Next consumed another thread's Action");
                    return std::unexpected(other.error()); }
                if (variant == 10) return ext::HandlerReturn::Denied("", "missing code");
                auto out = Continue(next); if (!out) return out;
                if (variant == 3) out->output_json = R"({"arguments":{"value":"forbidden"}})";
                else if (variant == 4) out->effects.push_back({ext::EffectType::ResultSupplement, "{\"text\":\"" + std::string(16 * 1024 + 1, 'x') + "\"}"});
                else for (unsigned n = 0; n < (variant == 6 ? 2u : 3u); ++n)
                    out->effects.push_back({ext::EffectType::ResultSupplement, "{\"text\":\"" + std::string(12 * 1024, 'x') + "\"}"});
                return out;
            }));
            auto session = rig.Open(std::move(options)); const auto operation = Run(session, "bounded");
            if (variant == 8) {
                Succeeded(operation);
                Check(observer_next_rejected->load() && state->pre == 1 && state->tools == 1 && state->models == 2,
                    "optional observer changed arguments, consumed Next or stopped the actual Action");
                Saved(session, operation, state->marker + " raw", false);
            } else {
                Check(operation.state == sdk::OperationState::Failed && state->tools == (pre ? 0u : 1u) && state->models == 1,
                    "invalid Action payload bypassed its true boundary or retried actual work");
                if (!pre) Saved(session, operation, state->marker + " raw", false);
            }
            Take(session->Close(), "Close limits");
        }
    } else if (name == "opening") {
        auto state = std::make_shared<State>(); auto options = rig.Options(state);
        auto first = Definition("first", ext::Point::PreAction), second = Definition("second", ext::Point::PreAction);
        first.before = {"second"}; second.before = {"first"};
        options.extensions.push_back(Registration(state, {first, second}, [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); }));
        Check(!rig.runtime->OpenSession(std::move(options)) && state->factories == 0 && state->models == 0 && state->tools == 0,
            "invalid Action plan ran a factory/model/effect before preflight");
        auto observer_state = std::make_shared<State>(); auto observer_options = rig.Options(observer_state);
        auto observer = Definition("required-observer", ext::Point::PreAction);
        observer.required = true; observer.observer = true;
        observer_options.extensions.push_back(Registration(observer_state, {observer},
            [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); }));
        Check(!rig.runtime->OpenSession(std::move(observer_options)) && observer_state->factories == 0 &&
            observer_state->models == 0 && observer_state->tools == 0,
            "required observer escaped declaration preflight or ran a factory/model/effect");
        bool journal = false; for (const auto& entry : fs::recursive_directory_iterator(rig.root / "data"))
            journal = journal || (entry.is_regular_file() && entry.path().extension() == ".jsonl");
        Check(!journal, "invalid Action plan published a main journal");
    } else if (name == "resume") {
        auto state = std::make_shared<State>(); auto options = rig.Options(state);
        auto registration = Registration(state, {Definition("pre", ext::Point::PreAction)},
            [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); });
        options.extensions.push_back(registration); auto session = rig.Open(std::move(options));
        const auto operation = Run(session, "seed"); Succeeded(operation); const auto id = session->id();
        Take(session->Close(), "Close seed"); session.reset();
        const auto directory = SessionDir(rig.root, id), plan = directory / "sdk-extension-plan.json", journal = directory / (id + ".jsonl");
        const auto old_plan = Read(plan), old_journal = Read(journal); const auto factories = state->factories.load();
        const auto resume = [&] (bool include) {
            auto value = rig.Options(state); value.system_prompt.clear(); value.resume_session_id = id;
            if (include) value.extensions.push_back(registration); return value;
        };
        Write(plan, old_plan + "\n"); Check(!rig.runtime->OpenSession(resume(true)), "Action plan byte drift was accepted"); Write(plan, old_plan);
        Write(plan, std::string(4 * 1024 * 1024 + 1, 'x'));
        Check(!rig.runtime->OpenSession(resume(true)), "oversized frozen Action plan was accepted"); Write(plan, old_plan);
        fs::remove(plan); Check(!rig.runtime->OpenSession(resume(true)), "missing half Action plan was accepted"); Write(plan, old_plan);
        Check(!rig.runtime->OpenSession(resume(false)), "removing all frozen Action points was accepted");
        Check(Read(journal) == old_journal && state->models == 2 && state->tools == 1 && state->factories == factories,
            "failed Action resume changed old journal or ran a factory/model/tool");
        auto reopened = rig.Open(resume(true)); const auto recovered = Take(reopened->WaitResult(operation.operation_id, 1s), "restored Action operation");
        Succeeded(recovered); Check(reopened->id() == id && recovered.turn_id == operation.turn_id && state->models == 2 && state->tools == 1,
            "Action resume replayed execution or changed owner"); Take(reopened->Close(), "Close restored");
    } else if (name == "close") {
        auto state = std::make_shared<State>(); auto gate = std::make_shared<Gate>(); auto options = rig.Options(state);
        options.extensions.push_back(Registration(state, {Definition("held", ext::Point::PreAction)},
            [gate](const ext::Context& context, const ext::Input&, ext::Next next) { gate->Enter(context.cancellation); return Continue(next); }));
        auto session = rig.Open(std::move(options)); std::future<sdk::Result<void>> closing;
        Release release{gate}; const auto receipt = Take(session->Submit("held", "held"), "Submit held"); gate->Wait(1);
        closing = std::async(std::launch::async, [session] { return session->Close(); });
        Check(closing.wait_for(15s) == std::future_status::ready, "Close did not cancel/wait for the real Action callback");
        Take(closing.get(), "Close held"); const auto operation = Take(session->WaitResult(receipt.operation_id, 1s), "closed operation");
        Check(operation.state == sdk::OperationState::Cancelled && operation.result_persisted && state->tools == 0 && state->destroyed == 1,
            "Close destroyed a live callback or let a cancelled Action execute");
        Check(!session->Submit("late", "late"), "closed Action Session admitted new work");
        Check(Take(session->DescribeExtensions(), "closed plan").find("sdkActionVersion") != std::string::npos, "closed Action plan snapshot disappeared");
    } else if (name == "isolation") {
        std::array<std::shared_ptr<State>, 4> states;
        std::array<std::shared_ptr<sdk::Session>, 4> sessions;
        std::array<sdk::Receipt, 4> receipts;
        auto gate = std::make_shared<Gate>(); Release release{gate};
        for (unsigned n = 0; n < 4; ++n) {
            auto state = std::make_shared<State>(); states[n] = state; state->marker = "ACTION_OWN_" + std::to_string(n);
            const auto cwd = n < 2 ? rig.cwd : rig.root / (n == 2 ? "project-b" : "project-c"); fs::create_directories(cwd);
            auto options = rig.Options(state, cwd); const auto expected_cwd = options.cwd;
            options.extensions.push_back(Registration(state, {Definition("held", ext::Point::PreAction)},
                [gate, expected_cwd](const ext::Context& context, const ext::Input&, ext::Next next) {
                    Check(context.effective_cwd && *context.effective_cwd == expected_cwd, "Action callback crossed cwd");
                    gate->Enter(context.cancellation); return Continue(next);
                }));
            sessions[n] = rig.Open(std::move(options)); receipts[n] = Take(sessions[n]->Submit("same-key", state->marker), "isolated Submit");
        }
        gate->Wait(4); Take(sessions[0]->Cancel(receipts[0].operation_id), "cancel own Action"); Take(sessions[1]->Close(), "close own Action");
        // Cancellation exits these actual held callbacks before releasing their peers.
        const auto first = Wait(sessions[0], receipts[0]); const auto second = Take(sessions[1]->ReadOperation(receipts[1].operation_id), "closed own result");
        Check(first.state == sdk::OperationState::Cancelled && second.state == sdk::OperationState::Cancelled && second.result_persisted && states[0]->tools == 0 && states[1]->tools == 0,
            "healthy Action cancellation/Close changed terminal or executed effects");
        gate->Release();
        for (unsigned n : {2u, 3u}) { const auto operation = Wait(sessions[n], receipts[n]); Succeeded(operation);
            Check(operation.final_text == states[n]->marker + " final" && states[n]->tools == 1, "cancel/Close leaked to another Action Session");
            Saved(sessions[n], operation, states[n]->marker + " raw", false); }
        for (unsigned n = 0; n < 4; ++n) { Take(sessions[n]->Close(), "isolation Close");
            const std::lock_guard lock(states[n]->mutex);
            Check(states[n]->scopes.size() == 1 && states[n]->scopes.front().session == sessions[n]->id() &&
                states[n]->scopes.front().operation == receipts[n].operation_id, "Action owner crossed another Session"); }
    } else throw std::runtime_error("unknown Action acceptance case: " + name);
    std::cout << "[sdk-action-path] " << name << std::endl;
}

void Actions(const fs::path& base) {
    for (const auto* name : {"chain", "deny", "rewrite-schema", "force-ask", "post-failure", "limits", "opening", "resume", "close", "isolation"})
        ActionCase(name, base / name);
}

// These real public entry points support native-only journal mutation fixtures.
// The relocated consumer itself never imports JSON/hash/private SDK code.
std::string ActionSeed(const fs::path& base) {
    Rig rig(base); auto state = std::make_shared<State>(); auto options = rig.Options(state);
    options.extensions.push_back(Registration(state, {Definition("pre", ext::Point::PreAction)},
        [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); }));
    auto session = rig.Open(std::move(options)); Succeeded(Run(session, "seed"));
    const auto id = session->id(); Take(session->Close(), "Close binding seed"); return id;
}
void ActionRestore(const fs::path& base, const std::string& id, bool reject) {
    Rig rig(base); auto state = std::make_shared<State>(); auto options = rig.Options(state);
    options.system_prompt.clear(); options.resume_session_id = id;
    options.extensions.push_back(Registration(state, {Definition("pre", ext::Point::PreAction)},
        [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); }));
    const auto directory = SessionDir(rig.root, id), journal = directory / (id + ".jsonl");
    const auto bytes = Read(journal); auto session = rig.runtime->OpenSession(std::move(options));
    if (reject) {
        std::cerr << "[sdk-action-binding] actual_open=" << session.has_value()
                  << " error_code=" << (session ? std::string{} : session.error().code)
                  << " factories=" << state->factories.load() << " models=" << state->models.load()
                  << " tools=" << state->tools.load() << " journal_unchanged=" << (Read(journal) == bytes) << '\n';
        Check(!session, "bad Action binding entered the real Session");
        Check(session.error().code == "sdk.action.plan_invalid", "Action binding rejected at the wrong phase: " + session.error().code);
        Check(state->factories == 0 && Read(journal) == bytes, "bad Action binding wrote the old journal or ran a factory");
    } else {
        auto opened = Take(std::move(session), "valid binding restore"); Take(opened->Close(), "Close valid binding restore");
    }
    Check(state->models == 0 && state->tools == 0, "Action binding restore reran model or tool");
}
} // namespace lubancore_consumer
