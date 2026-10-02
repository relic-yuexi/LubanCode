#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <lubancore/core.hpp>
#include <lubancore/skills.hpp>

#include "tools/path_utils.hpp"
#include "mcp_cwd_fixture.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string Utf8(const fs::path& path) { return lubancode::tools::PathToUtf8(path); }
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.close();
    REQUIRE_FALSE(out.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-skills-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
        Skill("paint", "fixture-paint", "BODY_MARKER");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
    void Skill(const std::string& name, const std::string& description, const std::string& body,
               const std::string& requires_yaml = "", fs::path skills_root = {}) {
        if (skills_root.empty()) skills_root = root / "skills";
        Write(skills_root / name / "SKILL.md", "---\nname: " + name + "\ndescription: " + description +
            "\n" + requires_yaml + "---\n" + body + "\n");
    }
    fs::path Directory(const std::string& id) const {
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        auto directory = lubancode::workspace::index::ResolveDirByWorkspaceKey(root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(directory.has_value());
        return *directory / "sessions" / id;
    }
};
using GenerateFunction = std::function<sdk::Result<sdk::ModelReply>(const sdk::ModelRequest&, sdk::Cancellation)>;
class Backend final : public sdk::Backend {
public:
    explicit Backend(GenerateFunction generate, std::shared_ptr<std::atomic<bool>> alive = {})
        : generate_(std::move(generate)), alive_(std::move(alive)) { if (alive_) *alive_ = true; }
    ~Backend() override { if (alive_) *alive_ = false; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        return generate_(request, cancellation);
    }
private:
    GenerateFunction generate_;
    std::shared_ptr<std::atomic<bool>> alive_;
};
sdk::ModelReply Answer() { return {"skills-answer", {}, sdk::Usage{4, 3}}; }
sdk::ModelReply Call(const Json& input) { return {"", {{"skill-call", "skill", input.dump()}}, sdk::Usage{4, 3}}; }
sdk::SessionOptions Options(const Fixture& fixture, GenerateFunction generate,
                            std::shared_ptr<std::atomic<bool>> alive = {}) {
    sdk::SessionOptions options;
    options.cwd = Utf8(fixture.root / "cwd");
    options.model = "skills-model";
    options.system_prompt = "SKILLS_USER";
    options.backend = std::make_unique<Backend>(std::move(generate), std::move(alive));
    options.skills = sdk::skills::v1::Selection{Utf8(fixture.root / "skills"), {"paint"}};
    options.max_steps_per_turn = 4;
    return options;
}
struct Capture {
    std::atomic<int> calls{0};
    Json input = {{"name", "paint"}};
    std::vector<sdk::ToolReply> replies;
    std::string system;
};
GenerateFunction Invoke(const std::shared_ptr<Capture>& capture) {
    return [capture](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        const auto call = capture->calls++;
        capture->system = request.system;
        if (call % 2 == 0) return Call(capture->input);
        for (auto m = request.messages.rbegin(); m != request.messages.rend(); ++m) {
            if (!m->tool_replies.empty()) {
                capture->replies.push_back(m->tool_replies.back());
                return Answer();
            }
        }
        return std::unexpected(sdk::Error{"fixture.missing_reply", "real SkillTool reply is absent"});
    };
}
sdk::Receipt Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    auto receipt = session->Submit(key, key);
    REQUIRE(receipt.has_value());
    auto result = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(result.has_value());
    INFO(result->error);
    REQUIRE(result->state == sdk::OperationState::Succeeded);
    REQUIRE(result->result_persisted);
    CHECK(result->final_text == "skills-answer");
    return *receipt;
}
sdk::results::v1::ToolResultIdentity CheckResults(const std::shared_ptr<sdk::Session>& session,
                                                const sdk::Receipt& receipt, const std::string& marker, bool exact = false) {
    auto rows = session->ListToolResults(receipt.operation_id);
    REQUIRE(rows.has_value());
    int raw = 0, formal = 0;
    sdk::results::v1::ToolResultIdentity identity;
    std::optional<sdk::results::v1::ToolResultSummary> raw_row, formal_row;
    std::string raw_body, formal_body;
    for (const auto& row : *rows) {
        if (!row.selected || row.tool_name != "skill") continue;
        auto snapshot = session->ReadToolResult(row.identity);
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->result().summary.identity == row.identity);
        CHECK(snapshot->result().metadata_state == sdk::results::v1::ArtifactState::Verified);
        CHECK(row.identity.session_id == session->id());
        CHECK(row.identity.operation_id == receipt.operation_id);
        REQUIRE_FALSE(row.identity.turn_id.empty());
        REQUIRE_FALSE(row.identity.tool_call_id.empty());
        REQUIRE_FALSE(row.identity.persisted_event_id.empty());
        CHECK(row.attempt > 0);
        std::string body;
        for (const auto& channel : snapshot->result().channels) {
            if (!channel.text) continue;
            CHECK(channel.capture_complete);
            CHECK(channel.artifact_verified);
            CHECK(channel.state == sdk::results::v1::ArtifactState::Verified);
            body += *channel.text;
        }
        CHECK((exact ? body == marker : body.find(marker) != std::string::npos));
        if (row.identity.result_id.starts_with("capture-")) { ++raw; raw_row = row; raw_body = body; }
        else if (row.identity.result_id.starts_with("res-")) { ++formal; identity = row.identity; formal_row = row; formal_body = body; }
        else FAIL("Unexpected selected Skills result identity");
    }
    REQUIRE(raw == 1);
    REQUIRE(formal == 1);
    REQUIRE(raw_row.has_value());
    REQUIRE(formal_row.has_value());
    CHECK(raw_row->identity.turn_id == formal_row->identity.turn_id);
    CHECK(raw_row->identity.tool_call_id == formal_row->identity.tool_call_id);
    CHECK(raw_row->attempt == formal_row->attempt);
    CHECK(raw_row->identity.persisted_event_id != formal_row->identity.persisted_event_id);
    CHECK(raw_row->identity.result_id != formal_row->identity.result_id);
    CHECK(raw_body == formal_body);
    auto operation = session->ReadOperation(receipt.operation_id);
    REQUIRE(operation.has_value());
    CHECK(operation->turn_id == identity.turn_id);
    return identity;
}
} // namespace

TEST_CASE("SDK Skills: omitted admission never discovers project materials") {
    Fixture fixture;
    fixture.Skill("paint", "IMPLICIT_DECOY", "IMPLICIT_DECOY", "", fixture.root / "cwd" / ".agents" / "skills");
    fixture.Skill("paint", "IMPLICIT_DECOY", "IMPLICIT_DECOY", "", fixture.root / "cwd" / ".lubancode" / "skills");
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto options = Options(fixture, [](const auto& request, auto) -> sdk::Result<sdk::ModelReply> {
        CHECK(request.system.find("fixture-paint") == std::string::npos);
        CHECK(request.system.find("IMPLICIT_DECOY") == std::string::npos);
        CHECK(std::none_of(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "skill"; }));
        return Answer();
    });
    options.skills.reset();
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto snapshot = (*session)->DescribeSkills();
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->enabled);
    CHECK(snapshot->entries.empty());
    CHECK(snapshot->session_id == (*session)->id());
    Turn(*session, "disabled");
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: frozen public values and real body results survive Close") {
    Fixture fixture;
    fixture.Skill("paint", "IMPLICIT_DECOY", "IMPLICIT_DECOY", "", fixture.root / "cwd" / ".agents" / "skills");
    fixture.Skill("paint", "IMPLICIT_DECOY", "IMPLICIT_DECOY", "", fixture.root / "cwd" / ".lubancode" / "skills");
    auto capture = std::make_shared<Capture>();
    auto alive = std::make_shared<std::atomic<bool>>(false);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, Invoke(capture), alive));
    REQUIRE(session.has_value());
    auto snapshot = (*session)->DescribeSkills();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->entries.size() == 1);
    CHECK(snapshot->entries[0].name == "paint");
    CHECK(snapshot->entries[0].content_sha256.size() == 64);
    CHECK(snapshot->plan_sha256.size() == 64);
    CHECK(snapshot->tool_names == std::vector<std::string>{"skill"});
    CHECK(capture->calls.load() == 0);
    const auto expected_body = "技能目录: " + snapshot->entries[0].directory +
        "(技能内相对路径以此为基准)\nBODY_MARKER\n";
    auto identity = CheckResults(*session, Turn(*session, "body"), expected_body, true);
    REQUIRE(capture->replies.size() == 1);
    CHECK_FALSE(capture->replies[0].is_error);
    CHECK(capture->system.find("SKILLS_USER") == 0);
    CHECK(capture->system.find(snapshot->prompt_segment) != std::string::npos);
    REQUIRE((*session)->Close().has_value());
    CHECK_FALSE(alive->load());
    auto closed = (*session)->DescribeSkills();
    REQUIRE(closed.has_value());
    REQUIRE(closed->entries.size() == 1);
    CHECK(closed->plan_sha256 == snapshot->plan_sha256);
    REQUIRE((*session)->ReadToolResult(identity).has_value());
    CHECK(capture->calls.load() == 2);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: dependency declaration uses actual tool surface without granting tools") {
    Fixture fixture;
    fixture.Skill("paint", "fixture-paint", "BODY_MARKER", "requires-tools: [explicit, run_command]\n");
    auto capture = std::make_shared<Capture>();
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto options = Options(fixture, Invoke(capture));
    sdk::Tool custom;
    custom.name = "explicit";
    custom.description = "Only explicitly supplied";
    custom.execute = [](const auto&, const auto&) -> sdk::Result<sdk::ToolResult> { FAIL("dependency lookup executed a tool"); return {}; };
    options.custom_tools.push_back(std::move(custom));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto snapshot = (*session)->DescribeSkills();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->entries.size() == 1);
    CHECK(snapshot->entries[0].missing_tools == std::vector<std::string>{"run_command"});
    CHECK(snapshot->tool_names == std::vector<std::string>{"explicit", "skill"});
    Turn(*session, "missing-dependency");
    REQUIRE(capture->replies.size() == 1);
    CHECK(capture->replies[0].is_error);
    CHECK(capture->replies[0].text.find("capability_unavailable") != std::string::npos);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: malformed selected sources fail before MCP and model startup") {
    Fixture fixture;
    SUBCASE("missing") { fs::remove(fixture.root / "skills" / "paint" / "SKILL.md"); }
    SUBCASE("duplicate name") { Write(fixture.root / "skills" / "paint" / "SKILL.md", "---\nname: paint\nname: paint\ndescription: fixture\n---\nbody"); }
    SUBCASE("bad yaml") { Write(fixture.root / "skills" / "paint" / "SKILL.md", "---\nname: [\ndescription: fixture\n---\nbody"); }
    SUBCASE("oversized actual body") { Write(fixture.root / "skills" / "paint" / "SKILL.md", std::string(1024 * 1024 + 1, 'x')); }
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto alive = std::make_shared<std::atomic<bool>>(false);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto options = Options(fixture, [calls](const auto&, auto) -> sdk::Result<sdk::ModelReply> { ++*calls; return Answer(); }, alive);
    sdk::McpServer mcp;
    mcp.name = "never";
    mcp.command = Utf8(fixture.root / "missing-mcp-program");
    options.mcp_servers.push_back(std::move(mcp));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().code.starts_with("sdk.skill."));
    CHECK(calls->load() == 0);
    CHECK_FALSE(alive->load());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: admitted MCP wire names satisfy dependencies after real startup") {
    Fixture fixture;
    lubancode::test_support::McpCwdFixture mcp_fixture;
    fixture.Skill("paint", "fixture-paint", "BODY_MARKER", "requires-tools: [mcp__location__where]\n");
    auto capture = std::make_shared<Capture>();
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto options = Options(fixture, Invoke(capture));
    sdk::McpServer mcp;
    mcp.name = "location";
    mcp.command = mcp_fixture.python;
    mcp.arguments = {mcp_fixture.Script()};
    mcp.tools = {"where"};
    for (const char* key : {"SystemRoot", "PATH", "TEMP", "TMP"})
        if (auto value = lubancode::platform::GetEnvVar(key)) mcp.environment.emplace_back(key, *value);
    options.mcp_servers.push_back(std::move(mcp));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto snapshot = (*session)->DescribeSkills();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->entries.size() == 1);
    CHECK(snapshot->entries[0].missing_tools.empty());
    CHECK(snapshot->tool_names == std::vector<std::string>{"mcp__location__where", "skill"});
    CheckResults(*session, Turn(*session, "mcp-dependency"), "BODY_MARKER");
    REQUIRE(capture->replies.size() == 1);
    CHECK_FALSE(capture->replies[0].is_error);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: attachments read current bytes while body and aliases enforce the fingerprint") {
    Fixture fixture;
    Write(fixture.root / "skills" / "paint" / "references" / "live.txt", "BEFORE");
    auto capture = std::make_shared<Capture>();
    capture->input = {{"name", "paint"}, {"path", "references/live.txt"}};
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, Invoke(capture)));
    REQUIRE(session.has_value());
    Write(fixture.root / "skills" / "paint" / "references" / "live.txt", "LIVE_NOW");
    CheckResults(*session, Turn(*session, "live"), "LIVE_NOW");
    REQUIRE(capture->replies.size() == 1);
    CHECK_FALSE(capture->replies.back().is_error);
    fixture.Skill("paint", "fixture-paint", "CHANGED_BODY");
    capture->input = {{"name", "paint"}};
    Turn(*session, "drift");
    CHECK(capture->replies.back().is_error);
    CHECK(capture->replies.back().text.find("skill.drifted") != std::string::npos);
    // Hard links exercise identity even when canonical paths differ, on every OS.
    fs::create_hard_link(fixture.root / "skills" / "paint" / "SKILL.md", fixture.root / "skills" / "paint" / "alias.md");
    for (const auto& path : {"SKILL.md", "alias.md"}) {
        capture->input = {{"name", "paint"}, {"path", path}};
        Turn(*session, std::string("alias-") + path);
        CHECK(capture->replies.back().is_error);
        CHECK(capture->replies.back().text.find("skill.path.body_alias") != std::string::npos);
    }
    capture->input = {{"name", "paint"}, {"path", "references/live.txt"}};
    Turn(*session, "live-after-drift");
    CHECK_FALSE(capture->replies.back().is_error);
    CHECK(capture->replies.back().text.find("LIVE_NOW") != std::string::npos);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: same-ID resume rejects one-sided or changed plans without writing the old journal") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto first = (*runtime)->OpenSession(Options(fixture, [](const auto&, auto) -> sdk::Result<sdk::ModelReply> { return Answer(); }));
    REQUIRE(first.has_value());
    const auto id = (*first)->id();
    REQUIRE((*first)->Close().has_value());
    const auto directory = fixture.Directory(id);
    const auto plan_path = directory / "sdk-skills-plan.json";
    const auto original_plan = Read(plan_path);
    const auto journal_path = directory / (id + ".jsonl");
    const auto original_journal = Read(journal_path);
    bool omit_selection = false;
    SUBCASE("missing") { fs::remove(plan_path); }
    SUBCASE("corrupt") { Write(plan_path, "{broken"); }
    SUBCASE("same schema changed digest") {
        auto plan = Json::parse(original_plan);
        plan["promptSegment"] = "altered";
        Write(plan_path, plan.dump());
    }
    SUBCASE("changed source") { fixture.Skill("paint", "fixture-paint", "DRIFT"); }
    SUBCASE("omitted selection") { omit_selection = true; }
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, [calls](const auto&, auto) -> sdk::Result<sdk::ModelReply> { ++*calls; return Answer(); });
    options.resume_session_id = id;
    if (omit_selection) options.skills.reset();
    sdk::McpServer mcp;
    mcp.name = "never";
    mcp.command = Utf8(fixture.root / "missing-mcp-program");
    options.mcp_servers.push_back(std::move(mcp));
    auto rejected = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code.starts_with("sdk.skill."));
    CHECK(calls->load() == 0);
    CHECK(Read(journal_path) == original_journal);
    Write(plan_path, original_plan);
    fixture.Skill("paint", "fixture-paint", "BODY_MARKER");
    auto valid = Options(fixture, [](const auto&, auto) -> sdk::Result<sdk::ModelReply> { return Answer(); });
    valid.resume_session_id = id;
    valid.system_prompt.clear();
    auto resumed = (*runtime)->OpenSession(std::move(valid));
    REQUIRE(resumed.has_value());
    CHECK((*resumed)->id() == id);
    Turn(*resumed, "after-rejection");
    REQUIRE((*runtime)->Shutdown().has_value());
}


TEST_CASE("SDK Skills: frozen tool-surface mismatch cannot silently change a resumed plan") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto first = (*runtime)->OpenSession(Options(fixture, [](const auto&, auto) -> sdk::Result<sdk::ModelReply> { return Answer(); }));
    REQUIRE(first.has_value());
    const auto id = (*first)->id();
    REQUIRE((*first)->Close().has_value());
    const auto journal = fixture.Directory(id) / (id + ".jsonl");
    const auto before = Read(journal);
    auto options = Options(fixture, [](const auto&, auto) -> sdk::Result<sdk::ModelReply> { return Answer(); });
    options.resume_session_id = id;
    options.builtin_tools = {"read_file"};
    auto rejected = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == "sdk.skill.resume_mismatch");
    CHECK(Read(journal) == before);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Skills: legacy malformed effective root fails before MCP startup") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    // Establish the owned workspace index without changing ambient process roots.
    auto seed = Options(fixture, [](const auto&, auto) -> sdk::Result<sdk::ModelReply> { return Answer(); });
    seed.skills.reset();
    auto first = (*runtime)->OpenSession(std::move(seed));
    REQUIRE(first.has_value());
    REQUIRE((*first)->Close().has_value());
    const std::string id = "legacy-invalid-skills-root";
    const auto directory = fixture.Directory((*first)->id()).parent_path() / id;
    fs::create_directories(directory);
    Json version = 0;
    SUBCASE("zero") { version = 0; }
    SUBCASE("negative") { version = -1; }
    SUBCASE("text") { version = "bad"; }
    SUBCASE("floating") { version = 1.5; }
    auto writer = v3::V3Writer::Start(directory / (id + ".jsonl"), id, "run-invalid", "legacy root", {{"settingsVersion", version}});
    REQUIRE(writer.has_value());
    REQUIRE(writer->Close().has_value());
    const auto before = Read(directory / (id + ".jsonl"));
    auto options = Options(fixture, [](const auto&, auto) -> sdk::Result<sdk::ModelReply> { FAIL("invalid root started model"); return Answer(); });
    options.skills.reset();
    options.resume_session_id = id;
    sdk::McpServer mcp;
    mcp.name = "never";
    mcp.command = Utf8(fixture.root / "missing-mcp-program");
    options.mcp_servers.push_back(std::move(mcp));
    auto rejected = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == "sdk.skill.plan_invalid");
    CHECK(Read(directory / (id + ".jsonl")) == before);
    REQUIRE((*runtime)->Shutdown().has_value());
}

namespace {
struct Overlap {
    std::mutex mutex;
    std::condition_variable cv;
    std::array<bool, 4> entered{};
    std::array<bool, 4> cancelled{};
    bool release = false;
};
struct ReleaseGuard {
    std::shared_ptr<Overlap> gate;
    ~ReleaseGuard() {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
        gate->cv.notify_all();
    }
};
} // namespace

TEST_CASE("SDK Skills: four overlapping sessions isolate shared projects, models, cancellation and Close") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto gate = std::make_shared<Overlap>();
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<std::shared_ptr<std::atomic<bool>>> alive;
    std::vector<sdk::Receipt> receipts;
    // Declared after handles: assertion unwinding releases backends before Close joins.
    ReleaseGuard cleanup{gate};
    const auto ambient = fs::current_path();
    for (int i = 0; i < 4; ++i) {
        auto capture = std::make_shared<Capture>();
        auto owner = std::make_shared<std::atomic<bool>>(false);
        const auto label = "label-" + std::to_string(i);
        const auto skills_root = fixture.root / ("skills-" + std::to_string(i));
        fixture.Skill("paint", label, label, "", skills_root);
        const auto expected_body = "技能目录: " + Utf8(fs::canonical(skills_root / "paint")) +
            "(技能内相对路径以此为基准)\n" + label + "\n";
        auto options = Options(fixture, [gate, capture, i, label, expected_body](const sdk::ModelRequest& request, sdk::Cancellation cancel)
            -> sdk::Result<sdk::ModelReply> {
            CHECK(request.model == "model-" + std::to_string(i));
            CHECK(request.system.starts_with("USER_" + label));
            CHECK(request.system.find("paint: " + label) != std::string::npos);
            for (int other = 0; other < 4; ++other)
                if (other != i) CHECK(request.system.find("paint: label-" + std::to_string(other)) == std::string::npos);
            if (capture->calls++ == 0) {
                std::unique_lock lock(gate->mutex);
                gate->entered[i] = true;
                gate->cv.notify_all();
                while (!gate->release && !cancel.requested()) gate->cv.wait_for(lock, 10ms);
                if (cancel.requested()) {
                    gate->cancelled[i] = true;
                    return std::unexpected(sdk::Error{"fixture.cancelled", label});
                }
                return Call({{"name", "paint"}});
            }
            bool found = false;
            for (const auto& message : request.messages) for (const auto& reply : message.tool_replies)
                found |= !reply.is_error && reply.text == expected_body;
            CHECK(found);
            return Answer();
        }, owner);
        options.model = "model-" + std::to_string(i);
        options.system_prompt = "USER_" + label;
        options.skills = sdk::skills::v1::Selection{Utf8(skills_root), {"paint"}};
        if (i > 1) {
            const auto cwd = fixture.root / ("project-" + std::to_string(i));
            fs::create_directories(cwd);
            options.cwd = Utf8(cwd);
        }
        auto session = (*runtime)->OpenSession(std::move(options));
        REQUIRE(session.has_value());
        sessions.push_back(*session);
        alive.push_back(owner);
        auto receipt = sessions.back()->Submit("same-client-key", label);
        REQUIRE(receipt.has_value());
        receipts.push_back(*receipt);
    }
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 20s, [&] { return std::all_of(gate->entered.begin(), gate->entered.end(), [](bool x) { return x; }); }));
    }
    REQUIRE(sessions[0]->Cancel(receipts[0].operation_id).has_value());
    REQUIRE(sessions[1]->Close().has_value());
    CHECK_FALSE(alive[1]->load());
    auto closed = sessions[1]->DescribeSkills();
    REQUIRE(closed.has_value());
    REQUIRE(closed->entries.size() == 1);
    CHECK(closed->entries[0].description == "label-1");
    CHECK(alive[2]->load());
    CHECK(alive[3]->load());
    {
        std::lock_guard lock(gate->mutex);
        CHECK(gate->cancelled[1]);
        CHECK_FALSE(gate->cancelled[2]);
        CHECK_FALSE(gate->cancelled[3]);
        gate->release = true;
        gate->cv.notify_all();
    }
    for (int i = 2; i < 4; ++i) {
        auto result = sessions[i]->WaitResult(receipts[i].operation_id, 30s);
        REQUIRE(result.has_value());
        CHECK(result->state == sdk::OperationState::Succeeded);
        const auto expected_body = "技能目录: " + Utf8(fs::canonical(fixture.root / ("skills-" + std::to_string(i)) / "paint")) +
            "(技能内相对路径以此为基准)\nlabel-" + std::to_string(i) + "\n";
        auto identity = CheckResults(sessions[i], receipts[i], expected_body, true);
        CHECK_FALSE(sessions[i == 2 ? 3 : 2]->ReadToolResult(identity).has_value());
    }
    auto cancelled = sessions[0]->WaitResult(receipts[0].operation_id, 30s);
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->state == sdk::OperationState::Cancelled);
    CHECK(fs::current_path() == ambient);
    REQUIRE((*runtime)->Shutdown().has_value());
    for (const auto& owner : alive) CHECK_FALSE(owner->load());
}

