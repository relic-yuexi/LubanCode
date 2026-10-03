#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/api.hpp>
#include <lubancore/lua.hpp>

#include "tools/tool.hpp"
#include "trajectory/opening.hpp"

namespace lubancore::detail {

// Initialization-only owner. Each tool/VM moves exactly once into the existing
// Session registry. The opening participant owns only declaration metadata.
class SessionLua final : public std::enable_shared_from_this<SessionLua> {
public:
    static Result<std::shared_ptr<SessionLua>> Prepare(
        const std::optional<lua::v1::Selection>& selection,
        std::filesystem::path owned_root, std::string workspace_key, std::string resume_id);
    std::vector<std::string> Names() const;
    Result<std::vector<std::unique_ptr<lubancode::tools::Tool>>> TakeTools();
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();
    lua::v1::Snapshot Describe() const { return snapshot_; }

private:
    using Json = nlohmann::json;
    Json Plan(const std::string& session_id) const;
    Result<void> Load(const lua::v1::Selection& selection);
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger& ledger) const;
    std::expected<Json, std::string> Open(const lubancode::trajectory::V3OpeningContext& context);

    std::filesystem::path owned_root_, resume_dir_;
    std::string resume_id_, saved_bytes_;
    Json saved_plan_;
    bool legacy_ = false, tools_taken_ = false;
    lua::v1::Snapshot snapshot_;
    std::vector<std::unique_ptr<lubancode::tools::Tool>> tools_;
};

} // namespace lubancore::detail
