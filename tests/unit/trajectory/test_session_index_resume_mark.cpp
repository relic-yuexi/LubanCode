// resume 续场标记(resume 列表可读性单)——v2 侧的投影与缓存升级:
//   - v2 的 resume.source.attached 载荷合同键是 source_session_id
//     (schema.cpp 载荷表;v3 才是 sourceRef.sessionId,那册见
//     test_v3_index_projection.cpp),折进摘要的 resumed_from_session_id;
//   - 非续场留空(空=非续场是展示层的合同);
//   - 摘要缓存序列化带该键;索引版本升级(老版本缓存)后整份重扫,
//     指纹没动的场也从主账重建出续场标记——版本门说了算,不靠
//     "改一字节"触发。
//
// 全册手植(workspace.json/session.json/main.jsonl 按 reader 合同写),
// 不经 SessionManager/环境变量,形状确定。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/session_index.hpp"

namespace platform = lubancode::platform;
using namespace lubancode::trajectory;

namespace {

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("lubancode-resume-mark-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

// 手植一间房(workspace.json 自描述 + sessions/ 目录)。ResolveDirBy-
// WorkspaceKey 按 manifest 反查,只认 workspace_key/identity_kind 两枚
// 必填键(见 WorkspaceManifest::FromJson)。
std::filesystem::path PlantWorkspaceRoom(const std::filesystem::path& root) {
    const std::filesystem::path room = root / "workspaces" / "room-resume-mark";
    std::error_code ec;
    std::filesystem::create_directories(room / "sessions", ec);
    const nlohmann::json manifest = nlohmann::json{{"version", 1},
                                                   {"workspace_key", "resume-mark-ws"},
                                                   {"identity_kind", "cwd"},
                                                   {"identity_root", "D:/resume/mark"},
                                                   {"created_at_ms", 1759000000000LL}};
    std::ofstream out(room / "workspace.json", std::ios::binary | std::ios::trunc);
    out << manifest.dump();
    return room;
}

// 手植一场 v2 会话:session.json(manifest 可读)+ main.jsonl(索引单遍
// 扫认 kind/payload/wall_time_ms,行给生产同形状)。resumed_from 非空时
// 落一枚 resume.source.attached;title 照 control.title.changed 折——
// 续场与源场同名,正是列表里"看着像重复"的形状。
void PlantV2Session(const std::filesystem::path& room, const std::string& session_id,
                    const std::string& title, const std::string& resumed_from) {
    const std::filesystem::path dir = room / "sessions" / platform::Utf8ToPath(session_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    {
        const nlohmann::json manifest =
            nlohmann::json{{"schema_version", 2},
                           {"workspace_key", "resume-mark-ws"},
                           {"session_id", session_id},
                           {"launch_cwd", "D:/resume/mark"},
                           {"main_run_id", "run-0001"},
                           {"run_kind", "main_session"},
                           {"start_reason", resumed_from.empty() ? "interactive" : "resume"},
                           {"status", "closed"},
                           {"created_at_ms", 1759000000000LL},
                           {"lubancode_version", "test"}};
        std::ofstream out(dir / "session.json", std::ios::binary | std::ios::trunc);
        out << manifest.dump();
    }
    nlohmann::json lines = nlohmann::json::array();
    std::int64_t wall = 1759000000000LL;
    std::int64_t seq = 0;
    const auto push = [&](const char* kind, nlohmann::json payload) {
        ++seq;
        wall += 1000;
        lines.push_back(nlohmann::json{{"seq", seq},
                                       {"event_id", "evt-" + std::to_string(1000000 + seq)},
                                       {"kind", kind},
                                       {"wall_time_ms", wall},
                                       {"payload", std::move(payload)}});
    };
    if (!resumed_from.empty()) {
        push("resume.source.attached",
             nlohmann::json{{"source_session_id", resumed_from},
                            {"source_terminal_event_hash", "hash-src-terminal"},
                            {"replay_version", "replay-v1"},
                            {"imported_state_hash", "hash-imported-state"}});
    }
    push("input.received",
         nlohmann::json{{"input_id", "input-0001"},
                        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                                          {"text", "问一句"}}})},
                        {"channel", "terminal"},
                        {"sender", nlohmann::json{{"kind", "local_user"}}}});
    push("control.title.changed", nlohmann::json{{"title", title}});
    std::ofstream out(dir / "main.jsonl", std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) {
        out << line.dump() << "\n";
    }
}

const WorkspaceSessionSummary* FindSummary(const SessionIndexPage& page, const std::string& id) {
    for (const auto& entry : page.entries) {
        if (entry.session_id == id) {
            return &entry;
        }
    }
    return nullptr;
}

nlohmann::json ReadIndexJson(const std::filesystem::path& room) {
    std::ifstream file(room / "indexes" / "sessions.json", std::ios::binary);
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
    REQUIRE_FALSE(json.is_discarded());
    return json;
}

SessionIndexQuery RoomQuery() {
    SessionIndexQuery query;
    query.current_workspace_key = "resume-mark-ws";
    return query;
}

}  // namespace

TEST_CASE("v2 投影: resume.source.attached 折来源场,非续场留空") {
    const std::filesystem::path root = MakeRoot("v2-proj");
    const std::filesystem::path room = PlantWorkspaceRoom(root);
    const std::string source_id = "20260901-090000-SRCV2A";
    const std::string resumed_id = "20260902-100000-RESV2B";
    PlantV2Session(room, source_id, "源场的标题", "");
    PlantV2Session(room, resumed_id, "源场的标题", source_id);  // 续场继承同名

    const auto page = QueryWorkspaceSessions(root / "workspaces", RoomQuery());
    CHECK(page.diagnostic.empty());
    REQUIRE(page.entries.size() == 2);
    const WorkspaceSessionSummary* source = FindSummary(page, source_id);
    REQUIRE(source != nullptr);
    CHECK(source->resumed_from_session_id.empty());  // 源场:非续场
    const WorkspaceSessionSummary* resumed = FindSummary(page, resumed_id);
    REQUIRE(resumed != nullptr);
    CHECK(resumed->resumed_from_session_id == source_id);  // v2 合同键折进来
    CHECK(resumed->title == "源场的标题");  // 同名继承:区分靠标记,不改标题
    CHECK(resumed->run_kind == "main_session");
}

TEST_CASE("缓存: 老版本索引整份重扫,续场标记从未变化的主账重建") {
    const std::filesystem::path root = MakeRoot("v2-cache");
    const std::filesystem::path room = PlantWorkspaceRoom(root);
    const std::string source_id = "20260903-090000-SRCV2C";
    const std::string resumed_id = "20260904-100000-RESV2D";
    PlantV2Session(room, source_id, "源场的标题", "");
    PlantV2Session(room, resumed_id, "源场的标题", source_id);

    {  // 第一查:写回当前版本索引,行带续场标记(序列化带键)。
        const auto page = QueryWorkspaceSessions(root / "workspaces", RoomQuery());
        REQUIRE(FindSummary(page, resumed_id) != nullptr);
    }
    const auto index_json = ReadIndexJson(room);
    REQUIRE(index_json.value("version", 0) == 3);
    bool serialized = false;
    for (const auto& row : index_json["sessions"]) {
        if (row.value("session_id", std::string()) == resumed_id) {
            serialized = row.value("resumed_from_session_id", std::string()) == source_id;
        }
    }
    CHECK(serialized);

    // 手工降回上一版(v2 缓存形状:行缺 resumed_from_session_id 键),
    // 主账一字不动——指纹全对得上,逼出整份重扫的只能是版本门。
    {
        nlohmann::json stale = index_json;
        stale["version"] = 2;
        for (auto& row : stale["sessions"]) {
            row.erase("resumed_from_session_id");
        }
        std::ofstream out(room / "indexes" / "sessions.json", std::ios::binary | std::ios::trunc);
        out << stale.dump();
    }

    const auto page = QueryWorkspaceSessions(root / "workspaces", RoomQuery());
    const WorkspaceSessionSummary* resumed = FindSummary(page, resumed_id);
    REQUIRE(resumed != nullptr);
    CHECK(resumed->resumed_from_session_id == source_id);  // 从主账重建
    const WorkspaceSessionSummary* source = FindSummary(page, source_id);
    REQUIRE(source != nullptr);
    CHECK(source->resumed_from_session_id.empty());
    CHECK(ReadIndexJson(room).value("version", 0) == 3);  // 写回新版本
}
