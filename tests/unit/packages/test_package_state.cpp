// 统一 Package 封装单阶段 6 的启停册:PackageStateStore(包外账本
// ~/.lubancode/package-state.json 的落盘形状:包 id、enabled、改动的时刻;
// 原子写照信任账惯例;坏 JSON 容错读按全启用续)、/package enable|disable
// 的账务回执(如实说"下回启动生效",不拆在跑的)、挂载启停门(停用的包
// 扫描发现照旧、挂载一律跳过连内容组件一件不挂)。测试账对齐单子 §12.2
// "启停账在包外"与阶段 6 清单前两条。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "app/commands/package_commands.hpp"  // ParsePackageCommand(纯函数)
#include "package/inventory.hpp"
#include "package/mounting.hpp"
#include "package/state.hpp"
#include "tools/skill_loader.hpp"

using namespace lubancode::package;
using lubancode::app::ParsePackageCommand;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_pkg_state_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 一只最小 content-only 包:清单 + 一件 Skill(引用闭合,valid)。
fs::path MakeContentPackage(const fs::path& parent, const std::string& dir,
                            const std::string& id = "test.content-kit") {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: 0.1.0\nname: Content Kit\n"
              "description: 内容包测试件。\n");
    WriteFile(root / "skills" / "checklist" / "SKILL.md",
              "---\nname: checklist\ndescription: 核对章法。\n---\n照单核。");
    return root;
}

PackageMount MountDevDir(const fs::path& layer_root, const PackageStateSnapshot& state = {}) {
    PackageMountInput input;
    input.scan.dev_roots.push_back(layer_root);
    input.state = state;
    return BuildPackageMount(input);
}

}  // namespace

// ---------------------------------------------------------------------------
// 账本
// ---------------------------------------------------------------------------

TEST_CASE("PackageStateStore:缺文件 = 空账全启用;SetEnabled 落账可回读") {
    TempDir temp;
    const fs::path store_path = temp.Get() / "package-state.json";

    auto [fresh, fresh_error] = PackageStateStore::Load(store_path.string());
    REQUIRE(fresh_error == std::nullopt);
    CHECK(fresh.IsEnabled("any.pkg") == true);  // 账上没有 = 启用(缺省态)
    CHECK(fresh.entries().empty());

    REQUIRE(fresh.SetEnabled("test.content-kit", "0.1.0", "dev", /*enabled=*/false));
    REQUIRE_FALSE(fresh.IsEnabled("test.content-kit"));
    REQUIRE(fresh.Save() == std::nullopt);
    CHECK(fs::exists(store_path));
    // 原子写:换名后不留 .tmp 残留。
    CHECK_FALSE(fs::exists(fs::path(store_path) += ".tmp"));

    // 落盘形状:v1 账本只记改过启停的包;enabled/version/scope/changed_at 齐。
    const std::string saved = ReadFile(store_path);
    CHECK(saved.find("\"schema_version\": 1") != std::string::npos);
    CHECK(saved.find("\"test.content-kit\"") != std::string::npos);
    CHECK(saved.find("\"enabled\": false") != std::string::npos);
    CHECK(saved.find("\"changed_at\"") != std::string::npos);

    // 回读:同一枚停用账。
    auto [reloaded, reload_error] = PackageStateStore::Load(store_path.string());
    REQUIRE(reload_error == std::nullopt);
    REQUIRE_FALSE(reloaded.IsEnabled("test.content-kit"));
    CHECK(reloaded.IsEnabled("other.pkg"));
    const std::optional<PackageStateEntry> entry = reloaded.Find("test.content-kit");
    REQUIRE(entry.has_value());
    CHECK(entry->enabled == false);
    CHECK(entry->version == "0.1.0");
    CHECK(entry->scope == "dev");
    CHECK_FALSE(entry->changed_at_unix.empty());

    // 复启:改回启用,时刻跟着走。
    REQUIRE(reloaded.SetEnabled("test.content-kit", "0.1.0", "dev", /*enabled=*/true));
    CHECK(reloaded.IsEnabled("test.content-kit"));
}

TEST_CASE("PackageStateStore:幂等——同态重记不动账,时刻不漂") {
    TempDir temp;
    const fs::path store_path = temp.Get() / "package-state.json";
    auto [store, error] = PackageStateStore::Load(store_path.string());
    REQUIRE(error == std::nullopt);
    REQUIRE(store.SetEnabled("test.content-kit", "0.1.0", "dev", false));
    const std::optional<PackageStateEntry> first = store.Find("test.content-kit");
    REQUIRE(first.has_value());
    // 已停用再 disable:返回 false,changed_at 一字不动。
    CHECK_FALSE(store.SetEnabled("test.content-kit", "0.1.0", "dev", false));
    const std::optional<PackageStateEntry> again = store.Find("test.content-kit");
    REQUIRE(again.has_value());
    CHECK(again->changed_at_unix == first->changed_at_unix);
}

TEST_CASE("PackageStateStore:坏 JSON 警告 + 按全启用续(不崩会话)") {
    TempDir temp;
    const fs::path store_path = temp.Get() / "package-state.json";
    WriteFile(store_path, "{ not json at all");
    auto [store, error] = PackageStateStore::Load(store_path.string());
    REQUIRE(error.has_value());
    CHECK(error->find("读不动") != std::string::npos);
    CHECK(error->find("全启用") != std::string::npos);
    CHECK(store.IsEnabled("test.content-kit"));  // 缺省启用续跑
}

// ---------------------------------------------------------------------------
// 账务回执
// ---------------------------------------------------------------------------

TEST_CASE("EnableDisablePackage:disable 回执如实说下回启动生效、不拆在跑的") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");
    const ScanOptions scan = [&] {
        ScanOptions options;
        options.dev_roots.push_back(dev_layer);
        return options;
    }();
    const std::vector<PackageCandidate> candidates = ScanPackages(scan);
    REQUIRE(candidates.size() == 1);
    const PackageInventory inventory = BuildPackageInventory(candidates[0], scan);

    auto [store, error] = PackageStateStore::Load(std::nullopt);  // 纯内存模式
    REQUIRE(error == std::nullopt);

    SUBCASE("disable:生效时机 + 会话快照仍挂着的如实说") {
        const PackageStateActionResult result =
            EnableDisablePackage(inventory, &store, /*enable=*/false, /*session_mounted_count=*/2);
        REQUIRE(result.ok);
        const std::string joined = [&] {
            std::string out;
            for (const auto& line : result.lines) out += line + "\n";
            return out;
        }();
        CHECK(joined.find("已停用 test.content-kit 0.1.0(dev 层)") != std::string::npos);
        CHECK(joined.find("下回启动") != std::string::npos);
        CHECK(joined.find("/package reload") != std::string::npos);
        CHECK(joined.find("连内容组件一件不挂") != std::string::npos);
        CHECK(joined.find("在跑的 Agent/Workflow 不拆") != std::string::npos);
        CHECK(joined.find("2 件内容组件这场照旧") != std::string::npos);
        CHECK(joined.find("/package enable test.content-kit") != std::string::npos);
    }
    SUBCASE("enable:回执同样说下回装配生效") {
        REQUIRE(EnableDisablePackage(inventory, &store, false, 0).ok);
        const PackageStateActionResult result =
            EnableDisablePackage(inventory, &store, /*enable=*/true, 0);
        REQUIRE(result.ok);
        const std::string joined = [&] {
            std::string out;
            for (const auto& line : result.lines) out += line + "\n";
            return out;
        }();
        CHECK(joined.find("已启用 test.content-kit") != std::string::npos);
        CHECK(joined.find("下回启动") != std::string::npos);
    }
    SUBCASE("幂等:本来停着再说一遍,账未动") {
        REQUIRE(EnableDisablePackage(inventory, &store, false).ok);
        const PackageStateActionResult result = EnableDisablePackage(inventory, &store, false);
        REQUIRE(result.ok);
        CHECK(result.lines[0].find("本来就停着,账未动") != std::string::npos);
    }
    SUBCASE("store 为空(找不到主目录):明拒") {
        const PackageStateActionResult result = EnableDisablePackage(inventory, nullptr, false);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("启停账不可用") != std::string::npos);
    }
    SUBCASE("清单读不出身份:指路 doctor") {
        // 造一份 manifest_ok=false 的账(disable 坏包是合法诉求,但身份
        // 出不来就记不了账——回执指路 doctor)。
        PackageInventory broken = inventory;
        broken.manifest_ok = false;
        const PackageStateActionResult result = EnableDisablePackage(broken, &store, false);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("/package doctor") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 挂载启停门
// ---------------------------------------------------------------------------

TEST_CASE("启停门:disabled 包挂载跳过(连内容组件),扫描发现照旧") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    // 扫描发现照旧:候选还在,启停不藏包。
    const ScanOptions scan = [&] {
        ScanOptions options;
        options.dev_roots.push_back(dev_layer);
        return options;
    }();
    REQUIRE(ScanPackages(scan).size() == 1);

    PackageStateSnapshot state;
    state.disabled.insert("test.content-kit");
    const PackageMount mount = MountDevDir(dev_layer, state);
    CHECK(mount.entries.empty());
    REQUIRE(mount.disabled_skipped_ids.size() == 1);
    CHECK(mount.disabled_skipped_ids[0] == "test.content-kit");
    CHECK(mount.records.empty());  // 连分析账都不进挂载账

    // 复启后照旧挂载。
    const PackageMount enabled_mount = MountDevDir(dev_layer);
    REQUIRE(enabled_mount.entries.size() == 1);
    CHECK(enabled_mount.entries[0].package_id == "test.content-kit");
    CHECK(enabled_mount.disabled_skipped_ids.empty());
}

TEST_CASE("快照也吃启停门:折进 PackageSnapshot 的挂载账同样跳过") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    PackageMountInput input;
    input.scan.dev_roots.push_back(dev_layer);
    input.state.disabled.insert("test.content-kit");
    const std::shared_ptr<const PackageSnapshot> snapshot = BuildPackageSnapshot(input, 1);
    CHECK(snapshot->empty());
    CHECK(snapshot->state.disabled.count("test.content-kit") == 1);  // 启停账随快照记档
    CHECK(snapshot->SkillBody("test.content-kit:checklist") == std::nullopt);

    input.state.disabled.clear();
    const std::shared_ptr<const PackageSnapshot> enabled = BuildPackageSnapshot(input, 2);
    REQUIRE(enabled->mount().entries.size() == 1);
    CHECK(enabled->SkillBody("test.content-kit:checklist").has_value());
}

// ---------------------------------------------------------------------------
// 命令拆解(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("ParsePackageCommand:enable/disable/reload 子命令") {
    CHECK(ParsePackageCommand("enable test.content-kit").action ==
          lubancode::app::PackageCommandAction::Enable);
    CHECK(ParsePackageCommand("enable test.content-kit").target == "test.content-kit");
    CHECK(ParsePackageCommand("disable test.content-kit").action ==
          lubancode::app::PackageCommandAction::Disable);
    CHECK(ParsePackageCommand("reload").action == lubancode::app::PackageCommandAction::Reload);
    // 缺目标 / reload 带赘词:Invalid。
    CHECK(ParsePackageCommand("enable").action == lubancode::app::PackageCommandAction::Invalid);
    CHECK(ParsePackageCommand("disable").action == lubancode::app::PackageCommandAction::Invalid);
    CHECK(ParsePackageCommand("reload now").action == lubancode::app::PackageCommandAction::Invalid);
    CHECK(ParsePackageCommand("enable").bad_word == "enable");
}
