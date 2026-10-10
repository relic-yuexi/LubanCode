// v3 writer 核心测试(P1 写入侧):开卷首行 system(seq=1、turnId=null、
// 自带身份)、两类行共链共 seq、哈希链确定性、退出不造空回合、prepared
// 引用先落稳、崩溃恢复(Continue 重放视图/尾行截断明报/发号续号)。
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/writer.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/canonical_json.hpp"

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

    std::optional<V3Writer> Start(V3WriterOptions options = {}) {
        auto writer = V3Writer::Start(jsonl, "20260910-120000-AAAAAA", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      std::move(options), &clock);
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

TEST_CASE("usage observation requires one actual prepared/sent source across write verify and Continue") {
    Harness harness("usage-observation-source");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    EventDraft observation;
    observation.kind = EventKindV3::ModelUsageObserved;
    observation.request_id = "request-000001";
    observation.turn_id = "turn-000001";
    observation.step_id = "step-000001";
    observation.payload = {{"version", 1}, {"numbers", nlohmann::json::array({-3, 5, 7, 11, 13})},
        {"reportedByProvider", true}, {"incomplete", true}, {"providerResponseId", "real-id"}};
    const auto untouched = writer->next_seq();
    CHECK(writer->AppendEvent(observation, Durability::PowerLoss).error_code == "v3writer.usage_source_missing");
    CHECK(writer->next_seq() == untouched);
    REQUIRE(writer->PrepareRequest("request-000001", "turn-000001", "step-000001", "conversation",
        writer->context().system_message_ref, {}, {{"provider", "fixture"}, {"model", "fixture-model"},
        {"wire", "responses"}}).status == WriteReceipt::Status::Committed);
    CHECK(writer->AppendEvent(observation, Durability::PowerLoss).error_code == "v3writer.usage_source_missing");
    EventDraft sent;
    sent.kind = EventKindV3::ModelRequestSent;
    sent.status = OpStatus::Done;
    sent.request_id = observation.request_id;
    sent.turn_id = observation.turn_id;
    sent.step_id = observation.step_id;
    sent.payload = {{"deliveryScope", "local_transport"}};
    REQUIRE(writer->AppendEvent(sent, Durability::PowerLoss).status == WriteReceipt::Status::Committed);
    auto foreign = observation;
    foreign.step_id = "other-step";
    CHECK(writer->AppendEvent(foreign, Durability::PowerLoss).error_code == "v3writer.usage_source_mismatch");
    REQUIRE(writer->AppendEvent(observation, Durability::PowerLoss).status == WriteReceipt::Status::Committed);
    CHECK(writer->AppendEvent(observation, Durability::PowerLoss).error_code == "v3writer.usage_observation_duplicate");
    REQUIRE(writer->Close().has_value());
    const auto original = ReadLines(harness.jsonl);
    REQUIRE(VerifyV3Lines(original).ok);
    REQUIRE(ReadV3LedgerOwned(harness.jsonl, original).has_value());
    auto continued = V3Writer::Continue(harness.jsonl);
    REQUIRE(continued.has_value());
    CHECK(continued->AppendEvent(observation, Durability::PowerLoss).error_code == "v3writer.usage_observation_duplicate");
    REQUIRE(continued->Close().has_value());
    CHECK(ReadLines(harness.jsonl) == original);

    // Recomputed hashes are insufficient when the actual source relation is broken.
    for (int variant = 0; variant < 4; ++variant) {
        INFO(variant);
        std::vector<nlohmann::json> rows;
        for (const auto& raw : original) rows.push_back(nlohmann::json::parse(raw));
        if (variant == 0) rows.back()["requestId"] = "foreign-request";
        if (variant == 1) rows.back()["turnId"] = "foreign-turn";
        if (variant == 2) {
            rows.push_back(rows.back());
            rows.back()["eventId"] = "evt-999999";
        }
        if (variant == 3) rows.erase(rows.end() - 2); // Remove the committed sent boundary.
        std::vector<std::string> changed;
        std::string previous(kGenesisHash);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            row.erase("prevHash"); row.erase("lineHash");
            row["seq"] = std::uint64_t(i + 1);
            const auto canonical = lubancode::trajectory::CanonicalJsonDump(row);
            REQUIRE(canonical.has_value());
            const auto hash = ComputeLineHash(previous, *canonical);
            row["prevHash"] = previous; row["lineHash"] = hash;
            const auto encoded = lubancode::trajectory::CanonicalJsonDump(row);
            REQUIRE(encoded.has_value());
            changed.push_back(*encoded);
            previous = hash;
        }
        CHECK_FALSE(VerifyV3Lines(changed).ok);
        CHECK_FALSE(ReadV3LedgerOwned(harness.jsonl, changed).has_value());
    }
}

TEST_CASE("usage observation native append and semantic confirmation failures remain distinguishable on recovery") {
    Harness harness("usage-observation-fault");
    bool fail_before = false, fail_after = false;
    V3WriterOptions options;
    options.inject_io_failure = [&]() -> std::optional<std::string> {
        if (fail_before) return std::string("usage observation append refused");
        return std::nullopt;
    };
    options.after_native_append = [&] { if (fail_after) throw 19; };
    auto writer = harness.Start(std::move(options));
    REQUIRE(writer.has_value());
    REQUIRE(writer->PrepareRequest("request-000001", "turn-000001", "step-000001", "conversation",
        writer->context().system_message_ref, {}, {{"provider", "fixture"}, {"model", "fixture-model"},
        {"wire", "responses"}}).status == WriteReceipt::Status::Committed);
    EventDraft sent;
    sent.kind = EventKindV3::ModelRequestSent;
    sent.status = OpStatus::Done;
    sent.request_id = "request-000001";
    sent.turn_id = "turn-000001";
    sent.step_id = "step-000001";
    sent.payload = {{"deliveryScope", "local_transport"}};
    REQUIRE(writer->AppendEvent(sent, Durability::PowerLoss).status == WriteReceipt::Status::Committed);
    EventDraft observation = sent;
    observation.kind = EventKindV3::ModelUsageObserved;
    observation.status.reset();
    observation.payload = {{"version", 1}, {"numbers", nlohmann::json::array({3, 5, 7, 11, 13})},
        {"reportedByProvider", true}, {"incomplete", true}, {"providerResponseId", "real-id"}};
    SUBCASE("before append") { fail_before = true; }
    SUBCASE("after actual append") { fail_after = true; }
    const auto receipt = writer->AppendEvent(observation, Durability::PowerLoss);
    CHECK(receipt.status != WriteReceipt::Status::Committed);
    CHECK(writer->broken());
    CHECK(receipt.error_code == (fail_before ? "v3writer.injected" : "v3writer.completion_unconfirmed"));
    writer.reset(); // Retire the broken handle before taking a fresh recovery owner.
    auto restored = V3Writer::Continue(harness.jsonl);
    REQUIRE(restored.has_value());
    if (fail_before) {
        CHECK(restored->AppendEvent(observation, Durability::PowerLoss).status == WriteReceipt::Status::Committed);
    } else {
        CHECK(restored->AppendEvent(observation, Durability::PowerLoss).error_code == "v3writer.usage_observation_duplicate");
        REQUIRE(receipt.journal_append.has_value());
        CHECK_FALSE(receipt.id.empty());
    }
    REQUIRE(restored->Close().has_value());
    const auto ledger = ReadV3Ledger(harness.jsonl);
    REQUIRE(ledger.has_value());
    int observations = 0;
    for (const auto& event : ledger->events) {
        if (event.kind != EventKindV3::ModelUsageObserved) continue;
        ++observations;
        CHECK(event.payload.at("numbers") == observation.payload.at("numbers"));
    }
    CHECK(observations == 1);
}

TEST_CASE("V3Writer: effective host binding survives switches and Continue") {
    Harness harness("host-bindings");
    const nlohmann::json bindings{{"skills", {{"schemaVersion", 1}, {"sha256", "plan-a"}}}};
    auto writer = V3Writer::Start(harness.jsonl, "20260910-120000-AAAAAA", "run-000001",
        "original", {{"settingsVersion", 7}, {"hostBindings", bindings}, {"initialOnly", "old"}});
    REQUIRE(writer.has_value());
    const auto first_root = writer->context().system_message_ref;
    const auto switched = writer->SwitchSystem("replacement",
        {{"cause", "system_prompt_changed"}, {"settingsVersion", 8}, {"systemChanged", true},
         {"hostBindings", {{"skills", "caller-must-not-replace"}}}});
    REQUIRE(switched.change_event.status == WriteReceipt::Status::Committed);
    REQUIRE(switched.system_message.status == WriteReceipt::Status::Committed);
    REQUIRE(switched.apply_event.status == WriteReceipt::Status::Committed);
    auto rows = ReadLines(harness.jsonl);
    REQUIRE(rows.size() == 5);
    const auto change = nlohmann::json::parse(rows[2]);
    const auto system = nlohmann::json::parse(rows[3]);
    const auto applied = nlohmann::json::parse(rows[4]);
    CHECK(change["payload"]["oldSystemMessageRef"] == first_root);
    CHECK(change["payload"]["hostBindings"] == bindings);
    CHECK(system["systemMeta"]["hostBindings"] == bindings);
    CHECK(system["systemMeta"]["settingsVersion"] == 8);
    CHECK(system["systemMeta"]["cause"] == "system_prompt_changed");
    CHECK_FALSE(system["systemMeta"].contains("initialOnly"));
    CHECK(applied["payload"]["hostBindings"] == bindings);
    REQUIRE(writer->Close().has_value());
    auto continued = V3Writer::Continue(harness.jsonl);
    REQUIRE(continued.has_value());
    const auto next = continued->SwitchSystem("third",
        {{"cause", "system_prompt_changed"}, {"settingsVersion", 9}, {"systemChanged", true}});
    REQUIRE(next.apply_event.status == WriteReceipt::Status::Committed);
    rows = ReadLines(harness.jsonl);
    REQUIRE(rows.size() == 8);
    CHECK(nlohmann::json::parse(rows[6])["systemMeta"]["hostBindings"] == bindings);
    CHECK(nlohmann::json::parse(rows[6])["systemMeta"]["settingsVersion"] == 9);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("V3Writer: an unadopted system cannot supply host bindings") {
    Harness harness("unadopted-host-bindings");
    const nlohmann::json bindings{{"skills", "adopted-plan"}};
    auto writer = V3Writer::Start(harness.jsonl, "20260910-120000-AAAAAA", "run-000001",
        "adopted", {{"settingsVersion", 1}, {"hostBindings", bindings}});
    REQUIRE(writer.has_value());
    const auto adopted_id = writer->context().system_message_ref;
    MessageDraft pending;
    pending.origin = MessageOrigin::SessionRuntime;
    pending.message = {{"role", "system"}, {"content", "unadopted"}};
    pending.system_meta = nlohmann::json{{"cause", "initial"}, {"changeEventRef", nullptr},
        {"systemChanged", false}, {"settingsVersion", 99},
        {"hostBindings", {{"skills", "unadopted-plan"}}}};
    REQUIRE(writer->AppendMessage(std::move(pending), Durability::PowerLoss).status == WriteReceipt::Status::Committed);
    REQUIRE(writer->Close().has_value());
    auto continued = V3Writer::Continue(harness.jsonl);
    REQUIRE(continued.has_value());
    REQUIRE(continued->context().system_message_ref == adopted_id);
    REQUIRE(continued->SwitchSystem("next", {{"cause", "system_prompt_changed"},
        {"settingsVersion", 2}, {"systemChanged", true}}).apply_event.status == WriteReceipt::Status::Committed);
    const auto rows = ReadLines(harness.jsonl);
    REQUIRE(rows.size() == 6);
    CHECK(nlohmann::json::parse(rows[3])["payload"]["oldSystemMessageRef"] == adopted_id);
    CHECK(nlohmann::json::parse(rows[4])["systemMeta"]["hostBindings"] == bindings);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

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
    CHECK(first["messageId"].get<std::string>() == writer->context().system_message_ref);
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

TEST_CASE("关柄:保留只读身份,拒绝迟到提交,对象存活时也能改名续接") {
    Harness harness("close");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    const auto before = ReadLines(harness.jsonl);
    const auto next_seq = writer->next_seq();
    const std::string last_hash = writer->last_line_hash();
    const std::string system_id = writer->context().system_message_ref;

    REQUIRE(writer->Close().has_value());
    REQUIRE(writer->Close().has_value());  // 重复关闭不碰已释放的句柄。
    CHECK_FALSE(writer->broken());
    CHECK(writer->path() == harness.jsonl);
    CHECK(writer->session_id() == "20260910-120000-AAAAAA");
    CHECK(writer->run_id() == "run-000001");
    CHECK(writer->context().system_message_ref == system_id);
    CHECK(writer->HasMessageId(system_id));

    MessageDraft user;
    user.turn_id = "turn-000001";
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json{{"role", "user"}, {"content", "迟到输入"}};
    const auto late_message = writer->AppendMessage(user, Durability::PowerLoss);
    CHECK(late_message.status == WriteReceipt::Status::Rejected);
    CHECK(late_message.error_code == "v3writer.closed");
    EventDraft late;
    late.kind = EventKindV3::SessionEnded;
    late.payload = nlohmann::json{{"reason", "exit"}, {"closeQuality", "clean"}};
    const auto late_event = writer->AppendEvent(std::move(late), Durability::PowerLoss);
    CHECK(late_event.status == WriteReceipt::Status::Rejected);
    CHECK(late_event.error_code == "v3writer.closed");
    CHECK(writer->next_seq() == next_seq);
    CHECK(writer->last_line_hash() == last_hash);
    CHECK(writer->context().revision == 1);
    CHECK(ReadLines(harness.jsonl) == before);

    // Windows 不允许改名仍被写句柄占用的文件;这里故意不析构 writer。
    const auto moved = harness.dir / "closed.jsonl";
    std::error_code ec;
    std::filesystem::rename(harness.jsonl, moved, ec);
    INFO(ec.message());
    REQUIRE_FALSE(ec);
    auto continued = V3Writer::Continue(moved);
    REQUIRE(continued.has_value());
    const auto accepted = continued->AppendMessage(std::move(user), Durability::PowerLoss);
    CHECK(accepted.status == WriteReceipt::Status::Committed);
    CHECK(accepted.seq == next_seq);
    CHECK(VerifyV3File(moved).ok);
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
        REQUIRE(!data.empty());
        REQUIRE(data.back() == '\n');
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
