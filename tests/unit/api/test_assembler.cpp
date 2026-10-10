// 脚本化 StreamEvent 序列,验证 MessageAssembler 攒出的 Message 对不对:
// 纯 text、单 tool_use、text+tool_use 混合、input JSON 劈多段、input 非法
// JSON 报错但不崩。

#include <doctest/doctest.h>

#include <variant>
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

#include "api/assembler.hpp"
#include "api/chat/events.hpp"
#include "api/gemini/events.hpp"
#include "api/anthropic/events.hpp"
#include "api/responses/events.hpp"
#include "api/types.hpp"
#include "api/usage_json.hpp"
#include "api/usage_lexical.hpp"
#include "api/usage_event_projection.hpp"
#include "api/usage_aggregation.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8:清洗结果断言

using namespace lubancode::api;

TEST_CASE("Provider numeric owner precedes raw material admission through shared normalization") {
    namespace wire = usage_wire;
    namespace facts = ::lubancore::usage::v1;
    using Normalize = std::expected<wire::Snapshot, std::string_view> (*)(
        const nlohmann::json&, const std::vector<facts::RawField>*, wire::NumericObserver, void*);
    const std::array<Normalize,4> normalize{wire::Chat, wire::Responses, wire::Anthropic, wire::Gemini};
    const std::array<nlohmann::json,4> usages{
        nlohmann::json{{"prompt_tokens",41},{"completion_tokens",7},
            {"prompt_tokens_details",{{"cached_tokens",13},{"cache_write_tokens",17}}},
            {"completion_tokens_details",{{"reasoning_tokens",3}}}},
        nlohmann::json{{"input_tokens",41},{"output_tokens",7},
            {"input_tokens_details",{{"cached_tokens",13},{"cache_write_tokens",17}}},
            {"output_tokens_details",{{"reasoning_tokens",3}}}},
        nlohmann::json{{"input_tokens",11},{"output_tokens",7},
            {"cache_read_input_tokens",13},{"cache_creation_input_tokens",17}},
        nlohmann::json{{"promptTokenCount",24},{"cachedContentTokenCount",13},
            {"candidatesTokenCount",4},{"thoughtsTokenCount",3},{"totalTokenCount",31}}
    };
    const std::array<const char*,4> paths{"usage.prompt_tokens", "usage.input_tokens",
        "usage.input_tokens", "usageMetadata.promptTokenCount"};
    const std::array<std::int64_t,4> raw_inputs{41,41,11,24};
    for (std::size_t provider = 0; provider < normalize.size(); ++provider)
        for (const bool reject_material : {false,true}) {
            INFO(provider); INFO(reject_material);
            struct Owner { wire::NumericValues values{}; int calls = 0; } owner;
            std::vector<facts::RawField> lexical;
            if (reject_material) {
                facts::RawField raw;
                raw.path = paths[provider]; raw.kind = facts::RawKind::SignedInteger;
                raw.integer = raw_inputs[provider];
                raw.summary = std::string(facts::kMaxSummaryBytes + 1, 'x');
                lexical.push_back(std::move(raw));
            }
            const auto snapshot = normalize[provider](usages[provider], &lexical,
                [](void* context, const wire::NumericValues& values) noexcept {
                    auto& target = *static_cast<Owner*>(context);
                    target.values = values; ++target.calls;
                }, &owner);
            REQUIRE(snapshot.has_value()); CHECK(owner.calls == 1);
            const wire::NumericValues expected{11,7,13,provider == 3 ? 0 : 17,provider == 2 ? 0 : 3};
            CHECK(owner.values == expected); CHECK(snapshot->values == expected);
            if (reject_material) {
                CHECK(snapshot->material_error == "usage.wire.raw_field");
                const auto delivered = wire::Nonterminal(*snapshot);
                CHECK_FALSE(delivered.usage_observation.has_value());
                CHECK(delivered.usage.input_tokens == 11); CHECK(delivered.usage.output_tokens == 7);
            } else CHECK(snapshot->material_error.empty());
        }
}

TEST_CASE("Four production parsers and nonstream fallback preserve accounting when response identity is refused") {
    const auto require = [](const std::vector<StreamEvent>& events,
                            std::array<std::int64_t,5> expected) {
        MessageAssembler owner;
        int snapshots = 0, errors = 0;
        for (const auto& event : events) {
            CHECK_FALSE(std::holds_alternative<ProviderResponseIdentity>(event));
            CHECK_FALSE(std::holds_alternative<MessageDone>(event));
            CHECK_FALSE(std::holds_alternative<TextDelta>(event));
            if (const auto* usage = std::get_if<UsageSnapshot>(&event)) {
                ++snapshots; CHECK_FALSE(usage->provider_response_id);
            }
            if (const auto* error = std::get_if<StreamError>(&event)) {
                ++errors; CHECK(error->code == "usage.response_id.invalid");
            }
            owner.Feed(event);
        }
        CHECK(snapshots == 1); CHECK(errors == 1); CHECK(owner.usage_seen());
        CHECK(owner.usage().input_tokens == expected[0]); CHECK(owner.usage().output_tokens == expected[1]);
        CHECK(owner.usage().cache_read_tokens == expected[2]); CHECK(owner.usage().cache_creation_tokens == expected[3]);
        CHECK(owner.usage().output_reasoning_tokens == expected[4]);
        CHECK_FALSE(owner.provider_response_id()); CHECK(owner.stop_reason().empty());
    };
    namespace facts = ::lubancore::usage::v1;
    const std::array<nlohmann::json,2> refused_ids{
        nlohmann::json(17), nlohmann::json(std::string(facts::kMaxResponseIdBytes+1,'x'))};
    for (const auto& id : refused_ids) {
        nlohmann::json chat_body{{"id",id},{"choices",nlohmann::json::array()},
            {"usage",{{"prompt_tokens",41},{"completion_tokens",7},
                {"prompt_tokens_details",{{"cached_tokens",13},{"cache_write_tokens",17}}},
                {"completion_tokens_details",{{"reasoning_tokens",3}}}}}};
        chat::EventParser chat_parser;
        require(chat_parser.Consume(SseFrame{"",chat_body.dump()}),{11,7,13,17,3});
        nlohmann::json response{{"id",id},{"output",nlohmann::json::array()},
            {"usage",{{"input_tokens",41},{"output_tokens",7},
                {"input_tokens_details",{{"cached_tokens",13},{"cache_write_tokens",17}}},
                {"output_tokens_details",{{"reasoning_tokens",3}}}}}};
        responses::EventParser response_parser;
        require(response_parser.Consume(SseFrame{"",nlohmann::json{{"type","response.created"},
            {"response",response}}.dump()}),{11,7,13,17,3});
        responses::EventParser fallback_parser;
        require(fallback_parser.ExpandNonStream(response.dump()),{11,7,13,17,3});
        gemini::EventParser gemini_parser;
        require(gemini_parser.Consume(SseFrame{"",nlohmann::json{{"responseId",id},
            {"usageMetadata",{{"promptTokenCount",24},{"cachedContentTokenCount",13},
                {"candidatesTokenCount",4},{"thoughtsTokenCount",3},{"totalTokenCount",31}}}}.dump()}),
            {11,7,13,0,3});
        anthropic::EventParser anthropic_parser;
        require(anthropic_parser.Consume(SseFrame{"",nlohmann::json{{"type","message_start"},
            {"message",{{"id",id},{"model","fixture"},{"content",nlohmann::json::array()},
                {"usage",{{"input_tokens",11},{"output_tokens",7},
                    {"cache_read_input_tokens",13},{"cache_creation_input_tokens",17}}}}}}.dump()}),
            {11,7,13,17,0});
    }
}

TEST_CASE("Anthropic numeric checkpoint keeps missing, malformed and lexical cumulative updates distinct") {
    namespace facts = ::lubancore::usage::v1;
    usage_wire::AnthropicAccounting accounting;
    usage_wire::NumericDeliveryOwner owner;
    const auto absorb = [&](const nlohmann::json& input,
                            const std::vector<facts::RawField>* lexical = nullptr) {
        accounting.Absorb(input, lexical,
            [](void* context, const usage_wire::AnthropicAccounting::NumericValues& values) noexcept {
                static_cast<usage_wire::NumericDeliveryOwner*>(context)->Own(values);
            }, &owner);
    };
    absorb(nlohmann::json{{"input_tokens",11},{"output_tokens",7},
                         {"cache_read_input_tokens",13},{"cache_creation_input_tokens",17}});
    auto pending = owner.Pending(); REQUIRE(pending);
    CHECK(pending->usage.input_tokens == 11); CHECK(pending->usage.output_tokens == 7);
    CHECK(pending->usage.cache_read_tokens == 13); CHECK(pending->usage.cache_creation_tokens == 17);
    CHECK(pending->usage.output_reasoning_tokens == 0);
    CHECK_FALSE(pending->usage_observation); CHECK_FALSE(pending->provider_response_id);

    absorb(nlohmann::json{{"input_tokens","bad"},{"output_tokens",0},
                         {"cache_read_input_tokens",(std::numeric_limits<std::uint64_t>::max)()}});
    pending = owner.Pending(); REQUIRE(pending);
    CHECK(pending->usage.input_tokens == 11); CHECK(pending->usage.output_tokens == 0);
    CHECK(pending->usage.cache_read_tokens == 13); CHECK(pending->usage.cache_creation_tokens == 17);
    auto material = accounting.View(); REQUIRE(material); CHECK(material->material_error.empty());
    CHECK(material->values[0] == 11); CHECK(material->values[1] == 0);
    CHECK(material->observation.fields[0].validity == facts::Validity::InvalidType);
    CHECK(material->observation.fields[2].validity == facts::Validity::OutOfRange);
    CHECK(material->observation.fields[3].validity == facts::Validity::ValidInteger);
    CHECK(material->observation.fields[4].presence == facts::Presence::Missing);

    facts::RawField raw;
    raw.path = "usage.input_tokens"; raw.kind = facts::RawKind::SignedInteger;
    raw.integer = -3; raw.summary = std::string(facts::kMaxSummaryBytes + 1, 'x');
    const std::vector<facts::RawField> lexical{raw};
    absorb(nlohmann::json{{"input_tokens",99}}, &lexical);
    pending = owner.Pending(); REQUIRE(pending);
    CHECK(pending->usage.input_tokens == -3); // Same lexical scalar wins in numbers and later evidence.
    CHECK(pending->usage.output_tokens == 0); CHECK(pending->usage.cache_read_tokens == 13);
    CHECK(pending->usage.cache_creation_tokens == 17); CHECK_FALSE(pending->usage_observation);
    material = accounting.View(); REQUIRE(material); CHECK_FALSE(material->material_error.empty());
    CHECK(material->values[0] == -3);
    const auto refused = usage_wire::Nonterminal(*material);
    CHECK(refused.usage.input_tokens == -3); CHECK_FALSE(refused.usage_observation);
    CHECK_FALSE(refused.cache_read_reported); CHECK_FALSE(refused.cache_creation_reported);
}

TEST_CASE("Four parser owners retain unpublished usage while preserving the original exception") {
    const auto check = [](auto parser_factory, const char* body, std::array<std::int64_t, 5> numbers) {
        for (int mode = 0; mode < 9; ++mode) {
            INFO(mode);
            auto parser = parser_factory(); MessageAssembler owner;
            int usage_callbacks = 0; bool caught = false;
            const std::function<void(const StreamEvent&)> callback = [&](const StreamEvent& event) {
                if (mode >= 6 && std::holds_alternative<ProviderResponseIdentity>(event)) {
                    if (mode == 7) throw std::bad_alloc{};
                    if (mode == 8) throw 17;
                    throw std::runtime_error("identity callback fault");
                }
                if (std::holds_alternative<UsageSnapshot>(event)) {
                    ++usage_callbacks;
                    if (mode == 4 || mode == 5) throw std::runtime_error("callback fault");
                }
                owner.Feed(event);
            };
            try {
                usage_wire::PendingUsageOnUnwind delivery(parser, callback);
                SseFrame frame; frame.data = body;
                const auto events = parser.Consume(frame);
                REQUIRE(parser.PendingUsage()); // Production parser owns numbers before publication.
                if (mode == 3 || mode == 4 || mode >= 6) {
                    for (const auto& event : events) delivery.Emit(event);
                    CHECK_FALSE(parser.PendingUsage());
                }
                if (mode == 1) throw std::bad_alloc{};
                if (mode == 2) throw 17;
                throw std::runtime_error("publication fault");
            } catch (const std::bad_alloc&) { caught = true; CHECK((mode == 1 || mode == 7)); }
            catch (const std::runtime_error& error) {
                caught = true; CHECK(std::string(error.what()) == (mode == 4 ? "callback fault" :
                    mode == 6 ? "identity callback fault" : "publication fault"));
            } catch (int value) { caught = true; CHECK((mode == 2 || mode == 8)); CHECK(value == 17); }
            CHECK(caught); CHECK(usage_callbacks == 1);
            CHECK(owner.stop_reason().empty());
            if (mode == 4 || mode == 5) {
                CHECK_FALSE(owner.usage_seen()); CHECK_FALSE(owner.usage_observation());
            } else {
                const auto& usage = owner.usage();
                CHECK(usage.input_tokens == numbers[0]); CHECK(usage.output_tokens == numbers[1]);
                CHECK(usage.cache_read_tokens == numbers[2]); CHECK(usage.cache_creation_tokens == numbers[3]);
                CHECK(usage.output_reasoning_tokens == numbers[4]); CHECK(owner.usage_seen());
                CHECK(owner.usage_observation().has_value() == (mode == 3));
                if (mode != 3) CHECK_FALSE(owner.provider_response_id()); // Never invent identity for numeric recovery.
            }
        }
    };
    check([] { return chat::EventParser{}; },
        R"({"id":"actual-chat","choices":[],"usage":{"prompt_tokens":41,"completion_tokens":7,"prompt_tokens_details":{"cached_tokens":13,"cache_write_tokens":17},"completion_tokens_details":{"reasoning_tokens":3}}})",
        {11,7,13,17,3});
    check([] { return responses::EventParser{}; },
        R"({"type":"response.created","response":{"id":"actual-responses","usage":{"input_tokens":41,"output_tokens":7,"input_tokens_details":{"cached_tokens":13,"cache_write_tokens":17},"output_tokens_details":{"reasoning_tokens":3}}}})",
        {11,7,13,17,3});
    check([] { return gemini::EventParser{}; },
        R"({"responseId":"actual-gemini","usageMetadata":{"promptTokenCount":24,"cachedContentTokenCount":13,"candidatesTokenCount":4,"thoughtsTokenCount":3,"totalTokenCount":31}})",
        {11,7,13,0,3});
    check([] { return anthropic::EventParser{}; },
        R"({"type":"message_start","message":{"id":"actual-anthropic","model":"fixture","content":[],"usage":{"input_tokens":11,"output_tokens":7,"cache_read_input_tokens":13,"cache_creation_input_tokens":17}}})",
        {11,7,13,17,0});
    // Exercise the actual HTTP fallback owner, including invalid body shape.
    // Missing output must retain accounting without issuing a success terminal.
    struct NonStreamResponsesOwner : responses::EventParser {
        std::vector<StreamEvent> Consume(const SseFrame& frame) {
            return ExpandNonStream(frame.data);
        }
    };
    check([] { return NonStreamResponsesOwner{}; },
        R"({"id":"actual-nonstream","usage":{"input_tokens":41,"output_tokens":7,"input_tokens_details":{"cached_tokens":13,"cache_write_tokens":17},"output_tokens_details":{"reasoning_tokens":3}}})",
        {11,7,13,17,3});
}

TEST_CASE("Wire material rejection preserves calculated numeric facts without admitting an observation") {
    namespace wire = usage_wire;
    namespace facts = ::lubancore::usage::v1;
    for (const int failure : {0, 1, 2}) {
        INFO(failure);
        wire::Builder builder("source.fixture");
        const nlohmann::json scalar = 11;
        for (std::size_t i = 0; i < facts::kFieldCount; ++i)
            builder.Report(static_cast<facts::Field>(i),
                builder.Capture("usage.field_" + std::to_string(i), &scalar));
        std::string_view expected;
        if (failure == 0) {
            for (std::size_t i = facts::kFieldCount; i <= facts::kMaxRawFields; ++i)
                builder.Capture("usage.extra_" + std::to_string(i), &scalar);
            expected = "usage.wire.raw_limit";
        } else if (failure == 1) {
            for (std::size_t i = 0; i <= facts::kMaxAnomalies; ++i)
                builder.Note(facts::Field::Input, facts::AnomalyCode::AliasConflict, "capacity witness", {});
            expected = "usage.wire.anomaly_limit";
        } else {
            builder.Capture(std::string(facts::kMaxPathBytes + 1, 'x'), &scalar);
            expected = "usage.wire.path";
        }
        auto snapshot = std::move(builder).Finish();
        REQUIRE(snapshot.has_value()); CHECK(snapshot->material_error == expected);
        for (const auto number : snapshot->values) CHECK(number == 11);
        const auto event = wire::Nonterminal(*snapshot, std::string("actual-response"));
        CHECK(event.usage_reported); CHECK_FALSE(event.usage_observation.has_value());
        CHECK_FALSE(event.cache_read_reported); CHECK_FALSE(event.cache_creation_reported);
        CHECK(event.provider_response_id == "actual-response");
        MessageAssembler owner; owner.Feed(event);
        owner.Feed(StreamError{std::string(expected), "usage.material.invalid"});
        CHECK(owner.usage().input_tokens == 11); CHECK(owner.usage().output_tokens == 11);
        CHECK(owner.usage().cache_read_tokens == 11); CHECK(owner.usage().cache_creation_tokens == 11);
        CHECK(owner.usage().output_reasoning_tokens == 11);
        CHECK_FALSE(owner.usage_observation().has_value()); CHECK(owner.stop_reason().empty());
        Usage total; facts::Coverage coverage;
        usage_aggregation::Add(total, coverage, owner.usage(), nullptr);
        CHECK(total.input_tokens == 11);
        for (std::size_t i = 0; i < facts::kFieldCount; ++i)
            CHECK_FALSE(usage_aggregation::Exact(coverage, static_cast<facts::Field>(i)));
    }
}

TEST_CASE("Five-field material capacity: incomplete marking preserves full anomaly history and owns numeric facts") {
    namespace facts = ::lubancore::usage::v1;
    auto original = usage_wire::LegacyBackend(Usage{-3, 9, 11, 13, 2});
    REQUIRE(original); REQUIRE(original->observation.anomalies.size() == 1);
    for (std::size_t i = 1; i < facts::kMaxAnomalies; ++i) {
        auto note = original->observation.anomalies.front();
        note.detail = "retained negative evidence " + std::to_string(i);
        original->observation.anomalies.push_back(std::move(note));
    }
    const auto before = usage_json::Encode(original->observation, usage_wire::Numbers(*original)); REQUIRE(before);
    usage_wire::LexicalUsage::MarkIncomplete(*original);
    CHECK(original->material_error == "usage.material.parse_marker_capacity");
    const auto after = usage_json::Encode(original->observation, usage_wire::Numbers(*original)); REQUIRE(after);
    CHECK(*after == *before); CHECK(original->observation.anomalies.size() == facts::kMaxAnomalies);
    const auto event = usage_wire::Nonterminal(*original, std::string("actual-id"));
    CHECK(event.usage_reported); CHECK_FALSE(event.usage_observation);
    CHECK_FALSE(event.cache_read_reported); CHECK_FALSE(event.cache_creation_reported);
    REQUIRE(event.provider_response_id); CHECK(*event.provider_response_id == "actual-id");
    MessageAssembler assembler; assembler.Feed(event);
    CHECK(assembler.usage().input_tokens == -3); CHECK(assembler.usage().output_tokens == 9);
    CHECK(assembler.usage().cache_read_tokens == 11); CHECK(assembler.usage().cache_creation_tokens == 13);
    CHECK(assembler.usage().output_reasoning_tokens == 2); CHECK(assembler.stop_reason().empty());
    Usage total; facts::Coverage coverage;
    usage_aggregation::Add(total, coverage, assembler.usage(), nullptr);
    for (std::size_t i = 0; i < facts::kFieldCount; ++i)
        CHECK_FALSE(usage_aggregation::Exact(coverage, static_cast<facts::Field>(i)));
    original->observation.anomalies.pop_back(); original->material_error = {};
    usage_wire::LexicalUsage::MarkIncomplete(*original);
    CHECK(original->material_error.empty()); CHECK(original->observation.anomalies.size() == facts::kMaxAnomalies);
    CHECK(original->observation.anomalies.back().code == facts::AnomalyCode::ParseIncomplete);
    const auto once = usage_json::Encode(original->observation, usage_wire::Numbers(*original)); REQUIRE(once);
    usage_wire::LexicalUsage::MarkIncomplete(*original);
    const auto twice = usage_json::Encode(original->observation, usage_wire::Numbers(*original)); REQUIRE(twice);
    CHECK(*once == *twice);  // Repeated refinement does not consume another slot.
}

TEST_CASE("Five-field material capacity: a byte-limit failure rejects provenance without discarding numeric slots") {
    namespace facts = ::lubancore::usage::v1;
    usage_wire::Snapshot snapshot;
    snapshot.values = {17, 9, 11, 13, 2};
    snapshot.observation.provider_namespace = std::string(facts::kMaxNamespaceBytes, 'n');
    for (int i = 0; i < 46; ++i) {
        const auto prefix = "usage." + std::to_string(i) + ".";
        facts::RawField raw;
        raw.path = prefix + std::string(facts::kMaxPathBytes - prefix.size(), 'p');
        raw.kind = facts::RawKind::String; raw.summary = std::string(facts::kMaxSummaryBytes, 's');
        snapshot.observation.raw_fields.push_back(std::move(raw));
    }
    facts::Extension extension;
    extension.namespace_name = std::string(facts::kMaxNamespaceBytes, 'e');
    extension.field.path = std::string(32, 'p'); extension.field.kind = facts::RawKind::Null;
    snapshot.observation.extensions.push_back(std::move(extension));
    REQUIRE(usage_observation::Validate(snapshot.observation, snapshot.values));
    usage_wire::LexicalUsage::MarkIncomplete(snapshot);
    CHECK(snapshot.material_error == "usage.material.byte_limit");
    CHECK(snapshot.observation.raw_fields.size() == 46); CHECK(snapshot.observation.extensions.size() == 1);
    const auto event = usage_wire::Nonterminal(snapshot);
    CHECK_FALSE(event.usage_observation); CHECK(event.usage_reported);
    CHECK(event.usage.input_tokens == 17); CHECK(event.usage.output_tokens == 9);
    CHECK(event.usage.cache_read_tokens == 11); CHECK(event.usage.cache_creation_tokens == 13);
    CHECK(event.usage.output_reasoning_tokens == 2);
}

namespace {
void RequireOriginalNumeric(const std::vector<StreamEvent>& events, const std::string& token,
                            ::lubancore::usage::v1::RawKind kind, bool incomplete) {
    namespace facts = ::lubancore::usage::v1;
    MessageAssembler assembler;
    bool found = false, error = false;
    for (const auto& event : events) {
        assembler.Feed(event);
        error = error || std::holds_alternative<StreamError>(event);
        CHECK_FALSE(std::holds_alternative<MessageDone>(event));
        if (const auto* snapshot = std::get_if<UsageSnapshot>(&event)) {
            REQUIRE(snapshot->usage_observation);
            for (const auto& raw : snapshot->usage_observation->raw_fields) {
                if (raw.summary == token) {
                    found = true; CHECK(raw.kind == kind); CHECK_FALSE(raw.integer);
                    CHECK(raw.fingerprint == lubancode::platform::Sha256Hex(token));
                }
            }
            const auto encoded = usage_json::Encode(*snapshot->usage_observation, snapshot->usage);
            REQUIRE(encoded);
            const auto restored = usage_json::Decode(*encoded, snapshot->usage); REQUIRE(restored);
            if (incomplete) {
                bool marked = false;
                for (const auto& anomaly : restored->anomalies) if (anomaly.code == facts::AnomalyCode::ParseIncomplete) {
                    marked = true; CHECK(anomaly.affected_field_count == facts::kFieldCount);
                }
                CHECK(marked);
            }
        }
    }
    CHECK(found); CHECK(error == incomplete);
    CHECK(assembler.usage_seen()); CHECK(assembler.usage().output_tokens == 9);
    CHECK(assembler.stop_reason().empty()); CHECK(assembler.BuildMessage().content.empty());
    Usage total;
    facts::Coverage coverage;
    lubancode::api::usage_aggregation::Add(total, coverage, assembler.usage(), assembler.usage_observation() ? &*assembler.usage_observation() : nullptr);
    CHECK_FALSE(lubancode::api::usage_aggregation::Exact(coverage, facts::Field::Input));
    if (incomplete) for (std::size_t i = 0; i < facts::kFieldCount; ++i)
        CHECK_FALSE(lubancode::api::usage_aggregation::Exact(coverage, static_cast<facts::Field>(i)));
}
}

TEST_CASE("Five-field lexical facts: four real parsers retain huge integers and original floating lexemes") {
    namespace facts = ::lubancore::usage::v1;
    for (const std::string token : {"184467440737095516160", "1.000000000000000000000e+00"}) {
        const auto kind = token.find('.') == std::string::npos ? facts::RawKind::UnsignedInteger : facts::RawKind::FloatingPoint;
        chat::EventParser chat;
        RequireOriginalNumeric(chat.Consume(SseFrame{"", "{\"usage\":{\"prompt_tokens\":" + token + ",\"completion_tokens\":9},\"choices\":[]}"}), token, kind, false);
        gemini::EventParser gemini;
        RequireOriginalNumeric(gemini.Consume(SseFrame{"", "{\"usageMetadata\":{\"promptTokenCount\":" + token + ",\"candidatesTokenCount\":9}}"}), token, kind, false);
        responses::EventParser responses;
        RequireOriginalNumeric(responses.Consume(SseFrame{"response.created", "{\"type\":\"response.created\",\"response\":{\"usage\":{\"input_tokens\":" + token + ",\"output_tokens\":9}}}"}), token, kind, false);
        anthropic::EventParser anthropic;
        RequireOriginalNumeric(anthropic.Consume(SseFrame{"message_start", "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":" + token + ",\"output_tokens\":9}}}"}), token, kind, false);
    }
}

TEST_CASE("Five-field lexical facts: four real parsers recover numeric facts when DOM rejects an overflowing float") {
    namespace facts = ::lubancore::usage::v1;
    chat::EventParser chat;
    RequireOriginalNumeric(chat.Consume(SseFrame{"", R"({"usage":{"prompt_tokens":1e999,"completion_tokens":9},"choices":[]})"}), "1e999", facts::RawKind::FloatingPoint, true);
    CHECK(chat.Finish().empty());
    gemini::EventParser gemini;
    RequireOriginalNumeric(gemini.Consume(SseFrame{"", R"({"usageMetadata":{"promptTokenCount":1e999,"candidatesTokenCount":9}})"}), "1e999", facts::RawKind::FloatingPoint, true);
    CHECK(gemini.Finish().empty());
    responses::EventParser responses;
    RequireOriginalNumeric(responses.Consume(SseFrame{"response.completed", R"({"type":"response.completed","response":{"usage":{"input_tokens":1e999,"output_tokens":9}}})"}), "1e999", facts::RawKind::FloatingPoint, true);
    anthropic::EventParser anthropic;
    RequireOriginalNumeric(anthropic.Consume(SseFrame{"message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":1e999,"output_tokens":9}}})"}), "1e999", facts::RawKind::FloatingPoint, true);
    CHECK(anthropic.Finish().empty());
}

TEST_CASE("Five-field lexical facts: four real parsers keep complete scalars before malformed body bytes") {
    namespace facts = ::lubancore::usage::v1;
    const std::string token = "184467440737095516160";
    chat::EventParser chat;
    RequireOriginalNumeric(chat.Consume(SseFrame{"", "{\"usage\":{\"prompt_tokens\":" + token + ",\"completion_tokens\":9},\"choices\":[invalid]}"}), token, facts::RawKind::UnsignedInteger, true);
    gemini::EventParser gemini;
    RequireOriginalNumeric(gemini.Consume(SseFrame{"", "{\"usageMetadata\":{\"promptTokenCount\":" + token + ",\"candidatesTokenCount\":9},\"candidates\":[invalid]}"}), token, facts::RawKind::UnsignedInteger, true);
    responses::EventParser responses;
    RequireOriginalNumeric(responses.Consume(SseFrame{"response.completed", "{\"response\":{\"usage\":{\"input_tokens\":" + token + ",\"output_tokens\":9},\"output\":[invalid]}}"}), token, facts::RawKind::UnsignedInteger, true);
    anthropic::EventParser anthropic;
    RequireOriginalNumeric(anthropic.Consume(SseFrame{"message_start", "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":" + token + ",\"output_tokens\":9},\"content\":[invalid]}}"}), token, facts::RawKind::UnsignedInteger, true);
}

TEST_CASE("Five-field lexical facts: bounded capture excludes body decoys and follows last canonical presence") {
    namespace facts = ::lubancore::usage::v1;
    chat::EventParser parser;
    const auto events = parser.Consume(SseFrame{"", R"({"body":{"usage":{"prompt_tokens":999}},"usage":{"prompt_tokens":11,"completion_tokens":9},"usage":{"prompt_tokens":null,"completion_tokens":7},"choices":[]})"});
    MessageAssembler assembler;
    for (const auto& event : events) assembler.Feed(event);
    REQUIRE(assembler.usage_observation());
    CHECK(assembler.usage().input_tokens == 0); CHECK(assembler.usage().output_tokens == 7);
    for (const auto& raw : assembler.usage_observation()->raw_fields)
        CHECK_FALSE((raw.integer == 11 || raw.integer == 999 || raw.integer == 9));
    chat::EventParser escaped;
    MessageAssembler decoded;
    for (const auto& event : escaped.Consume(SseFrame{"", R"({"us\u0061ge":{"prompt_\u0074okens":11,"completion_tokens":9},"choices":[]})"})) decoded.Feed(event);
    CHECK(decoded.usage().input_tokens == 11); CHECK(decoded.usage().output_tokens == 9);
    chat::EventParser enormous;
    const std::string number(facts::kMaxMaterialBytes + 1, '9');
    MessageAssembler bounded;
    for (const auto& event : enormous.Consume(SseFrame{"", "{\"usage\":{\"prompt_tokens\":" + number + ",\"completion_tokens\":9}}"})) bounded.Feed(event);
    REQUIRE(bounded.usage_observation());
    bool captured = false;
    for (const auto& raw : bounded.usage_observation()->raw_fields) if (raw.path == "usage.prompt_tokens") {
        captured = true; CHECK(raw.summary.size() == facts::kMaxSummaryBytes);
        CHECK(raw.fingerprint.empty()); CHECK_FALSE(raw.integer);
    }
    CHECK(captured); CHECK(bounded.usage().output_tokens == 9); CHECK(bounded.stop_reason().empty());
}

TEST_CASE("Provider identity: identity-only frames preserve numeric facts and do not complete") {
    MessageAssembler assembler;
    assembler.Feed(UsageSnapshot{Usage{17, 9, 3, 5, 7}, true});
    assembler.Feed(ProviderResponseIdentity{"real-provider-id"});
    REQUIRE(assembler.provider_response_id());
    CHECK(*assembler.provider_response_id() == "real-provider-id");
    CHECK(assembler.usage().input_tokens == 17);
    CHECK(assembler.usage().cache_creation_tokens == 5);
    CHECK(assembler.usage_seen()); CHECK(assembler.stop_reason().empty());
    MessageDone done;
    done.provider_response_id = "terminal-provider-id";
    assembler.Feed(done);
    CHECK(*assembler.provider_response_id() == "terminal-provider-id");
    CHECK(assembler.usage().input_tokens == 17);
    MessageAssembler absent;
    absent.Feed(ProviderResponseIdentity{"id-without-usage"});
    REQUIRE(absent.provider_response_id());
    CHECK_FALSE(absent.usage_seen()); CHECK_FALSE(absent.usage_observation());
    CHECK(absent.usage().input_tokens == 0); CHECK(absent.stop_reason().empty());
}

TEST_CASE("Five-field identity conflict: ID-only streams without prior usage never fabricate a snapshot") {
    const auto absent = [](const std::vector<StreamEvent>& events) {
        bool error = false;
        for (const auto& event : events) {
            error = error || std::holds_alternative<StreamError>(event);
            CHECK_FALSE(std::holds_alternative<UsageSnapshot>(event));
            CHECK_FALSE(std::holds_alternative<MessageDone>(event));
        }
        CHECK(error);
    };
    chat::EventParser chat;
    chat.Consume(SseFrame{"", R"({"id":"first-id","choices":[]})"});
    absent(chat.Consume(SseFrame{"", R"({"id":"second-id","choices":[]})"}));
    gemini::EventParser gemini;
    gemini.Consume(SseFrame{"", R"({"responseId":"first-id"})"});
    absent(gemini.Consume(SseFrame{"", R"({"responseId":"second-id"})"}));
    responses::EventParser responses;
    responses.Consume(SseFrame{"response.created", R"({"type":"response.created","response":{"id":"first-id"}})"});
    absent(responses.Consume(SseFrame{"response.completed", R"({"type":"response.completed","response":{"id":"second-id"}})"}));
}

TEST_CASE("纯 text:多段 TextDelta 拼成一个 TextBlock") {
    MessageAssembler assembler;
    assembler.Feed(MessageStart{"msg_1", "some-model"});
    assembler.Feed(TextDelta{"你好,"});
    assembler.Feed(TextDelta{"世界"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"end_turn", Usage{10, 5}});

    CHECK_FALSE(assembler.has_parse_error());
    CHECK(assembler.stop_reason() == "end_turn");
    CHECK(assembler.usage().input_tokens == 10);
    CHECK(assembler.usage().output_tokens == 5);

    const Message message = assembler.BuildMessage();
    REQUIRE(message.role == Role::Assistant);
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<TextBlock>(message.content[0]));
    CHECK(std::get<TextBlock>(message.content[0]).text == "你好,世界");
}

TEST_CASE("单 tool_use:input JSON 一次性喂完,攒出 ToolUseBlock") {
    MessageAssembler assembler;
    assembler.Feed(MessageStart{"msg_2", "some-model"});
    assembler.Feed(ToolUseStart{0, "toolu_01", "get_weather"});
    assembler.Feed(ToolUseInputDelta{0, R"({"city":"杭州"})"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK_FALSE(assembler.has_parse_error());
    CHECK(assembler.stop_reason() == "tool_use");

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[0]));
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.id == "toolu_01");
    CHECK(block.name == "get_weather");
    CHECK(block.input.at("city").get<std::string>() == "杭州");
}

TEST_CASE("text + tool_use 混合:先文本后工具调用,两个块顺序不错") {
    MessageAssembler assembler;
    assembler.Feed(MessageStart{"msg_3", "some-model"});
    assembler.Feed(TextDelta{"我来查一下天气。"});
    // 没有显式的 ContentBlockDone 也没关系:ToolUseStart 自己会把前一个块收尾。
    assembler.Feed(ToolUseStart{1, "toolu_02", "get_weather"});
    assembler.Feed(ToolUseInputDelta{1, R"({"city":)"});
    assembler.Feed(ToolUseInputDelta{1, R"("北京"})"});
    assembler.Feed(ContentBlockDone{1});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<TextBlock>(message.content[0]));
    CHECK(std::get<TextBlock>(message.content[0]).text == "我来查一下天气。");
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[1]));
    const auto& tool_block = std::get<ToolUseBlock>(message.content[1]);
    CHECK(tool_block.name == "get_weather");
    CHECK(tool_block.input.at("city").get<std::string>() == "北京");
}

TEST_CASE("input JSON 劈成好几段,拼起来还是对的") {
    MessageAssembler assembler;
    assembler.Feed(ToolUseStart{0, "toolu_03", "read_file"});
    assembler.Feed(ToolUseInputDelta{0, R"({"pa)"});
    assembler.Feed(ToolUseInputDelta{0, R"(th":"a)"});
    assembler.Feed(ToolUseInputDelta{0, R"(.txt","limit":)"});
    assembler.Feed(ToolUseInputDelta{0, R"(10})"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK_FALSE(assembler.has_parse_error());
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.input.at("path").get<std::string>() == "a.txt");
    CHECK(block.input.at("limit").get<int>() == 10);
}

TEST_CASE("input 非法 JSON:报错但不崩,依旧攒出一个可用的 Message") {
    MessageAssembler assembler;
    assembler.Feed(ToolUseStart{0, "toolu_04", "read_file"});
    assembler.Feed(ToolUseInputDelta{0, "{not valid json"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK(assembler.has_parse_error());
    CHECK_FALSE(assembler.parse_error().empty());

    // 就算解析失败,BuildMessage() 也不能崩、也不能丢块——input 退化成空对象。
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[0]));
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.id == "toolu_04");
    CHECK(block.name == "read_file");
    CHECK(block.input.is_object());
    CHECK(block.input.empty());
}

TEST_CASE("空的 tool_use(没有任何 ToolUseInputDelta)攒出空对象 input,不报错") {
    MessageAssembler assembler;
    assembler.Feed(ToolUseStart{0, "toolu_05", "no_args_tool"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK_FALSE(assembler.has_parse_error());
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.input.is_object());
    CHECK(block.input.empty());
}

TEST_CASE("thinking + text:ThinkingDelta 攒成 ThinkingBlock(signature 也拼上)") {
    MessageAssembler assembler;
    assembler.Feed(ThinkingDelta{"分析", ""});
    assembler.Feed(ThinkingDelta{"一下", ""});
    assembler.Feed(ThinkingDelta{"", "sig_abc"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(TextDelta{"答案是 42"});
    assembler.Feed(ContentBlockDone{1});
    assembler.Feed(MessageDone{"end_turn", Usage{}});

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<ThinkingBlock>(message.content[0]));
    const auto& thinking = std::get<ThinkingBlock>(message.content[0]);
    CHECK(thinking.text == "分析一下");
    CHECK(thinking.signature == "sig_abc");
    REQUIRE(std::holds_alternative<TextBlock>(message.content[1]));
    CHECK(std::get<TextBlock>(message.content[1]).text == "答案是 42");
}

TEST_CASE("chat wire 过渡:thinking 后接 text,没有 ContentBlockDone 也能自动收尾") {
    MessageAssembler assembler;
    assembler.Feed(ThinkingDelta{"先想想", ""});
    assembler.Feed(TextDelta{"再回答"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"end_turn", Usage{}});

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<ThinkingBlock>(message.content[0]));
    CHECK(std::get<ThinkingBlock>(message.content[0]).text == "先想想");
    REQUIRE(std::holds_alternative<TextBlock>(message.content[1]));
    CHECK(std::get<TextBlock>(message.content[1]).text == "再回答");
}

TEST_CASE("SanitizeMessage:合法内容原样不动,坏字节按 U+FFFD 清洗") {
    // 合法 UTF-8:一字不动。
    Message clean;
    clean.role = Role::User;
    clean.content.push_back(TextBlock{"你好,世界"});
    clean.content.push_back(ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.txt"}}});
    clean.content.push_back(ToolResultBlock{"call_1", "工具输出:正常内容", false});
    clean.content.push_back(ThinkingBlock{"思考一下", "sig_01"});
    Message clean_copy = clean;
    SanitizeMessage(clean);
    // 合法内容清洗后必须一字不动。
    REQUIRE(clean.content.size() == clean_copy.content.size());
    for (std::size_t i = 0; i < clean.content.size(); ++i) {
        const auto& a = clean.content[i];
        const auto& b = clean_copy.content[i];
        REQUIRE(a.index() == b.index());
        std::visit(
            [&](const auto& av) {
                using T = std::decay_t<decltype(av)>;
                const auto& bv = std::get<T>(b);
                if constexpr (std::is_same_v<T, TextBlock>) {
                    CHECK(av.text == bv.text);
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    CHECK(av.id == bv.id);
                    CHECK(av.name == bv.name);
                    CHECK(av.input == bv.input);
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    CHECK(av.tool_use_id == bv.tool_use_id);
                    CHECK(av.content == bv.content);
                    CHECK(av.is_error == bv.is_error);
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    CHECK(av.text == bv.text);
                    CHECK(av.signature == bv.signature);
                }
            },
            a);
    }

    // 非法 UTF-8(夹着 0xE4 开头的残序列):清洗后必须是合法 UTF-8,
    // 且合法片段保留、坏字节变成 U+FFFD。
    const std::string bad = "前\xE4\xB8后";  // \xE4\xB8 是"中"的前两字节,缺第三字节
    REQUIRE_FALSE(lubancode::platform::IsValidUtf8(bad));
    Message dirty;
    dirty.role = Role::User;
    dirty.content.push_back(TextBlock{bad});
    dirty.content.push_back(ToolUseBlock{"call_2", "search", nlohmann::json{{"path", bad}, {"pattern", "ok"}}});
    dirty.content.push_back(ToolResultBlock{"call_2", bad, false});
    dirty.content.push_back(ThinkingBlock{bad, bad});

    SanitizeMessage(dirty);
    for (const auto& block : dirty.content) {
        std::visit(
            [](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                    CHECK(b.text.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD
                    CHECK(b.text.find("后") != std::string::npos);            // 合法片段保留
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(
                        b.input.at("path").template get<std::string>()));
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.content));
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                    CHECK(lubancode::platform::IsValidUtf8(b.signature));
                }
            },
            block);
    }
}

TEST_CASE("SanitizeRequest:所有 wire 字符串出口一并清洗") {
    const std::string bad = "前\xE4\xB8后";
    REQUIRE_FALSE(lubancode::platform::IsValidUtf8(bad));

    Request request;
    request.model = bad;
    request.system = bad;
    request.reasoning_effort = bad;
    request.messages.push_back(Message{
        Role::User,
        {TextBlock{bad}, ToolUseBlock{bad, bad, nlohmann::json{{"path", bad}}},
         ToolResultBlock{bad, bad, false}, ThinkingBlock{bad, bad}}});
    request.tools.push_back(ToolDefinition{bad, bad, nlohmann::json{{"type", "object"}, {"title", bad}}});
    request.extra_body = nlohmann::json{{"vendor", nlohmann::json{{"note", bad}}}};

    SanitizeRequest(request);

    CHECK(lubancode::platform::IsValidUtf8(request.model));
    CHECK(lubancode::platform::IsValidUtf8(request.system));
    CHECK(lubancode::platform::IsValidUtf8(request.reasoning_effort));
    for (const auto& block : request.messages[0].content) {
        std::visit(
            [](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.id));
                    CHECK(lubancode::platform::IsValidUtf8(b.name));
                    CHECK(lubancode::platform::IsValidUtf8(
                        b.input.at("path").template get<std::string>()));
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.tool_use_id));
                    CHECK(lubancode::platform::IsValidUtf8(b.content));
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                    CHECK(lubancode::platform::IsValidUtf8(b.signature));
                }
            },
            block);
    }
    CHECK(lubancode::platform::IsValidUtf8(request.tools[0].name));
    CHECK(lubancode::platform::IsValidUtf8(request.tools[0].description));
    CHECK(lubancode::platform::IsValidUtf8(request.tools[0].input_schema.at("title").get<std::string>()));
    CHECK(lubancode::platform::IsValidUtf8(request.extra_body["vendor"]["note"].get<std::string>()));
}

// ---------------------------------------------------------------------------
// 缓存用量按 Wire 归一单 C2:MessageDone 的读/写明报位与异常位经 assembler
// 原样落账——中途不丢,"只报写入"不许被吞成"读取已知零"。
// ---------------------------------------------------------------------------

TEST_CASE("C2 旗标: MessageDone 的读/写明报位与异常位各自独立落账") {
    {
        MessageAssembler assembler;
        MessageDone done;
        done.stop_reason = "end_turn";
        done.usage = Usage{100, 5, 0, 300};  // 只报写入:R 数字 0、W=300
        done.usage_reported = true;
        done.cache_read_reported = false;
        done.cache_creation_reported = true;
        assembler.Feed(done);
        CHECK(assembler.usage_seen());
        CHECK_FALSE(assembler.cache_read_seen());  // 读取未知,不冒充已知零
        CHECK(assembler.cache_creation_seen());
        CHECK(assembler.usage().cache_creation_tokens == 300);
    }
    {
        MessageAssembler assembler;
        MessageDone done;
        done.usage_reported = true;
        done.cache_read_reported = true;  // 明报 R=0
        assembler.Feed(done);
        CHECK(assembler.cache_read_seen());
        CHECK(assembler.usage().cache_read_tokens == 0);  // 数字是零,旗标是"已报"
    }
    {
        MessageAssembler assembler;
        MessageDone done;
        done.usage_reported = true;
        done.usage_anomaly = "cached_tokens(1200) > input_tokens(1000)";
        assembler.Feed(done);
        CHECK_FALSE(assembler.usage_anomaly().empty());  // 异常位不丢
        CHECK(assembler.usage_seen());
    }
    // 缺省(老路径/未接线):三位皆 false——与"明报零"分得开。
    MessageAssembler bare;
    bare.Feed(MessageDone{"end_turn", Usage{10, 5}});
    CHECK(bare.usage_seen() == false);
    CHECK_FALSE(bare.cache_read_seen());
    CHECK_FALSE(bare.cache_creation_seen());
}


TEST_CASE("Usage snapshot: facts neither finalize content nor declare completion") {
    std::cout << "[usage-snapshot-assembler] nonterminal\n";
    MessageAssembler assembler;
    assembler.Feed(TextDelta{"unfinished"});
    assembler.Feed(UsageSnapshot{Usage{17, 9}, true});
    CHECK(assembler.stop_reason().empty());
    CHECK(assembler.BuildMessage().content.empty());
    CHECK(assembler.usage_seen());
    CHECK(assembler.usage().input_tokens == 17);
    assembler.Feed(TextDelta{" continued"});
    assembler.FinalizeOpenBlock();
    REQUIRE(assembler.BuildMessage().content.size() == 1);
    CHECK(std::get<TextBlock>(assembler.BuildMessage().content.front()).text == "unfinished continued");
    CHECK(assembler.stop_reason().empty());
}

TEST_CASE("Usage snapshot: empty terminal preserves facts and explicit zero overwrites") {
    std::cout << "[usage-snapshot-assembler] terminal\n";
    MessageAssembler assembler;
    assembler.Feed(UsageSnapshot{Usage{17, 9}, true});
    assembler.Feed(MessageDone{"end_turn", Usage{}});
    CHECK(assembler.stop_reason() == "end_turn");
    CHECK(assembler.usage().input_tokens == 17);
    CHECK(assembler.usage().output_tokens == 9);
    CHECK(assembler.usage_seen());
    MessageDone zero; zero.stop_reason = "end_turn"; zero.usage_reported = true;
    assembler.Feed(zero);
    CHECK(assembler.usage().input_tokens == 0);
    CHECK(assembler.usage().output_tokens == 0);
    CHECK(assembler.usage_seen());
    assembler.Feed(UsageSnapshot{Usage{17, 9}, true});
    assembler.Feed(MessageDone{"end_turn", Usage{3, 4}});
    CHECK(assembler.usage().input_tokens == 3);
    CHECK(assembler.usage().output_tokens == 4);
    CHECK_FALSE(assembler.usage_seen());
}

TEST_CASE("Usage snapshot: whole snapshots replace flags and preserve legacy terminals") {
    std::cout << "[usage-snapshot-assembler] flags\n";
    MessageAssembler assembler;
    assembler.Feed(UsageSnapshot{Usage{5, 2, 7, 11, 13}, true, true, true, "original anomaly"});
    CHECK(assembler.cache_read_seen()); CHECK(assembler.cache_creation_seen());
    CHECK(assembler.usage().cache_read_tokens == 7);
    CHECK(assembler.usage().cache_creation_tokens == 11);
    CHECK(assembler.usage().output_reasoning_tokens == 13);
    CHECK(assembler.usage_anomaly() == "original anomaly");
    assembler.Feed(UsageSnapshot{Usage{}, true});
    CHECK(assembler.usage_seen()); CHECK_FALSE(assembler.cache_read_seen());
    CHECK_FALSE(assembler.cache_creation_seen()); CHECK(assembler.usage_anomaly().empty());
    CHECK(assembler.usage().input_tokens == 0); CHECK(assembler.usage().output_reasoning_tokens == 0);
    MessageAssembler legacy;
    legacy.Feed(MessageDone{"end_turn", Usage{3, 4}});
    CHECK(legacy.usage().input_tokens == 3); CHECK_FALSE(legacy.usage_seen());
    legacy.Feed(MessageDone{"end_turn", Usage{}});
    CHECK(legacy.usage().input_tokens == 0); CHECK_FALSE(legacy.usage_seen());
}


namespace {
void RequireLateAccounting(const std::vector<StreamEvent>& events, MessageAssembler& assembler,
                           const char* id) {
    int snapshots = 0;
    for (const auto& event : events) {
        CHECK((std::holds_alternative<UsageSnapshot>(event) ||
               std::holds_alternative<ProviderResponseIdentity>(event)));
        if (const auto* snapshot = std::get_if<UsageSnapshot>(&event)) {
            ++snapshots; REQUIRE(snapshot->usage_observation);
            CHECK(snapshot->usage.input_tokens == 17); CHECK(snapshot->usage.output_tokens == 9);
            CHECK(snapshot->provider_response_id == id);
        }
        assembler.Feed(event);
    }
    CHECK(snapshots == 1); CHECK(assembler.stop_reason().empty());
    CHECK(assembler.usage().input_tokens == 17); CHECK(assembler.usage().output_tokens == 9);
}
void RequireFailure(const std::vector<StreamEvent>& events, MessageAssembler& assembler) {
    bool failed = false;
    for (const auto& event : events) {
        failed = failed || std::holds_alternative<StreamError>(event);
        CHECK_FALSE(std::holds_alternative<MessageDone>(event));
        assembler.Feed(event);
    }
    CHECK(failed); CHECK(assembler.stop_reason().empty());
}
}

TEST_CASE("Five-field failure fence: Chat late accounting cannot revive a failed stream") {
    chat::EventParser parser; MessageAssembler assembler;
    for (const auto& event : parser.Consume(SseFrame{"", R"({"id":"chat-real","choices":[{"delta":{"content":"before"}}]})"}))
        assembler.Feed(event);
    RequireFailure(parser.Consume(SseFrame{"", R"({"error":{"message":"failed","code":"bad_request"},"usage":{"prompt_tokens":11,"completion_tokens":7}})"}), assembler);
    RequireLateAccounting(parser.Consume(SseFrame{"", R"({"id":"chat-real","choices":[{"delta":{"content":"must not appear"},"finish_reason":"stop"}],"usage":{"prompt_tokens":17,"completion_tokens":9}})"}), assembler, "chat-real");
    CHECK(parser.Finish().empty()); CHECK(parser.Consume(SseFrame{"", "[DONE]"}).empty());
    assembler.FinalizeOpenBlock();
    REQUIRE(assembler.BuildMessage().content.size() == 1);
    CHECK(std::get<TextBlock>(assembler.BuildMessage().content[0]).text == "before");
}

TEST_CASE("Five-field failure fence: Gemini late accounting cannot flush a success terminal") {
    gemini::EventParser parser; MessageAssembler assembler;
    for (const auto& event : parser.Consume(SseFrame{"", R"({"responseId":"gemini-real","candidates":[{"content":{"parts":[{"text":"before"}]}}]})"}))
        assembler.Feed(event);
    RequireFailure(parser.Consume(SseFrame{"", R"({"error":{"message":"failed","code":400},"usageMetadata":{"promptTokenCount":11,"candidatesTokenCount":7}})"}), assembler);
    RequireLateAccounting(parser.Consume(SseFrame{"", R"({"responseId":"gemini-real","candidates":[{"content":{"parts":[{"text":"must not appear"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":17,"candidatesTokenCount":9}})"}), assembler, "gemini-real");
    CHECK(parser.Finish().empty());
    assembler.FinalizeOpenBlock();
    REQUIRE(assembler.BuildMessage().content.size() == 1);
    CHECK(std::get<TextBlock>(assembler.BuildMessage().content[0]).text == "before");
}

TEST_CASE("Five-field failure fence: Anthropic late cumulative usage survives without completion") {
    anthropic::EventParser parser; MessageAssembler assembler;
    for (const auto& event : parser.Consume(SseFrame{"message_start", R"({"type":"message_start","message":{"id":"anthropic-real","model":"fixture","usage":{"input_tokens":11,"output_tokens":7}}})"}))
        assembler.Feed(event);
    RequireFailure(parser.Consume(SseFrame{"error", R"({"type":"error","error":{"type":"bad_request","message":"failed"}})"}), assembler);
    RequireLateAccounting(parser.Consume(SseFrame{"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"input_tokens":17,"output_tokens":9}})"}), assembler, "anthropic-real");
    CHECK(parser.Consume(SseFrame{"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"must not appear"}})"}).empty());
    CHECK(parser.Finish().empty()); CHECK(assembler.BuildMessage().content.empty());
}

TEST_CASE("Five-field failure fence: Responses late completed frame cannot override an error") {
    responses::EventParser parser; MessageAssembler assembler;
    for (const auto& event : parser.Consume(SseFrame{"response.created", R"({"type":"response.created","response":{"id":"responses-real","status":"in_progress","usage":{"input_tokens":11,"output_tokens":7}}})"}))
        assembler.Feed(event);
    RequireFailure(parser.Consume(SseFrame{"error", R"({"type":"error","message":"failed","code":"bad_request"})"}), assembler);
    RequireLateAccounting(parser.Consume(SseFrame{"response.completed", R"({"type":"response.completed","response":{"id":"responses-real","status":"completed","output":[],"usage":{"input_tokens":17,"output_tokens":9}}})"}), assembler, "responses-real");
    CHECK(parser.Consume(SseFrame{"response.output_text.delta", R"({"type":"response.output_text.delta","delta":"must not appear"})"}).empty());
    CHECK(assembler.BuildMessage().content.empty());
}


namespace {
void RequireScopedIdentityConflict(const std::vector<StreamEvent>& events, std::int64_t input, std::int64_t output,
                                   bool expected_error = true) {
    namespace facts = lubancore::usage::v1;
    const UsageSnapshot* snapshot = nullptr; bool error = false;
    for (const auto& event : events) {
        if (const auto* value = std::get_if<UsageSnapshot>(&event)) snapshot = value;
        error = error || std::holds_alternative<StreamError>(event);
        CHECK_FALSE(std::holds_alternative<MessageDone>(event)); CHECK_FALSE(std::holds_alternative<TextDelta>(event));
    }
    REQUIRE(snapshot); REQUIRE(snapshot->usage_observation); CHECK(error == expected_error);
    CHECK(snapshot->provider_response_id == "second-id");
    CHECK(snapshot->usage.input_tokens == input); CHECK(snapshot->usage.output_tokens == output);
    const auto& material = *snapshot->usage_observation;
    bool conflict = false;
    for (const auto& anomaly : material.anomalies) {
        if (anomaly.code != facts::AnomalyCode::ResponseIdentityConflict) continue;
        conflict = true; CHECK(anomaly.affected_field_count == facts::kFieldCount); REQUIRE(anomaly.raw_field_count == 2);
        CHECK(material.raw_fields[anomaly.raw_fields[0]].fingerprint != material.raw_fields[anomaly.raw_fields[1]].fingerprint);
    }
    CHECK(conflict);
    Usage total; facts::Coverage coverage;
    usage_aggregation::Add(total, coverage, snapshot->usage, &material);
    CHECK(total.input_tokens == input); CHECK(coverage.samples == 1);
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        CHECK(coverage.fields[i].anomalous == 1);
        CHECK_FALSE(usage_aggregation::Exact(coverage, static_cast<facts::Field>(i)));
    }
    auto encoded = usage_json::Encode(material, snapshot->usage); REQUIRE(encoded);
    CHECK(encoded->at("version") == 2);
    auto decoded = usage_json::Decode(*encoded, snapshot->usage); REQUIRE(decoded);
    auto invalid_scope = *encoded;
    for (auto& anomaly : invalid_scope["anomalies"])
        if (anomaly["code"] == static_cast<unsigned>(facts::AnomalyCode::ResponseIdentityConflict))
            anomaly["affected_fields"] = nlohmann::json::array({0, 0});
    const auto duplicate = usage_json::Decode(invalid_scope, snapshot->usage);
    REQUIRE_FALSE(duplicate); CHECK(duplicate.error() == "usage.json.anomaly_scope");
    encoded->at("version") = 1;
    const auto fenced = usage_json::Decode(*encoded, snapshot->usage);
    REQUIRE_FALSE(fenced); CHECK(fenced.error() == "usage.json.anomaly_scope_version");
}
}

TEST_CASE("Five-field identity conflict: Chat keeps changed-ID facts and marks cached facts on identity-only changes") {
    for (const bool usage : {false, true}) {
        chat::EventParser parser;
        parser.Consume(SseFrame{"", R"({"id":"first-id","usage":{"prompt_tokens":11,"completion_tokens":7},"choices":[]})"});
        nlohmann::json changed{{"id", "second-id"}, {"choices", nlohmann::json::array()}};
        if (usage) changed["usage"] = {{"prompt_tokens", 17}, {"completion_tokens", 9}};
        RequireScopedIdentityConflict(parser.Consume(SseFrame{"", changed.dump()}), usage ? 17 : 11, usage ? 9 : 7);
        RequireScopedIdentityConflict(parser.Consume(SseFrame{"", R"({"usage":{"prompt_tokens":19,"completion_tokens":10},"choices":[]})"}), 19, 10, false);
        CHECK(parser.Finish().empty());
    }
}

TEST_CASE("input precision keeps independent output faults and rejects negative numbers without trusting anomaly labels") {
    auto source = usage_wire::Anthropic(nlohmann::json{{"input_tokens", -3}, {"output_tokens", 5},
        {"cache_read_input_tokens", 11}, {"cache_creation_input_tokens", 13}});
    REQUIRE(source.has_value());
    source->observation.anomalies.clear(); // Host omitted the negative diagnostic; numbers still prove it.
    const auto negative = usage_wire::Numbers(*source);
    Usage total;
    ::lubancore::usage::v1::Coverage coverage;
    usage_aggregation::Add(total, coverage, negative, &source->observation);
    CHECK(total.input_tokens == -3);
    CHECK(coverage.fields[0].anomalous == 1);
    CHECK_FALSE(usage_aggregation::Exact(coverage, ::lubancore::usage::v1::Field::Input));
    CHECK(usage_aggregation::Exact(coverage, ::lubancore::usage::v1::Field::CacheRead));
    CHECK_FALSE(usage_aggregation::ExactTotalInput(negative, &source->observation).has_value());
    CHECK_FALSE(usage_aggregation::ExactTotalInput(negative, nullptr).has_value());

    auto output_fault = usage_wire::Anthropic(nlohmann::json{{"input_tokens", 100}, {"output_tokens", -5},
        {"cache_read_input_tokens", 11}, {"cache_creation_input_tokens", 13}});
    REQUIRE(output_fault.has_value());
    const auto independent = usage_aggregation::ExactTotalInput(usage_wire::Numbers(*output_fault), &output_fault->observation);
    REQUIRE(independent.has_value());
    CHECK(*independent == 124);
}

TEST_CASE("Five-field identity conflict: Gemini changed-ID accounting cannot remain exact") {
    for (const bool usage : {false, true}) {
        gemini::EventParser parser;
        parser.Consume(SseFrame{"", R"({"responseId":"first-id","usageMetadata":{"promptTokenCount":11,"candidatesTokenCount":7}})"});
        nlohmann::json changed{{"responseId", "second-id"}};
        if (usage) changed["usageMetadata"] = {{"promptTokenCount", 17}, {"candidatesTokenCount", 9}};
        RequireScopedIdentityConflict(parser.Consume(SseFrame{"", changed.dump()}), usage ? 17 : 11, usage ? 9 : 7);
        RequireScopedIdentityConflict(parser.Consume(SseFrame{"", R"({"usageMetadata":{"promptTokenCount":19,"candidatesTokenCount":10}})"}), 19, 10, false);
        CHECK(parser.Finish().empty());
    }
}

TEST_CASE("Five-field identity conflict: Responses retains scope witnesses without fabricating new usage") {
    for (const bool usage : {false, true}) {
        responses::EventParser parser;
        parser.Consume(SseFrame{"response.created", R"({"type":"response.created","response":{"id":"first-id","usage":{"input_tokens":11,"output_tokens":7}}})"});
        nlohmann::json response{{"id", "second-id"}, {"status", "completed"}, {"output", nlohmann::json::array()}};
        if (usage) response["usage"] = {{"input_tokens", 17}, {"output_tokens", 9}};
        nlohmann::json changed{{"type", "response.completed"}, {"response", response}};
        RequireScopedIdentityConflict(parser.Consume(SseFrame{"response.completed", changed.dump()}), usage ? 17 : 11, usage ? 9 : 7);
        RequireScopedIdentityConflict(parser.Consume(SseFrame{"response.completed", R"({"type":"response.completed","response":{"usage":{"input_tokens":19,"output_tokens":10}}})"}), 19, 10, false);
    }
}
