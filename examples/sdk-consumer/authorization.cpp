#include <lubancore/authorization.hpp>
#include <lubancore/core.hpp>
#include <lubancore/managed.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
namespace auth = lubancore::authorization::v1;
void CheckManagedStorageConsumer(const std::filesystem::path&);

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T>
T Take(lubancore::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(*result);
}
bool Allowed(const std::shared_ptr<auth::PolicyProvider>& provider,
             const auth::ExecutionContext& context, auth::Action action) {
    const auto decision = auth::Authorize(provider, context, action);
    return decision && decision->allowed;
}

class ThrowingPolicy final : public auth::PolicyProvider {
public:
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        throw std::runtime_error("PRIVATE_PROVIDER_SECRET");
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope&, auth::PolicyChangeCallback) override {
        throw std::runtime_error("PRIVATE_PROVIDER_SECRET");
    }
};
class InvalidDecisionPolicy final : public auth::PolicyProvider {
public:
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        return auth::Decision{true, 0, "allow"};
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope&, auth::PolicyChangeCallback) override {
        return std::unique_ptr<auth::PolicySubscription>{};
    }
};
class InvalidChangePolicy final : public auth::PolicyProvider {
public:
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        return auth::Decision{false, 1, "denied"};
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope& scope, auth::PolicyChangeCallback callback) override {
        auto foreign = scope;
        foreign.tenant_id = "other-tenant";
        foreign.session_id = "private-other-session";
        callback({foreign, 9});
        callback({scope, 0});
        return auth::PolicySubscription::Create([] {});
    }
};
} // namespace

// This translation unit is copied with the consumer and uses only the installed
// public SDK. It tests exported symbols and provider virtual dispatch after the
// producer prefix has moved; it does not claim Managed Session enforcement.
void CheckSdkAuthorizationConsumer(const std::filesystem::path& base) {
    auto policy = auth::RevocablePolicy::Create();
    Require(static_cast<bool>(policy), "missing reference policy");
    const auth::AuthenticatedSubject alice{"tenant-a", "alice", auth::ActorKind::User, "credential-a"};
    const auth::AuthenticatedSubject bob{"tenant-a", "bob", auth::ActorKind::User, "credential-b"};
    const auth::ResourceScope first{"tenant-a", "same-project", "same-workspace", "session-a"};
    const auth::ResourceScope second{"tenant-a", "same-project", "same-workspace", "session-b"};
    auth::ExecutionContext context{alice, first, std::nullopt, 1};
    Take(policy->BindProject({"tenant-a", "same-project", "same-workspace", 1}));
    Take(policy->BindProject({"tenant-b", "same-project", "same-workspace", 1}));
    Require(!Allowed(policy, context, auth::Action::ReadOperation), "implicit administrator permission");
    Require(!Allowed({}, context, auth::Action::ReadOperation), "missing provider allowed access");
    auto missing = context;
    missing.request_actor.credential_id.clear();
    Require(!Allowed(policy, missing, auth::Action::ReadOperation), "missing subject allowed access");

    auto project = context;
    project.resource.session_id.clear();
    Take(policy->Grant(alice, project.resource, {auth::Action::OpenSession}));
    Require(Allowed(policy, project, auth::Action::OpenSession), "project-scoped create grant denied");
    Require(!Allowed(policy, project, auth::Action::ReadOperation), "project scope read Session data");
    Require(!Allowed(policy, context, auth::Action::ReadOperation), "project grant became a Session wildcard");

    Take(policy->Grant(alice, first, {auth::Action::ReadOperation, auth::Action::ReadToolResult}));
    Require(Allowed(policy, context, auth::Action::ReadToolResult), "explicit grant did not work");
    auto other = context;
    other.resource = second;
    Require(!Allowed(policy, other, auth::Action::ReadToolResult), "grant crossed Session boundary");
    other.resource = {"tenant-b", "same-project", "same-workspace", "session-a"};
    Require(!Allowed(policy, other, auth::Action::ReadToolResult), "grant crossed tenant boundary");
    other = context;
    other.request_actor.credential_id = "rotated-credential";
    Require(!Allowed(policy, other, auth::Action::ReadToolResult), "grant ignored credential identity");

    std::atomic<unsigned> notices{0};
    std::atomic<bool> wrong_scope{false};
    auto subscription = Take(auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange& change) {
        if (change.scope != first || change.revision == 0) wrong_scope.store(true);
        notices.fetch_add(1);
    }));
    Require(static_cast<bool>(subscription), "reference policy returned no subscription");
    Take(policy->Revoke(alice, first, {auth::Action::ReadToolResult}));
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "revoked grant remained usable");
    Require(notices.load() > 0 && !wrong_scope.load(), "revocation notification was missing or leaked scope");
    subscription->Unsubscribe();
    subscription->Unsubscribe();
    const auto stopped_notices = notices.load();
    Take(policy->Grant(alice, first, {auth::Action::ReadToolResult}));
    Require(notices.load() == stopped_notices, "unsubscribed callback still fired");
    subscription.reset();

    Take(policy->SetTenantAdministrator(alice, true));
    Require(Allowed(policy, context, auth::Action::ReadOperation), "explicit tenant administrator denied");
    auth::ExecutionScope execution{first, alice, policy->CurrentRevision(), "execution-a", 0,
                                  {auth::Action::RequestModel}};
    context.execution = execution;
    Require(Allowed(policy, context, auth::Action::RequestModel), "authorized initiating subject denied");
    Require(!Allowed(policy, context, auth::Action::DispatchTool), "execution capabilities widened permission");
    auto query = context;
    query.request_actor = bob;
    Require(!Allowed(policy, query, auth::Action::ReadOperation), "query borrowed initiating subject permission");
    Require(!Allowed(policy, query, auth::Action::RequestModel), "dispatch changed initiating subject");
    Take(policy->SetTenantAdministrator(alice, false));
    Require(!Allowed(policy, context, auth::Action::RequestModel), "saved revision became a cached permit");

    Take(policy->BindProject({"tenant-a", "same-project", "same-workspace", 2}));
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "stale project binding was accepted");
    context.execution.reset();
    context.project_binding_version = 2;
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "old grant survived project rebinding");
    Take(policy->UnbindProject("tenant-a", "same-project", "same-workspace"));
    Require(!policy->BindProject({"tenant-a", "same-project", "same-workspace", 1}),
            "unbound project accepted an old binding version");
    Take(policy->BindProject({"tenant-a", "same-project", "same-workspace", 3}));
    context.project_binding_version = 3;
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "unbound grant revived on rebinding");

    auto throwing = std::make_shared<ThrowingPolicy>();
    const auto rejected = auth::Authorize(throwing, context, auth::Action::ReadOperation);
    Require(!rejected, "provider exception granted permission");
    Require(rejected.error().message.find("PRIVATE_PROVIDER_SECRET") == std::string::npos,
            "provider exception text leaked");
    const auto rejected_watch = auth::SubscribeChanges(throwing, first, [](const auth::PolicyChange&) {});
    Require(!rejected_watch, "subscription exception was accepted");
    Require(rejected_watch.error().message.find("PRIVATE_PROVIDER_SECRET") == std::string::npos,
            "subscription exception text leaked");
    auto malformed = std::make_shared<InvalidDecisionPolicy>();
    Require(!Allowed(malformed, context, auth::Action::ReadOperation), "zero-revision allow was accepted");
    Require(!auth::SubscribeChanges(malformed, first, [](const auth::PolicyChange&) {}),
            "null subscription was accepted");
    unsigned invalidations = 0;
    bool invalid_notification = false;
    auto invalidating = std::make_shared<InvalidChangePolicy>();
    auto guarded = Take(auth::SubscribeChanges(invalidating, first, [&](const auth::PolicyChange& change) {
        ++invalidations;
        if (change.scope != first || change.revision != 0) invalid_notification = true;
    }));
    Require(invalidations == 2 && !invalid_notification,
            "invalid provider notification leaked scope or suppressed invalidation");
    guarded.reset();
    subscription = Take(auth::SubscribeChanges(policy, first, [](const auth::PolicyChange&) {}));
    policy.reset();
    subscription.reset(); // Subscription cleanup remains safe after provider loss.
    CheckManagedStorageConsumer(base / "managed-storage");
}

namespace {
namespace fs = std::filesystem;
namespace managed = lubancore::managed::v1;
std::string ManagedUtf8(const fs::path& path) {
    const auto text = path.u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path ManagedNative(const fs::path& path) {
#ifdef _WIN32
    const auto text = fs::absolute(path).native();
    if (text.starts_with(L"\\\\?\\")) return path;
    if (text.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + text.substr(2));
    return fs::path(L"\\\\?\\" + text);
#else
    return path;
#endif
}
std::string ManagedRead(const fs::path& path) {
    std::ifstream file(ManagedNative(path), std::ios::binary);
    Require(file.is_open(), "Managed original cannot be opened");
    std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Require(!file.bad(), "Managed original read failed");
    return bytes;
}
fs::path ManagedDirectory(const fs::path& root, const std::string& id) {
    fs::path result;
    for (const auto& entry : fs::recursive_directory_iterator(ManagedNative(root))) {
        if (!entry.is_directory() || entry.path().filename() != fs::path(id)) continue;
        Require(result.empty(), "Managed Session ID had multiple directories"); result = entry.path();
    }
    Require(!result.empty(), "Managed actual session directory absent");
    Require(fs::is_regular_file(result / "managed-session-ownership.json"), "Managed ownership original absent");
    Require(fs::is_regular_file(result / (id + ".jsonl")), "Managed V3 original absent");
    return result;
}
void CheckManagedStorageConsumer(const fs::path& base) {
    fs::create_directories(ManagedNative(base / "project"));
    fs::create_directories(ManagedNative(base / "resources"));
    auto runtime = Take(lubancore::Runtime::Create({ManagedUtf8(base / "state"), ManagedUtf8(base / "resources")}));
    auto policy = auth::RevocablePolicy::Create();
    const auth::AuthenticatedSubject actor{"consumer-tenant", "alice", auth::ActorKind::User, "consumer-credential"};
    auto project = Take(runtime->RegisterManagedProject({{"consumer-tenant", "same-project", {}, 1}, ManagedUtf8(base / "project"), policy}));
    const auto binding = project->binding();
    Take(policy->BindProject(binding));
    const auto scope = [&](const std::string& id = std::string()) {
        return auth::ResourceScope{binding.tenant_id, binding.project_id, binding.workspace_key, id};
    };
    Require(!runtime->OpenManagedSession(project, actor), "Managed implicit Open permission");
    Take(policy->Grant(actor, scope(), {auth::Action::OpenSession}));
    const auto first = Take(runtime->OpenManagedSession(project, actor));
    const auto second = Take(runtime->OpenManagedSession(project, actor));
    Require(first.session_id != second.session_id, "Managed sessions shared an identity");
    Require(!runtime->AcquireManagedView(project, actor, first.session_id), "Managed Open granted AcquireView");
    const auto grant = [&](const std::string& id) {
        Take(policy->Grant(actor, scope(id), {auth::Action::AcquireView, auth::Action::ReadSession, auth::Action::CloseSession}));
    };
    grant(first.session_id); grant(second.session_id);
    auto view = Take(runtime->AcquireManagedView(project, actor, first.session_id));
    auto other = Take(runtime->AcquireManagedView(project, actor, second.session_id));
    const auto identity = Take(view->ReadIdentity());
    Require(identity.resource == scope(first.session_id) && identity.creation_subject == actor &&
        identity.project_binding_version == binding.version && identity.opening_policy_revision > 0 &&
        identity.run_id == "main-0001" && identity.state == managed::StorageState::Open, "Managed actual identity mismatch");
    const auto first_dir = ManagedDirectory(base / "state", first.session_id);
    const auto second_dir = ManagedDirectory(base / "state", second.session_id);
    Require(first_dir.parent_path() == second_dir.parent_path(), "Managed same project sessions were placed in different workspaces");
    Require(fs::exists(first_dir / "session.lock") && fs::exists(second_dir / "session.lock"), "Managed live lock originals absent");
    const auto ownership = ManagedRead(first_dir / "managed-session-ownership.json");
    const auto main = ManagedRead(first_dir / (first.session_id + ".jsonl"));
    const auto lock = ManagedRead(first_dir / "session.lock");
    std::ofstream proof(ManagedNative(base / "opening-originals.txt"), std::ios::binary | std::ios::trunc);
    proof << "[sdk-managed-storage-ownership-original] " << ownership << '\n'
          << "[sdk-managed-storage-v3-original] " << main.substr(0, main.find('\n')) << '\n'
          << "[sdk-managed-storage-lock-original] " << lock << '\n';
    proof.close(); Require(!proof.fail(), "Managed opening evidence not saved");
    std::cout << "[sdk-managed-storage-ownership-original] " << ownership << '\n'
              << "[sdk-managed-storage-v3-original] " << main.substr(0, main.find('\n')) << '\n'
              << "[sdk-managed-storage-lock-original] " << lock << '\n';
    view.reset();
    Require(fs::exists(first_dir / "session.lock"), "dropping View closed supervised Managed storage");
    view = Take(runtime->AcquireManagedView(project, actor, first.session_id));
    auto other_project = Take(runtime->RegisterManagedProject({{"consumer-tenant", "other-project", {}, 1}, ManagedUtf8(base / "project"), policy}));
    const auto other_binding = other_project->binding(); Take(policy->BindProject(other_binding));
    auto other_scope = auth::ResourceScope{other_binding.tenant_id, other_binding.project_id, other_binding.workspace_key, {}};
    Take(policy->Grant(actor, other_scope, {auth::Action::OpenSession}));
    const auto third = Take(runtime->OpenManagedSession(other_project, actor));
    other_scope.session_id = third.session_id;
    Take(policy->Grant(actor, other_scope, {auth::Action::AcquireView, auth::Action::ReadSession, auth::Action::CloseSession}));
    auto third_view = Take(runtime->AcquireManagedView(other_project, actor, third.session_id));
    Require(Take(third_view->ReadIdentity()).resource == other_scope, "Managed other project identity mismatch");
    const auto third_dir = ManagedDirectory(base / "state", third.session_id);
    Require(third_dir.parent_path() != first_dir.parent_path(), "Managed different projects shared a storage root");
    Require(fs::exists(third_dir / "session.lock"), "Managed other project lock absent");
    const auto third_ownership = ManagedRead(third_dir / "managed-session-ownership.json");
    const auto third_main = ManagedRead(third_dir / (third.session_id + ".jsonl"));
    std::ofstream third_proof(ManagedNative(base / "other-project-originals.txt"), std::ios::binary | std::ios::trunc);
    third_proof << "[sdk-managed-storage-other-ownership-original] " << third_ownership << '\n'
                << "[sdk-managed-storage-other-v3-original] " << third_main.substr(0, third_main.find('\n')) << '\n';
    third_proof.close(); Require(!third_proof.fail(), "Managed other-project evidence not saved");
    std::cout << "[sdk-managed-storage-other-ownership-original] " << third_ownership << '\n'
              << "[sdk-managed-storage-other-v3-original] " << third_main.substr(0, third_main.find('\n')) << '\n';
    const auth::ResourceScope foreign_scope{other_binding.tenant_id, other_binding.project_id, other_binding.workspace_key, first.session_id};
    Take(policy->Grant(actor, foreign_scope, {auth::Action::AcquireView}));
    Require(!runtime->AcquireManagedView(other_project, actor, first.session_id), "Managed session crossed project binding");
    auto foreign_actor = actor; foreign_actor.tenant_id = "another-tenant";
    Require(!runtime->AcquireManagedView(project, foreign_actor, first.session_id), "Managed view crossed tenant");
    Take(policy->Revoke(actor, scope(first.session_id), {auth::Action::ReadSession, auth::Action::CloseSession}));
    Require(!view->ReadIdentity() && !view->Close(), "Managed saved View bypassed revocation");
    grant(first.session_id);
    Require(view->Close().has_value(), "Managed actual close failed");
    Require(!fs::exists(first_dir / "session.lock") && fs::exists(second_dir / "session.lock") &&
        fs::exists(third_dir / "session.lock"), "Managed close crossed session/project or retained lock");
    const auto closed = Take(view->ReadIdentity());
    Require(closed.state == managed::StorageState::Closed && closed.resource == identity.resource &&
        closed.opening_policy_revision == identity.opening_policy_revision, "Managed closed identity borrowed a live writer");
    Require(ManagedRead(first_dir / "managed-session-ownership.json") == ownership, "Managed Close rewrote ownership publication");
    Require(!runtime->AcquireManagedView(project, actor, first.session_id), "closed Managed supervisor entry retained");
    Take(policy->Revoke(actor, scope(first.session_id), {auth::Action::ReadSession}));
    Require(!view->ReadIdentity(), "closed Managed view lost current authorization");
    Require(runtime->Shutdown().has_value(), "Managed Runtime shutdown failed");
    Require(!fs::exists(second_dir / "session.lock"), "Managed Runtime shutdown retained live storage");
    Require(!fs::exists(third_dir / "session.lock"), "Managed Runtime shutdown retained other-project storage");
    Require(Take(other->ReadIdentity()).state == managed::StorageState::Closed, "Managed shutdown state not retained");
    Require(Take(third_view->ReadIdentity()).state == managed::StorageState::Closed, "Managed other project shutdown not retained");
    std::cout << "[sdk-managed-storage] {\"first\":\"" << first.session_id << "\",\"second\":\"" << second.session_id
              << "\",\"other_project_session\":\"" << third.session_id
              << "\",\"workspace\":\"" << binding.workspace_key
              << "\",\"view_drop_kept_lock\":true,\"scope_rejected\":true,\"revocation_rejected\":true,"
                 "\"first_close_released_only_first\":true,\"shutdown_released_second\":true,\"closed_read_rechecked\":true}\n";
}
} // namespace
