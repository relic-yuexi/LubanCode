#pragma once

#include <expected>
#include <string_view>
#include <utility>

#include "api/usage_wire_builder.hpp"

namespace lubancode::api::usage_wire {
using Field = facts::Field;

inline RawIndex MalformedDetails(Builder& builder, const nlohmann::json& usage,
                                const char* key, const char* path) {
    const auto* container = Find(usage, {key});
    return container && !container->is_object() ? builder.Capture(path, container) : RawIndex{};
}

// These functions consume an individual usage object. Stateful parsers own
// message boundaries and cumulative per-field updates; they must not add two
// copies of a stream's usage. No response ID is synthesized here.
inline std::expected<Snapshot, std::string_view> Chat(const nlohmann::json& usage,
    const std::vector<facts::RawField>* lexical = nullptr) {
    if (!usage.is_object()) return std::unexpected("usage.wire.object");
    Builder b("openai.chat", lexical);
    const auto bad_input_details = MalformedDetails(b, usage, "prompt_tokens_details", "usage.prompt_tokens_details");
    const auto bad_output_details = MalformedDetails(b, usage, "completion_tokens_details", "usage.completion_tokens_details");
    const auto total=b.Capture("usage.prompt_tokens",Find(usage,{"prompt_tokens"}));
    const auto output=b.Capture("usage.completion_tokens",Find(usage,{"completion_tokens"}));
    const auto hit=b.Capture("usage.prompt_cache_hit_tokens",Find(usage,{"prompt_cache_hit_tokens"}));
    const auto miss=b.Capture("usage.prompt_cache_miss_tokens",Find(usage,{"prompt_cache_miss_tokens"}));
    const auto cached=b.Capture("usage.prompt_tokens_details.cached_tokens",Find(usage,{"prompt_tokens_details","cached_tokens"}));
    const auto nested_write=b.Capture("usage.prompt_tokens_details.cache_write_tokens",Find(usage,{"prompt_tokens_details","cache_write_tokens"}));
    const auto legacy_write=b.Capture("usage.cache_write_tokens",Find(usage,{"cache_write_tokens"}));
    const auto nested_reasoning=b.Capture("usage.completion_tokens_details.reasoning_tokens",Find(usage,{"completion_tokens_details","reasoning_tokens"}));
    const auto legacy_reasoning=b.Capture("usage.reasoning_tokens",Find(usage,{"reasoning_tokens"}));
    const auto write=b.Select(Field::CacheCreation,nested_write,legacy_write);
    b.Report(Field::Output,output);
    b.Report(Field::CacheCreation,write);
    b.Report(Field::OutputReasoning,b.Select(Field::OutputReasoning,nested_reasoning,legacy_reasoning));
    if (hit || miss) {
        // A present malformed legacy split still selects that dialect. Falling
        // through to details would erase the provider's contradictory evidence.
        b.Select(Field::CacheRead,hit,cached);
        b.Report(Field::CacheRead,hit);
        if (miss) {
            if (write) b.Subtract(Field::Input,miss,write);
            else b.Report(Field::Input,miss);
            if (!hit) b.Subtract(Field::CacheRead,total,miss,facts::Presence::Missing,facts::Origin::Inferred);
        } else {
            b.Subtract(Field::Input,total,hit,facts::Presence::Missing,facts::Origin::Inferred);
            // Inferred ordinary input additionally subtracts reported writes.
            // Use all raw operands together, preserving the missing-miss origin.
            if (write) {
                b.OrdinaryInput(total,hit,write,facts::Presence::Missing,facts::Origin::Inferred);
                // This is derived from the total, not a direct miss report.
            }
            b.Note(Field::Input,facts::AnomalyCode::MissingOperand,
                   "legacy miss is absent; ordinary input is inferred",{total,hit,write});
        }
        if (!hit)
            b.Note(Field::CacheRead,facts::AnomalyCode::MissingOperand,
                   "legacy hit is absent; cache read is inferred",{total,miss});
        b.CheckSum(Field::Input,total,hit,miss);
    } else {
        b.Report(Field::CacheRead,cached);
        b.OrdinaryInput(total,cached,write);
    }
    if (bad_input_details) {
        if (!hit && !miss) b.BlockedDetails(Field::CacheRead, bad_input_details);
        else b.Note(Field::CacheRead, facts::AnomalyCode::InvalidType,
                    "alternate accounting details container is not an object", {bad_input_details});
        b.BlockedDetails(Field::CacheCreation, bad_input_details, legacy_write);
        b.BlockedDetails(Field::Input, bad_input_details, {},
                         total || miss ? facts::Presence::Present : facts::Presence::Missing);
    }
    if (bad_output_details) b.BlockedDetails(Field::OutputReasoning, bad_output_details, legacy_reasoning);
    return std::move(b).Finish();
}

inline std::expected<Snapshot, std::string_view> Responses(const nlohmann::json& usage,
    const std::vector<facts::RawField>* lexical = nullptr) {
    if (!usage.is_object()) return std::unexpected("usage.wire.object");
    Builder b("openai.responses", lexical);
    const auto bad_input_details = MalformedDetails(b, usage, "input_tokens_details", "usage.input_tokens_details");
    const auto bad_output_details = MalformedDetails(b, usage, "output_tokens_details", "usage.output_tokens_details");
    const auto total=b.Capture("usage.input_tokens",Find(usage,{"input_tokens"}));
    const auto output=b.Capture("usage.output_tokens",Find(usage,{"output_tokens"}));
    const auto read=b.Capture("usage.input_tokens_details.cached_tokens",Find(usage,{"input_tokens_details","cached_tokens"}));
    const auto nested_write=b.Capture("usage.input_tokens_details.cache_write_tokens",Find(usage,{"input_tokens_details","cache_write_tokens"}));
    const auto legacy_write=b.Capture("usage.cache_write_tokens",Find(usage,{"cache_write_tokens"}));
    const auto reasoning=b.Capture("usage.output_tokens_details.reasoning_tokens",Find(usage,{"output_tokens_details","reasoning_tokens"}));
    const auto write=b.Select(Field::CacheCreation,nested_write,legacy_write);
    b.Report(Field::Output,output);
    b.Report(Field::CacheRead,read);
    b.Report(Field::CacheCreation,write);
    b.Report(Field::OutputReasoning,reasoning);
    b.OrdinaryInput(total,read,write);
    if (bad_input_details) {
        b.BlockedDetails(Field::CacheRead, bad_input_details);
        b.BlockedDetails(Field::CacheCreation, bad_input_details, legacy_write);
        b.BlockedDetails(Field::Input, bad_input_details, {},
                         total ? facts::Presence::Present : facts::Presence::Missing);
    }
    if (bad_output_details) b.BlockedDetails(Field::OutputReasoning, bad_output_details);
    return std::move(b).Finish();
}

inline std::expected<Snapshot, std::string_view> Anthropic(const nlohmann::json& usage,
    const std::vector<facts::RawField>* lexical = nullptr) {
    if (!usage.is_object()) return std::unexpected("usage.wire.object");
    Builder b("anthropic.messages", lexical);
    b.Report(Field::Input,b.Capture("usage.input_tokens",Find(usage,{"input_tokens"})));
    b.Report(Field::Output,b.Capture("usage.output_tokens",Find(usage,{"output_tokens"})));
    b.Report(Field::CacheRead,b.Capture("usage.cache_read_input_tokens",Find(usage,{"cache_read_input_tokens"})));
    b.Report(Field::CacheCreation,b.Capture("usage.cache_creation_input_tokens",Find(usage,{"cache_creation_input_tokens"})));
    // Anthropic does not report a separate reasoning token scalar here.
    // Absence remains Unknown; a text thinking block cannot supply a token count.
    return std::move(b).Finish();
}

inline std::expected<Snapshot, std::string_view> Gemini(const nlohmann::json& usage,
    const std::vector<facts::RawField>* lexical = nullptr) {
    if (!usage.is_object()) return std::unexpected("usage.wire.object");
    Builder b("google.generateContent", lexical);
    const auto prompt=b.Capture("usageMetadata.promptTokenCount",Find(usage,{"promptTokenCount"}));
    const auto cached=b.Capture("usageMetadata.cachedContentTokenCount",Find(usage,{"cachedContentTokenCount"}));
    const auto candidates=b.Capture("usageMetadata.candidatesTokenCount",Find(usage,{"candidatesTokenCount"}));
    const auto thoughts=b.Capture("usageMetadata.thoughtsTokenCount",Find(usage,{"thoughtsTokenCount"}));
    const auto total=b.Capture("usageMetadata.totalTokenCount",Find(usage,{"totalTokenCount"}));
    b.Report(Field::CacheRead,cached);
    b.Report(Field::OutputReasoning,thoughts);
    b.OrdinaryInput(prompt,cached,std::nullopt);
    if (!thoughts) {
        b.Report(Field::Output,candidates);
    } else {
        const auto p=b.Integer(prompt), c=b.Integer(candidates), t=b.Integer(thoughts), all=b.Integer(total);
        const auto base=(p && c) ? usage_observation::CheckedAdd(*p,*c) : std::nullopt;
        const auto expanded=(base && t) ? usage_observation::CheckedAdd(*base,*t) : std::nullopt;
        if (all && base && expanded && *all==*base && *all!=*expanded) {
            // An older gateway explicitly includes thoughts in candidates.
            // Preserve the reported candidates total, never add thoughts twice.
            b.Report(Field::Output,candidates);
        } else {
            // Official modern counters separate candidates and thoughts. Zero
            // is still present and participates in the documented normalization.
            b.Add(Field::Output,candidates,thoughts);
            if (all && expanded && *all!=*expanded)
                b.Note(Field::Output,facts::AnomalyCode::InconsistentTotal,
                       "Gemini total matches neither supported accounting shape",{prompt,candidates,thoughts,total});
            if (p && c && t && (!base || !expanded))
                b.Note(Field::Output,facts::AnomalyCode::ArithmeticOverflow,
                       "Gemini total cross-check does not fit int64",{prompt,candidates,thoughts,total});
        }
    }
    // No cache-creation scalar: its Missing/Unknown observation stays intact.
    return std::move(b).Finish();
}

}  // namespace lubancode::api::usage_wire
