#include <lubancore/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// This installed-package consumer uses only public SDK headers and std.
// Its model is scripted. Search execution, pinned rg, persistence and lifetime
// all belong to the installed SDK, rather than a copied internal runtime.
namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("builtin-search: " + message);
}
template<class T>
T Take(sdk::Result<T> value, const std::string& message) {
    if (!value) throw std::runtime_error("builtin-search: " + message + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const std::string& message) {
    if (!value) throw std::runtime_error("builtin-search: " + message + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path Path(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.is_open(), "cannot write fixture file");
    output << text;
    output.close();
    Check(!output.fail(), "cannot close fixture file");
}
struct Fixture {
    fs::path root;
    explicit Fixture(const fs::path& base) {
        root = base / ("builtin-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "project");
        fs::create_directories(root / "decoy");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
struct CwdGuard {
    fs::path previous = fs::current_path();
    explicit CwdGuard(const fs::path& path) { fs::current_path(path); }
    ~CwdGuard() { std::error_code ignored; fs::current_path(previous, ignored); }
};
struct Rendezvous {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned arrived = 0;
    bool Meet() {
        std::unique_lock lock(mutex);
        ++arrived;
        cv.notify_all();
        return cv.wait_for(lock, 15s, [&] { return arrived == 4; });
    }
};
struct State {
    std::mutex mutex;
    std::vector<std::string> inputs;
    std::vector<sdk::ToolReply> replies;
    std::shared_ptr<Rendezvous> rendezvous;
    std::size_t turn = 0;
    bool pending = false;
    std::atomic<unsigned> models{0};
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        ++state_->models;
        std::unique_lock lock(state_->mutex);
        if (cancellation.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        const auto definition = std::find_if(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "search"; });
        if (definition == request.tools.end() || definition->input_schema_json.find("max_results") == std::string::npos)
            return std::unexpected(sdk::Error{"fixture.search_missing", "public search schema missing"});
        if (!state_->pending) {
            if (state_->turn >= state_->inputs.size()) return std::unexpected(sdk::Error{"fixture.extra_call", "unexpected model call"});
            if (state_->rendezvous && state_->turn == 0 && !state_->rendezvous->Meet())
                return std::unexpected(sdk::Error{"fixture.parallel_timeout", "four sessions did not overlap"});
            state_->pending = true;
            return sdk::ModelReply{"", {{"search-" + std::to_string(state_->turn), "search", state_->inputs[state_->turn]}}, std::nullopt};
        }
        const auto id = "search-" + std::to_string(state_->turn);
        std::optional<sdk::ToolReply> captured;
        for (const auto& message : request.messages) for (const auto& reply : message.tool_replies) if (reply.call_id == id) {
            if (captured) return std::unexpected(sdk::Error{"fixture.duplicate_reply", "duplicate tool result"});
            captured = reply;
        }
        if (!captured) return std::unexpected(sdk::Error{"fixture.result_missing", "actual search reply missing"});
        state_->replies.push_back(std::move(*captured));
        ++state_->turn;
        state_->pending = false;
        return sdk::ModelReply{"installed search complete", {}, std::nullopt};
    }
private:
    std::shared_ptr<State> state_;
};
sdk::SessionOptions Options(const fs::path& cwd, const std::shared_ptr<State>& state) {
    sdk::SessionOptions options;
    options.cwd = Utf8(cwd);
    options.model = "installed-search-fixture";
    options.system_prompt = "Use the installed public search capability.";
    options.backend = std::make_unique<Backend>(state);
    options.builtin_tools = {"search"};
    options.max_steps_per_turn = 4;
    return options;
}
sdk::Receipt Submit(const std::shared_ptr<sdk::Session>& session, unsigned turn) {
    return Take(session->Submit("search-request-" + std::to_string(turn), "search this project"), "submit actual search");
}
std::pair<sdk::Receipt, sdk::ToolReply> Finish(const std::shared_ptr<sdk::Session>& session,
    const std::shared_ptr<State>& state, unsigned turn, const sdk::Receipt& receipt) {
    const auto result = Take(session->WaitResult(receipt.operation_id, 30s), "wait for actual search");
    Check(result.state == sdk::OperationState::Succeeded && result.result_persisted, "search failed: " + result.error);
    Check(result.final_text == "installed search complete", "wrong final assistant reply");
    Check(session->PendingApprovals().empty(), "read-only search requested approval");
    std::lock_guard lock(state->mutex);
    Check(state->models.load() == (turn + 1) * 2 && state->replies.size() == turn + 1, "model/tool round did not complete exactly once");
    return {receipt, state->replies.back()};
}
std::pair<sdk::Receipt, sdk::ToolReply> Turn(const std::shared_ptr<sdk::Session>& session,
    const std::shared_ptr<State>& state, unsigned turn) {
    return Finish(session, state, turn, Submit(session, turn));
}
std::pair<sdk::results::v1::ToolResultIdentity, std::string> Saved(
    const std::shared_ptr<sdk::Session>& session, const std::string& operation) {
    const auto list = Take(session->ListToolResults(operation), "list durable search results");
    const auto selected = std::find_if(list.begin(), list.end(), [](const auto& item) { return item.selected && item.tool_name == "search"; });
    Check(selected != list.end(), "selected search result absent from public query");
    Check(std::count_if(list.begin(), list.end(), [](const auto& item) { return item.selected && item.tool_name == "search"; }) == 1, "duplicate selected result");
    const auto snapshot = Take(session->ReadToolResult(selected->identity), "read durable search body");
    Check(snapshot.result().metadata_state == sdk::results::v1::ArtifactState::Verified, "search metadata was not verified");
    std::string body;
    for (const auto& channel : snapshot.result().channels) if (channel.text) {
        Check(channel.artifact_verified && channel.capture_complete && channel.state == sdk::results::v1::ArtifactState::Verified,
              "search text artifact was incomplete or unverified");
        body += *channel.text;
    }
    Check(!body.empty(), "persistent search text absent");
    return {selected->identity, std::move(body)};
}
} // namespace

void BuiltinSearch(const fs::path& base, const fs::path& resource_root) {
    std::cerr << "[sdk-consumer] begin: BuiltinSearch" << std::endl;
    Check(resource_root.is_absolute(), "installed resource root must be explicit and absolute");
    Fixture fixture(base);
    const auto project = fixture.root / "project";
    Write(project / "root.txt", "ANCHOR own-root\n");
    Write(project / Path("目录/结果🙂.txt"), "中文🙂 ANCHOR nested\n");
    Write(fixture.root / "decoy" / "decoy.txt", "ANCHOR DECOY\nISOLATION_ANCHOR DECOY\n");
    std::string many;
    for (int i = 0; i != 20; ++i) many += "LIMIT_ANCHOR " + std::to_string(i) + "\n";
    Write(project / "many.txt", many);
    CwdGuard cwd(fixture.root / "decoy");
    auto runtime = Take(sdk::Runtime::Create({Utf8(fixture.root / "data"), Utf8(resource_root)}), "create runtime with installed resources");
    auto state = std::make_shared<State>();
    state->inputs = {R"({"mode":"grep","pattern":"ANCHOR","glob":"root.txt"})",
        R"({"mode":"grep","pattern":"own-root","path":null})",
        R"({"mode":"grep","pattern":"own-root","path":""})",
        R"({"mode":"grep","pattern":"中文🙂","path":"目录"})",
        R"({"mode":"glob","pattern":"**/*.txt","path":"目录"})",
        R"({"mode":"grep","pattern":"NO_SUCH_SEARCH_VALUE"})",
        R"({"mode":"glob","pattern":"*.absent"})",
        R"({"mode":"grep","pattern":"LIMIT_ANCHOR","max_results":3})"};
    auto session = Take(runtime->OpenSession(Options(project, state)), "open public builtin-search session");
    for (unsigned i = 0; i != 3; ++i) {
        const auto reply = Turn(session, state, i).second;
        Check(!reply.is_error && reply.text.find("root.txt:1:ANCHOR own-root") != std::string::npos, "default/null/empty path lost project root");
        Check(reply.text.find("DECOY") == std::string::npos, "search used process cwd");
    }
    const auto unicode = Turn(session, state, 3).second;
    Check(!unicode.is_error && unicode.text.find("结果🙂.txt:1:中文🙂 ANCHOR nested") != std::string::npos, "relative Unicode grep lost filename/body");
    const auto glob = Turn(session, state, 4).second;
    Check(!glob.is_error && glob.text == "结果🙂.txt\n", "real installed glob did not find Unicode filename");
    const auto grep_absent = Turn(session, state, 5).second;
    Check(!grep_absent.is_error && grep_absent.text.find("没搜到匹配的内容") != std::string::npos, "grep no-match became failure");
    const auto glob_absent = Turn(session, state, 6).second;
    Check(!glob_absent.is_error && glob_absent.text.find("没找到匹配的文件") != std::string::npos, "glob no-match became failure");
    const auto limited = Turn(session, state, 7);
    Check(!limited.second.is_error && limited.second.text.find("many.txt:3:LIMIT_ANCHOR 2") != std::string::npos &&
        limited.second.text.find("many.txt:4:") == std::string::npos && limited.second.text.find("已截断") != std::string::npos,
        "real search ignored its requested result cap");
    const auto saved = Saved(session, limited.first.operation_id);
    Check(saved.second == limited.second.text, "persisted search differs from actual model tool reply");
    const auto id = session->id();
    Take(session->Close(), "close search session");
    Check(Take(session->ReadToolResult(saved.first), "read search after Close").result().summary.identity == saved.first, "Close changed persisted result identity");
    auto resumed_state = std::make_shared<State>();
    auto resumed_options = Options(project, resumed_state);
    resumed_options.resume_session_id = id;
    auto resumed = Take(runtime->OpenSession(std::move(resumed_options)), "resume same search session");
    Check(Saved(resumed, limited.first.operation_id).second == saved.second && resumed_state->models.load() == 0,
        "resume lost search material or reran model/rg");
    Take(resumed->Close(), "close resumed search session");

    Write(fixture.root / "project-b" / "file.txt", "ISOLATION_ANCHOR PROJECT_B\n");
    Write(fixture.root / "project-c" / "file.txt", "ISOLATION_ANCHOR PROJECT_C\n");
    Write(project / "shared.txt", "ISOLATION_ANCHOR SHARED\n");
    auto rendezvous = std::make_shared<Rendezvous>();
    const std::vector<fs::path> projects{project, project, fixture.root / "project-b", fixture.root / "project-c"};
    const std::vector<std::string> expected{"SHARED", "SHARED", "PROJECT_B", "PROJECT_C"};
    std::vector<std::shared_ptr<State>> states;
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<sdk::Receipt> receipts;
    for (const auto& bound : projects) {
        auto current = std::make_shared<State>();
        current->inputs = {R"({"mode":"grep","pattern":"ISOLATION_ANCHOR"})"};
        current->rendezvous = rendezvous;
        sessions.push_back(Take(runtime->OpenSession(Options(bound, current)), "open independent search session"));
        states.push_back(current);
    }
    for (const auto& current : sessions) receipts.push_back(Submit(current, 0));
    for (unsigned i = 0; i != 4; ++i) {
        const auto result = Finish(sessions[i], states[i], 0, receipts[i]);
        const auto filename = i < 2 ? "shared.txt" : "file.txt";
        const auto expected_reply = std::string(filename) + ":1:ISOLATION_ANCHOR " + expected[i] + "\n";
        Check(!result.second.is_error && result.second.text == expected_reply,
              "overlapping session searched another project or mixed results");
        Check(result.second.text.find("DECOY") == std::string::npos, "overlapping search used host cwd");
    }
    const auto peer_identity = Saved(sessions[0], receipts[0].operation_id).first;
    Check(!sessions[1]->ReadToolResult(peer_identity), "same cwd crossed result ownership");
    for (const auto& current : sessions) Take(current->Close(), "close independent search session");
    Check(fs::equivalent(fs::current_path(), fixture.root / "decoy"), "SDK changed host cwd");
    Take(runtime->Shutdown(), "shutdown installed-search runtime");
    std::cerr << "[sdk-consumer] completed: BuiltinSearch (real installed rg, grep/glob, persistence and four-session isolation)" << std::endl;
}
} // namespace lubancore_consumer
