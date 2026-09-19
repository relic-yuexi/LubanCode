// 更新器布局层册(批二第③单)。覆盖:
//   - DetectLayout 三态:empty(什么都没有)/flat(根 EXE 或 install-state.json
//     在)/versioned(current.json 在),口径照 updater.py detect_layout;
//   - ReadCurrent:好指针读出(含 previous);坏 JSON 当无;schema 不对当无;
//     current 非单段名(含 '/'、'\\'、'.'、'..')拒——launcher 严格口径;
//     utf-8-sig 容错;
//   - WriteCurrent:原子写不留 tmp;指针换向记 previous;落盘字段
//     (schema/current/previous/updated_at_utc/transaction)逐名断言;
//   - install-state v2 往返:写 -> 读字段齐;坏 JSON/schema 不对当无;
//     rolled_back_at_utc 置值才落。
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "updater/layout.hpp"
#include "updater/manifest.hpp"

namespace {

using namespace lubancode::updater;

std::filesystem::path TempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_updater_layout_" + std::string(name) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void WriteBytes(const std::filesystem::path& file, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::optional<std::string> ReadBytes(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> DirFileNames(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().generic_string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

#ifdef _WIN32
constexpr const char* kExeName = "lubancode.exe";
#else
constexpr const char* kExeName = "lubancode";
#endif

std::string FixedNow() { return "2026-09-20T12:00:00Z"; }

}  // namespace

// ------------------------------------------------------------- 布局与探测 ---

TEST_CASE("MakeLayoutPaths: install_root 下的相对布局逐名对账") {
    const LayoutPaths paths = MakeLayoutPaths(std::filesystem::path("R") / "oot");
    CHECK(paths.root == std::filesystem::path("R") / "oot");
    CHECK(paths.versions == paths.root / "versions");
    CHECK(paths.current == paths.root / "current.json");
    CHECK(paths.state == paths.root / "install-state.json");
    CHECK(paths.staging == paths.root / "staging");
    CHECK(paths.backups == paths.root / "backups");
    CHECK(paths.updates == paths.root / "updates");
    CHECK(paths.exe == paths.root / kExeName);
    CHECK(paths.updater == paths.root / "updater");
}

TEST_CASE("DetectLayout: 三态探测(updater.py detect_layout 口径)") {
    const auto root = TempRoot("detect");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // 什么都没有:empty。
    CHECK(DetectLayout(root) == LayoutKind::Empty);

    // 只有一些不相干的目录/文件:还是 empty(目录不算数,只认文件)。
    std::filesystem::create_directories(paths.versions);
    CHECK(DetectLayout(root) == LayoutKind::Empty);

    // 根 EXE 在:flat。
    WriteBytes(paths.exe, "MZ");
    CHECK(DetectLayout(root) == LayoutKind::Flat);
    std::filesystem::remove(paths.exe);

    // install-state.json 在:也是 flat(旧平铺装过的账)。
    WriteBytes(paths.state, "{}");
    CHECK(DetectLayout(root) == LayoutKind::Flat);
    std::filesystem::remove(paths.state);

    // current.json 在:versioned(优先于其余两判)。
    WriteBytes(paths.exe, "MZ");
    WriteBytes(paths.current, "{}");
    CHECK(DetectLayout(root) == LayoutKind::Versioned);
}

// --------------------------------------------------------------- current ---

TEST_CASE("ReadCurrent: 好指针读出,previous 可有可无") {
    const auto root = TempRoot("read-current");
    const LayoutPaths paths = MakeLayoutPaths(root);

    WriteBytes(paths.current,
               R"({"schema": 1, "current": "0.26.280-bbbbbbbb", "previous": "0.26.279-aaaaaaaa"})");
    auto pointer = ReadCurrent(paths);
    REQUIRE(pointer.has_value());
    CHECK(pointer->current == "0.26.280-bbbbbbbb");
    REQUIRE(pointer->previous.has_value());
    CHECK(*pointer->previous == "0.26.279-aaaaaaaa");

    // previous 缺/null/空串:读出 nullopt(python read_current 同款)。
    WriteBytes(paths.current, R"({"schema": 1, "current": "0.26.280-bbbbbbbb"})");
    pointer = ReadCurrent(paths);
    REQUIRE(pointer.has_value());
    CHECK_FALSE(pointer->previous.has_value());

    WriteBytes(paths.current,
               R"({"schema": 1, "current": "0.26.280-bbbbbbbb", "previous": null})");
    CHECK_FALSE(ReadCurrent(paths)->previous.has_value());

    WriteBytes(paths.current,
               R"({"schema": 1, "current": "0.26.280-bbbbbbbb", "previous": ""})");
    CHECK_FALSE(ReadCurrent(paths)->previous.has_value());
}

TEST_CASE("ReadCurrent: 坏 JSON 与 schema 不对当无") {
    const auto root = TempRoot("read-current-bad");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // 文件不在:无。
    CHECK_FALSE(ReadCurrent(paths).has_value());

    WriteBytes(paths.current, "{ not json");
    CHECK_FALSE(ReadCurrent(paths).has_value());

    WriteBytes(paths.current, "[1, 2]");
    CHECK_FALSE(ReadCurrent(paths).has_value());

    // schema 不是整数 1:无(launcher is_number_integer 口径)。
    WriteBytes(paths.current, R"({"schema": "1", "current": "a-b"})");
    CHECK_FALSE(ReadCurrent(paths).has_value());
    WriteBytes(paths.current, R"({"schema": 2, "current": "a-b"})");
    CHECK_FALSE(ReadCurrent(paths).has_value());
    WriteBytes(paths.current, R"({"current": "a-b"})");
    CHECK_FALSE(ReadCurrent(paths).has_value());

    // current 不是字符串/空:无。
    WriteBytes(paths.current, R"({"schema": 1, "current": 42})");
    CHECK_FALSE(ReadCurrent(paths).has_value());
    WriteBytes(paths.current, R"({"schema": 1, "current": ""})");
    CHECK_FALSE(ReadCurrent(paths).has_value());
    WriteBytes(paths.current, R"({"schema": 1})");
    CHECK_FALSE(ReadCurrent(paths).has_value());
}

TEST_CASE("ReadCurrent: current 非单段名拒(launcher 严格口径)") {
    const auto root = TempRoot("read-current-seg");
    const LayoutPaths paths = MakeLayoutPaths(root);

    // 经 nlohmann dump 落盘:反斜杠这类字符要按 JSON 规矩转义,直拼进
    // 字面量会变成 \b 一类合法转义(退格符),测的就不是原字符了。
    for (const std::string bad : {"../evil", "a/b", "a\\b", ".", ".."}) {
        const nlohmann::json pointer = {{"schema", 1}, {"current", bad}};
        WriteBytes(paths.current, pointer.dump());
        CHECK_FALSE(ReadCurrent(paths).has_value());
    }
}

TEST_CASE("ReadCurrent: utf-8-sig 容错剥 BOM") {
    const auto root = TempRoot("read-current-bom");
    const LayoutPaths paths = MakeLayoutPaths(root);
    WriteBytes(paths.current,
               "\xEF\xBB\xBF"
               R"({"schema": 1, "current": "0.26.280-bbbbbbbb"})");
    auto pointer = ReadCurrent(paths);
    REQUIRE(pointer.has_value());
    CHECK(pointer->current == "0.26.280-bbbbbbbb");
}

TEST_CASE("WriteCurrent: 原子写不留 tmp,字段逐名断言,换向记 previous") {
    const auto root = TempRoot("write-current");
    const LayoutPaths paths = MakeLayoutPaths(root);

    WriteCurrent(paths, "0.26.280-bbbbbbbb", std::nullopt, "20260920T120000Z-1a2b3c4d", FixedNow);

    // 目录里只有 current.json——临时件已收干净(原子写不留 tmp)。
    REQUIRE(DirFileNames(root).size() == 1);
    CHECK(DirFileNames(root)[0] == "current.json");

    // 字段逐名断言(schema/current/previous/updated_at_utc/transaction)。
    const auto bytes = ReadBytes(paths.current);
    REQUIRE(bytes.has_value());
    const nlohmann::json parsed = nlohmann::json::parse(*bytes, nullptr, false);
    REQUIRE(parsed.is_object());
    CHECK(parsed["schema"] == 1);
    CHECK(parsed["current"] == "0.26.280-bbbbbbbb");
    CHECK(parsed["previous"].is_null());
    CHECK(parsed["updated_at_utc"] == "2026-09-20T12:00:00Z");
    CHECK(parsed["transaction"] == "20260920T120000Z-1a2b3c4d");
    // 落盘是 canonical 形状:排序 + 缩进 2 + 尾换行 + 纯 ASCII。
    CHECK(*bytes == CanonicalJsonDump(parsed));

    // 读回一体。
    auto pointer = ReadCurrent(paths);
    REQUIRE(pointer.has_value());
    CHECK(pointer->current == "0.26.280-bbbbbbbb");
    CHECK_FALSE(pointer->previous.has_value());

    // 指针换向:previous 记上一次的 current。
    WriteCurrent(paths, "0.26.281-cccccccc", pointer->current, "20260921T120000Z-2b3c4d5e",
                 FixedNow);
    pointer = ReadCurrent(paths);
    REQUIRE(pointer.has_value());
    CHECK(pointer->current == "0.26.281-cccccccc");
    REQUIRE(pointer->previous.has_value());
    CHECK(*pointer->previous == "0.26.280-bbbbbbbb");
    // 换向后目录里还是只有一份 current.json(全量替换,不留旧账)。
    CHECK(DirFileNames(root).size() == 1);

    // 坏指针写不出去:读侧既然拒,写侧也不收。
    CHECK_THROWS(WriteCurrent(paths, "../evil", std::nullopt, "t", FixedNow));
    CHECK_THROWS(WriteCurrent(paths, "a/b", std::nullopt, "t", FixedNow));
    CHECK_THROWS(WriteCurrent(paths, "..", std::nullopt, "t", FixedNow));
    CHECK_THROWS(WriteCurrent(paths, "ok-aaaaaaaa", "..", "t", FixedNow));
    // 拒掉的写没有半截落盘。
    const auto after_rejects = ReadCurrent(paths);
    REQUIRE(after_rejects.has_value());
    CHECK(after_rejects->current == "0.26.281-cccccccc");
}

// --------------------------------------------------------- install-state ---

TEST_CASE("InstallState v2 往返: 写 -> 读字段齐") {
    const auto root = TempRoot("state-roundtrip");
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallState state;
    state.version = "0.26.280";
    state.platform = "windows-x64";
    state.channel = "stable";
    state.source_repo = "relic-yuexi/LubanCode";
    state.source_release_id = 123456789;
    state.source_release_tag = "v0.26.280";
    state.source_asset_id = 987654321;
    state.source_asset_name = "lubancode-0.26.280-windows-x64.zip";
    state.source_asset_digest = "sha256:" + std::string(64, 'b');
    state.installer = "updater.py";
    state.transaction = "20260920T120000Z-1a2b3c4d";
    state.manifest = {{"schema", 1}, {"files", nlohmann::json::array()},
                       {"file_count", 0}};

    WriteInstallState(paths, state, FixedNow);

    auto back = ReadInstallState(paths);
    REQUIRE(back.has_value());
    CHECK(back->version == "0.26.280");
    REQUIRE(back->platform.has_value());
    CHECK(*back->platform == "windows-x64");
    CHECK(back->channel == "stable");
    REQUIRE(back->source_repo.has_value());
    CHECK(*back->source_repo == "relic-yuexi/LubanCode");
    REQUIRE(back->source_release_id.has_value());
    CHECK(*back->source_release_id == 123456789);
    REQUIRE(back->source_release_tag.has_value());
    CHECK(*back->source_release_tag == "v0.26.280");
    REQUIRE(back->source_asset_id.has_value());
    CHECK(*back->source_asset_id == 987654321);
    REQUIRE(back->source_asset_name.has_value());
    CHECK(*back->source_asset_name == "lubancode-0.26.280-windows-x64.zip");
    CHECK(back->source_asset_digest == "sha256:" + std::string(64, 'b'));
    CHECK(back->installer == "updater.py");
    CHECK(back->transaction == "20260920T120000Z-1a2b3c4d");
    CHECK(back->manifest == state.manifest);
    CHECK(back->installed_at_utc == "2026-09-20T12:00:00Z");
    CHECK_FALSE(back->rolled_back_at_utc.has_value());

    // 落盘字段名逐字对 python write_install_state:schema/layout/
    // manifest_provenance/pending_conflicts 一并在账上。
    const auto bytes = ReadBytes(paths.state);
    REQUIRE(bytes.has_value());
    const nlohmann::json parsed = nlohmann::json::parse(*bytes, nullptr, false);
    REQUIRE(parsed.is_object());
    CHECK(parsed["schema"] == 2);
    CHECK(parsed["layout"] == "versioned");
    CHECK(parsed["manifest_provenance"] == "official-package");
    CHECK(parsed["pending_conflicts"].is_array());
    CHECK(parsed["pending_conflicts"].empty());
    CHECK(parsed.contains("installed_at_utc"));
    CHECK(parsed.contains("version"));
    CHECK(parsed.contains("platform"));
    CHECK(parsed.contains("channel"));
    CHECK(parsed.contains("source"));
    CHECK(parsed.contains("installer"));
    CHECK(parsed.contains("transaction"));
    CHECK(parsed.contains("manifest"));
    CHECK_FALSE(parsed.contains("rolled_back_at_utc"));  // 置值才落
}

TEST_CASE("InstallState: rolled_back_at_utc 置值才落,读侧保留") {
    const auto root = TempRoot("state-rollback");
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallState state;
    state.version = "0.26.279";
    state.channel = "stable";
    state.installer = "updater.py";
    state.transaction = "t-1";
    state.rolled_back_at_utc = "2026-09-21T09:30:00Z";
    WriteInstallState(paths, state, FixedNow);

    auto back = ReadInstallState(paths);
    REQUIRE(back.has_value());
    REQUIRE(back->rolled_back_at_utc.has_value());
    CHECK(*back->rolled_back_at_utc == "2026-09-21T09:30:00Z");
}

TEST_CASE("InstallState: 坏 JSON 与 schema 不对当无") {
    const auto root = TempRoot("state-bad");
    const LayoutPaths paths = MakeLayoutPaths(root);

    CHECK_FALSE(ReadInstallState(paths).has_value());  // 文件不在

    WriteBytes(paths.state, "{ not json");
    CHECK_FALSE(ReadInstallState(paths).has_value());

    // schema 2 之外一概不认。
    WriteBytes(paths.state, R"({"schema": 1, "version": "1.0"})");
    CHECK_FALSE(ReadInstallState(paths).has_value());
    WriteBytes(paths.state, R"({"schema": "2"})");
    CHECK_FALSE(ReadInstallState(paths).has_value());
    WriteBytes(paths.state, R"({"version": "1.0"})");
    CHECK_FALSE(ReadInstallState(paths).has_value());

    // schema 对但字段缺:能读多少读多少(事实账,不整份拒)。
    WriteBytes(paths.state, R"({"schema": 2})");
    auto back = ReadInstallState(paths);
    REQUIRE(back.has_value());
    CHECK(back->version.empty());
    CHECK(back->channel.empty());
    CHECK_FALSE(back->source_repo.has_value());
}

TEST_CASE("UtcNowIso8601: UTC ISO8601 秒精度 Z 尾") {
    const std::string stamp = UtcNowIso8601();
    // 形状钉死:YYYY-MM-DDTHH:MM:SSZ,20 字符。
    REQUIRE(stamp.size() == 20);
    CHECK(stamp[4] == '-');
    CHECK(stamp[7] == '-');
    CHECK(stamp[10] == 'T');
    CHECK(stamp[13] == ':');
    CHECK(stamp[16] == ':');
    CHECK(stamp[19] == 'Z');
    for (const char c : stamp) {
        CHECK((std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == 'T' ||
               c == ':' || c == 'Z'));
    }
}
