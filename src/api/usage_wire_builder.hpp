#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include "api/usage_observation.hpp"
#include "platform/sha256.hpp"

namespace lubancode::api::usage_wire {
namespace facts = ::lubancore::usage::v1;
using RawIndex = std::optional<std::uint16_t>;

// Capture only named accounting scalars. Never dump an object, array, or model
// body to obtain an accounting summary. A DOM floating point summary describes
// the parsed scalar, not its original JSON number lexeme.
inline facts::RawField CaptureScalar(std::string path, const nlohmann::json& value,
                                    const std::vector<facts::RawField>* lexical = nullptr) {
    if (lexical) for (const auto& raw : *lexical) if (raw.path == path) return raw;
    facts::RawField raw; raw.path = std::move(path);
    if (value.is_number_unsigned()) {
        raw.kind = facts::RawKind::UnsignedInteger;
        const auto number = value.get<std::uint64_t>();
        raw.summary = std::to_string(number);
        if (number <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            raw.integer = static_cast<std::int64_t>(number);
    } else if (value.is_number_integer()) {
        raw.kind = facts::RawKind::SignedInteger;
        raw.integer = value.get<std::int64_t>();
        raw.summary = std::to_string(*raw.integer);
    } else if (value.is_number_float()) {
        raw.kind = facts::RawKind::FloatingPoint;
        raw.summary = value.dump();
    } else if (value.is_string()) {
        raw.kind = facts::RawKind::String;
        const auto& text = value.get_ref<const std::string&>();
        raw.summary = "string(bytes=" + std::to_string(text.size()) + ")";
        // Bad-type accounting strings can be arbitrarily large. Admission must
        // not hash a body-sized string merely to reject a token count.
        if (text.size() <= facts::kMaxMaterialBytes)
            raw.fingerprint = platform::Sha256Hex(std::string_view(text));
    } else if (value.is_boolean()) {
        raw.kind = facts::RawKind::Boolean;
        raw.summary = value.get<bool>() ? "true" : "false";
    } else if (value.is_object()) {
        raw.kind = facts::RawKind::Object; raw.summary = "object";
    } else if (value.is_array()) {
        raw.kind = facts::RawKind::Array; raw.summary = "array";
    } else {
        raw.kind = facts::RawKind::Null; raw.summary = "null";
    }
    return raw;
}

inline const nlohmann::json* Find(const nlohmann::json& object,
                                  std::initializer_list<const char*> keys) {
    const auto* value = &object;
    for (const auto* key : keys) {
        if (!value->is_object()) return nullptr;
        const auto it = value->find(key);
        if (it == value->end()) return nullptr;
        value = &*it;
    }
    return value;
}

struct Snapshot {
    std::array<std::int64_t, facts::kFieldCount> values{};
    facts::Observation observation;
    // Internal admission failure. Numbers remain facts, but this material must
    // not escape as a public observation or earn a success terminal.
    std::string_view material_error;
};

class Builder {
public:
    explicit Builder(std::string provider_namespace,
                     const std::vector<facts::RawField>* lexical = nullptr) : lexical_(lexical) {
        snapshot_.observation.provider_namespace = std::move(provider_namespace);
    }

    RawIndex Capture(std::string path, const nlohmann::json* value) {
        if (value == nullptr) return std::nullopt;
        auto& raws = snapshot_.observation.raw_fields;
        // Capture the same named scalar once even if several normalized fields
        // use it. Provider callers must keep each path tied to one DOM snapshot.
        for (std::size_t i = 0; i < raws.size(); ++i)
            if (raws[i].path == path) return static_cast<std::uint16_t>(i);
        if (raws.size() >= facts::kMaxRawFields) {
            material_error_ = "usage.wire.raw_limit"; return std::nullopt;
        }
        if (!usage_observation::TextFits(path, facts::kMaxPathBytes, true)) {
            material_error_ = "usage.wire.path"; return std::nullopt;
        }
        auto raw = CaptureScalar(std::move(path), *value, lexical_);
        if (!usage_observation::ValidRawField(raw)) {
            material_error_ = "usage.wire.raw_field"; return std::nullopt;
        }
        const auto index = static_cast<std::uint16_t>(raws.size());
        raws.push_back(std::move(raw)); return index;
    }

    std::optional<std::int64_t> Integer(RawIndex raw) const {
        if (!raw) return std::nullopt;
        return snapshot_.observation.raw_fields[*raw].integer;
    }

    void Report(facts::Field target, RawIndex raw) {
        auto& field = Get(target); field = {};
        snapshot_.values[static_cast<std::size_t>(target)] = 0;
        if (!raw) return;
        field.presence = facts::Presence::Present;
        SetOperands(field, {raw});
        const auto& evidence = snapshot_.observation.raw_fields[*raw];
        if (!evidence.integer) {
            const bool range = evidence.kind == facts::RawKind::UnsignedInteger ||
                               evidence.kind == facts::RawKind::SignedInteger;
            field.validity = range ? facts::Validity::OutOfRange : facts::Validity::InvalidType;
            Note(target, range ? facts::AnomalyCode::OutOfRange : facts::AnomalyCode::InvalidType,
                 "reported accounting scalar is not an int64", {raw});
            return;
        }
        field.validity = facts::Validity::ValidInteger;
        field.origin = facts::Origin::Reported;
        snapshot_.values[static_cast<std::size_t>(target)] = *evidence.integer;
        if (*evidence.integer < 0)
            Note(target, facts::AnomalyCode::Negative, "negative reported accounting scalar", {raw});
    }

    void BlockedDetails(facts::Field target, RawIndex container, RawIndex alias = {},
                        facts::Presence presence = facts::Presence::Missing) {
        auto& field = Get(target); field = {};
        field.presence = presence;
        field.validity = facts::Validity::UnavailableOperands;
        SetOperands(field, {container});
        snapshot_.values[static_cast<std::size_t>(target)] = 0;
        Note(target, facts::AnomalyCode::InvalidType, "accounting details container is not an object", {container});
        if (alias) Note(target, facts::AnomalyCode::AliasConflict,
                        "malformed canonical details prevent alias fallback", {container, alias});
    }

    // Presence, not value, picks the canonical spelling. A canonical zero or
    // wrong type prevents an alias from silently replacing it.
    RawIndex Select(facts::Field target, RawIndex canonical, RawIndex alias) {
        if (canonical && alias) {
            const auto& a = snapshot_.observation.raw_fields[*canonical];
            const auto& b = snapshot_.observation.raw_fields[*alias];
            if (a.kind != b.kind || a.integer != b.integer || a.summary != b.summary ||
                a.fingerprint != b.fingerprint)
                Note(target, facts::AnomalyCode::AliasConflict,
                     "canonical and alias both present; canonical wins", {canonical, alias});
            else if (a.kind == facts::RawKind::Object || a.kind == facts::RawKind::Array ||
                     a.kind == facts::RawKind::FloatingPoint ||
                     (a.kind == facts::RawKind::String && (a.fingerprint.empty() || b.fingerprint.empty())))
                Note(target, facts::AnomalyCode::AliasUnverifiable,
                     "bounded summaries cannot prove canonical and alias equality", {canonical, alias});
        }
        return canonical ? canonical : alias;
    }

    void Subtract(facts::Field target, RawIndex total, RawIndex part,
                  facts::Presence presence = facts::Presence::Present,
                  facts::Origin origin = facts::Origin::Normalized) {
        Calculate(target, total, part, false, presence, origin);
    }
    void Add(facts::Field target, RawIndex first, RawIndex second,
             facts::Presence presence = facts::Presence::Present,
             facts::Origin origin = facts::Origin::Normalized) {
        Calculate(target, first, second, true, presence, origin);
    }

    // Missing cache fields remain Missing in their own observations. Under a
    // provider's legacy total-only contract, absent splits do not manufacture
    // direct cache reports. Present-but-invalid splits prevent normalization.
    void OrdinaryInput(RawIndex total, RawIndex read, RawIndex write,
                       facts::Presence presence = facts::Presence::Present,
                       facts::Origin derived_origin = facts::Origin::Normalized) {
        auto& field = Get(facts::Field::Input); field = {};
        field.presence = total ? presence : facts::Presence::Missing;
        snapshot_.values[static_cast<std::size_t>(facts::Field::Input)] = 0;
        if (!total && !read && !write) return;
        SetOperands(field, {total, read, write});
        auto result = Integer(total);
        if (!result || (read && !Integer(read)) || (write && !Integer(write))) {
            field.validity = facts::Validity::UnavailableOperands;
            Note(facts::Field::Input, facts::AnomalyCode::MissingOperand,
                 "ordinary input requires valid present total and splits", {total, read, write});
            return;
        }
        if (read) result = usage_observation::CheckedSubtract(*result, *Integer(read));
        if (result && write) result = usage_observation::CheckedSubtract(*result, *Integer(write));
        if (!result) {
            field.validity = facts::Validity::Overflow;
            Note(facts::Field::Input, facts::AnomalyCode::ArithmeticOverflow,
                 "ordinary input normalization does not fit int64", {total, read, write});
            return;
        }
        field.validity = facts::Validity::ValidInteger;
        field.origin = (read || write) ? derived_origin : facts::Origin::Reported;
        snapshot_.values[static_cast<std::size_t>(facts::Field::Input)] = *result;
        if (*result < 0)
            Note(facts::Field::Input, facts::AnomalyCode::Negative,
                 "negative ordinary input is retained", {total, read, write});
    }

    void CheckSum(facts::Field field, RawIndex total, RawIndex a, RawIndex b) {
        if (!total || !a || !b) return;
        const auto t = Integer(total), x = Integer(a), y = Integer(b);
        if (!t || !x || !y) return;
        const auto sum = usage_observation::CheckedAdd(*x, *y);
        if (!sum)
            Note(field, facts::AnomalyCode::ArithmeticOverflow,
                 "cross-check sum does not fit int64", {total, a, b});
        else if (*sum != *t)
            Note(field, facts::AnomalyCode::InconsistentTotal,
                 "reported total differs from reported split sum", {total, a, b});
    }

    void Note(facts::Field field, facts::AnomalyCode code, std::string detail,
              std::initializer_list<RawIndex> witnesses) {
        auto& anomalies = snapshot_.observation.anomalies;
        if (anomalies.size() >= facts::kMaxAnomalies) {
            material_error_ = "usage.wire.anomaly_limit"; return;
        }
        facts::Anomaly anomaly; anomaly.field = field; anomaly.code = code;
        anomaly.detail = std::move(detail);
        for (const auto raw : witnesses) {
            if (!raw) continue;
            bool duplicate = false;
            for (std::size_t n = 0; n < anomaly.raw_field_count; ++n)
                duplicate = duplicate || anomaly.raw_fields[n] == *raw;
            if (duplicate) continue;
            if (anomaly.raw_field_count == facts::kMaxOperandsPerField) {
                material_error_ = "usage.wire.witness_limit"; return;
            }
            anomaly.raw_fields[anomaly.raw_field_count++] = *raw;
        }
        anomalies.push_back(std::move(anomaly));
    }

    std::expected<Snapshot, std::string_view> Finish() && {
        // Admission governs material, not ownership of already calculated
        // numeric facts. Parsers emit this numeric-only snapshot before their
        // error fence; Apply refuses to publish the rejected observation.
        if (!material_error_.empty()) {
            snapshot_.material_error = material_error_;
        } else {
            const auto admitted = usage_observation::Validate(snapshot_.observation, snapshot_.values);
            if (!admitted) snapshot_.material_error = admitted.error();
        }
        return std::move(snapshot_);
    }

private:
    facts::FieldObservation& Get(facts::Field field) {
        return snapshot_.observation.fields[static_cast<std::size_t>(field)];
    }
    void SetOperands(facts::FieldObservation& field, std::initializer_list<RawIndex> operands) {
        for (const auto raw : operands) {
            if (!raw) continue;
            bool duplicate = false;
            for (std::size_t n = 0; n < field.operand_count; ++n)
                duplicate = duplicate || field.operands[n] == *raw;
            if (!duplicate) field.operands[field.operand_count++] = *raw;
        }
    }
    void Calculate(facts::Field target, RawIndex first, RawIndex second, bool add,
                   facts::Presence presence, facts::Origin origin) {
        auto& field = Get(target); field = {}; field.presence = presence;
        snapshot_.values[static_cast<std::size_t>(target)] = 0;
        SetOperands(field, {first, second});
        const auto a = Integer(first), b = Integer(second);
        if (!a || !b) {
            field.validity = facts::Validity::UnavailableOperands;
            Note(target, facts::AnomalyCode::MissingOperand,
                 "normalization requires two valid integer operands", {first, second});
            return;
        }
        const auto result = add ? usage_observation::CheckedAdd(*a, *b) :
                                  usage_observation::CheckedSubtract(*a, *b);
        if (!result) {
            field.validity = facts::Validity::Overflow;
            Note(target, facts::AnomalyCode::ArithmeticOverflow,
                 "accounting arithmetic does not fit int64", {first, second});
            return;
        }
        field.validity = facts::Validity::ValidInteger; field.origin = origin;
        snapshot_.values[static_cast<std::size_t>(target)] = *result;
        if (*result < 0)
            Note(target, facts::AnomalyCode::Negative, "negative normalized accounting scalar", {first, second});
    }
    Snapshot snapshot_;
    const std::vector<facts::RawField>* lexical_ = nullptr;
    std::string_view material_error_;
};

}  // namespace lubancode::api::usage_wire
