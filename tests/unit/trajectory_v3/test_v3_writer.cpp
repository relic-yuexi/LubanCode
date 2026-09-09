// v3 writer 核心测试(P1 写入侧):开卷首行 system(seq=1、turnId=null、
// 自带身份)、两类行共链共 seq、哈希链确定性、退出不造空回合、prepared
// 引用先落稳、崩溃恢复(Continue 重放视图/尾行截断明报/发号续号)。
#include <doctest/doctest.h>

#include <cstdio>
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
    std::filesystem::path dir;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-writer-" + std::string(tag));
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
};

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace

TEST_CASE("开卷:首行 system,seq=1,turnId=null,不造空回合") {
    Harness harness("start");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());

    auto lines = ReadLines(harness.jsonl);
    REQUIRE(lines.size() == 2);  // system + session.started,不凭空造用户回合

    auto first = nlohmann::json::parse(lines[0]);
    CHECK(first["type"] == "message");
    CHECK(first["schemaVersion"] == 3);
    CHECK(first["sessionId"] == "20260910-120000-AAAAAA");
    CHECK(first["runId"] == "run-000001");
    CHECK(first["seq"] == 1);
    CHECK(first["turnId"].is_null());
    CHECK(first["message"]["role"] == "system");
    CHECK(first["systemMeta"]["cause"] == "initial");
    CHECK(first["prevHash"] == std::string(kGenesisHash));

    auto second = nlohmann::json::parse(lines[1]);
    CHECK(second["type"] == "event");
    CHECK(second["kind"] == "session.started");
    CHECK(second["seq"] == 2);  // 两类行共用递增 seq
    CHECK(second["payload"]["context"]["revision"] == 1);
    CHECK(second["payload"]["context"]["contextChain"].size() == 1);

    CHECK(writer->context().revision == 1);
    CHECK(writer->context().system_message_ref == first["messageId"]);
}

TEST_CASE("哈希链承继 v2:衔接、确定性、VerifyV3File 全绿") {
    Harness harness("chain");
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        MessageDraft user;
        user.turn_id = "turn-000001";
        user.purpose = MessagePurpose::Conversation;
        user.origin = MessageOrigin::Human;
        user.message =
            nlohmann::json::object({{"role", "user"}, {"content", "查一下天气。"}});
        WriteReceipt receipt = writer->AppendMessage(std::move(user), Durability::PowerLoss);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
        WriteReceipt admit = writer->AdmitMessages({receipt.id});
        REQUIRE(admit.status == WriteReceipt::Status::Committed);
        CHECK(writer->context().revision == 2);
        CHECK(writer->context().chain.size() == 2);
    }
    V3VerifyReport report = VerifyV3File(harness.jsonl);
    REQUIRE(report.ok);
    CHECK(report.lines == 4);  // system + started + user + applied
    CHECK(report.context.revision == 2);
    CHECK(report.context.chain.size() == 2);

    // 再开一次同路径的卷:create-new 拒绝。
    auto again = V3Writer::Start(harness.jsonl, "x", "y", "z");
    CHECK(!again.has_value());
}

TEST_CASE("prepared:引用先落稳才许落,空缺引用拒收") {
    Harness harness("prepared");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());

    MessageDraft user;
    user.turn_id = "turn-000001";
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", "你好"}});
    WriteReceipt user_receipt = writer->AppendMessage(std::move(user), Durability::PowerLoss);
    REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);
    REQUIRE(writer->AdmitMessages({user_receipt.id}).status == WriteReceipt::Status::Committed);

    // 未落稳的 system 引用拒收(§4.4:不能"从历史猜一份")。
    WriteReceipt bad = writer->PrepareRequest(
        "request-000001", "turn-000001", "step-000001", "conversation", "msg-999999",
        {user_receipt.id}, nlohmann::json::object());
    CHECK(bad.status == WriteReceipt::Status::Rejected);
    CHECK(bad.error_code == "v3writer.dangling_ref");

    // 引用齐全:inputMessageRefs 按序、readThroughSeq/hash 落档。
    WriteReceipt good = writer->PrepareRequest(
        "request-000001", "turn-000001", "step-000001", "conversation",
        writer->context().system_message_ref, {user_receipt.id},
        nlohmann::json::object({{"provider", "moonshot"}, {"model", "kimi-k2.6"}}));
    REQUIRE(good.status == WriteReceipt::Status::Committed);

    auto lines = ReadLines(harness.jsonl);
    bool found = false;
    for (const auto& line : lines) {
        auto json = nlohmann::json::parse(line);
        if (json.value("kind", "") == "model.request.prepared") {
            found = true;
            CHECK(json["payload"]["inputMessageRefs"].size() == 1);
            CHECK(json["payload"]["inputMessageRefs"][0] == user_receipt.id);
            CHECK(json["payload"]["contextRevision"] == 2);
            CHECK(json["payload"].contains("readThroughHash"));
        }
    }
    CHECK(found);
}

TEST_CASE("崩溃恢复:Continue 重放视图、尾行截断明报、发号续号") {
    Harness harness("recovery");
    std::string user_id;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        MessageDraft user;
        user.turn_id = "turn-000001";
        user.purpose = MessagePurpose::Conversation;
        user.origin = MessageOrigin::Human;
        user.message = nlohmann::json::object({{"role", "user"}, {"content", "继续"}});
        WriteReceipt receipt = writer->AppendMessage(std::move(user), Durability::PowerLoss);
        user_id = receipt.id;
        REQUIRE(writer->AdmitMessages({user_id}).status == WriteReceipt::Status::Committed);
    }
    SUBCASE("干净续卷") {
        auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
        REQUIRE(continued.has_value());
        CHECK(continued->context().revision == 2);
        CHECK(continued->context().chain.size() == 2);
        CHECK(continued->HasMessageId(user_id));
        // 续写一行,链继续衔接。
        EventDraft extra;
        extra.kind = EventKindV3::SessionEnded;
        extra.payload = nlohmann::json::object({{"reason", "user_quit"}});
        WriteReceipt receipt = continued->AppendEvent(std::move(extra), Durability::PowerLoss);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
        CHECK(receipt.seq == 5);
        V3VerifyReport report = VerifyV3File(harness.jsonl);
        CHECK(report.ok);
    }
    SUBCASE("尾行截断:明报拒开,不偷偷裁") {
        // 模拟崩溃半行:去掉末行换行。
        std::ifstream in(harness.jsonl, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        REQUIRE(!data.empty() && data.back() == '\n');
        data.pop_back();
        std::ofstream out(harness.jsonl, std::ios::binary | std::ios::trunc);
        out << data;
        out.close();
        V3VerifyReport report = VerifyV3File(harness.jsonl);
        CHECK(!report.ok);
        CHECK(report.truncated_tail);
        auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
        CHECK(!continued.has_value());  // 写入侧不裁尾
    }
    SUBCASE("坏链:拒开") {
        auto lines = ReadLines(harness.jsonl);
        // 篡改末行正文(哈希对不上)。
        auto json = nlohmann::json::parse(lines.back());
        json["payload"]["reason"] = "tampered";
        lines.back() = json.dump();
        std::ofstream out(harness.jsonl, std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) {
            out << line << "\n";
        }
        auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
        CHECK(!continued.has_value());
    }
}

TEST_CASE("发号续号不撞:Continue 后新 ID 不与旧 ID 重叠") {
    Harness harness("ids");
    std::string last_event_id;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        EventDraft noise;
        noise.kind = EventKindV3::SessionEnded;
        noise.payload = nlohmann::json::object({{"reason", "x"}});
        WriteReceipt receipt = writer->AppendEvent(std::move(noise), Durability::PowerLoss);
        last_event_id = receipt.id;
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    }
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    std::string next = continued->NewEventId();
    CHECK(next != last_event_id);
    CHECK(next > last_event_id);  // msg/evt 计数器从重放行恢复
}
