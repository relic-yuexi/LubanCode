// v3 流式与 usage owner 测试(§4.43/§4.63/§4.44/§4.12):片段记事件、收齐
// 追加唯一 assistant 并接纳;Esc 把已收内容定稿为 interrupted assistant;
// usage 缺实报为 null 不补零、不倒填;assistant 自带 provider/model 与
// responseModel 分开;片段不重复成第二条 assistant。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

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
                                    ("lubancode-v3-stream-" + std::string(tag));
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

    std::string SeedUser(V3Writer& writer) {
        MessageDraft user;
        user.turn_id = "turn-000001";
        user.purpose = MessagePurpose::Conversation;
        user.origin = MessageOrigin::Human;
        user.message = nlohmann::json::object({{"role", "user"}, {"content", "讲讲思路"}});
        WriteReceipt receipt = writer.AppendMessage(std::move(user), Durability::PowerLoss);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
        REQUIRE(writer.AdmitMessages({receipt.id}).status == WriteReceipt::Status::Committed);
        return receipt.id;
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

}  // namespace

TEST_CASE("正常收齐:片段事件 + 唯一 assistant,来源自带,接纳进链") {
    Harness harness("complete");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    harness.SeedUser(*writer);

    const std::string reserved = writer->NewMessageId();
    REQUIRE(writer
                ->BeginStreamResponse("request-000001", "stream-000001", "turn-000001",
                                      "step-000001", reserved)
                .status == WriteReceipt::Status::Committed);
    REQUIRE(writer
                ->AppendStreamDelta("request-000001", "stream-000001", reserved, 1, "reasoning",
                                    nlohmann::json::object({{"text", "先想"}}))
                .status == WriteReceipt::Status::Committed);
    REQUIRE(writer
                ->AppendStreamDelta("request-000001", "stream-000001", reserved, 2, "text",
                                    nlohmann::json::object({{"text", "思路如下。"}}))
                .status == WriteReceipt::Status::Committed);

    const std::uint64_t revision_before = writer->context().revision;
    WriteReceipt done = writer->CompleteStreamResponse(
        "request-000001", "stream-000001", "turn-000001", "step-000001", reserved,
        nlohmann::json::object({{"role", "assistant"}, {"content", "思路如下。"}}), "moonshot",
        "openai-chat-completions", "kimi-k2.6", nlohmann::json("kimi-k2.6"),
        nlohmann::json::object({{"inputTokens", 210}, {"outputTokens", 16}}), "end_turn");
    REQUIRE(done.status == WriteReceipt::Status::Committed);
    CHECK(done.id == reserved);  // 预留 id 成行,不另造
    CHECK(writer->context().revision == revision_before + 1);  // 接纳进链

    auto lines = ReadJsonLines(harness.jsonl);
    int assistants = 0, deltas = 0;
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"]["role"] == "assistant") {
            ++assistants;
            CHECK(line["messageId"] == reserved);
            CHECK(line["provider"] == "moonshot");
            CHECK(line["wire"] == "openai-chat-completions");
            CHECK(line["model"] == "kimi-k2.6");
            CHECK(line["responseModel"] == "kimi-k2.6");  // 请求名与返回名分开存
            CHECK(line["usage"]["inputTokens"] == 210);
            CHECK(line["requestId"] == "request-000001");
        }
        if (line.value("kind", "") == "model.response.delta") ++deltas;
        if (line.value("kind", "") == "model.response.completed") {
            CHECK(line["payload"]["finishReason"] == "end_turn");
            CHECK(line["status"] == "done");
        }
    }
    CHECK(assistants == 1);  // 最终 message 只出现一次,片段不重复成行
    CHECK(deltas == 2);
    V3VerifyReport report = VerifyV3File(harness.jsonl);
    CHECK(report.ok);
}

TEST_CASE("SSE 按 Esc:已收内容定稿 interrupted assistant,usage 缺实报为 null") {
    Harness harness("esc");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    harness.SeedUser(*writer);

    const std::string reserved = writer->NewMessageId();
    REQUIRE(writer
                ->BeginStreamResponse("request-000001", "stream-000001", "turn-000001",
                                      "step-000001", reserved)
                .status == WriteReceipt::Status::Committed);
    writer->AppendStreamDelta("request-000001", "stream-000001", reserved, 1, "text",
                              nlohmann::json::object({{"text", "已收到一段。"}}));
    writer->AppendStreamDelta("request-000001", "stream-000001", reserved, 2, "tool_args",
                              nlohmann::json::object({{"name", "search"}, {"fragment", "{\"q\""}}));

    const std::uint64_t revision_before = writer->context().revision;
    WriteReceipt interrupted = writer->InterruptStreamResponse(
        "request-000001", "stream-000001", "turn-000001", "step-000001", reserved,
        // 已收到的内容;未收齐的工具调用不伪造完整(§1.28)。
        nlohmann::json::object({{"role", "assistant"},
                                {"content", "已收到一段。"},
                                {"interruptedToolCall", true}}),
        "moonshot", "openai-chat-completions", "kimi-k2.6",
        /*received_through=*/2,
        /*usage=*/std::nullopt);  // 没收到实报
    REQUIRE(interrupted.status == WriteReceipt::Status::Committed);
    CHECK(writer->context().revision == revision_before + 1);

    auto lines = ReadJsonLines(harness.jsonl);
    int assistants = 0;
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"]["role"] == "assistant") {
            ++assistants;
            CHECK(line["completionStatus"] == "interrupted");
            // 缺实报:usage 键必须在,值为 null——不补 0(§4.12/§4.11)。
            REQUIRE(line.contains("usage"));
            CHECK(line["usage"].is_null());
            CHECK(line["responseModel"].is_null());
        }
        if (line.value("kind", "") == "model.response.cancelled") {
            CHECK(line["payload"]["receivedThrough"] == 2);  // 取消水位留档
            CHECK(line["status"] == "cancelled");
        }
    }
    CHECK(assistants == 1);  // 正式 interrupted assistant,不是草稿
    V3VerifyReport report = VerifyV3File(harness.jsonl);
    CHECK(report.ok);
}

TEST_CASE("中断时已收部分 usage 照实内联,不倒改旧 message") {
    Harness harness("partial-usage");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    harness.SeedUser(*writer);
    const std::string reserved = writer->NewMessageId();
    writer->BeginStreamResponse("request-000001", "stream-000001", "turn-000001", "step-000001",
                                reserved);
    WriteReceipt interrupted = writer->InterruptStreamResponse(
        "request-000001", "stream-000001", "turn-000001", "step-000001", reserved,
        nlohmann::json::object({{"role", "assistant"}, {"content", "半截"}}), "moonshot",
        "openai-chat-completions", "kimi-k2.6", 1,
        nlohmann::json::object({{"inputTokens", 180}}));  // 已收部分 usage
    REQUIRE(interrupted.status == WriteReceipt::Status::Committed);

    // 迟到 usage:追加事件,不倒改旧 message(§4.43/§4.12)。
    EventDraft late;
    late.kind = EventKindV3::ModelUsageAppended;
    late.request_id = "request-000001";
    late.payload = nlohmann::json::object(
        {{"messageRef", reserved},
         {"usage", nlohmann::json::object({{"inputTokens", 180}, {"outputTokens", 5}})},
         {"cause", "late_arrival"}});
    REQUIRE(writer->AppendEvent(std::move(late), Durability::PowerLoss).status ==
            WriteReceipt::Status::Committed);

    auto lines = ReadJsonLines(harness.jsonl);
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"]["role"] == "assistant") {
            // 旧 message 的 usage 保持中断时已收的部分,未被迟到数据倒改。
            CHECK(line["usage"]["inputTokens"] == 180);
            CHECK(!line["usage"].contains("outputTokens"));
        }
    }
    V3VerifyReport report = VerifyV3File(harness.jsonl);
    CHECK(report.ok);
}

TEST_CASE("length 截断:completionStatus=truncated,不冒充正常完成") {
    Harness harness("truncated");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    harness.SeedUser(*writer);
    const std::string reserved = writer->NewMessageId();
    writer->BeginStreamResponse("request-000001", "stream-000001", "turn-000001", "step-000001",
                                reserved);
    WriteReceipt done = writer->CompleteStreamResponse(
        "request-000001", "stream-000001", "turn-000001", "step-000001", reserved,
        nlohmann::json::object({{"role", "assistant"}, {"content", "被输出上限截断"}}),
        "moonshot", "openai-chat-completions", "kimi-k2.6", nlohmann::json(nullptr),
        nlohmann::json::object({{"inputTokens", 90}, {"outputTokens", 32768}}), "length",
        MessagePurpose::Conversation, std::nullopt,
        CompletionStatus::Truncated);
    REQUIRE(done.status == WriteReceipt::Status::Committed);
    auto lines = ReadJsonLines(harness.jsonl);
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"]["role"] == "assistant") {
            CHECK(line["completionStatus"] == "truncated");
        }
    }
}
