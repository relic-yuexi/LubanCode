#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <lubancore/usage.hpp>
#include "platform/text_encoding.hpp"

namespace lubancode::api::usage_observation {
namespace facts = ::lubancore::usage::v1;

// Avoid negating INT64_MIN. Every intermediate in each guard is representable.
inline std::optional<std::int64_t> CheckedAdd(std::int64_t a, std::int64_t b) {
    constexpr auto min = (std::numeric_limits<std::int64_t>::min)();
    constexpr auto max = (std::numeric_limits<std::int64_t>::max)();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return std::nullopt;
    return a + b;
}
inline std::optional<std::int64_t> CheckedSubtract(std::int64_t a, std::int64_t b) {
    constexpr auto min = (std::numeric_limits<std::int64_t>::min)();
    constexpr auto max = (std::numeric_limits<std::int64_t>::max)();
    if ((b > 0 && a < min + b) || (b < 0 && a > max + b)) return std::nullopt;
    return a - b;
}

template <typename Enum>
inline bool KnownEnum(Enum value, Enum last) {
    using Raw = std::underlying_type_t<Enum>;
    return static_cast<Raw>(value) <= static_cast<Raw>(last);
}
inline bool TextFits(const std::string& value, std::size_t limit, bool nonempty = false) {
    return (!nonempty || !value.empty()) && value.size() <= limit &&
           value.find('\0') == std::string::npos && platform::IsValidUtf8(value);
}
inline bool FingerprintFits(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 64) return false;
    for (const auto c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool ValidRawField(const facts::RawField& raw) {
    if (!TextFits(raw.path, facts::kMaxPathBytes, true) ||
        !TextFits(raw.summary, facts::kMaxSummaryBytes) || !FingerprintFits(raw.fingerprint) ||
        !KnownEnum(raw.kind, facts::RawKind::Array)) return false;
    if (raw.integer) {
        if (raw.kind != facts::RawKind::SignedInteger && raw.kind != facts::RawKind::UnsignedInteger) return false;
        if (raw.kind == facts::RawKind::UnsignedInteger && *raw.integer < 0) return false;
    }
    // No integer on an integer-shaped raw field is permitted evidence for
    // OutOfRange, not a numeric zero and not an exception that discards the rest.
    return true;
}

// Read admission before copying a host's strings/vectors. Selected numbers live
// in Usage; the observation cannot introduce a contradictory second copy.
inline std::expected<void, std::string_view> Validate(
    const facts::Observation& observation,
    const std::array<std::int64_t, facts::kFieldCount>& selected) {
    if (observation.raw_fields.size() > facts::kMaxRawFields ||
        observation.extensions.size() > facts::kMaxExtensions ||
        observation.anomalies.size() > facts::kMaxAnomalies) return std::unexpected("usage.material.count_limit");
    if (!TextFits(observation.provider_namespace, facts::kMaxNamespaceBytes, true))
        return std::unexpected("usage.material.provider_namespace");
    std::size_t bytes = 0;
    const auto add_bytes = [&](std::size_t count) {
        if (count > facts::kMaxMaterialBytes - bytes) return false;
        bytes += count; return true;
    };
    const auto raw_bytes = [&](const facts::RawField& raw) {
        return add_bytes(raw.path.size()) && add_bytes(raw.summary.size()) && add_bytes(raw.fingerprint.size());
    };
    if (!add_bytes(observation.provider_namespace.size())) return std::unexpected("usage.material.byte_limit");
    for (std::size_t n = 0; n < observation.raw_fields.size(); ++n) {
        const auto& raw = observation.raw_fields[n];
        if (!ValidRawField(raw)) return std::unexpected("usage.material.raw_field");
        for (std::size_t previous = 0; previous < n; ++previous)
            if (observation.raw_fields[previous].path == raw.path)
                return std::unexpected("usage.material.duplicate_path");
        if (!raw_bytes(raw)) return std::unexpected("usage.material.byte_limit");
    }
    for (std::size_t index = 0; index < facts::kFieldCount; ++index) {
        const auto& field = observation.fields[index];
        if (!KnownEnum(field.presence, facts::Presence::Present) ||
            !KnownEnum(field.validity, facts::Validity::Overflow) ||
            !KnownEnum(field.origin, facts::Origin::Inferred)) return std::unexpected("usage.material.field_enum");
        if (field.operand_count > facts::kMaxOperandsPerField) return std::unexpected("usage.material.operand_limit");
        for (std::size_t n = 0; n < field.operand_count; ++n) {
            const auto operand = field.operands[n];
            if (operand >= observation.raw_fields.size()) return std::unexpected("usage.material.operand_index");
            for (std::size_t previous = 0; previous < n; ++previous)
                if (field.operands[previous] == operand) return std::unexpected("usage.material.operand_index");
        }
        for (std::size_t n = field.operand_count; n < facts::kMaxOperandsPerField; ++n)
            if (field.operands[n] != 0) return std::unexpected("usage.material.unused_operand");
        // A missing raw field can have a computed value (e.g. legacy cache-hit
        // inferred from total minus misses). It must never become direct report.
        if (field.presence == facts::Presence::Missing && field.origin == facts::Origin::Reported)
            return std::unexpected("usage.material.missing_reported_field");
        if (field.validity == facts::Validity::ValidInteger) {
            if (field.origin == facts::Origin::Unknown || field.operand_count == 0)
                return std::unexpected("usage.material.valid_field_without_evidence");
            for (std::size_t n = 0; n < field.operand_count; ++n)
                if (!observation.raw_fields[field.operands[n]].integer)
                    return std::unexpected("usage.material.noninteger_operand");
            if (field.origin == facts::Origin::Reported &&
                selected[index] != *observation.raw_fields[field.operands[0]].integer)
                return std::unexpected("usage.material.reported_value_mismatch");
        }
    }
    for (const auto& anomaly : observation.anomalies) {
        if (!KnownEnum(anomaly.field, facts::Field::OutputReasoning) ||
            !KnownEnum(anomaly.code, facts::AnomalyCode::ParseIncomplete) ||
            !TextFits(anomaly.detail, facts::kMaxPathBytes)) return std::unexpected("usage.material.anomaly");
        if (anomaly.raw_field_count > facts::kMaxOperandsPerField) return std::unexpected("usage.material.anomaly_witness_limit");
        for (std::size_t n = 0; n < anomaly.raw_field_count; ++n) {
            if (anomaly.raw_fields[n] >= observation.raw_fields.size()) return std::unexpected("usage.material.anomaly_witness_index");
            for (std::size_t previous = 0; previous < n; ++previous)
                if (anomaly.raw_fields[previous] == anomaly.raw_fields[n]) return std::unexpected("usage.material.anomaly_witness_index");
        }
        for (std::size_t n = anomaly.raw_field_count; n < facts::kMaxOperandsPerField; ++n)
            if (anomaly.raw_fields[n] != 0) return std::unexpected("usage.material.unused_anomaly_witness");
        if (anomaly.affected_field_count > facts::kFieldCount)
            return std::unexpected("usage.material.anomaly_scope_limit");
        for (std::size_t n = 0; n < anomaly.affected_field_count; ++n) {
            if (!KnownEnum(anomaly.affected_fields[n], facts::Field::OutputReasoning))
                return std::unexpected("usage.material.anomaly_scope");
            for (std::size_t previous = 0; previous < n; ++previous)
                if (anomaly.affected_fields[previous] == anomaly.affected_fields[n])
                    return std::unexpected("usage.material.anomaly_scope");
        }
        for (std::size_t n = anomaly.affected_field_count; n < facts::kFieldCount; ++n)
            if (anomaly.affected_fields[n] != facts::Field::Input)
                return std::unexpected("usage.material.unused_anomaly_scope");
        if (anomaly.code == facts::AnomalyCode::ParseIncomplete &&
            anomaly.affected_field_count != facts::kFieldCount)
            return std::unexpected("usage.material.parse_scope");
        if (anomaly.code == facts::AnomalyCode::ResponseIdentityConflict) {
            if (anomaly.affected_field_count != facts::kFieldCount || anomaly.raw_field_count != 2)
                return std::unexpected("usage.material.identity_scope");
            for (std::size_t n = 0; n < 2; ++n) {
                const auto& witness = observation.raw_fields[anomaly.raw_fields[n]];
                if (witness.kind != facts::RawKind::String || witness.fingerprint.empty())
                    return std::unexpected("usage.material.identity_witness");
            }
            if (observation.raw_fields[anomaly.raw_fields[0]].fingerprint ==
                observation.raw_fields[anomaly.raw_fields[1]].fingerprint)
                return std::unexpected("usage.material.identity_witness");
        }
        if (!add_bytes(anomaly.detail.size())) return std::unexpected("usage.material.byte_limit");
    }
    for (std::size_t n = 0; n < observation.extensions.size(); ++n) {
        const auto& extension = observation.extensions[n];
        if (!TextFits(extension.namespace_name, facts::kMaxNamespaceBytes, true) || !ValidRawField(extension.field))
            return std::unexpected("usage.material.extension");
        for (std::size_t previous = 0; previous < n; ++previous) {
            const auto& other = observation.extensions[previous];
            if (other.namespace_name == extension.namespace_name && other.field.path == extension.field.path)
                return std::unexpected("usage.material.duplicate_extension");
        }
        if (!add_bytes(extension.namespace_name.size()) || !raw_bytes(extension.field))
            return std::unexpected("usage.material.byte_limit");
    }
    return {};
}

}  // namespace lubancode::api::usage_observation
