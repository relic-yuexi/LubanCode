#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include <nlohmann/json.hpp>

#include "tools/path_utils.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore_consumer {
void SubagentCase(const std::string& name, const std::filesystem::path& base);
void SubagentSeed(const std::filesystem::path& base);
}
namespace {
struct FixtureDirectory {
    std::filesystem::path path;
    FixtureDirectory() {
        static std::atomic<unsigned> counter{0};
        path = std::filesystem::temp_directory_path() / ("sdk-child-assembly-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter));
    }
    ~FixtureDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void Case(const std::string& name) {
    FixtureDirectory directory;
    // Every helper checks actual public model/tool/approval/owned-report facts;
    // it throws on failed admission, timeout, wrong data or unknown scenario.
    CHECK_NOTHROW(lubancore_consumer::SubagentCase(name, directory.path));
}
namespace fs = std::filesystem;
namespace sdk = lubancore;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using namespace std::chrono_literals;
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary); REQUIRE(stream.is_open());
    std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(stream.bad()); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc); REQUIRE(stream.is_open());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); stream.close(); REQUIRE_FALSE(stream.fail());
}
std::string Alter(const std::string& original, const std::function<void(std::vector<Json>&)>& change) {
    std::istringstream input(original); std::string line; std::vector<Json> rows;
    while (std::getline(input, line)) { REQUIRE_FALSE(line.empty()); rows.push_back(Json::parse(line)); }
    REQUIRE_FALSE(rows.empty()); change(rows);
    std::string previous(v3::kGenesisHash), output; std::uint64_t seq = 0;
    for (auto& row : rows) {
        row["seq"] = ++seq;
        if (row.value("kind", std::string()) == "model.request.prepared") {
            row["payload"]["readThroughSeq"] = seq - 1; row["payload"]["readThroughHash"] = previous;
        }
        row.erase("prevHash"); row.erase("lineHash");
        auto canonical = lubancode::trajectory::CanonicalJsonDump(row); REQUIRE(canonical.has_value());
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        row["prevHash"] = previous; row["lineHash"] = hash;
        auto full = lubancode::trajectory::CanonicalJsonDump(row); REQUIRE(full.has_value());
        output += *full + '\n'; previous = hash;
    }
    return output;
}
Json& Row(std::vector<Json>& rows, const std::string& id) {
    for (auto& row : rows) if (row.value("eventId", std::string()) == id || row.value("messageId", std::string()) == id) return row;
    FAIL("actual SDK adoption row is missing"); throw std::runtime_error("actual SDK adoption row is missing");
}
struct NoRerun final : sdk::Backend {
    unsigned& calls;
    explicit NoRerun(unsigned& value) : calls(value) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++calls; return std::unexpected(sdk::Error{"fixture.unexpected_model", "resume must not call a model"});
    }
};
// Only this native wrapper uses the common canonical/hash reader. The relocated
// consumer above remains public-header/std-only. These files come from a real
// public Session child run, not an invented parent observation fixture.
void RehashedAdoptionResume() {
    FixtureDirectory directory;
    lubancore_consumer::SubagentSeed(directory.path);
    const auto root = fs::canonical(directory.path);
    std::ifstream saved(root / "owned-child.state"); std::string sid, op;
    REQUIRE(static_cast<bool>(std::getline(saved, sid))); REQUIRE(static_cast<bool>(std::getline(saved, op)));
    REQUIRE_FALSE(sid.empty()); REQUIRE_FALSE(op.empty());
    fs::path parent;
    for (const auto& entry : fs::recursive_directory_iterator(root / "data"))
        if (entry.path().filename() == "sdk-subagent-plan.json" && entry.path().parent_path().filename() == sid) {
            REQUIRE(parent.empty()); parent = entry.path().parent_path();
        }
    REQUIRE_FALSE(parent.empty());
    auto runtime = sdk::Runtime::Create({lubancode::tools::PathToUtf8(root / "data"), lubancode::tools::PathToUtf8(root / "resources")});
    REQUIRE(runtime.has_value()); unsigned models = 0, effects = 0;
    const auto options = [&] {
        sdk::SessionOptions o; o.cwd = lubancode::tools::PathToUtf8(root / "project"); o.model = "parent-model";
        o.max_steps_per_turn = 20; o.approval_timeout = 10s; o.resume_session_id = sid;
        o.backend = std::make_unique<NoRerun>(models);
        sdk::Tool tool; tool.name = "guarded"; tool.description = "owned child fixture effect"; tool.requires_approval = true;
        tool.execute = [&effects](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            ++effects; return sdk::ToolResult{"unexpected resumed effect", false};
        };
        o.custom_tools.push_back(std::move(tool)); return o;
    };
    auto session = (*runtime)->OpenSession(options()); REQUIRE(session.has_value());
    auto reports = (*session)->GetSubagentReports(op); REQUIRE(reports.has_value()); REQUIRE(reports->size() == 1);
    REQUIRE(reports->front().adoption.has_value()); const auto adopted = *reports->front().adoption;
    REQUIRE((*session)->Close().has_value()); session->reset();
    const auto journal = parent / lubancode::tools::Utf8ToPath(sid + ".jsonl");
    const auto original = Read(journal);
    for (int variant = 0; variant != 3; ++variant) {
        CAPTURE(variant);
        const auto forged = Alter(original, [&](auto& rows) {
            if (variant == 0) Row(rows, adopted.original_tool_message_id)["sessionId"] = "foreign-sdk-parent";
            if (variant == 1) Row(rows, adopted.selected_event_id)["sessionId"] = "foreign-sdk-parent";
            if (variant == 2) Row(rows, adopted.prepared_event_id)["payload"]["contextRevision"] = adopted.prepared_context_revision + 100000;
        });
        Write(journal, forged);
        const auto readable = v3::ReadV3Ledger(journal);
        const auto read_error = readable.has_value() ? std::string() : readable.error();
        REQUIRE_MESSAGE(readable.has_value(), read_error);
        if (variant == 0) {
            const auto* changed = readable->FindMessage(adopted.original_tool_message_id); REQUIRE(changed);
            CHECK(changed->session_id == "foreign-sdk-parent");
        } else {
            const auto* changed = readable->FindEvent(variant == 1 ? adopted.selected_event_id : adopted.prepared_event_id); REQUIRE(changed);
            if (variant == 1) CHECK(changed->session_id == "foreign-sdk-parent");
            else CHECK(changed->payload.at("contextRevision").get<std::uint64_t>() == adopted.prepared_context_revision + 100000);
        }
        auto rejected = (*runtime)->OpenSession(options()); REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == "sdk.subagent.report_invalid");
        CHECK(Read(journal) == forged); CHECK(models == 0); CHECK(effects == 0);
        Write(journal, original);
    }
    auto valid = (*runtime)->OpenSession(options()); REQUIRE(valid.has_value());
    auto report = (*valid)->GetSubagentReports(op); REQUIRE(report.has_value()); REQUIRE(report->size() == 1);
    CHECK(report->front().adoption_state == sdk::subagents::v1::AdoptionState::Validated);
    REQUIRE((*valid)->Close().has_value()); CHECK(models == 0); CHECK(effects == 0);
}
}

TEST_CASE("SDK child: default off admits no dispatch") { Case("default-off"); }
TEST_CASE("SDK child: actual foreground model tool result and checked Close") { Case("success"); }
TEST_CASE("SDK child: parent temporary grant never authorizes child") { Case("parent-grant"); }
TEST_CASE("SDK child: explicit budgets reject missing overflow and widening before execution") { Case("budgets"); }
TEST_CASE("SDK child: reserved missing and colliding tool names reject admission") { Case("reserved-tools"); }
TEST_CASE("SDK child: unsupported modes type isolation and per-call widening open no child") { Case("modes"); }
TEST_CASE("SDK child: Explore intersects true readonly metadata and frozen role names") { Case("explore"); }
TEST_CASE("SDK child: same-ID resume retains frozen plan and historical adoption without rerun") { Case("resume"); }
TEST_CASE("SDK child: plan drift rejects before system transfer or model") { Case("plan-drift"); RehashedAdoptionResume(); }
TEST_CASE("SDK child: positive step cap guards real child model turns") { Case("step-budget"); }
TEST_CASE("SDK child: positive wall cap reaches real child cancellation") { Case("wall-budget"); }
TEST_CASE("SDK child: two shared-project and two other-project children isolate cancel and Close") { Case("four-sessions"); }
