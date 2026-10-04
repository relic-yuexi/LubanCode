// Host-owned retrieval example. Only installed public SDK headers and STL.
#include <lubancore/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore;
namespace results = sdk::results::v1;
using namespace std::chrono_literals;
constexpr std::size_t kQueryBytes = 128, kInputBytes = 1024, kDocuments = 16;
constexpr std::size_t kExcerptBytes = 4096, kHits = 2, kResultBytes = 16384;
const std::string kSchema = R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z0-9 -]+$"}},"required":["query"],"additionalProperties":false})";
const std::string kPrompt =
    "Use retrieve_evidence to answer from this host's small demonstration catalog. "
    "Queries must contain ASCII English letters, digits, spaces or hyphens. "
    "You decide whether to query again. Cite returned evidence as [source:ID]. "
    "Do not invent sources; say when evidence is insufficient. The source URIs identify demo documents.";

void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error("agentic-rag: " + message);
}
template<class T> T Take(sdk::Result<T> value, const char* operation) {
    if (!value) throw std::runtime_error(std::string("agentic-rag: ") + operation + ": " +
        value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* operation) {
    if (!value) throw std::runtime_error(std::string("agentic-rag: ") + operation + ": " +
        value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string Token() {
    static std::atomic<unsigned> next{0};
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(next++);
}
struct Directory {
    fs::path path;
    explicit Directory(const fs::path& base) {
        Check(base.is_absolute(), "state root must be explicit and absolute");
        path = base / ("agentic-rag-" + Token());
        fs::create_directories(path / "project");
        fs::create_directories(path / "resources");
    }
    ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
bool Printable(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
std::string Lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// A deliberately narrow host-side JSON codec, not an SDK parser or new SPI.
// One query string, ASCII search terms; escaped ASCII is accepted too.
sdk::Result<std::string> Query(std::string_view input) {
    const auto invalid = []() -> sdk::Result<std::string> {
        return std::unexpected(sdk::Error{"example.rag.invalid_query", "Expected one bounded ASCII query field"});
    };
    if (input.size() > kInputBytes) return invalid();
    std::size_t at = 0;
    const auto space = [&] { while (at < input.size() && (input[at] == ' ' || input[at] == '\t' ||
        input[at] == '\n' || input[at] == '\r')) ++at; };
    const auto character = [&](char expected) { space(); if (at == input.size() || input[at] != expected) return false;
        ++at; return true; };
    const auto string = [&]() -> std::optional<std::string> {
        if (!character('"')) return std::nullopt;
        std::string out;
        while (at < input.size()) {
            unsigned char c = static_cast<unsigned char>(input[at++]);
            if (c == '"') return out;
            if (c == '\\') {
                if (at == input.size()) return std::nullopt;
                c = static_cast<unsigned char>(input[at++]);
                if (c == 'u') {
                    unsigned number = 0;
                    for (unsigned n = 0; n < 4; ++n) {
                        if (at == input.size()) return std::nullopt;
                        const char digit = input[at++];
                        unsigned value = 16;
                        if (digit >= '0' && digit <= '9') value = static_cast<unsigned>(digit - '0');
                        if (digit >= 'a' && digit <= 'f') value = static_cast<unsigned>(digit - 'a' + 10);
                        if (digit >= 'A' && digit <= 'F') value = static_cast<unsigned>(digit - 'A' + 10);
                        if (value == 16) return std::nullopt;
                        number = number * 16 + value;
                    }
                    if (number < 32 || number > 126) return std::nullopt;
                    c = static_cast<unsigned char>(number);
                } else if (c != '"' && c != '\\' && c != '/') return std::nullopt;
            }
            if (c < 32 || c > 126 || out.size() >= kQueryBytes) return std::nullopt;
            out.push_back(static_cast<char>(c));
        }
        return std::nullopt;
    };
    if (!character('{')) return invalid();
    auto key = string();
    if (!key || *key != "query" || !character(':')) return invalid();
    auto value = string();
    if (!value || value->empty() || !character('}')) return invalid();
    space();
    if (at != input.size() || !std::all_of(value->begin(), value->end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-';
    })) return invalid();
    if (value->find_first_not_of(' ') == std::string::npos) return invalid();
    return std::move(*value);
}

struct Evidence { std::string id, source, excerpt; };
std::vector<Evidence> Documents(const std::string& scope, const std::string& nonce, bool large = false) {
    const auto padding = large ? " " + std::string(2500, 'x') + " local-only-tail-" + nonce : "";
    return {{scope + "-transport", "example://" + scope + "/transport",
        "A worker in this demo uses explicit transport; the host owns its connection. Evidence nonce " + nonce + padding},
        {scope + "-preview", "example://" + scope + "/preview",
        "The worker returns preview by default; full needs Node permission and Session selection. Evidence nonce " + nonce + padding}};
}
std::string Encode(const std::vector<Evidence>& values) {
    std::string out = "Evidence results:\n";
    for (const auto& value : values) out += "ID: " + value.id + "\nSource: " + value.source + "\nExcerpt: " + value.excerpt + "\n";
    Check(out.size() <= kResultBytes, "host result exceeded its bound");
    return out;
}
std::vector<Evidence> Decode(std::string_view body) {
    Check(body.size() <= kResultBytes, "retrieval reply exceeded host codec budget");
    std::istringstream stream{std::string(body)};
    std::string line;
    Check(static_cast<bool>(std::getline(stream, line)) && line == "Evidence results:", "tool returned an unexpected evidence record");
    std::vector<Evidence> values;
    while (std::getline(stream, line)) {
        Check(line.starts_with("ID: ") && values.size() < kHits, "invalid evidence ID or too many hits");
        Evidence value; value.id = line.substr(4);
        Check(static_cast<bool>(std::getline(stream, line)) && line.starts_with("Source: "), "missing actual source");
        value.source = line.substr(8);
        Check(static_cast<bool>(std::getline(stream, line)) && line.starts_with("Excerpt: "), "missing actual excerpt");
        value.excerpt = line.substr(9);
        Check(Printable(value.id) && Printable(value.source) && Printable(value.excerpt), "invalid actual evidence bytes");
        values.push_back(std::move(value));
    }
    return values;
}
std::string Answer(const std::vector<Evidence>& values) {
    if (values.empty()) return "Insufficient evidence: the actual retrieval returned no source.";
    std::string out = "Answer from actual retrieved evidence:\n";
    for (const auto& value : values) out += value.excerpt + " [source:" + value.id + "] (" + value.source + ")\n";
    return out;
}
void CheckCitations(const std::string& answer, const std::vector<Evidence>& values) {
    std::set<std::string> allowed;
    for (const auto& value : values) allowed.insert(value.id);
    std::size_t at = 0, citations = 0;
    while ((at = answer.find("[source:", at)) != std::string::npos) {
        const auto end = answer.find(']', at + 8);
        Check(end != std::string::npos && allowed.contains(answer.substr(at + 8, end - at - 8)),
            "answer cited a source not returned by the actual tool");
        ++citations; at = end + 1;
    }
    Check(values.empty() ? citations == 0 : citations != 0, "answer omitted available evidence citations");
}
struct Rendezvous {
    std::mutex mutex; std::condition_variable cv; unsigned arrived = 0;
    void Meet() {
        std::unique_lock lock(mutex); ++arrived; cv.notify_all();
        Check(cv.wait_for(lock, 15s, [&] { return arrived == 2; }), "two actual retrieval callbacks did not overlap");
    }
};
struct RetrievalState {
    std::mutex mutex; std::condition_variable cv;
    std::vector<std::string> queries;
    bool block = false, entered = false, hold_after_cancel = false, cancel_observed = false, release_cancel = false;
    std::shared_ptr<Rendezvous> rendezvous;
    std::atomic<unsigned> destroyed{0}, cancelled{0};
};
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual sdk::Result<std::vector<Evidence>> Search(const std::string&, sdk::Cancellation) = 0;
};
class TextRetriever final : public Retriever {
public:
    TextRetriever(std::vector<Evidence> values, std::shared_ptr<RetrievalState> state)
        : values_(std::move(values)), state_(std::move(state)) {
        Check(!values_.empty() && values_.size() <= kDocuments, "invalid host corpus size");
        std::set<std::string> ids;
        for (const auto& value : values_) Check(Printable(value.id) && value.id.size() <= 128 &&
            value.id.find_first_of("[]") == std::string::npos && ids.insert(value.id).second &&
            Printable(value.source) && value.source.size() <= 256 && Printable(value.excerpt) &&
            value.excerpt.size() <= kExcerptBytes, "invalid host evidence record");
    }
    ~TextRetriever() override { ++state_->destroyed; }
    sdk::Result<std::vector<Evidence>> Search(const std::string& query, sdk::Cancellation cancel) override {
        std::unique_lock lock(state_->mutex);
        state_->queries.push_back(query); state_->entered = true; state_->cv.notify_all();
        auto rendezvous = state_->queries.size() == 1 ? state_->rendezvous : nullptr;
        if (rendezvous) { lock.unlock(); rendezvous->Meet(); lock.lock(); }
        if (state_->block) {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) state_->cv.wait_for(lock, 5ms);
            Check(cancel.requested(), "actual retrieval did not receive cooperative cancellation");
        }
        if (cancel.requested()) {
            state_->cancel_observed = true; state_->cv.notify_all();
            if (state_->hold_after_cancel) Check(state_->cv.wait_for(lock, 15s, [&] { return state_->release_cancel; }),
                "host did not release the actually cancelled retrieval callback");
            ++state_->cancelled;
            return std::unexpected(sdk::Error{"example.rag.cancelled", "Retrieval cancelled"});
        }
        lock.unlock();
        std::istringstream terms{Lower(query)};
        std::vector<std::string> words; std::string word;
        while (terms >> word) words.push_back(std::move(word));
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        for (std::size_t n = 0; n < values_.size(); ++n) {
            const auto text = Lower(values_[n].excerpt);
            std::size_t score = 0;
            for (const auto& term : words) if (text.find(term) != std::string::npos) ++score;
            if (score) matches.emplace_back(score, n);
        }
        std::sort(matches.begin(), matches.end(), [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : values_[a.second].id < values_[b.second].id;
        });
        std::vector<Evidence> out;
        for (const auto& item : matches) { out.push_back(values_[item.second]); if (out.size() == kHits) break; }
        return out; // Owned evidence; no pointer into corpus or borrowed cancellation.
    }
private:
    std::vector<Evidence> values_;
    std::shared_ptr<RetrievalState> state_;
};
sdk::Tool RetrievalTool(std::shared_ptr<Retriever> retriever) {
    sdk::Tool tool;
    tool.name = "retrieve_evidence"; tool.description = "Retrieve at most two sourced excerpts from the host's demo corpus.";
    tool.input_schema_json = kSchema;
    // Keep public default external-effect approval. This is not a tenant ACL.
    tool.execute = [retriever = std::move(retriever)](const std::string& input, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
        auto query = Query(input); if (!query) return std::unexpected(query.error());
        auto values = retriever->Search(*query, context.cancellation);
        if (!values) return std::unexpected(values.error());
        return sdk::ToolResult{Encode(*values), false};
    };
    return tool;
}
struct ModelState {
    std::mutex mutex;
    const std::string instance = Token();
    std::vector<sdk::ToolReply> actual_replies, expected_history;
    std::string expected_answer;
    bool history_seen = false;
    std::atomic<unsigned> requests{0}, destroyed{0}, approvals{0};
};
class FixtureBackend final : public sdk::Backend {
public:
    FixtureBackend(std::shared_ptr<ModelState> state, std::string model, std::string first = "transport")
        : state_(std::move(state)), model_(std::move(model)), first_(std::move(first)) {}
    ~FixtureBackend() override { ++state_->destroyed; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        ++state_->requests;
        Check(request.model == model_, "model crossed Sessions");
        Check(std::count_if(request.tools.begin(), request.tools.end(), [](const auto& tool) {
            return tool.name == "retrieve_evidence" && tool.input_schema_json.find("query") != std::string::npos;
        }) == 1, "actual model request lost host retrieval tool");
        if (cancel.requested()) return std::unexpected(sdk::Error{"example.rag.cancelled", "Model cancelled"});
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->expected_history.empty() && !state_->history_seen) {
                for (const auto& old : state_->expected_history) {
                    unsigned found = 0;
                    for (const auto& message : request.messages) for (const auto& reply : message.tool_replies)
                        if (reply.call_id == old.call_id && reply.text == old.text && reply.is_error == old.is_error) ++found;
                    Check(found == 1, "same-ID restore lost or duplicated actual tool history");
                }
                Check(std::any_of(request.messages.begin(), request.messages.end(), [&](const auto& message) {
                    return message.role == "assistant" && message.text == state_->expected_answer;
                }), "same-ID restore lost the actual final answer");
                state_->history_seen = true;
            }
        }
        if (finished_) { finished_ = false; ++operation_; queries_ = 0; values_.clear(); }
        if (!pending_.empty()) {
            std::optional<sdk::ToolReply> actual;
            for (const auto& message : request.messages) for (const auto& reply : message.tool_replies)
                if (reply.call_id == pending_) { Check(!actual, "duplicate actual model tool reply"); actual = reply; }
            Check(actual.has_value() && !actual->is_error, "actual retrieval reply is missing or failed");
            const auto hits = Decode(actual->text);
            for (const auto& hit : hits) if (std::none_of(values_.begin(), values_.end(), [&](const auto& value) {
                return value.id == hit.id;
            })) values_.push_back(hit);
            { std::lock_guard lock(state_->mutex); state_->actual_replies.push_back(std::move(*actual)); }
            pending_.clear();
            // The fixture chooses a follow-up only after actual evidence arrives.
            if (hits.empty() || values_.size() >= 2 || queries_ == 2) {
                finished_ = true;
                return sdk::ModelReply{Answer(values_), {}, std::nullopt};
            }
        }
        Check(queries_ < 2, "fixture requested an unbounded query loop");
        const std::string query = queries_ == 0 ? first_ : "preview";
        pending_ = "rag-" + model_ + "-" + state_->instance + "-" + std::to_string(operation_) + "-" + std::to_string(++queries_);
        return sdk::ModelReply{"", {{pending_, "retrieve_evidence", "{\"query\":\"" + query + "\"}"}}, std::nullopt};
    }
private:
    std::shared_ptr<ModelState> state_;
    std::string model_, first_, pending_;
    std::vector<Evidence> values_;
    unsigned queries_ = 0, operation_ = 0;
    bool finished_ = false;
};
std::unique_ptr<sdk::Runtime> Runtime(const fs::path& path) {
    return Take(sdk::Runtime::Create({Utf8(path / "data"), Utf8(path / "resources")}), "Runtime::Create");
}
sdk::SessionOptions Options(const fs::path& cwd, const std::shared_ptr<ModelState>& model,
    const std::string& label, const std::vector<Evidence>& documents, const std::shared_ptr<RetrievalState>& retrieval,
    std::string resume = {}, std::string first = "transport") {
    sdk::SessionOptions out;
    out.cwd = Utf8(cwd); out.model = label; out.system_prompt = kPrompt; out.resume_session_id = std::move(resume);
    out.backend = std::make_unique<FixtureBackend>(model, label, std::move(first));
    out.custom_tools.push_back(RetrievalTool(std::make_shared<TextRetriever>(documents, retrieval)));
    out.max_steps_per_turn = 6;
    // result_policy omitted: actual default Preview/v1, no Node full permission.
    return out;
}
sdk::Receipt Submit(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    return Take(session->Submit(key, "Explain worker transport and preview using sourced evidence."), "Submit");
}
bool Terminal(sdk::OperationState state) {
    return state != sdk::OperationState::Accepted && state != sdk::OperationState::Running;
}
void Approve(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<ModelState>& model,
    sdk::ApprovalDecision decision = sdk::ApprovalDecision::Accept) {
    for (const auto& approval : session->PendingApprovals()) {
        Check(approval.tool_name == "retrieve_evidence", "unexpected approval tool");
        Take(session->ResolveApproval(approval.request_id, decision), "ResolveApproval");
        ++model->approvals;
    }
}
sdk::Operation Finish(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<ModelState>& model,
    const sdk::Receipt& receipt) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        Approve(session, model);
        const auto operation = Take(session->ReadOperation(receipt.operation_id), "ReadOperation");
        if (Terminal(operation.state)) return Take(session->WaitResult(receipt.operation_id, 1s), "WaitResult");
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("agentic-rag: actual model/tool round did not finish");
}
void Succeeded(const sdk::Operation& operation, const std::vector<Evidence>& documents) {
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted,
        "model/tool operation did not persist successfully: " + operation.error);
    CheckCitations(operation.final_text, documents);
}
std::size_t Queries(const std::shared_ptr<RetrievalState>& state) {
    std::lock_guard lock(state->mutex); return state->queries.size();
}
std::vector<results::SavedSnapshot> Saved(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    std::vector<results::SavedSnapshot> out;
    for (const auto& summary : Take(session->ListToolResults(receipt.operation_id), "ListToolResults")) if (summary.selected) {
        Check(summary.tool_name == "retrieve_evidence" && summary.identity.session_id == session->id() &&
            summary.identity.operation_id == receipt.operation_id, "saved evidence crossed its owning operation");
        auto snapshot = Take(session->ReadToolResult(summary.identity), "ReadToolResult");
        Check(snapshot.result().metadata_state == results::ArtifactState::Verified &&
            snapshot.policy().mode == results::Mode::Preview && snapshot.policy().version == 1,
            "saved evidence was unverified or changed the default preview policy");
        Check(std::any_of(snapshot.result().channels.begin(), snapshot.result().channels.end(), [](const auto& channel) {
            return channel.artifact_verified && channel.capture_complete && channel.text && channel.text->starts_with("Evidence results:\n");
        }), "actual owned retrieval bytes were not captured");
        out.push_back(std::move(snapshot));
    }
    return out;
}
std::vector<Evidence> SavedEvidence(const std::vector<results::SavedSnapshot>& snapshots) {
    std::vector<Evidence> out;
    for (const auto& snapshot : snapshots) for (const auto& channel : snapshot.result().channels)
        if (channel.artifact_verified && channel.capture_complete && channel.text && channel.text->starts_with("Evidence results:\n")) {
            for (auto value : Decode(*channel.text)) if (std::none_of(out.begin(), out.end(), [&](const auto& old) { return old.id == value.id; }))
                out.push_back(std::move(value));
            break;
        }
    return out;
}
void Closed(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<ModelState>& model,
    const std::shared_ptr<RetrievalState>& retrieval) {
    Take(session->Close(), "Close"); Take(session->Close(), "repeated Close");
    Check(model->destroyed == 1 && retrieval->destroyed == 1, "Close did not retire exactly one actual model and retriever");
    Check(!session->Submit("late", "closed"), "closed Session accepted work");
}
void WaitEntered(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<ModelState>& model,
    const std::shared_ptr<RetrievalState>& retrieval) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        Approve(session, model);
        { std::lock_guard lock(retrieval->mutex); if (retrieval->entered) return; }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("agentic-rag: actual retrieval callback did not enter");
}
void Retrieval(const fs::path& base) {
    Directory directory(base); auto runtime = Runtime(directory.path);
    const auto nonce = Token(); const auto documents = Documents("retrieval", nonce);
    auto model = std::make_shared<ModelState>(); auto retrieval = std::make_shared<RetrievalState>();
    auto session = Take(runtime->OpenSession(Options(directory.path / "project", model, "retrieval", documents, retrieval)), "open retrieval");
    const auto receipt = Submit(session, "query-twice"); const auto operation = Finish(session, model, receipt);
    Succeeded(operation, documents);
    Check(Queries(retrieval) == 2 && model->requests == 3 && model->approvals == 2, "actual follow-up/approval counts differ");
    Check(operation.final_text.find(nonce) != std::string::npos, "answer did not contain freshly generated actual evidence");
    const auto saved = Saved(session, receipt); Check(saved.size() == 2, "two actual invocations were not saved");
    CheckCitations(operation.final_text, SavedEvidence(saved));
    Closed(session, model, retrieval);
    Check(Take(session->ReadOperation(receipt.operation_id), "closed answer").final_text == operation.final_text, "Close lost owned answer");
    // A broad query returns both documents. Actual results stop the fixture after
    // one query: two calls are not blindly hardcoded into every operation.
    auto fast_model = std::make_shared<ModelState>(); auto fast_retrieval = std::make_shared<RetrievalState>();
    auto fast = Take(runtime->OpenSession(Options(directory.path / "project", fast_model, "sufficient", documents,
        fast_retrieval, {}, "worker")), "open sufficient evidence");
    Succeeded(Finish(fast, fast_model, Submit(fast, "query-once")), documents);
    Check(Queries(fast_retrieval) == 1 && fast_model->requests == 2, "actual sufficient evidence did not end querying");
    Closed(fast, fast_model, fast_retrieval); Take(runtime->Shutdown(), "Shutdown");
}
void Sources(const fs::path& base) {
    Directory directory(base); auto runtime = Runtime(directory.path);
    const auto nonce = Token(); auto input = Documents("sources", nonce); const auto expected = input;
    auto model = std::make_shared<ModelState>(); auto retrieval = std::make_shared<RetrievalState>();
    auto options = Options(directory.path / "project", model, "sources", input, retrieval);
    input[0].excerpt = "mutated caller memory"; input.clear(); input.shrink_to_fit();
    auto session = Take(runtime->OpenSession(std::move(options)), "open owned sources");
    const auto receipt = Submit(session, "owned-source"); const auto operation = Finish(session, model, receipt);
    Succeeded(operation, expected); const auto saved = Saved(session, receipt); const auto values = SavedEvidence(saved);
    Check(values.size() == 2 && operation.final_text.find("mutated caller memory") == std::string::npos,
        "retriever kept a borrowed corpus or lost owned sources");
    for (const auto& value : values) {
        const auto source = std::find_if(expected.begin(), expected.end(), [&](const auto& document) { return document.id == value.id; });
        Check(source != expected.end() && source->source == value.source && source->excerpt == value.excerpt,
            "actual evidence source/body differs from host corpus");
    }
    CheckCitations(operation.final_text, values);
    bool refused = false; try { CheckCitations("unsupported [source:invented]", values); } catch (const std::runtime_error&) { refused = true; }
    Check(refused, "host checker accepted a fabricated citation");
    Closed(session, model, retrieval);
    auto missing_model = std::make_shared<ModelState>(); auto missing_retrieval = std::make_shared<RetrievalState>();
    auto missing = Take(runtime->OpenSession(Options(directory.path / "project", missing_model, "no-hit", expected,
        missing_retrieval, {}, "unlistedtopic")), "open missing evidence");
    const auto absent = Finish(missing, missing_model, Submit(missing, "no-hit"));
    Succeeded(absent, {});
    Check(absent.final_text.starts_with("Insufficient evidence") && Queries(missing_retrieval) == 1,
        "actual zero-hit retrieval fabricated a source");
    Closed(missing, missing_model, missing_retrieval);
    // The same host callback refuses bad arguments before invoking its retriever.
    auto invalid_state = std::make_shared<RetrievalState>();
    {
        auto tool = RetrievalTool(std::make_shared<TextRetriever>(expected, invalid_state));
        for (const auto& bad : std::vector<std::string>{"{}", R"({"query":"transport","query":"preview"})",
            R"({"query":"transport","extra":true})", R"({"query":"   "})", R"({"query":"../private"})",
            "{\"query\":\"" + std::string(kQueryBytes + 1, 'a') + "\"}", std::string(kInputBytes + 1, ' ')} ) {
            const auto result = tool.execute(bad, sdk::ToolContext{Utf8(directory.path / "project"), {}});
            Check(!result && result.error().code == "example.rag.invalid_query", "host callback accepted malformed query");
        }
        Check(Queries(invalid_state) == 0, "invalid host query reached actual retrieval");
        auto escaped = tool.execute(R"({"query":"\u0074ransport"})", sdk::ToolContext{Utf8(directory.path / "project"), {}});
        Check(escaped && Decode(escaped->text).size() == 1 && Queries(invalid_state) == 1, "valid escaped ASCII query was not searched");
    }
    Check(invalid_state->destroyed == 1, "host callback retained invalid-query retriever");
    Take(runtime->Shutdown(), "Shutdown");
}
void Preview(const fs::path& base) {
    Directory directory(base); auto runtime = Runtime(directory.path);
    const auto nonce = Token(); const auto documents = Documents("preview", nonce, true);
    auto model = std::make_shared<ModelState>(); auto retrieval = std::make_shared<RetrievalState>();
    auto session = Take(runtime->OpenSession(Options(directory.path / "project", model, "preview", documents, retrieval)), "open preview");
    const auto receipt = Submit(session, "preview"); Succeeded(Finish(session, model, receipt), documents);
    auto snapshots = Saved(session, receipt); Check(snapshots.size() == 2, "preview has no actual saved evidence");
    for (const auto& snapshot : snapshots) {
        const auto values = SavedEvidence({snapshot});
        Check(values.size() == 1 && values[0].excerpt.find("local-only-tail-" + nonce) != std::string::npos,
            "trusted local read lost its complete captured evidence");
        const results::NodeResultPolicy node{false, 128, "rag-preview-v1"};
        auto projector = Take(results::ResultProjector::Create(node, snapshot.policy()), "preview projector");
        const auto frozen = Take(projector->Project(snapshot), "Project");
        const auto wire = Take(frozen.ForTransmission(*projector), "ForTransmission");
        Check(wire.find("Evidence results:") != std::string::npos && wire.find("local-only-tail-" + nonce) == std::string::npos,
            "outbound default preview exposed the complete evidence tail");
        Check(wire.find(Utf8(directory.path)) == std::string::npos, "outbound preview exposed a local data path");
        auto full = snapshot.policy(); full.mode = results::Mode::Full;
        Check(!results::ResultProjector::Create(node, full), "Node denial was bypassed by Session full selection");
    }
    Closed(session, model, retrieval);
    Check(Saved(session, receipt).size() == 2, "Close lost the trusted owned saved result read");
    Take(runtime->Shutdown(), "Shutdown");
}
void Isolation(const fs::path& base) {
    Directory directory(base); auto runtime = Runtime(directory.path);
    auto a_model = std::make_shared<ModelState>(); auto b_model = std::make_shared<ModelState>();
    auto a_retrieval = std::make_shared<RetrievalState>(); auto b_retrieval = std::make_shared<RetrievalState>();
    auto rendezvous = std::make_shared<Rendezvous>(); a_retrieval->rendezvous = rendezvous; b_retrieval->rendezvous = rendezvous;
    const auto a_documents = Documents("alpha", Token()), b_documents = Documents("beta", Token());
    auto a = Take(runtime->OpenSession(Options(directory.path / "project", a_model, "alpha", a_documents, a_retrieval)), "open alpha");
    auto b = Take(runtime->OpenSession(Options(directory.path / "project", b_model, "beta", b_documents, b_retrieval)), "open beta");
    auto a_receipt = Submit(a, "first"); auto b_receipt = Submit(b, "first");
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    sdk::Operation a_operation, b_operation;
    while (std::chrono::steady_clock::now() < deadline) {
        Approve(a, a_model, sdk::ApprovalDecision::AcceptForSession); Approve(b, b_model);
        a_operation = Take(a->ReadOperation(a_receipt.operation_id), "read alpha");
        b_operation = Take(b->ReadOperation(b_receipt.operation_id), "read beta");
        if (Terminal(a_operation.state) && Terminal(b_operation.state)) break;
        std::this_thread::sleep_for(2ms);
    }
    Succeeded(a_operation, a_documents); Succeeded(b_operation, b_documents);
    Check(a_model->approvals == 1 && b_model->approvals == 2, "session approval crossed into its peer");
    Check(a_operation.final_text.find("beta-") == std::string::npos && b_operation.final_text.find("alpha-") == std::string::npos,
        "same-project Sessions shared actual corpus evidence");
    CheckCitations(a_operation.final_text, SavedEvidence(Saved(a, a_receipt)));
    CheckCitations(b_operation.final_text, SavedEvidence(Saved(b, b_receipt)));
    { std::lock_guard lock(a_retrieval->mutex); a_retrieval->block = true; a_retrieval->entered = false; }
    a_receipt = Submit(a, "cancel-only-alpha"); b_receipt = Submit(b, "beta-continues");
    WaitEntered(a, a_model, a_retrieval); Take(a->Cancel(a_receipt.operation_id), "cancel alpha");
    const auto cancelled = Finish(a, a_model, a_receipt);
    Check(cancelled.state == sdk::OperationState::Cancelled && a_retrieval->cancelled == 1,
        "alpha retrieval did not cancel through its own Session");
    Closed(a, a_model, a_retrieval);
    Succeeded(Finish(b, b_model, b_receipt), b_documents);
    Check(Queries(a_retrieval) == 3 && Queries(b_retrieval) == 4 && b_retrieval->cancelled == 0,
        "cancel/Close crossed into the healthy same-project peer");
    Closed(b, b_model, b_retrieval); Take(runtime->Shutdown(), "Shutdown");
}
void Recovery(const fs::path& base) {
    Directory directory(base); const auto documents = Documents("restore", Token());
    std::string sid, answer; sdk::Receipt receipt; std::vector<sdk::ToolReply> replies;
    {
        auto runtime = Runtime(directory.path); auto model = std::make_shared<ModelState>(); auto retrieval = std::make_shared<RetrievalState>();
        auto session = Take(runtime->OpenSession(Options(directory.path / "project", model, "seed", documents, retrieval)), "open seed");
        sid = session->id(); receipt = Submit(session, "seed-key"); const auto operation = Finish(session, model, receipt);
        Succeeded(operation, documents); answer = operation.final_text;
        { std::lock_guard lock(model->mutex); replies = model->actual_replies; }
        Check(replies.size() == 2 && Saved(session, receipt).size() == 2, "seed did not persist actual retrieval history");
        Closed(session, model, retrieval); Take(runtime->Shutdown(), "seed Shutdown");
    }
    auto runtime = Runtime(directory.path); auto model = std::make_shared<ModelState>(); auto retrieval = std::make_shared<RetrievalState>();
    model->expected_history = replies; model->expected_answer = answer;
    auto session = Take(runtime->OpenSession(Options(directory.path / "project", model, "restored", documents, retrieval, sid)), "restore same ID");
    Check(session->id() == sid && Queries(retrieval) == 0 && model->requests == 0, "opening re-executed old retrieval");
    Check(Take(session->ReadOperation(receipt.operation_id), "old operation").final_text == answer,
        "same-ID restore lost owned operation answer");
    Check(Saved(session, receipt).size() == 2, "same-ID restore lost old saved evidence");
    const auto duplicate = Submit(session, "seed-key");
    Check(duplicate.duplicate && duplicate.operation_id == receipt.operation_id && Queries(retrieval) == 0 && model->requests == 0,
        "old idempotency key repeated actual retrieval");
    Succeeded(Finish(session, model, Submit(session, "new-query")), documents);
    Check(model->history_seen && Queries(retrieval) == 2 && model->requests == 3,
        "new query did not see actual restored tool history or repeated old work");
    Closed(session, model, retrieval); Take(runtime->Shutdown(), "restore Shutdown");
}
void Lifetime(const fs::path& base) {
    for (const std::string mode : {"cancel", "close", "last-handle", "shutdown"}) {
        Directory directory(base); auto runtime = Runtime(directory.path);
        auto model = std::make_shared<ModelState>(); auto retrieval = std::make_shared<RetrievalState>(); retrieval->block = true;
        retrieval->hold_after_cancel = mode != "cancel";
        auto session = Take(runtime->OpenSession(Options(directory.path / "project", model, mode, Documents(mode, Token()), retrieval)), "open lifetime");
        const auto receipt = Submit(session, "held-retrieval"); WaitEntered(session, model, retrieval);
        Check(retrieval->destroyed == 0 && model->destroyed == 0 && Queries(retrieval) == 1,
            "in-flight callback lost an owner before cancellation");
        if (mode == "cancel") {
            Take(session->Cancel(receipt.operation_id), "Cancel");
            Check(Finish(session, model, receipt).state == sdk::OperationState::Cancelled, "Cancel did not finish the actual held retrieval");
            Closed(session, model, retrieval);
        } else {
            std::atomic<bool> returned{false}; std::exception_ptr failure;
            std::thread closer([&] {
                try {
                    if (mode == "close") Closed(session, model, retrieval);
                    else if (mode == "last-handle") session.reset();
                    else Take(runtime->Shutdown(), "held Shutdown");
                } catch (...) { failure = std::current_exception(); }
                returned = true;
            });
            // Unwind always releases the actual callback and joins the host
            // closer, including a failing assertion. No borrowed owner escapes.
            struct ReleaseAndJoin {
                std::shared_ptr<RetrievalState> state; std::thread& thread;
                ~ReleaseAndJoin() {
                    { std::lock_guard lock(state->mutex); state->release_cancel = true; }
                    state->cv.notify_all();
                    if (thread.joinable()) thread.join();
                }
            } guard{retrieval, closer};
            {
                std::unique_lock lock(retrieval->mutex);
                Check(retrieval->cv.wait_for(lock, 15s, [&] { return retrieval->cancel_observed; }),
                    "Close did not cancel the actual held callback");
                Check(!returned && retrieval->destroyed == 0 && model->destroyed == 0 && retrieval->cancelled == 0,
                    "Close returned or destroyed live owners before callback exit");
                retrieval->release_cancel = true;
            }
            retrieval->cv.notify_all(); closer.join();
            if (failure) std::rethrow_exception(failure);
            Check(returned, "host close did not return after actual callback exit");
        }
        Check(retrieval->cancelled == 1 && retrieval->destroyed == 1 && model->destroyed == 1 && Queries(retrieval) == 1,
            "actual owner retirement did not join cooperative retrieval exactly once");
        fs::rename(directory.path / "data", directory.path / "retired-data");
        if (session) Check(Take(session->ReadOperation(receipt.operation_id), "closed operation").state == sdk::OperationState::Cancelled,
            "closed handle lost owned cancellation outcome");
        Take(runtime->Shutdown(), "repeated Shutdown");
    }
}
std::string PreviewWire(const results::SavedSnapshot& snapshot) {
    auto projector = Take(results::ResultProjector::Create({false, 512, "rag-demo-node-v1"}, snapshot.policy()), "demo projector");
    auto projection = Take(projector->Project(snapshot), "demo Project");
    return Take(projection.ForTransmission(*projector), "demo ForTransmission");
}
} // namespace

void AgenticRagCase(const std::string& name, const fs::path& base) {
    if (name == "retrieval") Retrieval(base);
    else if (name == "sources") Sources(base);
    else if (name == "preview") Preview(base);
    else if (name == "isolation") Isolation(base);
    else if (name == "recovery") Recovery(base);
    else if (name == "lifetime") Lifetime(base);
    else throw std::runtime_error("agentic-rag: unknown case: " + name);
    std::cout << "[sdk-agentic-rag-path] " << name << '\n';
}
void AgenticRag(const fs::path& base) {
    for (const char* name : {"retrieval", "sources", "preview", "isolation", "recovery", "lifetime"}) AgenticRagCase(name, base);
    std::cout << "[sdk-agentic-rag-consumer] complete\n";
}
int AgenticRagDemo(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
            AgenticRag(fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))));
            return 0;
        }
        if (argc != 9 || std::string_view(argv[1]) != "--live") {
            std::cerr << "Usage: lubancore_rag --fixture ABSOLUTE_STATE_ROOT\n"
                "   or: lubancore_rag --live ABSOLUTE_STATE_ROOT ABSOLUTE_RESOURCE_ROOT ABSOLUTE_PROJECT_ROOT BASE_URL MODEL API_KEY_ENV QUESTION\n";
            return 2;
        }
        const auto path = [](const char* value) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value))); };
        const auto state_root = path(argv[2]), resource_root = path(argv[3]), project_root = path(argv[4]);
        Check(state_root.is_absolute() && resource_root.is_absolute() && project_root.is_absolute(), "live paths must be explicit and absolute");
        Check(fs::is_directory(resource_root) && fs::is_directory(project_root), "resource/project roots must exist");
        const char* key = std::getenv(argv[7]); Check(key && *key, "explicit API key environment variable is empty");
        fs::create_directories(state_root);
        auto runtime = Take(sdk::Runtime::Create({Utf8(state_root / "data"), Utf8(resource_root)}), "live Runtime");
        auto retrieval = std::make_shared<RetrievalState>();
        sdk::SessionOptions options; options.cwd = Utf8(project_root); options.model = argv[6]; options.system_prompt = kPrompt;
        options.connection = sdk::Connection{sdk::Wire::ChatCompletions, argv[5], key};
        options.custom_tools.push_back(RetrievalTool(std::make_shared<TextRetriever>(Documents("demo", "live-reference"), retrieval)));
        options.max_steps_per_turn = 6;
        auto session = Take(runtime->OpenSession(std::move(options)), "live OpenSession");
        const auto receipt = Take(session->Submit("question-" + Token(), argv[8]), "live Submit");
        const auto deadline = std::chrono::steady_clock::now() + 10min;
        sdk::Operation operation;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& approval : session->PendingApprovals()) {
                std::cout << "Allow host retrieval? [y/N] " << std::flush;
                std::string choice; std::getline(std::cin, choice);
                Take(session->ResolveApproval(approval.request_id, choice == "y" || choice == "Y" ?
                    sdk::ApprovalDecision::Accept : sdk::ApprovalDecision::Decline), "live approval");
            }
            operation = Take(session->ReadOperation(receipt.operation_id), "live operation");
            if (Terminal(operation.state)) break;
            std::this_thread::sleep_for(10ms);
        }
        Check(Terminal(operation.state) && operation.state == sdk::OperationState::Succeeded,
            "live operation did not succeed: " + operation.error);
        const auto saved = Saved(session, receipt);
        Check(!saved.empty() && Queries(retrieval) != 0, "live model did not call actual host retrieval");
        CheckCitations(operation.final_text, SavedEvidence(saved));
        std::cout << operation.final_text << "\nSaved evidence previews (Node full permission is off):\n";
        for (const auto& snapshot : saved) std::cout << PreviewWire(snapshot) << '\n';
        Take(session->Close(), "live Close"); Take(runtime->Shutdown(), "live Shutdown");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
} // namespace lubancore_consumer
