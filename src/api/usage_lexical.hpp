#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "api/usage_provider_normalizers.hpp"

namespace lubancode::api::usage_wire {
enum class Dialect { Chat, Responses, Anthropic, Gemini, ResponsesNonStream };

// Borrow the frame. Unknown strings and numbers are traversed without copying
// or hashing them. Stack, keys, captured scalars and summaries have fixed caps.
// This observer never decides whether a body is acceptable; the DOM parser does.
class LexicalUsage {
public:
    using SourceCheckpoint = void (*)(void*, const SourceNumbers&, bool, bool) noexcept;
    LexicalUsage(std::string_view source, Dialect dialect,
        NumericObserver observer = nullptr, void* context = nullptr,
        SourceCheckpoint source_checkpoint = nullptr) : source_(source), dialect_(dialect) {
        if (observer || source_checkpoint || dialect == Dialect::Anthropic) {
            // First pass borrows bytes and fills fixed slots only. Even an ID
            // preceding usage cannot allocate before these numeric facts.
            LexicalUsage numeric(source, dialect, NumericOnly{});
            anthropic_scope_ = numeric.event_name_;
            const auto& captured = dialect == Dialect::Anthropic && numeric.event_name_ == 1
                ? numeric.message_numbers_ : numeric.source_numbers_;
            if (source_checkpoint) source_checkpoint(context, captured,
                numeric.event_name_ == 1, numeric.event_name_ == 2);
            else if (observer && captured.is_object()) {
                const auto values = dialect == Dialect::Chat
                    ? detail::ChatProjection<NumericBuilder>(captured)
                    : (dialect == Dialect::Responses || dialect == Dialect::ResponsesNonStream)
                        ? detail::ResponsesProjection<NumericBuilder>(captured)
                    : dialect == Dialect::Gemini ? detail::GeminiProjection<NumericBuilder>(captured)
                        : detail::AnthropicProjection<NumericBuilder>(captured);
                if (values) observer(context, *values);
            }
        }
        Scan();
    }
    bool complete = false;
    bool duplicate = false;
    std::vector<facts::RawField> numbers;
    std::optional<std::string> response_id;
    std::optional<std::string> event_type;

    nlohmann::json NumericObject() const {
        auto object = nlohmann::json::object();
        for (const auto& raw : numbers) {
            const auto first = raw.path.find('.');
            auto suffix = std::string_view(raw.path).substr(first + 1);
            const auto dot = suffix.find('.');
            // The placeholder carries presence only. CaptureScalar replaces it
            // with the original lexical evidence, including out-of-range ints.
            if (dot == std::string_view::npos) object[std::string(suffix)] = nullptr;
            else object[std::string(suffix.substr(0, dot))][std::string(suffix.substr(dot + 1))] = nullptr;
        }
        return object;
    }
    std::expected<Snapshot, std::string_view> Partial(
        NumericObserver observer = nullptr, void* observer_context = nullptr) const {
        const auto object = NumericObject();
        auto result = dialect_ == Dialect::Chat ? Chat(object, &numbers, observer, observer_context)
            : (dialect_ == Dialect::Responses || dialect_ == Dialect::ResponsesNonStream) ? Responses(object, &numbers, observer, observer_context)
            : dialect_ == Dialect::Gemini ? Gemini(object, &numbers, observer, observer_context)
                : Anthropic(object, &numbers, observer, observer_context);
        if (result) MarkIncomplete(*result);
        return result;
    }
    static void MarkIncomplete(Snapshot& snapshot) {
        for (const auto& old : snapshot.observation.anomalies)
            if (old.code == facts::AnomalyCode::ParseIncomplete && old.affected_field_count == facts::kFieldCount) return;
        if (snapshot.observation.anomalies.size() >= facts::kMaxAnomalies) {
            snapshot.material_error = "usage.material.parse_marker_capacity";
            return;  // Preserve every existing fact; refuse unrepresentable material.
        }
        facts::Anomaly anomaly;
        anomaly.code = facts::AnomalyCode::ParseIncomplete;
        anomaly.detail = "original frame did not admit a complete accounting observation";
        anomaly.affected_field_count = facts::kFieldCount;
        for (std::size_t i = 0; i < facts::kFieldCount; ++i)
            anomaly.affected_fields[i] = static_cast<facts::Field>(i);
        snapshot.observation.anomalies.push_back(std::move(anomaly));
        const auto admitted = usage_observation::Validate(snapshot.observation, snapshot.values);
        if (!admitted) snapshot.material_error = admitted.error();
    }
private:
    std::string_view source_;
    Dialect dialect_;
    std::size_t offset_ = 0;
    std::array<std::string_view, 64> path_{};
    struct NumericOnly {};
    LexicalUsage(std::string_view source, Dialect dialect, NumericOnly)
        : source_(source), dialect_(dialect), material_(false) { Scan(); }
    void Scan() {
        complete = Value(0, false);
        Space(); complete = complete && offset_ == source_.size();
    }
    bool material_ = true;
    SourceNumbers source_numbers_;
    SourceNumbers message_numbers_;
    unsigned event_name_ = 0;
    unsigned anthropic_scope_ = 0;
    SourceNumbers& SourceForPath() noexcept {
        return dialect_ == Dialect::Anthropic && path_[0] == "message" ? message_numbers_ : source_numbers_;
    }
    std::array<std::string_view,64> seen_paths_{};
    std::size_t seen_path_count_ = 0;
    std::array<std::string_view,8> seen_scopes_{};
    std::size_t seen_scope_count_ = 0;
    bool identity_seen_ = false;
    void Space() {
        while (offset_ < source_.size() && (source_[offset_] == ' ' || source_[offset_] == '\t' ||
               source_[offset_] == '\r' || source_[offset_] == '\n')) ++offset_;
    }
    bool Eat(char value) {
        Space(); if (offset_ == source_.size() || source_[offset_] != value) return false;
        ++offset_; return true;
    }
    static int Hex(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }
    bool String(std::span<char> decoded, std::size_t& size, bool& fits) {
        if (!Eat('"')) return false;
        size = 0; fits = true;
        const auto put = [&](char c) { if (size < decoded.size()) decoded[size++] = c; else fits = false; };
        while (offset_ < source_.size()) {
            unsigned char c = source_[offset_++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { put(static_cast<char>(c)); continue; }
            if (offset_ == source_.size()) return false;
            const char escape = source_[offset_++];
            if (escape == '"' || escape == '\\' || escape == '/') put(escape);
            else if (escape == 'b') put('\b'); else if (escape == 'f') put('\f');
            else if (escape == 'n') put('\n'); else if (escape == 'r') put('\r'); else if (escape == 't') put('\t');
            else if (escape == 'u') {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (offset_ == source_.size()) return false;
                    const int hex = Hex(source_[offset_++]); if (hex < 0) return false;
                    code = code * 16 + static_cast<unsigned>(hex);
                }
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (source_.size() - offset_ < 6 || source_[offset_] != '\\' || source_[offset_ + 1] != 'u') return false;
                    offset_ += 2; unsigned low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int hex = Hex(source_[offset_++]); if (hex < 0) return false;
                        low = low * 16 + static_cast<unsigned>(hex);
                    }
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                if (code < 0x80) put(static_cast<char>(code));
                else if (code < 0x800) {
                    put(static_cast<char>(0xc0 | (code >> 6))); put(static_cast<char>(0x80 | (code & 0x3f)));
                } else if (code < 0x10000) {
                    put(static_cast<char>(0xe0 | (code >> 12))); put(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    put(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                    put(static_cast<char>(0xf0 | (code >> 18))); put(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                    put(static_cast<char>(0x80 | ((code >> 6) & 0x3f))); put(static_cast<char>(0x80 | (code & 0x3f)));
                }
            } else return false;
        }
        return false;
    }
    std::optional<std::string_view> Target(std::size_t depth, bool array) const {
        if (array) return {};
        std::size_t start = 0;
        const auto prefix = dialect_ == Dialect::Gemini ? "usageMetadata" : "usage";
        if (dialect_ == Dialect::Responses) {
            if (depth < 3 || path_[0] != "response") return {};
            start = 1;
        } else if (dialect_ == Dialect::Anthropic && depth >= 3 && path_[0] == "message") start = 1;
        if (material_ && dialect_ == Dialect::Anthropic &&
            ((anthropic_scope_ == 1 && start == 0) || (anthropic_scope_ == 2 && start == 1))) return {};
        if (depth < start + 2 || depth > start + 3 || path_[start] != prefix) return {};
        const auto leaf = path_[depth - 1];
        bool known = false;
        if (dialect_ == Dialect::Chat) {
            if (depth == start + 2) known = leaf == "prompt_tokens" || leaf == "completion_tokens" ||
                leaf == "prompt_cache_hit_tokens" || leaf == "prompt_cache_miss_tokens" ||
                leaf == "cache_write_tokens" || leaf == "reasoning_tokens";
            else known = (path_[start + 1] == "prompt_tokens_details" && (leaf == "cached_tokens" || leaf == "cache_write_tokens")) ||
                (path_[start + 1] == "completion_tokens_details" && leaf == "reasoning_tokens");
        } else if (dialect_ == Dialect::Responses || dialect_ == Dialect::ResponsesNonStream) {
            if (depth == start + 2) known = leaf == "input_tokens" || leaf == "output_tokens" || leaf == "cache_write_tokens";
            else known = (path_[start + 1] == "input_tokens_details" && (leaf == "cached_tokens" || leaf == "cache_write_tokens")) ||
                (path_[start + 1] == "output_tokens_details" && leaf == "reasoning_tokens");
        } else if (depth == start + 2) {
            known = dialect_ == Dialect::Anthropic ? leaf == "input_tokens" || leaf == "output_tokens" ||
                leaf == "cache_read_input_tokens" || leaf == "cache_creation_input_tokens"
                : leaf == "promptTokenCount" || leaf == "cachedContentTokenCount" || leaf == "candidatesTokenCount" ||
                  leaf == "thoughtsTokenCount" || leaf == "totalTokenCount";
        }
        if (!known) return {};
        static constexpr std::string_view paths[] = {
            "usage.prompt_tokens", "usage.completion_tokens", "usage.prompt_cache_hit_tokens",
            "usage.prompt_cache_miss_tokens", "usage.cache_write_tokens", "usage.reasoning_tokens",
            "usage.prompt_tokens_details.cached_tokens", "usage.prompt_tokens_details.cache_write_tokens",
            "usage.completion_tokens_details.reasoning_tokens", "usage.input_tokens", "usage.output_tokens",
            "usage.input_tokens_details.cached_tokens", "usage.input_tokens_details.cache_write_tokens",
            "usage.output_tokens_details.reasoning_tokens", "usage.cache_read_input_tokens",
            "usage.cache_creation_input_tokens", "usageMetadata.promptTokenCount",
            "usageMetadata.cachedContentTokenCount", "usageMetadata.candidatesTokenCount",
            "usageMetadata.thoughtsTokenCount", "usageMetadata.totalTokenCount"
        };
        for (const auto canonical : paths) {
            auto suffix = canonical;
            bool match = true;
            for (auto i = start; i < depth; ++i) {
                const auto separator = suffix.find('.');
                if (suffix.substr(0, separator) != path_[i]) { match = false; break; }
                if (i + 1 < depth) {
                    if (separator == std::string_view::npos) { match = false; break; }
                    suffix.remove_prefix(separator + 1);
                } else if (separator != std::string_view::npos) match = false;
            }
            if (match) return canonical;
        }
        return {};
    }
    bool Number(std::size_t depth, bool array) {
        const auto begin = offset_;
        if (source_[offset_] == '-') ++offset_;
        if (offset_ == source_.size()) return false;
        if (source_[offset_] == '0') ++offset_;
        else {
            if (source_[offset_] < '1' || source_[offset_] > '9') return false;
            while (offset_ < source_.size() && source_[offset_] >= '0' && source_[offset_] <= '9') ++offset_;
        }
        bool floating = false;
        if (offset_ < source_.size() && source_[offset_] == '.') {
            floating = true; ++offset_; const auto digits = offset_;
            while (offset_ < source_.size() && source_[offset_] >= '0' && source_[offset_] <= '9') ++offset_;
            if (offset_ == digits) return false;
        }
        if (offset_ < source_.size() && (source_[offset_] == 'e' || source_[offset_] == 'E')) {
            floating = true; ++offset_;
            if (offset_ < source_.size() && (source_[offset_] == '+' || source_[offset_] == '-')) ++offset_;
            const auto digits = offset_;
            while (offset_ < source_.size() && source_[offset_] >= '0' && source_[offset_] <= '9') ++offset_;
            if (offset_ == digits) return false;
        }
        // A valid token boundary is required before admitting a numeric fact.
        if (offset_ < source_.size() && source_[offset_] != ',' && source_[offset_] != '}' && source_[offset_] != ']' &&
            source_[offset_] != ' ' && source_[offset_] != '\t' && source_[offset_] != '\n' && source_[offset_] != '\r') return false;
        auto path = Target(depth, array);
        if (!path) return true;
        const auto token = source_.substr(begin, offset_ - begin);
        std::optional<std::int64_t> owned_integer;
        if (!floating) {
            std::int64_t integer = 0;
            const auto converted = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (converted.ec == std::errc{} && converted.ptr == token.data() + token.size()) owned_integer = integer;
        }
        SourceForPath().Set(*path, owned_integer);
        if (!material_) return true;
        facts::RawField raw; raw.path = *path;
        raw.kind = floating ? facts::RawKind::FloatingPoint : token.front() == '-' ? facts::RawKind::SignedInteger : facts::RawKind::UnsignedInteger;
        raw.summary = std::string(token.substr(0, facts::kMaxSummaryBytes));
        if (token.size() <= facts::kMaxMaterialBytes) raw.fingerprint = platform::Sha256Hex(token);
        raw.integer = owned_integer;
        for (auto& old : numbers) if (old.path == raw.path) { old = std::move(raw); duplicate = true; return true; }
        if (numbers.size() >= facts::kMaxRawFields) return false;
        numbers.push_back(std::move(raw)); return true;
    }
    bool Value(std::size_t depth, bool array) {
        Space(); if (depth >= path_.size() || offset_ == source_.size()) return false;
        const char c = source_[offset_];
        const bool identity_path = !array && (dialect_ == Dialect::Chat || dialect_ == Dialect::ResponsesNonStream
            ? depth == 1 && path_[0] == "id"
            : dialect_ == Dialect::Gemini ? depth == 1 && path_[0] == "responseId"
            : depth == 2 && path_[1] == "id" && path_[0] == (dialect_ == Dialect::Responses ? "response" : "message"));
        if (identity_path) {
            duplicate = duplicate || identity_seen_; identity_seen_ = true;
            response_id.reset();  // Last wrong-type identity never borrows an earlier ID.
        }
        if (!array && depth == 1 && path_[0] == "type") { event_type.reset(); event_name_ = 0; }
        std::size_t usage_depth = dialect_ == Dialect::Responses ? 2 :
            dialect_ == Dialect::Anthropic && depth > 1 && path_[0] == "message" ? 2 : 1;
        const auto usage_key = dialect_ == Dialect::Gemini ? "usageMetadata" : "usage";
        const bool selected_anthropic_scope = !material_ || dialect_ != Dialect::Anthropic ||
            anthropic_scope_ == 0 || (anthropic_scope_ == 1 ? usage_depth == 2 : usage_depth == 1);
        const bool correct_root = selected_anthropic_scope &&
            (usage_depth == 1 || path_[0] == (dialect_ == Dialect::Responses ? "response" : "message"));
        if (!array && correct_root && depth >= usage_depth && depth <= usage_depth + 1 &&
            path_[usage_depth - 1] == usage_key &&
            (depth == usage_depth || path_[depth - 1] == "prompt_tokens_details" ||
             path_[depth - 1] == "completion_tokens_details" || path_[depth - 1] == "input_tokens_details" ||
             path_[depth - 1] == "output_tokens_details")) {
            std::string_view scope(usage_key);
            if (depth > usage_depth) {
                const auto key = path_[depth - 1];
                scope = key == "prompt_tokens_details" ? "usage.prompt_tokens_details"
                    : key == "completion_tokens_details" ? "usage.completion_tokens_details"
                    : key == "input_tokens_details" ? "usage.input_tokens_details" : "usage.output_tokens_details";
            }
            SourceForPath().Scope(scope, c == '{');
            bool seen = false;
            for (std::size_t i = 0; i < seen_scope_count_; ++i) seen = seen || seen_scopes_[i] == scope;
            if (seen) {
                duplicate = true;
                if (material_) for (auto it = numbers.begin(); it != numbers.end();)
                    if (it->path.starts_with(scope) && it->path.size() > scope.size() && it->path[scope.size()] == '.')
                        it = numbers.erase(it); else ++it;
            } else {
                if (seen_scope_count_ == seen_scopes_.size()) return false;
                seen_scopes_[seen_scope_count_++] = scope;
            }
        }
        if (auto target = Target(depth, array)) {
            SourceForPath().Set(*target, {}, c == '{');
            bool seen = false;
            for (std::size_t i = 0; i < seen_path_count_; ++i) seen = seen || seen_paths_[i] == *target;
            if (seen) {
                duplicate = true;
                // A last null/string/container must not inherit a previous
                // number. DOM duplicate-key behavior and source evidence agree.
                for (auto it = numbers.begin(); it != numbers.end(); ++it)
                    if (it->path == *target) { numbers.erase(it); break; }
            } else {
                if (seen_path_count_ == seen_paths_.size()) return false;
                seen_paths_[seen_path_count_++] = *target;
            }
        }
        if (c == '{') {
            ++offset_; if (Eat('}')) return true;
            do {
                std::array<char, 64> key{}; std::size_t size = 0; bool fits = false;
                if (!String(key, size, fits) || !Eat(':')) return false;
                path_[depth] = fits ? std::string_view(key.data(), size) : std::string_view{};
                if (!Value(depth + 1, array)) return false;
                Space(); if (Eat('}')) return true;
            } while (Eat(','));
            return false;
        }
        if (c == '[') {
            ++offset_; if (Eat(']')) return true;
            do { if (!Value(depth + 1, true)) return false; if (Eat(']')) return true; } while (Eat(','));
            return false;
        }
        if (c == '"') {
            std::array<char, facts::kMaxResponseIdBytes> text{}; std::size_t size = 0; bool fits = false;
            if (!String(text, size, fits)) return false;
            if (!array && fits) {
                std::string_view value(text.data(), size);
                if (depth == 1 && path_[0] == "type") {
                    event_name_ = value == "message_start" ? 1 : value == "message_delta" ? 2 : 0;
                    if (material_) event_type = std::string(value);
                }
                if (material_ && identity_path && !value.empty()) {
                    std::string owned(value);
                    if (usage_observation::TextFits(owned, facts::kMaxResponseIdBytes, true)) response_id = std::move(owned);
                }
            }
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return Number(depth, array);
        for (const std::string_view literal : {"null", "true", "false"})
            if (source_.substr(offset_, literal.size()) == literal) { offset_ += literal.size(); return true; }
        return false;
    }
};
} // namespace lubancode::api::usage_wire
