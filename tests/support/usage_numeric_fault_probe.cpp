// A separate test process owns this allocator replacement. SDK/CLI binaries
// and their aggregate test executables retain their normal allocators.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <new>

#include "api/usage_provider_normalizers.hpp"
#include "api/usage_lexical.hpp"
#include "api/chat/events.hpp"
#include "api/responses/events.hpp"
#include "api/anthropic/events.hpp"
#include "api/gemini/events.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "sdk/usage_result.hpp"

namespace {
thread_local bool refuse_next = false;
thread_local unsigned refused = 0;
thread_local std::size_t fail_after = 0;
thread_local std::size_t allocations = 0;
thread_local bool count_allocations = false;
thread_local const char* active_stage = "setup";
thread_local const char* active_route = "none";
thread_local std::size_t active_fault = 0;
}

void* operator new(std::size_t size) {
    if (count_allocations) ++allocations;
    if (fail_after && --fail_after == 0) {
        ++refused;
        throw std::bad_alloc{};
    }
    if (refuse_next) {
        refuse_next = false; ++refused;
        throw std::bad_alloc{};
    }
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

int main(int argc, char** argv) {
    // A real command hook consumes the actual production stdin envelope.
    // Fault injection stays disarmed in this observer mode.
    if (argc == 2) {
        try {
            const std::string mode(argv[1]);
            std::string input;
            std::getline(std::cin, input);
            const auto payload = nlohmann::json::parse(input);
            if (mode.starts_with("post-turn-")) {
                const bool overflow = mode == "post-turn-overflow";
                const bool zero = mode == "post-turn-zero";
                if (!overflow && !zero && mode != "post-turn-normal") return 9;
                if (payload.at("hook_event_name") != "PostTurn" || payload.at("turn_id") != "turn-checked" ||
                    payload.at("last_assistant_message") != "owned last reply" || payload.at("steps") != 2 ||
                    payload.at("actions") != 3 || payload.at("duration_ms") != 1234 || payload.at("cancelled") != false ||
                    payload.at("input_tokens_overflow") != overflow || payload.at("output_tokens_overflow") != overflow) return 10;
                if (overflow) {
                    if (!payload.at("input_tokens").is_null() || !payload.at("output_tokens").is_null()) return 11;
                } else if (!payload.at("input_tokens").is_number_integer() || !payload.at("output_tokens").is_number_integer() ||
                    payload.at("input_tokens") != (zero ? 0 : 160) || payload.at("output_tokens") != (zero ? 0 : 7)) return 12;
                std::puts("{\"systemMessage\":\"actual PostTurn usage observer passed\"}");
                return 0;
            }
            std::array<std::int64_t, 5> expected{};
            if (mode == "post-step-normal") expected = {100,7,50,10,9};
            else if (mode == "post-step-overflow")
                expected = {(std::numeric_limits<std::int64_t>::max)(),7,1,-1,9};
            else if (mode != "post-step-zero") return 2;
            if (payload.at("hook_event_name") != "PostStep" ||
                payload.at("step_id") != "step-checked" || payload.at("turn_id") != "turn-checked" ||
                payload.at("usage_reported") != true || payload.at("usage_numbers") != nlohmann::json(expected) ||
                payload.at("output_tokens") != expected[1]) return 3;
            for (const auto& number : payload.at("usage_numbers"))
                if (!number.is_number_integer()) return 4;
            if (mode == "post-step-overflow") {
                if (!payload.at("input_tokens").is_null() || payload.at("input_tokens_overflow") != true) return 5;
            } else if (!payload.at("input_tokens").is_number_integer() ||
                       payload.at("input_tokens") != (mode == "post-step-normal" ? 160 : 0) ||
                       payload.at("input_tokens_overflow") != false) return 6;
            std::puts("{\"systemMessage\":\"actual PostStep usage observer passed\"}");
            return 0;
        } catch (...) { return 7; }
    }
    if (argc != 1) return 8;
    // Preserve the last completed route even if an allocation crosses an
    // unexpected noexcept boundary and the C++ runtime terminates the process.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate([] {
        std::fprintf(stderr, "unexpected-allocation-terminate:stage=%s:route=%s:index=%zu\n",
            active_stage, active_route, active_fault);
        std::abort(); // A noexcept escape still fails the real process test.
    });
    namespace wire = lubancode::api::usage_wire;
    namespace facts = lubancore::usage::v1;
    using Normalize = std::expected<wire::Snapshot, std::string_view> (*)(
        const nlohmann::json&, const std::vector<facts::RawField>*, wire::NumericObserver, void*);
    const std::array<Normalize,4> normalize{wire::Chat, wire::Responses, wire::Anthropic, wire::Gemini};
    const std::array<const char*,4> names{"chat", "responses", "anthropic", "gemini"};
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
    for (std::size_t provider = 0; provider < normalize.size(); ++provider) {
        active_stage = "material"; active_route = names[provider]; active_fault = 1;
        struct Owner { wire::NumericValues values{}; unsigned calls = 0; } owner;
        const auto prior = refused;
        bool caught = false, returned = false;
        try {
            const auto result = normalize[provider](usages[provider], nullptr,
                [](void* context, const wire::NumericValues& values) noexcept {
                    auto& target = *static_cast<Owner*>(context);
                    target.values = values; ++target.calls;
                    // The callback returns normally. The next actual ordinary
                    // allocation in the production material builder must fail.
                    refuse_next = true;
                }, &owner);
            (void)result; returned = true;
        } catch (const std::bad_alloc&) { caught = true; }
        catch (...) { refuse_next = false; return 10; }
        const bool still_armed = refuse_next;
        refuse_next = false;
        const wire::NumericValues expected{11,7,13,provider == 3 ? 0 : 17,provider == 2 ? 0 : 3};
        if (!caught || returned || still_armed || refused != prior + 1 || owner.calls != 1 || owner.values != expected)
            return 20 + static_cast<int>(provider);
        std::printf("actual-material-allocation-fault:%s\n", names[provider]);
    }
    const std::string long_id(128, 'i');
    const std::array<std::string,5> frames{
        nlohmann::json{{"id",long_id},{"usage",usages[0]}}.dump(),
        nlohmann::json{{"type","response.created"},{"response",{{"id",long_id},{"usage",usages[1]}}}}.dump(),
        nlohmann::json{{"type","message_start"},{"message",{{"id",long_id},{"usage",usages[2]}}}}.dump(),
        nlohmann::json{{"responseId",long_id},{"usageMetadata",usages[3]}}.dump(),
        nlohmann::json{{"id",long_id},{"usage",usages[1]}}.dump()
    };
    const std::array<wire::Dialect,5> dialects{wire::Dialect::Chat, wire::Dialect::Responses,
        wire::Dialect::Anthropic, wire::Dialect::Gemini, wire::Dialect::ResponsesNonStream};
    const std::array<const char*,5> lexical_names{"chat","responses","anthropic","gemini","responses-nonstream"};
    for (std::size_t provider = 0; provider < frames.size(); ++provider) {
        active_stage = "lexical"; active_route = lexical_names[provider]; active_fault = 1;
        struct Owner { wire::NumericValues values{}; unsigned calls = 0; } owner;
        const auto prior = refused;
        bool caught = false, returned = false;
        try {
            const wire::LexicalUsage scan(frames[provider], dialects[provider],
                [](void* context, const wire::NumericValues& values) noexcept {
                    auto& target = *static_cast<Owner*>(context);
                    target.values = values; ++target.calls;
                    refuse_next = true;
                }, &owner);
            (void)scan; returned = true;
        } catch (const std::bad_alloc&) { caught = true; }
        catch (...) { refuse_next = false; return 30; }
        const bool still_armed = refuse_next;
        refuse_next = false;
        const wire::NumericValues expected{11,7,13,provider == 3 ? 0 : 17,provider == 2 ? 0 : 3};
        if (!caught || returned || still_armed || refused != prior + 1 || owner.calls != 1 || owner.values != expected)
            return 40 + static_cast<int>(provider);
        std::printf("actual-lexical-allocation-fault:%s\n", lexical_names[provider]);
    }
    const auto parser_fault = [&](auto parser, std::size_t provider, auto consume) {
        active_stage = "parser"; active_route = lexical_names[provider]; active_fault = 1;
        namespace api = lubancode::api;
        struct Owner { wire::NumericValues values{}; unsigned calls = 0, other = 0; } owner;
        const std::function<void(const api::StreamEvent&)> callback = [&](const api::StreamEvent& event) {
            if (const auto* usage = std::get_if<api::UsageSnapshot>(&event)) {
                owner.values = {usage->usage.input_tokens, usage->usage.output_tokens,
                    usage->usage.cache_read_tokens, usage->usage.cache_creation_tokens,
                    usage->usage.output_reasoning_tokens};
                ++owner.calls;
                if (!usage->usage_reported || usage->usage_observation || usage->provider_response_id ||
                    usage->cache_read_reported || usage->cache_creation_reported) ++owner.other;
            } else ++owner.other;
        };
        api::SseFrame frame; frame.data = frames[provider];
        const auto prior = refused;
        bool caught = false, returned = false;
        try {
            wire::PendingUsageOnUnwind delivery(parser, callback);
            refuse_next = true;
            const auto events = consume(parser, frame);
            for (const auto& event : events) delivery.Emit(event);
            returned = true;
        } catch (const std::bad_alloc&) { caught = true; }
        catch (...) { refuse_next = false; return false; }
        const bool still_armed = refuse_next;
        refuse_next = false;
        const wire::NumericValues expected{11,7,13,provider == 3 ? 0 : 17,provider == 2 ? 0 : 3};
        if (!caught || returned || still_armed || refused != prior + 1 || owner.calls != 1 ||
            owner.other != 0 || owner.values != expected) return false;
        std::printf("actual-parser-allocation-fault:%s\n", lexical_names[provider]);
        return true;
    };
    const auto stream = [](auto& parser, const auto& frame) { return parser.Consume(frame); };
    if (!parser_fault(lubancode::api::chat::EventParser{}, 0, stream)) return 60;
    if (!parser_fault(lubancode::api::responses::EventParser{}, 1, stream)) return 61;
    if (!parser_fault(lubancode::api::anthropic::EventParser{}, 2, stream)) return 62;
    if (!parser_fault(lubancode::api::gemini::EventParser{}, 3, stream)) return 63;
    if (!parser_fault(lubancode::api::responses::EventParser{}, 4,
        [](auto& parser, const auto& frame) { return parser.ExpandNonStream(frame.data); })) return 64;

    // Count a real successful route, then fail every ordinary allocation on
    // that route in a fresh parser. All fixture/callback storage is prepared
    // before arming the allocator. A failed parser must not deliver the body
    // or a success terminal merely because those events had been assembled.
    auto rich_frames = frames;
    {
        auto chat = nlohmann::json::parse(rich_frames[0]);
        chat["model"] = long_id;
        chat["choices"] = nlohmann::json::array({{{"delta", {{"content", long_id}}}}});
        rich_frames[0] = chat.dump();
        auto responses = nlohmann::json::parse(rich_frames[1]);
        responses["response"]["model"] = long_id;
        rich_frames[1] = responses.dump();
        auto anthropic = nlohmann::json::parse(rich_frames[2]);
        anthropic["message"]["model"] = long_id;
        anthropic["message"]["role"] = "assistant";
        anthropic["message"]["content"] = nlohmann::json::array();
        rich_frames[2] = anthropic.dump();
        auto gemini = nlohmann::json::parse(rich_frames[3]);
        gemini["candidates"] = nlohmann::json::array({{{"content", {
            {"role", "model"}, {"parts", nlohmann::json::array({{{"text", long_id}}})}}}}});
        rich_frames[3] = gemini.dump();
        auto nonstream = nlohmann::json::parse(rich_frames[4]);
        nonstream["status"] = "completed";
        nonstream["output"] = nlohmann::json::array({{{"type", "message"},
            {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", long_id}}})}}});
        rich_frames[4] = nonstream.dump();
    }
    const auto parser_sweep = [&](auto prototype, std::size_t provider, auto consume) {
        namespace api = lubancode::api;
        struct Owner {
            wire::NumericValues values{};
            unsigned calls = 0, other = 0;
            bool precise = false;
            bool material_seen = false;
        } owner;
        const std::function<void(const api::StreamEvent&)> callback = [&](const api::StreamEvent& event) {
            if (const auto* usage = std::get_if<api::UsageSnapshot>(&event)) {
                owner.values = {usage->usage.input_tokens, usage->usage.output_tokens,
                    usage->usage.cache_read_tokens, usage->usage.cache_creation_tokens,
                    usage->usage.output_reasoning_tokens};
                ++owner.calls;
                owner.precise = usage->usage_observation.has_value() && usage->provider_response_id == long_id;
                owner.material_seen = usage->usage_observation.has_value() || usage->provider_response_id.has_value() ||
                    usage->cache_read_reported || usage->cache_creation_reported || !usage->usage_anomaly.empty();
                if (!usage->usage_reported) ++owner.other;
            } else ++owner.other;
        };
        api::SseFrame frame; frame.data = rich_frames[provider];
        const wire::NumericValues expected{11,7,13,provider == 3 ? 0 : 17,provider == 2 ? 0 : 3};
        allocations = 0;
        {
            auto parser = prototype;
            wire::PendingUsageOnUnwind delivery(parser, callback);
            count_allocations = true;
            try {
                const auto events = consume(parser, frame);
                count_allocations = false;
                for (const auto& event : events) delivery.Emit(event);
            } catch (...) { count_allocations = false; return false; }
        }
        const auto count = allocations;
        if (!count || count > 10000 || owner.calls != 1 || owner.values != expected || !owner.precise)
            return false;
        for (std::size_t index = 1; index <= count; ++index) {
            active_stage = "parser-sweep"; active_route = lexical_names[provider]; active_fault = index;
            owner = {};
            auto parser = prototype;
            const auto prior = refused;
            bool caught = false, returned = false;
            try {
                wire::PendingUsageOnUnwind delivery(parser, callback);
                fail_after = index;
                const auto events = consume(parser, frame);
                for (const auto& event : events) delivery.Emit(event);
                returned = true;
            } catch (const std::bad_alloc&) { caught = true; }
            catch (...) { fail_after = 0; return false; }
            const bool still_armed = fail_after != 0;
            fail_after = 0;
            if (!caught || returned || still_armed || refused != prior + 1 || owner.calls != 1 ||
                owner.other != 0 || owner.material_seen || owner.values != expected) {
                std::fprintf(stderr, "allocation-sweep-failed:%s:index=%zu:total=%zu:usage=%u:other=%u\n",
                    lexical_names[provider], index, count, owner.calls, owner.other);
                return false;
            }
        }
        std::printf("actual-parser-allocation-sweep:%s:allocations=%zu:faults=%zu\n",
            lexical_names[provider], count, count);
        return true;
    };
    if (!parser_sweep(lubancode::api::chat::EventParser{}, 0, stream)) return 70;
    if (!parser_sweep(lubancode::api::responses::EventParser{}, 1, stream)) return 71;
    if (!parser_sweep(lubancode::api::anthropic::EventParser{}, 2, stream)) return 72;
    if (!parser_sweep(lubancode::api::gemini::EventParser{}, 3, stream)) return 73;
    if (!parser_sweep(lubancode::api::responses::EventParser{}, 4,
        [](auto& parser, const auto& frame) { return parser.ExpandNonStream(frame.data); })) return 74;

    // The production typed callback owns numbers before allocating attempt
    // records. This covers both direct and subordinate aggregation; it does
    // not stand in for public Session Close/recovery acceptance.
    const auto source = wire::Chat(usages[0]);
    if (!source) return 80;
    const auto owner_sweep = [&](bool subordinate) {
        namespace api = lubancode::api;
        namespace runtime = lubancode::runtime;
        namespace sdk = lubancore;
        runtime::UsageAttemptContext context;
        context.trajectory_request_id = long_id;
        context.provider_response_id = long_id;
        context.model = long_id;
        context.step_id = long_id;
        context.turn_id = long_id;
        context.purpose = "main_turn";
        context.cache_epoch = 1;
        context.reported_by_provider = true;
        context.source_session_id = long_id;
        context.source_run_id = long_id;
        const auto numbers = wire::Numbers(*source);
        std::size_t count = 0;
        for (std::size_t index = 0; index <= count; ++index) {
            active_stage = "typed-owner"; active_route = subordinate ? "subordinate" : "direct"; active_fault = index;
            sdk::OperationUsage owned;
            owned.attempts_complete = true;
            runtime::IdAuthority ids;
            runtime::TurnEventAdapter events("fault-owner", ids);
            unsigned calls = 0, serialized = 0;
            events.ObserveUsage([&](const api::Usage& usage, const facts::Observation* observation,
                bool child, bool incomplete, const runtime::UsageAttemptContext& actual) {
                ++calls;
                sdk::detail::usage_result::CaptureAttempt(owned, usage, observation, child, incomplete, actual);
            });
            events.Attach([&](const runtime::ServerEvent&) { ++serialized; });
            const auto prior = refused;
            allocations = 0;
            bool returned = false, caught = false;
            try {
                count_allocations = index == 0;
                fail_after = index;
                returned = events.OnUsageFacts(numbers, &source->observation, subordinate, false, context);
                count_allocations = false;
            } catch (const std::bad_alloc&) { count_allocations = false; caught = true; }
            catch (...) { count_allocations = false; fail_after = 0; return false; }
            const bool still_armed = fail_after != 0;
            fail_after = 0;
            const auto& target = subordinate ? owned.subordinate : owned.direct;
            const auto& other = subordinate ? owned.direct : owned.subordinate;
            if (calls != 1 || serialized != 0 || target.coverage.samples != 1 || other.coverage.samples != 0 ||
                sdk::detail::usage_result::Native(target.total).input_tokens != 11 ||
                sdk::detail::usage_result::Native(target.total).output_tokens != 7 ||
                sdk::detail::usage_result::Native(target.total).cache_read_tokens != 13 ||
                sdk::detail::usage_result::Native(target.total).cache_creation_tokens != 17 ||
                sdk::detail::usage_result::Native(target.total).output_reasoning_tokens != 3) return false;
            if (index == 0) {
                count = allocations;
                if (!count || count > 10000 || !returned || caught || refused != prior ||
                    !owned.attempts_complete || owned.attempts.size() != 1) return false;
                for (std::size_t field = 0; field < facts::kFieldCount; ++field)
                    if (!api::usage_aggregation::Exact(target.coverage, static_cast<facts::Field>(field))) return false;
            } else {
                if (!caught || returned || still_armed || refused != prior + 1 ||
                    owned.attempts_complete || !owned.attempts.empty()) return false;
                for (std::size_t field = 0; field < facts::kFieldCount; ++field)
                    if (target.coverage.fields[field].anomalous != 1 ||
                        api::usage_aggregation::Exact(target.coverage, static_cast<facts::Field>(field))) return false;
            }
        }
        std::printf("actual-typed-owner-allocation-sweep:%s:allocations=%zu:faults=%zu\n",
            subordinate ? "subordinate" : "direct", count, count);
        return true;
    };
    if (!owner_sweep(false)) return 81;
    if (!owner_sweep(true)) return 82;
    return 0;
}
