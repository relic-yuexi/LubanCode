// Installed public SDK and STL only. Todo state and execution stay in the SDK.
#include <lubancore/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore;
using namespace std::chrono_literals;
const std::string kFirst = R"({"items":[{"content":"step alpha","status":"pending"},{"content":"step beta","status":"in_progress"}]})";
const std::string kReplaced = R"({"items":[{"content":"step alpha","status":"completed"}]})";
const std::string kOther = R"({"items":[{"content":"other board","status":"pending"}]})";
const std::string kEmpty = R"({"items":[]})";

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("todo-write: " + message);
}
template<class T> T Take(sdk::Result<T> value, const char* operation) {
    if (!value) throw std::runtime_error(std::string("todo-write: ") + operation + ": " +
        value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* operation) {
    if (!value) throw std::runtime_error(std::string("todo-write: ") + operation + ": " +
        value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct Directory {
    fs::path path;
    explicit Directory(const fs::path& base) {
        Check(base.is_absolute(), "fixture root must be explicit and absolute");
        static std::atomic<unsigned> next{0};
        path = base / ("todo-write-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(next++));
        fs::create_directories(path / "project");
        fs::create_directories(path / "other-project");
        fs::create_directories(path / "resources");
    }
    ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
std::size_t Files(const fs::path& root) {
    std::size_t count = 0;
    if (fs::exists(root)) for (const auto& item : fs::recursive_directory_iterator(root))
        if (item.is_regular_file()) ++count;
    return count;
}
std::unique_ptr<sdk::Runtime> Runtime(const fs::path& base, const char* data = "data") {
    return Take(sdk::Runtime::Create({Utf8(base / data), Utf8(base / "resources")}), "Runtime::Create");
}
struct Rendezvous {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned arrived = 0;
    void Meet() {
        std::unique_lock lock(mutex);
        ++arrived; cv.notify_all();
        Check(cv.wait_for(lock, 15s, [&] { return arrived == 4; }), "four actual Sessions did not overlap");
    }
};
struct Observed {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> inputs;
    std::vector<sdk::ToolReply> replies;
    std::optional<sdk::ToolReply> old_reply;
    std::shared_ptr<Rendezvous> rendezvous;
    std::optional<std::size_t> block_at;
    std::atomic<unsigned> requests{0}, destroyed{0}, cancellations{0};
    std::size_t turn = 0;
    bool pending = false, entered = false, saw_old_reply = false;
};
std::string CallId(const std::string& model, std::size_t turn) {
    return "todo-" + model + "-" + std::to_string(turn);
}
std::string Final(const std::string& model, std::size_t turn) {
    return "todo complete:" + model + ":" + std::to_string(turn);
}
class Backend final : public sdk::Backend {
public:
    Backend(std::shared_ptr<Observed> state, std::string model, bool admitted)
        : state_(std::move(state)), model_(std::move(model)), admitted_(admitted) {}
    ~Backend() override { ++state_->destroyed; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        ++state_->requests;
        Check(request.model == model_, "model crossed Sessions");
        const auto definitions = std::count_if(request.tools.begin(), request.tools.end(), [](const auto& tool) {
            return tool.name == "todo_write";
        });
        Check(definitions == (admitted_ ? 1 : 0), "actual tool surface differs from explicit admission");
        if (admitted_) {
            const auto tool = std::find_if(request.tools.begin(), request.tools.end(), [](const auto& value) {
                return value.name == "todo_write";
            });
            for (const char* field : {"items", "content", "status", "pending", "in_progress", "completed"})
                Check(tool->input_schema_json.find(field) != std::string::npos, "actual todo schema missing a field");
        }
        std::unique_lock lock(state_->mutex);
        if (state_->old_reply && !state_->saw_old_reply) {
            for (const auto& message : request.messages) for (const auto& reply : message.tool_replies)
                if (reply.call_id == state_->old_reply->call_id && reply.text == state_->old_reply->text &&
                    reply.is_error == state_->old_reply->is_error) state_->saw_old_reply = true;
            Check(state_->saw_old_reply, "same-ID resume lost actual historical tool reply");
        }
        if (state_->block_at && state_->turn == *state_->block_at && !state_->pending) {
            state_->entered = true; state_->cv.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) state_->cv.wait_for(lock, 5ms);
            Check(cancel.requested(), "SDK never cancelled actual backend work");
            ++state_->cancellations;
            return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        }
        if (!admitted_) return sdk::ModelReply{"todo absent:" + model_, {}, std::nullopt};
        Check(state_->turn < state_->inputs.size(), "unexpected extra model/tool round");
        if (!state_->pending) {
            if (state_->rendezvous && state_->turn == 0) state_->rendezvous->Meet();
            state_->pending = true;
            return sdk::ModelReply{"", {{CallId(model_, state_->turn), "todo_write", state_->inputs[state_->turn]}}, std::nullopt};
        }
        std::optional<sdk::ToolReply> captured;
        const auto id = CallId(model_, state_->turn);
        for (const auto& message : request.messages) for (const auto& reply : message.tool_replies) if (reply.call_id == id) {
            Check(!captured, "one actual invocation produced duplicate model replies");
            captured = reply;
        }
        Check(captured.has_value(), "actual todo result did not reach model history");
        state_->replies.push_back(std::move(*captured));
        const auto completed = state_->turn++;
        state_->pending = false;
        return sdk::ModelReply{Final(model_, completed), {}, std::nullopt};
    }
private:
    std::shared_ptr<Observed> state_;
    std::string model_;
    bool admitted_;
};
std::shared_ptr<Observed> State(std::vector<std::string> inputs) {
    auto state = std::make_shared<Observed>(); state->inputs = std::move(inputs); return state;
}
sdk::SessionOptions Options(const fs::path& cwd, const std::shared_ptr<Observed>& state,
                            const std::string& model, bool admitted = true, std::string resume = {}) {
    sdk::SessionOptions out;
    out.cwd = Utf8(cwd); out.model = model; out.resume_session_id = std::move(resume);
    out.backend = std::make_unique<Backend>(state, model, admitted);
    if (admitted) out.builtin_tools = {"todo_write"};
    out.max_steps_per_turn = 4;
    // Confirm stays enabled. A todo write must not invent an approval request.
    return out;
}
sdk::Receipt Submit(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    return Take(session->Submit(key, "update the private todo board: " + key), "Submit");
}
sdk::ToolReply Finish(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<Observed>& state,
                      const std::string& model, std::size_t turn, const sdk::Receipt& receipt) {
    const auto result = Take(session->WaitResult(receipt.operation_id, 30s), "WaitResult");
    Check(result.state == sdk::OperationState::Succeeded && result.result_persisted && result.final_text == Final(model, turn),
        "actual todo turn did not finish durably: " + result.error);
    Check(session->PendingApprovals().empty(), "todo unexpectedly requested approval");
    std::lock_guard lock(state->mutex);
    Check(state->requests == 2 * (turn + 1) && state->replies.size() == turn + 1,
        "model/tool work did not run exactly once");
    return state->replies.back();
}
sdk::ToolReply Turn(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<Observed>& state,
                    const std::string& model, std::size_t turn) {
    return Finish(session, state, model, turn, Submit(session, model + "-request-" + std::to_string(turn)));
}
void Reply(const sdk::ToolReply& reply, const char* expected, bool error = false) {
    Check(reply.is_error == error && reply.text.find(expected) != std::string::npos,
        "wrong actual tool receipt for " + reply.call_id + ": " + reply.text);
}
void Closed(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<Observed>& state) {
    Take(session->Close(), "Close"); Take(session->Close(), "repeated Close");
    Check(state->destroyed == 1, "Close retained or twice destroyed the actual backend");
    Check(!session->Submit("late", "closed board"), "closed Session accepted a todo turn");
}
void WaitBlocked(const std::shared_ptr<Observed>& state) {
    std::unique_lock lock(state->mutex);
    Check(state->cv.wait_for(lock, 15s, [&] { return state->entered; }), "actual backend did not enter pending work");
}
void Admission(const fs::path& root) {
    auto runtime = Runtime(root);
    auto off = State({});
    auto plain = Take(runtime->OpenSession(Options(root / "project", off, "default-off", false)), "default OpenSession");
    const auto receipt = Submit(plain, "default");
    const auto result = Take(plain->WaitResult(receipt.operation_id, 30s), "default WaitResult");
    Check(result.state == sdk::OperationState::Succeeded && result.final_text == "todo absent:default-off" &&
        off->requests == 1 && off->replies.empty(), "default SDK enabled todo");
    Closed(plain, off);
    const auto before = Files(root / "data");
    for (const unsigned kind : {0u, 1u, 2u, 3u}) {
        auto state = State({kFirst}); auto options = Options(root / "project", state, "refused");
        std::atomic<unsigned> custom_calls{0};
        const char* code = "sdk.tool.unsupported_or_duplicate";
        if (kind == 0) options.builtin_tools = {"todo_write_unknown"};
        else if (kind == 1) options.builtin_tools.push_back("todo_write");
        else if (kind == 2) {
            sdk::Tool custom; custom.name = "todo_write"; custom.description = "must not replace the built-in";
            custom.requires_approval = false;
            custom.execute = [&](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
                ++custom_calls; return sdk::ToolResult{"wrong implementation", false};
            };
            options.custom_tools.push_back(std::move(custom)); code = "sdk.tool.duplicate";
        } else {
            options.subagents = sdk::subagents::v1::Options{true, {{"general-purpose", {"todo_write"}, "", 2, 5}}};
            code = "sdk.subagent.invalid_plan";
        }
        const auto opened = runtime->OpenSession(std::move(options));
        Check(!opened && opened.error().code == code, "invalid admission/child tool was not refused before execution");
        Check(state->requests == 0 && state->destroyed == 1 && custom_calls == 0 && Files(root / "data") == before,
            "refused admission executed/published work or retained its owner");
    }
    auto enabled = State({kFirst});
    auto session = Take(runtime->OpenSession(Options(root / "project", enabled, "admitted")), "explicit OpenSession");
    Reply(Turn(session, enabled, "admitted", 0), "已创建,共 2 项,0 项已完成");
    Closed(session, enabled); Take(runtime->Shutdown(), "admission Shutdown");
}
void Replacement(const fs::path& root) {
    auto runtime = Runtime(root); auto state = State({kFirst, kReplaced, kReplaced, kEmpty});
    auto session = Take(runtime->OpenSession(Options(root / "project", state, "replacement")), "replacement OpenSession");
    Reply(Turn(session, state, "replacement", 0), "已创建,共 2 项,0 项已完成");
    Reply(Turn(session, state, "replacement", 1), "已更新 2 项,共 1 项,1 项已完成");
    Reply(Turn(session, state, "replacement", 2), "清单没有变化,共 1 项,1 项已完成");
    Reply(Turn(session, state, "replacement", 3), "已清空,共 0 项,0 项已完成");
    Closed(session, state); Take(runtime->Shutdown(), "replacement Shutdown");
}
void Validation(const fs::path& root) {
    const std::vector<std::pair<std::string, const char*>> bad{
        {R"({"items":[{"content":"new prefix","status":"completed"},{"content":"bad enum","status":"done"}]})", "status 不认得: done"},
        {R"({"items":[{"content":"new prefix","status":"completed"},42]})", "第 2 项格式不对"},
        {R"({"items":[{"content":"","status":"pending"}]})", "content 不能是空字符串"},
        {R"({"items":"wrong shape"})", "缺少必填参数 items"},
        {R"({})", "缺少必填参数 items"},
        {R"({"items":[{"content":"new prefix","status":"completed"},{"content":"missing status"}]})", "第 2 项格式不对"},
    };
    std::vector<std::string> inputs{kFirst};
    for (const auto& item : bad) { inputs.push_back(item.first); inputs.push_back(kFirst); }
    auto runtime = Runtime(root); auto state = State(std::move(inputs));
    auto session = Take(runtime->OpenSession(Options(root / "project", state, "validation")), "validation OpenSession");
    Reply(Turn(session, state, "validation", 0), "已创建,共 2 项,0 项已完成");
    std::size_t turn = 1;
    for (const auto& item : bad) {
        Reply(Turn(session, state, "validation", turn++), item.second, true);
        Reply(Turn(session, state, "validation", turn++), "清单没有变化,共 2 项,0 项已完成");
    }
    Closed(session, state); Take(runtime->Shutdown(), "validation Shutdown");
}
void Isolation(const fs::path& root) {
    auto runtime = Runtime(root); auto other = Runtime(root, "other-data");
    auto rendezvous = std::make_shared<Rendezvous>();
    std::vector<std::shared_ptr<Observed>> states;
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<sdk::Receipt> receipts;
    std::vector<std::string> models;
    for (unsigned i = 0; i != 4; ++i) {
        auto state = State({i % 2 == 0 ? kFirst : kOther, i % 2 == 0 ? kFirst : kOther});
        state->rendezvous = rendezvous; states.push_back(state);
        const auto model = "isolated-" + std::to_string(i); models.push_back(model);
        auto& owner = i == 3 ? other : runtime;
        sessions.push_back(Take(owner->OpenSession(Options(root / (i == 2 ? "other-project" : "project"), state, model)),
            "isolated OpenSession"));
    }
    for (unsigned i = 0; i != 4; ++i) receipts.push_back(Submit(sessions[i], models[i] + "-request-0"));
    for (unsigned i = 0; i != 4; ++i) Reply(Finish(sessions[i], states[i], models[i], 0, receipts[i]), "已创建");
    // One board must not see a peer's previous table; their inputs differ.
    for (unsigned i = 4; i != 0; --i) Reply(Turn(sessions[i - 1], states[i - 1], models[i - 1], 1), "清单没有变化");
    Closed(sessions[0], states[0]);
    Check(states[1]->destroyed == 0 && states[2]->destroyed == 0 && states[3]->destroyed == 0,
        "closing one Session retired a peer backend");
    for (unsigned i = 1; i != 4; ++i) Closed(sessions[i], states[i]);
    Take(runtime->Shutdown(), "isolated Shutdown"); Take(other->Shutdown(), "other Runtime Shutdown");
}
void Recovery(const fs::path& root) {
    auto runtime = Runtime(root); auto seed = State({kFirst, kReplaced});
    auto session = Take(runtime->OpenSession(Options(root / "project", seed, "seed")), "seed OpenSession");
    Reply(Turn(session, seed, "seed", 0), "已创建");
    const auto receipt = Submit(session, "seed-request-1");
    const auto saved_reply = Finish(session, seed, "seed", 1, receipt);
    Reply(saved_reply, "已更新 2 项,共 1 项,1 项已完成");
    const auto id = session->id(); Closed(session, seed);
    auto resumed = State({kReplaced, kReplaced}); resumed->old_reply = saved_reply;
    auto restored = Take(runtime->OpenSession(Options(root / "project", resumed, "restored", true, id)), "same-ID OpenSession");
    Check(restored->id() == id, "restore changed Session identity");
    const auto old = Take(restored->ReadOperation(receipt.operation_id), "restored ReadOperation");
    Check(old.state == sdk::OperationState::Succeeded && old.result_persisted && old.final_text == Final("seed", 1),
        "restore lost the completed operation");
    Reply(Turn(restored, resumed, "restored", 0), "已创建,共 1 项,1 项已完成");
    Reply(Turn(restored, resumed, "restored", 1), "清单没有变化,共 1 项,1 项已完成");
    Check(resumed->saw_old_reply, "restore fabricated a new empty history"); Closed(restored, resumed);
    auto off = State({}); off->old_reply = saved_reply;
    auto unselected = Take(runtime->OpenSession(Options(root / "project", off, "restored-off", false, id)), "unselected resume");
    const auto plain = Submit(unselected, "unselected-resume");
    const auto result = Take(unselected->WaitResult(plain.operation_id, 30s), "unselected resume WaitResult");
    Check(result.state == sdk::OperationState::Succeeded && result.final_text == "todo absent:restored-off" && off->saw_old_reply,
        "resume enabled an unselected tool or lost old history");
    Closed(unselected, off); Take(runtime->Shutdown(), "recovery Shutdown");
}
void Lifetime(const fs::path& root) {
    auto runtime = Runtime(root); auto state = State({kFirst, kOther}); state->block_at = 1;
    auto session = Take(runtime->OpenSession(Options(root / "project", state, "lifetime")), "lifetime OpenSession");
    const auto saved = Turn(session, state, "lifetime", 0); Reply(saved, "已创建");
    const auto pending = Submit(session, "pending-cancel"); WaitBlocked(state);
    Take(session->Cancel(pending.operation_id), "Cancel"); Closed(session, state);
    const auto cancelled = Take(session->ReadOperation(pending.operation_id), "closed cancelled operation");
    Check(cancelled.state == sdk::OperationState::Cancelled && cancelled.result_persisted && state->cancellations == 1,
        "actual cancellation did not finish before Close returned");
    Take(runtime->Shutdown(), "lifetime Shutdown");
    const auto moved = root / "retired-data"; fs::rename(root / "data", moved); fs::remove_all(moved);
    Check(Take(session->ReadOperation(pending.operation_id), "owned closed operation").state == sdk::OperationState::Cancelled,
        "closed query borrowed the retired writer/files");
    Reply(saved, "已创建,共 2 项,0 项已完成");
    session.reset(); runtime.reset();

    auto fresh = Runtime(root); auto dropped = State({kFirst}); dropped->block_at = 0;
    auto final_handle = Take(fresh->OpenSession(Options(root / "project", dropped, "drop")), "drop OpenSession");
    const std::weak_ptr<sdk::Session> weak = final_handle;
    (void)Submit(final_handle, "drop-pending"); WaitBlocked(dropped); final_handle.reset();
    Check(weak.expired() && dropped->destroyed == 1 && dropped->cancellations == 1,
        "last-handle destruction retained a live Session/backend");

    auto blocked = State({kFirst}); blocked->block_at = 0;
    auto active = Take(fresh->OpenSession(Options(root / "project", blocked, "shutdown")), "shutdown OpenSession");
    const auto work = Submit(active, "shutdown-pending"); WaitBlocked(blocked);
    auto peer = State({kFirst});
    auto board = Take(fresh->OpenSession(Options(root / "project", peer, "fresh")), "fresh board OpenSession");
    Reply(Turn(board, peer, "fresh", 0), "已创建,共 2 项,0 项已完成");
    Take(fresh->Shutdown(), "Shutdown pending actual work");
    Check(blocked->destroyed == 1 && peer->destroyed == 1 && blocked->cancellations == 1,
        "Runtime Shutdown retained live execution owners");
    Check(Take(active->ReadOperation(work.operation_id), "shutdown operation").state == sdk::OperationState::Cancelled,
        "Runtime Shutdown lost cancellation");
    Check(!active->Submit("late-shutdown", "must reject") && !board->Submit("late-fresh", "must reject"),
        "Runtime Shutdown left a board open");
}
} // namespace

void TodoWriteCase(const std::string& name, const fs::path& base) {
    Directory directory(base);
    if (name == "admission") Admission(directory.path);
    else if (name == "replacement") Replacement(directory.path);
    else if (name == "validation") Validation(directory.path);
    else if (name == "isolation") Isolation(directory.path);
    else if (name == "recovery") Recovery(directory.path);
    else if (name == "lifetime") Lifetime(directory.path);
    else throw std::runtime_error("unknown todo-write case");
    std::cout << "[sdk-todo-write-path] " << name << '\n';
}
void TodoWrite(const fs::path& base) {
    std::cerr << "[sdk-consumer] begin: TodoWrite" << std::endl;
    for (const char* name : {"admission", "replacement", "validation", "isolation", "recovery", "lifetime"})
        TodoWriteCase(name, base / name);
    std::cout << "[sdk-todo-write-consumer] complete\n";
}
} // namespace lubancore_consumer
