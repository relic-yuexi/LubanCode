#include <doctest/doctest.h>

#include <filesystem>
#include <string>

namespace lubancore_consumer {
void CommandJobsCase(const std::string&, const std::filesystem::path&, const std::filesystem::path&);
}
namespace {
void Run(const char* path) {
    const auto probe = std::filesystem::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
    REQUIRE(probe.is_absolute());
    REQUIRE(std::filesystem::is_regular_file(probe));
    lubancore_consumer::CommandJobsCase(path, std::filesystem::temp_directory_path(), probe);
}
}

TEST_CASE("SDK Command Jobs: explicit admission and ordinary foreground compatibility") { REQUIRE_NOTHROW(Run("admission")); }
TEST_CASE("SDK Command Jobs: parent success leaves a real Job and bounded restored preview") { REQUIRE_NOTHROW(Run("completion")); }
TEST_CASE("SDK Command Jobs: queued registered budget expires without invoking command") { REQUIRE_NOTHROW(Run("queue")); }
TEST_CASE("SDK Command Jobs: query does not pump while parent backend is blocked") { REQUIRE_NOTHROW(Run("blocked-parent")); }
TEST_CASE("SDK Command Jobs: failed parent cancels its actual command") { REQUIRE_NOTHROW(Run("failed-parent")); }
TEST_CASE("SDK Command Jobs: shared and separate project sessions keep distinct owners") { REQUIRE_NOTHROW(Run("isolation")); }
TEST_CASE("SDK Command Jobs: Runtime shutdown joins its actual command") { REQUIRE_NOTHROW(Run("close")); }
TEST_CASE("SDK Command Jobs: real Prepare ignores parent and sibling approval grants") { REQUIRE_NOTHROW(Run("approvals")); }
TEST_CASE("SDK Command Jobs: live parent cancellation reaches its process") { REQUIRE_NOTHROW(Run("parent-cancel")); }
TEST_CASE("SDK Command Jobs: exhausted parent step budget cancels its Job") { REQUIRE_NOTHROW(Run("step-limit")); }
