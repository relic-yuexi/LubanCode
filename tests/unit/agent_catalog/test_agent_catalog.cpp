// AgentCatalog 的单测(自定义 Agent 单阶段 1)。真在临时目录里造三层
// (builtin/user/project)的 agents 目录,验证:内置登记、三层覆盖次序、
// 同层重名、解析失败进 unavailable(不炸整个 Catalog)、名不符 warning、
// 稳定排序(与文件写入次序无关)、夹具样本真文件走一遍。
// 夹具在 tests/fixtures/agent_catalog(本域专用,tests/fixtures/agents 另有
// 人家占着,不碰)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent_catalog.hpp"

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "tests/fixtures"
#endif

using namespace lubancode;

namespace {

// 一处临时三层根,析构清场(同 test_skills.cpp 的 TempSkillsRoot 路数)。
class TempCatalogRoots {
public:
    TempCatalogRoots() {
        base_ = std::filesystem::temp_directory_path() /
               ("lubancode_agent_catalog_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
        std::filesystem::create_directories(base_, ec);
    }
    ~TempCatalogRoots() {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    // 某层的 agents 目录(还没建也返回路径,夹具整目录拷进来的用法用得上)。
    std::filesystem::path Dir(const std::string& layer) const { return base_ / layer / "agents"; }

    // 在 <base>/<layer>/agents/<file> 写一份定义。
    void Write(const std::string& layer, const std::string& file, const std::string& content) const {
        std::error_code ec;
        std::filesystem::create_directories(Dir(layer), ec);
        std::ofstream out(Dir(layer) / file, std::ios::binary);
        out << content;
    }

    agent::AgentCatalogScanRoots Roots(bool with_builtin_dir) const {
        agent::AgentCatalogScanRoots roots;
        roots.user_dir = Dir("user");
        roots.project_dir = Dir("project");
        if (with_builtin_dir) {
            roots.builtin_dir = Dir("builtin");
        }
        return roots;
    }

private:
    std::filesystem::path base_;
};

const agent::AgentCatalogEntry* FindEntry(const agent::AgentCatalog& catalog, const std::string& name) {
    const agent::AgentCatalogEntry* entry = catalog.Find(name);
    REQUIRE(entry != nullptr);
    return entry;
}

}  // namespace

TEST_CASE("内置登记:general-purpose 与 Explore 进 Catalog,可用,行为字段如实") {
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog({});
    const auto* gp = FindEntry(catalog, "general-purpose");
    CHECK(gp->available);
    CHECK(gp->layer == agent::AgentSourceLayer::Builtin);
    CHECK(gp->file == "(builtin)");
    CHECK(gp->shadowed_sources.empty());
    const auto* explore = FindEntry(catalog, "Explore");
    CHECK(explore->available);
    CHECK(explore->definition->tools.allow ==
          std::vector<std::string>{"read_file", "search", "web_fetch", "web_search", "lsp"});
    // 只读不另设权限档(契约 4.9):read_only 不进首版,空 = inherit。
    CHECK(explore->definition->permissions_mode.empty());
    CHECK(agent::ToString(agent::AgentSourceLayer::Project) == "project");
}

TEST_CASE("三层覆盖:同名取 project,被盖住的来源记进覆盖链") {
    TempCatalogRoots tmp;
    tmp.Write("user", "layered.yaml", "schema: 1\nname: layered\ndescription: 用户层\n");
    tmp.Write("project", "layered.yaml", "schema: 1\nname: layered\ndescription: 项目层\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    const auto* entry = FindEntry(catalog, "layered");
    CHECK(entry->layer == agent::AgentSourceLayer::Project);
    CHECK(entry->definition->description == "项目层");
    CHECK(entry->available);
    REQUIRE(entry->shadowed_sources.size() == 1);
    CHECK(entry->shadowed_sources[0].find("user") != std::string::npos);
}

TEST_CASE("三层覆盖:user 盖 builtin 的码内定义,也留账") {
    TempCatalogRoots tmp;
    tmp.Write("user", "general-purpose.yaml",
              "schema: 1\nname: general-purpose\ndescription: 用户自己改口的通用代理。\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    const auto* entry = FindEntry(catalog, "general-purpose");
    CHECK(entry->layer == agent::AgentSourceLayer::User);
    CHECK(entry->definition->description == "用户自己改口的通用代理。");
    REQUIRE(entry->shadowed_sources.size() == 1);
    CHECK(entry->shadowed_sources[0] == "(builtin)");
}

TEST_CASE("高层定义坏了也占住名字:不静默退回低层") {
    TempCatalogRoots tmp;
    tmp.Write("user", "layered.yaml", "schema: 1\nname: layered\ndescription: 好的用户层定义\n");
    tmp.Write("project", "layered.yaml", "schema: 1\nname: layered\ndescription: 坏的\nprompt:\n  soul: on\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    const auto* entry = FindEntry(catalog, "layered");
    CHECK_FALSE(entry->available);
    CHECK_FALSE(entry->definition.has_value());
    CHECK(entry->layer == agent::AgentSourceLayer::Project);
    CHECK_FALSE(entry->FirstError().empty());
    REQUIRE(entry->shadowed_sources.size() == 1);  // 低层那份仍在账上,只是不让位
}

TEST_CASE("同层重名:整组不可用,load_errors 报两处来源;别人照常可用") {
    TempCatalogRoots tmp;
    tmp.Write("project", "dup.yaml", "schema: 1\nname: dup\ndescription: 第一份\n");
    tmp.Write("project", "dup-again.yaml", "schema: 1\nname: dup\ndescription: 第二份\n");
    tmp.Write("project", "fine.yaml", "schema: 1\nname: fine\ndescription: 好的\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    const auto* dup = FindEntry(catalog, "dup");
    CHECK_FALSE(dup->available);
    REQUIRE_FALSE(catalog.load_errors.empty());
    const std::string& error = catalog.load_errors.front();
    CHECK(error.find("重名") != std::string::npos);
    CHECK(error.find("dup.yaml") != std::string::npos);
    CHECK(error.find("dup-again.yaml") != std::string::npos);
    CHECK(FindEntry(catalog, "fine")->available);
    CHECK(catalog.Available().size() == 3);  // fine + 两个内置;dup 不算
}

TEST_CASE("同层重名也管 builtin:磁盘定义想盖码内 general-purpose,整组不可用") {
    TempCatalogRoots tmp;
    tmp.Write("builtin", "general-purpose.yaml",
              "schema: 1\nname: general-purpose\ndescription: 想改内置的胆大之徒。\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(true));
    const auto* entry = FindEntry(catalog, "general-purpose");
    CHECK_FALSE(entry->available);
    REQUIRE_FALSE(catalog.load_errors.empty());
    CHECK(catalog.load_errors.front().find("builtin 层重名") != std::string::npos);
    CHECK(FindEntry(catalog, "Explore")->available);
}

TEST_CASE("解析失败进 unavailable:Catalog 不炸,诊断带行列") {
    TempCatalogRoots tmp;
    tmp.Write("user", "broken.yaml", "schema: 1\nname: broken\ndescription: d\npersona: 多余字段\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    const auto* broken = FindEntry(catalog, "broken");
    CHECK_FALSE(broken->available);
    CHECK_FALSE(broken->definition.has_value());
    CHECK(broken->FirstError().find("未知字段") != std::string::npos);
    CHECK(broken->FirstError().find("broken.yaml") != std::string::npos);
    CHECK(FindEntry(catalog, "general-purpose")->available);
}

TEST_CASE("文件名与 name 不一致:以 name 为准,给 warning 不给 error") {
    TempCatalogRoots tmp;
    tmp.Write("user", "name-mismatch.yaml", "schema: 1\nname: real-name\ndescription: d\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    const auto* entry = FindEntry(catalog, "real-name");
    CHECK(entry->available);
    CHECK(catalog.Find("name-mismatch") == nullptr);  // 名字以 name 为准
    bool has_warning = false;
    for (const auto& issue : entry->issues) {
        if (issue.warning && issue.message.find("不一致") != std::string::npos) {
            has_warning = true;
        }
    }
    CHECK(has_warning);
    CHECK(entry->FirstError().empty());  // 只有 warning 时 FirstError 为空
}

TEST_CASE("稳定排序:写入次序倒着来,结果仍按名字节序") {
    TempCatalogRoots tmp;
    tmp.Write("user", "zeta.yaml", "schema: 1\nname: zeta\ndescription: d\n");
    tmp.Write("user", "alpha.yaml", "schema: 1\nname: alpha\ndescription: d\n");
    tmp.Write("user", "mid.yaml", "schema: 1\nname: mid\ndescription: d\n");
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));
    REQUIRE(catalog.entries.size() == 5);  // Explore, alpha, general-purpose, mid, zeta
    CHECK(catalog.entries[0].name == "Explore");  // 大写字母节序排在小写前
    CHECK(catalog.entries[1].name == "alpha");
    CHECK(catalog.entries[2].name == "general-purpose");
    CHECK(catalog.entries[3].name == "mid");
    CHECK(catalog.entries[4].name == "zeta");
}

TEST_CASE("杂音不收:非 .yaml 文件、不存在的层目录都安静") {
    TempCatalogRoots tmp;
    tmp.Write("user", "notes.txt", "不是定义");
    tmp.Write("user", "real.yaml", "schema: 1\nname: real\ndescription: d\n");
    agent::AgentCatalogScanRoots roots = tmp.Roots(false);
    roots.project_dir = tmp.Dir("project") / "不存在子目录";  // 目录缺席 = 层没有
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(roots);
    CHECK(catalog.Find("real") != nullptr);
    CHECK(catalog.Find("notes") == nullptr);
    CHECK(catalog.load_errors.empty());
}

TEST_CASE("夹具样本真文件走一遍:完整/最小/坏样本/名不符") {
    const std::filesystem::path fixtures =
        std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / "agent_catalog";
    TempCatalogRoots tmp;
    std::error_code ec;
    std::filesystem::create_directories(tmp.Dir("project"), ec);
    // 逐文件拷(copy 整目录会多套一层子目录,Catalog 只扫本层 *.yaml)。
    for (const auto& entry : std::filesystem::directory_iterator(fixtures, ec)) {
        if (entry.is_regular_file()) {
            std::filesystem::copy_file(entry.path(), tmp.Dir("project") / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog(tmp.Roots(false));

    const auto* browser = FindEntry(catalog, "browser-tester");
    CHECK(browser->available);
    REQUIRE(browser->definition.has_value());
    CHECK(browser->definition->prompt.profile == std::optional<std::string>("browser-tester"));
    CHECK(browser->definition->max_steps_per_turn == std::optional<int>(24));
    CHECK(browser->definition->tools.deny == std::vector<std::string>{"shell"});

    const auto* minimal = FindEntry(catalog, "minimal-agent");
    CHECK(minimal->available);

    // 解析失败的条目按文件名 stem 记名(定义没解析成,读不出 name 字段)。
    const auto* unknown_field = FindEntry(catalog, "bad-unknown-field");
    CHECK_FALSE(unknown_field->available);
    CHECK(unknown_field->FirstError().find("未知字段") != std::string::npos);

    const auto* type_error = FindEntry(catalog, "bad-type");
    CHECK_FALSE(type_error->available);
    CHECK(type_error->FirstError().find("runtime.max_steps_per_turn") != std::string::npos);

    const auto* future = FindEntry(catalog, "bad-schema");
    CHECK_FALSE(future->available);
    CHECK(future->FirstError().find("schema") != std::string::npos);

    // name-mismatch.yaml 里 name 是 real-name:挂在 real-name 名下,可用。
    const auto* real = FindEntry(catalog, "real-name");
    CHECK(real->available);
    CHECK(real->layer == agent::AgentSourceLayer::Project);

    // permissions.mode 契约四值各一枚夹具,都可用、值原样落账。
    for (const auto& [fixture, mode] : std::vector<std::pair<const char*, const char*>>{
             {"perm-inherit", "inherit"}, {"perm-confirm", "confirm"}, {"perm-auto", "auto"},
             {"perm-yolo", "yolo"}}) {
        const auto* perm = FindEntry(catalog, fixture);
        CHECK(perm->available);
        REQUIRE(perm->definition.has_value());
        CHECK(perm->definition->permissions_mode == mode);
    }

    // bad-permissions-mode.yaml 写 read_only:不可用,诊断指路 tools.allow。
    const auto* bad_perm = FindEntry(catalog, "bad-permissions-mode");
    CHECK_FALSE(bad_perm->available);
    CHECK(bad_perm->FirstError().find("tools.allow") != std::string::npos);
}
