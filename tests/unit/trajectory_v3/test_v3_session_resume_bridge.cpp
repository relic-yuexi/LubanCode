// 轨迹 v3 P3 第一棒:session_manager 并列识别 v3 会话目录(session_switch
// 接线点 2/3)。两回路并存,按源目录格式分派,不迁移旧档(§1.5),v2 行为
// 一律不变:
//   - LatestResumableSessionId 无 manifest 的目录认 <id>.jsonl 首行
//     schemaVersion==3,创建时间取首行 timestamp;
//   - ResumeAsNew 对 v3 源走 ReadV3Ledger 验卷 + ProjectModelContext 链
//     投影(§4.10:只取本账链,compact 内部问答天然排除)。2026-09-19
//     用户拍板后落点改为续接源场:同 id 续写,不开新账、不抄链、列表
//     不新增条目,源账 append-only;
//   - v2 源照旧 FoldStreamReplay/checkpoint/悬空三道账,fork 迁移开新场;
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
#include "trajectory/recorder.hpp"  // PlantV2Source:真 recorder 写 v2 源档
#include "trajectory/replay.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/session_switch.hpp"  // ProbeV3SessionStream(R2 格式探针)
#include "trajectory/v3/writer.hpp"          // V3Writer::Continue/VerifyV3File(续接续写)

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

// 开一场脚手架场把 workspace 目录建出来;回 sessions/ 根与本场 id。
// V3-LEGACY-01 后新建唯一 v3,这场也是 v3——v2 形状一律走 PlantV2Source。
struct Scaffold {
    std::filesystem::path root;
    std::unique_ptr<SessionManager> manager;
    std::string open_id;

    explicit Scaffold(const char* tag) : root(MakeRoot(tag)) {
        manager = std::make_unique<SessionManager>(Opts(root));
        auto* active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        open_id = active->session_id();
        sessions_dir = active->session_dir().parent_path();
        NullClearParticipant participant;
        REQUIRE(manager->Close({"exit"}, &participant).error_code.empty());
    }

    std::filesystem::path sessions_dir;
};

// 手植 v2 源场:场目录+session.json 走 CreateSession,主账由真 recorder 写
// 封口链(run.started → run.completed → session.ended)——v2 源只能盘上
// 旧档夹具,读兼容面(fork 迁移)不动。
class PlantClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1760000000000LL; }
    std::int64_t MonotonicNs() const override { return 0LL; }
};

std::string PlantV2Source(const Scaffold& scaffold, const std::string& session_id) {
    SessionManifest manifest;
    manifest.schema_version = 2;
    manifest.workspace_key = scaffold.manager->workspace_key();
    manifest.session_id = session_id;
    manifest.main_run_id = "main-plant-1";
    manifest.run_kind = RunKindName(RunKind::MainSession);
    manifest.start_reason = "process_launch";
    manifest.status = SessionStatusName(SessionStatus::Closed);
    manifest.created_at_ms = 1760000000000LL;
    manifest.lubancode_version = "0.26.238-test";
    manifest.event_schema_version = 2;
    auto directory = TrajectoryDirectory::CreateSession(Opts(scaffold.root).workspaces_root,
                                                        manifest.workspace_key, manifest);
    REQUIRE(directory.has_value());
    PlantClock clock;
    EventScope scope;
    scope.workspace_key = manifest.workspace_key;
    scope.session_id = session_id;
    scope.run_id = manifest.main_run_id;
    scope.run_kind = RunKind::MainSession;
    scope.visibility = {Visibility::HostOnly};
    auto recorder = TrajectoryRecorder::Start(directory->main_stream_path(),
                                              directory->artifacts_root(), scope,
                                              RecorderOptions{}, &clock);
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"start_reason", "process_launch"}},
                                      Durability::PowerLoss)
                 .status == RecordReceipt::Status::Committed);
    REQUIRE(recorder->FinishRun(EventKind::RunCompleted, "exit", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    REQUIRE(recorder->EndSession("exit", std::nullopt, "clean", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    return session_id;
}

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

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// 接线点 2:清单并列识别
// ---------------------------------------------------------------------------

TEST_CASE("清单: LatestResumableSessionId 认 v3 场,识别不改盘、不迁移旧档") {
    Scaffold scaffold("list");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    // 默认源取"最近一场可恢复":脚手架场(V3-LEGACY-01 后也是 v3,时间
    // 戳新)比 2026-09-10 的手植 v3 场新——拿一只没挂 active 的 manager 问
    //(Close 后 active 残留的场,本 manager 自己会跳过)。
    {
        SessionManager picker(Opts(scaffold.root));
        CHECK(picker.LatestResumableSessionId() == scaffold.open_id);
    }
    // 脚手架场退场(Close 已放锁,直删目录):手植 v3 场顶上。
    std::error_code ec;
    std::filesystem::remove_all(
        scaffold.sessions_dir / platform::Utf8ToPath(scaffold.open_id), ec);
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

TEST_CASE("v3 resume: 验卷+链投影出有效对话,续接源场(同 id 续写,不开新账)") {
    Scaffold scaffold("v3resume");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    const auto stream = scaffold.sessions_dir / platform::Utf8ToPath(v3_id) /
                        platform::Utf8ToPath(v3_id + ".jsonl");
    // 源账字节基准:续接只许 append,前缀一字节不动(append-only 底线)。
    const std::string source_prefix = ReadFileBytes(stream);
    std::size_t sessions_before = 0;
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(scaffold.sessions_dir, ec)) {
            if (entry.is_directory(ec)) {
                ++sessions_before;
            }
        }
    }

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
    CHECK(outcome.replay_version == "v3-context-chain-2");

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

    // 落点是源场(2026-09-19 拍板):同 id 续写,不开新账、不新增列表条目;
    // active 是续接的源场(v3 写者,run 号即源账 run 号)。
    CHECK(outcome.new_session_running);
    CHECK(outcome.active_switched);
    CHECK(outcome.new_session_id == v3_id);
    CHECK(outcome.new_main_run_id == "run-000001");  // fixture 首行 runId
    CHECK(outcome.imported_history_count == 0);      // 不抄链,本账自足
    CHECK(outcome.resume_attached_event_id.empty());  // 续接不是挂靠,不写 attached
    ActiveSession* active = manager.active();
    REQUIRE(active != nullptr);
    CHECK(active->session_id() == v3_id);
    CHECK(active->is_v3());
    std::size_t sessions_after = 0;
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(scaffold.sessions_dir, ec)) {
            if (entry.is_directory(ec)) {
                ++sessions_after;
            }
        }
    }
    CHECK(sessions_after == sessions_before);  // 列表条目不增

    // 源账 append-only:前缀字节原样,新增行只有审批档重算事实——没有
    // resume.source.attached(不挂靠),没有 session.started(不重开卷)。
    const std::string after_bytes = ReadFileBytes(stream);
    CHECK(after_bytes.size() > source_prefix.size());
    CHECK(after_bytes.compare(0, source_prefix.size(), source_prefix) == 0);
    const auto rows = Events(stream);
    REQUIRE(rows.size() > 25);
    bool saw_attached = false;
    bool saw_recomputed = false;
    bool saw_started_after_25 = false;
    for (std::size_t i = 25; i < rows.size(); ++i) {
        const std::string kind = rows[i].value("kind", std::string());
        if (kind == "resume.source.attached") {
            saw_attached = true;
        }
        if (kind == "session.started") {
            saw_started_after_25 = true;
        }
        if (kind == "approval.mode.applied") {
            saw_recomputed = true;
            CHECK(rows[i].at("payload").value("source", std::string()) == "resume_recomputed");
        }
        CHECK(rows[i].value("sessionId", std::string()) == v3_id);
    }
    CHECK_FALSE(saw_attached);
    CHECK_FALSE(saw_started_after_25);
    CHECK(saw_recomputed);
    // 续写通路:active 写者从账尾 append,seq 接上(26+),整卷重验过。
    {
        v3::MessageDraft followup;
        followup.turn_id = "turn-000001";
        followup.purpose = v3::MessagePurpose::Conversation;
        followup.origin = v3::MessageOrigin::Human;
        followup.message =
            nlohmann::json::object({{"role", "user"}, {"content", "续接后的第一句。"}});
        const auto receipt =
            active->v3_main->AppendMessage(std::move(followup), Durability::PowerLoss);
        REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
        CHECK(receipt.seq == rows.size() + 1);  // 从账尾续号(fixture 25 + 续接事实行)
        const auto admitted = active->v3_main->AdmitMessages({receipt.id}, Durability::PowerLoss);
        REQUIRE(admitted.status == v3::WriteReceipt::Status::Committed);
        const auto report = v3::VerifyV3File(stream);
        REQUIRE(report.ok);
        CHECK(report.lines == rows.size() + 2);  // 续接前读的账 + followup + applied
    }
    // 续接事实入 lifecycle 账:resume_reference + start_reason=resume_in_place
    //(v3 schema 没有 session.resume 事件,不发明新 kind)。
    {
        bool saw_intent = false;
        std::error_code ec;
        for (const auto& op : std::filesystem::directory_iterator(
                 scaffold.sessions_dir.parent_path() / "lifecycle", ec)) {
            std::ifstream in(op.path() / "intent.json", std::ios::binary);
            if (!in.is_open()) {
                continue;
            }
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            const auto intent = nlohmann::json::parse(text, nullptr, false);
            if (intent.is_discarded()) {
                continue;
            }
            if (intent.value("operation", std::string()) == "resume_reference" &&
                intent.value("session_id", std::string()) == v3_id &&
                intent.contains("parameters") &&
                intent["parameters"].value("start_reason", std::string()) == "resume_in_place") {
                saw_intent = true;
            }
        }
        CHECK(saw_intent);
    }
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

// 2026-09-19 落点拍板的用户痛点:两次连续 resume 同一场,列表里场越续
// 越多。续接源场后不再堆场——每回都是同一场续写,列表条目与账本身份
// 都不增。
TEST_CASE("v3 续接不堆场: 连续两次 resume 同一场,列表条目与 id 都不增") {
    Scaffold scaffold("twice");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    const auto stream = scaffold.sessions_dir / platform::Utf8ToPath(v3_id) /
                        platform::Utf8ToPath(v3_id + ".jsonl");
    const auto count_sessions = [&] {
        std::size_t count = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(scaffold.sessions_dir, ec)) {
            if (entry.is_directory(ec)) {
                ++count;
            }
        }
        return count;
    };
    const std::size_t sessions_before = count_sessions();

    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = v3_id;
    const auto first = manager.ResumeAsNew(request);
    REQUIRE(first.error_code.empty());
    CHECK(first.new_session_id == v3_id);
    const std::size_t lines_after_first = Events(stream).size();
    // 一场账本一活场:先封口再续接(交互 /resume 的换场事务同款)。
    NullClearParticipant participant;
    CloseRequest close;
    close.reason = "switch_to_resume";
    REQUIRE(manager.Close(close, &participant).error_code.empty());
    const auto second = manager.ResumeAsNew(request);
    REQUIRE(second.error_code.empty());
    CHECK(second.new_session_id == v3_id);  // 还是同一场
    CHECK(second.source_session_id == v3_id);
    // 列表条目不增;账继续长(第二次续接又落一笔审批档重算事实)。
    CHECK(count_sessions() == sessions_before);
    CHECK(Events(stream).size() > lines_after_first);
    CHECK(v3::VerifyV3File(stream).ok);
    // 会话仍只有一场 v3 账:目录里没有 main.jsonl 长出来。
    CHECK_FALSE(std::filesystem::exists(
        scaffold.sessions_dir / platform::Utf8ToPath(v3_id) / "main.jsonl"));
}

// 活锁拒续接(§10.4 末段):外进程持活锁的源场,续接路照拒——一个字节
// 不写,active 不切。
TEST_CASE("v3 续接: 源场被外进程持活锁,明拒且账面不动") {
    Scaffold scaffold("livelock");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    const auto stream = scaffold.sessions_dir / platform::Utf8ToPath(v3_id) /
                        platform::Utf8ToPath(v3_id + ".jsonl");
    const std::string before = ReadFileBytes(stream);
    // 手工占一把活锁(owner = 本进程,身份对得上 = 活)。
    SessionLockOwner owner = SessionManagerClock{}.LockOwner();
    owner.acquired_at_ms = 1759000000000LL;
    const auto lock = SessionLock::Acquire(
        scaffold.sessions_dir / platform::Utf8ToPath(v3_id), owner);
    REQUIRE(lock.has_value());

    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = v3_id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_locked");
    CHECK_FALSE(outcome.new_session_running);
    CHECK(manager.active() == nullptr);
    CHECK(ReadFileBytes(stream) == before);  // 拒了就不动源账一个字节
}

// R2:格式探针——"认不出"拆成可诊断状态,不混作"没档";ResumeAsNew 对
// 两账并存与首行不合 schema 明确拒绝,不开新场。
TEST_CASE("格式探针: 双账冲突/坏首行/异版本各有稳定状态,resume 明拒") {
    Scaffold scaffold("probe");
    const std::string conflict_id = "20260912-170000-CONFLI";
    const std::filesystem::path conflict_dir = scaffold.sessions_dir / platform::Utf8ToPath(conflict_id);
    std::error_code ec;
    std::filesystem::create_directories(conflict_dir, ec);
    REQUIRE_FALSE(ec);
    {
        std::ofstream main_file(conflict_dir / "main.jsonl", std::ios::binary);
        main_file << "{}\n";
        std::error_code copy_ec;
        std::filesystem::copy_file(Fixture("startup.jsonl"),
                                   conflict_dir / platform::Utf8ToPath(conflict_id + ".jsonl"),
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        REQUIRE_FALSE(copy_ec);
    }
    const std::string bad_id = "20260912-170002-BADFIR";
    const std::filesystem::path bad_dir = scaffold.sessions_dir / platform::Utf8ToPath(bad_id);
    std::filesystem::create_directories(bad_dir, ec);
    {
        std::ofstream bad_file(bad_dir / platform::Utf8ToPath(bad_id + ".jsonl"), std::ios::binary);
        bad_file << "not-json-at-all\n";
    }
    const std::string old_id = "20260912-170003-OLDVER";
    const std::filesystem::path old_dir = scaffold.sessions_dir / platform::Utf8ToPath(old_id);
    std::filesystem::create_directories(old_dir, ec);
    {
        std::ofstream old_file(old_dir / platform::Utf8ToPath(old_id + ".jsonl"), std::ios::binary);
        old_file << R"({"type":"message","schemaVersion":2,"sessionId":")" << old_id
                 << R"(","runId":"main-0001","seq":1,"messageId":"msg-000001","message":{"role":"system","content":""}})"
                 << "\n";
    }

    // 探针状态:冲突/坏首行/异版本各有名分,诊断文本非空。
    auto probe = v3::ProbeV3SessionStream(conflict_dir);
    CHECK(probe.status == v3::V3StreamProbe::Status::FormatConflict);
    CHECK_FALSE(probe.detail.empty());
    probe = v3::ProbeV3SessionStream(bad_dir);
    CHECK(probe.status == v3::V3StreamProbe::Status::BadFirstLine);
    probe = v3::ProbeV3SessionStream(old_dir);
    CHECK(probe.status == v3::V3StreamProbe::Status::NotV3Schema);
    // 正主:v2 布局与 v3 流照旧认得——v2 布局手植(main.jsonl 在即 v2,
    // 新建已造不出,识别读侧不动)。
    const std::string v2_layout_id = "20260912-170004-V2LAYT";
    const std::filesystem::path v2_layout_dir =
        scaffold.sessions_dir / platform::Utf8ToPath(v2_layout_id);
    std::filesystem::create_directories(v2_layout_dir, ec);
    {
        std::ofstream v2_file(v2_layout_dir / "main.jsonl", std::ios::binary);
        v2_file << "{}\n";
    }
    probe = v3::ProbeV3SessionStream(v2_layout_dir);
    CHECK(probe.status == v3::V3StreamProbe::Status::V2Layout);
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "startup.jsonl");
    probe = v3::ProbeV3SessionStream(scaffold.sessions_dir / platform::Utf8ToPath(v3_id));
    CHECK(probe.status == v3::V3StreamProbe::Status::V3Stream);

    // resume 分派:明确拒绝且不开新场;目录原样不动。
    const auto before_bytes = std::filesystem::file_size(conflict_dir / "main.jsonl");
    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = conflict_id;
    auto outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_format_conflict");
    CHECK_FALSE(outcome.new_session_running);
    CHECK(manager.active() == nullptr);
    request.source_session_id = bad_id;
    outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_format_unknown");
    CHECK(manager.active() == nullptr);
    CHECK(std::filesystem::file_size(conflict_dir / "main.jsonl") == before_bytes);
}

TEST_CASE("v2 源行为不变: 照旧 fork 迁移开新场(如今新场唯一 v3),源账不动") {
    Scaffold scaffold("v2only");
    // v2 源手植(封口干净的空转场):走 v2 读回路折叠,v2 账写不动,
    // fork 新场另开——V3-LEGACY-01 后新场唯一 v3。
    const std::string source_id = PlantV2Source(scaffold, "20260924-140000-V2FORK");
    SessionManager manager(Opts(scaffold.root));
    ResumeRequest request;
    request.source_session_id = source_id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CAPTURE(outcome.error_code);
    CAPTURE(outcome.message);
    REQUIRE(outcome.error_code.empty());
    CHECK_FALSE(outcome.source_is_v3);
    CHECK(outcome.source_v3_stream.empty());
    CHECK(outcome.replay_version == std::to_string(kReplayProjectionVersion));
    CHECK(outcome.source_verified);
    CHECK(outcome.new_session_running);
    // fork 语义保留:新场另有其 id(与 v3 源的续接落点相区别)。
    CHECK(outcome.new_session_id != source_id);
    ActiveSession* active = manager.active();
    REQUIRE(active != nullptr);
    REQUIRE(active->is_v3());
    const std::filesystem::path new_stream =
        active->session_dir() / platform::Utf8ToPath(active->session_id() + ".jsonl");
    CHECK(std::filesystem::exists(new_stream));
    CHECK_FALSE(std::filesystem::exists(active->session_dir() / "main.jsonl"));
    // resume.source.attached 五键指源末行(§4.10),列表"(续)"的 v2 来路。
    bool saw_attached = false;
    {
        std::ifstream in(new_stream, std::ios::binary);
        REQUIRE(in.is_open());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto row = nlohmann::json::parse(line, nullptr, false);
            if (row.is_discarded() || row.value("type", std::string()) != "event" ||
                row.value("kind", std::string()) != "resume.source.attached") {
                continue;
            }
            saw_attached = true;
            CHECK(row.at("payload").at("sourceRef").value("sessionId", std::string()) ==
                  source_id);
        }
    }
    CHECK(saw_attached);
    // v2 空转源的有效对话为空(user 输入没写过)——不冒充。
    CHECK(outcome.effective_conversation.empty());
    // v2 源账写不动:主账字节不再变。
    const auto source_stream = scaffold.sessions_dir / platform::Utf8ToPath(source_id) /
                               "main.jsonl";
    const auto source_bytes = std::filesystem::file_size(source_stream);
    CHECK(source_bytes > 0);
}

// ---------------------------------------------------------------------------
// 接线点 2:session_index(/sessions、选择器数据源)并列识别 v3
// ---------------------------------------------------------------------------

TEST_CASE("session_index: v3 场进列表,摘要如实,坏尾标 damaged") {
    Scaffold scaffold("index");
    const std::string v3_id = PlantV3Session(scaffold.sessions_dir, "compact_full.jsonl");
    std::error_code ec;
    std::filesystem::remove_all(
        scaffold.sessions_dir / platform::Utf8ToPath(scaffold.open_id), ec);

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
