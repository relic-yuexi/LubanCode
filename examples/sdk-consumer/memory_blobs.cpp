#include <lubancore/core.hpp>
#include <lubancore/memory_blobs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace blob = sdk::memory_blobs::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error("memory-blobs: " + message); }
template<class T> T Take(sdk::Result<T> value, const std::string& action) {
    if (!value) throw std::runtime_error("memory-blobs: " + action + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const std::string& action) {
    if (!value) throw std::runtime_error("memory-blobs: " + action + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto value = path.generic_u8string(); return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path Path(const std::string& value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
struct Entity { blob::Reference reference; std::string bytes; };
struct State {
    std::mutex mutex;
    std::map<std::string, Entity> entities;
    std::vector<sdk::ModelRequest> requests;
    std::vector<blob::Scope> scopes;
    std::atomic<unsigned> writes{0}, reads{0}, opens{0}, stores_retired{0}, providers_retired{0};
};
std::string Key(const blob::Reference& reference) {
    return reference.scope.workspace_key + "/" + reference.scope.session_id + "/" + reference.sha256;
}
class Store final : public blob::Store {
public:
    Store(std::shared_ptr<State> state, blob::Scope scope) : state_(std::move(state)), scope_(std::move(scope)) {}
    ~Store() override { ++state_->stores_retired; }
    blob::WriteReceipt Write(blob::WriteRequest request) override {
        ++state_->writes;
        Check(request.reference.scope == scope_ && request.reference.bytes == request.bytes.size() &&
            request.reference.sha256.size() == 64 && request.reference.media_type == "text/plain" &&
            request.required_durability == blob::Durability::ProcessCrash, "wrong actual write identity");
        std::lock_guard lock(state_->mutex);
        state_->entities[Key(request.reference)] = {request.reference, request.bytes};
        return {blob::CommitState::Committed, std::move(request.reference), blob::Durability::ProcessCrash, {}};
    }
    sdk::Result<std::string> Read(blob::Reference reference, std::size_t cap) override {
        ++state_->reads;
        if (reference.scope != scope_ || reference.bytes > cap)
            return std::unexpected(sdk::Error{"fixture.read_owner", "wrong scope or cap"});
        std::lock_guard lock(state_->mutex);
        const auto found = state_->entities.find(Key(reference));
        if (found == state_->entities.end() || found->second.reference != reference || found->second.bytes.size() > cap)
            return std::unexpected(sdk::Error{"fixture.read_missing", "no complete owned entity"});
        return found->second.bytes;
    }
private:
    std::shared_ptr<State> state_;
    blob::Scope scope_;
};
class Provider final : public blob::Provider {
public:
    explicit Provider(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Provider() override { ++state_->providers_retired; }
    sdk::Result<std::unique_ptr<blob::Store>> Open(blob::Scope scope) override {
        Check(!scope.workspace_key.empty() && !scope.session_id.empty(), "empty actual scope");
        ++state_->opens;
        { std::lock_guard lock(state_->mutex); state_->scopes.push_back(scope); }
        return std::unique_ptr<blob::Store>(std::make_unique<Store>(state_, std::move(scope)));
    }
private:
    std::shared_ptr<State> state_;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        if (cancel.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        std::lock_guard lock(state_->mutex); state_->requests.push_back(request);
        Check(std::any_of(request.messages.begin(), request.messages.end(), [](const auto& message) {
            return message.role == "user" && message.text.find("BLOB_INSTALLED_MARKER") != std::string::npos;
        }), "actual recalled context absent from provider request");
        Check(std::none_of(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "memory_save"; }),
            "CAS selection enabled memory_save");
        return sdk::ModelReply{"installed-blob-answer", {}, std::nullopt};
    }
private:
    std::shared_ptr<State> state_;
};
sdk::SessionOptions Options(const fs::path& root, const std::shared_ptr<State>& state) {
    sdk::SessionOptions options;
    options.cwd = Utf8(root / "project"); options.model = "installed-blob-model";
    options.system_prompt = "Use the explicit project preference.";
    options.backend = std::make_unique<Backend>(state);
    options.memory_blob_provider = std::make_unique<Provider>(state);
    return options;
}
sdk::memory::v1::RecallReport Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key, const std::string& query) {
    const auto receipt = Take(session->Submit(key, query), "submit");
    const auto operation = Take(session->WaitResult(receipt.operation_id, 30s), "wait");
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted &&
        operation.final_text == "installed-blob-answer", "real operation failed: " + operation.error);
    const auto report = Take(session->GetMemoryRecall(receipt.operation_id), "read report");
    Check(report.session_id == session->id() && report.operation_id == receipt.operation_id && report.turn_id == operation.turn_id,
        "report belongs to another operation");
    return report;
}
} // namespace

void MemoryBlobs(const fs::path& base) {
    static std::atomic<unsigned> serial{0};
    fs::path root;
    fs::create_directories(base);
    for (unsigned i = 0; i < 64; ++i) {
        auto candidate = base / ("memory-blob-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++serial));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) { root = std::move(candidate); break; }
        if (ec) throw std::runtime_error(ec.message());
    }
    Check(!root.empty(), "cannot own fresh consumer state");
    for (const auto* name : {"project", "resources"}) fs::create_directories(root / name);
    auto state = std::make_shared<State>();
    auto runtime = Take(sdk::Runtime::Create({Utf8(root / "data"), Utf8(root / "resources")}), "create Runtime");
    auto options = Options(root, state); options.memory = sdk::memory::v1::RecallOptions{8192, 1};
    auto session = Take(runtime->OpenSession(std::move(options)), "open recall with public CAS");
    const auto plan = Take(session->DescribeMemory(), "describe recall");
    Check(plan.enabled && plan.session_id == session->id(), "recall plan missing");
    const auto topic = Path(plan.memory_directory) / "preferences" / "preference.sdk-blob-consumer.md";
    fs::create_directories(topic.parent_path());
    std::ofstream out(topic, std::ios::binary | std::ios::trunc); Check(out.is_open(), "cannot write project seed");
    out << "---\nname: sdk-blob-consumer\ndescription: SDKBLOBCONSUMER preference\nmetadata:\n"
        "  schema: 3\n  node_type: memory\n  type: preference\n  id: preference.sdk-blob-consumer\n"
        "  confidence: user-stated\n  status: active\n  scope: {level: project, kind: project, value: ''}\n"
        "  keywords: [SDKBLOBCONSUMER]\n  evidence: []\n  fingerprints: {}\n---\n\n"
        "# SDKBLOBCONSUMER preference\n\nSDKBLOBCONSUMER BLOB_INSTALLED_MARKER " << std::string(1500, 'x') << '\n';
    out.close(); Check(!out.fail(), "cannot close project seed");
    const auto report = Turn(session, "blob", "SDKBLOBCONSUMER preference");
    Check(report.state == "admitted" && report.error.empty() && report.context_sha256.size() == 64 &&
        !report.context_message_id.empty() && report.entries.size() == 1 && report.entries.front().selected,
        "actual recall context was not adopted");
    Check(state->writes == 1 && state->reads >= 2 && state->opens == 1, "Store/Read did not run once through actual owner");
    Entity entity;
    {
        std::lock_guard lock(state->mutex); Check(state->entities.size() == 1, "duplicated or missing CAS entity");
        entity = state->entities.begin()->second;
        Check(state->requests.size() == 1 && state->scopes.size() == 1 &&
            state->scopes.front() == entity.reference.scope, "wrong request/scope count");
        const auto& messages = state->requests.front().messages;
        Check(std::count_if(messages.begin(), messages.end(), [&](const auto& message) {
            return message.role == "user" && message.text.find(entity.bytes) != std::string::npos;
        }) == 1, "owned CAS bytes were not adopted exactly once");
    }
    Check(entity.bytes.size() > 512 && entity.reference.bytes == entity.bytes.size() &&
        entity.reference.scope.session_id == session->id() && entity.reference.scope.workspace_key == plan.workspace_key &&
        report.entries.front().bytes == entity.bytes.size(), "fragment identity does not match owned report");
    const auto directory = Path(plan.memory_directory).parent_path() / "sessions" / session->id();
    Check(!fs::exists(directory / "artifacts" / "sha256"), "custom CAS silently fell back to File");
    const auto id = session->id(); Take(session->Close(), "close first Session");
    Check(state->stores_retired == 1 && state->providers_retired == 1, "first provider captures remain live");
    const auto reads = state->reads.load();
    auto resume = Options(root, state); resume.resume_session_id = id; resume.system_prompt.clear();
    auto restored = Take(runtime->OpenSession(std::move(resume)), "resume with public CAS");
    const auto old = Take(restored->GetMemoryRecall(report.operation_id), "read restored report");
    Check(old.session_id == report.session_id && old.operation_id == report.operation_id && old.turn_id == report.turn_id &&
        old.context_message_id == report.context_message_id && old.context_sha256 == report.context_sha256 &&
        old.workspace_key == report.workspace_key && old.plan_sha256 == report.plan_sha256 && old.state == "admitted",
        "same-ID resume changed the historical adopted report");
    Check(state->reads > reads && state->writes == 1, "resume rewrote rather than verifying historical CAS");
    const auto next = Turn(restored, "next", "continue the existing context");
    Check(next.state == "no_match" && state->writes == 1, "continuation rewrote an old fragment");
    Take(restored->Close(), "close resumed Session"); Take(runtime->Shutdown(), "shutdown Runtime");
    Check(state->opens == 2 && state->stores_retired == 2 && state->providers_retired == 2,
        "provider/Store ownership did not retire across both actual Sessions");
    std::cout << "[sdk-memory-blob-consumer] actual-owned-store" << std::endl;
}
} // namespace lubancore_consumer
