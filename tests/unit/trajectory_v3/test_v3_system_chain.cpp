// v3 system 版本链测试(§4.3 三态):完整切换(旧 system -> system.change
// -> 新 system -> context.system.applied,内存换根);只落 change 事件就崩
// (恢复仍用旧版,变更未完成);新 system 落稳、applied 未落稳就崩(候选
// 在档,恢复仍用旧根)。systemChanged=false 不制造假版本差异仍留档。
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
                                    ("lubancode-v3-soul-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
    }

    std::optional<V3Writer> Start() {
        auto writer = V3Writer::Start(jsonl, "20260910-120000-AAAAAA", "run-000001",
                                      "旧 system 正文。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
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

TEST_CASE("完整切换:三步齐,内存换根,旧请求仍指旧版") {
    Harness harness("full");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    const std::string old_root = writer->context().system_message_ref;
    const std::uint64_t revision_before = writer->context().revision;

    // 接纳一条用户消息,链尾有内容(切换应保留后续节点并重接)。
    MessageDraft user;
    user.turn_id = "turn-000001";
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", "在吗"}});
    WriteReceipt user_receipt = writer->AppendMessage(std::move(user), Durability::PowerLoss);
    REQUIRE(writer->AdmitMessages({user_receipt.id}).status == WriteReceipt::Status::Committed);

    V3Writer::SwitchSystemResult result = writer->SwitchSystem(
        "新 system 正文(拼装后的完整结果)。",
        nlohmann::json::object({{"cause", "soul_loaded"},
                                {"settingsVersion", 7},
                                {"soulRef", "souls/reviewer.soul.md"}}),
        MessageOrigin::Soul);
    REQUIRE(result.change_event.status == WriteReceipt::Status::Committed);
    REQUIRE(result.system_message.status == WriteReceipt::Status::Committed);
    REQUIRE(result.apply_event.status == WriteReceipt::Status::Committed);

    // 内存换根:revision 前进(admit 一次 + 切换一次),链根为新 system,
    // 旧链后续节点保留重接([新system, user])。
    CHECK(writer->context().system_message_ref == result.system_message.id);
    CHECK(writer->context().revision == revision_before + 2);
    REQUIRE(writer->context().chain.size() == 2);
    CHECK(writer->context().chain[0].message_ref == result.system_message.id);
    CHECK(writer->context().chain[1].message_ref == user_receipt.id);
    CHECK(writer->context().chain[1].prev_message_ref == result.system_message.id);

    // 档上三步齐全,新 system 回指变更事件,旧消息不改不删。
    auto lines = ReadJsonLines(harness.jsonl);
    int change_events = 0, systems = 0, applies = 0;
    std::string change_event_id, new_system_id;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "system.change") {
            ++change_events;
            change_event_id = line["eventId"];
            CHECK(line["payload"]["oldSystemMessageRef"] == old_root);
            CHECK(line["payload"]["cause"] == "soul_loaded");
        }
        if (line.value("type", "") == "message" && line["message"]["role"] == "system" &&
            line["systemMeta"]["cause"] != "initial") {
            ++systems;
            new_system_id = line["messageId"];
            CHECK(line["systemMeta"]["changeEventRef"] == change_event_id);
            CHECK(line["turnId"].is_null());
        }
        if (line.value("kind", "") == "context.system.applied") {
            ++applies;
            CHECK(line["payload"]["rootMessageRef"] == new_system_id);
            CHECK(line["payload"]["contextChain"].size() == 2);
        }
    }
    CHECK(change_events == 1);
    CHECK(systems == 1);
    CHECK(applies == 1);

    V3VerifyReport report = VerifyV3File(harness.jsonl);
    CHECK(report.ok);
    CHECK(report.context.system_message_ref == result.system_message.id);
}

TEST_CASE("切换事件后崩溃:恢复仍用旧版,变更未完成") {
    Harness harness("crash1");
    std::string old_root;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        old_root = writer->context().system_message_ref;
        // 只落 system.change(第一步),模拟随后崩溃。
        EventDraft change;
        change.kind = EventKindV3::SystemChange;
        change.payload = nlohmann::json::object(
            {{"cause", "soul_loaded"}, {"oldSystemMessageRef", old_root},
             {"settingsVersion", 7}, {"systemChanged", true}});
        REQUIRE(writer->AppendEvent(std::move(change), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    }
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    // 旧 system 仍有效;system.change 不换根。
    CHECK(continued->context().system_message_ref == old_root);
    CHECK(continued->context().revision == 1);
    CHECK(continued->context().chain.size() == 1);
}

TEST_CASE("新 system 落稳、applied 未落稳就崩:恢复仍用旧根") {
    Harness harness("crash2");
    std::string old_root;
    {
        auto writer = harness.Start();
        REQUIRE(writer.has_value());
        old_root = writer->context().system_message_ref;
        // 落 change + 新 system 消息,模拟 applied 前崩溃。
        EventDraft change;
        change.kind = EventKindV3::SystemChange;
        change.payload = nlohmann::json::object(
            {{"cause", "soul_loaded"}, {"oldSystemMessageRef", old_root},
             {"settingsVersion", 7}, {"systemChanged", true}});
        WriteReceipt change_receipt = writer->AppendEvent(std::move(change),
                                                          Durability::PowerLoss);
        REQUIRE(change_receipt.status == WriteReceipt::Status::Committed);
        MessageDraft system_draft;
        system_draft.purpose = MessagePurpose::Conversation;
        system_draft.origin = MessageOrigin::Soul;
        system_draft.system_meta = nlohmann::json::object(
            {{"cause", "soul_loaded"},
             {"changeEventRef", change_receipt.id},
             {"settingsVersion", 7},
             {"systemChanged", true}});
        system_draft.message =
            nlohmann::json::object({{"role", "system"}, {"content", "孤儿候选 system"}});
        REQUIRE(writer->AppendMessage(std::move(system_draft), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    }
    auto continued = V3Writer::Continue(harness.jsonl, V3WriterOptions{});
    REQUIRE(continued.has_value());
    // 候选在档,但未 applied 不生效:恢复用旧根。
    CHECK(continued->context().system_message_ref == old_root);
    CHECK(continued->context().chain.size() == 1);
}

TEST_CASE("systemChanged=false:设置变了正文没变,不制造假版本差异") {
    Harness harness("nochange");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    V3Writer::SwitchSystemResult result = writer->SwitchSystem(
        "旧 system 正文。",  // 字节未变
        nlohmann::json::object({{"cause", "settings_changed"},
                                {"settingsVersion", 8},
                                {"systemChanged", false}}));
    REQUIRE(result.change_event.status == WriteReceipt::Status::Committed);
    REQUIRE(result.system_message.status == WriteReceipt::Status::Committed);
    REQUIRE(result.apply_event.status == WriteReceipt::Status::Committed);
    auto lines = ReadJsonLines(harness.jsonl);
    for (const auto& line : lines) {
        if (line.value("kind", "") == "system.change") {
            CHECK(line["payload"]["systemChanged"] == false);
        }
    }
    // 链根指向字节相同的新 message(留档了),revision 前进一次。
    CHECK(writer->context().system_message_ref == result.system_message.id);
}
