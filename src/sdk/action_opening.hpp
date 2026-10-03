#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <lubancore/extensions.hpp>

#include "trajectory/opening.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore::detail {

// Declaration-only value owner. No instance, factory, writer or Agent borrow.
class SessionActionOpening final : public std::enable_shared_from_this<SessionActionOpening> {
public:
    static Result<std::shared_ptr<SessionActionOpening>> Prepare(
        const std::vector<extensions::v1::Registration>& registrations,
        std::filesystem::path owned_root, const std::string& workspace_key,
        std::string resume_id);
    bool enabled() const { return enabled_; }
    const std::string& plan() const { return plan_; }
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();

private:
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger&) const;
    std::expected<nlohmann::json, std::string> Open(const lubancode::trajectory::V3OpeningContext&);
    bool enabled_ = false;
    std::filesystem::path owned_root_, resume_dir_;
    std::string resume_id_, plan_, saved_bytes_, plan_hash_;
};

} // namespace lubancore::detail
