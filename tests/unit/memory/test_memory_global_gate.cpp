// 存储 v2 P0-4:收紧全局 Memory 的闸与账。
//
// 盖五件事:
//   1. memory_save 工具的 scope=user 直接拒(§6.1 模型不得自行提交全局);
//      EnqueueSave 不带 user_initiated 的 user 层请求也拒——回合尾抽取、
//      候选 accept、Skill/Hook 借道工具的同此一条。
//   2. 用户主动命令(user_initiated=true)且全局配置授权(user_enabled)
//      才进得去;job 落用户层目录。
//   3. 没有用户命令就永不新增全局主题(验收线:两条非命令路全拒)。
//   4. 管理读口:list global 不看召回授权(user_enabled 关着也列得出)。
//   5. CheckGlobalMemoryHealth:旧 projects/ 遗留被点名,failed job 有账。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lmg-" + std::to_string(run_id % 100000) + "-" + name + "-" +
                     std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct Rig {
    fs::path root;
    fs::path repo;
    fs::path home;
    std::shared_ptr<memory::ProjectMemory> store;

    explicit Rig(const std::string& name, bool user_enabled) {
        root = TempRoot(name);
        repo = root / "repo";
        home = root / "home";
        fs::create_directories(repo / ".git");
        fs::create_directories(home);
        memory::Options options;
        options.global_allowed = true;
        options.enabled = true;
        options.user_enabled = user_enabled;
        auto identity = memory::ResolveProjectIdentity(repo, home);
        REQUIRE(identity.has_value());
        store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home, options);
    }
};

memory::SaveRequest MakeGlobalPreference() {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Preference;
    request.id = "preference.lang";
    request.title = "答复语言";
    request.summary = "答复语言";
    request.content = "跨项目偏好:答复用中文。";
    request.keywords = {"答复语言"};
    request.scope.level = "user";
    request.scope.kind = "user";
    request.confidence = "user-stated";
    return request;
}

}  // namespace

TEST_CASE("P0-4: memory_save 工具拒写全局层") {
    Rig rig("tool-gate", /*user_enabled=*/true);
    memory::MemorySaveTool tool(rig.store);
    const auto result = tool.execute(nlohmann::json{
        {"kind", "preference"},
        {"title", "答复语言"},
        {"summary", "答复语言"},
        {"content", "跨项目偏好。"},
        {"scope", nlohmann::json{{"kind", "user"}}},
    });
    REQUIRE(result.is_error);
    CHECK(result.content.find("memory.global_unauthorized") != std::string::npos);
    CHECK(result.content.find("/memory remember global") != std::string::npos);
}

TEST_CASE("P0-4: 非用户命令的 user 层请求一律拒(验收线)") {
    Rig rig("gate", /*user_enabled=*/true);
    // 模型工具路(不带 user_initiated)。
    const auto denied = rig.store->EnqueueSave(MakeGlobalPreference());
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().find("memory.global_unauthorized") != std::string::npos);
    // 全局授权没开:连用户命令也进不去,指路全局配置。
    Rig closed("gate-closed", /*user_enabled=*/false);
    const auto closed_denied = closed.store->EnqueueSave(MakeGlobalPreference(), /*user_initiated=*/true);
    REQUIRE_FALSE(closed_denied.has_value());
    CHECK(closed_denied.error().find("memory.user_enabled") != std::string::npos);
    // 两场都没排进任何 job——没有用户命令+授权,全局层永不新增主题。
    std::error_code ec;
    for (const Rig* rig_ptr : {&rig, &closed}) {
        fs::directory_iterator it(rig_ptr->home / "memory-jobs" / "pending", ec);
        std::size_t pending = 0;
        if (!ec) {
            for (const auto& item : it) ++pending;
        }
        CHECK(pending == 0);
        CHECK_FALSE(fs::exists(rig_ptr->home / "memory" / "user" / "preferences", ec));
    }
}

TEST_CASE("P0-4: 用户命令+全局授权进全局层,worker 写进 memory/user") {
    Rig rig("user-cmd", /*user_enabled=*/true);
    const auto queued = rig.store->EnqueueSave(MakeGlobalPreference(), /*user_initiated=*/true);
    REQUIRE(queued.has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    std::error_code ec;
    CHECK(fs::exists(rig.home / "memory" / "user" / "preferences" / "lang.md", ec));
    // 全局层主题不落 workspace 记忆树。
    CHECK_FALSE(fs::exists(rig.store->memory_dir() / "preferences", ec));
    // 回召(授权开着)能命中用户层。
    const std::string context = rig.store->BuildTurnContext("答复语言怎么选", rig.repo);
    CHECK(context.find("preference.lang") != std::string::npos);
    CHECK(context.find("(用户级记忆)") != std::string::npos);
}

TEST_CASE("P0-4: 管理读口 list global 不看召回授权") {
    Rig rig("mgmt", /*user_enabled=*/true);
    REQUIRE(rig.store->EnqueueSave(MakeGlobalPreference(), /*user_initiated=*/true).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    // 授权关着的另一场(同 home 同 repo):管理读口仍列得出,召回闸不掺和。
    memory::Options closed_options;
    closed_options.global_allowed = true;
    closed_options.enabled = true;
    closed_options.user_enabled = false;
    auto identity = memory::ResolveProjectIdentity(rig.repo, rig.home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory viewer(std::move(*identity), rig.home, closed_options);
    CHECK(viewer.ListUserEntries().empty());  // 召回读口跟着授权关
    const auto managed = viewer.ListGlobalEntriesForManagement();
    REQUIRE(managed.size() == 1);
    CHECK(managed[0].id == "preference.lang");
    // show 管理口也找得到(FindTopic 不再被授权闸拦)。
    const auto topic = viewer.ReadTopicForShow("preference.lang");
    REQUIRE(topic.has_value());
}

TEST_CASE("P0-4: CheckGlobalMemoryHealth 点名旧 projects/ 与 failed job") {
    const fs::path root = TempRoot("health");
    const fs::path home = root / "home";
    fs::create_directories(home);
    Write(home / "projects" / "old-key" / "memory" / "facts" / "x.md", "legacy\n");
    Write(home / "memory-jobs" / "failed" / "17000-1.json", "{\"schema\":1}\n");
    Write(home / "memory-jobs" / "failed" / "17000-1.json.error.txt", "memory.job_failed: x\n");

    const auto lines = memory::CheckGlobalMemoryHealth(home);
    std::string joined;
    for (const std::string& line : lines) joined += line + "\n";
    CHECK(joined.find("projects/") != std::string::npos);
    CHECK(joined.find("失败积压 1") != std::string::npos);
    CHECK(joined.find("尚未创建") != std::string::npos);  // user 目录还没建
}
