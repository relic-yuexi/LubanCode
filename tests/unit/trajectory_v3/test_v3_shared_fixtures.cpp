// T00(SessionV3 旧设计清理单 B1):共用 v3 夹具冻结册。给 T06/T07/T14
//(B2 消费链:usage/Telemetry/Insights)与 T15-B(生命周期)预备同一组
// 验收材料——形状按单子 T00 冻结清单:
//   主账多轮、compact、工具逐项结果、缺 usage、迟到 usage、缺 blob、
//   两层子代理、两次 resume 来源链、坏尾/坏中段、锁态。
// 生成路数:合法账一律经 V3Writer 现场生成(与现行写者同一口径,不手拼
// 哈希链);错误形状(坏尾/坏中段/缺 blob)在合法账上注入——writer 造
// 不出错误账,恰好避免 writer/reader 同一个错误互相自证。
// 已有静态夹具(tests/fixtures/trajectory_v3/ 九份)覆盖主账/工具轮/
// compact/流中断/降档/父子一层的验卷面,本册补"生成 + 投影 + 错误"面。
// 归档态:v3 场的生产归档形状未发行(T15-B),清单记缺口不伪造。
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/v3_compact_runtime.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;
using lubancode::runtime::V3CompactModelClient;
using lubancode::runtime::V3CompactModelReply;
using lubancode::runtime::V3CompactProfile;
using lubancode::runtime::V3CompactRunInput;
using lubancode::runtime::V3CompactRunResult;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct SessionsRoot {
    FixedClock clock;
    std::filesystem::path root;  // sessions/<id>/<id>.jsonl

    explicit SessionsRoot(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-v3-shared-fixtures-" + std::string(tag));
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

// 一轮 user → assistant(usage 可缺:缺实报为 null,不补 0),接纳进链。
std::string InstallRound(V3Writer& writer, const std::string& turn_id,
                         const std::string& user_text, bool with_usage) {
    MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", user_text}});
    WriteReceipt user_receipt = writer.AppendMessage(std::move(user), Durability::PowerLoss);
    REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status == WriteReceipt::Status::Committed);

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
    assistant.usage = with_usage
                          ? std::optional<nlohmann::json>(
                                nlohmann::json({{"inputTokens", 120}, {"outputTokens", 30}}))
                          : std::optional<nlohmann::json>(nlohmann::json(nullptr));
    assistant.message = nlohmann::json::object(
        {{"role", "assistant"}, {"content", "收到:" + user_text}});
    WriteReceipt assistant_receipt =
        writer.AppendMessage(std::move(assistant), Durability::PowerLoss);
    REQUIRE(assistant_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({assistant_receipt.id}).status ==
            WriteReceipt::Status::Committed);
    return assistant_receipt.id;
}

// resume.source.attached:五键指源末行(§4.10 步 2;与 reader_resume 同形)。
void AttachSource(V3Writer& writer, const V3Ledger& source) {
    auto last = source.LastEntry();
    REQUIRE(last.has_value());
    const std::string id = last->is_message ? source.messages[last->index].message_id
                                            : source.events[last->index].event_id;
    const std::string hash = last->is_message ? source.messages[last->index].line_hash
                                              : source.events[last->index].line_hash;
    EventDraft attached;
    attached.kind = EventKindV3::ResumeSourceAttached;
    attached.payload = nlohmann::json::object({
        {"sourceRef",
         nlohmann::json::object({{"sessionId", source.session_id},
                                 {"runId", source.run_id},
                                 {"seq", last->seq},
                                 {"id", id},
                                 {"hash", hash}})},
        {"contextRevision", source.context.revision},
        {"systemMessageRef", source.context.system_message_ref},
        {"branch", "main"},
    });
    REQUIRE(writer.AppendEvent(std::move(attached), Durability::PowerLoss).status ==
            WriteReceipt::Status::Committed);
}

// 压缩模型桩:合格摘要 + manifest 围栏(compact_runtime 同形)。
class StubClient : public V3CompactModelClient {
public:
    int calls = 0;

    V3CompactModelReply Send(const std::string& system,
                             const std::vector<nlohmann::json>& messages) override {
        (void)system;
        (void)messages;
        ++calls;
        V3CompactModelReply reply;
        reply.ok = true;
        nlohmann::json manifest = nlohmann::json::object(
            {{"goal", "把会话接下去"}, {"constraints", nlohmann::json::array({"不许动旧档"})},
             {"open_items", nlohmann::json::array({"还差一步"})}, {"next_action", "继续干活"}});
        reply.text = "摘要正文。\n```json\n" + manifest.dump() + "\n```\n";
        reply.usage = nlohmann::json({{"inputTokens", 50}, {"outputTokens", 10}});
        return reply;
    }
};

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

}  // namespace

// ---------------------------------------------------------------------------
// 冻结清单:主账多轮 + 缺 usage / 迟到 usage
// ---------------------------------------------------------------------------

TEST_CASE("夹具: 主账多轮可读,缺 usage 的 assistant 保持 null 不补 0") {
    SessionsRoot root("base");
    std::string no_usage_id;
    {
        auto writer = StartSession(root, "S-MAIN");
        REQUIRE(writer.has_value());
        InstallRound(*writer, "turn-000001", "第一问", /*with_usage=*/true);
        no_usage_id = InstallRound(*writer, "turn-000002", "第二问", /*with_usage=*/false);
    }
    const auto ledger = ReadV3Ledger(root.Ledger("S-MAIN"));
    REQUIRE(ledger.has_value());
    // 链:system + 两轮四消息。
    REQUIRE(ledger->context.chain.size() == 5);
    const auto it = ledger->message_index.find(no_usage_id);
    REQUIRE(it != ledger->message_index.end());
    // usage 读回是 optional<json>:线上 "usage": null 装成含 null 的 optional,
    // 整键缺位是 nullopt——两种形状都算缺实报,不许补成 0 的对象。
    CHECK(!ledger->messages[it->second].usage.has_value() ||
          ledger->messages[it->second].usage->is_null());
}

TEST_CASE("夹具: 迟到 usage 走 model.usage.appended 观察,不改旧 message") {
    SessionsRoot root("late-usage");
    std::string assistant_id;
    {
        auto writer = StartSession(root, "S-LATE");
        REQUIRE(writer.has_value());
        assistant_id = InstallRound(*writer, "turn-000001", "会迟到的一问", /*with_usage=*/false);
        // 迟到实报:消息已成行后到的补报 → 单独立账(§4.12)。
        EventDraft appended;
        appended.kind = EventKindV3::ModelUsageAppended;
        appended.request_id = "request-turn-000001";
        appended.turn_id = "turn-000001";
        appended.payload = nlohmann::json{{"usage", {{"inputTokens", 130}, {"outputTokens", 35}}},
                                          {"reportedByProvider", true},
                                          {"providerResponseId", "resp-late"}};
        REQUIRE(writer->AppendEvent(std::move(appended), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    }
    const auto ledger = ReadV3Ledger(root.Ledger("S-LATE"));
    REQUIRE(ledger.has_value());
    bool saw_appended = false;
    for (const auto& event : ledger->events) {
        saw_appended = saw_appended || event.kind == EventKindV3::ModelUsageAppended;
    }
    CHECK(saw_appended);
    // 旧 message 的 usage 仍是 null:迟到观察不倒改 owner(§五)。
    const auto it = ledger->message_index.find(assistant_id);
    REQUIRE(it != ledger->message_index.end());
    CHECK(!ledger->messages[it->second].usage.has_value() ||
          ledger->messages[it->second].usage->is_null());
}

// ---------------------------------------------------------------------------
// 冻结清单:compact(applied 全链)
// ---------------------------------------------------------------------------

TEST_CASE("夹具: compact applied 后链重接为 system+摘要+保留") {
    SessionsRoot root("compact");
    {
        auto writer = StartSession(root, "S-COMPACT");
        REQUIRE(writer.has_value());
        // 体量给足,压缩才有得赚(收益校验不白拦)。
        MessageDraft big;
        big.turn_id = "turn-000001";
        big.purpose = MessagePurpose::Conversation;
        big.origin = MessageOrigin::Human;
        big.message = nlohmann::json::object(
            {{"role", "user"}, {"content", std::string(6000, 'x')}});
        WriteReceipt big_receipt = writer->AppendMessage(std::move(big), Durability::PowerLoss);
        REQUIRE(big_receipt.status == WriteReceipt::Status::Committed);
        MessageDraft answer;
        answer.turn_id = "turn-000001";
        answer.request_id = "request-turn-000001";
        answer.provider = "stub";
        answer.wire = "openai";
        answer.model = "stub-mini";
        answer.response_model = nlohmann::json(nullptr);
        answer.usage = nlohmann::json({{"inputTokens", 900}, {"outputTokens", 200}});
        answer.origin = MessageOrigin::SessionRuntime;
        answer.message = nlohmann::json::object(
            {{"role", "assistant"}, {"content", std::string(4000, 'y')}});
        WriteReceipt answer_receipt = writer->AppendMessage(std::move(answer),
                                                            Durability::PowerLoss);
        REQUIRE(answer_receipt.status == WriteReceipt::Status::Committed);
        REQUIRE(writer
                    ->AdmitMessages({big_receipt.id, answer_receipt.id},
                                    Durability::PowerLoss)
                    .status == WriteReceipt::Status::Committed);

        StubClient client;
        V3CompactProfile profile;
        profile.provider = "stub";
        profile.wire = "openai";
        profile.model = "stub-mini";
        profile.compact_window_tokens = 0;  // 门禁关:链路形状与容量变量分开
        V3CompactRunInput input;
        input.trigger = "manual";
        input.reason = "user_command";
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, profile, std::move(input));
        REQUIRE(result.applied);
    }
    const auto ledger = ReadV3Ledger(root.Ledger("S-COMPACT"));
    REQUIRE(ledger.has_value());
    CHECK(ledger->context.open_compact_ids.empty());  // applied 收口
    const auto context = ProjectModelContext(*ledger);
    CHECK(context.inputs.size() < 2);  // 压缩后链上只留摘要(+保留)
    bool saw_marker = false;
    for (const auto& item : ProjectHistoryTimeline(*ledger).items) {
        saw_marker = saw_marker || item.kind == HistoryTimeline::Item::Kind::CompactMarker;
    }
    CHECK(saw_marker);
}

// ---------------------------------------------------------------------------
// 冻结清单:工具逐项结果 + 缺 blob
// ---------------------------------------------------------------------------

TEST_CASE("夹具: 工具逐项结果链齐全;artifact 缺 blob 标 missing_blob") {
    SessionsRoot root("tool");
    std::string tool_message_id;
    {
        auto writer = StartSession(root, "S-TOOL");
        REQUIRE(writer.has_value());
        InstallRound(*writer, "turn-000001", "查一下", /*with_usage=*/true);

        auto action = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                               "action-000001", "queued", std::nullopt,
                                               "prov-call-1");
        REQUIRE(action.Start(*writer, "args-ref-1",
                             ToolIdentity{"search", "builtin", "1.0", "workspace"},
                             std::nullopt)
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, /*exit_code=*/0, /*execution_duration_ms=*/120)
                    .status == WriteReceipt::Status::Committed);
        // 结果持久化:artifactRef 六键,文件不落地(缺 blob 形状)。
        nlohmann::json artifact = nlohmann::json::object(
            {{"artifactId", "art-1"},
             {"kind", "report"},
             {"path", "artifacts/report-1.txt"},
             {"sha256", std::string(64, 'a')},
             {"bytes", 128},
             {"mediaType", "text/plain"}});
        const auto persisted =
            action.PersistedResult(*writer, {artifact}, std::nullopt, std::nullopt);
        REQUIRE(persisted.status == WriteReceipt::Status::Committed);
        const auto selected = action.SelectResult(*writer, {persisted.id}, {}, "done",
                                                  std::nullopt);
        REQUIRE(selected.status == WriteReceipt::Status::Committed);
        tool_message_id = writer->NewMessageId();
        // tool 消息带 resultSelectionRef + 预留 id(§4.18/§4.19)。
        MessageDraft tool;
        tool.message_id_override = tool_message_id;
        tool.turn_id = "turn-000001";
        tool.step_id = "step-000001";
        tool.action_id = "action-000001";
        tool.purpose = MessagePurpose::Conversation;
        tool.origin = MessageOrigin::SessionRuntime;
        tool.message = nlohmann::json::object(
            {{"role", "tool"}, {"tool_call_id", "action-000001"}, {"content", "报告见产物"}});
        tool.result_selection_ref = selected.id;
        REQUIRE(writer->AppendMessage(std::move(tool), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
        REQUIRE(writer->AdmitMessages({tool_message_id}).status ==
                WriteReceipt::Status::Committed);
    }
    const auto ledger = ReadV3Ledger(root.Ledger("S-TOOL"));
    REQUIRE(ledger.has_value());
    // 折叠快照:一条尝试链全终态,结果选用链可追。
    const auto snapshots = FoldToolActions(*ledger);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].folded_status == "done");
    CHECK(snapshots[0].selected_event_ref.has_value());
    // 缺 blob:实探目录,artifact 不在 → missing_blob,complete=false。
    const auto preview = ExpandResultPreview(*ledger, root.Dir("S-TOOL"), tool_message_id);
    REQUIRE(preview.artifacts.size() == 1);
    CHECK_FALSE(preview.artifacts[0].exists);
    CHECK(preview.artifacts[0].gap_reason == "missing_blob");
    CHECK_FALSE(preview.complete);
}

// ---------------------------------------------------------------------------
// 冻结清单:两层子代理
// ---------------------------------------------------------------------------

TEST_CASE("夹具: 两层子代理可递归遍历,父子身份不串") {
    SessionsRoot root("two-level-subagent");
    {
        auto parent = StartSession(root, "S-PARENT");
        REQUIRE(parent.has_value());
        InstallRound(*parent, "turn-000001", "派个孩子去查", /*with_usage=*/true);

        // 第一层:父 → 子。
        ChildSessionRef child_ref{"S-CHILD", "run-S-CHILD", "subagents/S-CHILD/S-CHILD.jsonl"};
        ParentActionRef parent_action{"S-PARENT", "run-S-PARENT", "turn-000001", "step-000001",
                                      "action-000001", "msg-000001"};
        auto spawn = SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                            "step-000001", "task-000001", child_ref,
                                            parent_action, nlohmann::json::object(),
                                            nlohmann::json::object());
        auto boot = spawn.BootstrapChild(*parent, "run-S-CHILD", "你是子代理一层。",
                                         "去查第一层。");
        REQUIRE(boot.child_writer.has_value());
        REQUIRE(spawn.Link(*parent, boot.checkpoint).status == WriteReceipt::Status::Committed);

        // 第二层:子 → 孙(孙账开在子目录 subagents/ 下,每层完整布局)。
        ChildSessionRef grandchild_ref{"S-GRAND", "run-S-GRAND",
                                       "subagents/S-GRAND/S-GRAND.jsonl"};
        ParentActionRef child_action{"S-CHILD", "run-S-CHILD", "turn-000002", "step-000002",
                                     "action-000002", "msg-000002"};
        auto spawn2 = SubagentSpawn::Request(*boot.child_writer, "action-000002", "turn-000002",
                                             "step-000002", "task-000002", grandchild_ref,
                                             child_action, nlohmann::json::object(),
                                             nlohmann::json::object());
        auto boot2 = spawn2.BootstrapChild(*boot.child_writer, "run-S-GRAND",
                                           "你是子代理二层。", "再往下查一层。");
        REQUIRE(boot2.child_writer.has_value());
        REQUIRE(spawn2.Link(*boot.child_writer, boot2.checkpoint).status ==
                WriteReceipt::Status::Committed);
    }
    // 递归遍历:root 一步进,两层全见。
    const auto tree = WalkSessionTree(root.Ledger("S-PARENT"));
    CHECK(tree.session_id == "S-PARENT");
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].session_id == "S-CHILD");
    REQUIRE(tree.children[0].children.size() == 1);
    CHECK(tree.children[0].children[0].session_id == "S-GRAND");
    CHECK(tree.link_status == "linked");
    CHECK(tree.children[0].link_status == "linked");
}

// ---------------------------------------------------------------------------
// 冻结清单:两次 resume 来源链
// ---------------------------------------------------------------------------

TEST_CASE("夹具: 两次 resume 的来源链可回溯,逐级验 hash 无重复") {
    SessionsRoot root("resume-chain");
    {
        auto oldest = StartSession(root, "S-OLDEST");
        REQUIRE(oldest.has_value());
        InstallRound(*oldest, "turn-000001", "最早的一问", /*with_usage=*/true);
    }
    {
        const auto source = ReadV3Ledger(root.Ledger("S-OLDEST"));
        REQUIRE(source.has_value());
        auto middle = StartSession(root, "S-MIDDLE");
        REQUIRE(middle.has_value());
        AttachSource(*middle, *source);
        InstallRound(*middle, "turn-000001", "中间场的一问", /*with_usage=*/true);
    }
    {
        const auto source = ReadV3Ledger(root.Ledger("S-MIDDLE"));
        REQUIRE(source.has_value());
        auto newest = StartSession(root, "S-NEWEST");
        REQUIRE(newest.has_value());
        AttachSource(*newest, *source);
        InstallRound(*newest, "turn-000001", "最新场的一问", /*with_usage=*/true);
    }
    const auto projection = ProjectResume(root.Ledger("S-NEWEST"));
    REQUIRE(projection.has_value());
    REQUIRE(projection->source_chain.size() == 2);
    CHECK(projection->source_chain[0].session_id == "S-MIDDLE");  // 直接源在前
    CHECK(projection->source_chain[1].session_id == "S-OLDEST");  // 祖先在后
    CHECK(projection->source_chain_ok);
    CHECK(projection->source_chain[0].check.ok);
    CHECK(projection->source_chain[1].check.ok);
    CHECK_FALSE(projection->source_chain[0].duplicate);
}

// ---------------------------------------------------------------------------
// 冻结清单:坏尾 / 坏中段
// ---------------------------------------------------------------------------

TEST_CASE("夹具: 坏尾(截断)与坏中段(改字节)都过不了验卷") {
    SessionsRoot root("corrupt");
    root.Ledger("S-GOOD");  // 占位:同根下做一份好账,两份坏拷贝
    std::string good_bytes;
    {
        auto writer = StartSession(root, "S-GOOD");
        REQUIRE(writer.has_value());
        InstallRound(*writer, "turn-000001", "将被弄坏的一问", /*with_usage=*/true);
        good_bytes = ReadFileBytes(root.Ledger("S-GOOD"));
    }
    REQUIRE(ReadV3Ledger(root.Ledger("S-GOOD")).has_value());  // 原账可读(对照)

    // 坏尾:去掉末行换行(崩溃截断形状)。
    {
        const std::filesystem::path dir = root.Dir("S-BADTAIL");
        WriteFileBytes(dir / "S-BADTAIL.jsonl", good_bytes.substr(0, good_bytes.size() - 5));
        const auto report = VerifyV3File(dir / "S-BADTAIL.jsonl");
        CHECK_FALSE(report.ok);
        CHECK(report.truncated_tail);
        CHECK_FALSE(ReadV3Ledger(dir / "S-BADTAIL.jsonl").has_value());
    }
    // 坏中段:改中段一行的正文字节(哈希链必断)。
    {
        const std::filesystem::path dir = root.Dir("S-BADMID");
        std::string bytes = good_bytes;
        const std::size_t middle = bytes.find("将被弄坏的一问");
        REQUIRE(middle != std::string::npos);
        bytes[middle] = 'X';
        WriteFileBytes(dir / "S-BADMID.jsonl", bytes);
        const auto report = VerifyV3File(dir / "S-BADMID.jsonl");
        CHECK_FALSE(report.ok);
        CHECK_FALSE(ReadV3Ledger(dir / "S-BADMID.jsonl").has_value());
    }
}

// ---------------------------------------------------------------------------
// 冻结清单:锁态
// ---------------------------------------------------------------------------

TEST_CASE("夹具: SessionLock 活锁可探、释放即消(v3 场删除/归档门的锁面)") {
    SessionsRoot root("lock-state");
    const auto dir = root.Dir("S-LOCKED");
    {
        lubancode::trajectory::SessionLockOwner owner;
        owner.pid = lubancode::platform::CurrentProcessId();
        owner.process_start_token = lubancode::trajectory::CurrentProcessStartToken();
        owner.acquired_at_ms = 1759468800000LL;
        auto lock = lubancode::trajectory::SessionLock::Acquire(dir, owner);
        REQUIRE(lock.has_value());
        const auto holder = lubancode::trajectory::SessionLock::Inspect(dir);
        REQUIRE(holder.has_value());
        CHECK(lubancode::trajectory::ProbeLockHolder(*holder) ==
              lubancode::trajectory::LockHolderState::Alive);
    }  // 析构即释放
    CHECK_FALSE(lubancode::trajectory::SessionLock::Inspect(dir).has_value());
    // 归档态(T15-B 前):v3 场无生产归档形状,清单记缺口,不伪造。
}
