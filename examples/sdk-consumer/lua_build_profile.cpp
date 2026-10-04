// Installed public SDK and STL only. Each phase runs against one loaded SDK.
#include <lubancore/core.hpp>
#include <lubancore/lua.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef LUBANCORE_CONSUMER_WITH_LUA
#error "The actual installed/build profile must be declared"
#endif

namespace lubancore_consumer {
namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore;
using namespace std::chrono_literals;
constexpr bool kWithLua = LUBANCORE_CONSUMER_WITH_LUA != 0;
constexpr const char* kTool = "plugin__profile__value";
const std::string kScript =
    "return {name='value',description='profile fixture',input_schema={type='object'},"
    "execute=function() assert(pcall==nil and xpcall==nil); return 'lua-profile-value' end}";

void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("lua-build: " + message);
}
template<class T> T Take(sdk::Result<T> value, const char* operation) {
    if (!value) throw std::runtime_error(std::string("lua-build: ") + operation + ": " + value.error().code);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* operation) {
    if (!value) throw std::runtime_error(std::string("lua-build: ") + operation + ": " + value.error().code);
}
std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Check(out.is_open(), "fixture open failed");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close(); Check(!out.fail(), "fixture write failed");
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(in.is_open(), "fixture read open failed");
    std::string out{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Check(!in.bad(), "fixture read failed"); return out;
}
std::map<std::string, std::string> Files(const fs::path& root) {
    std::map<std::string, std::string> out;
    if (fs::exists(root)) for (const auto& item : fs::recursive_directory_iterator(root))
        if (item.is_regular_file()) out.emplace(Utf8(item.path().lexically_relative(root)), Read(item.path()));
    return out;
}
fs::path Plan(const fs::path& base, const std::string& id) {
    fs::path found;
    for (const auto& item : fs::recursive_directory_iterator(base / "data"))
        if (item.path().filename() == "sdk-lua-plan.json" && item.path().parent_path().filename() == id) {
            Check(found.empty(), "duplicate session plan"); found = item.path();
        }
    Check(!found.empty(), "actual session plan missing"); return found;
}
void Prepare(const fs::path& base) {
    Check(base.is_absolute(), "state root is relative");
    fs::create_directories(base / "project"); fs::create_directories(base / "resources");
    Write(base / "scripts/profile.lua", kScript);
    Write(base / "scripts/unselected.lua", "error('unselected Lua must never run')");
}
std::unique_ptr<sdk::Runtime> Runtime(const fs::path& base) {
    return Take(sdk::Runtime::Create({Utf8(base / "data"), Utf8(base / "resources")}), "Runtime::Create");
}
struct Observed {
    std::atomic<unsigned> requests{0}, tool_replies{0}, destroyed{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, block = false;
};
class Provider final : public sdk::Backend {
public:
    Provider(std::shared_ptr<Observed> state, bool lua, std::string model)
        : state_(std::move(state)), lua_(lua), model_(std::move(model)) {}
    ~Provider() override { ++state_->destroyed; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        ++state_->requests;
        Check(request.model == model_, "model crossed sessions");
        Check(std::any_of(request.tools.begin(), request.tools.end(), [](const auto& tool) {
            return tool.name == kTool;
        }) == lua_, "actual model request has wrong Lua surface");
        if (state_->block) {
            std::unique_lock lock(state_->mutex); state_->entered = true; state_->cv.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) state_->cv.wait_for(lock, 5ms);
            Check(cancel.requested(), "actual backend cancellation never arrived");
            return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        }
        if (!lua_) return sdk::ModelReply{"plain:" + model_, {}, {}};
        std::size_t user = request.messages.size();
        for (std::size_t i = request.messages.size(); i != 0; --i)
            if (request.messages[i - 1].role == "user" && !request.messages[i - 1].text.empty()) { user = i - 1; break; }
        Check(user != request.messages.size(), "actual model request has no user message");
        for (std::size_t i = user + 1; i < request.messages.size(); ++i)
            for (const auto& reply : request.messages[i].tool_replies) {
                Check(!reply.is_error && reply.text == "lua-profile-value", "actual Lua tool failed");
                ++state_->tool_replies; return sdk::ModelReply{reply.text, {}, {}};
            }
        sdk::ModelReply out; out.tool_calls.push_back({"profile-call", kTool, "{}"}); return out;
    }
private:
    std::shared_ptr<Observed> state_;
    bool lua_;
    std::string model_;
};
sdk::SessionOptions Options(const fs::path& base, const std::shared_ptr<Observed>& state,
                            bool lua = false, std::string resume = {}, std::string model = "profile") {
    sdk::SessionOptions out;
    out.cwd = Utf8(base / "project"); out.model = model; out.resume_session_id = std::move(resume);
    out.backend = std::make_unique<Provider>(state, lua, std::move(model));
    out.approval_mode = sdk::ApprovalMode::Yolo; out.max_steps_per_turn = 4;
    if (lua) {
        sdk::lua::v1::Selection selection;
        selection.root = Utf8(fs::canonical(base / "scripts")); selection.scripts = {{"profile.lua", kTool}};
        selection.instruction_budget = 2'000'000; selection.memory_cap_bytes = 1024 * 1024; selection.wall_budget = 5s;
        out.lua = std::move(selection);
    }
    return out;
}
void Run(const std::shared_ptr<sdk::Session>& session, const std::string& key, const std::string& text) {
    const auto receipt = Take(session->Submit(key, "execute"), "Submit");
    const auto result = Take(session->WaitResult(receipt.operation_id, 20s), "WaitResult");
    Check(result.state == sdk::OperationState::Succeeded && result.result_persisted && result.final_text == text,
          "actual Session did not complete durably");
}
std::pair<std::string, sdk::lua::v1::Snapshot> Seed(const fs::path& base, bool lua) {
    Prepare(base); auto runtime = Runtime(base); auto state = std::make_shared<Observed>();
    auto session = Take(runtime->OpenSession(Options(base, state, lua)), "seed OpenSession");
    const auto id = session->id(); const auto saved = Take(session->DescribeLua(), "seed DescribeLua");
    Check(saved.enabled == lua && saved.session_id == id && saved.plan_sha256.size() == 64, "seed snapshot differs");
    Run(session, "seed", lua ? "lua-profile-value" : "plain:profile");
    Check(!lua || state->tool_replies == 1, "seed did not execute its real Lua tool");
    Take(session->Close(), "seed Close");
    Check(Take(session->DescribeLua(), "closed DescribeLua").plan_sha256 == saved.plan_sha256, "Close lost owned snapshot");
    session.reset(); Take(runtime->Shutdown(), "seed Shutdown");
    return {id, saved};
}
void ResumeDisabled(const fs::path& base, const std::string& id) {
    const auto plan = Plan(base, id); const auto original = Read(plan);
    auto runtime = Runtime(base); auto state = std::make_shared<Observed>();
    auto session = Take(runtime->OpenSession(Options(base, state, false, id)), "disabled resume");
    const auto snapshot = Take(session->DescribeLua(), "resumed DescribeLua");
    Check(session->id() == id && !snapshot.enabled && snapshot.entries.empty(), "disabled restore changed identity or enabled Lua");
    Run(session, kWithLua ? "resumed-on" : "resumed-off", "plain:profile"); Take(session->Close(), "resumed Close");
    Take(runtime->Shutdown(), "resumed Shutdown"); Check(Read(plan) == original, "disabled resume rewrote frozen plan");
}
} // namespace

void LuaBuildCase(const std::string& name, const fs::path& base) {
    if (name == "default-off") {
        const auto [id, snapshot] = Seed(base, false);
        Check(!snapshot.enabled && snapshot.root.empty() && snapshot.entries.empty() && snapshot.instruction_budget == 0 &&
              snapshot.memory_cap_bytes == 0 && snapshot.wall_budget == 0ms, "default disabled snapshot differs");
        Check(Read(Plan(base, id)).find("\"enabled\":false") != std::string::npos, "disabled plan was not frozen");
    } else if (name == "selection") {
        Prepare(base); auto runtime = Runtime(base); auto state = std::make_shared<Observed>();
        const auto bytes = Read(base / "scripts/profile.lua");
        auto opened = runtime->OpenSession(Options(base, state, true));
        if constexpr (kWithLua) {
            auto session = Take(std::move(opened), "enabled OpenSession");
            Check(Take(session->DescribeLua(), "enabled DescribeLua").enabled, "full build did not enable Lua");
            Run(session, "selection", "lua-profile-value"); Check(state->tool_replies == 1, "real tool reply missing");
            Take(session->Close(), "enabled Close");
        } else {
            Check(!opened && opened.error().code == "sdk.lua.build_unavailable", "thin build did not refuse explicit Lua");
            Check(state->requests == 0 && Files(base / "data").empty(), "refusal started execution or published a session");
            auto missing = Options(base, state, true); missing.lua->scripts[0].path = "missing.lua";
            auto refused = runtime->OpenSession(std::move(missing));
            Check(!refused && refused.error().code == "sdk.lua.build_unavailable", "thin build read a selected missing script");
        }
        Take(runtime->Shutdown(), "selection Shutdown"); Check(Read(base / "scripts/profile.lua") == bytes, "selection changed source");
    } else if (name == "disabled-resume") {
        const auto [id, snapshot] = Seed(base, false); (void)snapshot; ResumeDisabled(base, id);
    } else if (name == "frozen-plan") {
        const auto [id, snapshot] = Seed(base, false); (void)snapshot;
        const auto plan = Plan(base, id); const auto original = Read(plan);
        for (const auto& broken : {original + " ", std::string("{}"), std::string("not-json")}) {
            Write(plan, broken); const auto before = Files(base / "data");
            auto runtime = Runtime(base); auto state = std::make_shared<Observed>();
            auto opened = runtime->OpenSession(Options(base, state, false, id));
            Check(!opened && opened.error().code == "sdk.lua.plan_invalid" && state->requests == 0, "bad frozen plan was accepted");
            Take(runtime->Shutdown(), "bad-plan Shutdown"); Check(Files(base / "data") == before, "bad-plan refusal changed original state");
        }
        Write(plan, original); ResumeDisabled(base, id);
    } else if (name == "isolation") {
        Prepare(base / "a"); Prepare(base / "b");
        auto run = [&](const fs::path& project, const std::string& model) {
            auto runtime = Runtime(project); auto state = std::make_shared<Observed>();
            auto session = Take(runtime->OpenSession(Options(project, state, false, {}, model)), "isolated OpenSession");
            Run(session, "isolated", "plain:" + model);
            const auto snapshot = Take(session->DescribeLua(), "isolated DescribeLua");
            Take(session->Close(), "isolated Close"); Take(runtime->Shutdown(), "isolated Shutdown"); return snapshot;
        };
        auto a = std::async(std::launch::async, [&] { return run(base / "a", "model-a"); });
        auto b = std::async(std::launch::async, [&] { return run(base / "b", "model-b"); });
        const auto first = a.get(), second = b.get();
        Check(!first.enabled && !second.enabled && first.session_id != second.session_id, "isolated sessions shared Lua identity");
    } else if (name == "lifetime") {
        Prepare(base); auto runtime = Runtime(base); auto state = std::make_shared<Observed>(); state->block = true;
        auto session = Take(runtime->OpenSession(Options(base, state)), "lifetime OpenSession");
        const auto receipt = Take(session->Submit("cancel", "execute"), "lifetime Submit");
        { std::unique_lock lock(state->mutex); Check(state->cv.wait_for(lock, 15s, [&] { return state->entered; }), "actual backend was not reached"); }
        Take(session->Cancel(receipt.operation_id), "actual Cancel"); Take(session->Close(), "actual Close");
        const auto snapshot = Take(session->DescribeLua(), "closed snapshot"); Check(!snapshot.enabled, "Close changed disabled snapshot");
        Check(!session->Submit("late", "execute"), "closed Session accepted work");
        session.reset(); Take(runtime->Shutdown(), "lifetime Shutdown"); runtime.reset();
        Check(state->destroyed == 1, "actual backend retained a dangling/extra owner");
        const auto moved = base / "retired"; fs::rename(base / "data", moved); fs::remove_all(moved);
        Check(!snapshot.session_id.empty() && snapshot.plan_sha256.size() == 64, "owned snapshot depended on retired files");
    } else throw std::runtime_error("unknown Lua build case");
}

void LuaBuildProfile(const fs::path& base) {
    for (const char* name : {"default-off", "selection", "disabled-resume", "frozen-plan", "isolation", "lifetime"})
        LuaBuildCase(name, base / name);
    std::cout << "[sdk-lua-build-profile] " << (kWithLua ? "on" : "off") << '\n';
    std::cout << "[sdk-lua-build-consumer] complete\n";
}
bool LuaBuildWithLua() { return kWithLua; }
std::string LuaBuildSeedDisabled(const fs::path& base) { return Seed(base, false).first; }
void LuaBuildRestoreBadBinding(const fs::path& base, const std::string& id) {
    const auto before = Files(base / "data");
    auto runtime = Runtime(base); auto state = std::make_shared<Observed>();
    auto opened = runtime->OpenSession(Options(base, state, false, id));
    Check(!opened && opened.error().code == "sdk.lua.plan_invalid" && state->requests == 0,
          "valid V3 with a foreign Lua binding was accepted or executed");
    Take(runtime->Shutdown(), "binding refusal Shutdown");
    Check(Files(base / "data") == before, "binding refusal changed original state");
}
void LuaBuildCross(const std::string& mode, const fs::path& base) {
    const auto off = base / "disabled", on = base / "enabled";
    if (mode == "lua-build-seed-off") {
        Check(kWithLua, "disabled seed must originate in full build");
        const auto [id, snapshot] = Seed(off, false); (void)snapshot; Write(base / "disabled-id", id);
    } else if (mode == "lua-build-resume-off") {
        const auto id = Read(base / "disabled-id"); ResumeDisabled(off, id);
    } else if (mode == "lua-build-seed-on") {
        Check(kWithLua, "enabled seed must originate in full build");
        const auto [id, snapshot] = Seed(on, true); (void)snapshot; Write(base / "enabled-id", id);
    } else if (mode == "lua-build-reject-on") {
        Check(!kWithLua, "enabled refusal must use thin build");
        const auto before = Files(on / "data"); const auto script = Read(on / "scripts/profile.lua");
        auto runtime = Runtime(on); auto state = std::make_shared<Observed>();
        auto opened = runtime->OpenSession(Options(on, state, false, Read(base / "enabled-id")));
        Check(!opened && opened.error().code == "sdk.lua.build_unavailable", "enabled source was silently disabled");
        Take(runtime->Shutdown(), "cross refusal Shutdown");
        Check(state->requests == 0 && Files(on / "data") == before && Read(on / "scripts/profile.lua") == script,
              "thin refusal executed or changed original enabled state");
    } else throw std::runtime_error("unknown Lua cross mode");
    std::cout << "[sdk-lua-build-cross] " << mode << ' ' << (kWithLua ? "on" : "off") << " complete\n";
}
} // namespace lubancore_consumer
