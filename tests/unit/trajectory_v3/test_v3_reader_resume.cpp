// v3 读取侧 resume 测试(P2,§4.10/§4.59):resume 分别恢复执行状态/
// 模型输入/历史索引;压缩后 resume 原文可查、请求不重携全史、token 标记
// 在;resume 后再 resume 来源链可遍历、无重复显示、seq 不跨文件混排;
// §4.59 tool1/2 配齐 tool3/4 未启动 → 恢复从第三项续,不重跑。
#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct SessionsRoot {
    FixedClock clock;
    std::filesystem::path root;  // sessions/<id>/<id>.jsonl

    explicit SessionsRoot(const char* tag) {
        root = std::filesystem::temp_directory_path() / ("lubancode-v3-resume-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    std::filesystem::path Ledger(const std::string& session_id) {
        std::filesystem::path dir = root / session_id;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / (session_id + ".jsonl");
    }
};

// 一轮"user → assistant → 接纳"的最小对话。
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
    assistant.step_id = "step-000001";
    assistant.request_id = "request-000001";
    assistant.purpose = MessagePurpose::Conversation;
    assistant.origin = MessageOrigin::SessionRuntime;
    assistant.provider = "stub";
    assistant.wire = "openai";
    assistant.model = "stub-mini";
    assistant.response_model = nlohmann::json(nullptr);
    assistant.usage = with_usage
                          ? std::optional<nlohmann::json>(nlohmann::json(
                                {{"inputTokens", 120}, {"outputTokens", 30}}))
                          : std::optional<nlohmann::json>(nlohmann::json(nullptr));
    assistant.message = nlohmann::json::object({{"role", "assistant"},
                                                {"content", "收到:" + user_text}});
    WriteReceipt assistant_receipt =
        writer.AppendMessage(std::move(assistant), Durability::PowerLoss);
    REQUIRE(assistant_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({assistant_receipt.id}).status == WriteReceipt::Status::Committed);
    return assistant_receipt.id;
}

// resume.source.attached:五键指源末行(§4.10 步 2)。
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

std::optional<V3Writer> StartSession(SessionsRoot& root, const std::string& session_id) {
    auto writer = V3Writer::Start(root.Ledger(session_id), session_id, "run-" + session_id,
                                  "你是 LubanCode。", nlohmann::json::object(),
                                  V3WriterOptions{}, &root.clock);
    if (!writer.has_value()) {
        return std::nullopt;
    }
    return std::move(*writer);
}

}  // namespace

// ---------------------------------------------------------------------------
// 5.1 行 1:普通 resume——旧 user/assistant/tool 能滚动查看,ID 和顺序不变
// ---------------------------------------------------------------------------

TEST_CASE("普通 resume:三恢复各归各,历史 ID 与顺序不变") {
    SessionsRoot root("plain");
    std::string tool_message_id;
    {
        auto writer = StartSession(root, "S1");
        REQUIRE(writer.has_value());
        InstallRound(*writer, "turn-000001", "查一下天气", true);
        // 一轮工具。
        ToolActionSession action = ToolActionSession::Admit(
            *writer, "turn-000001", "step-000001", "action-000001", "queued", std::nullopt,
            std::nullopt);
        REQUIRE(action.Start(*writer, "args", ToolIdentity{"weather", "builtin", "1.0", ""})
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, 0, 20).status == WriteReceipt::Status::Committed);
        WriteReceipt persisted = action.PersistedResult(
            *writer,
            {MakeArtifactRef("res-000001", "result_metadata", "artifacts/res-000001.json",
                             std::string(64, '1'), 100, "application/json")},
            action.last_event_id());
        REQUIRE(persisted.status == WriteReceipt::Status::Committed);
        WriteReceipt selected = action.SelectResult(*writer, {persisted.id}, {}, "done");
        REQUIRE(selected.status == WriteReceipt::Status::Committed);
        WriteReceipt message =
            action.AppendToolMessage(*writer, "晴,26 度", action.selected_event_id());
        REQUIRE(message.status == WriteReceipt::Status::Committed);
        tool_message_id = message.id;
    }
    // 模拟 resume:新场 C 记 resume.source.attached → 读侧 ProjectResume。
    {
        auto writer = StartSession(root, "S2");
        REQUIRE(writer.has_value());
        auto source = ReadV3Ledger(root.Ledger("S1"));
        REQUIRE(source.has_value());
        AttachSource(*writer, *source);
    }
    auto resume = ProjectResume(root.Ledger("S2"));
    REQUIRE(resume.has_value());
    // ① 历史索引:源场的消息沿来源链可滚动,ID/顺序原样(§4.10 步 3)。
    REQUIRE(resume->source_chain.size() == 1);
    CHECK(resume->source_chain_ok);
    const ResumeSourceStep& step = resume->source_chain[0];
    CHECK(step.check.ok);
    REQUIRE(step.ledger.has_value());
    HistoryTimeline source_timeline = ProjectHistoryTimeline(*step.ledger);
    std::vector<std::string> ids;
    for (const auto& item : source_timeline.items) {
        if (item.kind == HistoryTimeline::Item::Kind::Message) {
            ids.push_back(item.id);
        }
    }
    REQUIRE(ids.size() == 4);  // system + user + assistant + tool
    CHECK(ids[0] == "msg-000001");
    CHECK(ids[1] == "msg-000002");
    CHECK(ids[2] == "msg-000003");
    CHECK(ids[3] == tool_message_id);
    // ② 模型输入:新场自己的链(system),请求不重携源场全史(§4.10)。
    REQUIRE(resume->model_context.inputs.empty());
    // ③ 执行状态:源场工具已配齐,无未收口工作。
    CHECK(resume->execution.open_actions.empty());
    CHECK(resume->execution.open_compact_ids.empty());
}

// ---------------------------------------------------------------------------
// §4.59:A(tool1..4),tool1/2 配齐、tool3/4 未启动 → 从第三项续
// ---------------------------------------------------------------------------

TEST_CASE("执行状态恢复:tool1/2 完整,tool3/4 只 pending,不重跑配齐项") {
    SessionsRoot root("batch");
    {
        auto writer = StartSession(root, "B1");
        REQUIRE(writer.has_value());
        std::string assistant_id = InstallRound(*writer, "turn-000001", "跑四项检查", true);
        for (int i = 1; i <= 4; ++i) {
            const std::string action_id = "action-00000" + std::to_string(i);
            ToolActionSession action = ToolActionSession::Admit(
                *writer, "turn-000001", "step-000001", action_id, "queued", assistant_id,
                "call_prov_" + std::to_string(i));
            if (i >= 3) {
                continue;  // tool3/4:已接纳从未 started(§4.61"未启动")
            }
            REQUIRE(action.Start(*writer, "args-" + std::to_string(i),
                                 ToolIdentity{"check", "builtin", "1.0", ""})
                        .status == WriteReceipt::Status::Committed);
            REQUIRE(action.Finish(*writer, 0, 10 + i).status == WriteReceipt::Status::Committed);
            WriteReceipt persisted = action.PersistedResult(
                *writer,
                {MakeArtifactRef("res-00000" + std::to_string(i), "result_metadata",
                                 "artifacts/res-00000" + std::to_string(i) + ".json",
                                 std::string(64, '1'), 50, "application/json")},
                action.last_event_id());
            REQUIRE(persisted.status == WriteReceipt::Status::Committed);
            WriteReceipt selected = action.SelectResult(*writer, {persisted.id}, {}, "done");
            REQUIRE(selected.status == WriteReceipt::Status::Committed);
            REQUIRE(action.AppendToolMessage(*writer, "ok " + std::to_string(i),
                                             action.selected_event_id())
                        .status == WriteReceipt::Status::Committed);
        }
    }
    auto resume = ProjectResume(root.Ledger("B1"));
    REQUIRE(resume.has_value());
    // 恢复决策(§4.59):tool3/4 恢复待执行;tool1/2 不重跑、不让模型重新
    // 生成整组调用。
    REQUIRE(resume->execution.open_actions.size() == 2);
    CHECK(resume->execution.open_actions[0].tool_call_id == "action-000003");
    CHECK(resume->execution.open_actions[1].tool_call_id == "action-000004");
    CHECK(resume->execution.open_actions[0].folded_status == "pending");
    CHECK(resume->execution.open_actions[0].attempts[0].pending_reason == "queued");
    CHECK_FALSE(resume->execution.open_actions[0].attempts[0].started);
    // 有未收口工作的 turn。
    REQUIRE(resume->execution.turns_with_open_work.size() == 2);
    CHECK(resume->execution.turns_with_open_work[0] == "turn-000001");
    // 配齐项在折叠账里是 done(含消息版本),不进 open——模型不重生成整组。
    auto ledger = ReadV3Ledger(root.Ledger("B1"));
    REQUIRE(ledger.has_value());
    std::vector<ToolActionSnapshot> all = FoldToolActions(*ledger);
    const ToolActionSnapshot* first = FindActionSnapshot(all, "action-000001");
    REQUIRE(first != nullptr);
    CHECK(first->folded_status == "done");
    REQUIRE(first->message_versions.size() == 1);
    CHECK(first->message_versions[0].on_current_chain);
}

// ---------------------------------------------------------------------------
// 5.1 行 18:compact 后 resume
// ---------------------------------------------------------------------------

TEST_CASE("压缩后 resume:原文可查、请求不重携全史、token 标记在") {
    // 直接吃 compact_full fixture:它就是"applied 后、内存发布前崩溃,
    // resume 从 applied 重建新上下文"的场(§2.3)。
    std::filesystem::path fixture = std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" /
                                    "fixtures" / "trajectory_v3" / "compact_full.jsonl";
    auto resume = ProjectResume(fixture, [](const std::string&) {
        return std::filesystem::path();  // 无来源链;本账自足
    });
    REQUIRE(resume.has_value());
    // 历史全在:被压缩原文(msg-000002/3)仍在时间线可滚动查看。
    auto ledger = ReadV3Ledger(fixture);
    REQUIRE(ledger.has_value());
    HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    auto removed = timeline.message_items.find("msg-000002");
    REQUIRE(removed != timeline.message_items.end());
    CHECK_FALSE(timeline.items[removed->second].message.in_current_context);
    // 模型输入:system + 摘要 + 保留 + 新输入 = 5 枚,不是全史 9 枚
    //(§5.1"请求不重新携带全部旧史")。
    REQUIRE(resume->model_context.inputs.size() == 4);
    CHECK(resume->model_context.inputs.size() < ledger->messages.size() - 1);
    // token 标记仍在:读 applied 持久字段,resume 不重算(§4.11)。
    REQUIRE(resume->compact_markers.size() == 1);
    CHECK(resume->compact_markers[0].context_tokens_before == 142800);
    CHECK(resume->compact_markers[0].context_tokens_after == 31600);
    CHECK(resume->execution.open_compact_ids.empty());  // applied 已收口
    CHECK(resume->execution.open_actions.empty());
}

TEST_CASE("压缩未终态:open compact 按源上下文恢复,不冒充主链") {
    SessionsRoot root("opencompact");
    {
        auto writer = StartSession(root, "OC1");
        REQUIRE(writer.has_value());
        InstallRound(*writer, "turn-000001", "干活", true);
        auto begun = CompactSession::Begin(*writer, "manual", "user_command", std::nullopt,
                                           nlohmann::json::object({{"fields", {"goal"}}}));
        REQUIRE(begun.info.began);
    }
    auto resume = ProjectResume(root.Ledger("OC1"));
    REQUIRE(resume.has_value());
    REQUIRE(resume->execution.open_compact_ids.size() == 1);
    CHECK(resume->execution.open_compact_ids[0] == "compact-000001");
    // 主链不受内部回合影响:还是 system + user + assistant。
    REQUIRE(resume->model_context.inputs.size() == 2);
}

// ---------------------------------------------------------------------------
// 5.1 行 21:resume 后再 resume
// ---------------------------------------------------------------------------

TEST_CASE("resume 后再 resume:来源链可遍历,无重复,seq 不混排") {
    SessionsRoot root("chain");
    {
        auto a = StartSession(root, "A");
        REQUIRE(a.has_value());
        InstallRound(*a, "turn-000001", "第一场", true);
    }
    {
        auto b = StartSession(root, "B");
        REQUIRE(b.has_value());
        AttachSource(*b, ReadV3Ledger(root.Ledger("A")).value());
        InstallRound(*b, "turn-000001", "第二场", true);
    }
    {
        auto c = StartSession(root, "C");
        REQUIRE(c.has_value());
        AttachSource(*c, ReadV3Ledger(root.Ledger("B")).value());
        InstallRound(*c, "turn-000001", "第三场", true);
    }
    auto resume = ProjectResume(root.Ledger("C"));
    REQUIRE(resume.has_value());
    // 来源链:C → B → A,直接源在前;全部验过、无重复(§4.10 步 2/§4.59)。
    REQUIRE(resume->source_chain.size() == 2);
    CHECK(resume->source_chain_ok);
    CHECK(resume->source_chain[0].session_id == "B");
    CHECK(resume->source_chain[1].session_id == "A");
    CHECK(resume->source_chain[0].check.ok);
    CHECK(resume->source_chain[1].check.ok);
    CHECK_FALSE(resume->source_chain[0].duplicate);
    CHECK_FALSE(resume->source_chain[1].duplicate);
    // seq 各归各:三本账各自 seq 都从 1 起,不按同名 seq 全局排序(§4.10)。
    REQUIRE(resume->source_chain[1].ledger.has_value());
    CHECK(resume->source_chain[1].ledger->timeline.front().seq == 1);
    CHECK(resume->timeline.items.front().seq == 1);
    // 祖先的历史沿链可读:B 的时间线里有 A 的消息吗?不——B 的时间线是
    // B 自己的账;祖先消息在 A 的账里,由链上一并给出,不混进 C。
    REQUIRE(resume->source_chain[0].ledger.has_value());
    CHECK(resume->source_chain[0].ledger->session_id == "B");
}

TEST_CASE("来源链环:回指已访问的会话 → 标重复,不再下钻") {
    SessionsRoot root("cycle");
    {
        auto a = StartSession(root, "A");
        REQUIRE(a.has_value());
        InstallRound(*a, "turn-000001", "第一场", true);
    }
    {
        auto b = StartSession(root, "B");
        REQUIRE(b.has_value());
        AttachSource(*b, ReadV3Ledger(root.Ledger("A")).value());
    }
    {
        auto c = StartSession(root, "C");
        REQUIRE(c.has_value());
        AttachSource(*c, ReadV3Ledger(root.Ledger("B")).value());
    }
    // 给 A 补一笔指向 C 的来源(人为造环:A→C→B→A)。
    {
        auto continued = V3Writer::Continue(root.Ledger("A"), V3WriterOptions{}, nullptr);
        REQUIRE(continued.has_value());
        AttachSource(*continued, ReadV3Ledger(root.Ledger("C")).value());
    }
    auto resume = ProjectResume(root.Ledger("C"));
    REQUIRE(resume.has_value());
    REQUIRE(resume->source_chain.size() == 3);  // B、A、C(环上最后一步)
    CHECK_FALSE(resume->source_chain_ok);
    CHECK(resume->source_chain[0].session_id == "B");
    CHECK(resume->source_chain[1].session_id == "A");
    CHECK(resume->source_chain[2].session_id == "C");
    CHECK(resume->source_chain[2].duplicate);  // 无重复显示(§4.10"检查环")
    CHECK_FALSE(resume->source_chain[2].ledger.has_value());
}

TEST_CASE("来源链坏引用:hash 对不上 → 链标坏,本账恢复不受影响") {
    SessionsRoot root("badhash");
    {
        auto a = StartSession(root, "A");
        REQUIRE(a.has_value());
        InstallRound(*a, "turn-000001", "第一场", true);
    }
    {
        auto b = StartSession(root, "B");
        REQUIRE(b.has_value());
        // 伪造五键:hash 错。
        EventDraft attached;
        attached.kind = EventKindV3::ResumeSourceAttached;
        attached.payload = nlohmann::json::object(
            {{"sourceRef", nlohmann::json::object({{"sessionId", "A"},
                                                   {"runId", std::string("run-A")},
                                                   {"seq", 3},
                                                   {"id", "msg-000002"},
                                                   {"hash", std::string(64, 'f')}})},
             {"contextRevision", 3},
             {"systemMessageRef", "msg-000001"},
             {"branch", "main"}});
        REQUIRE(b->AppendEvent(std::move(attached), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    }
    auto resume = ProjectResume(root.Ledger("B"));
    REQUIRE(resume.has_value());
    REQUIRE(resume->source_chain.size() == 1);
    CHECK_FALSE(resume->source_chain_ok);
    CHECK_FALSE(resume->source_chain[0].check.ok);
    CHECK(resume->source_chain[0].check.reason == "hash_mismatch");
    // 本账上下文照常恢复(精确恢复用的是本账链,不是祖先全史)。
    CHECK(resume->model_context.revision == 1);
    CHECK(resume->model_context.inputs.empty());
}

TEST_CASE("selected 无消息:补消息,不重跑(§4.59 折叠态)") {
    SessionsRoot root("selected");
    {
        auto writer = StartSession(root, "SEL1");
        REQUIRE(writer.has_value());
        ToolActionSession action = ToolActionSession::Admit(
            *writer, "turn-000001", "step-000001", "action-000001", "queued", std::nullopt,
            std::nullopt);
        REQUIRE(action.Start(*writer, "args", ToolIdentity{"check", "builtin", "1.0", ""})
                    .status == WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, 0, 10).status == WriteReceipt::Status::Committed);
        WriteReceipt persisted = action.PersistedResult(
            *writer,
            {MakeArtifactRef("res-000001", "result_metadata", "artifacts/res-000001.json",
                             std::string(64, '2'), 40, "application/json")},
            action.last_event_id());
        REQUIRE(persisted.status == WriteReceipt::Status::Committed);
        WriteReceipt selected = action.SelectResult(*writer, {persisted.id}, {}, "done");
        REQUIRE(selected.status == WriteReceipt::Status::Committed);
        // 崩溃:结果已选用,tool 消息未落(§4.20"最终结果选用后、tool 消息
        // 前崩溃 → 补交同调用结果一次,不重跑工具或 hook")。
    }
    auto resume = ProjectResume(root.Ledger("SEL1"));
    REQUIRE(resume.has_value());
    REQUIRE(resume->execution.open_actions.size() == 1);
    CHECK(resume->execution.open_actions[0].folded_status == "selected_no_message");
    CHECK(resume->execution.open_actions[0].selected_event_ref.has_value());
}
