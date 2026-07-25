// 远端技能库:URL/文件名清洗、来源账本、本地装删与 GitHub 仓库打包。
// HTTP 一律由 lambda 夹具供给,这些用例不出网。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "config/skill_store.hpp"

namespace {

namespace fs = std::filesystem;
using lubancode::config::RemoteSkillRecord;
using lubancode::config::SkillHttpResponse;

class TempSkillStore {
public:
    TempSkillStore() {
        root_ = fs::temp_directory_path() /
                ("lubancode_skill_store_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }

    ~TempSkillStore() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path SkillsRoot() const { return root_ / "skills"; }
    fs::path Root() const { return root_; }

private:
    fs::path root_;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string out;
    file.seekg(0, std::ios::end);
    out.resize(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(out.data(), static_cast<std::streamsize>(out.size()));
    return out;
}

}  // namespace

TEST_CASE("ParseRemoteSkillSource: 裸 Markdown 与 GitHub 仓库") {
    const auto markdown = lubancode::config::ParseRemoteSkillSource("https://example.test/skills/poem.md?raw=1");
    REQUIRE(markdown.has_value());
    CHECK(markdown->kind == lubancode::config::RemoteSkillSourceKind::MarkdownFile);

    const auto repo =
        lubancode::config::ParseRemoteSkillSource("https://github.com/owner/repo/tree/feature/new-skill");
    REQUIRE(repo.has_value());
    CHECK(repo->kind == lubancode::config::RemoteSkillSourceKind::GithubRepository);
    CHECK(repo->owner == "owner");
    CHECK(repo->repository == "repo");
    CHECK(repo->ref == "feature/new-skill");

    CHECK_FALSE(lubancode::config::ParseRemoteSkillSource("https://gitlab.com/owner/repo").has_value());
    CHECK_FALSE(lubancode::config::ParseRemoteSkillSource("https://example.test/../bad.md").has_value());
}

TEST_CASE("SanitizeSkillDirectoryName: 路径穿越与脏名字拒绝") {
    CHECK(lubancode::config::SanitizeSkillDirectoryName("clean-name_2.0").value() == "clean-name_2.0");
    CHECK_FALSE(lubancode::config::SanitizeSkillDirectoryName("../escape").has_value());
    CHECK_FALSE(lubancode::config::SanitizeSkillDirectoryName("dir/name").has_value());
    CHECK_FALSE(lubancode::config::SanitizeSkillDirectoryName("white space").has_value());
}

TEST_CASE("Remote skill metadata: 读写保持 URL 与时间") {
    TempSkillStore temp;
    const std::vector<RemoteSkillRecord> records = {
        {"poem", "https://example.test/poem.md", "2026-07-18T01:02:03Z"},
    };
    REQUIRE(lubancode::config::SaveRemoteSkillRecords(temp.SkillsRoot(), records).has_value());

    const auto loaded = lubancode::config::LoadRemoteSkillRecords(temp.SkillsRoot());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 1);
    CHECK((*loaded)[0].name == "poem");
    CHECK((*loaded)[0].source_url == "https://example.test/poem.md");
    CHECK((*loaded)[0].installed_at == "2026-07-18T01:02:03Z");
}

TEST_CASE("Install, list, update, remove a direct Markdown skill with mocked HTTP") {
    TempSkillStore temp;
    const std::string url = "https://example.test/poem.md";
    const auto first_fetch = [&](const std::string& requested) {
        CHECK(requested == url);
        return SkillHttpResponse{200, "---\nname: poem\ndescription: 写诗\n---\n第一版\n", {}};
    };
    lubancode::config::SkillInstallOptions first_options;
    first_options.installed_at = "2026-07-18T01:02:03Z";
    const auto installed =
        lubancode::config::InstallRemoteSkills(temp.SkillsRoot(), url, first_fetch, first_options);
    REQUIRE(installed.has_value());
    REQUIRE(installed->installed_names == std::vector<std::string>{"poem"});

    const auto listed = lubancode::config::ListStoredSkills(temp.SkillsRoot());
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK((*listed)[0].name == "poem");
    REQUIRE((*listed)[0].source_url.has_value());
    CHECK(*((*listed)[0].source_url) == url);

    const auto second_fetch = [](const std::string&) {
        return SkillHttpResponse{200, "---\nname: poem\ndescription: 写诗\n---\n第二版\n", {}};
    };
    lubancode::config::SkillInstallOptions update_options;
    update_options.overwrite = true;
    update_options.only_names = {"poem"};
    update_options.installed_at = "2026-07-18T02:02:03Z";
    REQUIRE(lubancode::config::InstallRemoteSkills(temp.SkillsRoot(), url, second_fetch, update_options).has_value());
    CHECK(ReadFile(temp.SkillsRoot() / "poem" / "SKILL.md").find("第二版") != std::string::npos);

    REQUIRE(lubancode::config::RemoveStoredSkill(temp.SkillsRoot(), "poem").has_value());
    const auto after_remove = lubancode::config::ListStoredSkills(temp.SkillsRoot());
    REQUIRE(after_remove.has_value());
    CHECK(after_remove->empty());

    const fs::path manual_dir = temp.SkillsRoot() / "manual";
    fs::create_directories(manual_dir);
    std::ofstream(manual_dir / "SKILL.md") << "手工技能\n";
    const auto local = lubancode::config::ListStoredSkills(temp.SkillsRoot());
    REQUIRE(local.has_value());
    REQUIRE(local->size() == 1);
    CHECK_FALSE((*local)[0].source_url.has_value());
}

TEST_CASE("InstallRemoteSkills: GitHub 仓库取 skills 包与根 Markdown") {
    TempSkillStore temp;
    const std::string repo_url = "https://github.com/owner/repo";
    const std::map<std::string, std::string> bodies = {
        {"https://api.github.com/repos/owner/repo", R"({"default_branch":"main"})"},
        {"https://api.github.com/repos/owner/repo/git/trees/main?recursive=1",
         R"({"tree":[{"type":"blob","path":"skills/cpp/SKILL.md"},{"type":"blob","path":"skills/cpp/examples/note.md"},{"type":"blob","path":"README.md"}]})"},
        {"https://raw.githubusercontent.com/owner/repo/main/skills/cpp/SKILL.md", "---\nname: cpp\ndescription: C++\n---\n正文\n"},
        {"https://raw.githubusercontent.com/owner/repo/main/skills/cpp/examples/note.md", "辅助文件\n"},
        {"https://raw.githubusercontent.com/owner/repo/main/README.md", "# repo skill\n"},
    };
    const auto fetch = [&](const std::string& url) {
        const auto it = bodies.find(url);
        return it == bodies.end() ? SkillHttpResponse{404, {}, {}} : SkillHttpResponse{200, it->second, {}};
    };

    const auto installed = lubancode::config::InstallRemoteSkills(temp.SkillsRoot(), repo_url, fetch);
    REQUIRE(installed.has_value());
    CHECK(installed->installed_names == std::vector<std::string>({"README", "cpp"}));
    CHECK(fs::exists(temp.SkillsRoot() / "cpp" / "examples" / "note.md"));
    CHECK(fs::exists(temp.SkillsRoot() / "README" / "SKILL.md"));
}

TEST_CASE("InstallRemoteSkills: 空内容和超限内容不落盘") {
    TempSkillStore temp;
    const std::string url = "https://example.test/big.md";
    const auto empty = [](const std::string&) { return SkillHttpResponse{200, {}, {}}; };
    CHECK_FALSE(lubancode::config::InstallRemoteSkills(temp.SkillsRoot(), url, empty).has_value());

    const std::string too_large(lubancode::config::kMaxRemoteSkillBytes + 1, 'x');
    const auto large = [&](const std::string&) { return SkillHttpResponse{200, too_large, {}}; };
    CHECK_FALSE(lubancode::config::InstallRemoteSkills(temp.SkillsRoot(), url, large).has_value());
    CHECK_FALSE(fs::exists(temp.SkillsRoot() / "big"));
}

TEST_CASE("InstallLocalSkill: 带空格目录整包复制到技能根") {
    TempSkillStore temp;
    const fs::path source = temp.Root() / "source with spaces" / "anysearch";
    fs::create_directories(source / "scripts");
    std::ofstream(source / "SKILL.md", std::ios::binary)
        << "---\nname: anysearch\ndescription: search\n---\n正文\n";
    std::ofstream(source / "scripts" / "search.py", std::ios::binary) << "print('ok')\n";
    std::ofstream(source / ".env", std::ios::binary) << "KEY=test-only\n";

    const auto installed = lubancode::config::InstallSkillSource(
        temp.SkillsRoot(), "\"" + source.string() + "\"",
        [](const std::string&) -> SkillHttpResponse {
            FAIL("本地路径不该发 HTTP 请求");
            return {};
        });

    REQUIRE(installed.has_value());
    CHECK(installed->installed_names == std::vector<std::string>{"anysearch"});
    CHECK(fs::exists(temp.SkillsRoot() / "anysearch" / "SKILL.md"));
    CHECK(fs::exists(temp.SkillsRoot() / "anysearch" / "scripts" / "search.py"));
    CHECK(ReadFile(temp.SkillsRoot() / "anysearch" / ".env") == "KEY=test-only\n");

    const auto listed = lubancode::config::ListStoredSkills(temp.SkillsRoot());
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK_FALSE((*listed)[0].source_url.has_value());
    CHECK_FALSE(lubancode::config::InstallLocalSkill(temp.SkillsRoot(), source.string()).has_value());
}

TEST_CASE("InstallLocalSkill: SKILL.md 路径复制整包，独立 Markdown 改名落盘") {
    TempSkillStore temp;
    const fs::path package = temp.Root() / "local-skill";
    fs::create_directories(package);
    std::ofstream(package / "SKILL.md", std::ios::binary) << "整包正文\n";
    std::ofstream(package / "note.txt", std::ios::binary) << "辅助文件\n";

    const auto package_result =
        lubancode::config::InstallLocalSkill(temp.SkillsRoot(), (package / "SKILL.md").string());
    REQUIRE(package_result.has_value());
    CHECK(fs::exists(temp.SkillsRoot() / "local-skill" / "note.txt"));

    const fs::path standalone = temp.Root() / "single.md";
    std::ofstream(standalone, std::ios::binary) << "独立正文\n";
    const auto standalone_result = lubancode::config::InstallLocalSkill(temp.SkillsRoot(), standalone.string());
    REQUIRE(standalone_result.has_value());
    CHECK(ReadFile(temp.SkillsRoot() / "single" / "SKILL.md") == "独立正文\n");
}

TEST_CASE("InstallLocalSkill: 没有根 SKILL.md 的目录不落盘") {
    TempSkillStore temp;
    const fs::path source = temp.Root() / "broken";
    fs::create_directories(source);
    std::ofstream(source / "README.md") << "不是技能清单\n";

    const auto result = lubancode::config::InstallLocalSkill(temp.SkillsRoot(), source.string());
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(fs::exists(temp.SkillsRoot() / "broken"));
}

TEST_CASE("InstallSkillSource: 带引号 HTTP 地址仍走远端安装") {
    TempSkillStore temp;
    const std::string url = "https://example.test/quoted.md";
    const auto installed = lubancode::config::InstallSkillSource(
        temp.SkillsRoot(), "\"" + url + "\"", [&](const std::string& requested) {
            CHECK(requested == url);
            return SkillHttpResponse{200, "---\nname: quoted\ndescription: test\n---\n正文\n", {}};
        });
    REQUIRE(installed.has_value());
    CHECK(installed->installed_names == std::vector<std::string>{"quoted"});
}
