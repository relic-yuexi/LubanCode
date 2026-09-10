// v3 compact 运行时接线测试(compact 全链单):RunV3Compact 全链——
// manual/auto 触发与 reason 落档、空闲手动不伪造 protected turn、turn 中途
// 主 turn 不变、连续两次压缩旧摘要入料(§4.41)、只有旧摘要可重压与收益
// 不足拒收、usage 缺失不补 0、§4.64 撞窗整轮回退(8k 目标/不拆整轮/参考
// 尾部移出)与退空拒绝(不空调模型)、no_eligible_history、busy、applied
// 写盘失败的恢复账(候选可查、旧上下文仍有效、未 applied 不生效)。
// 崩溃注入只做到事件账与恢复逻辑;真进程注入跑法留验收单。
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/v3_compact_runtime.hpp"
#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/reader.hpp"
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

// 压缩模型桩:回什么、截断与否、usage 给不给都听测试的。
class StubClient : public V3CompactModelClient {
public:
    int calls = 0;
    std::vector<nlohmann::json> last_messages;
    std::function<V3CompactModelReply()> respond = [] {
        return V3CompactModelReply{};
    };

    V3CompactModelReply Send(const std::string& system,
                             const std::vector<nlohmann::json>& messages) override {
        (void)system;
        ++calls;
        last_messages = messages;
        return respond();
    }
};

// 合格摘要回复:正文 + 末尾 manifest 围栏(与缺省校验清单同形)。
std::string ManifestReply(const std::string& body, const std::vector<std::string>& open_items) {
    nlohmann::json manifest = nlohmann::json::object(
        {{"goal", "把会话接下去"}, {"constraints", nlohmann::json::array({"不许动旧档"})},
         {"open_items", open_items}, {"next_action", "继续干活"}});
    return body + "\n```json\n" + manifest.dump() + "\n```\n";
}

struct Harness {
    FixedClock clock;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("lubancode-v3-compact-runtime-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
    }

    std::optional<V3Writer> Start(V3WriterOptions options = V3WriterOptions{}) {
        auto writer = V3Writer::Start(jsonl, "20260910-120000-AAAAAA", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(), options,
                                      &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
    }

    // 一轮完整对话(user + assistant)接纳进链,返回两枚消息 id。
    std::pair<std::string, std::string> SeedTurn(V3Writer& writer, const std::string& turn_id,
                                                 const std::string& user_text,
                                                 const std::string& assistant_text = "收到。") {
        MessageDraft user;
        user.turn_id = turn_id;
        user.purpose = MessagePurpose::Conversation;
        user.origin = MessageOrigin::Human;
        user.message = nlohmann::json::object({{"role", "user"}, {"content", user_text}});
        WriteReceipt user_receipt = writer.AppendMessage(std::move(user), Durability::PowerLoss);
        REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);

        MessageDraft assistant;
        assistant.turn_id = turn_id;
        assistant.request_id = "request-seed";
        assistant.provider = "moonshot";
        assistant.wire = "openai-chat-completions";
        assistant.model = "kimi-k2.6";
        assistant.response_model = nlohmann::json("kimi-k2.6");
        assistant.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
        assistant.origin = MessageOrigin::SessionRuntime;
        assistant.message =
            nlohmann::json::object({{"role", "assistant"}, {"content", assistant_text}});
        WriteReceipt assistant_receipt =
            writer.AppendMessage(std::move(assistant), Durability::PowerLoss);
        REQUIRE(assistant_receipt.status == WriteReceipt::Status::Committed);

        REQUIRE(writer.AdmitMessages({user_receipt.id, assistant_receipt.id}).status ==
                WriteReceipt::Status::Committed);
        return {user_receipt.id, assistant_receipt.id};
    }

    // 一轮带工具对的对话:assistant(tool_calls) + tool 消息同 turn 接纳。
    std::pair<std::string, std::string> SeedToolTurn(V3Writer& writer, const std::string& turn_id,
                                                     const std::string& action_id) {
        MessageDraft user;
        user.turn_id = turn_id;
        user.purpose = MessagePurpose::Conversation;
        user.origin = MessageOrigin::Human;
        user.message = nlohmann::json::object({{"role", "user"}, {"content", "查一下"}});
        WriteReceipt user_receipt = writer.AppendMessage(std::move(user), Durability::PowerLoss);
        REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);

        MessageDraft declaration;
        declaration.turn_id = turn_id;
        declaration.action_id = action_id;
        declaration.request_id = "request-seed";
        declaration.provider = "moonshot";
        declaration.wire = "openai-chat-completions";
        declaration.model = "kimi-k2.6";
        declaration.response_model = nlohmann::json(nullptr);
        declaration.usage = nlohmann::json(nullptr);
        declaration.origin = MessageOrigin::SessionRuntime;
        declaration.message = nlohmann::json::object(
            {{"role", "assistant"},
             {"tool_calls", nlohmann::json::array({nlohmann::json::object(
                                {{"id", action_id},
                                 {"function", nlohmann::json::object({{"name", "search"},
                                                                      {"arguments", "{}"}})}})})}});
        WriteReceipt declaration_receipt =
            writer.AppendMessage(std::move(declaration), Durability::PowerLoss);
        REQUIRE(declaration_receipt.status == WriteReceipt::Status::Committed);

        MessageDraft tool;
        tool.turn_id = turn_id;
        tool.action_id = action_id;
        tool.origin = MessageOrigin::SessionRuntime;
        tool.message = nlohmann::json::object(
            {{"role", "tool"}, {"tool_call_id", action_id}, {"content", "结果正文"}});
        WriteReceipt tool_receipt = writer.AppendMessage(std::move(tool), Durability::PowerLoss);
        REQUIRE(tool_receipt.status == WriteReceipt::Status::Committed);

        REQUIRE(writer.AdmitMessages({user_receipt.id, declaration_receipt.id, tool_receipt.id})
                    .status == WriteReceipt::Status::Committed);
        return {user_receipt.id, tool_receipt.id};
    }
};

std::vector<nlohmann::json> ReadJsonLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

V3CompactProfile BaseProfile() {
    V3CompactProfile profile;
    profile.provider = "moonshot";
    profile.wire = "openai-chat-completions";
    profile.model = "kimi-k2.6";
    profile.compact_window_tokens = 0;  // 门禁关:成功链路不掺容量变量
    return profile;
}

V3CompactRunInput ManualInput() {
    V3CompactRunInput input;
    input.trigger = "manual";
    input.reason = "user_command";
    return input;
}

// 大块正文:摘要 + manifest 围栏天然有百余 token 的底,迷你史压了反而
// 涨(收益校验会正当拦下);材料给足体量,压缩才有得赚。
std::string BigText(std::size_t bytes, char fill = 'x') { return std::string(bytes, fill); }

// 找指定 kind 的事件行(全部)。
std::vector<const nlohmann::json*> EventsOf(const std::vector<nlohmann::json>& lines,
                                            const std::string& kind) {
    std::vector<const nlohmann::json*> found;
    for (const auto& line : lines) {
        if (line.value("kind", "") == kind) {
            found.push_back(&line);
        }
    }
    return found;
}

}  // namespace

// ---------------------------------------------------------------------------
// §5.1 验收行:一次 compact 成功(requested、完整 prompt、回复、校验、
// 摘要、applied 引用可闭合)+ 手动空闲不伪造 protected turn + marker 字段。
// ---------------------------------------------------------------------------
TEST_CASE("手动空闲 compact 全链一次成功:八类行闭合,marker 字段落全") {
    Harness harness("full");
    std::string turn1_user, turn1_assistant, turn2_user, turn2_assistant;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        std::tie(turn1_user, turn1_assistant) =
            harness.SeedTurn(*writer, "turn-000001", BigText(600));
        std::tie(turn2_user, turn2_assistant) =
            harness.SeedTurn(*writer, "turn-000002", BigText(600, 'y'));

        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 交接摘要\n- 用户定了方案又改了方案", {"继续干活"});
            reply.usage = nlohmann::json::object({{"inputTokens", 500}, {"outputTokens", 40}});
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(result.applied);
        CHECK(result.terminal_kind == "applied");
        CHECK(result.model_calls == 1);
        CHECK(result.tokens_before > result.tokens_after);
        CHECK(result.tokens_after > 0);
        // 空闲手动:protectedTurnIds 空,不伪造未完成 turn(§4.9)。
        CHECK(writer->context().chain.size() == 2);  // system + 摘要
        CHECK(writer->context().open_compact_ids.empty());
        CHECK(client.last_messages.size() == 5);  // 4 条材料 + 1 条指令
        CHECK(client.last_messages.back().value("role", "") == "user");
    }
    // resume:applied 后从新链重建;候选/内部问答不进链(§4.9)。
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    CHECK(continued->context().chain.size() == 2);

    auto lines = ReadJsonLines(harness.jsonl);
    REQUIRE(EventsOf(lines, "compact.requested").size() == 1);
    REQUIRE(EventsOf(lines, "compact.started").size() == 1);
    REQUIRE(EventsOf(lines, "model.request.prepared").size() == 1);
    REQUIRE(EventsOf(lines, "compact.validation.started").size() == 1);
    REQUIRE(EventsOf(lines, "compact.validation.completed").size() == 1);
    REQUIRE(EventsOf(lines, "compact.applied").size() == 1);

    // 候选三件套(D1 延伸):started/completed 恰各一,零 delta 批,
    // 定稿 messageId 与候选 assistant 成行同一枚。
    REQUIRE(EventsOf(lines, "model.response.started").size() == 1);
    REQUIRE(EventsOf(lines, "model.response.completed").size() == 1);
    CHECK(EventsOf(lines, "model.response.delta").empty());
    {
        const auto& started_row = *EventsOf(lines, "model.response.started").front();
        const auto& completed_row = *EventsOf(lines, "model.response.completed").front();
        CHECK(started_row["payload"]["messageId"] == completed_row["payload"]["messageId"]);
        CHECK(completed_row["payload"]["finishReason"] == "end_turn");
        bool candidate_finalized = false;
        const std::string finalized_id = completed_row["payload"]["messageId"].get<std::string>();
        for (const auto& line : lines) {
            if (line.value("type", "") == "message" &&
                line["message"].value("role", "") == "assistant" &&
                line.value("purpose", "") == "compact") {
                candidate_finalized = line.value("messageId", "") == finalized_id;
            }
        }
        CHECK(candidate_finalized);
    }

    const auto& requested = *EventsOf(lines, "compact.requested").front();
    CHECK(requested["payload"]["trigger"] == "manual");
    CHECK(requested["payload"]["reason"] == "user_command");
    CHECK(requested["payload"].contains("requirementsSnapshot"));
    CHECK(requested["turnId"].get<std::string>().rfind("compact-turn-", 0) == 0);
    // 空闲手动:parentTurnId 不假称挂主 turn(§4.6)。
    CHECK((!requested.contains("parentTurnId") || requested["parentTurnId"].is_null()));

    const auto& prepared = *EventsOf(lines, "model.request.prepared").front();
    CHECK(prepared["payload"]["purpose"] == "compact");
    CHECK(prepared["payload"]["inputMessageRefs"].size() == 5);
    // systemMessageRef 指压缩专用 system,不是会话 system(§4.3)。
    bool found_special_system = false;
    std::string special_system_id;
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"].value("role", "") == "system" &&
            line.value("purpose", "") == "compact") {
            found_special_system = true;
            special_system_id = line["messageId"];
            CHECK(line.value("origin", "") == "compact_runtime");
        }
    }
    REQUIRE(found_special_system);
    CHECK(prepared["payload"]["systemMessageRef"] == special_system_id);

    // 候选 assistant:usage 原样归属压缩模型,provider/wire/model 齐。
    bool found_candidate = false;
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"].value("role", "") == "assistant" &&
            line.value("purpose", "") == "compact") {
            found_candidate = true;
            CHECK(line["usage"]["inputTokens"] == 500);
            CHECK(line.value("provider", "") == "moonshot");
        }
    }
    CHECK(found_candidate);

    const auto& applied = *EventsOf(lines, "compact.applied").front();
    // removed 盖住两轮;retained/protected 空;marker 字段(显示侧压缩
    // 分界线吃的两枚 token 数)必须落全(§4.11)。
    CHECK(applied["payload"]["removedMessageRefs"].size() == 4);
    CHECK(applied["payload"]["removedMessageRefs"][0] == turn1_user);
    CHECK(applied["payload"]["removedMessageRefs"][1] == turn1_assistant);
    CHECK(applied["payload"]["removedMessageRefs"][2] == turn2_user);
    CHECK(applied["payload"]["removedMessageRefs"][3] == turn2_assistant);
    CHECK(applied["payload"]["retainedMessageRefs"].size() == 0);
    CHECK(applied["payload"]["protectedTurnIds"].size() == 0);
    CHECK(applied["payload"]["contextTokensBefore"].is_number_unsigned());
    CHECK(applied["payload"]["contextTokensAfter"].is_number_unsigned());
    CHECK(applied["payload"]["contextTokensAfter"] <
          applied["payload"]["contextTokensBefore"]);
    CHECK(applied["payload"]["tokenMetric"]["estimator"] == "utf8_bytes_div4");
    CHECK(applied["payload"]["trigger"] == "manual");

    // 校验详情留档(§4.8:失败项带详情;通过项也有 checks)。
    const auto& validation = *EventsOf(lines, "compact.validation.completed").front();
    CHECK(validation["payload"]["passed"] == true);
    CHECK(validation["payload"]["checks"].size() >= 6);

    // 读取侧:压缩标记(marker)吃 applied 持久字段(§4.10/§4.11)。
    auto ledger = ReadV3Ledger(harness.jsonl);
    REQUIRE(ledger.has_value());
    const HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    REQUIRE(timeline.compact_items.size() == 1);
    const auto& marker =
        timeline.items[timeline.compact_items.begin()->second].compact;
    CHECK(marker.context_tokens_before == applied["payload"]["contextTokensBefore"]);
    CHECK(marker.context_tokens_after == applied["payload"]["contextTokensAfter"]);
    CHECK(marker.removed_message_refs.size() == 4);
    CHECK(marker.trigger == "manual");

    CHECK(VerifyV3File(harness.jsonl).ok);
}

// ---------------------------------------------------------------------------
// §5.1 验收行:turn 中途自动 compact,主 turn ID 不变,内部 turn 不冒充
// 真人回合;protectedTurnIds 如实记主 turn。
// ---------------------------------------------------------------------------
TEST_CASE("turn 中途自动 compact:主 turn 保留,内部回合挂 parentTurnId") {
    Harness harness("midturn");
    std::string open_user, open_assistant;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", BigText(600));
        std::tie(open_user, open_assistant) =
            harness.SeedTurn(*writer, "turn-000002", BigText(600, 'y'));

        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 摘要\n- 旧一轮已并入", {});
            reply.usage = nlohmann::json(nullptr);  // provider usage 缺失
            return reply;
        };
        V3CompactRunInput input;
        input.trigger = "auto";
        input.reason = "pre_send_overflow";
        input.parent_turn_id = "turn-000002";
        input.protected_turn_ids = {"turn-000002"};
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), std::move(input));
        REQUIRE(result.applied);

        // 主 turn 的消息原样保留在新链尾,ID 不变;摘要接 system。
        REQUIRE(writer->context().chain.size() == 4);
        CHECK(writer->context().chain[1].message_ref != open_user);   // 摘要在前
        CHECK(writer->context().chain[2].message_ref == open_user);   // 主 turn 原序
        CHECK(writer->context().chain[3].message_ref == open_assistant);
    }
    auto lines = ReadJsonLines(harness.jsonl);
    const auto& requested = *EventsOf(lines, "compact.requested").front();
    CHECK(requested["payload"]["trigger"] == "auto");
    CHECK(requested["payload"]["reason"] == "pre_send_overflow");
    CHECK(requested["parentTurnId"] == "turn-000002");
    const auto& applied = *EventsOf(lines, "compact.applied").front();
    CHECK(applied["payload"]["protectedTurnIds"].size() == 1);
    CHECK(applied["payload"]["protectedTurnIds"][0] == "turn-000002");
    CHECK(applied["payload"]["retainedMessageRefs"].size() == 2);
    CHECK(applied["payload"]["removedMessageRefs"].size() == 2);
    // usage 缺失:候选 assistant 的 usage 键为 null,不补 0(§5.1)。
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"].value("role", "") == "assistant" &&
            line.value("purpose", "") == "compact") {
            REQUIRE(line.contains("usage"));
            CHECK(line["usage"].is_null());
        }
    }
    CHECK(VerifyV3File(harness.jsonl).ok);
}

// ---------------------------------------------------------------------------
// §5.1 验收行:连续两次 compact——第二次请求读入当前旧摘要并合并后续
// 可压缩历史;成功后新摘要替换旧摘要,链上只有一份生效摘要;两处标记
// 各有数字与范围。
// ---------------------------------------------------------------------------
TEST_CASE("连续两次 compact:旧摘要入料,新摘要替换,不叠摘要") {
    Harness harness("twice");
    std::string summary1_id;
    std::string turn3_user;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", BigText(600));
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 第一份摘要\n- 第一轮已并入", {});
            return reply;
        };
        const V3CompactRunResult first =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(first.applied);
        summary1_id = writer->context().chain[1].message_ref;

        // 新增一轮,再压第二次:请求材料必须包含 Q1(§4.41)。
        std::tie(turn3_user, std::ignore) =
            harness.SeedTurn(*writer, "turn-000002", BigText(600, 'y'));
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 第二份摘要\n- 合并了第一份摘要与新历史", {});
            return reply;
        };
        const V3CompactRunResult second =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(second.applied);

        // 链上:system + 新摘要(只此一份);旧摘要退出链。
        REQUIRE(writer->context().chain.size() == 2);
        CHECK(writer->context().chain[1].message_ref != summary1_id);
    }
    auto lines = ReadJsonLines(harness.jsonl);
    // 第二次的 prepared:inputMessageRefs 包含旧摘要 id 与新增历史
    //(材料清单里,Q1 无永久保留特权)。
    const auto prepared_all = EventsOf(lines, "model.request.prepared");
    REQUIRE(prepared_all.size() == 2);
    bool second_reads_q1 = false;
    bool second_reads_new_turn = false;
    for (const auto ref : prepared_all[1]->at("payload").at("inputMessageRefs")) {
        if (ref.is_string() && ref.get<std::string>() == summary1_id) {
            second_reads_q1 = true;
        }
        if (ref.is_string() && ref.get<std::string>() == turn3_user) {
            second_reads_new_turn = true;
        }
    }
    CHECK(second_reads_q1);
    CHECK(second_reads_new_turn);
    // 第二次的 applied:removed 含旧摘要;两枚 applied 各有 token 数字。
    const auto applied_all = EventsOf(lines, "compact.applied");
    REQUIRE(applied_all.size() == 2);
    bool removed_q1 = false;
    for (const auto ref : applied_all[1]->at("payload").at("removedMessageRefs")) {
        if (ref.is_string() && ref.get<std::string>() == summary1_id) {
            removed_q1 = true;
        }
    }
    CHECK(removed_q1);
    CHECK(applied_all[0]->at("payload").contains("contextTokensBefore"));
    CHECK(applied_all[1]->at("payload").contains("contextTokensBefore"));
    // 读取侧:两处压缩标记,各有数字(§4.11 多次压缩各显示各次)。
    auto ledger = ReadV3Ledger(harness.jsonl);
    REQUIRE(ledger.has_value());
    const HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    CHECK(timeline.compact_items.size() == 2);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

// ---------------------------------------------------------------------------
// §5.1 验收行:只有旧摘要可压缩——不误报无材料,可评估重压;收益不足
// 则拒收停止,不反复空转。
// ---------------------------------------------------------------------------
TEST_CASE("只有旧摘要可压缩:重压资格与收益不足拒收") {
    SUBCASE("更短的新摘要 -> 重压成功,removed 只有旧摘要") {
        Harness harness("resummarize");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", BigText(600));
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply(
                "# 摘要\n- 冗长的一大段内容,压过一次之后还是留了不少细节,"
                "足以让第二次压缩有明确的收益空间", {});
            return reply;
        };
        REQUIRE(lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput())
                    .applied);
        const std::string q1 = writer->context().chain[1].message_ref;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 更短的摘要", {});
            return reply;
        };
        const V3CompactRunResult second =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(second.applied);  // 不是 no_eligible_history(§4.41)
        CHECK(second.model_calls == 1);
        auto lines = ReadJsonLines(harness.jsonl);
        const auto& applied = *EventsOf(lines, "compact.applied").back();
        REQUIRE(applied["payload"]["removedMessageRefs"].size() == 1);
        CHECK(applied["payload"]["removedMessageRefs"][0] == q1);
    }
    SUBCASE("压不小 -> 收益不足拒收,不空转") {
        Harness harness("nogain");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", BigText(600));
        StubClient client;
        const std::string long_reply = ManifestReply(
            "# 摘要\n- 这一版摘要写得非常长,几乎追平原始对话的体量,"
            "压了等于没压,还占地方,应当被收益校验拦下来", {});
        client.respond = [&long_reply] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = long_reply;
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(result.applied);
        // 第二次同一副牌重洗:候选没变小 -> benefit 不过 -> rejected。
        // 后缀要压过旧摘要落档时的 JSON 外壳开销,收益账才一定为负。
        client.respond = [&long_reply] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = long_reply + BigText(200, 'n');
            return reply;
        };
        const V3CompactRunResult second =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(!second.applied);
        CHECK(second.terminal_kind == "rejected");
        CHECK(second.reason == "validation_failed");
        // 校验详情里能看到 benefit 没过与数字(§4.8 失败项带证据)。
        auto lines = ReadJsonLines(harness.jsonl);
        const auto& validation = *EventsOf(lines, "compact.validation.completed").back();
        bool benefit_failed_with_detail = false;
        for (const auto& check : validation["payload"]["checks"]) {
            if (check["code"] == "benefit" && !check["passed"].get<bool>() &&
                check.contains("detail")) {
                benefit_failed_with_detail = true;
            }
        }
        CHECK(benefit_failed_with_detail);
        // 旧上下文仍有效:链没换。
        CHECK(writer->context().chain.size() == 2);
    }
}

// ---------------------------------------------------------------------------
// §4.64:撞窗整轮回退——本地预检不过先移出仅供参考的保留尾部,再从
// 可压缩历史最新一轮起整轮移出(8k 目标可退出多轮);不拆工具组;
// 回退事件落档;退空仍不过则 rejected,一次模型都不调。
// ---------------------------------------------------------------------------
TEST_CASE("撞窗整轮回退:8k 目标退出多轮,整轮不拆,事件落档") {
    Harness harness("retreat");
    std::string turn1_user, turn2_user, turn3_user;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        // 三轮各 ~8 KiB 正文(粗估 ~2k token):8192 的目标要退出两轮才够。
        const std::string chunk(8192, 'x');
        std::tie(turn1_user, std::ignore) =
            harness.SeedTurn(*writer, "turn-000001", chunk, chunk);
        std::tie(turn2_user, std::ignore) =
            harness.SeedTurn(*writer, "turn-000002", chunk, chunk);
        std::tie(turn3_user, std::ignore) =
            harness.SeedTurn(*writer, "turn-000003", chunk, chunk);

        V3CompactProfile profile = BaseProfile();
        profile.compact_window_tokens = 6000;  // 全量输入装不下;退两轮后装得下
        profile.compact_output_reserve_tokens = 64;
        profile.compact_margin_tokens = 64;

        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 摘要\n- 前两轮已并入", {});
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, profile, ManualInput());
        REQUIRE(result.applied);
        CHECK(result.retreat_steps == 1);  // 一步修订退出多轮(单轮不足 8k)
        CHECK(result.gate_checked);
    }
    auto lines = ReadJsonLines(harness.jsonl);
    const auto retreated = EventsOf(lines, "compact.range.retreated");
    REQUIRE(retreated.size() == 1);
    const auto& event = *retreated.front();
    CHECK(event["payload"]["planRevision"] == 1);
    CHECK(event["payload"]["lastFailedRequestId"].is_null());  // 本地预检
    CHECK(event["payload"]["estimatedInputTokensAfter"] <
          event["payload"]["estimatedInputTokensBefore"]);
    CHECK(event["payload"]["retreatTargetTokens"] == 8192);
    // 整轮退出:turn-3 与 turn-2 各自的全部消息都在退出清单里,不拆轮。
    REQUIRE(event["payload"]["retreatedTurnIds"].size() == 2);
    CHECK(event["payload"]["retreatedTurnIds"][0] == "turn-000003");
    CHECK(event["payload"]["retreatedTurnIds"][1] == "turn-000002");
    CHECK(event["payload"]["retreatedMessageRefs"].size() == 4);
    // started 与 applied 的口径一致:removed 只剩第一轮,退出者进 retained。
    const auto& applied = *EventsOf(lines, "compact.applied").front();
    CHECK(applied["payload"]["removedMessageRefs"].size() == 2);
    CHECK(applied["payload"]["removedMessageRefs"][0] == turn1_user);
    CHECK(applied["payload"]["retainedMessageRefs"].size() == 4);
    CHECK(applied["payload"]["retainedMessageRefs"][0] == turn2_user);
    CHECK(applied["payload"]["retainedMessageRefs"][2] == turn3_user);
    // 回退先行:retreated 在 started 之前(seq 序)。
    const auto& started = *EventsOf(lines, "compact.started").front();
    CHECK(retreated.front()->at("seq").get<std::uint64_t>() < started["seq"].get<std::uint64_t>());
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("撞窗回退:保留尾部先移出参考,工具组不拆") {
    Harness harness("retreat-tool");
    std::string tool_user, tool_result, open_user;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", BigText(600));
        std::tie(tool_user, tool_result) =
            harness.SeedToolTurn(*writer, "turn-000002", "action-000001");
        std::tie(open_user, std::ignore) =
            harness.SeedTurn(*writer, "turn-000003", std::string(4096, 'y'));

        V3CompactProfile profile = BaseProfile();
        // turn-000003 是受保护的开放轮:先作参考尾部进输入,撞窗后整段
        // 移出参考(§4.64 第一步);移出后装得下——工具轮不参与回退,
        // 配对整轮留在压缩材料里。
        profile.compact_window_tokens = 1500;
        profile.compact_output_reserve_tokens = 64;
        profile.compact_margin_tokens = 64;
        profile.min_retreat_tokens = 64;

        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 摘要\n- 旧一轮与工具轮已并入", {});
            return reply;
        };
        V3CompactRunInput input = ManualInput();
        input.protected_turn_ids = {"turn-000003"};
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, profile, std::move(input));
        REQUIRE(result.applied);
        CHECK(result.retreat_steps == 1);
    }
    auto lines = ReadJsonLines(harness.jsonl);
    // 回退事件:参考移出(无 turn 退出、参考清单清空),估算确实变小。
    const auto retreated = EventsOf(lines, "compact.range.retreated");
    REQUIRE(retreated.size() == 1);
    CHECK(retreated.front()->at("payload").at("retreatedTurnIds").size() == 0);
    CHECK(retreated.front()->at("payload").at("retreatedMessageRefs").size() == 0);
    CHECK(retreated.front()->at("payload").at("referenceMessageRefs").size() == 0);
    CHECK(retreated.front()->at("payload").at("estimatedInputTokensAfter") <
          retreated.front()->at("payload").at("estimatedInputTokensBefore"));
    const auto& applied = *EventsOf(lines, "compact.applied").front();
    // removed 覆盖旧一轮 + 整个工具轮(声明与结果同侧,不拆组)。
    const auto removed = applied["payload"]["removedMessageRefs"];
    CHECK(removed.size() == 5);
    bool removed_tool_user = false;
    bool removed_tool_result = false;
    for (const auto& ref : removed) {
        if (ref == tool_user) removed_tool_user = true;
        if (ref == tool_result) removed_tool_result = true;
    }
    CHECK(removed_tool_user);
    CHECK(removed_tool_result);
    // 开放轮原样保留(retained = 参考移出后仍在链尾),消息 ID 不变。
    CHECK(applied["payload"]["retainedMessageRefs"].size() == 2);
    CHECK(applied["payload"]["retainedMessageRefs"][0] == open_user);
    CHECK(applied["payload"]["protectedTurnIds"].size() == 1);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("退空仍装不下:rejected 收场,一次模型都不调") {
    Harness harness("exhausted");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    harness.SeedTurn(*writer, "turn-000001", std::string(8192, 'z'), std::string(8192, 'z'));

    V3CompactProfile profile = BaseProfile();
    profile.compact_window_tokens = 64;  // 怎么退都装不下
    profile.compact_output_reserve_tokens = 0;
    profile.compact_margin_tokens = 0;

    StubClient client;
    const V3CompactRunResult result =
        lubancode::runtime::RunV3Compact(*writer, client, profile, ManualInput());
    REQUIRE(!result.applied);
    CHECK(result.terminal_kind == "rejected");
    CHECK(((result.reason == "input_capacity_exceeded") ||
           (result.reason == "retreat_budget_exhausted")));
    CHECK(result.model_calls == 0);  // 不发请求碰运气(§4.64)
    auto lines = ReadJsonLines(harness.jsonl);
    CHECK(EventsOf(lines, "model.request.prepared").empty());
    // 终态落档:requested + retreated(s) + rejected,链没换。
    CHECK(EventsOf(lines, "compact.rejected").size() == 1);
    CHECK(writer->context().chain.size() == 3);  // system + user + assistant
    CHECK(VerifyV3File(harness.jsonl).ok);
}

// ---------------------------------------------------------------------------
// §4.9/§5.1:无可压缩历史(空链/全是受保护 turn)明确拒绝,不调模型。
// ---------------------------------------------------------------------------
TEST_CASE("空历史与全受保护:no_eligible_history,不空调模型") {
    SUBCASE("只有 system") {
        Harness harness("empty");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        StubClient client;
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        CHECK(!result.applied);
        CHECK(result.terminal_kind == "rejected");
        CHECK(result.reason == "no_eligible_history");
        CHECK(result.model_calls == 0);
    }
    SUBCASE("唯一一轮是受保护 turn") {
        Harness harness("all-protected");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", "唯一且开放的一轮。");
        V3CompactRunInput input = ManualInput();
        input.protected_turn_ids = {"turn-000001"};
        StubClient client;
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), std::move(input));
        CHECK(result.terminal_kind == "rejected");
        CHECK(result.reason == "no_eligible_history");
        CHECK(result.model_calls == 0);
    }
}

// ---------------------------------------------------------------------------
// 摘要缺必需字段(§5.1):校验失败有详情;不 applied、不改链;截断候选
// 标 incomplete 也不 applied(§4.39)。
// ---------------------------------------------------------------------------
TEST_CASE("校验失败:缺 manifest 有详情;截断候选不采用") {
    SUBCASE("缺 manifest 围栏 -> content_structure 不过") {
        Harness harness("badfence");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", "一轮。");
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = "一段没有围栏的摘要正文,长度倒是足够,可机器合同缺席,该拒。";
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        CHECK(!result.applied);
        CHECK(result.terminal_kind == "rejected");
        CHECK(result.reason == "validation_failed");
        auto lines = ReadJsonLines(harness.jsonl);
        const auto& validation = *EventsOf(lines, "compact.validation.completed").front();
        CHECK(validation["payload"]["passed"] == false);
        bool structure_detail = false;
        for (const auto& check : validation["payload"]["checks"]) {
            if (check["code"] == "content_structure" && !check["passed"].get<bool>() &&
                check.contains("detail")) {
                structure_detail = true;
            }
        }
        CHECK(structure_detail);
        // 候选在档可查;没有 applied;链没换(§5.1"不 applied、不改内存")。
        CHECK(EventsOf(lines, "compact.applied").empty());
        CHECK(writer->context().chain.size() == 3);
    }
    SUBCASE("输出截断 -> 候选标 truncated,不采用") {
        Harness harness("truncated");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", "一轮。");
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.truncated = true;  // finish_reason=length
            reply.text = ManifestReply("# 半份摘要", {});
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        CHECK(!result.applied);
        auto lines = ReadJsonLines(harness.jsonl);
        // 候选留档且标 incomplete(§4.39:保存原始部分回复,标 incomplete)。
        bool found_truncated_candidate = false;
        for (const auto& line : lines) {
            if (line.value("type", "") == "message" && line["message"].value("role", "") == "assistant" &&
                line.value("purpose", "") == "compact") {
                REQUIRE(line.contains("completionStatus"));
                CHECK(line["completionStatus"] == "truncated");
                found_truncated_candidate = true;
            }
        }
        CHECK(found_truncated_candidate);
        const auto& validation = *EventsOf(lines, "compact.validation.completed").front();
        bool output_limit_failed = false;
        for (const auto& check : validation["payload"]["checks"]) {
            if (check["code"] == "output_limit" && !check["passed"].get<bool>()) {
                output_limit_failed = true;
            }
        }
        CHECK(output_limit_failed);
        CHECK(EventsOf(lines, "compact.applied").empty());
    }
    SUBCASE("空正文 -> 不造空 assistant") {
        Harness harness("emptybody");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", "一轮。");
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;  // HTTP 200 后无响应正文一类
            reply.text = " ";
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        CHECK(result.terminal_kind == "failed");
        CHECK(result.reason == "empty_compact_response");
        auto lines = ReadJsonLines(harness.jsonl);
        int compact_assistants = 0;
        for (const auto& line : lines) {
            if (line.value("type", "") == "message" && line["message"].value("role", "") == "assistant" &&
                line.value("purpose", "") == "compact") {
                ++compact_assistants;
            }
        }
        CHECK(compact_assistants == 0);  // 只记错误终态,不造空 assistant
    }
}

// ---------------------------------------------------------------------------
// busy(§4.6 一次只运行一个 compact)与模型请求失败(§4.5 failed 态)。
// ---------------------------------------------------------------------------
TEST_CASE("busy 拒收与模型请求失败分型") {
    SUBCASE("已有进行中的 compact -> 不再开场") {
        Harness harness("busy");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", "一轮。");
        auto open = CompactSession::Begin(*writer, "auto", "threshold", std::nullopt,
                                          nlohmann::json::object({}));
        REQUIRE(open.info.began);
        StubClient client;
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        CHECK(!result.began);
        CHECK(result.terminal_kind == "busy");
        CHECK(result.model_calls == 0);
        open.session->Fail(*writer, CompactSession::FailKind::Cancelled, "test_cleanup");
    }
    SUBCASE("模型请求失败 -> compact.failed,候选缺席") {
        Harness harness("providerfail");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", "一轮。");
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = false;
            reply.error_code = "provider_error";
            reply.error_detail = "HTTP 200 后 SSE 报错";
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        CHECK(!result.applied);
        CHECK(result.terminal_kind == "failed");
        CHECK(result.reason == "provider_error");
        auto lines = ReadJsonLines(harness.jsonl);
        CHECK(EventsOf(lines, "compact.failed").size() == 1);
        // HTTP 200 后 SSE 报错:只记错误终态,不造空 assistant。
        int compact_assistants = 0;
        for (const auto& line : lines) {
            if (line.value("type", "") == "message" && line["message"].value("role", "") == "assistant" &&
                line.value("purpose", "") == "compact") {
                ++compact_assistants;
            }
        }
        CHECK(compact_assistants == 0);
        CHECK(writer->context().chain.size() == 3);
        CHECK(VerifyV3File(harness.jsonl).ok);
    }
}

// ---------------------------------------------------------------------------
// §5.1 崩溃行(事件账与恢复逻辑级,真注入跑法留验收单):applied 写盘
// 失败 -> 旧内存视图不被替换;resume 后候选可查、未 applied 不生效、
// 场上仍有未收口 compact(再来一次 busy)。
// ---------------------------------------------------------------------------
TEST_CASE("applied 写盘失败:摘要在档不生效,恢复后旧上下文仍有效") {
    Harness harness("iofail");
    V3WriterOptions fail_options;
    int commits = 0;
    // 1 system,2 session.started,3 user,4 assistant,5 admit,6 requested,
    // 7 started,8 专用 system,9 prompt,10 prepared,11 response.started,
    // 12 response.completed,13 candidate,14 validation.started,
    // 15 validation.completed,16 摘要,17 applied(注入失败)。
    //(候选走流式三件套,D1 延伸:11-13 三笔。)
    fail_options.inject_io_failure = [&commits]() -> std::optional<std::string> {
        ++commits;
        return commits >= 17 ? std::optional<std::string>("io.injected") : std::nullopt;
    };
    {
        auto writer = harness.Start(fail_options);
        REQUIRE(writer.has_value());
        harness.SeedTurn(*writer, "turn-000001", BigText(600));
        StubClient client;
        client.respond = [] {
            V3CompactModelReply reply;
            reply.ok = true;
            reply.text = ManifestReply("# 摘要\n- 已通过校验", {});
            return reply;
        };
        const V3CompactRunResult result =
            lubancode::runtime::RunV3Compact(*writer, client, BaseProfile(), ManualInput());
        REQUIRE(!result.applied);
        CHECK(result.reason.rfind("v3writer.injected", 0) == 0);
        // 旧内存视图未被新摘要替换(§4.8:applied 写盘失败不得发布新视图)。
        CHECK(writer->context().chain.size() == 3);
        CHECK(writer->broken());
    }
    // resume:候选与摘要在档可查;没有 applied,旧上下文仍有效。
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    CHECK(continued->context().chain.size() == 3);
    REQUIRE(continued->context().open_compact_ids.size() == 1);
    auto ledger = ReadV3Ledger(harness.jsonl);
    REQUIRE(ledger.has_value());
    const ModelContext context = ProjectModelContext(*ledger);
    CHECK(context.open_compact_ids.size() == 1);
    CHECK(context.inputs.size() == 2);  // 旧上下文:user + assistant
    // 候选可查(时间线里 purpose=compact 的 assistant 在档)。
    const HistoryTimeline timeline = ProjectHistoryTimeline(*ledger);
    bool candidate_visible = false;
    for (const auto& item : timeline.items) {
        if (item.kind == HistoryTimeline::Item::Kind::Message &&
            item.message.purpose == MessagePurpose::Compact &&
            item.message.role == MessageRole::Assistant) {
            candidate_visible = true;
        }
    }
    CHECK(candidate_visible);
    // 场上仍有未收口 compact:再来一次 busy(§4.6 恢复边界)。
    StubClient client;
    const V3CompactRunResult retry =
        lubancode::runtime::RunV3Compact(*continued, client, BaseProfile(), ManualInput());
    CHECK(retry.terminal_kind == "busy");
}
