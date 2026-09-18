// 轨迹 v3 resume 链语义(2026-09-19 落点拍板后续接源场:同 id 续写,不
// 开新账、不抄链、列表不新增条目)。本册钉三件事:
//   - 单源续接:--continue/交互 resume 都续源场,LaunchResumeHistory 链
//     序完整,首笔请求 prepared.inputMessageRefs 与本账链逐位一致(裸键,
//     无跨场抄本);
//   - 存量 fork 档(旧 resume-as-new 落下的账:resume.source.attached 五键
//     指源 + 祖先史抄本进链)续接照常:FoldV3ResumeChain 沿链折算,祖先
//     原装按完整来源键去重,不重复不丢史;续写只落本场,不添新抄本;
//   - 护栏:链上祖先缺失/回环 → resume.source_chain_broken 精确恢复拒绝。
// 多跳去重幂等的读面(来源链遍历/去重/深度帽)另册钉着
// (test_v3_reader_resume.cpp),本册只管 session_manager 的折算接线。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_history_view.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
namespace v3 = lubancode::trajectory::v3;
using namespace lubancode;
using lubancode::runtime::RestoredHistoryItem;
using lubancode::runtime::TrajectorySessionLedger;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-resume-chain-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root,
                                               const std::string& resume_source = {}) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.278-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    if (!resume_source.empty()) {
        options.resume_at_launch = true;
        options.resume_source_session_id = resume_source;
    }
    return options;
}

trajectory::SessionManagerOptions ManagerOptionsOf(
    const TrajectorySessionLedger::Options& options) {
    trajectory::SessionManagerOptions manager_options;
    manager_options.workspaces_root = options.workspaces_root;
    manager_options.identity = options.workspace_identity;
    manager_options.workspace_root = options.workspace_root;
    manager_options.launch_cwd = options.launch_cwd;
    manager_options.lubancode_version = options.lubancode_version;
    manager_options.v3_system_content = options.v3_system_content;
    return manager_options;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Request MakeRequest(const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = "你是 LubanCode,读写跑都走工具。";
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

// 开场(可续接源场)→ 一轮问答 → 封场。回这场 id。续接语义下带
// resume_source 时返回的就是源场 id(同场续写)。
std::string RunRoundTrip(const std::filesystem::path& root, const std::string& user_text,
                         const std::string& assistant_text, const std::string& resume_source = {}) {
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, resume_source));
    REQUIRE(ledger.has_value());
    const std::string session_id = ledger->session_id();
    {
        auto bridge =
            ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage(user_text));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest({UserMessage(user_text)}), PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText(assistant_text), "end_turn",
                                          "resp-1"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    return session_id;
}

std::filesystem::path StreamOf(const std::filesystem::path& root, const std::string& session_id) {
    std::error_code ec;
    for (const auto& room : std::filesystem::directory_iterator(root / "workspaces", ec)) {
        const auto dir = room.path() / "sessions" / platform::Utf8ToPath(session_id);
        if (std::filesystem::exists(dir, ec)) {
            return dir / platform::Utf8ToPath(session_id + ".jsonl");
        }
    }
    return {};
}

std::vector<nlohmann::json> ReadLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

std::string MessageText(const nlohmann::json& row) {
    if (!row.contains("message")) {
        return {};
    }
    const auto& content = row.at("message").value("content", nlohmann::json());
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (content.is_array()) {
        for (const auto& part : content) {
            if (part.is_object() && part.value("type", std::string()) == "text") {
                return part.value("text", std::string());
            }
        }
    }
    return {};
}

std::vector<nlohmann::json> RowsOf(const std::vector<nlohmann::json>& rows, const char* type,
                                   const char* kind = nullptr) {
    std::vector<nlohmann::json> found;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) != type) {
            continue;
        }
        if (kind != nullptr && row.value("kind", std::string()) != kind) {
            continue;
        }
        found.push_back(row);
    }
    return found;
}

// 一场账里链上(messageId 带来源键 "/")的抄本行。
std::vector<nlohmann::json> ImportRows(const std::vector<nlohmann::json>& rows) {
    std::vector<nlohmann::json> imports;
    for (const auto& row : RowsOf(rows, "message")) {
        const std::string id = row.value("messageId", std::string());
        if (id.find('/') != std::string::npos) {
            imports.push_back(row);
        }
    }
    return imports;
}

// 折算出的有效对话文本(LaunchResumeHistory → api::Message 文本块)。
std::vector<std::string> HistoryTexts(const std::vector<api::Message>& history) {
    std::vector<std::string> texts;
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                texts.push_back(text->text);
            }
        }
    }
    return texts;
}

std::size_t CountSessions(const std::filesystem::path& root) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& room : std::filesystem::directory_iterator(root / "workspaces", ec)) {
        const auto sessions = room.path() / "sessions";
        if (!std::filesystem::exists(sessions, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(sessions, ec)) {
            if (entry.is_directory(ec)) {
                ++count;
            }
        }
    }
    return count;
}

// 手工铸一枚"存量 fork 档"(旧 resume-as-new 落下的形状):resume.source.
// attached 五键指源末行 + 源场有效对话的抄本进链 + 一句本场新话。
// 2026-09-19 落点改续接后新账不再长这样,但盘上存量照旧要能续能读。
// 抄本键带来源场名("<源>/<msgId>"),与折算/去重口径同形。
std::string PlantForkSession(const std::filesystem::path& root, const std::string& fork_id,
                             const std::string& source_id, const std::string& own_text) {
    const std::filesystem::path source_stream = StreamOf(root, source_id);
    REQUIRE_FALSE(source_stream.empty());
    const auto source_rows = ReadLines(source_stream);
    REQUIRE_FALSE(source_rows.empty());
    const nlohmann::json& last = source_rows.back();
    nlohmann::json source_ref = nlohmann::json::object(
        {{"sessionId", source_id},
         {"runId", last.value("runId", std::string())},
         {"seq", last.value("seq", std::uint64_t(0))},
         {"id", last.value("eventId", last.value("messageId", std::string()))},
         {"hash", last.value("lineHash", std::string())}});
    const std::filesystem::path dir = source_stream.parent_path().parent_path() /
                                      platform::Utf8ToPath(fork_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    auto writer = v3::V3Writer::Start(
        dir / platform::Utf8ToPath(fork_id + ".jsonl"), fork_id, "run-000009",
        "你是 LubanCode,读写跑都走工具。");
    REQUIRE(writer.has_value());
    v3::EventDraft attached;
    attached.kind = v3::EventKindV3::ResumeSourceAttached;
    attached.payload = nlohmann::json{{"sourceRef", std::move(source_ref)}};
    REQUIRE(writer->AppendEvent(std::move(attached), trajectory::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
    // 源场有效对话抄本(system 行不算)进链;usage 不复制(缺实报 null)。
    std::vector<std::string> admitted;
    for (const auto& row : source_rows) {
        if (row.value("type", std::string()) != "message" ||
            row.value("purpose", std::string()) != "conversation") {
            continue;
        }
        const auto& body = row.at("message");
        if (body.value("role", std::string()) == "system") {
            continue;
        }
        v3::MessageDraft draft;
        // 抄本键:祖源转抄保原键(与生产折算同形)——源行自身已是抄本
        //(带 sourceMessageRef)时沿用其键,不套两层场名;原生行拼
        // "<源>/<msgId>"。
        const std::string key =
            (row.contains("sourceMessageRef") && row["sourceMessageRef"].is_string())
                ? row["sourceMessageRef"].get<std::string>()
                : source_id + "/" + row.value("messageId", std::string());
        draft.message_id_override = key;
        draft.purpose = v3::MessagePurpose::Conversation;
        draft.origin = v3::MessageOriginFromName(row.value("origin", std::string()))
                           .value_or(v3::MessageOrigin::Human);
        draft.message = body;
        draft.source_message_ref = key;
        // 信封必填件照抄(与生产 fork 抄本同形):user/assistant 的 turnId、
        // assistant 的 requestId 与 provider/wire/model/responseModel/usage。
        if (row.contains("turnId") && row["turnId"].is_string()) {
            draft.turn_id = row["turnId"].get<std::string>();
        }
        if (row.contains("requestId") && row["requestId"].is_string()) {
            draft.request_id = row["requestId"].get<std::string>();
        }
        if (row.contains("provider") && row["provider"].is_string()) {
            draft.provider = row["provider"].get<std::string>();
        }
        if (row.contains("wire") && row["wire"].is_string()) {
            draft.wire = row["wire"].get<std::string>();
        }
        if (row.contains("model") && row["model"].is_string()) {
            draft.model = row["model"].get<std::string>();
        }
        if (row.contains("responseModel")) {
            draft.response_model = row["responseModel"];
        }
        if (body.value("role", std::string()) == "assistant") {
            // usage 唯一 owner 在原场(§4.12):抄本不复制,键必现值 null。
            draft.usage = nlohmann::json(nullptr);
        }
        const auto receipt =
            writer->AppendMessage(std::move(draft), trajectory::Durability::PowerLoss);
        REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
        admitted.push_back(receipt.id);
    }
    v3::MessageDraft own;
    own.turn_id = "turn-000001";
    own.purpose = v3::MessagePurpose::Conversation;
    own.origin = v3::MessageOrigin::Human;
    own.message = nlohmann::json::object({{"role", "user"}, {"content", own_text}});
    const auto own_receipt =
        writer->AppendMessage(std::move(own), trajectory::Durability::PowerLoss);
    REQUIRE(own_receipt.status == v3::WriteReceipt::Status::Committed);
    admitted.push_back(own_receipt.id);
    REQUIRE(writer->AdmitMessages(admitted, trajectory::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
    return fork_id;
}

}  // namespace

// ---------------------------------------------------------------------------
// 单源续接:resume 不再 fork,源场同 id 续写;链投影完整;账实一致。
// ---------------------------------------------------------------------------

TEST_CASE("单源续接: 同 id 续写,史完整,prepared 与链逐位一致") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("single-source");
    const std::string a_id = RunRoundTrip(root, "单源问", "单源答");
    const std::size_t sessions_before = CountSessions(root);
    const std::size_t lines_before = ReadLines(StreamOf(root, a_id)).size();

    // --continue 续接源场:session_id 不换,有效对话从本账链折回。
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, a_id));
        REQUIRE(ledger.has_value());
        CHECK(ledger->session_id() == a_id);  // 续接源场,不另开新场
        CHECK(ledger->resumed_at_launch());
        CHECK(HistoryTexts(ledger->LaunchResumeHistory()) ==
              std::vector<std::string>{"单源问", "单源答"});
        CHECK(ledger->LaunchRestoredHistoryView().has_value());
        // 续写一轮:新话落本场账尾,prepared 引用本账链(裸键,无抄本)。
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-2", "external_user");  // 源账已有 turn-1,续写续号
        bridge->RecordInput(UserMessage("单源再问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest({UserMessage("单源问"), AssistantText("单源答"),
                                                    UserMessage("单源再问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("单源再答"), "end_turn",
                                          "resp-1"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }

    // 列表条目不增;账继续长;没有跨场抄本;prepared 与链逐位一致。
    CHECK(CountSessions(root) == sessions_before);
    const auto rows = ReadLines(StreamOf(root, a_id));
    CHECK(rows.size() > lines_before);
    CHECK(ImportRows(rows).empty());
    const auto prepared = RowsOf(rows, "event", "model.request.prepared");
    REQUIRE(prepared.size() == 2);  // 续接前的首轮 + 续接后的新轮
    const auto& refs = prepared.back().at("payload").at("inputMessageRefs");
    REQUIRE(refs.is_array());
    REQUIRE(refs.size() == 3);
    for (const auto& ref : refs) {
        CHECK(ref.get<std::string>().find('/') == std::string::npos);  // 本账裸键
    }
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, a_id)).ok);
}

// ---------------------------------------------------------------------------
// 存量 fork 档续接:祖先原装按完整来源键去重,史不丢不重;续写不添抄本。
// ---------------------------------------------------------------------------

TEST_CASE("存量 fork 档续接: 祖先原装去重,史不丢不重,续写不抄链") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("legacy-fork");
    const std::string a_id = RunRoundTrip(root, "A 场第一句", "A 场回答");
    const std::string b_id =
        PlantForkSession(root, "20260919-000001-FORKB", a_id, "B 场自己的话");
    const std::size_t sessions_before = CountSessions(root);
    const std::size_t lines_before = ReadLines(StreamOf(root, b_id)).size();

    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, b_id));
    REQUIRE(ledger.has_value());
    CHECK(ledger->session_id() == b_id);  // 续接的是 B 自己,不是开新场
    // 折算 = A 原装(2)+ B 自己(1);B 账上的 A 抄本让位原装,不重不漏。
    CHECK(HistoryTexts(ledger->LaunchResumeHistory()) ==
          std::vector<std::string>{"A 场第一句", "A 场回答", "B 场自己的话"});
    // 显示侧:来源链 B → A 各画一次,最老祖先在前。
    const auto view = ledger->LaunchRestoredHistoryView();
    REQUIRE(view.has_value());
    REQUIRE(view->source_sessions.size() == 2);
    CHECK(view->source_sessions[0] == a_id);
    CHECK(view->source_sessions[1] == b_id);
    std::map<std::string, int> seen;
    for (const auto& item : view->items) {
        if (item.kind != RestoredHistoryItem::Kind::Message) {
            continue;
        }
        for (const auto& block : item.message.message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                ++seen[text->text];
            }
        }
    }
    CHECK(seen["A 场第一句"] == 1);
    CHECK(seen["A 场回答"] == 1);
    CHECK(seen["B 场自己的话"] == 1);

    // 续写一轮:prepared 引用 B 链(2 抄本 + 1 本场 + 新输入),续接不再
    // 添新抄本(ImportRows 数不变——这是"不抄链"的账面证据)。
    const std::size_t imports_before = ImportRows(ReadLines(StreamOf(root, b_id))).size();
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-2", "external_user");  // B 账已有 turn-1,续写续号
        bridge->RecordInput(UserMessage("B 场续接问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest({UserMessage("A 场第一句"),
                                                   AssistantText("A 场回答"),
                                                   UserMessage("B 场自己的话"),
                                                   UserMessage("B 场续接问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("B 场续接答"), "end_turn",
                                          "resp-1"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }
    CHECK(CountSessions(root) == sessions_before);
    const auto rows = ReadLines(StreamOf(root, b_id));
    CHECK(rows.size() > lines_before);
    CHECK(ImportRows(rows).size() == imports_before);  // 续接零新抄本
    const auto prepared = RowsOf(rows, "event", "model.request.prepared");
    REQUIRE(prepared.size() == 1);
    const auto& refs = prepared[0].at("payload").at("inputMessageRefs");
    REQUIRE(refs.is_array());
    REQUIRE(refs.size() == 4);  // A 抄本×2 + B 场自己 + 新输入
    CHECK(refs[0].get<std::string>().rfind(a_id + "/", 0) == 0);
    CHECK(refs[1].get<std::string>().rfind(a_id + "/", 0) == 0);
    CHECK(refs[2].get<std::string>().find('/') == std::string::npos);
    CHECK(refs[3].get<std::string>().find('/') == std::string::npos);
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, b_id)).ok);
}

TEST_CASE("三跳存量链: 折算幂等,祖先史一路全携,续写只落本场") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("three-hop");
    const std::string a_id = RunRoundTrip(root, "A 问", "A 答");
    const std::string b_id =
        PlantForkSession(root, "20260919-000002-FORKB", a_id, "B 问");
    const std::string c_id =
        PlantForkSession(root, "20260919-000003-FORKC", b_id, "C 问");
    const std::size_t sessions_before = CountSessions(root);

    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, c_id));
    REQUIRE(ledger.has_value());
    CHECK(ledger->session_id() == c_id);
    // C 折算 = A 原 + B 自己 + C 自己(B 账上的 A 抄本让位 A 原装,各一次)。
    CHECK(HistoryTexts(ledger->LaunchResumeHistory()) ==
          std::vector<std::string>{"A 问", "A 答", "B 问", "C 问"});
    const auto view = ledger->LaunchRestoredHistoryView();
    REQUIRE(view.has_value());
    REQUIRE(view->source_sessions.size() == 3);
    CHECK(view->source_sessions[0] == a_id);
    CHECK(view->source_sessions[1] == b_id);
    CHECK(view->source_sessions[2] == c_id);
    // 续接不堆场:列表条目与账本身份都不增。
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    CHECK(CountSessions(root) == sessions_before);
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, c_id)).ok);
}

// ---------------------------------------------------------------------------
// 护栏:链上祖先缺失 / 回环 → 精确恢复拒绝(§4.10"源缺失时……精确
// 上下文恢复应拒绝")。走 SessionManager 直查,不经 ledger 启动路的
// "失败回落普通开张"。
// ---------------------------------------------------------------------------

TEST_CASE("链护栏: 祖先缺失 → resume.source_chain_broken,不续接") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("guard-missing");
    const std::string a_id = RunRoundTrip(root, "甲问", "甲答");
    const std::string b_id =
        PlantForkSession(root, "20260919-000004-FORKB", a_id, "乙问");
    const std::string before = [&] {
        std::ifstream file(StreamOf(root, b_id), std::ios::binary);
        REQUIRE(file.is_open());
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }();

    std::error_code ec;
    std::filesystem::remove_all(StreamOf(root, a_id).parent_path(), ec);
    trajectory::SessionManager manager(ManagerOptionsOf(LedgerOptions(root)));
    trajectory::ResumeRequest request;
    request.source_session_id = b_id;
    const auto outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_chain_broken");
    CHECK(manager.active() == nullptr);  // 拒了就不续接、不开场
    // 拒续接不动源账一个字节(append-only 底线的拒绝面)。
    std::ifstream after_file(StreamOf(root, b_id), std::ios::binary);
    REQUIRE(after_file.is_open());
    const std::string after((std::istreambuf_iterator<char>(after_file)),
                            std::istreambuf_iterator<char>());
    CHECK(after == before);
}

TEST_CASE("链护栏: 回环 → resume.source_chain_broken,不续接") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("guard-cycle");
    const std::string a_id = RunRoundTrip(root, "甲问", "甲答");
    const std::string b_id =
        PlantForkSession(root, "20260919-000005-FORKB", a_id, "乙问");
    // 把 A 也接上指向 B 的 attached(V3Writer::Continue 续写):A → B → A 回环。
    {
        const auto a_rows = ReadLines(StreamOf(root, a_id));
        const nlohmann::json& last = a_rows.back();
        nlohmann::json source_ref = nlohmann::json::object(
            {{"sessionId", b_id},
             {"runId", last.value("runId", std::string())},
             {"seq", last.value("seq", std::uint64_t(0))},
             {"id", last.value("eventId", last.value("messageId", std::string()))},
             {"hash", last.value("lineHash", std::string())}});
        auto writer = v3::V3Writer::Continue(StreamOf(root, a_id));
        REQUIRE(writer.has_value());
        v3::EventDraft attached;
        attached.kind = v3::EventKindV3::ResumeSourceAttached;
        attached.payload = nlohmann::json{{"sourceRef", std::move(source_ref)}};
        REQUIRE(writer->AppendEvent(std::move(attached), trajectory::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
    }

    trajectory::SessionManager manager(ManagerOptionsOf(LedgerOptions(root)));
    trajectory::ResumeRequest request;
    request.source_session_id = b_id;
    const auto outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_chain_broken");
    CHECK(manager.active() == nullptr);
}
