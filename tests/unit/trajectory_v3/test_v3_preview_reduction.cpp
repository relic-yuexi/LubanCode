// v3 工具预览降档测试(P1 其余,§4.38):32/16/8/4 KiB 阶梯、派生 tool
// 消息(origin=context_runtime、sourceToolMessageRef 指原消息)、原消息
// 不改、context.tool_previews.reduced 独立提交换链、Continue 重放恢复档位。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-reduce-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
    }

    std::optional<V3Writer> Start() {
        auto writer = V3Writer::Start(jsonl, "20260910-150000-REDU01", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
    }
};

std::vector<nlohmann::json> ReadJson(const std::filesystem::path& path) {
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

// 造一轮链:S -> U1 -> A1(call=X) -> R1(32 KiB 预览版)。
std::string InstallToolRound(V3Writer& writer, const std::string& preview_text) {
    MessageDraft user;
    user.turn_id = "turn-000001";
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", "查日志"}});
    WriteReceipt user_receipt = writer.AppendMessage(std::move(user));
    REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status == WriteReceipt::Status::Committed);

    ToolActionSession action = ToolActionSession::Admit(writer, "turn-000001", "step-000001",
                                                        "action-000001", "queued", std::nullopt,
                                                        std::nullopt);
    REQUIRE(action.Start(writer, "args-ref",
                         ToolIdentity{"grep", "builtin", "1.0", "cwd=/repo"})
                .status == WriteReceipt::Status::Committed);
    REQUIRE(action.Finish(writer, 0, 90).status == WriteReceipt::Status::Committed);
    nlohmann::json result_ref = nlohmann::json::array(
        {MakeArtifactRef("res-000001", "result_metadata", "artifacts/res-000001.json",
                         std::string(64, '3'), 800, "application/json")});
    WriteReceipt persisted = action.PersistedResult(
        writer, result_ref.get<std::vector<nlohmann::json>>(), action.last_event_id());
    REQUIRE(persisted.status == WriteReceipt::Status::Committed);
    WriteReceipt selected = action.SelectResult(writer, {persisted.id}, {}, "done");
    REQUIRE(selected.status == WriteReceipt::Status::Committed);
    REQUIRE(action.AppendToolMessage(writer, preview_text, action.selected_event_id())
                .status == WriteReceipt::Status::Committed);
    return writer.context().chain.back().message_ref;  // R1 的 messageId
}

}  // namespace

TEST_CASE("降档换链:R1_32 → R1_16,新消息 origin=context_runtime 指回原消息") {
    Harness harness("chain");
    std::string replacement_id;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        std::string original = InstallToolRound(*writer, "R1_32 全量预览正文");
        REQUIRE(writer->context().chain.size() == 3);  // system -> user -> tool

        auto result = writer->ReduceToolPreviews(
            16384, "input-hash-1", 90000, 48000,
            {V3Writer::PreviewReplacement{original, "R1_16 短预览正文", std::nullopt}},
            {"pairing-check-1"});
        REQUIRE(result.ok);
        REQUIRE(result.replacement_messages.size() == 1);
        REQUIRE(result.reduced_event.status == WriteReceipt::Status::Committed);
        replacement_id = result.replacement_messages[0].id;

        // 新链一次提交:同一位置换派生消息,前后接点不变。
        REQUIRE(writer->context().chain.size() == 3);
        CHECK(writer->context().chain[2].message_ref == result.replacement_messages[0].id);
        CHECK(writer->context().chain[2].prev_message_ref.value_or("") ==
              writer->context().chain[1].message_ref);
        CHECK(writer->context().revision == 4);  // 1(开卷)+user+tool+降档
        CHECK(writer->context().preview_budget_bytes == 16384);

        // 原消息不改写:旧行原样在档(§4.38)。
        auto lines = ReadJson(harness.jsonl);
        const nlohmann::json* original_line = nullptr;
        const nlohmann::json* derived_line = nullptr;
        const nlohmann::json* reduced_event = nullptr;
        for (const auto& line : lines) {
            if (line.value("messageId", "") == original) {
                original_line = &line;
            }
            if (line.value("messageId", "") == result.replacement_messages[0].id) {
                derived_line = &line;
            }
            if (line.value("kind", "") == "context.tool_previews.reduced") {
                reduced_event = &line;
            }
        }
        REQUIRE(original_line != nullptr);
        REQUIRE(derived_line != nullptr);
        REQUIRE(reduced_event != nullptr);
        CHECK((*original_line)["message"]["content"].get<std::string>().find("R1_32") !=
              std::string::npos);  // 原文未动
        // 派生消息:同 turn/step/action/tool_call_id 与选用引用(§4.38)。
        CHECK((*derived_line)["origin"] == "context_runtime");
        CHECK((*derived_line)["sourceToolMessageRef"] == original);
        CHECK((*derived_line)["turnId"] == (*original_line)["turnId"]);
        CHECK((*derived_line)["stepId"] == (*original_line)["stepId"]);
        CHECK((*derived_line)["actionId"] == "action-000001");
        CHECK((*derived_line)["message"]["tool_call_id"] == "action-000001");
        CHECK((*derived_line)["resultSelectionRef"] == (*original_line)["resultSelectionRef"]);
        CHECK((*derived_line)["message"]["content"].get<std::string>().find("R1_16") !=
              std::string::npos);
        // 降档提交事件:档位、替换、完整链、估算、配对校验引用齐全。
        CHECK((*reduced_event)["payload"]["oldPreviewBudget"] == 32768);
        CHECK((*reduced_event)["payload"]["newPreviewBudget"] == 16384);
        CHECK((*reduced_event)["payload"]["beforeRevision"] == 3);
        CHECK((*reduced_event)["payload"]["afterRevision"] == 4);
        CHECK((*reduced_event)["payload"]["replacementRefs"].size() == 1);
        CHECK((*reduced_event)["payload"]["contextChain"].size() == 3);
        CHECK((*reduced_event)["payload"]["inputHash"] == "input-hash-1");
        CHECK((*reduced_event)["payload"]["estimatedTokensBefore"] == 90000);
        CHECK((*reduced_event)["payload"]["estimatedTokensAfter"] == 48000);
        CHECK((*reduced_event)["payload"]["pairingCheckRefs"].size() == 1);
        CHECK(!reduced_event->contains("status"));
    }
    // 崩溃恢复:Continue 重放降档提交,档位与链取新值(§4.38 resume)。
    auto resumed = V3Writer::Continue(harness.jsonl, V3WriterOptions{}, &harness.clock);
    REQUIRE(resumed.has_value());
    CHECK(resumed->context().revision == 4);
    CHECK(resumed->context().preview_budget_bytes == 16384);
    CHECK(resumed->context().chain.size() == 3);
    CHECK(resumed->context().chain[2].message_ref == replacement_id);
}

TEST_CASE("档位只降不升:新档位不低于当前档则拒收") {
    Harness harness("no-up");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    std::string original = InstallToolRound(*writer, "R1_32 正文");
    auto same = writer->ReduceToolPreviews(
        32768, "hash", 1, 1, {V3Writer::PreviewReplacement{original, "短", std::nullopt}},
        {});
    CHECK(!same.ok);
    auto up = writer->ReduceToolPreviews(
        65536, "hash", 1, 1, {V3Writer::PreviewReplacement{original, "短", std::nullopt}},
        {});
    CHECK(!up.ok);
    // 32 -> 16 -> 8 阶梯:任一档够用就停(§4.38),这里验证可连续降。
    auto first = writer->ReduceToolPreviews(
        16384, "hash", 1, 1, {V3Writer::PreviewReplacement{original, "16K 版", std::nullopt}},
        {});
    REQUIRE(first.ok);
    std::string second_original = writer->context().chain[2].message_ref;
    auto second = writer->ReduceToolPreviews(
        8192, "hash", 1, 1,
        {V3Writer::PreviewReplacement{second_original, "8K 版", std::nullopt}}, {});
    REQUIRE(second.ok);
    CHECK(writer->context().preview_budget_bytes == 8192);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("原消息不在链上或不是 tool 消息:拒收,不造错链") {
    Harness harness("bad-original");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    std::string original = InstallToolRound(*writer, "R1_32 正文");
    // 不在账上。
    auto ghost = writer->ReduceToolPreviews(
        16384, "hash", 1, 1,
        {V3Writer::PreviewReplacement{"msg-999999", "短", std::nullopt}}, {});
    CHECK(!ghost.ok);
    // 在账上但不在当前链(system 消息)。
    std::string system_ref = writer->context().chain[0].message_ref;
    auto not_tool = writer->ReduceToolPreviews(
        16384, "hash", 1, 1, {V3Writer::PreviewReplacement{system_ref, "短", std::nullopt}},
        {});
    CHECK(!not_tool.ok);
    CHECK(not_tool.error.find("original_not_tool") != std::string::npos);
    // 链未被动过:降档全被拒,原链原样。
    CHECK(writer->context().chain[2].message_ref == original);
    CHECK(writer->context().revision == 3);
    CHECK(writer->context().preview_budget_bytes == 32768);
}

TEST_CASE("多枚降档一次提交:整批替换,链一次换稳") {
    Harness harness("batch");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    // 两枚工具消息在链上。
    std::string first = InstallToolRound(*writer, "R1_32 正文");
    // 第二枚:直接再造一轮(新 action/新消息)。
    ToolActionSession second_action = ToolActionSession::Admit(
        *writer, "turn-000002", "step-000002", "action-000002", "queued", std::nullopt,
        std::nullopt);
    REQUIRE(second_action.Start(*writer, "args-2", ToolIdentity{"ls", "builtin", "1.0", ""})
                .status == WriteReceipt::Status::Committed);
    REQUIRE(second_action.Finish(*writer, 0, 5).status == WriteReceipt::Status::Committed);
    REQUIRE(second_action.AppendToolMessage(*writer, "R2_32 正文", std::nullopt).status ==
            WriteReceipt::Status::Committed);
    std::string second = writer->context().chain.back().message_ref;
    REQUIRE(writer->context().chain.size() == 4);

    auto result = writer->ReduceToolPreviews(
        4096, "hash-2", 80000, 20000,
        {V3Writer::PreviewReplacement{first, "R1_4 正文", std::nullopt},
         V3Writer::PreviewReplacement{second, "R2_4 正文", std::nullopt}},
        {"pair-a", "pair-b"});
    REQUIRE(result.ok);
    REQUIRE(result.replacement_messages.size() == 2);
    CHECK(writer->context().chain.size() == 4);  // 一一换位,不增删节点
    CHECK(writer->context().chain[2].message_ref == result.replacement_messages[0].id);
    CHECK(writer->context().chain[3].message_ref == result.replacement_messages[1].id);
    CHECK(writer->context().chain[3].prev_message_ref.value_or("") ==
          result.replacement_messages[0].id);  // 后续节点同步改接
    CHECK(writer->context().preview_budget_bytes == 4096);
    CHECK(VerifyV3File(harness.jsonl).ok);
    // 每次请求只选一个版本:链上只挂新版本,旧版本仍在档可查。
    auto resumed = V3Writer::Continue(harness.jsonl, V3WriterOptions{}, &harness.clock);
    REQUIRE(resumed.has_value());
    CHECK(resumed->context().chain[2].message_ref == result.replacement_messages[0].id);
}
