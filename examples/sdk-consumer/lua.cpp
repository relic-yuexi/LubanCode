#include <lubancore/core.hpp>
#include <lubancore/lua.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// This host uses only installed SDK headers and the standard library. The
// provider supplies requests; Core owns permissions, execution and persistence.
namespace lubancore_consumer {
namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore;
using namespace std::chrono_literals;
constexpr const char* kName = "plugin__counter__count";
const std::string kScript =
    "assert(print==nil,'stdio print must be disabled'); "
    "assert(pcall==nil and xpcall==nil and coroutine==nil and debug==nil,'script catches must be disabled'); "
    "local calls=0; return {name='count',description='owned Lua counter',"
    "input_schema={type='object',properties={text={type='string'},loop={type='boolean'}},required={'text'}},"
    "execute=function(input) assert(pcall==nil and xpcall==nil,'script catches were restored'); "
    "if input.loop then while true do end end "
    "calls=calls+1; return tostring(calls)..':'..input.text end}";

void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("lua: " + message);
}
template<class T> T Take(sdk::Result<T> value, const std::string& action) {
    if (!value) throw std::runtime_error("lua: " + action + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const std::string& action) {
    if (!value) throw std::runtime_error("lua: " + action + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Check(out.is_open(), "cannot create fixture");
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close(); Check(!out.fail(), "cannot close fixture");
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); Check(in.is_open(), "cannot read fixture");
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Check(!in.bad(), "fixture read failed"); return bytes;
}
sdk::lua::v1::Selection Select(const fs::path& scripts) {
    sdk::lua::v1::Selection value;
    value.root = Utf8(fs::canonical(scripts)); value.scripts = {{"counter.lua", kName}};
    value.instruction_budget = 2'000'000'000;
    value.memory_cap_bytes = 1024 * 1024;
    value.wall_budget = 5s;
    return value;
}
void Prepare(const fs::path& base) {
    fs::create_directories(base / "project");
    fs::create_directories(base / "resources");
    Write(base / "scripts/counter.lua", kScript);
    // Loading every directory member would execute this fatal unselected file.
    Write(base / "scripts/unselected.lua", "error('unselected script was executed')");
}
std::unique_ptr<sdk::Runtime> Runtime(const fs::path& base) {
    return Take(sdk::Runtime::Create({Utf8(base / "data"), Utf8(base / "resources")}), "create runtime");
}
struct Observed {
    std::atomic<unsigned> requests{0}, actual_replies{0};
};
struct ModelGate {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned entered = 0;
    bool released = false;
    void Enter(sdk::Cancellation cancel) {
        std::unique_lock lock(mutex); ++entered; cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!released && !cancel.requested() && std::chrono::steady_clock::now() < deadline)
            cv.wait_for(lock, 5ms);
        Check(released && !cancel.requested(), "overlapping model gate was cancelled or timed out");
    }
    void WaitAndRelease() {
        std::unique_lock lock(mutex);
        Check(cv.wait_for(lock, 20s, [&] { return entered == 4; }), "four real Session workers did not overlap");
        released = true; cv.notify_all();
    }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
};
struct ReleaseModels { std::shared_ptr<ModelGate> gate; ~ReleaseModels() { gate->Release(); } };
class Provider final : public sdk::Backend {
public:
    Provider(std::shared_ptr<Observed> observed, bool enabled = true,
             std::shared_ptr<ModelGate> gate = {}, std::string model = "lua-fixture")
        : observed_(std::move(observed)), enabled_(enabled), gate_(std::move(gate)), model_(std::move(model)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        ++observed_->requests;
        Check(request.model == model_, "model configuration crossed Sessions");
        const auto definition = std::find_if(request.tools.begin(), request.tools.end(),
            [](const auto& tool) { return tool.name == kName; });
        Check((definition != request.tools.end()) == enabled_, "real request has the wrong Lua surface");
        if (!enabled_) return sdk::ModelReply{"off", {}, {}};
        Check(definition->input_schema_json.find("object") != std::string::npos,
              "real Lua schema was not exported");
        std::size_t user = request.messages.size();
        for (std::size_t i = request.messages.size(); i != 0; --i) {
            const auto& message = request.messages[i - 1];
            if (message.role == "user" && message.tool_replies.empty() && !message.text.empty()) {
                user = i - 1; break;
            }
        }
        Check(user != request.messages.size(), "provider has no current user request");
        for (std::size_t i = user + 1; i < request.messages.size(); ++i) {
            for (const auto& reply : request.messages[i].tool_replies) {
                Check(reply.call_id == "lua-call", "model reply changed the provider wire call identity");
                ++observed_->actual_replies;
                return sdk::ModelReply{reply.is_error ? "tool-error:" + reply.text : reply.text, {}, {}};
            }
        }
        const bool loop = request.messages[user].text == "loop";
        if (gate_) { gate_->Enter(cancel); gate_.reset(); }
        sdk::ModelReply reply;
        reply.tool_calls.push_back({"lua-call", kName,
            std::string("{\"text\":\"value\",\"loop\":") + (loop ? "true}" : "false}")});
        return reply;
    }
private:
    std::shared_ptr<Observed> observed_;
    bool enabled_;
    std::shared_ptr<ModelGate> gate_;
    std::string model_;
};
sdk::SessionOptions Options(const fs::path& base, std::shared_ptr<Observed> observed,
                            bool enabled = true, std::string resume = {}) {
    sdk::SessionOptions options;
    options.cwd = Utf8(base / "project"); options.model = "lua-fixture";
    options.backend = std::make_unique<Provider>(std::move(observed), enabled);
    options.approval_mode = sdk::ApprovalMode::Yolo;
    options.max_steps_per_turn = 4;
    options.resume_session_id = std::move(resume);
    if (enabled) options.lua = Select(base / "scripts");
    return options;
}
sdk::Operation Run(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    auto receipt = Take(session->Submit(key, "count"), "submit Lua");
    auto result = Take(session->WaitResult(receipt.operation_id, 20s), "wait Lua");
    Check(result.state == sdk::OperationState::Succeeded && result.result_persisted, "Lua did not finish durably");
    const auto captures = Take(session->ListToolResults(result.operation_id), "list actual Lua results");
    std::string diagnostics = "\nLua query session=" + session->id() + " operation=" + result.operation_id +
        " turn=" + result.turn_id + " final_text=" + result.final_text;
    for (const auto& row : captures) {
        const auto& id = row.identity;
        diagnostics += "\nresult session=" + id.session_id + " operation=" + id.operation_id +
            " turn=" + id.turn_id + " action=" + id.tool_call_id + " persisted=" + id.persisted_event_id +
            " result=" + id.result_id + " tool=" + row.tool_name + " attempt=" + std::to_string(row.attempt) +
            " selected=" + (row.selected ? "true" : "false");
    }
    Check(captures.size() == 2, "one Lua action did not retain both raw and formal sources" + diagnostics);
    Check(std::count_if(captures.begin(), captures.end(), [](const auto& row) {
              return row.identity.result_id.starts_with("capture-"); }) == 1 &&
          std::count_if(captures.begin(), captures.end(), [](const auto& row) {
              return row.identity.result_id.starts_with("res-"); }) == 1,
          "Lua raw or formal source is missing or duplicated" + diagnostics);
    Check(captures[0].identity.result_id != captures[1].identity.result_id &&
          captures[0].identity.persisted_event_id != captures[1].identity.persisted_event_id,
          "Lua raw and formal records reuse a persisted identity" + diagnostics);
    // Provider IDs remain wire correlation keys. The committed V3 action owns
    // both persisted sources and receives its own identity from the writer.
    Check(!captures[0].identity.tool_call_id.empty() &&
          captures[0].identity.tool_call_id == captures[1].identity.tool_call_id &&
          captures[0].identity.tool_call_id != "lua-call",
          "Lua sources do not share the actual persisted action identity" + diagnostics);
    for (const auto& row : captures) {
        const auto& identity = row.identity;
        Check(row.selected && row.tool_name == kName && row.attempt == 1 &&
              identity.session_id == session->id() && identity.operation_id == result.operation_id &&
              identity.turn_id == result.turn_id && !identity.tool_call_id.empty() &&
              !identity.persisted_event_id.empty(), "Lua result did not enter this action's selected V3 chain" + diagnostics);
        const auto saved = Take(session->ReadToolResult(identity), "read actual Lua source" + diagnostics);
        Check(saved.result().summary.identity == identity && saved.result().metadata_state == sdk::results::v1::ArtifactState::Verified &&
              std::any_of(saved.result().channels.begin(), saved.result().channels.end(),
                [&](const auto& channel) { return channel.text && channel.artifact_verified && *channel.text == result.final_text; }),
              "model reply differs from the verified stored Lua source" + diagnostics);
    }
    return result;
}
void WaitApproval(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    auto stream = Take(session->Subscribe(), "subscribe approval");
    while (std::chrono::steady_clock::now() < deadline) {
        const auto pending = session->PendingApprovals();
        if (!pending.empty()) {
            Check(pending.size() == 1 && pending[0].tool_name == kName &&
                  pending[0].operation_id == receipt.operation_id && !pending[0].child,
                  "Lua approval crossed the Session boundary");
            Take(session->ResolveApproval(pending[0].request_id, sdk::ApprovalDecision::Accept), "approve actual Lua");
            stream->Close(); return;
        }
        (void)Take(stream->Next(25ms), "wait approval event");
    }
    throw std::runtime_error("lua: real approval did not arrive");
}
void WaitDispatch(const std::shared_ptr<sdk::EventStream>& stream, const std::string& operation) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto event = Take(stream->Next(50ms), "wait actual Lua dispatch");
        if (event && event->operation_id == operation && event->kind == "item.started" &&
            event->payload_json.find(kName) != std::string::npos) return;
    }
    throw std::runtime_error("lua: actual tool dispatch did not arrive");
}
}

void LuaCase(const std::string& name, const fs::path& base) {
    Prepare(base); auto runtime = Runtime(base);
    auto observed = std::make_shared<Observed>();
    if (name == "off-and-visible") {
        auto off = Take(runtime->OpenSession(Options(base, observed, false)), "open default-off Session");
        Check(!Take(off->DescribeLua(), "describe off").enabled, "Lua defaulted on");
        const auto receipt = Take(off->Submit("off", "plain"), "submit off");
        Check(Take(off->WaitResult(receipt.operation_id, 20s), "wait off").final_text == "off", "off request failed");
        Take(off->Close(), "close off");
        auto active = Take(runtime->OpenSession(Options(base, observed)), "open selected Lua");
        Check(Run(active, "visible").final_text == "1:value", "first selected Lua call failed");
        const auto snapshot = Take(active->DescribeLua(), "describe selected");
        Check(snapshot.enabled && snapshot.session_id == active->id() && snapshot.entries.size() == 1 &&
              snapshot.entries[0].tool_name == kName && snapshot.entries[0].content_sha256.size() == 64 &&
              snapshot.plan_sha256.size() == 64, "owned Lua declaration is incomplete");
        Take(active->Close(), "close selected");
        Check(Take(active->DescribeLua(), "describe closed").plan_sha256 == snapshot.plan_sha256,
              "closed metadata retained no owned declaration");
    } else if (name == "four-sessions") {
        std::array<std::shared_ptr<sdk::Session>, 4> sessions;
        auto gate = std::make_shared<ModelGate>();
        ReleaseModels release{gate}; // Unblock model calls before Session destructors, even on failure.
        for (std::size_t i = 0; i != sessions.size(); ++i) {
            auto own = std::make_shared<Observed>();
            auto options = Options(base, own);
            options.model = "lua-fixture-" + std::to_string(i);
            options.backend = std::make_unique<Provider>(own, true, gate, options.model);
            if (i >= 2) {
                const auto project = base / ("project-" + std::to_string(i));
                fs::create_directories(project); options.cwd = Utf8(project);
            }
            sessions[i] = Take(runtime->OpenSession(std::move(options)), "open isolated VM");
        }
        std::array<sdk::Receipt, 4> receipts;
        for (std::size_t i = 0; i != sessions.size(); ++i)
            receipts[i] = Take(sessions[i]->Submit("overlap", "count"), "submit overlapping Lua");
        gate->WaitAndRelease();
        for (std::size_t i = 0; i != sessions.size(); ++i) {
            const auto result = Take(sessions[i]->WaitResult(receipts[i].operation_id, 20s), "wait overlapping Lua");
            Check(result.state == sdk::OperationState::Succeeded && result.final_text == "1:value" && result.result_persisted,
                  "VM count crossed overlapping Sessions");
        }
        Take(sessions[0]->Close(), "close first shared project");
        for (std::size_t i = 1; i != sessions.size(); ++i) {
            Check(Run(sessions[i], "second").final_text == "2:value", "closing a sibling reset its VM");
            Take(sessions[i]->Close(), "close isolated VM");
        }
    } else if (name == "approval") {
        auto options = Options(base, observed); options.approval_mode = sdk::ApprovalMode::Confirm;
        auto session = Take(runtime->OpenSession(std::move(options)), "open approval Session");
        const auto receipt = Take(session->Submit("ask", "count"), "submit approved Lua");
        WaitApproval(session, receipt);
        const auto result = Take(session->WaitResult(receipt.operation_id, 20s), "wait approved Lua");
        Check(result.state == sdk::OperationState::Succeeded && result.final_text == "1:value", "approval did not reach real VM");
        Check(session->PendingApprovals().empty(), "closed approval retained a pending ticket");
        Take(session->Close(), "close approval Session");
    } else if (name == "cancel-and-close") {
        auto session = Take(runtime->OpenSession(Options(base, observed)), "open cancelling Session");
        auto stream = Take(session->Subscribe(), "subscribe actual dispatch");
        const auto receipt = Take(session->Submit("cancel-loop", "loop"), "submit cancelling Lua");
        WaitDispatch(stream, receipt.operation_id); Take(session->Cancel(receipt.operation_id), "cancel actual Lua");
        Check(Take(session->WaitResult(receipt.operation_id, 20s), "wait cancelled Lua").state == sdk::OperationState::Cancelled,
              "Lua cancellation did not end its operation");
        Check(Run(session, "after-cancel").final_text == "1:value", "cancel contaminated the following call");
        const auto closing = Take(session->Submit("close-loop", "loop"), "submit closing Lua");
        WaitDispatch(stream, closing.operation_id); Take(session->Close(), "join active Lua on Close");
        Check(!session->Submit("late", "count"), "closed Session admitted another Lua call");
        Check(Take(session->DescribeLua(), "describe after join").enabled, "Close lost owned Lua declaration");
        stream->Close();
    } else if (name == "invalid-budget") {
        for (unsigned i = 0; i != 3; ++i) {
            auto options = Options(base, observed);
            if (i == 0) options.lua->instruction_budget = 0;
            if (i == 1) options.lua->memory_cap_bytes = 0;
            if (i == 2) options.lua->wall_budget = 0ms;
            const auto failed = runtime->OpenSession(std::move(options));
            Check(!failed && failed.error().code == "sdk.lua.invalid_selection", "zero budget opened a VM");
        }
        Check(observed->requests == 0, "budget rejection called the model");
    } else if (name == "bad-declarations") {
        for (unsigned i = 0; i != 4; ++i) {
            auto options = Options(base, observed);
            if (i == 0) options.lua->scripts[0].path = "missing.lua";
            if (i == 1) options.lua->scripts[0].tool_name = "plugin__counter__different";
            if (i == 2) options.lua->scripts.push_back(options.lua->scripts[0]);
            if (i == 3) options.custom_tools.push_back({kName, "collision", "{\"type\":\"object\"}", false,
                [](const auto&, const auto&) -> sdk::Result<sdk::ToolResult> { throw std::runtime_error("colliding tool ran"); }});
            const auto failed = runtime->OpenSession(std::move(options));
            Check(!failed, "missing, mismatched, duplicate or colliding script opened a Session");
        }
        Check(observed->requests == 0, "bad declaration called the model");
        auto healthy = Take(runtime->OpenSession(Options(base, observed)), "open after declarations failed");
        Check(Run(healthy, "healthy").final_text == "1:value", "failed declaration retained a VM");
        Take(healthy->Close(), "close healthy declaration");
    } else if (name == "resume-fresh-vm" || name == "resume-drift") {
        auto session = Take(runtime->OpenSession(Options(base, observed)), "open resume seed");
        Check(Run(session, "seed").final_text == "1:value", "resume seed failed");
        const auto id = session->id(); const auto plan = Take(session->DescribeLua(), "describe seed").plan_sha256;
        Take(session->Close(), "close resume seed");
        if (name == "resume-drift") {
            Write(base / "scripts/counter.lua", kScript + "\n-- source drift");
            const auto failed = runtime->OpenSession(Options(base, observed, true, id));
            Check(!failed, "same-ID source drift entered a Session");
            Check(observed->requests == 2, "drift rejection called the model or reran a tool");
            Write(base / "scripts/counter.lua", kScript);
        }
        auto options = Options(base, observed, true, id); options.lua.reset();
        auto restored = Take(runtime->OpenSession(std::move(options)), "inherit frozen Lua");
        Check(restored->id() == id && Take(restored->DescribeLua(), "describe inherited").plan_sha256 == plan,
              "resume changed the owned identity");
        Check(observed->requests == 2, "resume reran old model work");
        Check(Run(restored, "new-turn").final_text == "1:value", "resume restored old globals or replayed an old tool");
        Take(restored->Close(), "close inherited Lua");
    } else throw std::runtime_error("lua: unknown acceptance case: " + name);
    Take(runtime->Shutdown(), "shutdown Lua runtime");
    std::cout << "[sdk-lua-path] " << name << '\n';
}

void Lua(const fs::path& base) {
    for (const auto* name : {"off-and-visible", "four-sessions", "approval", "cancel-and-close",
                            "invalid-budget", "bad-declarations", "resume-fresh-vm", "resume-drift"})
        LuaCase(name, base / name);
}
void LuaSeed(const fs::path& base) {
    Prepare(base); auto runtime = Runtime(base); auto observed = std::make_shared<Observed>();
    auto session = Take(runtime->OpenSession(Options(base, observed)), "open separate-process seed");
    Check(Run(session, "seed").final_text == "1:value", "separate-process seed failed");
    Write(base / "session.txt", session->id());
    Take(session->Close(), "close separate-process seed"); Take(runtime->Shutdown(), "shutdown seed");
}
void LuaResume(const fs::path& base) {
    auto runtime = Runtime(base); auto observed = std::make_shared<Observed>();
    auto options = Options(base, observed, true, Read(base / "session.txt")); options.lua.reset();
    auto session = Take(runtime->OpenSession(std::move(options)), "restore separate-process Lua");
    Check(observed->requests == 0, "fresh host replayed a model during Open");
    Check(Run(session, "after-process-restart").final_text == "1:value", "fresh host did not create a fresh VM");
    Take(session->Close(), "close separate-process resume"); Take(runtime->Shutdown(), "shutdown resume");
}
void LuaAdoptSystem(const fs::path& base, const std::string& id) {
    auto runtime = Runtime(base); auto observed = std::make_shared<Observed>();
    auto options = Options(base, observed, true, id); options.lua.reset();
    options.system_prompt = "Lua fixture: adopted replacement system";
    auto session = Take(runtime->OpenSession(std::move(options)), "adopt replacement system");
    Check(session->id() == id && observed->requests == 0, "system replacement changed identity or ran the model");
    Take(session->Close(), "close system replacement"); Take(runtime->Shutdown(), "shutdown replacement");
}
namespace {
void RestoreRejected(const fs::path& base, const std::string& id, const char* expected_code) {
    auto runtime = Runtime(base); auto observed = std::make_shared<Observed>();
    auto options = Options(base, observed, true, id); options.lua.reset();
    const auto failed = runtime->OpenSession(std::move(options));
    std::cout << "[sdk-lua-rejection] actual_open=" << failed.has_value()
              << " code=" << (failed ? std::string{} : failed.error().code)
              << " models=" << observed->requests.load() << '\n';
    Check(!failed, "bad owned Lua declaration opened a Session");
    Check(failed.error().code == expected_code, "bad declaration returned the wrong precheck phase");
    Check(observed->requests == 0, "bad declaration ran a model");
    Take(runtime->Shutdown(), "shutdown rejected restore");
}
} // namespace
void LuaRestoreBad(const fs::path& base, const std::string& id) {
    RestoreRejected(base, id, "sdk.lua.plan_invalid");
}
void LuaRestoreForeignSystem(const fs::path& base, const std::string& id) {
    RestoreRejected(base, id, "sdk.job.plan_invalid");
}
} // namespace lubancore_consumer
