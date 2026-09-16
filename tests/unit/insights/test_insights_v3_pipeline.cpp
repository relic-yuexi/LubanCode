// Insights 接入 Session v3(T14/细化单 I2/I3):integrity_gate 分派、领域
// 读模型、prompt runtime v3 规则、摩擦 v3 分类、session 分析与去重的
// 验收册。夹具一律经 V3Writer 现场生成(与写者同口径);错误形状(坏尾/
// 坏中段/缺 blob/缺子账)在合法账上注入——writer 造不出错误账,恰好避免
// writer/reader 同一个错误互相自证(T00 夹具纪律)。
//
// 验收口径(细化单 §五/清理单 T14):
//   - 坏来源/缺 blob/不完整请求标 partial,不跳整场、不记零风险;
//   - prompt 审计吃 prepared 持久请求视图(引用/revision/toolNames/
//     inputView),不从聊天显示倒推;R01 不对 v3 误报;
//   - 摩擦从 v3 工具/请求事实取材;无审批/验证事实的类别不判分;
//   - resume/父子链在同一聚合范围只算一次(T06 范围);
//   - evidence 锚 event_id+seq;同一封口 session 重算字节稳定;
//   - 红action allowlist 认得新键(format/limitations/seq)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "insights/finding.hpp"
#include "insights/friction_classifier.hpp"
#include "insights/integrity_gate.hpp"
#include "insights/prompt_auditor.hpp"
#include "insights/redaction.hpp"
#include "insights/report_model.hpp"
#include "insights/session_analyzer.hpp"
#include "insights/session_summary.hpp"
#include "insights/v3_facts.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::insights;
using namespace lubancode::trajectory::v3;

namespace {

// 环境变量门卫:构造置值,析构还原(不漏开关状态给别的册)。本册夹具
// 直写 V3Writer 不经建场开关,EnvGuard 是防御性的(册内任何路径都不该
// 因全局默认 v2 而改变行为)。
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

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct SessionsRoot {
    FixedClock clock;
    std::filesystem::path root;

    explicit SessionsRoot(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-insights-v3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    std::filesystem::path Dir(const std::string& session_id) {
        std::error_code ec;
        const std::filesystem::path dir = root / session_id;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path Ledger(const std::string& session_id) {
        return Dir(session_id) / (session_id + ".jsonl");
    }
};

std::optional<V3Writer> StartSession(SessionsRoot& root, const std::string& session_id) {
    auto writer = V3Writer::Start(root.Ledger(session_id), session_id, "run-" + session_id,
                                  "你是 LubanCode。", nlohmann::json::object(),
                                  V3WriterOptions{}, &root.clock);
    if (!writer.has_value()) {
        return std::nullopt;
    }
    return std::move(*writer);
}

// prepared 事件的 provider_snapshot:与生产写口(trajectory_session.cpp
// V3RequestPrepared)同款合同——provider/wire/model/toolNames/inputView。
nlohmann::json SnapshotOf(const char* provider, const char* wire, const char* model,
                          const std::vector<std::string>& tool_names,
                          std::optional<std::uint64_t> wire_message_count,
                          std::size_t chain_refs) {
    nlohmann::json snapshot = nlohmann::json::object({{"provider", provider},
                                                      {"wire", wire},
                                                      {"model", model}});
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& name : tool_names) {
        tools.push_back(name);
    }
    snapshot["toolNames"] = std::move(tools);
    if (wire_message_count.has_value()) {
        snapshot["inputView"] = nlohmann::json::object(
            {{"messageCount", *wire_message_count},
             {"chainRefCount", chain_refs},
             {"divergent", *wire_message_count != chain_refs},
             {"messageFingerprints", nlohmann::json::array()},
             {"systemFingerprint", "fp-system-0001"}});
    }
    return snapshot;
}

// 一轮带模型请求的对话:user -> prepared(toolNames/视图账) -> assistant
// (usage owner)。返回 request_id。
std::string InstallRequestedRound(V3Writer& writer, const std::string& turn_id,
                                  const std::string& user_text,
                                  const std::vector<std::string>& tool_names,
                                  std::optional<nlohmann::json> usage,
                                  std::optional<std::uint64_t> wire_message_count,
                                  const std::string& purpose = "conversation") {
    MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", user_text}});
    const WriteReceipt user_receipt =
        writer.AppendMessage(std::move(user), Durability::PowerLoss);
    REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status == WriteReceipt::Status::Committed);

    const std::string request_id = writer.NewRequestId();
    const std::string step_id = writer.NewStepId();
    std::vector<std::string> input_refs;
    for (const auto& node : writer.context().chain) {
        input_refs.push_back(node.message_ref);
    }
    if (!input_refs.empty()) {
        input_refs.erase(input_refs.begin());  // 去掉根 system
    }
    const auto prepared = writer.PrepareRequest(
        request_id, turn_id, step_id, purpose, writer.context().system_message_ref,
        input_refs,
        SnapshotOf("stub", "openai", "stub-mini", tool_names, wire_message_count,
                   input_refs.size()),
        std::nullopt, Durability::PowerLoss);
    REQUIRE(prepared.status == WriteReceipt::Status::Committed);
    const auto sent = writer.AppendEvent(
        EventDraft{.kind = EventKindV3::ModelRequestSent,
                   .status = OpStatus::Done,
                   .turn_id = turn_id,
                   .step_id = step_id,
                   .request_id = request_id},
        Durability::PowerLoss);
    REQUIRE(sent.status == WriteReceipt::Status::Committed);
    // 响应终态(与生产 CompleteStreamResponse 同族:completed 带身份;
    // 夹具不流式,直接落终态)。
    const auto completed = writer.AppendEvent(
        EventDraft{.kind = EventKindV3::ModelResponseCompleted,
                   .status = OpStatus::Done,
                   .turn_id = turn_id,
                   .step_id = step_id,
                   .request_id = request_id},
        Durability::PowerLoss);
    REQUIRE(completed.status == WriteReceipt::Status::Committed);

    MessageDraft assistant;
    assistant.turn_id = turn_id;
    assistant.step_id = step_id;
    assistant.request_id = request_id;
    assistant.purpose = MessagePurpose::Conversation;
    assistant.origin = MessageOrigin::SessionRuntime;
    assistant.provider = "stub";
    assistant.wire = "openai";
    assistant.model = "stub-mini";
    assistant.response_model = nlohmann::json(nullptr);
    assistant.usage = usage.has_value() ? std::optional<nlohmann::json>(*usage)
                                        : std::optional<nlohmann::json>(
                                              nlohmann::json(nullptr));
    assistant.message = nlohmann::json::object({{"role", "assistant"},
                                                {"content", "收到:" + user_text}});
    const WriteReceipt assistant_receipt =
        writer.AppendMessage(std::move(assistant), Durability::PowerLoss);
    REQUIRE(assistant_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({assistant_receipt.id}).status ==
            WriteReceipt::Status::Committed);
    return request_id;
}

// 封口:session.ended(v3 封口唯一事实,与生产 Finish 同款载荷)。
void Seal(V3Writer& writer, const char* reason = "completed") {
    EventDraft ended;
    ended.kind = EventKindV3::SessionEnded;
    ended.payload = nlohmann::json{{"reason", reason}, {"closeQuality", "clean"}};
    REQUIRE(writer.AppendEvent(std::move(ended), Durability::PowerLoss).status ==
            WriteReceipt::Status::Committed);
}

// 一枚带声明块与失败终态的工具(声明块在 assistant 的 tool_call 块里,
// declared_args 由折叠快照从那里取)。
std::string InstallFailingTool(V3Writer& writer, const std::string& turn_id,
                               const std::string& action_id, const std::string& tool_name,
                               const std::string& error_text,
                               const nlohmann::json& args = nlohmann::json::object()) {
    MessageDraft assistant;
    assistant.turn_id = turn_id;
    assistant.step_id = "step-" + turn_id;
    assistant.request_id = "request-" + turn_id;
    assistant.purpose = MessagePurpose::Conversation;
    assistant.origin = MessageOrigin::SessionRuntime;
    assistant.provider = "stub";
    assistant.wire = "openai";
    assistant.model = "stub-mini";
    assistant.response_model = nlohmann::json(nullptr);
    assistant.usage = nlohmann::json(nullptr);
    assistant.message = nlohmann::json::object(
        {{"role", "assistant"},
         {"content", "调工具"},
         {"tool_calls", nlohmann::json::array({nlohmann::json{
                            {"id", action_id},
                            {"type", "function"},
                            {"function", nlohmann::json{{"name", tool_name},
                                                        {"arguments", args.dump()}}}}})}});
    const WriteReceipt receipt = writer.AppendMessage(std::move(assistant), Durability::PowerLoss);
    REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({receipt.id}).status == WriteReceipt::Status::Committed);

    auto action = ToolActionSession::Admit(writer, turn_id, "step-" + turn_id, action_id,
                                           "queued", receipt.id, action_id);
    REQUIRE(action.Start(writer, "args-ref", ToolIdentity{tool_name, "builtin", "1.0", "workspace"},
                         std::nullopt)
                .status == WriteReceipt::Status::Committed);
    REQUIRE(action.Fail(writer, error_text, std::uint64_t{3}).status ==
            WriteReceipt::Status::Committed);
    return receipt.id;
}

bool HasCategory(const std::vector<FrictionOccurrence>& occurrences,
                 const std::string& category) {
    return std::any_of(occurrences.begin(), occurrences.end(),
                       [&](const FrictionOccurrence& o) { return o.category == category; });
}

bool HasFinding(const std::vector<Finding>& findings, const std::string& id) {
    return std::any_of(findings.begin(), findings.end(),
                       [&](const Finding& f) { return f.finding_id == id; });
}

bool NoteMentions(const SessionGateReport& report, const std::string& needle) {
    return std::any_of(report.notes.begin(), report.notes.end(),
                       [&](const std::string& note) { return note.find(needle) != std::string::npos; });
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << bytes;
}

// 一场标准 v3 封口场:两轮请求(工具面变化 + divergent 第二笔)。
std::filesystem::path StandardSealed(SessionsRoot& root, const char* id) {
    {
        auto writer = StartSession(root, id);
        REQUIRE(writer.has_value());
        InstallRequestedRound(*writer, "turn-000001", "第一问", {"read_file", "grep"},
                              nlohmann::json({{"inputTokens", 1000},
                                              {"cacheReadTokens", 800},
                                              {"outputTokens", 120}}),
                              std::nullopt);
        InstallRequestedRound(*writer, "turn-000002", "第二问", {"read_file"},
                              nlohmann::json({{"inputTokens", 600},
                                              {"cacheReadTokens", 9000},
                                              {"outputTokens", 90}}),
                              std::uint64_t{7}  // 发送视图 7 条,链引用 3 条 → divergent
                              );
        Seal(*writer);
    }
    return root.Dir(id);
}

}  // namespace

// ---------------------------------------------------------------------------
// GateSession 的 v3 半场
// ---------------------------------------------------------------------------

TEST_CASE("gate v3: 封口场 Analyzed,读模型与终态指纹齐") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("gate-ok");
    const std::filesystem::path dir = StandardSealed(root, "S-GATE-OK");
    const SessionGateReport report = GateSession(dir);
    REQUIRE(report.status == SessionGateStatus::Analyzed);
    CHECK(report.format == "v3");
    CHECK(report.error_code.empty());
    CHECK(report.session_id == "S-GATE-OK");
    CHECK(report.sealed());
    REQUIRE(report.v3_facts.size() == 1);
    const V3SessionFacts& facts = report.v3_facts.front();
    CHECK(facts.sealed);
    REQUIRE(facts.requests.size() == 2);
    CHECK(facts.requests[0].request_id != facts.requests[1].request_id);
    CHECK(facts.requests[0].purpose == "conversation");
    CHECK(!facts.requests[0].system_message_ref.empty());
    CHECK(facts.requests[0].input_message_refs.size() == 1);  // 首轮只带 user
    CHECK(facts.requests[1].input_message_refs.size() == 3);  // user+assistant+user
    CHECK(facts.requests[0].usage_reported);
    CHECK(facts.requests[0].input_tokens == 1000);
    CHECK(facts.requests[0].cache_read_tokens == 800);
    CHECK(facts.requests[0].sent);
    CHECK(facts.requests[0].outcome == "completed");
    CHECK(facts.requests[1].tool_names_recorded);
    REQUIRE(facts.requests[1].tool_names.size() == 1);
    CHECK(facts.requests[1].tool_names[0] == "read_file");
    CHECK(facts.requests[1].wire_view_divergent.has_value());
    CHECK(*facts.requests[1].wire_view_divergent);
    CHECK(facts.requests[0].seq > 0);  // 证据锚:prepared 行 seq
    CHECK(!facts.terminal_hash.empty());
    CHECK(report.stream_terminal_hashes.contains("S-GATE-OK:run-S-GATE-OK"));
}

TEST_CASE("gate v3: 未封口标 active;截断尾 incomplete;坏中段 corrupt") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("gate-bad");
    const std::filesystem::path good = StandardSealed(root, "S-GOOD");

    // active:同款账不落 session.ended。
    {
        auto writer = StartSession(root, "S-ACTIVE");
        REQUIRE(writer.has_value());
        InstallRequestedRound(*writer, "turn-000001", "还在跑", {"read_file"},
                              std::nullopt, std::nullopt);
        const SessionGateReport report = GateSession(root.Dir("S-ACTIVE"));
        CHECK(report.status == SessionGateStatus::Active);
        CHECK(report.error_code == "gate.active");
        CHECK_FALSE(report.sealed());
    }

    // 坏尾:去掉末行换行(崩溃截断)。
    {
        const std::string bytes = ReadFileBytes(good / "S-GOOD.jsonl");
        const std::filesystem::path dir = root.Dir("S-BADTAIL");
        WriteFileBytes(dir / "S-BADTAIL.jsonl", bytes.substr(0, bytes.size() - 5));
        const SessionGateReport report = GateSession(dir);
        CHECK(report.status == SessionGateStatus::Incomplete);
        CHECK(report.error_code == "gate.incomplete");
        CHECK(report.v3_facts.empty());
    }

    // 坏中段:改正文字节(哈希链必断)。
    {
        std::string bytes = ReadFileBytes(good / "S-GOOD.jsonl");
        const std::size_t at = bytes.find("第一问");
        REQUIRE(at != std::string::npos);
        bytes[at] = 'X';
        const std::filesystem::path dir = root.Dir("S-BADMID");
        WriteFileBytes(dir / "S-BADMID.jsonl", bytes);
        const SessionGateReport report = GateSession(dir);
        CHECK(report.status == SessionGateStatus::Corrupt);
        CHECK_FALSE(report.error_code.empty());
        CHECK(report.v3_facts.empty());
    }
}

TEST_CASE("gate v3: 子账缺/坏标 partial 不跳整场;缺 blob 点名不记零风险") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("gate-partial");
    {
        auto parent = StartSession(root, "S-PARENT");
        REQUIRE(parent.has_value());
        InstallRequestedRound(*parent, "turn-000001", "派个孩子",
                              {"read_file"},
                              nlohmann::json({{"inputTokens", 300}, {"outputTokens", 40}}),
                              std::nullopt);
        // 子账:spawn 请求在父账,子账目录被删(child_missing 形状)。
        const ChildSessionRef child_ref{"S-CHILD", "run-S-CHILD",
                                        "subagents/S-CHILD/S-CHILD.jsonl"};
        const ParentActionRef parent_action{"S-PARENT", "run-S-PARENT", "turn-000001",
                                            "step-000001", "action-000001", "msg-000001"};
        auto spawn = SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                            "step-000001", "task-000001", child_ref,
                                            parent_action, nlohmann::json::object(),
                                            nlohmann::json::object());
        auto boot = spawn.BootstrapChild(*parent, "run-S-CHILD", "你是子代理。", "去查。");
        REQUIRE(boot.child_writer.has_value());
        InstallRequestedRound(*boot.child_writer, "turn-000001", "子账的一问",
                              {"grep"},
                              nlohmann::json({{"inputTokens", 50}, {"outputTokens", 10}}),
                              std::nullopt);
        REQUIRE(spawn.Link(*parent, boot.checkpoint).status == WriteReceipt::Status::Committed);
        Seal(*parent);
        // 子账开完就删:父账还指着它 → child_missing。
        std::error_code ec;
        std::filesystem::remove_all(root.Dir("S-PARENT") / "subagents", ec);
    }
    const SessionGateReport report = GateSession(root.Dir("S-PARENT"));
    REQUIRE(report.status == SessionGateStatus::Analyzed);  // 主账好:整场仍可分析
    REQUIRE(report.v3_facts.size() == 1);                   // 缺的子账不装
    CHECK(NoteMentions(report, "facts.subsession_missing: S-CHILD"));
    CHECK(NoteMentions(report, "gate.v3_partial"));

    // 缺 blob:persisted 的 result_ref 指向不存在文件 → notes 点名。
    {
        SessionsRoot blob_root("gate-blob");
        auto writer = StartSession(blob_root, "S-BLOB");
        REQUIRE(writer.has_value());
        const std::string assistant_id =
            InstallRequestedRound(*writer, "turn-000001", "查一下", {"read_file"},
                                  nlohmann::json({{"inputTokens", 90}, {"outputTokens", 9}}),
                                  std::nullopt);
        auto action = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                               "action-000001", "queued", assistant_id,
                                               "prov-call-1");
        REQUIRE(action
                    .Start(*writer, "args-ref-1",
                           ToolIdentity{"search", "builtin", "1.0", "workspace"}, std::nullopt)
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, std::int64_t{0}, std::uint64_t{120}).status ==
                    WriteReceipt::Status::Committed);
        const nlohmann::json artifact = nlohmann::json::object(
            {{"artifactId", "art-x"},
             {"kind", "report"},
             {"path", "artifacts/report-x.txt"},
             {"sha256", std::string(64, 'a')},
             {"bytes", 64},
             {"mediaType", "text/plain"}});
        const auto persisted = action.PersistedResult(*writer, {artifact}, std::nullopt,
                                                       std::nullopt);
        REQUIRE(persisted.status == WriteReceipt::Status::Committed);
        REQUIRE(action.SelectResult(*writer, {persisted.id}, {}, "done", std::nullopt).status ==
                WriteReceipt::Status::Committed);
        Seal(*writer);
        const SessionGateReport blob_report = GateSession(blob_root.Dir("S-BLOB"));
        REQUIRE(blob_report.status == SessionGateStatus::Analyzed);
        CHECK(NoteMentions(blob_report, "facts.missing_blob"));
    }
}

TEST_CASE("gate v3: 不认的格式标 Unsupported,不伪装空会话") {
    SessionsRoot root("gate-unsupported");
    const std::filesystem::path dir = root.Dir("S-ODD");
    WriteFileBytes(dir / "S-ODD.jsonl",
                   std::string("{\"schemaVersion\":99,\"schema\":\"something.else\"}\n"));
    const SessionGateReport report = GateSession(dir);
    CHECK(report.status == SessionGateStatus::Unsupported);
    CHECK(report.error_code == "gate.session_format_unsupported");
    CHECK(std::string(SessionGateStatusName(report.status)) == "unsupported");
    // 两种主账打架同理。
    const std::filesystem::path conflict = root.Dir("S-MIX");
    WriteFileBytes(conflict / "S-MIX.jsonl",
                   std::string("{\"schemaVersion\":3,\"schema\":\"lubancode.trajectory.v3\"}\n"));
    WriteFileBytes(conflict / "main.jsonl", std::string("{}\n"));
    const SessionGateReport mixed = GateSession(conflict);
    CHECK(mixed.status == SessionGateStatus::Unsupported);
    CHECK(mixed.error_code == "gate.session_format_conflict");
}

// ---------------------------------------------------------------------------
// 领域读模型与摩擦 v3
// ---------------------------------------------------------------------------

TEST_CASE("facts: 三种视图不混——旧请求的引用不随当前链变化") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("facts-refs");
    std::vector<std::string> first_refs;
    {
        auto writer = StartSession(root, "S-REFS");
        REQUIRE(writer.has_value());
        InstallRequestedRound(*writer, "turn-000001", "第一问", {"read_file"},
                              nlohmann::json({{"inputTokens", 10}, {"outputTokens", 2}}),
                              std::nullopt);
        const V3FactsRead facts = CollectV3SessionFacts(root.Dir("S-REFS"),
                                                        root.Ledger("S-REFS"));
        REQUIRE(facts.ok);
        first_refs = facts.sessions.front().requests.front().input_message_refs;
        InstallRequestedRound(*writer, "turn-000002", "第二问", {"read_file"},
                              nlohmann::json({{"inputTokens", 12}, {"outputTokens", 3}}),
                              std::nullopt);
    }
    const V3FactsRead facts = CollectV3SessionFacts(root.Dir("S-REFS"), root.Ledger("S-REFS"));
    REQUIRE(facts.ok);
    REQUIRE(facts.sessions.front().requests.size() == 2);
    // 首笔请求的引用账不动——不拿今天的链倒推历史请求(§3.1)。
    CHECK(facts.sessions.front().requests.front().input_message_refs == first_refs);
    CHECK(facts.sessions.front().requests[1].input_message_refs.size() ==
          first_refs.size() + 2);
}

TEST_CASE("friction v3: 失败归类/重试/落盘失败/取消/provider 失败;缺件类别不判") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("friction");
    {
        auto writer = StartSession(root, "S-FRICTION");
        REQUIRE(writer.has_value());
        // 三种失败 reason:普通/参数/权限(failed 落 error_code 键)。
        InstallFailingTool(*writer, "turn-000001", "action-000001", "read_file",
                           "missing_file", nlohmann::json{{"path", "a.cpp"}});
        InstallFailingTool(*writer, "turn-000001", "action-000002", "edit_file",
                           "invalid_arguments", nlohmann::json{{"path", "b.cpp"}});
        InstallFailingTool(*writer, "turn-000001", "action-000003", "run_command",
                           "permission_denied", nlohmann::json::object());
        // 同 action 两次尝试失败 → repeated_retry。
        {
            const std::string assistant_id = InstallFailingTool(
                *writer, "turn-000002", "action-000004", "run_command", "timeout");
            // 账面对齐把手:上一 attempt 已 Failed(§4.14 先终态再重试)。
            auto action = ToolActionSession::ReopenAligned("turn-000002", "step-000002",
                                                           "action-000004", std::uint64_t{1},
                                                           true, ToolActionSession::Terminal::Failed);
            REQUIRE(action.BeginNextAttempt(*writer, "retry", Durability::ProcessCrash)
                        .status == WriteReceipt::Status::Committed);
            REQUIRE(action
                        .Start(*writer, "args-ref-2",
                               ToolIdentity{"run_command", "builtin", "1.0", "workspace"},
                               std::nullopt)
                        .status == WriteReceipt::Status::Committed);
            REQUIRE(action.Fail(*writer, "timeout_again", std::uint64_t{4}).status ==
                    WriteReceipt::Status::Committed);
            (void)assistant_id;
        }
        // 落盘失败:执行 done、结果存不进。
        {
            const std::string assistant_id = InstallFailingTool(
                *writer, "turn-000003", "action-000005", "read_file", "unused_error");
            // 上面 InstallFailingTool 已把该 action 记成失败;落盘失败另开一枚。
            (void)assistant_id;
            const std::string done_id = InstallFailingTool(
                *writer, "turn-000004", "action-000006", "read_file", "never_happens");
            (void)done_id;
        }
        {
            // 干净执行 + 落盘失败。
            MessageDraft caller;
            caller.turn_id = "turn-000005";
            caller.step_id = "step-turn-000005";
            caller.request_id = "request-turn-000005";
            caller.purpose = MessagePurpose::Conversation;
            caller.origin = MessageOrigin::SessionRuntime;
            caller.provider = "stub";
            caller.wire = "openai";
            caller.model = "stub-mini";
            caller.response_model = nlohmann::json(nullptr);
            caller.usage = nlohmann::json(nullptr);
            caller.message = nlohmann::json::object(
                {{"role", "assistant"},
                 {"content", "调一枚会落盘失败的"},
                 {"tool_calls", nlohmann::json::array({nlohmann::json{
                                    {"id", "action-000007"},
                                    {"type", "function"},
                                    {"function", nlohmann::json{{"name", "read_file"},
                                                                {"arguments", "{}"}}}}})}});
            const WriteReceipt receipt =
                (*writer).AppendMessage(std::move(caller), Durability::PowerLoss);
            REQUIRE(receipt.status == WriteReceipt::Status::Committed);
            auto action = ToolActionSession::Admit(*writer, "turn-000005",
                                                   "step-turn-000005", "action-000007",
                                                   "queued", receipt.id, "action-000007");
            REQUIRE(action
                        .Start(*writer, "args-ref",
                               ToolIdentity{"read_file", "builtin", "1.0", "workspace"},
                               std::nullopt)
                        .status == WriteReceipt::Status::Committed);
            REQUIRE(action.Finish(*writer, std::int64_t{0}, std::uint64_t{10}).status ==
                    WriteReceipt::Status::Committed);
            REQUIRE(action.PersistFailed(*writer, "disk_full").status ==
                    WriteReceipt::Status::Committed);
        }
        // 取消的工具尝试。
        {
            MessageDraft caller;
            caller.turn_id = "turn-000006";
            caller.step_id = "step-turn-000006";
            caller.request_id = "request-turn-000006";
            caller.purpose = MessagePurpose::Conversation;
            caller.origin = MessageOrigin::SessionRuntime;
            caller.provider = "stub";
            caller.wire = "openai";
            caller.model = "stub-mini";
            caller.response_model = nlohmann::json(nullptr);
            caller.usage = nlohmann::json(nullptr);
            caller.message = nlohmann::json::object(
                {{"role", "assistant"},
                 {"content", "调一枚会被取消的"},
                 {"tool_calls", nlohmann::json::array({nlohmann::json{
                                    {"id", "action-000008"},
                                    {"type", "function"},
                                    {"function", nlohmann::json{{"name", "run_command"},
                                                                {"arguments", "{}"}}}}})}});
            const WriteReceipt receipt =
                (*writer).AppendMessage(std::move(caller), Durability::PowerLoss);
            REQUIRE(receipt.status == WriteReceipt::Status::Committed);
            auto action = ToolActionSession::Admit(*writer, "turn-000006",
                                                   "step-turn-000006", "action-000008",
                                                   "queued", receipt.id, "action-000008");
            REQUIRE(action
                        .Start(*writer, "args-ref",
                               ToolIdentity{"run_command", "builtin", "1.0", "workspace"},
                               std::nullopt)
                        .status == WriteReceipt::Status::Committed);
            REQUIRE(action.Cancel(*writer, "during_execution", "user_interrupt").status ==
                    WriteReceipt::Status::Committed);
        }
        // provider 失败:prepared -> sent -> model.request.failed。
        {
            MessageDraft user;
            user.turn_id = "turn-000007";
            user.purpose = MessagePurpose::Conversation;
            user.origin = MessageOrigin::Human;
            user.message = nlohmann::json::object({{"role", "user"}, {"content", "会失败的一问"}});
            const WriteReceipt user_receipt =
                (*writer).AppendMessage(std::move(user), Durability::PowerLoss);
            REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);
            REQUIRE((*writer).AdmitMessages({user_receipt.id}).status ==
                    WriteReceipt::Status::Committed);
            const std::string request_id = (*writer).NewRequestId();
            std::vector<std::string> input_refs;
            for (const auto& node : (*writer).context().chain) {
                input_refs.push_back(node.message_ref);
            }
            if (!input_refs.empty()) {
                input_refs.erase(input_refs.begin());
            }
            REQUIRE((*writer)
                        .PrepareRequest(request_id, "turn-000007", "step-turn-000007",
                                        "conversation", (*writer).context().system_message_ref,
                                        input_refs,
                                        SnapshotOf("stub", "openai", "stub-mini",
                                                   {"read_file"}, std::nullopt,
                                                   input_refs.size()),
                                        std::nullopt, Durability::PowerLoss)
                        .status == WriteReceipt::Status::Committed);
            EventDraft failed;
            failed.kind = EventKindV3::ModelRequestFailed;
            failed.status = OpStatus::Failed;
            failed.turn_id = "turn-000007";
            failed.step_id = "step-turn-000007";
            failed.request_id = request_id;
            failed.payload = nlohmann::json{{"reason", "provider_error"}};
            REQUIRE((*writer).AppendEvent(std::move(failed), Durability::PowerLoss).status ==
                    WriteReceipt::Status::Committed);
        }
        Seal(*writer);
    }
    const V3FactsRead facts = CollectV3SessionFacts(root.Dir("S-FRICTION"),
                                                    root.Ledger("S-FRICTION"));
    REQUIRE(facts.ok);
    const std::vector<FrictionOccurrence> occurrences = ClassifyFrictionV3(facts.sessions);
    CHECK(HasCategory(occurrences, "tool.execution_failure"));
    CHECK(HasCategory(occurrences, "tool.invalid_input"));
    CHECK(HasCategory(occurrences, "permission.denied"));
    CHECK(HasCategory(occurrences, "tool.repeated_retry"));
    CHECK(HasCategory(occurrences, "cancelled"));
    CHECK(HasCategory(occurrences, "provider.failure"));
    // 落盘失败单独表达:phase=persist,仍归 execution_failure 族。
    {
        const auto it = std::find_if(
            occurrences.begin(), occurrences.end(), [](const FrictionOccurrence& o) {
                return o.evidence.metric == "tool_persist_failed";
            });
        REQUIRE(it != occurrences.end());
        CHECK(it->evidence.value.at("phase") == "persist");
        CHECK(it->evidence.value.at("reason") == "disk_full");
    }
    // v3 无审批/验证/任务结果事实:这些类别一个都不出(缺件不是零)。
    CHECK_FALSE(HasCategory(occurrences, "approval.wait"));
    CHECK_FALSE(HasCategory(occurrences, "verification.failure"));
    CHECK_FALSE(HasCategory(occurrences, "verification.missing"));
    CHECK_FALSE(HasCategory(occurrences, "user.correction"));
    CHECK_FALSE(HasCategory(occurrences, "unknown"));
    // 证据锚:每枚 occurrence 带 event_id;provider 失败锚在失败事件行。
    {
        const auto it = std::find_if(
            occurrences.begin(), occurrences.end(),
            [](const FrictionOccurrence& o) { return o.category == "provider.failure"; });
        REQUIRE(it != occurrences.end());
        REQUIRE(it->evidence.event_id.has_value());
        CHECK(it->evidence.seq.has_value());
        CHECK(!it->rule_version.empty());
    }
    for (const auto& occurrence : occurrences) {
        CHECK(occurrence.evidence.event_id.has_value());
    }
}

TEST_CASE("friction v3: 空账零摩擦;unsupported 表在册") {
    const std::vector<FrictionOccurrence> empty = ClassifyFrictionV3({});
    CHECK(empty.empty());
    const std::vector<std::string>& unsupported = UnsupportedFrictionCategoriesV3();
    REQUIRE(unsupported.size() == 3);
    CHECK(std::find(unsupported.begin(), unsupported.end(), "approval.wait") !=
          unsupported.end());
}

// ---------------------------------------------------------------------------
// prompt 审计 runtime v3
// ---------------------------------------------------------------------------

TEST_CASE("prompt v3: 工具面抖动/divergent/估算缺口出 finding,R01 不误报") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("prompt");
    const std::filesystem::path dir = StandardSealed(root, "S-PROMPT");
    const RuntimeRequestsRead read = CollectRuntimeRequests(dir);
    REQUIRE(read.ok);
    CHECK(read.session_id == "S-PROMPT");
    REQUIRE(read.requests.size() == 2);
    CHECK(read.requests[0].input_refs_recorded);
    CHECK(read.requests[0].tool_names_recorded);
    CHECK(read.requests[1].context_revision >= read.requests[0].context_revision);

    RuntimeAuditInput input;
    input.session_id = read.session_id;
    input.requests = read.requests;
    const std::vector<Finding> findings = AuditPromptRuntime(input);
    // 工具面 read_file+grep -> read_file:一次变化不足以报抖动(≥2 次才报),
    // 两次请求只一对,不出 V02。
    CHECK_FALSE(HasFinding(findings, "P-AUD-V02"));
    // divergent 第二笔:V01 点名。
    CHECK(HasFinding(findings, "P-AUD-V01"));
    // 没有 tokenEstimateRef:V04 覆盖面。
    CHECK(HasFinding(findings, "P-AUD-V04"));
    // v3 没有 v2 manifest 不算缺件:R01 不出。
    CHECK_FALSE(HasFinding(findings, "P-AUD-R01"));
    // V01 证据锚 event_id+seq。
    const auto it = std::find_if(findings.begin(), findings.end(),
                                 [](const Finding& f) { return f.finding_id == "P-AUD-V01"; });
    REQUIRE(it != findings.end());
    bool anchored = false;
    for (const auto& item : it->evidence) {
        if (item.metric == "divergent_request") {
            anchored = item.event_id.has_value() && item.seq.has_value();
        }
    }
    CHECK(anchored);
}

TEST_CASE("prompt v3: 连续三次工具面变化报 V02;usage 关联进视图") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("prompt-churn");
    {
        auto writer = StartSession(root, "S-CHURN");
        REQUIRE(writer.has_value());
        InstallRequestedRound(*writer, "turn-000001", "一", {"a", "b"},
                              nlohmann::json({{"inputTokens", 10}, {"outputTokens", 1}}),
                              std::nullopt);
        InstallRequestedRound(*writer, "turn-000002", "二", {"a"},
                              nlohmann::json({{"inputTokens", 10}, {"outputTokens", 1}}),
                              std::nullopt);
        InstallRequestedRound(*writer, "turn-000003", "三", {"a", "c"},
                              nlohmann::json({{"inputTokens", 10}, {"outputTokens", 1}}),
                              std::nullopt);
        Seal(*writer);
    }
    const RuntimeRequestsRead read = CollectRuntimeRequests(root.Dir("S-CHURN"));
    REQUIRE(read.ok);
    REQUIRE(read.requests.size() == 3);
    CHECK(read.requests[0].usage_reported);
    CHECK(read.requests[0].total_input_tokens == 10);
    RuntimeAuditInput input;
    input.session_id = read.session_id;
    input.requests = read.requests;
    const std::vector<Finding> findings = AuditPromptRuntime(input);
    CHECK(HasFinding(findings, "P-AUD-V02"));
    const RuntimeChangeSummary changes = SummarizeRuntimeChanges(read.requests);
    CHECK(changes.v3_comparable == 2);
    CHECK(changes.v3_toolset_changes == 2);
}

// ---------------------------------------------------------------------------
// session 分析 v3:摘要/去重/字节稳定
// ---------------------------------------------------------------------------

TEST_CASE("analyze v3: 摘要 format/limitations/usage owner;重算字节稳定") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root_a("analyze-a");
    SessionsRoot root_b("analyze-b");
    const std::filesystem::path dir_a = StandardSealed(root_a, "S-AN");
    const std::filesystem::path dir_b = StandardSealed(root_b, "S-AN");

    const SessionAnalyzeResult first = AnalyzeSession(dir_a, SessionAnalyzeOptions{});
    REQUIRE(first.analyzed);
    CHECK_FALSE(first.provisional);
    CHECK(first.summary.source.format == "v3");
    CHECK(first.summary.analyzer_version == kInsightsAnalyzerVersion);
    CHECK(first.summary.coverage.requests_total == 2);
    CHECK(first.summary.coverage.requests_with_usage == 2);
    CHECK(first.summary.usage.input_tokens == 1600);
    CHECK(first.summary.usage.cache_read_tokens == 9800);
    CHECK(first.summary.usage.output_tokens == 210);
    // v3 无 verification/outcome/审批事实:0/空如实,limitations 声明在册。
    CHECK(first.summary.work.verifications == 0);
    CHECK(first.summary.work.outcome.empty());
    REQUIRE(first.summary.coverage.limitations.size() == 3);
    CHECK(first.summary_written);

    // 同一份夹具在另一目录重算:摘要 JSON 字节一致(派生可删可重算)。
    const SessionAnalyzeResult second = AnalyzeSession(dir_b, SessionAnalyzeOptions{});
    REQUIRE(second.analyzed);
    CHECK(second.summary_written);
    CHECK(ReadFileBytes(dir_a / "derived" / "insights-v1" / "session-summary.json") ==
          ReadFileBytes(dir_b / "derived" / "insights-v1" / "session-summary.json"));

    // 再算一遍:fresh 摘要复用,不重写。
    const SessionAnalyzeResult reuse = AnalyzeSession(dir_a, SessionAnalyzeOptions{});
    CHECK(reuse.summary_reused);
    CHECK_FALSE(reuse.summary_written);
}

TEST_CASE("analyze v3: active 场 include_active 才分析,不写长期摘要") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("analyze-active");
    {
        auto writer = StartSession(root, "S-ACTIVE-AN");
        REQUIRE(writer.has_value());
        InstallRequestedRound(*writer, "turn-000001", "在跑", {"read_file"},
                              nlohmann::json({{"inputTokens", 5}, {"outputTokens", 1}}),
                              std::nullopt);
    }
    const std::filesystem::path dir = root.Dir("S-ACTIVE-AN");
    const SessionAnalyzeResult skipped = AnalyzeSession(dir, SessionAnalyzeOptions{});
    CHECK_FALSE(skipped.analyzed);
    SessionAnalyzeOptions with_active;
    with_active.include_active = true;
    const SessionAnalyzeResult analyzed = AnalyzeSession(dir, with_active);
    CHECK(analyzed.analyzed);
    CHECK(analyzed.provisional);
    CHECK_FALSE(analyzed.summary_written);
    CHECK(analyzed.summary.source.integrity == "provisional");
    CHECK(analyzed.summary.source.format == "v3");
}

TEST_CASE("analyze v3: 父子树只算一次;resume 祖先不重算(T06 范围)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SessionsRoot root("dedupe");
    {
        // 祖先场:两笔请求。
        auto ancestor = StartSession(root, "S-ANC");
        REQUIRE(ancestor.has_value());
        InstallRequestedRound(*ancestor, "turn-000001", "祖先一问", {"read_file"},
                              nlohmann::json({{"inputTokens", 100}, {"outputTokens", 10}}),
                              std::nullopt);
        Seal(*ancestor);
        // 后继场:resume 祖先 + 自己一笔请求(共 2 笔:resume 的模型输入
        // 不重携祖先全史,自己只发一笔)。
        auto successor = StartSession(root, "S-SUC");
        REQUIRE(successor.has_value());
        const auto source = ReadV3Ledger(root.Ledger("S-ANC"));
        REQUIRE(source.has_value());
        const auto last = source->LastEntry();
        REQUIRE(last.has_value());
        EventDraft attached;
        attached.kind = EventKindV3::ResumeSourceAttached;
        attached.payload = nlohmann::json::object(
            {{"sourceRef",
              nlohmann::json::object({{"sessionId", source->session_id},
                                      {"runId", source->run_id},
                                      {"seq", last->seq},
                                      {"id", last->is_message
                                                 ? source->messages[last->index].message_id
                                                 : source->events[last->index].event_id},
                                      {"hash", last->is_message
                                                   ? source->messages[last->index].line_hash
                                                   : source->events[last->index].line_hash}})},
             {"contextRevision", source->context.revision},
             {"systemMessageRef", source->context.system_message_ref},
             {"branch", "main"}});
        REQUIRE(successor
                    ->AppendEvent(std::move(attached), Durability::PowerLoss)
                    .status == WriteReceipt::Status::Committed);
        InstallRequestedRound(*successor, "turn-000001", "后继自己的一问", {"read_file"},
                              nlohmann::json({{"inputTokens", 40}, {"outputTokens", 5}}),
                              std::nullopt);
        Seal(*successor);
    }
    const SessionAnalyzeResult anc = AnalyzeSession(root.Dir("S-ANC"), SessionAnalyzeOptions{});
    REQUIRE(anc.analyzed);
    CHECK(anc.summary.coverage.requests_total == 1);  // 祖先只算自己那一笔
    const SessionAnalyzeResult suc = AnalyzeSession(root.Dir("S-SUC"), SessionAnalyzeOptions{});
    REQUIRE(suc.analyzed);
    // 后继的树不含祖先账:resume 引用祖先不代表再花一次钱(§3.2)。
    CHECK(suc.summary.coverage.requests_total == 1);
    CHECK(suc.summary.usage.input_tokens == 40);

    // 两层子代理:父场树内递归一次,父子请求各计各,不重复。
    {
        auto parent = StartSession(root, "S-PARENT-2");
        REQUIRE(parent.has_value());
        InstallRequestedRound(*parent, "turn-000001", "派孩子", {"agent"},
                              nlohmann::json({{"inputTokens", 200}, {"outputTokens", 20}}),
                              std::nullopt);
        const ChildSessionRef child_ref{"S-KID", "run-S-KID", "subagents/S-KID/S-KID.jsonl"};
        const ParentActionRef parent_action{"S-PARENT-2", "run-S-PARENT-2", "turn-000001",
                                            "step-000001", "action-000001", "msg-000001"};
        auto spawn = SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                            "step-000001", "task-000001", child_ref,
                                            parent_action, nlohmann::json::object(),
                                            nlohmann::json::object());
        auto boot = spawn.BootstrapChild(*parent, "run-S-KID", "你是子代理。", "去查。");
        REQUIRE(boot.child_writer.has_value());
        InstallRequestedRound(*boot.child_writer, "turn-000001", "子账一问", {"grep"},
                              nlohmann::json({{"inputTokens", 30}, {"outputTokens", 4}}),
                              std::nullopt);
        REQUIRE(spawn.Link(*parent, boot.checkpoint).status == WriteReceipt::Status::Committed);
        Seal(*parent);
    }
    const SessionAnalyzeResult parent =
        AnalyzeSession(root.Dir("S-PARENT-2"), SessionAnalyzeOptions{});
    REQUIRE(parent.analyzed);
    // 树内两账:父 1 笔 + 子 1 笔,各计一次(WalkSessionTree 树内去重)。
    CHECK(parent.summary.coverage.requests_total == 2);
    CHECK(parent.summary.usage.input_tokens == 230);
}

// ---------------------------------------------------------------------------
// 报告边界:redaction allowlist 认得 v3 新键
// ---------------------------------------------------------------------------

TEST_CASE("redaction: format/limitations/seq 路径在 allowlist") {
    CHECK(IsAllowedReportFieldPath({"source", "format"}));
    CHECK(IsAllowedReportFieldPath({"coverage", "limitations"}));
    CHECK(IsAllowedReportFieldPath({"sessions", "source", "format"}));
    CHECK(IsAllowedReportFieldPath({"sessions", "coverage", "limitations"}));
    CHECK(IsAllowedReportFieldPath({"prompt_findings", "evidence", "seq"}));
    CHECK(IsAllowedReportFieldPath({"sessions", "prompt_findings", "evidence", "seq"}));
    CHECK(IsAllowedReportFieldPath({"findings", "evidence", "seq"}));
    CHECK_FALSE(IsAllowedReportFieldPath({"sessions", "coverage", "made_up"}));
}

TEST_CASE("summary schema: format/limitations 序列化往返") {
    std::string error;
    SessionInsightSummary summary;
    summary.source.format = "v3";
    summary.coverage.limitations = {"verification: 无该事实"};
    const nlohmann::json json = summary.ToJson();
    CHECK(json.at("source").at("format") == "v3");
    const auto parsed = SessionInsightSummary::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->source.format == "v3");
    REQUIRE(parsed->coverage.limitations.size() == 1);
    // 旧摘要(无 format/limitations 键)读回:format=v2、limitations 空。
    nlohmann::json legacy = json;
    legacy.at("source").erase("format");
    legacy.at("coverage").erase("limitations");
    const auto old = SessionInsightSummary::FromJsonStrict(legacy, &error);
    REQUIRE(old.has_value());
    CHECK(old->source.format == "v2");
    CHECK(old->coverage.limitations.empty());
    // 未知键拒绝。
    nlohmann::json bad = json;
    bad.at("source")["formatx"] = "v3";
    const auto rejected = SessionInsightSummary::FromJsonStrict(bad, &error);
    CHECK_FALSE(rejected.has_value());
}
