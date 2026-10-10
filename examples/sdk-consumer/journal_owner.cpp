// Installed public SDK + standard library only. The host does not assemble a
// Writer, SessionManager, replay engine, file anchor or journal hash chain.
#include <lubancore/core.hpp>
#include <lubancore/results.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace results = sdk::results::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error("journal-owner: " + reason); }
template<class T> T Take(sdk::Result<T> value, const char* phase) {
    if (!value) throw std::runtime_error(std::string("journal-owner: ") + phase + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* phase) {
    if (!value) throw std::runtime_error(std::string("journal-owner: ") + phase + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto bytes = path.u8string(); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); Check(input.is_open(), "missing actual host file");
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Check(!input.bad(), "actual file read failed"); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); Check(output.is_open(), "host evidence open failed");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.close();
    Check(!output.fail(), "host evidence write failed");
}
std::size_t Count(const std::string& bytes, const std::string& needle) {
    std::size_t count = 0, at = 0;
    while ((at = bytes.find(needle, at)) != std::string::npos) { ++count; at += needle.size(); }
    return count;
}
fs::path MainFile(const fs::path& state, const std::string& id) {
    fs::path found;
    for (const auto& entry : fs::recursive_directory_iterator(state / "workspaces")) {
        if (!entry.is_directory() || entry.path().filename() != id || entry.path().parent_path().filename() != "sessions") continue;
        Check(found.empty(), "ambiguous actual Session folder"); found = entry.path() / (id + ".jsonl");
    }
    Check(!found.empty() && fs::is_regular_file(found), "actual main Journal was not published"); return found;
}
struct Counts {
    std::atomic<unsigned> models{0}, tools{0}, backends{0}, destroyed{0};
    std::mutex mutex;
    std::vector<sdk::ModelRequest> requests;
};
class Backend final : public sdk::Backend {
public:
    Backend(std::shared_ptr<Counts> counts, std::string tag) : counts_(std::move(counts)), tag_(std::move(tag)) { ++counts_->backends; }
    ~Backend() override { --counts_->backends; ++counts_->destroyed; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        Check(!cancellation.requested(), "unexpected cancellation");
        { std::lock_guard lock(counts_->mutex); counts_->requests.push_back(request); }
        ++counts_->models;
        if (calls_++ == 0)
            return sdk::ModelReply{{}, {{"host-call-" + tag_, "journal_fixture", "{}"}}, sdk::Usage{2, 1}};
        Check(calls_ == 2, "host unexpectedly replayed or repeated model execution");
        bool returned = false;
        for (const auto& message : request.messages)
            for (const auto& result : message.tool_replies) returned = returned || result.text == "JOURNAL_TOOL_" + tag_;
        Check(returned, "real tool result did not enter the second model request");
        return sdk::ModelReply{"JOURNAL_ANSWER_" + tag_, {}, sdk::Usage{3, 2}};
    }
private:
    std::shared_ptr<Counts> counts_;
    std::string tag_;
    unsigned calls_ = 0;
};
struct Fixture {
    fs::path root, project, resources, state;
    explicit Fixture(const fs::path& base) {
        Check(base.is_absolute(), "host root must be explicit and absolute");
        static std::atomic<unsigned> serial{0};
        root = base / ("journal-owner-host-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        project = root / "project"; resources = root / "resources"; state = root / "state";
        fs::create_directories(project); fs::create_directories(resources); project = fs::canonical(project);
    }
    // State belongs to the caller, and stays available for actual CI/native
    // receipt verification after this SDK-only host exits. The caller removes it.
    std::unique_ptr<sdk::Runtime> Runtime() const {
        return Take(sdk::Runtime::Create({Utf8(state), Utf8(resources)}), "Runtime::Create");
    }
    sdk::SessionOptions Options(const std::shared_ptr<Counts>& counts, const std::string& tag,
        const std::string& resume = {}) const {
        sdk::SessionOptions options; options.cwd = Utf8(project); options.model = "journal-host-model";
        if (resume.empty()) options.system_prompt = "JOURNAL_HOST_SYSTEM";
        options.resume_session_id = resume; options.backend = std::make_unique<Backend>(counts, tag);
        sdk::Tool tool; tool.name = "journal_fixture"; tool.description = "A host-owned journal acceptance callback";
        tool.requires_approval = false;
        tool.execute = [counts, tag](const std::string&, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(!context.cancellation.requested(), "unexpected tool cancellation"); ++counts->tools;
            return sdk::ToolResult{"JOURNAL_TOOL_" + tag, false};
        };
        options.custom_tools.push_back(std::move(tool)); return options;
    }
};
sdk::Receipt Turn(const std::shared_ptr<sdk::Session>& session, const std::string& tag) {
    auto receipt = Take(session->Submit("journal-key-" + tag, "JOURNAL_USER_" + tag), "Submit");
    const auto operation = Take(session->WaitResult(receipt.operation_id, 30s), "WaitResult");
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted, "actual turn did not persist");
    Check(operation.final_text == "JOURNAL_ANSWER_" + tag, "model result changed"); return receipt;
}
std::string RequestText(const sdk::ModelRequest& request) {
    std::string text;
    for (const auto& message : request.messages) {
        text += message.text + "\n";
        for (const auto& reply : message.tool_replies) text += reply.text + "\n";
    }
    return text;
}
bool Digest(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
const results::ToolResultChannel& Combined(const results::SavedSnapshot& snapshot) {
    Check(snapshot.result().channels.size() == 1, "plain host callback acquired extra saved channels");
    const auto& channel = snapshot.result().channels.front();
    Check(channel.channel == "combined", "plain host callback lost its combined channel"); return channel;
}
struct InvocationMaterials { results::SavedSnapshot raw, formal; };
InvocationMaterials SavedInvocation(const std::shared_ptr<sdk::Session>& session,
    const sdk::Receipt& receipt, const std::string& tag) {
    struct Materials { std::optional<results::SavedSnapshot> raw, formal; };
    std::map<std::pair<std::string, std::uint64_t>, Materials> invocations;
    std::set<std::string> persisted_ids, result_ids;
    const auto operation = Take(session->ReadOperation(receipt.operation_id), "saved invocation operation");
    Check(operation.operation_id == receipt.operation_id && !operation.turn_id.empty() &&
        operation.state == sdk::OperationState::Succeeded && operation.result_persisted,
        "saved invocation lost its completed owning operation");
    const auto list = Take(session->ListToolResults(receipt.operation_id), "saved invocation list");
    Check(list.size() == 2, "one host invocation must retain raw capture and formal result");
    for (const auto& summary : list) {
        Check(summary.tool_name == "journal_fixture" && summary.selected && summary.attempt == 1 &&
            summary.identity.session_id == session->id() && summary.identity.operation_id == receipt.operation_id &&
            summary.identity.turn_id == operation.turn_id && !summary.identity.tool_call_id.empty() &&
            !summary.identity.persisted_event_id.empty() && !summary.identity.result_id.empty(),
            "saved host material crossed its Session operation turn or actual attempt");
        Check(persisted_ids.insert(summary.identity.persisted_event_id).second &&
            result_ids.insert(summary.identity.result_id).second, "saved host material repeated a durable identity");
        auto snapshot = Take(session->ReadToolResult(summary.identity), "saved invocation material");
        const auto& data = snapshot.result();
        Check(data.summary.identity == summary.identity && data.summary.attempt == summary.attempt &&
            data.summary.selected == summary.selected && data.summary.tool_name == summary.tool_name &&
            !data.execution_event_id.empty() && data.result_kind == "text" &&
            data.metadata_state == results::ArtifactState::Verified && Digest(data.metadata_sha256) &&
            data.metadata_bytes > 0 && data.content_present && !data.structured_content_present &&
            snapshot.policy() == results::SessionResultPolicy{session->id(), results::Mode::Preview, 1},
            "saved host material changed its identity execution metadata or default preview policy");
        const auto& channel = Combined(snapshot); const auto text = "JOURNAL_TOOL_" + tag;
        Check(channel.state == results::ArtifactState::Verified && channel.artifact_verified &&
            channel.artifact_id == summary.identity.result_id + "-combined" && channel.media_type == "text/plain" &&
            channel.encoding == "utf-8" && Digest(channel.sha256) && channel.text && *channel.text == text &&
            channel.captured_bytes == text.size() && channel.output_bytes == text.size() &&
            !channel.output_bytes_lower_bound && channel.capture_complete && channel.capture_reason.empty() &&
            channel.issue_code.empty(), "saved host material differs from the real callback bytes or capture facts");
        auto& materials = invocations[{summary.identity.tool_call_id, summary.attempt}];
        if (summary.identity.result_id.starts_with("capture-")) {
            Check(!materials.raw, "one host attempt repeated its raw capture"); materials.raw = std::move(snapshot);
        } else if (summary.identity.result_id.starts_with("res-")) {
            Check(!materials.formal, "one host attempt repeated its formal result"); materials.formal = std::move(snapshot);
        } else Check(false, "host invocation has an unknown saved result identity");
    }
    Check(invocations.size() == 1, "one host callback became several durable invocations");
    auto& materials = invocations.begin()->second;
    Check(materials.raw && materials.formal, "host invocation lost raw or formal saved material");
    Check(materials.raw->result().execution_event_id == materials.formal->result().execution_event_id &&
        Combined(*materials.raw).text == Combined(*materials.formal).text &&
        Combined(*materials.raw).sha256 == Combined(*materials.formal).sha256,
        "raw capture and formal result disagree about actual execution or bytes");
    return {std::move(*materials.raw), std::move(*materials.formal)};
}
void SameSaved(const results::SavedSnapshot& actual, const results::SavedSnapshot& original) {
    const auto& a = actual.result(); const auto& b = original.result();
    Check(actual.policy() == original.policy() && a.summary.identity == b.summary.identity &&
        a.summary.attempt == b.summary.attempt && a.summary.selected == b.summary.selected &&
        a.summary.tool_name == b.summary.tool_name && a.execution_event_id == b.execution_event_id &&
        a.result_kind == b.result_kind && a.metadata_state == b.metadata_state && a.metadata_sha256 == b.metadata_sha256 &&
        a.metadata_bytes == b.metadata_bytes && a.content_present == b.content_present &&
        a.structured_content_present == b.structured_content_present,
        "saved raw/formal metadata or owning identity changed on readback");
    const auto& ac = Combined(actual); const auto& bc = Combined(original);
    Check(ac.channel == bc.channel && ac.artifact_id == bc.artifact_id && ac.media_type == bc.media_type &&
        ac.encoding == bc.encoding && ac.sha256 == bc.sha256 && ac.captured_bytes == bc.captured_bytes &&
        ac.output_bytes == bc.output_bytes && ac.output_bytes_lower_bound == bc.output_bytes_lower_bound &&
        ac.capture_complete == bc.capture_complete && ac.capture_reason == bc.capture_reason &&
        ac.state == bc.state && ac.artifact_verified == bc.artifact_verified && ac.text == bc.text &&
        ac.issue_code == bc.issue_code, "saved raw/formal bytes or capture identity changed on readback");
}
void SameInvocation(const InvocationMaterials& actual, const InvocationMaterials& original) {
    SameSaved(actual.raw, original.raw); SameSaved(actual.formal, original.formal);
}
void RetainedInvocation(const std::shared_ptr<sdk::Session>& session, const InvocationMaterials& original) {
    SameSaved(Take(session->ReadToolResult(original.raw.result().summary.identity), "closed raw capture"), original.raw);
    SameSaved(Take(session->ReadToolResult(original.formal.result().summary.identity), "closed formal result"), original.formal);
}
void InvocationReceipt(std::ostream& output, const char* prefix, const InvocationMaterials& materials) {
    for (const auto* snapshot : {&materials.raw, &materials.formal}) {
        const auto& data = snapshot->result(); const auto& identity = data.summary.identity;
        const std::string field = std::string(prefix) + (identity.result_id.starts_with("capture-") ? "_raw_" : "_formal_");
        output << field << "session=" << identity.session_id << "\n" << field << "operation=" << identity.operation_id << "\n"
            << field << "turn=" << identity.turn_id << "\n" << field << "tool_call=" << identity.tool_call_id << "\n"
            << field << "attempt=" << data.summary.attempt << "\n" << field << "execution=" << data.execution_event_id << "\n"
            << field << "persisted=" << identity.persisted_event_id << "\n" << field << "result=" << identity.result_id << "\n"
            << field << "metadata_sha256=" << data.metadata_sha256 << "\n" << field << "metadata_bytes=" << data.metadata_bytes << "\n"
            << field << "combined_sha256=" << Combined(*snapshot).sha256 << "\n"
            << field << "combined_bytes=" << Combined(*snapshot).captured_bytes << "\n";
    }
}
}

// Returns the real retained File path to the private native verifier. This path
// is a host-owned acceptance artifact, never an SDK storage-provider option.
std::string JournalOwnerCase(const std::string& path, const fs::path& base) {
    Check(path == "roundtrip" || path == "close-read" || path == "isolation", "unknown acceptance path");
    Fixture fixture(base); auto counts = std::make_shared<Counts>(); auto runtime = fixture.Runtime();
    auto first = Take(runtime->OpenSession(fixture.Options(counts, "OLD")), "first OpenSession");
    const auto sid = first->id(); const auto old = Turn(first, "OLD");
    const auto original = SavedInvocation(first, old, "OLD");
    const auto file = MainFile(fixture.state, sid); const auto before_close = Bytes(file);
    Take(first->Close(), "first Close"); Take(first->Close(), "repeated first Close");
    const auto prefix = Bytes(file); Check(prefix.size() > before_close.size() && prefix.starts_with(before_close), "Close rewrote the main prefix");
    // Raw acceptance artifact only. The running SDK never reads this copy.
    Write(fixture.root / "journal-owner-before.jsonl", prefix);
    Check(counts->models == 2 && counts->tools == 1 && counts->backends == 0 && counts->destroyed == 1,
        "first Session retained a live backend or replayed work");
    Take(runtime->Shutdown(), "first Shutdown"); runtime.reset();
    std::shared_ptr<sdk::Session> closed = path == "close-read" ? first : nullptr;
    std::weak_ptr<sdk::Session> weak_first = first; first.reset();
    if (!closed) Check(weak_first.expired(), "closed first public Session remained rooted");
    if (closed) {
        RetainedInvocation(closed, original);
        Check(Take(closed->ReadOperation(old.operation_id), "closed operation").state == sdk::OperationState::Succeeded,
            "closed read retained a live operation");
    }
    auto next_runtime = fixture.Runtime();
    auto resumed = Take(next_runtime->OpenSession(fixture.Options(counts, "NEW", sid)), "fresh Runtime same-ID OpenSession");
    Check(resumed->id() == sid, "same-ID resume replaced the Session");
    Check(counts->models == 2 && counts->tools == 1, "opening replayed model/tool work");
    const auto duplicate = Take(resumed->Submit("journal-key-OLD", "JOURNAL_USER_OLD"), "recovered duplicate");
    Check(duplicate.duplicate && duplicate.operation_id == old.operation_id, "same-ID restoration lost durable operation identity");
    Check(Take(resumed->ReadOperation(old.operation_id), "restored operation").state == sdk::OperationState::Succeeded,
        "restored operation was reclassified");
    const auto restored = SavedInvocation(resumed, old, "OLD"); SameInvocation(restored, original);
    Check(counts->models == 2 && counts->tools == 1, "duplicate redispatched actual work");
    const auto next = Turn(resumed, "NEW"); Check(next.operation_id != old.operation_id, "new operation reused old identity");
    const auto newest = SavedInvocation(resumed, next, "NEW");
    Check(newest.formal.result().summary.identity.turn_id != original.formal.result().summary.identity.turn_id &&
        newest.formal.result().summary.identity.tool_call_id != original.formal.result().summary.identity.tool_call_id &&
        newest.formal.result().execution_event_id != original.formal.result().execution_event_id,
        "new host callback reused the old durable invocation");
    Check(counts->models == 4 && counts->tools == 2, "second complete host turn missing");
    { std::lock_guard lock(counts->mutex); Check(counts->requests.size() == 4, "request count differs from actual model calls");
      const auto history = RequestText(counts->requests.at(2));
      Check(Count(history, "JOURNAL_USER_OLD") == 1 && Count(history, "JOURNAL_ANSWER_OLD") == 1 &&
            Count(history, "JOURNAL_TOOL_OLD") == 1 && Count(history, "JOURNAL_USER_NEW") == 1,
          "same-ID request omitted or duplicated captured history"); }
    if (path == "isolation") {
        auto other_counts = std::make_shared<Counts>();
        auto other = Take(next_runtime->OpenSession(fixture.Options(other_counts, "OTHER")), "same-project second Session");
        Check(other->id() != sid, "same-project Session identity collided"); const auto other_receipt = Turn(other, "OTHER");
        (void)SavedInvocation(other, other_receipt, "OTHER");
        { std::lock_guard lock(other_counts->mutex);
          for (const auto& request : other_counts->requests) {
              const auto text = RequestText(request); Check(text.find("JOURNAL_USER_OLD") == std::string::npos &&
                  text.find("JOURNAL_USER_NEW") == std::string::npos, "same-project Sessions shared history");
          } }
        Take(other->Close(), "isolated Close"); Check(other_counts->models == 2 && other_counts->tools == 1 && other_counts->backends == 0,
            "isolated Session did not finish its owned work");
        Check(counts->models == 4 && counts->tools == 2, "second Session changed original execution counts");
    }
    Take(resumed->Close(), "resumed Close"); Take(next_runtime->Shutdown(), "fresh Runtime Shutdown"); next_runtime.reset();
    Check(counts->backends == 0 && counts->destroyed == 2, "fresh Runtime retained its backend after Close");
    const auto after = Bytes(file); Check(after.starts_with(prefix), "same-ID continuation changed any captured old byte");
    Check(after.size() > prefix.size(), "same-ID host produced no new main rows");
    if (closed) {
        RetainedInvocation(closed, original);
        closed.reset(); Check(weak_first.expired(), "old closed Session acquired a live owner on resume");
    }
    // This receipt describes executed SDK work and raw retained artifacts. CI
    // independently verifies actual canonical/seq/hash/run facts from the files.
    std::ofstream receipt(fixture.root / "journal-owner-host-receipt.txt", std::ios::binary);
    Check(receipt.is_open(), "host receipt open failed");
    receipt << "session_id=" << sid << "\nprefix_bytes=" << prefix.size() << "\nafter_bytes=" << after.size()
            << "\nmodel_calls=" << counts->models.load() << "\ntool_calls=" << counts->tools.load()
            << "\nold_operation=" << old.operation_id << "\nnew_operation=" << next.operation_id << "\n";
    InvocationReceipt(receipt, "old", original); InvocationReceipt(receipt, "new", newest);
    receipt.close(); Check(!receipt.fail(), "host receipt write failed");
    std::cout << "[sdk-journal-owner-path] " << path << "\n";
    std::cout << "[sdk-journal-owner-host] session=" << sid << " prefix=" << prefix.size() << " after=" << after.size()
              << " models=" << counts->models.load() << " tools=" << counts->tools.load() << std::endl;
    return Utf8(file);
}
void JournalOwner(const fs::path& base) {
    for (const auto& path : {"roundtrip", "close-read", "isolation"}) (void)JournalOwnerCase(path, base);
}
} // namespace lubancore_consumer
