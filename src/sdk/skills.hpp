#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>

#include <lubancore/api.hpp>
#include <lubancore/skills.hpp>

#include "tools/skill_loader.hpp"
#include "tools/tool.hpp"
#include "trajectory/opening.hpp"

namespace lubancore::detail {

// Initialization-only value owner. The opening callback owns this value; tools
// own independent metadata copies. No Agent, writer or callback is borrowed.
class SessionSkills final : public std::enable_shared_from_this<SessionSkills> {
public:
    static Result<std::shared_ptr<SessionSkills>> Prepare(
        const std::optional<skills::v1::Selection>& selection,
        std::filesystem::path owned_root, std::string workspace_key,
        std::string resume_id, std::string user_system);
    Result<void> BindToolSurface(std::set<std::string> names);
    bool enabled() const { return snapshot_.enabled; }
    std::unique_ptr<lubancode::tools::Tool> BuildTool() const;
    std::string EffectiveSystem() const;
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();
    skills::v1::Snapshot Describe() const { return snapshot_; }

private:
    using Json = nlohmann::json;
    Json Plan(const std::string& session_id) const;
    Result<void> CompareSaved(bool include_tools) const;
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger& ledger) const;
    std::expected<Json, std::string> Open(const lubancode::trajectory::V3OpeningContext& context);

    std::filesystem::path owned_root_;
    std::filesystem::path expected_resume_dir_;
    std::string resume_id_;
    std::string user_system_;
    std::string saved_bytes_;
    Json saved_plan_;
    bool legacy_ = false;
    bool surface_bound_ = false;
    skills::v1::Snapshot snapshot_;
    std::optional<lubancode::tools::StrictSkillScanResult> scan_;
    std::set<std::string> tool_names_;
};

} // namespace lubancore::detail
