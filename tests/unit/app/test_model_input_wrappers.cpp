#include <doctest/doctest.h>

#include <iostream>
#include <memory>

#include "api/backend.hpp"
#include "cli/spinner_backend.hpp"
#include "cli/theme.hpp"
#include "runtime/assembly/backend.hpp"

namespace {
namespace api = lubancode::api;
struct InputProbe final : api::Backend {
    enum class Mode { Available, Unavailable, Error } mode = Mode::Available;
    mutable unsigned projections = 0, serializations = 0;
    std::expected<std::optional<api::ModelInputSnapshot>, std::string>
    PrepareModelInput(const api::Request& request) const override {
        ++projections;
        if (mode == Mode::Unavailable) return std::optional<api::ModelInputSnapshot>{};
        if (mode == Mode::Error) return std::unexpected("fixture.input_error");
        return std::optional<api::ModelInputSnapshot>{api::ModelInputSnapshot{
            nlohmann::json{{"system", request.system}, {"messages", nlohmann::json::array()}},
            api::kSdkModelRequestInputScope, api::kSdkGenerateOutputLimitScope}};
    }
    std::string SerializeForDiagnostics(const api::Request&) const override { ++serializations; return {}; }
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>&, const std::atomic<bool>*) override { return {}; }
};
} // namespace

TEST_CASE("model input wrappers: Spinner and nested Rebuildable forward scope and all three outcomes") {
    std::cout << "[model-input-wrappers-path] spinner-three-state\n";
    auto inner = std::make_shared<InputProbe>();
    lubancode::runtime::assembly::RebuildableBackend stable(inner);
    const lubancode::cli::Theme theme{};
    lubancode::cli::SpinnerBackend direct(*inner, theme, false), nested(stable, theme, false);
    api::Request request; request.system = "actual input";
    for (auto* wrapper : {static_cast<api::Backend*>(&direct), static_cast<api::Backend*>(&nested)}) {
        inner->mode = InputProbe::Mode::Available;
        const auto projected = wrapper->PrepareModelInput(request); REQUIRE(projected.has_value()); REQUIRE(projected->has_value());
        REQUIRE((**projected).input.at("system") == request.system);
        REQUIRE((**projected).scope == api::kSdkModelRequestInputScope);
        REQUIRE((**projected).output_limit_scope == api::kSdkGenerateOutputLimitScope);
        inner->mode = InputProbe::Mode::Unavailable;
        const auto unavailable = wrapper->PrepareModelInput(request); REQUIRE(unavailable.has_value()); REQUIRE_FALSE(unavailable->has_value());
        inner->mode = InputProbe::Mode::Error;
        const auto error = wrapper->PrepareModelInput(request); REQUIRE_FALSE(error.has_value()); REQUIRE(error.error() == "fixture.input_error");
    }
    REQUIRE(inner->projections == 6); REQUIRE(inner->serializations == 0);
}
