#include "job_runner/server.hpp"
#include "job_runner/common.hpp"
#include "job_runner/process.hpp"
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace lubancode::job_runner {
namespace {
class Server {
public:
    Server(fs::path root, std::size_t max_running, std::size_t max_records)
        : root_(std::move(root)), max_running_(max_running), max_records_(max_records) {
        PrivateDirectory(root_);
        if (!platform::RejectReparsePoint(platform::FileIoPath(root_ / ".owner.lock"))) throw Error("runner.state_is_link");
        lock_ = std::make_unique<platform::InterProcessFileLock>(platform::FileIoPath(root_ / ".owner.lock"), 0);
        if (!lock_->holds()) throw Error("runner.already_running");
        std::error_code ec;
        const bool existing = fs::exists(platform::FileIoPath(root_ / "identity.json"), ec);
        if (ec) throw Error("runner.state_unreadable");
        if (existing) {
            identity_ = ReadJson(root_ / "identity.json");
            if (!identity_.contains("schemaVersion") || !identity_["schemaVersion"].is_number_integer() ||
                identity_["schemaVersion"] != 1 || !HexId(identity_.value("runner_id", "")) ||
                !HexId(identity_.value("auth_token", ""))) throw Error("runner.corrupt_state");
            // Missing journals/directories are not a new empty installation.
            for (const char* name : {"requests", "responses", "jobs"}) VerifyPrivateDirectory(root_ / name);
            const auto envelope = ReadJson(root_ / "jobs.json", kMaxLedgerBytes);
            if (!envelope.contains("schemaVersion") || !envelope["schemaVersion"].is_number_integer() ||
                envelope["schemaVersion"] != 1 || !envelope.contains("records") ||
                !envelope["records"].is_object() || envelope.value("sha256", "") != Digest(envelope["records"])) {
                throw Error("runner.corrupt_state");
            }
            records_ = envelope["records"];
        } else {
            // A nonempty orphaned store is not adopted under a fresh identity.
            for (const auto& entry : fs::directory_iterator(platform::FileIoPath(root_))) {
                if (entry.path().filename() != ".owner.lock") throw Error("runner.state_incomplete");
            }
            for (const char* name : {"requests", "responses", "jobs"}) PrivateDirectory(root_ / name);
            identity_ = Json{{"schemaVersion", 1}, {"runner_id", RandomHex()}, {"auth_token", RandomHex()}};
            if (!platform::WriteNewSecureFile(platform::FileIoPath(root_ / "identity.json"), identity_.dump())) {
                throw Error("runner.store_failed");
            }
            Save();
        }
        boot_ = RandomHex();
        Recover();
        SaveJson(root_ / "endpoint.json", Json{{"schemaVersion", 1}, {"runner_id", identity_["runner_id"]}, {"boot_id", boot_}});
    }
    Json Info() const {
        return Json{{"ok", true}, {"runner_id", identity_["runner_id"]}, {"boot_id", boot_},
            {"capabilities", {{"deployment", "independent_runner_required"},
                              {"worker_exit_survival", "independent_domain_required"},
                              {"node_process_exit_survival", "independent_domain_required"},
                              {"arbitrary_service_restart_survival", false}, {"runner_crash_resume", false},
                              {"machine_restart_resume", false}, {"termination_scope", ProcessScope()},
                              {"terminal_evidence", ProcessScope() == "windows_job_object" ?
                                  "leader_exit_and_job_empty" : "leader_exit_and_group_signal"},
                              {"escaped_descendants_contained", ProcessScope() == "windows_job_object"},
                              {"gpu_management", false}, {"log_sync", "metadata_only"}}},
            {"active_jobs", active_.size()}};
    }
    void Tick() {
        Poll();
        std::size_t processed = 0;
        for (const auto& entry : fs::directory_iterator(platform::FileIoPath(root_ / "requests"))) {
            const auto filename = platform::PathToUtf8(entry.path().filename());
            if (filename.size() != 37 || filename.substr(32) != ".json" || !HexId(filename.substr(0, 32))) continue;
            if (++processed > 64) break;
            const auto request_id = filename.substr(0, 32);
            Json response;
            try {
                const auto envelope = ReadJson(entry.path());
                Keys(envelope, {"schemaVersion", "runner_id", "boot_id", "auth_token", "request_id", "expires_at_ms", "body"});
                if (!envelope.contains("schemaVersion") || !envelope["schemaVersion"].is_number_integer() ||
                    envelope["schemaVersion"] != 1 || envelope.value("request_id", "") != request_id ||
                    envelope.value("runner_id", "") != identity_["runner_id"].get<std::string>() ||
                    !EqualSecret(envelope.value("auth_token", ""), identity_["auth_token"].get<std::string>())) {
                    throw Error("runner.unauthorized");
                }
                if (envelope.value("boot_id", "") != boot_) throw Error("runner.stale_boot");
                if (!envelope.contains("expires_at_ms") || !envelope["expires_at_ms"].is_number_integer() ||
                    envelope["expires_at_ms"].get<std::int64_t>() < NowMs() ||
                    envelope["expires_at_ms"].get<std::int64_t>() > NowMs() + 31000) throw Error("runner.request_expired");
                response = Dispatch(envelope.at("body"));
            } catch (const Error& error) {
                if (std::string_view(error.what()) == "runner.store_failed") throw;
                response = Failure(error.what());
            } catch (...) {
                response = Failure("runner.invalid_request");
            }
            response["runner_id"] = identity_["runner_id"];
            response["boot_id"] = boot_;
            response["request_id"] = request_id;
            try { PublishJson(root_ / "responses", filename, response); }
            catch (const Error&) {
                // A lost transport receipt does not terminate accepted jobs.
                // Their durable idempotency key answers a later retry.
            }
            std::error_code ec;
            fs::remove(entry.path(), ec);
        }
        // Replies are transport receipts, not the durable job/idempotency book.
        const auto stale = fs::file_time_type::clock::now() - std::chrono::minutes(5);
        for (const auto& entry : fs::directory_iterator(platform::FileIoPath(root_ / "responses"))) {
            std::error_code ec;
            if (entry.last_write_time(ec) < stale && !ec) fs::remove(entry.path(), ec);
        }
    }
    void Stop() {
        for (auto& [key, process] : active_) {
            auto& record = records_[key];
            record["cancel_requested"] = true;
            Save();
            process->Cancel();
            record["cancel_delivered"] = true;
            Save();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!active_.empty() && std::chrono::steady_clock::now() < deadline) {
            Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // Remaining processes have no observed final result, even on shutdown.
        for (const auto& [key, process] : active_) {
            (void)process;
            records_[key]["state"] = "indeterminate";
            records_[key]["error_code"] = "runner.exit_unobserved";
        }
        if (!active_.empty()) Save();
    }
private:
    void Save() {
        Json book{{"schemaVersion", 1}, {"records", records_}, {"sha256", Digest(records_)}};
        if (book.dump().size() > kMaxLedgerBytes) throw Error("runner.store_failed");
        SaveJson(root_ / "jobs.json", book);
    }
    void Recover() {
        std::set<std::string> jobs;
        bool changed = false;
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            auto& record = it.value();
            try {
                const auto session = record.at("session_id").get<std::string>();
                const auto client_key = record.at("client_job_id").get<std::string>();
                const auto id = record.at("job_id").get<std::string>();
                const auto state = record.at("state").get<std::string>();
                if (!Identity(session) || !Identity(client_key) || !HexId(id) || !jobs.insert(id).second ||
                    !HexId(record.at("job_nonce").get<std::string>()) || it.key() != KeyFor(session, client_key) ||
                    !HexId(record.at("owner_boot_id").get<std::string>()) ||
                    record.at("process_nonce") != record.at("job_nonce") || !record.at("pid").is_number_integer() ||
                    record.at("runner_id") != identity_["runner_id"] ||
                    record.at("spec_sha256") != Digest(NormalizeSpec(record.at("spec"))) ||
                    (state != "starting" && state != "running" && !Terminal(state)) ||
                    !record.at("cancel_requested").is_boolean() || !record.at("cancel_delivered").is_boolean()) {
                    throw Error("runner.corrupt_state");
                }
                if ((state == "succeeded" && (!record.at("exit_code").is_number_integer() || record.at("exit_code") != 0)) ||
                    (state == "cancelled" && (!record.at("exit_code").is_number_integer() || !record.at("cancel_delivered").get<bool>()))) {
                    throw Error("runner.corrupt_state");
                }
                if ((record.at("cancel_delivered").get<bool>() && !record.at("cancel_requested").get<bool>()) ||
                    (state == "indeterminate" && !record.at("exit_code").is_null()) ||
                    (state == "failed" && !record.at("exit_code").is_number_integer() &&
                     !(record.at("exit_code").is_null() && !record.at("error_code").get<std::string>().empty()))) {
                    throw Error("runner.corrupt_state");
                }
                VerifyPrivateDirectory(root_ / "jobs" / id);
                if (!Terminal(state)) {
                    // Never reopen/kill a process from this record's numeric PID.
                    record["state"] = "indeterminate";
                    record["error_code"] = "runner.previous_owner_lost";
                    record["exit_code"] = nullptr;
                    changed = true;
                }
            } catch (...) { throw Error("runner.corrupt_state"); }
        }
        if (changed) Save();
    }
    Json Snapshot(const Json& record) const {
        const auto id = record["job_id"].get<std::string>();
        Json logs = Json::object();
        for (const char* stream : {"stdout", "stderr"}) {
            std::error_code ec;
            const auto bytes = fs::file_size(platform::FileIoPath(root_ / "jobs" / id / (std::string(stream) + ".log")), ec);
            logs[stream] = Json{{"log_id", id + "." + stream}, {"bytes", ec ? Json(nullptr) : Json(bytes)}};
        }
        return Json{{"ok", true}, {"job", {{"job_id", record["job_id"]}, {"job_nonce", record["job_nonce"]},
            {"session_id", record["session_id"]}, {"state", record["state"]}, {"exit_code", record["exit_code"]},
            {"error_code", record["error_code"]}, {"cancel_requested", record["cancel_requested"]},
            {"termination_scope", ProcessScope()}, {"logs", logs}}}};
    }
    Json Dispatch(const Json& body) {
        Poll();
        const auto method = body.at("method").get<std::string>();
        if (method == "runner.info") { Keys(body, {"method"}); return Info(); }
        if (method == "job.logs") return Failure("runner.full_log_sync_disabled");
        if (method == "job.start") {
            Keys(body, {"method", "session_id", "client_job_id", "spec"});
            const auto session = body.at("session_id").get<std::string>();
            const auto client_key = body.at("client_job_id").get<std::string>();
            if (!Identity(session) || !Identity(client_key)) throw Error("runner.invalid_identity");
            const auto spec = NormalizeSpec(body.at("spec"));
            const auto digest = Digest(spec);
            const auto key = KeyFor(session, client_key);
            if (records_.contains(key)) {
                if (records_[key]["spec_sha256"] != digest) throw Error("runner.idempotency_conflict");
                return Snapshot(records_[key]);
            }
            if (active_.size() >= max_running_) throw Error("runner.busy");
            if (records_.size() >= max_records_) throw Error("runner.retention_limit");
            const auto environment = ResolveEnvironment(spec);
            const auto id = RandomHex();
            const auto nonce = RandomHex();
            const Json initial{{"runner_id", identity_["runner_id"]}, {"session_id", session},
                {"client_job_id", client_key}, {"job_id", id}, {"job_nonce", nonce},
                {"spec", spec}, {"spec_sha256", digest}, {"state", "starting"},
                {"created_at_ms", NowMs()}, {"owner_boot_id", boot_}, {"process_nonce", nonce},
                {"pid", 0}, {"exit_code", nullptr}, {"error_code", ""},
                {"cancel_requested", false}, {"cancel_delivered", false}};
            // Leave room for every retained job's final evidence. A configured
            // retention bound must refuse admission, not crash existing work.
            if (records_.dump().size() + initial.dump().size() + key.size() + 128 +
                (records_.size() + 1) * 512 > kMaxLedgerBytes) throw Error("runner.retention_limit");
            PrivateDirectory(root_ / "jobs" / id);
            records_[key] = initial;
            Save(); // Intent is durable before any process may execute.
            auto& record = records_[key];
            try {
                auto process = Process::Create(spec, root_ / "jobs" / id, environment);
                record["pid"] = process->pid();
                record["state"] = "running";
                Save(); // Persist the startup identity before releasing user code.
                process->Release();
                active_.emplace(key, std::move(process));
            } catch (const Error& error) {
                if (std::string_view(error.what()) == "runner.store_failed") throw;
                record["state"] = "failed";
                record["error_code"] = error.what();
                Save();
            }
            return Snapshot(record);
        }
        if (method != "job.get" && method != "job.cancel") throw Error("runner.unsupported_method");
        Keys(body, {"method", "session_id", "job_id", "job_nonce"});
        const auto id = body.at("job_id").get<std::string>();
        const auto session = body.at("session_id").get<std::string>();
        const auto nonce = body.at("job_nonce").get<std::string>();
        if (!HexId(id) || !HexId(nonce) || !Identity(session)) throw Error("runner.invalid_identity");
        auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& record) { return record["job_id"] == id; });
        if (found == records_.end() || (*found)["session_id"] != session || !EqualSecret((*found)["job_nonce"].get<std::string>(), nonce)) {
            throw Error("runner.job_not_owned");
        }
        auto& record = *found;
        if (method == "job.cancel") {
            if (record["state"] == "indeterminate") throw Error("runner.job_indeterminate");
            if (!Terminal(record["state"].get<std::string>())) {
                const auto active = active_.find(found.key());
                if (active == active_.end()) throw Error("runner.job_indeterminate");
                record["cancel_requested"] = true;
                Save();
                active->second->Cancel();
                record["cancel_delivered"] = true;
                Save();
            }
        }
        return Snapshot(record);
    }
    void Poll() {
        for (auto it = active_.begin(); it != active_.end();) {
            auto& record = records_[it->first];
            bool finished = false;
            try {
                if (const auto exit = it->second->Poll()) {
                    record["exit_code"] = *exit;
                    record["state"] = record["cancel_delivered"].get<bool>() ? "cancelled" : (*exit == 0 ? "succeeded" : "failed");
                    record["finished_at_ms"] = NowMs();
                    finished = true;
                }
            } catch (const Error& error) {
                record["state"] = "indeterminate";
                record["error_code"] = error.what();
                record["exit_code"] = nullptr;
                finished = true;
            }
            if (finished) { Save(); it = active_.erase(it); } else ++it;
        }
    }
    fs::path root_;
    std::size_t max_running_, max_records_;
    std::unique_ptr<platform::InterProcessFileLock> lock_;
    Json identity_;
    std::string boot_;
    Json records_ = Json::object();
    std::map<std::string, std::unique_ptr<Process>> active_;
};
}
int Serve(const fs::path& state_root, std::size_t max_running, std::size_t max_records,
          volatile std::sig_atomic_t& stop) {
    Server server(fs::absolute(state_root).lexically_normal(), max_running, max_records);
    std::cout << server.Info().dump() << std::endl;
    while (!stop) {
        server.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    server.Stop();
    return 0;
}
}  // namespace lubancode::job_runner
