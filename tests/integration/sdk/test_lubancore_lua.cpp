#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore_consumer {
void LuaCase(const std::string&, const std::filesystem::path&);
void LuaSeed(const std::filesystem::path&);
void LuaResume(const std::filesystem::path&);
void LuaAdoptSystem(const std::filesystem::path&, const std::string&);
void LuaRestoreBad(const std::filesystem::path&, const std::string&);
}
namespace {
struct OwnedDirectory {
    std::filesystem::path path;
    OwnedDirectory() {
        static std::atomic<unsigned> counter{0};
        path = std::filesystem::temp_directory_path() / ("sdk-lua-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter));
    }
    ~OwnedDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void Run(const char* name) {
    OwnedDirectory directory;
    CHECK_NOTHROW(lubancore_consumer::LuaCase(name, directory.path));
}
namespace fs = std::filesystem;
namespace trajectory = lubancode::trajectory;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); REQUIRE(in.is_open());
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(in.bad()); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc); REQUIRE(out.is_open());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.close(); REQUIRE_FALSE(out.fail());
}
fs::path Journal(const fs::path& base, const std::string& id) {
    fs::path journal;
    for (const auto& entry : fs::recursive_directory_iterator(base / "data")) {
        if (entry.is_regular_file() && entry.path().filename() == id + ".jsonl" &&
            entry.path().parent_path().filename() == id) {
            REQUIRE(journal.empty()); journal = entry.path();
        }
    }
    REQUIRE_FALSE(journal.empty()); return journal;
}
std::string Rehash(const std::string& bytes, const std::string& message_id, unsigned variant) {
    std::vector<Json> rows; std::istringstream in(bytes); std::string line;
    while (std::getline(in, line)) { REQUIRE_FALSE(line.empty()); rows.push_back(Json::parse(line)); }
    unsigned modified = 0;
    for (auto& row : rows) {
        if (variant == 3) {
            if (modified == 0 && row.value("kind", std::string{}) == "session.started") {
                row["sessionId"] = "foreign-lua-session";
                ++modified;
            }
            continue;
        }
        if (row.value("messageId", std::string{}) != message_id) continue;
        REQUIRE(row.contains("systemMeta"));
        if (variant == 0) row["systemMeta"]["hostBindings"].erase("lua");
        if (variant == 1) row["systemMeta"]["hostBindings"]["lua"]["sha256"] = std::string(64, 'a');
        if (variant == 2) row["sessionId"] = "foreign-lua-session";
        ++modified;
    }
    REQUIRE(modified == 1);
    std::string previous(v3::kGenesisHash), changed;
    for (auto& row : rows) {
        if (row.value("kind", std::string{}) == "model.request.prepared") row["payload"]["readThroughHash"] = previous;
        row.erase("prevHash"); row.erase("lineHash");
        auto canonical = trajectory::CanonicalJsonDump(row); REQUIRE(canonical.has_value());
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        row["prevHash"] = previous; row["lineHash"] = hash;
        auto rendered = trajectory::CanonicalJsonDump(row); REQUIRE(rendered.has_value());
        changed += *rendered + '\n'; previous = hash;
    }
    return changed;
}
}
TEST_CASE("SDK Lua defaults off and its explicit tool reaches the actual model and V3") { Run("off-and-visible"); }
TEST_CASE("SDK Lua owns four VMs across shared and distinct project directories") { Run("four-sessions"); }
TEST_CASE("SDK Lua retains the actual external approval path") { Run("approval"); }
TEST_CASE("SDK Lua cancellation and Close retire actual Session work") { Run("cancel-and-close"); }
TEST_CASE("SDK Lua requires every host execution budget") { Run("invalid-budget"); }
TEST_CASE("SDK Lua rejects missing mismatched duplicate and colliding declarations") { Run("bad-declarations"); }
TEST_CASE("SDK Lua same-ID restore inherits the plan and creates a fresh VM") { Run("resume-fresh-vm"); }
TEST_CASE("SDK Lua drift refusal does not replay earlier model or tool work") { Run("resume-drift"); }
TEST_CASE("SDK Lua validates the actual adopted systems and owned plan before restoring") {
    OwnedDirectory directory;
    REQUIRE_NOTHROW(lubancore_consumer::LuaSeed(directory.path));
    const auto id = Read(directory.path / "session.txt");
    REQUIRE_NOTHROW(lubancore_consumer::LuaAdoptSystem(directory.path, id));
    const auto journal = Journal(directory.path, id), plan = journal.parent_path() / "sdk-lua-plan.json";
    const auto original = Read(journal), saved_plan = Read(plan);
    const auto ledger = v3::ReadV3Ledger(journal);
    REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string{} : ledger.error()));
    REQUIRE(ledger->session_id == id); REQUIRE_FALSE(ledger->messages.empty());
    const auto initial_id = ledger->messages.front().message_id;
    REQUIRE(ledger->messages.front().seq == 1);
    REQUIRE(ledger->messages.front().message.value("role", std::string{}) == "system");
    const auto adopted_id = ledger->context.system_message_ref;
    REQUIRE(adopted_id != initial_id);
    const auto* adopted = ledger->FindMessage(adopted_id);
    REQUIRE(adopted != nullptr); REQUIRE(adopted->session_id == id);
    REQUIRE(adopted->message.value("role", std::string{}) == "system");
    REQUIRE_FALSE(ledger->revision_chains.empty());
    bool referenced = false;
    for (const auto& [revision, chain] : ledger->revision_chains) {
        (void)revision; if (chain.first == adopted_id) referenced = true;
    }
    REQUIRE(referenced);
    bool owned_start = false;
    for (const auto& event : ledger->events) {
        if (event.kind != v3::EventKindV3::SessionStarted) continue;
        REQUIRE(event.session_id == id);
        owned_start = true; break;
    }
    REQUIRE(owned_start);
    for (unsigned variant = 0; variant != 4; ++variant) {
        const auto changed = Rehash(original, variant == 2 ? adopted_id : initial_id, variant);
        Write(journal, changed);
        const auto readable = v3::ReadV3Ledger(journal);
        REQUIRE_MESSAGE(readable.has_value(), (readable ? std::string{} : readable.error()));
        REQUIRE(readable->context.system_message_ref == adopted_id);
        const auto* actual_system = readable->FindMessage(adopted_id);
        REQUIRE(actual_system != nullptr);
        if (variant == 2) REQUIRE(actual_system->session_id == "foreign-lua-session");
        if (variant == 3) {
            bool foreign_start = false;
            for (const auto& event : readable->events) {
                if (event.kind != v3::EventKindV3::SessionStarted) continue;
                REQUIRE(event.session_id == "foreign-lua-session");
                foreign_start = true; break;
            }
            REQUIRE(foreign_start);
        }
        CHECK_NOTHROW(lubancore_consumer::LuaRestoreBad(directory.path, id));
        CHECK(Read(journal) == changed); CHECK(Read(plan) == saved_plan);
    }
    Write(journal, original);
    const auto frozen_profile = Json::parse(saved_plan);
    REQUIRE(frozen_profile.at("protectedCalls") == false);
    for (const bool remove_entry : {false, true}) {
        auto wrong_profile = frozen_profile;
        if (remove_entry) wrong_profile.erase("protectedCalls");
        else wrong_profile["protectedCalls"] = true;
        const auto changed_plan = wrong_profile.dump();
        Write(plan, changed_plan);
        CHECK_NOTHROW(lubancore_consumer::LuaRestoreBad(directory.path, id));
        CHECK(Read(journal) == original);
        CHECK(Read(plan) == changed_plan);
    }
    Write(plan, "{bad plan");
    CHECK_NOTHROW(lubancore_consumer::LuaRestoreBad(directory.path, id)); CHECK(Read(journal) == original);
    fs::remove(plan);
    CHECK_NOTHROW(lubancore_consumer::LuaRestoreBad(directory.path, id)); CHECK(Read(journal) == original);
    Write(plan, saved_plan);
    CHECK_NOTHROW(lubancore_consumer::LuaResume(directory.path));
    std::cout << "[sdk-lua-path] owned-opening\n";
}
