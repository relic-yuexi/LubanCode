#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <lubancore/jobs.hpp>
#include "trajectory/opening.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore::detail {

class SessionCommandJobPlan final : public std::enable_shared_from_this<SessionCommandJobPlan> {
public:
    static Result<std::shared_ptr<SessionCommandJobPlan>> Prepare(
        const std::optional<jobs::v1::CommandOptions>& requested,
        std::filesystem::path root, std::string workspace_key, std::string resume_id,
        std::string cwd);
    bool enabled() const { return options_.has_value(); }
    const std::optional<jobs::v1::CommandOptions>& options() const { return options_; }
    const std::string& cwd() const { return cwd_; }
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();
private:
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger&) const;
    std::expected<nlohmann::json, std::string> Open(const lubancode::trajectory::V3OpeningContext&);
    std::filesystem::path root_, resume_dir_;
    std::string resume_id_, cwd_, saved_bytes_, plan_, hash_;
    bool legacy_ = false;
    std::optional<jobs::v1::CommandOptions> options_;
};

Result<void> ValidateCommandJobOptions(const jobs::v1::CommandOptions&);

} // namespace lubancore::detail
