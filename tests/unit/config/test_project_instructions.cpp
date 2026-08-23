#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "config/project_instructions.hpp"

namespace {

class TempProject {
public:
    TempProject() {
        path = std::filesystem::temp_directory_path() /
               ("lubancode_agents_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path / ".git");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void Write(const std::filesystem::path& relative, const std::string& content) {
        std::filesystem::create_directories((path / relative).parent_path());
        std::ofstream file(path / relative, std::ios::binary);
        file << content;
    }

    std::filesystem::path path;
};

}  // namespace

TEST_CASE("project instructions load root to cwd and prefer override") {
    TempProject project;
    project.Write("AGENTS.md", "root rule");
    project.Write("src/AGENTS.md", "shadowed rule");
    project.Write("src/AGENTS.override.md", "near rule");
    std::filesystem::create_directories(project.path / "src/deep");

    const auto loaded = lubancode::config::LoadProjectInstructions(project.path / "src/deep");
    REQUIRE(loaded.sources.size() == 2);
    CHECK(loaded.content.find("root rule") != std::string::npos);
    CHECK(loaded.content.find("near rule") != std::string::npos);
    CHECK(loaded.content.find("shadowed rule") == std::string::npos);
    CHECK(loaded.content.find("root rule") < loaded.content.find("near rule"));
}

TEST_CASE("project instructions skip empty files and cap content") {
    TempProject project;
    project.Write("AGENTS.md", "   \n");
    project.Write("sub/AGENTS.md", std::string(1000, 'x'));

    const auto loaded = lubancode::config::LoadProjectInstructions(project.path / "sub", 512);
    REQUIRE(loaded.sources.size() == 1);
    CHECK(loaded.truncated);
    CHECK(loaded.content.find("xxx") != std::string::npos);
}

TEST_CASE("empty override falls back to AGENTS.md in the same directory") {
    TempProject project;
    project.Write("AGENTS.override.md", "  \n");
    project.Write("AGENTS.md", "fallback rule");
    const auto loaded = lubancode::config::LoadProjectInstructions(project.path);
    REQUIRE(loaded.sources.size() == 1);
    CHECK(loaded.content.find("fallback rule") != std::string::npos);
}

TEST_CASE("init project instructions creates once and never overwrites") {
    TempProject project;
    project.Write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    const auto created = lubancode::config::InitializeProjectInstructions(project.path / "src");
    CHECK(created.status == lubancode::config::InitProjectInstructionsStatus::Created);
    CHECK(std::filesystem::exists(project.path / "AGENTS.md"));
    const auto loaded = lubancode::config::LoadProjectInstructions(project.path);
    CHECK(loaded.content.find("cmake --build build") != std::string::npos);

    project.Write("AGENTS.md", "keep me");
    const auto existing = lubancode::config::InitializeProjectInstructions(project.path);
    CHECK(existing.status == lubancode::config::InitProjectInstructionsStatus::AlreadyExists);
    CHECK(lubancode::config::LoadProjectInstructions(project.path).content.find("keep me") != std::string::npos);
}
