// Telemetry cursor 测试(端云协同可观测单 §14.2,实施分期 T1):
//   - 往返:store 后 load 回同值;schema/version/身份不符明拒;
//   - 原子替换:写完不留 .tmp 尾巴,重写覆盖旧值;
//   - 文件不存在 = 新 stream(nullopt,无错误);
//   - 坏 JSON = telemetry.cursor_corrupt,不猜。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "telemetry/contract.hpp"
#include "telemetry/cursor.hpp"

using namespace lubancode::telemetry;

namespace {

std::filesystem::path FreshRoot(const char* tag) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lubancode-tel-cursor-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

StreamCursor MakeCursor() {
    StreamCursor cursor;
    cursor.workspace_key = "ws-000000000000";
    cursor.session_id = "20260830-031522-TEST01";
    cursor.stream = "main.jsonl";
    cursor.last_event_id = "main-1:evt-00000003";
    cursor.last_event_hash = "abc123";
    cursor.projector_version = std::string(kProjectorVersion);
    cursor.projection_generation = 1;
    cursor.updated_at_ms = 1759000000000LL;
    return cursor;
}

}  // namespace

TEST_CASE("cursor 往返:store 后 load 回同值,不留 .tmp") {
    const std::filesystem::path root = FreshRoot("roundtrip");
    const StreamCursor cursor = MakeCursor();
    REQUIRE(StoreCursor(root, cursor));
    std::string error;
    const auto loaded = LoadCursor(root, cursor.workspace_key, cursor.session_id,
                                   cursor.stream, &error);
    REQUIRE(loaded.has_value());
    CHECK(error.empty());
    CHECK(loaded->last_event_id == cursor.last_event_id);
    CHECK(loaded->last_event_hash == cursor.last_event_hash);
    CHECK(loaded->projection_generation == 1);
    CHECK(loaded->projector_version == std::string(kProjectorVersion));
    // 原子替换不留尾巴(§14.2)。
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(
        CursorFilePath(root, cursor.workspace_key, cursor.session_id, cursor.stream).string() + ".tmp", ec));
    // subagent stream 的 '/' 折进文件名,不逃出 session 目录。
    const StreamCursor sub = [] {
        StreamCursor cursor = MakeCursor();
        cursor.stream = "subagents/agent-1-main.jsonl";
        return cursor;
    }();
    REQUIRE(StoreCursor(root, sub));
    CHECK(std::filesystem::exists(CursorFilePath(root, sub.workspace_key, sub.session_id, sub.stream)));
}

TEST_CASE("cursor:重写覆盖;文件不存在 = 新 stream") {
    const std::filesystem::path root = FreshRoot("overwrite");
    StreamCursor cursor = MakeCursor();
    REQUIRE(StoreCursor(root, cursor));
    cursor.last_event_id = "main-1:evt-00000009";
    cursor.last_event_hash = "def456";
    REQUIRE(StoreCursor(root, cursor));
    const auto loaded = LoadCursor(root, cursor.workspace_key, cursor.session_id, cursor.stream, nullptr);
    REQUIRE(loaded.has_value());
    CHECK(loaded->last_event_id == "main-1:evt-00000009");
    // 没写过的 stream:无文件、无错误。
    std::string error = "stale";
    CHECK_FALSE(
        LoadCursor(root, "ws-000000000000", "20260830-031522-OTHER", "main.jsonl", &error).has_value());
    CHECK(error.empty());
}

TEST_CASE("cursor:坏 JSON 与身份错位明拒,不猜") {
    const std::filesystem::path root = FreshRoot("corrupt");
    const StreamCursor cursor = MakeCursor();
    REQUIRE(StoreCursor(root, cursor));
    const auto path = CursorFilePath(root, cursor.workspace_key, cursor.session_id, cursor.stream);
    // 坏 JSON。
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << "{ not json"; }
    std::string error;
    CHECK_FALSE(LoadCursor(root, cursor.workspace_key, cursor.session_id, cursor.stream, &error).has_value());
    CHECK(error == "telemetry.cursor_corrupt");
    // 身份错位:别的 stream 名来读,文件身份对不上(§14.2 换账拒绝)。
    REQUIRE(StoreCursor(root, MakeCursor()));
    error.clear();
    CHECK_FALSE(LoadCursor(root, cursor.workspace_key, cursor.session_id, "subagents/x.jsonl",
                           &error).has_value());
    CHECK(error.empty());  // 该 stream 没自己的文件 = 新 stream,不算错
    // 目录穿越材料当场拒。
    error.clear();
    CHECK_FALSE(LoadCursor(root, "..", "20260830-031522-TEST01", "main.jsonl", &error).has_value());
    CHECK(error == "telemetry.cursor_bad_identity");
}
