// 轨迹 v3 P3 第一棒:session_manager 并列识别 v3 会话目录(session_switch
// 接线点 2/3)。两回路并存,按源目录格式分派,不迁移旧档(§1.5),v2 行为
// 一律不变:
//   - LatestResumableSessionId 无 manifest 的目录认 <id>.jsonl 首行
//     schemaVersion==3,创建时间取首行 timestamp;
//   - ResumeAsNew 对 v3 源走 ReadV3Ledger 验卷 + ProjectModelContext 链
//     投影(§4.10:只取本账链,compact 内部问答天然排除),新场照开;
//   - v2 源照旧 FoldStreamReplay/checkpoint/悬空三道账;
//   - session_index(/sessions、选择器的数据源)把 v3 场列进摘要。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"

namespace platform = lubancode::platform;
using namespace lubancode::trajectory;

namespace {

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

struct FakeClock : SessionManagerClock {
    // 2027-01-01:比 fixture 的 2026-09-10 晚——v2 脚手架场比 v3 场"新",
    // 默认源取谁一测便知。
    std::int64_t wall = 1798761600000LL;
    std::int64_t WallMs() const override { return wall; }
};

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("lubancode-v3-resume-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.238-test";
    return options;
}

// 把 fixture 拷成 workspace 里的 v3 会话目录:sessions/<sessionId>/<sessionId>.jsonl
//(sessionId 取自首行,布局合同 §1.2)。回 v3 会话 id。
std::string PlantV3Session(const std::filesystem::path& sessions_dir, const char* fixture) {
    std::ifstream head(Fixture(fixture), std::ios::binary);
    std::string line;
    std::getline(head, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const auto first = nlohmann::json::parse(line, nullptr, false);
    REQUIRE_FALSE(first.is_discarded());
    const std::string session_id = first.at("sessionId").get<std::string>();
    const std::filesystem::path dir = sessions_dir / platform::Utf8ToPath(session_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::copy_file(Fixture(fixture), dir / platform::Utf8ToPath(session_id + ".jsonl"),
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    return session_id;
}

// 开一场 v2 脚手架场只为把 workspace 目录建出来;回 sessions/ 根与本场 id。
struct Scaffold {
    std::filesystem::path root;
    std::unique_ptr<SessionManager> manager;
    std::string v2_id;

    explicit Scaffold(const char* tag) : root(MakeRoot(tag)) {
        manager = std::make_unique<SessionManager>(Opts(root));
        auto* active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        v2_id = active->session_id();
        sessions_dir = active->session_dir().parent_path();
        NullClearParticipant participant;
        REQUIRE(manager->Close({"exit"}, &participant).error_code.empty());
    }

    std::filesystem::path sessions_dir;
};

std::vector<nlohmann::json> Events(const std::filesystem::path& stream) {
    const auto lines = ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    std::vector<nlohmann::json> events;
    for (const std::string& raw : *lines) {
        events.push_back(nlohmann::json::parse(raw, nullptr, false));
        REQUIRE_FALSE(events.back().is_discarded());
    }
    return events;
}

}  // namespace

// ---------------------------------------------------------------------------
// 接线点 2:清单并列识别
// ---------------------------------------------------------------------------

TEST_CASE("清单: LatestResumableSessionId 认 v3 场,识别不改盘、不迁移旧档") {
    Scaffold scaffold("list");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    // 默认源取"最近一场可恢复":2027 的 v2 脚手架场比 2026-09-10 的 v3
    // 场新——拿一只没挂 active 的 manager 问(Close 后 active 残留的场,
    // 本 manager 自己会跳过)。
    {
        SessionManager picker(Opts(scaffold.root));
        CHECK(picker.LatestResumableSessionId() == scaffold.v2_id);
    }
    // v2 场退场(Close 已放锁,直删目录):v3 场顶上。
    std::error_code ec;
    std::filesystem::remove_all(
        scaffold.sessions_dir / platform::Utf8ToPath(scaffold.v2_id), ec);
    {
        SessionManager picker(Opts(scaffold.root));
        CHECK(picker.LatestResumableSessionId() == v3_id);
    }

    // 识别是纯读:v3 目录没长出 main.jsonl,原档字节原样(不迁移)。
    CHECK_FALSE(std::filesystem::exists(
        scaffold.sessions_dir / platform::Utf8ToPath(v3_id) / "main.jsonl", ec));
}

// ---------------------------------------------------------------------------
// 接线点 3:resume 读回路分派
// ---------------------------------------------------------------------------

TEST_CASE("v3 resume: 验卷+链投影出有效对话,新场照开,attached 记 v3 版本") {
    Scaffold scaffold("v3resume");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");

    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = v3_id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CAPTURE(outcome.error_code);
    CAPTURE(outcome.message);
    REQUIRE(outcome.error_code.empty());

    // v3 分派印记:source_is_v3 + 流路径;事件数 = v3 总行数(25)。
    CHECK(outcome.source_is_v3);
    CHECK(outcome.source_v3_stream.filename().generic_string() == v3_id + ".jsonl");
    CHECK(outcome.source_event_count == 25);
    CHECK(outcome.source_verified);
    CHECK(IsHex64(outcome.source_main_last_event_hash));
    CHECK(outcome.replay_version == "v3-context-chain-1");

    // 有效对话只取本账链(§4.10):compact.applied 后的链 = 摘要 msg-000008
    // + 保留 msg-000004/000005 + 压缩后续问 msg-000009;被压缩原文
    // msg-000002/000003 与 compact 内部问答(prompt/候选)都不进模型输入。
    REQUIRE(outcome.effective_conversation.size() == 4);
    CHECK(outcome.effective_conversation[0].source_event_id == "msg-000008");
    CHECK(outcome.effective_conversation[0].role == ReplayMessage::Role::User);
    REQUIRE(outcome.effective_conversation[0].blocks.size() == 1);
    CHECK(outcome.effective_conversation[0].blocks[0].value("text", std::string()).find("摘要") !=
          std::string::npos);
    CHECK(outcome.effective_conversation[1].source_event_id == "msg-000004");
    CHECK(outcome.effective_conversation[1].role == ReplayMessage::Role::User);
    CHECK(outcome.effective_conversation[2].source_event_id == "msg-000005");
    CHECK(outcome.effective_conversation[2].role == ReplayMessage::Role::Assistant);
    CHECK(outcome.effective_conversation[3].source_event_id == "msg-000009");
    for (const auto& message : outcome.effective_conversation) {
        CHECK(message.source_event_id != "msg-000002");  // 被压缩原文不重携
        CHECK(message.source_event_id != "msg-000006");  // compact prompt 未入链
        CHECK(message.source_event_id != "msg-000007");  // compact 候选回复未入链
    }

    // 新场照开(v2 写侧未切,接线点 1 另算):run.started(resume) 是首条,
    // attached 的 replay_version 记 v3 链投影版本。
    CHECK(outcome.new_session_running);
    CHECK(outcome.new_session_id != v3_id);
    ActiveSession* active = manager.active();
    REQUIRE(active != nullptr);
    const auto events = Events(active->directory.main_stream_path());
    REQUIRE(events.size() >= 2);
    CHECK(events[0].at("kind").get<std::string>() == "run.started");
    CHECK(events[0].at("payload").at("resumed_from_session_id").get<std::string>() == v3_id);
    CHECK(events[0].at("payload").at("caused_by_event_ref").at("session_id").get<std::string>() ==
          v3_id);
    CHECK(events[1].at("kind").get<std::string>() == "resume.source.attached");
    CHECK(events[1].at("payload").at("replay_version").get<std::string>() == "v3-context-chain-1");
    // v3 源的悬空三道账不伪造(v2 折叠概念;执行状态恢复是后续棒)。
    CHECK(outcome.dangling_tools.empty());
}

TEST_CASE("v3 坏账: 验卷不过明拒 resume.source_corrupt,不开新场") {
    Scaffold scaffold("v3corrupt");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "tool_round.jsonl");
    // 等长改坏中段一行正文(哈希链断):ReadV3Ledger 验卷应拒。
    const auto stream = scaffold.sessions_dir / platform::Utf8ToPath(v3_id) /
                        platform::Utf8ToPath(v3_id + ".jsonl");
    std::ifstream in(stream, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string needle = "我读一下入口文件";
    const std::size_t tamper = data.find(needle);
    REQUIRE(tamper != std::string::npos);
    data.replace(tamper, needle.size(), "我改了入口文件。");
    std::ofstream out(stream, std::ios::binary | std::ios::trunc);
    out << data;
    out.close();

    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = v3_id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_corrupt");
    CHECK_FALSE(outcome.new_session_running);
    CHECK(manager.active() == nullptr);
}

TEST_CASE("v2 源行为不变: 照旧 FoldStreamReplay,不标 v3") {
    Scaffold scaffold("v2only");
    // 只留 v2 场(不种 v3):走原 v2 回路。
    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = scaffold.v2_id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CAPTURE(outcome.error_code);
    CAPTURE(outcome.message);
    REQUIRE(outcome.error_code.empty());
    CHECK_FALSE(outcome.source_is_v3);
    CHECK(outcome.source_v3_stream.empty());
    CHECK(outcome.replay_version == std::to_string(kReplayProjectionVersion));
    CHECK(outcome.source_verified);
    CHECK(outcome.new_session_running);
    // v2 空转场的有效对话为空(user 输入没写过,脚手架只开了张)——不冒充。
    CHECK(outcome.effective_conversation.empty());
}

// ---------------------------------------------------------------------------
// 接线点 2:session_index(/sessions、选择器数据源)并列识别 v3
// ---------------------------------------------------------------------------

TEST_CASE("session_index: v3 场进列表,摘要如实,坏尾标 damaged") {
    Scaffold scaffold("index");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    std::error_code ec;
    std::filesystem::remove_all(
        scaffold.sessions_dir / platform::Utf8ToPath(scaffold.v2_id), ec);

    SessionIndexQuery query;
    query.current_workspace_key = scaffold.manager->workspace_key();
    const SessionIndexPage page = QueryWorkspaceSessions(Opts(scaffold.root).workspaces_root, query);
    REQUIRE(page.entries.size() == 1);
    const WorkspaceSessionSummary& summary = page.entries[0];
    CHECK(summary.session_id == v3_id);
    // fixture 没有 session.ended:按事实折 incomplete,不冒充 closed。
    CHECK(summary.status == SessionStatusName(SessionStatus::Incomplete));
    CHECK_FALSE(summary.damaged);
    CHECK(summary.event_count == 25);          // 两类行合计
    CHECK(summary.message_count == 5);         // human user×3 + assistant×2(compact 内部不算)
    CHECK(summary.model == "kimi-k2.6");       // 首个 model.request.prepared
    CHECK(summary.first_user_text.find("第一轮") != std::string::npos);
    CHECK(summary.created_at_ms > 0);          // 首行 timestamp 折出来了
    CHECK(summary.updated_at_ms >= summary.created_at_ms);

    // 指纹认 v3 字节:重查一次走索引缓存,摘要不回潮;追加一行后指纹变动
    // 重扫,坏行跳过但如实标 damaged(与 v2 扫描同口径)。
    const SessionIndexPage again = QueryWorkspaceSessions(Opts(scaffold.root).workspaces_root, query);
    REQUIRE(again.entries.size() == 1);
    CHECK(again.entries[0].event_count == 25);
    const auto stream = scaffold.sessions_dir / platform::Utf8ToPath(v3_id) /
                        platform::Utf8ToPath(v3_id + ".jsonl");
    {
        std::ofstream append(stream, std::ios::binary | std::ios::app);
        append << "{\"broken";
    }
    const SessionIndexPage after = QueryWorkspaceSessions(Opts(scaffold.root).workspaces_root, query);
    REQUIRE(after.entries.size() == 1);
    CHECK(after.entries[0].damaged);
    CHECK(after.entries[0].event_count == 25);  // 坏行跳过不计数,与 v2 同口径
}
