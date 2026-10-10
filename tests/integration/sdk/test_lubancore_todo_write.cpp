#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace lubancore_consumer {
void TodoWriteCase(const std::string&, const std::filesystem::path&);
}
namespace {
struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> next{0};
        path = std::filesystem::temp_directory_path() / ("sdk-todo-write-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(next++));
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void Run(const std::string& name) {
    Directory directory;
    REQUIRE_NOTHROW(lubancore_consumer::TodoWriteCase(name, directory.path));
}
}
TEST_CASE("SDK todo write preserves default off and explicit main-only admission") { Run("admission"); }
TEST_CASE("SDK todo write replaces the whole board across actual model rounds") { Run("replacement"); }
TEST_CASE("SDK todo write rejects malformed input without partial board mutation") { Run("validation"); }
TEST_CASE("SDK todo write isolates overlapping same-project and other-host Sessions") { Run("isolation"); }
TEST_CASE("SDK todo write restores actual history with a fresh board by the same ID") { Run("recovery"); }
TEST_CASE("SDK todo write closes cancels and destroys actual owners without dangling work") { Run("lifetime"); }
