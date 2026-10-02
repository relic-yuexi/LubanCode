#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/extensions.hpp>

#include "hooks/dispatcher.hpp"
#include "runtime/assembly/session_resources.hpp"

namespace lubancore::detail {

// Sole ownership moves into SessionResources. Agent/turn callbacks only borrow
// this module; resources release it after Agent, before registry/MCP/backend.
class SessionExtensions final : public lubancode::runtime::assembly::SessionResourceAttachment {
public:
    static Result<std::unique_ptr<SessionExtensions>> Build(
        std::vector<extensions::v1::Registration>& registrations,
        const extensions::v1::SessionContext& context,
        std::optional<std::string> expected_plan_json = std::nullopt);
    ~SessionExtensions() override;
    SessionExtensions(const SessionExtensions&) = delete;
    SessionExtensions& operator=(const SessionExtensions&) = delete;

    lubancode::hooks::HookDispatcher* dispatcher() const { return dispatcher_.get(); }
    // Worker-only writes at turn boundaries; observers read a synchronized copy.
    void SetOperationScope(std::string operation_id);
    std::string DescribePlan() const;

private:
    explicit SessionExtensions(extensions::v1::SessionContext context);
    std::string OperationScope() const;
    extensions::v1::SessionContext context_;
    mutable std::mutex scope_mutex_;
    std::string operation_id_;
    // Explicit shutdown clears dispatcher closures/sinks before instances. Both
    // members still exist while user destructors query session diagnostics.
    std::vector<std::unique_ptr<extensions::v1::Instance>> instances_;
    std::unique_ptr<lubancode::hooks::HookDispatcher> dispatcher_;
    std::string plan_json_;
};

} // namespace lubancore::detail
