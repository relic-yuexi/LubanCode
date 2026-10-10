#pragma once

#include <doctest/doctest.h>
#include "api/assembler.hpp"
#include "api/responses/events.hpp"
#include "api/sse_framing.hpp"
#include "api/usage_aggregation.hpp"

namespace lubancode::api::usage_fixture {
// Existing fixtures assert the legacy body-event sequence. Validate the added
// nonterminal fact channel separately, then keep their original body assertions.
// New fact/identity fixtures call the actual parser directly, without projection.
inline std::vector<StreamEvent> LegacyBody(std::vector<StreamEvent> actual) {
    std::vector<StreamEvent> body;
    MessageAssembler facts;
    for (auto& event : actual) {
        if (const auto* snapshot = std::get_if<UsageSnapshot>(&event)) {
            REQUIRE(snapshot->usage_observation);
            CHECK(usage_observation::Validate(*snapshot->usage_observation,
                                              usage_aggregation::Values(snapshot->usage)).has_value());
            facts.Feed(event);
            CHECK(facts.stop_reason().empty()); CHECK(facts.BuildMessage().content.empty());
        } else if (const auto* identity = std::get_if<ProviderResponseIdentity>(&event)) {
            CHECK(usage_observation::TextFits(identity->id, ::lubancore::usage::v1::kMaxResponseIdBytes, true));
            const bool was_seen = facts.usage_seen();
            const auto before = usage_aggregation::Values(facts.usage());
            facts.Feed(event);
            CHECK(facts.usage_seen() == was_seen);
            CHECK(usage_aggregation::Values(facts.usage()) == before);
            CHECK(facts.stop_reason().empty());
        } else body.push_back(std::move(event));
    }
    return body;
}
template <typename Parser>
std::vector<StreamEvent> ConsumeLegacyBody(Parser& parser, const SseFrame& frame) {
    return LegacyBody(parser.Consume(frame));
}
inline std::vector<StreamEvent> ExpandNonStreamLegacyBody(const std::string& body) {
    return LegacyBody(responses::ExpandNonStreamResponse(body));
}
}  // namespace lubancode::api::usage_fixture
