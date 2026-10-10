// A separate test process owns this allocator replacement. SDK/CLI binaries
// and their aggregate test executables retain their normal allocators.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>

#include "api/usage_provider_normalizers.hpp"
#include "api/usage_lexical.hpp"
#include "api/chat/events.hpp"
#include "api/responses/events.hpp"
#include "api/anthropic/events.hpp"
#include "api/gemini/events.hpp"

namespace {
thread_local bool refuse_next = false;
thread_local unsigned refused = 0;
}

void* operator new(std::size_t size) {
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

int main() {
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
    return 0;
}
