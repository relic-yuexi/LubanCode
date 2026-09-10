// 轨迹 v3 D2 修复:resume-as-new 沿 resume.source.attached 链折算(§4.10
// 第 3-4 条)。本册钉链遍历语义:
//   - 两跳/三跳:祖先场历史全部进新场模型上下文(LaunchResumeHistory
//     链序 = 最老祖先 → 直接源),跨场完整来源键去重,不重复不丢史;
//   - 链自足:折出的史以来源键("<场>/<msgId>")抄进新账并接纳进链,
//     首笔请求 prepared.inputMessageRefs 与链逐位一致(修前 1 比 5);
//   - 单源照旧:无祖先时对话键/事件 id 用裸键,现行形状不惊动;
//   - 显示侧:RestoredHistoryView 画链上祖先(时间线原序,不重复);
//   - 护栏:链上祖先缺失/回环 → resume.source_chain_broken 精确恢复拒绝。
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
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
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
    options.lubancode_version = "0.26.247-test";
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

// 开场(可 resume)→ 一轮问答 → 封场。回这场 id。
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

}  // namespace

// ---------------------------------------------------------------------------
// 两跳链:A → B → C。祖先史进新场、抄本落账进链、prepared 与链一致。
// ---------------------------------------------------------------------------

TEST_CASE("两跳链: 祖先史进新场上下文,抄本进链,prepared 与链逐位一致") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("two-hop");

    // A 场:一轮问答。
    const std::string a_id = RunRoundTrip(root, "A 场第一句", "A 场回答");
    // B 场:--continue 式 resume(显式指 A)。
    const std::string b_id = RunRoundTrip(root, "B 场第一句", "B 场回答", a_id);

    // B 折算的有效对话 = A 史(§4.10 第 3 条:祖先场历史进新场模型上下文)。
    // 顺路再开一场 resume 自 A 的对照场读 LaunchResumeHistory。
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, a_id));
        REQUIRE(ledger.has_value());
        const std::vector<std::string> texts = HistoryTexts(ledger->LaunchResumeHistory());
        CHECK(texts == std::vector<std::string>{"A 场第一句", "A 场回答"});
    }

    // B 账:resume.source.attached 之后落 A 史抄本(来源键 messageId),
    // 一条 context.input.applied 批量接纳;assistant 抄本不复制 usage
    //(唯一 owner 在原场,§4.12):键必现、值 null。
    const auto b_rows = ReadLines(StreamOf(root, b_id));
    const auto b_imports = ImportRows(b_rows);
    REQUIRE(b_imports.size() == 2);
    for (const auto& row : b_imports) {
        const std::string key = row.value("messageId", std::string());
        CHECK(row.value("sourceMessageRef", std::string()) == key);
        CHECK(key.rfind(a_id + "/", 0) == 0);  // 来源键指 A 场
    }
    CHECK(MessageText(b_imports[0]) == "A 场第一句");
    CHECK(MessageText(b_imports[1]) == "A 场回答");
    CHECK(b_imports[1].contains("usage"));
    CHECK(b_imports[1].at("usage").is_null());
    CHECK(b_imports[1].value("provider", std::string()) == "moonshot");
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, b_id)).ok);

    // B 自己那轮的 prepared:链 = A 抄本×2 + B 新输入 → 3 枚,与实发同拍。
    const auto b_prepared = RowsOf(b_rows, "event", "model.request.prepared");
    REQUIRE(b_prepared.size() == 1);
    const auto& b_refs = b_prepared[0].at("payload").at("inputMessageRefs");
    REQUIRE(b_refs.is_array());
    REQUIRE(b_refs.size() == 3);
    CHECK(b_refs[0].get<std::string>().rfind(a_id + "/", 0) == 0);
    CHECK(b_refs[1].get<std::string>().rfind(a_id + "/", 0) == 0);
    CHECK(b_refs[2].get<std::string>().find('/') == std::string::npos);  // 本场新输入裸键

    // C 场:resume 自 B(两跳)。折算 = A 原 + B 新,不重不漏——B 账上的
    // A 抄本让位给 A 原装(跨场完整来源键去重)。
    const std::string c_id = RunRoundTrip(root, "C 场第一句", "C 场回答", b_id);
    const auto c_rows = ReadLines(StreamOf(root, c_id));
    const auto c_imports = ImportRows(c_rows);
    REQUIRE(c_imports.size() == 4);  // A×2 + B×2;B 账里的 A 抄本不重复折
    CHECK(MessageText(c_imports[0]) == "A 场第一句");
    CHECK(MessageText(c_imports[1]) == "A 场回答");
    CHECK(MessageText(c_imports[2]) == "B 场第一句");
    CHECK(MessageText(c_imports[3]) == "B 场回答");
    CHECK(c_imports[0].value("messageId", std::string()).rfind(a_id + "/", 0) == 0);
    CHECK(c_imports[2].value("messageId", std::string()).rfind(b_id + "/", 0) == 0);
    // C 首笔 prepared:4 抄本 + C 新输入 = 5 枚(账实一致;D2 修前 1 比 5)。
    const auto c_prepared = RowsOf(c_rows, "event", "model.request.prepared");
    REQUIRE(c_prepared.size() == 1);
    CHECK(c_prepared[0].at("payload").at("inputMessageRefs").size() == 5);
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, c_id)).ok);

    // 显示侧(§4.10"多次 resume 沿源链读取……别重复显示同一祖先"):
    // resume 自 B 的新场,开场重放的 RestoredHistoryView 画 B(直接源)+
    // A(祖先),各恰一次;来源场名最老在前。
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, b_id));
        REQUIRE(ledger.has_value());
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
        CHECK(seen["B 场第一句"] == 1);
        CHECK(seen["B 场回答"] == 1);
    }
}

// ---------------------------------------------------------------------------
// 三跳链:A → B → C → D。跨场去重幂等,祖先史一路全携。
// ---------------------------------------------------------------------------

TEST_CASE("三跳链: D 场上下文含 A/B/C 全史,去重幂等") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("three-hop");
    const std::string a_id = RunRoundTrip(root, "A 问", "A 答");
    const std::string b_id = RunRoundTrip(root, "B 问", "B 答", a_id);
    const std::string c_id = RunRoundTrip(root, "C 问", "C 答", b_id);
    const std::string d_id = RunRoundTrip(root, "D 问", "D 答", c_id);

    const auto d_rows = ReadLines(StreamOf(root, d_id));
    const auto d_imports = ImportRows(d_rows);
    REQUIRE(d_imports.size() == 6);  // A×2 + B×2 + C×2
    const std::vector<std::string> expected_order = {
        "A 问", "A 答", "B 问", "B 答", "C 问", "C 答"};
    for (std::size_t i = 0; i < d_imports.size(); ++i) {
        CHECK(MessageText(d_imports[i]) == expected_order[i]);
    }
    const auto d_prepared = RowsOf(d_rows, "event", "model.request.prepared");
    REQUIRE(d_prepared.size() == 1);
    CHECK(d_prepared[0].at("payload").at("inputMessageRefs").size() == 7);
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, d_id)).ok);
}

// ---------------------------------------------------------------------------
// 单源照旧:无祖先时对话键/事件 id 用裸键(现行 wire 形状不惊动)。
// ---------------------------------------------------------------------------

TEST_CASE("单源 resume: 有效对话照旧,抄本照落(链自足与单/多源无关)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("single-source");
    const std::string a_id = RunRoundTrip(root, "单源问", "单源答");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, a_id));
    REQUIRE(ledger.has_value());
    CHECK(HistoryTexts(ledger->LaunchResumeHistory()) ==
          std::vector<std::string>{"单源问", "单源答"});
    const std::string new_id = ledger->session_id();
    CHECK(new_id != a_id);
    const auto rows = ReadLines(StreamOf(root, new_id));
    CHECK(ImportRows(rows).size() == 2);
    CHECK(lubancode::trajectory::v3::VerifyV3File(StreamOf(root, new_id)).ok);
}

// ---------------------------------------------------------------------------
// 护栏:链上祖先缺失 / 回环 → 精确恢复拒绝(§4.10"源缺失时……精确
// 上下文恢复应拒绝")。走 SessionManager 直查,不经 ledger 启动路的
// "失败回落普通开张"。
// ---------------------------------------------------------------------------

TEST_CASE("链护栏: 祖先缺失 → resume.source_chain_broken,不开新场") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("guard-missing");
    const std::string a_id = RunRoundTrip(root, "甲问", "甲答");
    const std::string b_id = RunRoundTrip(root, "乙问", "乙答", a_id);

    std::error_code ec;
    std::filesystem::remove_all(StreamOf(root, a_id).parent_path(), ec);
    trajectory::SessionManager manager(ManagerOptionsOf(LedgerOptions(root)));
    trajectory::ResumeRequest request;
    request.source_session_id = b_id;
    const auto outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_chain_broken");
    CHECK(manager.active() == nullptr);  // 拒了就不开新场
}

TEST_CASE("链护栏: 回环 → resume.source_chain_broken,不开新场") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("guard-cycle");
    const std::string a_id = RunRoundTrip(root, "甲问", "甲答");
    const std::string b_id = RunRoundTrip(root, "乙问", "乙答", a_id);
    const std::string c_id = RunRoundTrip(root, "丙问", "丙答", b_id);

    // 人为给 A 补一枚指向 C 的 attached:链 C → B → A → C 回环。
    {
        auto continued = lubancode::trajectory::v3::V3Writer::Continue(StreamOf(root, a_id));
        REQUIRE(continued.has_value());
        const auto c_ledger = lubancode::trajectory::v3::ReadV3Ledger(StreamOf(root, c_id));
        REQUIRE(c_ledger.has_value());
        const auto c_last = c_ledger->LastEntry();
        REQUIRE(c_last.has_value());
        lubancode::trajectory::v3::EventDraft attached;
        attached.kind = lubancode::trajectory::v3::EventKindV3::ResumeSourceAttached;
        attached.payload = nlohmann::json{
            {"sourceRef", nlohmann::json{{"sessionId", c_id},
                                         {"runId", c_ledger->run_id},
                                         {"seq", c_last->seq},
                                         {"id", c_last->is_message
                                                    ? c_ledger->messages[c_last->index].message_id
                                                    : c_ledger->events[c_last->index].event_id},
                                         {"hash", c_last->is_message
                                                      ? c_ledger->messages[c_last->index].line_hash
                                                      : c_ledger->events[c_last->index].line_hash}}}};
        REQUIRE(continued->AppendEvent(std::move(attached), trajectory::Durability::PowerLoss)
                    .status == lubancode::trajectory::v3::WriteReceipt::Status::Committed);
    }
    trajectory::SessionManager manager(ManagerOptionsOf(LedgerOptions(root)));
    trajectory::ResumeRequest request;
    request.source_session_id = c_id;
    const auto outcome = manager.ResumeAsNew(request);
    CHECK(outcome.error_code == "resume.source_chain_broken");
    CHECK(manager.active() == nullptr);
}
