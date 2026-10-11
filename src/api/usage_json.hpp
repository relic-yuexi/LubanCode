#pragma once

#include <expected>
#include <string_view>
#include <nlohmann/json.hpp>
#include "api/types.hpp"
#include "api/usage_aggregation.hpp"

namespace lubancode::api::usage_json {
namespace facts = ::lubancore::usage::v1;

inline const nlohmann::json* Member(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}
inline bool Unsigned(const nlohmann::json* value, std::uint64_t limit) {
    if (!value) return false;
    if (value->is_number_unsigned()) return value->get<std::uint64_t>() <= limit;
    return value->is_number_integer() && value->get<std::int64_t>() >= 0 &&
           static_cast<std::uint64_t>(value->get<std::int64_t>()) <= limit;
}
inline bool Signed(const nlohmann::json* value) {
    return value && ((value->is_number_unsigned() &&
        value->get<std::uint64_t>() <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) ||
        (value->is_number_integer() && !value->is_number_unsigned()));
}

// The first pass borrows strings and arrays. No material string/vector is copied
// until every count, enum, integer, index and byte budget has been admitted.
inline std::expected<facts::Observation, std::string_view> Decode(
    const nlohmann::json& encoded, const Usage& selected) {
    if (!Unsigned(Member(encoded, "version"), 2) || *Member(encoded, "version") < 1)
        return std::unexpected("usage.json.version");
    std::size_t bytes = 0;
    const auto text = [&](const nlohmann::json* value, std::size_t limit, bool required) {
        if (!value) return !required;
        if (!value->is_string()) return false;
        const auto& borrowed = value->get_ref<const std::string&>();
        if (!usage_observation::TextFits(borrowed, limit, required) ||
            borrowed.size() > facts::kMaxMaterialBytes - bytes) return false;
        bytes += borrowed.size(); return true;
    };
    const auto array = [](const nlohmann::json* value, std::size_t limit) {
        return value && value->is_array() && value->size() <= limit;
    };
    const auto* fields = Member(encoded, "fields");
    const auto* raw = Member(encoded, "raw_fields");
    const auto* anomalies = Member(encoded, "anomalies");
    const auto* extensions = Member(encoded, "extensions");
    if (!text(Member(encoded, "provider_namespace"), facts::kMaxNamespaceBytes, true) ||
        !array(fields, facts::kFieldCount) || fields->size() != facts::kFieldCount ||
        !array(raw, facts::kMaxRawFields) || !array(anomalies, facts::kMaxAnomalies) ||
        !array(extensions, facts::kMaxExtensions)) return std::unexpected("usage.json.shape");
    const auto admit_raw = [&](const nlohmann::json& field) {
        const auto* fingerprint = Member(field, "fingerprint");
        const auto* integer = Member(field, "integer");
        const auto* kind = Member(field, "kind");
        if (!text(Member(field, "path"), facts::kMaxPathBytes, true) ||
            !text(Member(field, "summary"), facts::kMaxSummaryBytes, false) ||
            !text(fingerprint, 64, false) ||
            (fingerprint && !usage_observation::FingerprintFits(fingerprint->get_ref<const std::string&>())) ||
            !Unsigned(kind, static_cast<unsigned>(facts::RawKind::Array))) return false;
        if (integer && (!Signed(integer) ||
            (*kind != static_cast<unsigned>(facts::RawKind::SignedInteger) &&
             *kind != static_cast<unsigned>(facts::RawKind::UnsignedInteger)) ||
            (*kind == static_cast<unsigned>(facts::RawKind::UnsignedInteger) && integer->get<std::int64_t>() < 0)))
            return false;
        return true;
    };
    for (const auto& field : *raw) if (!admit_raw(field)) return std::unexpected("usage.json.raw");
    const auto indices = [&](const nlohmann::json* values) {
        if (!array(values, facts::kMaxOperandsPerField)) return false;
        for (std::size_t i = 0; i < values->size(); ++i) {
            if (raw->empty() || !Unsigned(&(*values)[i], raw->size() - 1)) return false;
            for (std::size_t j = 0; j < i; ++j) if ((*values)[i] == (*values)[j]) return false;
        }
        return true;
    };
    for (const auto& field : *fields) {
        if (!Unsigned(Member(field, "presence"), static_cast<unsigned>(facts::Presence::Present)) ||
            !Unsigned(Member(field, "validity"), static_cast<unsigned>(facts::Validity::Overflow)) ||
            !Unsigned(Member(field, "origin"), static_cast<unsigned>(facts::Origin::Inferred)) ||
            !indices(Member(field, "operands"))) return std::unexpected("usage.json.field");
    }
    for (const auto& anomaly : *anomalies) {
        if (!Unsigned(Member(anomaly, "field"), facts::kFieldCount - 1) ||
            !Unsigned(Member(anomaly, "code"), static_cast<unsigned>(facts::AnomalyCode::ParseIncomplete)) ||
            !text(Member(anomaly, "detail"), facts::kMaxPathBytes, false) ||
            !indices(Member(anomaly, "raw_fields"))) return std::unexpected("usage.json.anomaly");
        if (const auto* affected = Member(anomaly, "affected_fields")) {
            if (!array(affected, facts::kFieldCount)) return std::unexpected("usage.json.anomaly_scope");
            if (!affected->empty() && *Member(encoded, "version") != 2)
                return std::unexpected("usage.json.anomaly_scope_version");
            for (std::size_t i = 0; i < affected->size(); ++i) {
                if (!Unsigned(&(*affected)[i], facts::kFieldCount - 1)) return std::unexpected("usage.json.anomaly_scope");
                for (std::size_t j = 0; j < i; ++j)
                    if ((*affected)[i] == (*affected)[j]) return std::unexpected("usage.json.anomaly_scope");
            }
        }
    }
    for (const auto& extension : *extensions) {
        const auto* field = Member(extension, "field");
        if (!text(Member(extension, "namespace"), facts::kMaxNamespaceBytes, true) ||
            !field || !admit_raw(*field)) return std::unexpected("usage.json.extension");
    }
    const auto copy_text = [](const nlohmann::json& object, const char* key) {
        const auto* value = Member(object, key);
        return value ? value->get<std::string>() : std::string{};
    };
    const auto copy_raw = [&](const nlohmann::json& field) {
        facts::RawField value;
        value.path = copy_text(field, "path");
        value.kind = static_cast<facts::RawKind>(field.at("kind").get<unsigned>());
        if (const auto* integer = Member(field, "integer")) value.integer = integer->get<std::int64_t>();
        value.summary = copy_text(field, "summary"); value.fingerprint = copy_text(field, "fingerprint");
        return value;
    };
    facts::Observation result;
    result.provider_namespace = copy_text(encoded, "provider_namespace");
    for (const auto& field : *raw) result.raw_fields.push_back(copy_raw(field));
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        const auto& field = (*fields)[i]; auto& value = result.fields[i];
        value.presence = static_cast<facts::Presence>(field.at("presence").get<unsigned>());
        value.validity = static_cast<facts::Validity>(field.at("validity").get<unsigned>());
        value.origin = static_cast<facts::Origin>(field.at("origin").get<unsigned>());
        const auto& operands = field.at("operands");
        value.operand_count = static_cast<std::uint8_t>(operands.size());
        for (std::size_t n = 0; n < operands.size(); ++n) value.operands[n] = operands[n].get<std::uint16_t>();
    }
    for (const auto& anomaly : *anomalies) {
        facts::Anomaly value;
        value.field = static_cast<facts::Field>(anomaly.at("field").get<unsigned>());
        value.code = static_cast<facts::AnomalyCode>(anomaly.at("code").get<unsigned>());
        value.detail = copy_text(anomaly, "detail");
        const auto& witnesses = anomaly.at("raw_fields");
        value.raw_field_count = static_cast<std::uint8_t>(witnesses.size());
        for (std::size_t n = 0; n < witnesses.size(); ++n) value.raw_fields[n] = witnesses[n].get<std::uint16_t>();
        if (const auto* affected = Member(anomaly, "affected_fields")) {
            value.affected_field_count = static_cast<std::uint8_t>(affected->size());
            for (std::size_t n = 0; n < affected->size(); ++n)
                value.affected_fields[n] = static_cast<facts::Field>((*affected)[n].get<unsigned>());
        }
        result.anomalies.push_back(std::move(value));
    }
    for (const auto& extension : *extensions)
        result.extensions.push_back({copy_text(extension, "namespace"), copy_raw(extension.at("field"))});
    const auto valid = usage_observation::Validate(result, usage_aggregation::Values(selected));
    if (!valid) return std::unexpected(valid.error());
    return result;
}

inline void Restore(UsageReport& report, const nlohmann::json& payload) {
    report.usage_observation.reset();
    report.usage_observation_error.clear();
    if (const auto* error = Member(payload, "usage_observation_error")) {
        if (!error->is_string() || !usage_observation::TextFits(error->get_ref<const std::string&>(), facts::kMaxPathBytes))
            report.usage_observation_error = "usage.json.error_invalid";
        else report.usage_observation_error = error->get<std::string>();
        // A reported material failure cannot be overridden by a second object.
        if (report.usage_observation_error.empty()) report.usage_observation_error = "usage.json.material_unavailable";
        return;
    }
    const auto* material = Member(payload, "usage_observation");
    if (!material) return;  // Legacy absence remains unknown, including nonzero numbers.
    auto decoded = Decode(*material, report.usage);
    if (decoded) report.usage_observation = std::move(*decoded);
    else report.usage_observation_error = std::string(decoded.error());
}

inline nlohmann::json Raw(const facts::RawField& raw) {
    nlohmann::json result{{"path", raw.path}, {"kind", static_cast<unsigned>(raw.kind)}};
    if (raw.integer) result["integer"] = *raw.integer;
    if (!raw.summary.empty()) result["summary"] = raw.summary;
    if (!raw.fingerprint.empty()) result["fingerprint"] = raw.fingerprint;
    return result;
}

// Admission rechecks the borrowed bounded material before allocating JSON.
// The selected numbers stay in the enclosing UsageReport, never in this object.
inline std::expected<nlohmann::json, std::string_view> Encode(
    const facts::Observation& observation, const Usage& selected) {
    const auto admitted = usage_observation::Validate(observation, usage_aggregation::Values(selected));
    if (!admitted) return std::unexpected(admitted.error());
    nlohmann::json result{{"version", 1}, {"provider_namespace", observation.provider_namespace}};
    auto fields = nlohmann::json::array();
    for (const auto& field : observation.fields) {
        auto operands = nlohmann::json::array();
        for (std::size_t i = 0; i < field.operand_count; ++i) operands.push_back(field.operands[i]);
        fields.push_back({{"presence", static_cast<unsigned>(field.presence)},
                          {"validity", static_cast<unsigned>(field.validity)},
                          {"origin", static_cast<unsigned>(field.origin)},
                          {"operands", std::move(operands)}});
    }
    result["fields"] = std::move(fields);
    auto raw = nlohmann::json::array();
    for (const auto& field : observation.raw_fields) raw.push_back(Raw(field));
    result["raw_fields"] = std::move(raw);
    auto anomalies = nlohmann::json::array();
    for (const auto& anomaly : observation.anomalies) {
        auto witnesses = nlohmann::json::array();
        for (std::size_t i = 0; i < anomaly.raw_field_count; ++i) witnesses.push_back(anomaly.raw_fields[i]);
        anomalies.push_back({{"field", static_cast<unsigned>(anomaly.field)},
                             {"code", static_cast<unsigned>(anomaly.code)},
                             {"detail", anomaly.detail}, {"raw_fields", std::move(witnesses)}});
        if (anomaly.affected_field_count != 0) {
            // Older material readers must reject this semantic scope extension,
            // rather than silently treating a multi-field anomaly as one field.
            result["version"] = 2;
            auto affected = nlohmann::json::array();
            for (std::size_t n = 0; n < anomaly.affected_field_count; ++n)
                affected.push_back(static_cast<unsigned>(anomaly.affected_fields[n]));
            anomalies.back()["affected_fields"] = std::move(affected);
        }
    }
    result["anomalies"] = std::move(anomalies);
    auto extensions = nlohmann::json::array();
    for (const auto& extension : observation.extensions)
        extensions.push_back({{"namespace", extension.namespace_name}, {"field", Raw(extension.field)}});
    result["extensions"] = std::move(extensions);
    return result;
}

// Shared source material only. Bridge owners decide identity, durability and wake.
inline std::expected<nlohmann::json, std::string> ObservationPayload(
    const Usage& usage, const facts::Observation* observation, bool reported,
    std::string_view response_id, bool incomplete) {
    if (response_id.size() > facts::kMaxResponseIdBytes || response_id.find('\0') != response_id.npos ||
        !usage_observation::TextFits(std::string(response_id), facts::kMaxResponseIdBytes))
        return std::unexpected("invalid usage response identity");
    nlohmann::json payload = {{"version", 1}, {"numbers", usage_aggregation::Values(usage)},
        {"reportedByProvider", reported}, {"incomplete", incomplete},
        {"providerResponseId", response_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(response_id)}};
    if (observation) {
        auto material = Encode(*observation, usage);
        if (material) payload["observation"] = std::move(*material);
        else { payload["incomplete"] = true; payload["observationError"] = material.error(); }
    }
    return payload;
}

}  // namespace lubancode::api::usage_json
