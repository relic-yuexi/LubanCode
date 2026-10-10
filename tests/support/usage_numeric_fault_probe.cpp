// A separate test process owns this allocator replacement. SDK/CLI binaries
// and their aggregate test executables retain their normal allocators.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "api/usage_provider_normalizers.hpp"

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
    return 0;
}
