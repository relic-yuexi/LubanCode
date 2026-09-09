// v3 compact 全链测试(§4.5-4.8):八行链闭合、compactId 贯穿、内部回合
// parentTurnId 挂接、applied 唯一终态与原子提交、失败三态分型、
// no_eligible_history 不空调模型、源版本冲突拒收、applied 写盘失败内存
// 不换账、恢复按 applied 重建新上下文、并发第二场拒收。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("lubancode-v3-compact-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
    }

    std::optional<V3Writer> Start() {
        auto writer = V3Writer::Start(jsonl, "20260910-120000-AAAAAA", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
    }

    // 造一轮已接纳的对话:user + assistant,返回消息 id。
    std::pair<std::string, std::string> SeedTurn(V3Writer& writer, const char* text) {
        MessageDraft user;
        user.turn_id = "turn-000001";
        user.purpose = MessagePurpose::Conversation;
        user.origin = MessageOrigin::Human;
        user.message = nlohmann::json::object({{"role", "user"}, {"content", text}});
        WriteReceipt user_receipt = writer.AppendMessage(std::move(user), Durability::PowerLoss);
        REQUIRE(writer->AdmitMessages({user_receipt.id}).status ==
                WriteReceipt::Status::Committed);
        return {user_receipt.id, user_receipt.id};
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

// 走完整链直到 Apply 就绪(prompt -> candidate -> validation)。
void RunToValidation(V3Writer& writer, CompactSession& session, const std::string& user_id) {
    auto freeze = session.Freeze(writer, {user_id}, {}, {});
    REQUIRE(freeze.eligible);
    REQUIRE(freeze.event.status == WriteReceipt::Status::Committed);
    REQUIRE(session.AppendPrompt(
                 writer, nlohmann::json::object({{"role", "user"}, {"content", "请压缩以上对话。"}}))
                 .status == WriteReceipt::Status::Committed);
    REQUIRE(writer
                .PrepareRequest("request-000009", session.turn_id(), "step-000009", "compact",
                                writer.context().system_message_ref, {user_id},
                                nlohmann::json::object({{"provider", "moonshot"}}),
                                session.compact_id())
                .status == WriteReceipt::Status::Committed);
    REQUIRE(session
                .WriteCandidate(writer,
                                nlohmann::json::object(
                                    {{"role", "assistant"}, {"content", "# 摘要\n- 用户问天气"}}),
                                "request-000009", "step-000009", "moonshot",
                                "openai-chat-completions", "kimi-k2.6",
                                nlohmann::json::object({{"inputTokens", 500}, {"outputTokens", 40}}))
                .status == WriteReceipt::Status::Committed);
    REQUIRE(session.StartValidation(writer).status == WriteReceipt::Status::Committed);
    REQUIRE(session
                .CompleteValidation(writer, true,
                                    {nlohmann::json::object({{"code", "content_structure"},
                                                             {"passed", true}}),
                                     nlohmann::json::object({{"code", "coverage"},
                                                             {"passed", true}})})
                .status == WriteReceipt::Status::Committed);
}

}  // namespace

TEST_CASE("八行全链一次成功:applied 唯一终态,链重接,恢复重建新上下文") {
    Harness harness("full");
    std::string user_id;
    std::string system_ref;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        system_ref = writer->context().system_message_ref;
        auto seeded = harness.SeedTurn(*writer, "第一轮对话,内容略。");
        user_id = seeded.first;
        const std::uint64_t revision_before = writer->context().revision;

        auto begin = CompactSession::Begin(*writer, "manual", "user_command", std::nullopt,
                                           nlohmann::json::object({{"requiredFields", {"summary"}}}));
        REQUIRE(begin.info.began);
        // 空闲手动:parentTurnId 为 null,不假称上一轮仍在运行(§4.6)。
        CHECK(!begin.session->parent_turn_id().has_value());
        std::string compact_id = begin.info.compact_id;
        RunToValidation(*writer, *begin.session, user_id);

        auto apply = begin.session->Apply(*writer, "# 摘要\n- 用户问天气", 148200, 31600,
                                          nlohmann::json::object({{"estimator", "utf8_bytes_div4"},
                                                                  {"estimatorVersion", 1}}));
        REQUIRE(apply.ok);
        // 内存换账:新链 = system + 摘要(retained 空)。
        CHECK(writer->context().revision == revision_before + 1);
        REQUIRE(writer->context().chain.size() == 2);
        CHECK(writer->context().chain[0].message_ref == system_ref);
        CHECK(writer->context().chain[1].message_ref == apply.summary.id);
        CHECK(writer->context().chain[1].prev_message_ref == system_ref);
        CHECK(writer->context().open_compact_ids.empty());
    }
    // applied 后崩溃(内存发布前):resume 从 applied 重建新上下文。
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    CHECK(continued->context().chain.size() == 2);
    CHECK(continued->context().chain[0].message_ref == system_ref);

    // 档上链完整:requested -> started -> prompt(user) -> prepared -> 候选
    // (assistant) -> validation.started/completed -> 摘要(user) -> applied;
    // compactId 贯穿;无第二枚成功终态。
    auto lines = ReadJsonLines(harness.jsonl);
    int compact_lines = 0;
    int terminals = 0;
    std::string prompt_turn;
    for (const auto& line : lines) {
        const bool is_compact = line.value("compactId", "") != "" ||
                                line.value("kind", "").rfind("compact.", 0) == 0;
        if (line.value("kind", "").rfind("compact.", 0) == 0) {
            ++compact_lines;
            REQUIRE(line.contains("compactId"));
            if (line["kind"] == "compact.applied") {
                ++terminals;
                CHECK(line["payload"]["summaryMessageRef"].is_string());
                CHECK(line["payload"]["contextChain"].size() == 2);
                CHECK(line["payload"]["protectedTurnIds"].size() == 0);
                CHECK(line["payload"]["contextTokensBefore"] == 148200);
                CHECK(line["payload"]["contextTokensAfter"] == 31600);
            }
            if (line["kind"] == "compact.requested") {
                CHECK(line["payload"]["trigger"] == "manual");
                CHECK(line["turnId"].is_string());
                // 空闲手动:无 parentTurnId(或显式 null),不假称挂主 turn。
                CHECK(!line.contains("parentTurnId") || line["parentTurnId"].is_null());
            }
        }
        if (line.value("type", "") == "message" && line.value("purpose", "") == "compact" &&
            line["message"]["role"] == "user") {
            prompt_turn = line["turnId"];
        }
        if (is_compact && line.value("type", "") == "message" &&
            line.value("purpose", "") == "compact") {
            CHECK(line["turnId"] == prompt_turn);  // 内部回合一致
            CHECK(line["display"]["mode"] == "hidden");
        }
    }
    CHECK(compact_lines == 5);  // requested/started/validation.started/validation.completed/applied
    CHECK(terminals == 1);
    V3VerifyReport report = VerifyV3File(harness.jsonl);
    CHECK(report.ok);
}

TEST_CASE("turn 中途自动 compact:主 turn 不变,内部回合挂 parentTurnId") {
    Harness harness("auto");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto seeded = harness.SeedTurn(*writer, "长对话。");
    auto begin = CompactSession::Begin(*writer, "auto", "preflight_capacity",
                                       std::optional<std::string>("turn-000001"),
                                       nlohmann::json::object({}));
    REQUIRE(begin.info.began);
    CHECK(begin.session->parent_turn_id() == "turn-000001");
    CHECK(begin.info.turn_id != "turn-000001");  // 不冒充主 turn
    auto freeze = begin.session->Freeze(*writer, {seeded.first}, {}, {});
    REQUIRE(freeze.eligible);
    // 主 turn 的消息仍在链上,未被 compact 抢走或重编号。
    CHECK(writer->context().chain.back().message_ref == seeded.first);
}

TEST_CASE("失败三态分型:failed/cancelled/rejected 各自终态,不更新链") {
    SUBCASE("模型请求失败 -> compact.failed") {
        Harness harness("failed");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        auto seeded = harness.SeedTurn(*writer, "内容。");
        auto begin = CompactSession::Begin(*writer, "auto", "threshold", std::nullopt,
                                           nlohmann::json::object({}));
        REQUIRE(begin.info.began);
        begin.session->Freeze(*writer, {seeded.first}, {}, {});
        WriteReceipt fail = begin.session->Fail(*writer, CompactSession::FailKind::Failed,
                                                "provider_error");
        REQUIRE(fail.status == WriteReceipt::Status::Committed);
        CHECK(writer->context().open_compact_ids.empty());
        CHECK(writer->context().chain.size() == 2);  // 链没换
    }
    SUBCASE("用户取消 -> compact.cancelled") {
        Harness harness("cancelled");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        auto seeded = harness.SeedTurn(*writer, "内容。");
        auto begin = CompactSession::Begin(*writer, "manual", "user_command", std::nullopt,
                                           nlohmann::json::object({}));
        REQUIRE(begin.info.began);
        begin.session->Freeze(*writer, {seeded.first}, {}, {});
        REQUIRE(begin.session->Fail(*writer, CompactSession::FailKind::Cancelled, "user_esc")
                    .status == WriteReceipt::Status::Committed);
    }
    SUBCASE("校验不过 -> compact.rejected,不 applied 不改内存") {
        Harness harness("rejected");
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        auto seeded = harness.SeedTurn(*writer, "内容。");
        auto begin = CompactSession::Begin(*writer, "auto", "threshold", std::nullopt,
                                           nlohmann::json::object({}));
        REQUIRE(begin.info.began);
        begin.session->Freeze(*writer, {seeded.first}, {}, {});
        begin.session->AppendPrompt(
            *writer, nlohmann::json::object({{"role", "user"}, {"content", "压缩"}}));
        begin.session->WriteCandidate(
            *writer, nlohmann::json::object({{"role", "assistant"}, {"content", "半份"}}),
            "request-000009", "step-000009", "moonshot", "openai-chat-completions", "kimi-k2.6",
            nlohmann::json(nullptr));
        begin.session->StartValidation(*writer);
        begin.session->CompleteValidation(
            *writer, false, {nlohmann::json::object({{"code", "content_structure"},
                                                     {"passed", false},
                                                     {"detail", "缺必需字段 summary"}})});
        REQUIRE(begin.session->Fail(*writer, CompactSession::FailKind::Rejected,
                                    "validation_failed")
                    .status == WriteReceipt::Status::Committed);
        // 校验通过/摘要落盘后崩溃路径同构:没有 applied 就不生效。
        auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
        REQUIRE(continued.has_value());
        CHECK(continued->context().chain.size() == 2);  // 旧上下文仍有效
    }
}

TEST_CASE("无可压缩历史:no_eligible_history,不空调压缩模型") {
    Harness harness("empty");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto begin = CompactSession::Begin(*writer, "manual", "user_command", std::nullopt,
                                       nlohmann::json::object({}));
    REQUIRE(begin.info.began);
    auto freeze = begin.session->Freeze(*writer, {}, {}, {});  // removed 空
    REQUIRE(!freeze.eligible);
    REQUIRE(freeze.event.status == WriteReceipt::Status::Committed);
    // 档上:requested + rejected(no_eligible_history);没有 prompt/prepared/
    // assistant——不调用模型假压缩。
    auto lines = ReadJsonLines(harness.jsonl);
    int prompts = 0, prepared = 0;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "compact.rejected") {
            CHECK(line["payload"]["reason"] == "no_eligible_history");
        }
        if (line.value("purpose", "") == "compact") ++prompts;
        if (line.value("kind", "") == "model.request.prepared") ++prepared;
    }
    CHECK(prompts == 0);
    CHECK(prepared == 0);
}

TEST_CASE("源版本冲突:提交前源变了,记 rejected 不静默拼接") {
    Harness harness("conflict");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto seeded = harness.SeedTurn(*writer, "第一轮。");
    auto begin = CompactSession::Begin(*writer, "auto", "threshold", std::nullopt,
                                       nlohmann::json::object({}));
    REQUIRE(begin.info.began);
    RunToValidation(*writer, *begin.session, seeded.first);
    // 压缩期间插话被接纳:源上下文改变。
    MessageDraft steered;
    steered.turn_id = "turn-000001";
    steered.purpose = MessagePurpose::Conversation;
    steered.origin = MessageOrigin::Human;
    steered.message = nlohmann::json::object({{"role", "user"}, {"content", "插话"}});
    WriteReceipt steered_receipt =
        writer->AppendMessage(std::move(steered), Durability::PowerLoss);
    REQUIRE(writer->AdmitMessages({steered_receipt.id}).status ==
            WriteReceipt::Status::Committed);

    auto apply = begin.session->Apply(*writer, "摘要", 100, 50,
                                      nlohmann::json::object({}));
    REQUIRE(!apply.ok);
    CHECK(apply.error == "compact.source_conflict");
    // 被插话的新消息没丢:仍在链上。
    CHECK(writer->context().chain.back().message_ref == steered_receipt.id);
}

TEST_CASE("applied 写盘失败:旧内存视图不被替换,后续受阻") {
    // 注入点数准:1 system,2 started,3 user,4 admit,5 requested,6 freeze,
    // 7 prompt,8 prepared,9 candidate,10 validation.started,
    // 11 validation.completed,12 摘要落稳,13 applied 注入失败。
    Harness harness("iofail");
    V3WriterOptions fail_options;
    int count = 0;
    fail_options.inject_io_failure = [&count]() -> std::optional<std::string> {
        ++count;
        return count >= 13 ? std::optional<std::string>("io.injected") : std::nullopt;
    };
    auto writer = V3Writer::Start(harness.jsonl, "20260910-120000-AAAAAA", "run-000001",
                                  "你是 LubanCode。", nlohmann::json::object(), fail_options,
                                  &harness.clock);
    REQUIRE(writer.has_value());
    // 3 user + 4 admit。
    MessageDraft user;
    user.turn_id = "turn-000001";
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", "x"}});
    WriteReceipt user_receipt = writer->AppendMessage(std::move(user), Durability::PowerLoss);
    REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer->AdmitMessages({user_receipt.id}).status == WriteReceipt::Status::Committed);
    // 5 requested + 6 freeze + 7 prompt + 8 prepared + 9 candidate +
    // 10 validation.started + 11 validation.completed。
    auto begin = CompactSession::Begin(*writer, "manual", "user_command", std::nullopt,
                                       nlohmann::json::object({}));
    REQUIRE(begin.info.began);
    begin.session->Freeze(*writer, {user_receipt.id}, {}, {});
    begin.session->AppendPrompt(
        *writer, nlohmann::json::object({{"role", "user"}, {"content", "压缩"}}));
    writer->PrepareRequest("request-000009", begin.session->turn_id(), "step-000009",
                           "compact", writer->context().system_message_ref, {user_receipt.id},
                           nlohmann::json::object({{"provider", "moonshot"}}),
                           begin.session->compact_id());
    begin.session->WriteCandidate(
        *writer, nlohmann::json::object({{"role", "assistant"}, {"content", "摘要"}}),
        "request-000009", "step-000009", "moonshot", "openai-chat-completions", "kimi-k2.6",
        nlohmann::json(nullptr));
    begin.session->StartValidation(*writer);
    begin.session->CompleteValidation(
        *writer, true, {nlohmann::json::object({{"code", "content_structure"}, {"passed", true}})});
    // 12 摘要落稳,13 applied 注入失败。
    auto apply = begin.session->Apply(*writer, "摘要正文", 100, 50,
                                      nlohmann::json::object({}));
    REQUIRE(!apply.ok);
    CHECK(apply.error_code == "v3writer.injected");
    // 旧内存视图未被新摘要替换:链仍是 system+user;句柄 broken,后续受阻。
    CHECK(writer->context().chain.size() == 2);
    CHECK(writer->broken());
    EventDraft blocked;
    blocked.kind = EventKindV3::SessionEnded;
    blocked.payload = nlohmann::json::object({{"reason", "x"}});
    WriteReceipt receipt = writer->AppendEvent(std::move(blocked), Durability::PowerLoss);
    CHECK(receipt.status == WriteReceipt::Status::IoFailed);
    CHECK(receipt.error_code == "v3writer.broken");
    // 恢复:摘要在档但无 applied,旧上下文仍有效。
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    CHECK(continued->context().chain.size() == 2);
}

TEST_CASE("一场未收口,第二场拒收") {
    Harness harness("busy");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto seeded = harness.SeedTurn(*writer, "内容。");
    auto begin = CompactSession::Begin(*writer, "auto", "threshold", std::nullopt,
                                       nlohmann::json::object({}));
    REQUIRE(begin.info.began);
    auto second = CompactSession::Begin(*writer, "auto", "threshold", std::nullopt,
                                        nlohmann::json::object({}));
    CHECK(!second.info.began);
    CHECK(second.info.error.rfind("compact.busy", 0) == 0);
}
