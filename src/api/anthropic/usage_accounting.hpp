#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>

#include "api/usage_wire_builder.hpp"

namespace lubancode::api::usage_wire {

// Four independent cumulative fields, bounded independently of frame count.
// Missing fields preserve previous observations; present zero overwrites them.
class AnthropicAccounting {
public:
    void Absorb(const nlohmann::json& usage, const std::vector<facts::RawField>* lexical = nullptr) {
        for (std::size_t i=0;i<keys_.size();++i) {
            const auto* value=Find(usage,{keys_[i]});
            if (!value) continue;
            auto raw=CaptureScalar(std::string("usage.")+keys_[i],*value,lexical);
            if (raw.integer) last_valid_[i]=raw;
            latest_[i]=std::move(raw);
        }
    }

    std::expected<Snapshot,std::string_view> View() const {
        Snapshot snapshot;snapshot.observation.provider_namespace="anthropic.messages";
        for (std::size_t i=0;i<keys_.size();++i) {
            if (!latest_[i]) continue;
            auto& field=snapshot.observation.fields[i];
            field.presence=facts::Presence::Present;
            const auto index=static_cast<std::uint16_t>(snapshot.observation.raw_fields.size());
            snapshot.observation.raw_fields.push_back(*latest_[i]);
            field.operands[0]=index;field.operand_count=1;
            // Keep the old CLI last-valid numeric behavior after a malformed
            // update. Precise consumers see the latest field as invalid, and
            // the retained raw integer is separate historical material.
            if (last_valid_[i]) snapshot.values[i]=*last_valid_[i]->integer;
            if (latest_[i]->integer) {
                field.validity=facts::Validity::ValidInteger;
                field.origin=facts::Origin::Reported;
                if (*latest_[i]->integer<0)
                    Note(snapshot,i,index,facts::AnomalyCode::Negative,"negative cumulative accounting scalar");
            } else {
                const bool range=latest_[i]->kind==facts::RawKind::SignedInteger ||
                                 latest_[i]->kind==facts::RawKind::UnsignedInteger;
                field.validity=range ? facts::Validity::OutOfRange : facts::Validity::InvalidType;
                Note(snapshot,i,index,range ? facts::AnomalyCode::OutOfRange : facts::AnomalyCode::InvalidType,
                     "latest cumulative field is invalid; retained numeric slot is historical");
                if (last_valid_[i])
                    snapshot.observation.extensions.push_back(facts::Extension{"lubancode.retained.accounting",*last_valid_[i]});
            }
        }
        // Reasoning is never synthesized from thinking text. Its fifth field
        // remains Missing/Unknown even when the other four are explicitly zero.
        const auto valid=usage_observation::Validate(snapshot.observation,snapshot.values);
        if (!valid) snapshot.material_error = valid.error();
        return snapshot;
    }
private:
    static void Note(Snapshot& snapshot,std::size_t field,std::uint16_t raw,
                     facts::AnomalyCode code,std::string detail) {
        facts::Anomaly anomaly;
        anomaly.field=static_cast<facts::Field>(field);anomaly.code=code;anomaly.detail=std::move(detail);
        anomaly.raw_fields[0]=raw;anomaly.raw_field_count=1;
        snapshot.observation.anomalies.push_back(std::move(anomaly));
    }
    static constexpr std::array<const char*,4> keys_{
        "input_tokens","output_tokens","cache_read_input_tokens","cache_creation_input_tokens"};
    std::array<std::optional<facts::RawField>,4> latest_;
    std::array<std::optional<facts::RawField>,4> last_valid_;
};

}  // namespace lubancode::api::usage_wire
