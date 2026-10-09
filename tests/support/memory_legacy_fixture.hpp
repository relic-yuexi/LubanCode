// Private native fixture: actual V2 archive and real recovery owner.
#pragma once
#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "runtime/trajectory_session.hpp"
#include "runtime/trajectory_session_impl.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/managed_session_reservation.hpp"
#include "workspace/identity.hpp"
namespace lubancode::runtime::testing {
struct MemoryDurableLegacyFixtureAccess {
    static void SetWorkflowNodeFault(TrajectorySessionLedger& ledger) {
        REQUIRE(ledger.impl_ != nullptr);
        ledger.impl_->workflow_node_start_fault = []() -> std::optional<std::string> {
            return "diagnostics.node.refused";
        };
    }
    static std::unique_ptr<TrajectorySessionLedger> OpenManaged(const std::filesystem::path& root) {
        namespace tr = ::lubancode::trajectory;
        tr::SessionManagerOptions options;
        options.workspaces_root = root/"workspaces"; options.workspace_root = root/"repo";
        options.identity = ::lubancode::workspace::MakeFallbackIdentity(options.workspace_root);
        options.launch_cwd = options.workspace_root.generic_string();
        options.lubancode_version = "test"; options.v3_system_content = "test system";
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto workspace = tr::TrajectoryDirectory::CreateWorkspace(options.workspaces_root,options.identity,now);
        REQUIRE(workspace.has_value());
        const std::string session = "20261009-120001-MAN001";
        const tr::ManagedSessionOwnership expected{"tenant-probe","project-probe",
            options.identity.workspace_key,session,1};
        auto pending = tr::ManagedSessionReservation::Reserve(
            options.workspaces_root,expected,tr::SessionManagerClock{}.LockOwner());
        REQUIRE(pending.has_value());
        const auto publication = (*pending)->PublishOwnership();
        REQUIRE(publication.knowledge == tr::ManagedSessionOwnershipPublication::Knowledge::Committed);
        auto admitted = (*pending)->Finish(); REQUIRE(admitted.has_value());
        auto manager = std::make_unique<tr::SessionManager>(options);
        auto opened = manager->LaunchManagedSession(std::move(*admitted),
            tr::ManagedSessionCreationAudit{"tenant-probe","user-probe","user","credential-probe",1});
        REQUIRE(opened.has_value());
        REQUIRE((*opened)->managed_publication != nullptr);
        REQUIRE((*opened)->managed_publication->expected == expected);
        REQUIRE((*opened)->managed_publication->publication_bytes == publication.publication_bytes);
        REQUIRE((*opened)->lock.holds());
        REQUIRE((*opened)->is_v3());
        REQUIRE((*opened)->status == tr::SessionStatus::Running);
        auto ledger = std::unique_ptr<TrajectorySessionLedger>(new TrajectorySessionLedger());
        ledger->impl_ = std::make_unique<TrajectorySessionLedger::Impl>();
        auto& owner = *ledger->impl_;
        owner.manager = std::move(manager); owner.active = owner.manager->active();
        owner.workspaces_root = options.workspaces_root;
        owner.main_run_id = owner.active->manifest.main_run_id;
        owner.lubancode_version = "test"; owner.workspace_root_text = options.launch_cwd;
        owner.v3_system_content = options.v3_system_content;
        ledger->BindV3Books_();
        return ledger;
    }
    static std::unique_ptr<TrajectorySessionLedger> Adopt(
        std::unique_ptr<trajectory::SessionManager> manager, const std::filesystem::path& root) {
        REQUIRE(manager != nullptr);
        auto* active=manager->active();
        REQUIRE(active != nullptr); REQUIRE_FALSE(active->is_v3());
        REQUIRE(active->main.has_value());
        REQUIRE(active->status==trajectory::SessionStatus::Running);
        auto ledger=std::unique_ptr<TrajectorySessionLedger>(new TrajectorySessionLedger());
        ledger->impl_=std::make_unique<TrajectorySessionLedger::Impl>();
        auto& owner=*ledger->impl_;
        owner.manager=std::move(manager); owner.active=owner.manager->active();
        owner.workspaces_root=root/"workspaces";
        owner.main_run_id=owner.active->manifest.main_run_id;
        owner.lubancode_version="test"; owner.workspace_root_text=(root/"repo").generic_string();
        owner.recorder_options.event_schema_version=1;
        return ledger;
    }
};
}

namespace lubancode::runtime::testing {
namespace lmb = ::lubancode;
namespace fs = std::filesystem;
using Json = nlohmann::json;
inline std::unique_ptr<lmb::runtime::TrajectorySessionLedger> OpenRecoveredMemoryLegacyLedger(const fs::path& root) {
    namespace tr=lmb::trajectory;
    tr::SessionManagerOptions options;
    options.workspaces_root=root/"workspaces"; options.workspace_root=root/"repo";
    options.identity=lmb::workspace::MakeFallbackIdentity(root/"repo");
    options.launch_cwd=(root/"repo").generic_string(); options.lubancode_version="test";
    options.recorder.event_schema_version=1;
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto room=tr::TrajectoryDirectory::CreateWorkspace(options.workspaces_root,options.identity,now);
    REQUIRE(room.has_value());
    const std::string old_id="20261009-010001-R00001", next_id="20261009-010002-R00002";
    auto manifest=[&](const std::string& session,const std::string& run) {
        tr::SessionManifest value;
        value.schema_version=2; value.workspace_key=options.identity.workspace_key;
        value.session_id=session; value.main_run_id=run; value.launch_cwd=options.launch_cwd;
        value.run_kind=tr::RunKindName(tr::RunKind::MainSession); value.start_reason="process_launch";
        value.status="preparing"; value.created_at_ms=now; value.lubancode_version="test";
        value.event_schema_version=1; return value;
    };
    auto old=tr::TrajectoryDirectory::CreateSession(options.workspaces_root,options.identity.workspace_key,
                                                   manifest(old_id,"main-0001"));
    REQUIRE(old.has_value());
    auto next_manifest=manifest(next_id,"main-0002");
    next_manifest.start_reason="clear"; next_manifest.previous_session_id=old_id;
    auto next=tr::TrajectoryDirectory::CreateSession(options.workspaces_root,options.identity.workspace_key,next_manifest);
    REQUIRE(next.has_value());
    {
        // A real V2 archive of a switch interrupted after old-session sealing.
        // Recovery owns creating/adopting the next main; the fixture never does.
        tr::EventScope scope;
        scope.workspace_key=options.identity.workspace_key; scope.session_id=old_id; scope.run_id="main-0001";
        scope.run_kind=tr::RunKind::MainSession; scope.actor=tr::Actor::Host; scope.origin=tr::Origin::ScheduledHost;
        scope.visibility={tr::Visibility::HostOnly}; scope.training_policy=tr::TrainingPolicy::Exclude;
        auto recorder=tr::TrajectoryRecorder::Start(old->main_stream_path(),old->artifacts_root(),scope,options.recorder);
        REQUIRE(recorder.has_value());
        REQUIRE(recorder->WriteRunStarted(Json{{"start_reason","process_launch"}},tr::Durability::PowerLoss).status==tr::RecordReceipt::Status::Committed);
        tr::RecordRequest requested;
        requested.kind=tr::EventKind::ControlCommandRequested; requested.scope=recorder->base_scope();
        requested.scope.actor=tr::Actor::User; requested.scope.origin=tr::Origin::ExternalUser;
        requested.payload=Json{{"command_id","cmd-clear-identity"},{"command_name","clear"},
            {"action_name","clear"},{"effect_class","session_boundary"},
            {"args_ref",Json{{"boundary_operation_id","memory-identity-boundary"}}}};
        requested.links.correlation_id="memory-identity-boundary";
        REQUIRE(recorder->Record(requested,tr::Durability::PowerLoss).status==tr::RecordReceipt::Status::Committed);
        tr::RecordRequest clear;
        clear.kind=tr::EventKind::SessionClearRequested; clear.scope=recorder->base_scope();
        clear.payload=Json{{"next_session_id",next_id},{"reason","user_clear"}};
        clear.links.correlation_id="memory-identity-boundary";
        REQUIRE(recorder->Record(clear,tr::Durability::PowerLoss).status==tr::RecordReceipt::Status::Committed);
        REQUIRE(recorder->FinishRun(tr::EventKind::RunCompleted,"clear",tr::Durability::PowerLoss).status==tr::RecordReceipt::Status::Committed);
        REQUIRE(recorder->EndSession("clear",next_id,"clean",tr::Durability::PowerLoss).status==tr::RecordReceipt::Status::Committed);
        REQUIRE(recorder->Close().has_value());
        auto sealed=tr::ReadSessionJson(old->session_dir()); REQUIRE(sealed.has_value()); sealed->status="closed";
        REQUIRE(tr::WriteSessionJsonAtomic(old->session_dir(),*sealed).has_value());
    }
    auto recovered=std::make_unique<tr::SessionManager>(options);
    const auto report=recovered->RecoverWorkspace(tr::ClearRecoveryPolicy::CompleteSwitch);
    REQUIRE(report.adopted_session_id==next_id);
    REQUIRE(recovered->active()!=nullptr); REQUIRE_FALSE(recovered->active()->is_v3());
    REQUIRE(recovered->active()->main.has_value());
    REQUIRE(tr::VerifyJournalFile(old->main_stream_path()).ok);
    REQUIRE(tr::VerifyJournalFile(next->main_stream_path()).ok);
    return lmb::runtime::testing::MemoryDurableLegacyFixtureAccess::Adopt(std::move(recovered),root);
}
}  // namespace lubancode::runtime::testing
