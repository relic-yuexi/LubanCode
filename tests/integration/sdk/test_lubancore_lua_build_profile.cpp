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
void LuaBuildCase(const std::string&, const std::filesystem::path&);
bool LuaBuildWithLua();
std::string LuaBuildSeedDisabled(const std::filesystem::path&);
void LuaBuildRestoreBadBinding(const std::filesystem::path&, const std::string&);
}
namespace {
struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> next{0};
        path = std::filesystem::temp_directory_path() / ("sdk-lua-build-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(next++));
        std::filesystem::create_directories(path);
    }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void BadBinding(const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    namespace trajectory = lubancode::trajectory;
    namespace v3 = lubancode::trajectory::v3;
    using Json = nlohmann::json;
    std::string id;
    REQUIRE_NOTHROW(id = lubancore_consumer::LuaBuildSeedDisabled(base));
    fs::path journal;
    for (const auto& entry : fs::recursive_directory_iterator(base / "data"))
        if (entry.is_regular_file() && entry.path().filename() == id + ".jsonl" &&
            entry.path().parent_path().filename() == id) {
            REQUIRE(journal.empty()); journal = entry.path();
        }
    REQUIRE_FALSE(journal.empty());
    std::ifstream input(journal, std::ios::binary); REQUIRE(input.is_open());
    const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(input.bad()); input.close();
    std::vector<Json> rows; std::istringstream stream(original); std::string line;
    while (std::getline(stream, line)) { REQUIRE_FALSE(line.empty()); rows.push_back(Json::parse(line)); }
    unsigned changed = 0;
    for (auto& row : rows) if (row.contains("systemMeta")) {
        REQUIRE(row["systemMeta"].contains("hostBindings"));
        REQUIRE(row["systemMeta"]["hostBindings"].contains("lua"));
        row["systemMeta"]["hostBindings"]["lua"]["sha256"] = std::string(64, 'a');
        ++changed;
    }
    REQUIRE(changed == 1);
    std::string previous(v3::kGenesisHash), bytes;
    for (auto& row : rows) {
        if (row.value("kind", std::string{}) == "model.request.prepared") row["payload"]["readThroughHash"] = previous;
        row.erase("prevHash"); row.erase("lineHash");
        const auto canonical = trajectory::CanonicalJsonDump(row); REQUIRE(canonical.has_value());
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        row["prevHash"] = previous; row["lineHash"] = hash;
        const auto rendered = trajectory::CanonicalJsonDump(row); REQUIRE(rendered.has_value());
        bytes += *rendered + '\n'; previous = hash;
    }
    std::ofstream output(journal, std::ios::binary | std::ios::trunc); REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.close(); REQUIRE_FALSE(output.fail());
    const auto ledger = v3::ReadV3Ledger(journal);
    const std::string read_error = ledger ? std::string{} : ledger.error();
    REQUIRE_MESSAGE(ledger.has_value(), read_error);
    REQUIRE_FALSE(ledger->messages.empty());
    REQUIRE(ledger->messages.front().system_meta.has_value());
    REQUIRE(ledger->messages.front().system_meta->at("hostBindings").at("lua").at("sha256") == std::string(64, 'a'));
    REQUIRE_NOTHROW(lubancore_consumer::LuaBuildRestoreBadBinding(base, id));
}
void Run(const std::string& name) {
    Directory directory; REQUIRE_NOTHROW(lubancore_consumer::LuaBuildCase(name, directory.path));
    if (name == "frozen-plan") BadBinding(directory.path / "binding");
    std::cout << "[sdk-lua-build-path] " << name << '\n';
    std::cout << "[sdk-lua-build-case-profile] " << name << ' '
              << (lubancore_consumer::LuaBuildWithLua() ? "on" : "off") << '\n';
}
}
TEST_CASE("Lua build profile keeps actual default-off Session and owned plan") { Run("default-off"); }
TEST_CASE("Lua build profile admits or refuses explicit actual execution") { Run("selection"); }
TEST_CASE("Lua build profile restores actual disabled Session by the same ID") { Run("disabled-resume"); }
TEST_CASE("Lua build profile preserves strict frozen-plan refusal and original bytes") { Run("frozen-plan"); }
TEST_CASE("Lua build profile isolates concurrent Session models and identities") { Run("isolation"); }
TEST_CASE("Lua build profile cancels actual work and retires runtime resources") { Run("lifetime"); }
