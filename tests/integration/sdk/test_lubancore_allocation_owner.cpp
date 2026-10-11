#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include "platform/paths.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"
#include "sdk/allocation_test_hooks.hpp"
#include "sdk/usage_result.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace {
using Kind = lubancore::detail::testing::AllocationKind;
std::terminate_handler prior_allocation_terminate = nullptr;
[[noreturn]] void TraceAllocationTermination() noexcept {
    // The original host owns this failure diagnostic. No SDK result is invented
    // and the original terminate outcome remains a native test failure.
    std::fputs("[sdk-allocation-terminate] actual host stack follows\n", stderr);
    std::array<void*, 64> frames{};
#ifdef _WIN32
    const auto count = CaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    for (unsigned i = 0; i != count; ++i) std::fprintf(stderr, "[sdk-allocation-frame] %u %p\n", i, frames[i]);
#else
    const auto count = ::backtrace(frames.data(), static_cast<int>(frames.size()));
    if (count > 0) ::backtrace_symbols_fd(frames.data(), count, 2);
#endif
    std::fflush(stderr);
    if (prior_allocation_terminate && prior_allocation_terminate != &TraceAllocationTermination)
        prior_allocation_terminate();
    std::abort();
}
struct ScopedAllocationTerminationDiagnostic {
    std::terminate_handler previous;
    ScopedAllocationTerminationDiagnostic() noexcept {
        // Warm native unwinding before installing any refusal scope.
        std::array<void*, 64> frames{};
#ifdef _WIN32
        (void)CaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
#else
        (void)::backtrace(frames.data(), static_cast<int>(frames.size()));
#endif
        previous = std::set_terminate(&TraceAllocationTermination);
        prior_allocation_terminate = previous;
    }
    ~ScopedAllocationTerminationDiagnostic() { std::set_terminate(previous); }
    ScopedAllocationTerminationDiagnostic(const ScopedAllocationTerminationDiagnostic&) = delete;
    ScopedAllocationTerminationDiagnostic& operator=(const ScopedAllocationTerminationDiagnostic&) = delete;
};
std::filesystem::path ImageOf(const void* address) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module))
        throw std::runtime_error("allocation qualification module lookup failed");
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("allocation qualification module path failed");
    return std::filesystem::path(path.data());
#else
    Dl_info image{};
    if (!dladdr(address, &image) || !image.dli_fname)
        throw std::runtime_error("allocation qualification module lookup failed");
    return std::filesystem::path(image.dli_fname);
#endif
}
// This implementation belongs to the actual host executable, never SDK sources.
void* HostAllocate(std::size_t size, std::size_t alignment, Kind kind) {
    const auto align = static_cast<std::align_val_t>(alignment);
    switch (kind) {
        case Kind::Scalar: return ::operator new(size);
        case Kind::Array: return ::operator new[](size);
        case Kind::ScalarNoThrow: return ::operator new(size, std::nothrow);
        case Kind::ArrayNoThrow: return ::operator new[](size, std::nothrow);
        case Kind::AlignedScalar: return ::operator new(size, align);
        case Kind::AlignedArray: return ::operator new[](size, align);
        case Kind::AlignedScalarNoThrow: return ::operator new(size, align, std::nothrow);
        case Kind::AlignedArrayNoThrow: return ::operator new[](size, align, std::nothrow);
    }
    throw std::bad_alloc{};
}
void HostRelease(void* pointer, std::size_t size, std::size_t alignment, Kind kind, bool sized) noexcept {
    const unsigned bits = static_cast<unsigned>(kind);
    const bool array = (bits & 1U) != 0, nothrow = (bits & 2U) != 0, aligned = (bits & 4U) != 0;
    const auto align = static_cast<std::align_val_t>(alignment);
    if (aligned) {
        if (sized) {
            if (array) ::operator delete[](pointer, size, align);
            else ::operator delete(pointer, size, align);
        } else if (nothrow) {
            if (array) ::operator delete[](pointer, align, std::nothrow);
            else ::operator delete(pointer, align, std::nothrow);
        } else {
            if (array) ::operator delete[](pointer, align);
            else ::operator delete(pointer, align);
        }
    } else if (sized) {
        if (array) ::operator delete[](pointer, size);
        else ::operator delete(pointer, size);
    } else if (nothrow) {
        if (array) ::operator delete[](pointer, std::nothrow);
        else ::operator delete(pointer, std::nothrow);
    } else {
        if (array) ::operator delete[](pointer);
        else ::operator delete(pointer);
    }
}
struct OwnedAllocation {
    void* pointer;
    decltype(lubancore::detail::testing::AllocationQualificationPort::release) release;
    std::size_t size, alignment;
    Kind kind;
    bool sized;
    ~OwnedAllocation() { if (pointer) release(pointer, size, alignment, kind, sized); }
};
struct OwnedRefusalImports {
    lubancore::detail::testing::AllocationFailurePort port;
    bool installed;
    ~OwnedRefusalImports() { port.end(); if (installed) port.restore(); }
};
struct OwnedRefusalScope {
    lubancore::detail::testing::AllocationFailurePort port;
    bool active;
    ~OwnedRefusalScope() { if (active) port.end(); }
    void End() { if (active) { port.end(); active = false; } }
};

using namespace std::chrono_literals;
namespace sdk = lubancore;
namespace platform = lubancode::platform;
enum class ReturnedPath { Success, Cancel, Close };
struct ReturnedGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    std::atomic<bool> entered{false}, cancelled{false}, prepared{false}, destroyed{false}, timed_out{false};
    std::atomic<unsigned> calls{0};
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
};
struct ReleaseReturnedGate {
    std::shared_ptr<ReturnedGate> gate;
    ~ReleaseReturnedGate() { gate->Release(); }
};
bool WaitReturnedFlag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    return flag.load();
}
class ReturnedAllocationBackend final : public sdk::Backend {
public:
    ReturnedAllocationBackend(std::shared_ptr<ReturnedGate> gate,
            lubancore::detail::testing::AllocationFailurePort port,
            lubancore::detail::testing::AllocationFailureControl& control)
        : gate_(std::move(gate)), port_(port), control_(control) {
        // Reply construction happens before the Session exists, outside all scopes.
        reply_.text = "actual post-return allocation calibration reply";
        reply_.stop_reason = "end_turn";
        reply_.provider_response_id = "actual-allocation-calibration-response";
        reply_.usage = sdk::Usage{11, 7, 13, 17, 3};
        namespace facts = sdk::usage::v1;
        facts::Observation observation;
        observation.provider_namespace = "sdk.allocation.calibration";
        const std::array<std::int64_t, facts::kFieldCount> numbers{11, 7, 13, 17, 3};
        for (std::size_t i = 0; i != numbers.size(); ++i) {
            facts::RawField raw;
            raw.path = "reply.usage." + std::to_string(i);
            raw.kind = facts::RawKind::SignedInteger;
            raw.integer = numbers[i]; raw.summary = std::to_string(numbers[i]);
            observation.raw_fields.push_back(std::move(raw));
            auto& field = observation.fields[i];
            field.presence = facts::Presence::Present; field.validity = facts::Validity::ValidInteger;
            field.origin = facts::Origin::Reported;
            field.operands[0] = static_cast<std::uint16_t>(i); field.operand_count = 1;
        }
        reply_.usage_observation = std::move(observation);
    }
    ~ReturnedAllocationBackend() override { gate_->destroyed.store(true); }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation cancel) override {
        gate_->calls.fetch_add(1); gate_->entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        std::unique_lock lock(gate_->mutex);
        while (!gate_->released && std::chrono::steady_clock::now() < deadline) {
            if (cancel.requested()) gate_->cancelled.store(true);
            gate_->cv.wait_for(lock, 2ms);
        }
        gate_->cancelled.store(cancel.requested());
        if (!gate_->released) gate_->timed_out.store(true);
        gate_->prepared.store(port_.prepare_returned_backend(control_));
        // The private port only prepared a borrow. The real SDK return activates it.
        return std::move(reply_);
    }
private:
    std::shared_ptr<ReturnedGate> gate_;
    lubancore::detail::testing::AllocationFailurePort port_;
    lubancore::detail::testing::AllocationFailureControl& control_;
    sdk::ModelReply reply_;
};
class RecoveryAllocationBackend final : public sdk::Backend {
public:
    explicit RecoveryAllocationBackend(std::atomic<unsigned>& calls) : calls_(calls) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        calls_.fetch_add(1);
        return std::unexpected(sdk::Error{"fixture.replay", "recovery must not generate"});
    }
private:
    std::atomic<unsigned>& calls_;
};
struct ReturnedAllocationFixture {
    std::filesystem::path root;
    std::filesystem::path material_source, material_target;
    std::string material_operation_id;
    ReturnedAllocationFixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() / ("sdk-actual-returned-alloc-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial.fetch_add(1)));
        std::filesystem::create_directories(root / "cwd");
        std::filesystem::create_directories(root / "resources");
    }
    void Archive() const {
        if (material_source.empty()) return;
        std::filesystem::create_directories(material_target);
        for (const auto& [source, name] : std::array<std::pair<std::filesystem::path, const char*>, 3>{
                std::pair{material_source / (material_source.filename().string() + ".jsonl"), "main.jsonl"},
                std::pair{material_source / "operations.jsonl", "operations.jsonl"},
                std::pair{material_source / "sdk-results" / (material_operation_id + ".json"), "result.json"}}) {
            if (!std::filesystem::exists(source)) continue;
            if (std::filesystem::is_symlink(source)) throw std::runtime_error("allocation evidence is a symbolic link");
            std::filesystem::copy_file(source, material_target / name, std::filesystem::copy_options::overwrite_existing);
        }
    }
    ~ReturnedAllocationFixture() {
        try { Archive(); } // Also retain the original failed point after owners retire.
        catch (...) { } // A failed assertion/exception must remain the native failure.
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    sdk::SessionOptions Options() const {
        sdk::SessionOptions options;
        options.cwd = platform::PathToUtf8(root / "cwd"); options.model = "fixture";
        options.system_prompt = "actual returned allocation calibration"; options.max_steps_per_turn = 1;
        return options;
    }
    std::filesystem::path SessionDir(const std::string& id) const {
        const auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity);
        const auto directory = lubancode::workspace::index::ResolveDirByWorkspaceKey(root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(directory);
        return *directory / "sessions" / id;
    }
};
void CheckAllocationObservation(const sdk::usage::v1::Observation& observation) {
    namespace facts = sdk::usage::v1;
    const std::array<std::int64_t, facts::kFieldCount> numbers{11, 7, 13, 17, 3};
    CHECK(observation.provider_namespace == "sdk.allocation.calibration");
    REQUIRE(observation.raw_fields.size() == numbers.size());
    for (std::size_t i = 0; i != numbers.size(); ++i) {
        const auto& raw = observation.raw_fields[i];
        CHECK(raw.path == "reply.usage." + std::to_string(i));
        CHECK(raw.kind == facts::RawKind::SignedInteger); CHECK(raw.integer == numbers[i]);
        CHECK(raw.summary == std::to_string(numbers[i]));
        const auto& field = observation.fields[i];
        CHECK(field.presence == facts::Presence::Present); CHECK(field.validity == facts::Validity::ValidInteger);
        CHECK(field.origin == facts::Origin::Reported); CHECK(field.operand_count == 1); CHECK(field.operands[0] == i);
    }
}
void CheckReturnedOperation(const sdk::Operation& operation, ReturnedPath path) {
    CHECK(operation.state == (path == ReturnedPath::Success ? sdk::OperationState::Succeeded : sdk::OperationState::Cancelled));
    CHECK(operation.result_persisted);
    CHECK(operation.final_text == (path == ReturnedPath::Success ? "actual post-return allocation calibration reply" : ""));
    REQUIRE(operation.usage);
    const auto& direct = operation.usage->direct;
    CHECK(direct.coverage.samples == 1);
    CHECK(direct.total.input_tokens == 11); CHECK(direct.total.output_tokens == 7);
    CHECK(direct.total.cache_read_tokens == 13); CHECK(direct.total.cache_creation_tokens == 17);
    CHECK(direct.total.output_reasoning_tokens == 3);
    REQUIRE(operation.usage->attempts.size() == 1);
    const auto& attempt = operation.usage->attempts.front();
    CHECK(attempt.reported_by_provider); CHECK_FALSE(attempt.subordinate);
    CHECK(attempt.incomplete == (path != ReturnedPath::Success));
    CHECK(attempt.provider_response_id == "actual-allocation-calibration-response");
    REQUIRE(attempt.observation); CheckAllocationObservation(*attempt.observation);
    CHECK_FALSE(attempt.trajectory_request_id.empty());
}
struct ReturnedAllocationRun {
    std::uint64_t allocations = 0;
    unsigned phase = 0, finals = 0, observations = 0, terminals = 0;
    bool persisted = false;
    sdk::OperationState state = sdk::OperationState::Accepted;
};
nlohmann::json OperationMaterial(const sdk::Operation& operation) {
    nlohmann::json result{{"operationId", operation.operation_id}, {"turnId", operation.turn_id},
        {"state", static_cast<unsigned>(operation.state)}, {"finalText", operation.final_text},
        {"error", operation.error}, {"resultPersisted", operation.result_persisted}};
    if (operation.usage) result["usage"] = lubancore::detail::usage_result::Encode(*operation.usage);
    return result;
}
void WriteAllocationMaterial(const std::filesystem::path& path, const nlohmann::json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); REQUIRE(output);
    output << value.dump(); output.close(); REQUIRE_FALSE(output.fail());
}
void CheckReturnedFaultNumbers(const sdk::Operation& operation, ReturnedPath path) {
    CHECK((operation.state == sdk::OperationState::Succeeded || operation.state == sdk::OperationState::Failed ||
           operation.state == sdk::OperationState::Cancelled || operation.state == sdk::OperationState::Indeterminate));
    if (!operation.result_persisted) CHECK(operation.state == sdk::OperationState::Indeterminate);
    if (path != ReturnedPath::Success) CHECK(operation.final_text.empty());
    if (operation.state == sdk::OperationState::Succeeded) {
        CHECK(path == ReturnedPath::Success); CHECK(operation.result_persisted);
        CHECK(operation.final_text == "actual post-return allocation calibration reply");
    }
    REQUIRE(operation.usage);
    const auto& usage = *operation.usage;
    CHECK(usage.direct.coverage.samples == 1);
    CHECK(usage.direct.total.input_tokens == 11); CHECK(usage.direct.total.output_tokens == 7);
    CHECK(usage.direct.total.cache_read_tokens == 13); CHECK(usage.direct.total.cache_creation_tokens == 17);
    CHECK(usage.direct.total.output_reasoning_tokens == 3); CHECK(usage.subordinate.coverage.samples == 0);
    REQUIRE(usage.attempts.size() <= 1);
    if (usage.attempts.empty()) CHECK_FALSE(usage.attempts_complete);
    for (const auto& attempt : usage.attempts) {
        CHECK(attempt.reported_by_provider); CHECK_FALSE(attempt.subordinate);
        CHECK(attempt.usage.input_tokens == 11); CHECK(attempt.usage.output_tokens == 7);
        CHECK(attempt.usage.cache_read_tokens == 13); CHECK(attempt.usage.cache_creation_tokens == 17);
        CHECK(attempt.usage.output_reasoning_tokens == 3);
        if (!attempt.provider_response_id.empty()) CHECK(attempt.provider_response_id == "actual-allocation-calibration-response");
        if (attempt.observation) CheckAllocationObservation(*attempt.observation);
        else CHECK(attempt.incomplete);
    }
}
ReturnedAllocationRun RunReturnedSession(lubancore::detail::testing::AllocationFailurePort port,
                                        ReturnedPath path, std::uint64_t ordinal = 0) {
    ReturnedAllocationFixture fixture;
    lubancore::detail::testing::AllocationFailureControl control;
    control.require_returned_backend = true;
    control.refuse_ordinal = ordinal;
    auto gate = std::make_shared<ReturnedGate>();
    auto runtime = sdk::Runtime::Create({platform::PathToUtf8(fixture.root / "data"), platform::PathToUtf8(fixture.root / "resources")});
    REQUIRE(runtime);
    std::atomic<unsigned> tools{0};
    const auto tool_sentinel = [&] {
        sdk::Tool tool;
        tool.name = "allocation_replay_probe"; tool.description = "must not execute"; tool.requires_approval = false;
        tool.execute = [&](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            tools.fetch_add(1); return std::unexpected(sdk::Error{"fixture.tool_replay", "must not execute"});
        };
        return tool;
    };
    auto options = fixture.Options();
    options.custom_tools.push_back(tool_sentinel());
    options.backend = std::make_unique<ReturnedAllocationBackend>(gate, port, control);
    auto opened = (*runtime)->OpenSession(std::move(options)); REQUIRE(opened);
    auto session = std::move(*opened); const auto id = session->id();
    fixture.material_source = fixture.SessionDir(id);
    fixture.material_target = std::filesystem::path(LUBANCORE_TEST_ALLOCATION_EVIDENCE_ROOT) /
        (std::to_string(static_cast<unsigned>(path)) + "-" + std::to_string(ordinal)) / id;
    std::filesystem::create_directories(fixture.material_target);
    auto events = session->Subscribe(); REQUIRE(events);
    ReturnedAllocationRun run;
    std::future<sdk::Result<void>> closing;
    ReleaseReturnedGate release{gate}; // Unblocks before future/session destruction on every assertion failure.
    const auto receipt = session->Submit("actual-allocation-calibration", "calibrate the real SDK reply return");
    REQUIRE(receipt); REQUIRE(WaitReturnedFlag(gate->entered));
    fixture.material_operation_id = receipt->operation_id;
    CHECK_FALSE(control.backend_returned.load()); CHECK(control.allocations.load() == 0);
    if (path == ReturnedPath::Close) {
        closing = std::async(std::launch::async, [owner = session.get(), port, &control] {
            OwnedRefusalScope scope{port, port.begin(control)};
            if (!scope.active) return sdk::Result<void>(std::unexpected(sdk::Error{"fixture.scope", "Close allocation borrow failed"}));
            return owner->Close();
        });
        REQUIRE(WaitReturnedFlag(gate->cancelled));
        CHECK(closing.wait_for(20ms) == std::future_status::timeout);
        CHECK(control.allocations.load() == 0); CHECK_FALSE(control.backend_returned.load());
    } else if (path == ReturnedPath::Cancel) {
        REQUIRE(session->Cancel(receipt->operation_id)); REQUIRE(WaitReturnedFlag(gate->cancelled));
    }
    gate->Release();
    if (path == ReturnedPath::Close) {
        REQUIRE(closing.wait_for(15s) == std::future_status::ready);
        try {
            const auto closed = closing.get();
            if (!ordinal) REQUIRE(closed);
            else if (!closed) CHECK_FALSE(closed.error().code.empty());
        } catch (const std::bad_alloc&) {
            REQUIRE(ordinal != 0); REQUIRE(control.refusals.load() == 1);
            // The actual Close borrow has unwound. Retry the actual API with
            // ordinary allocation restored, never substitute a retirement flag.
            const auto retired = session->Close();
            if (!retired) CHECK_FALSE(retired.error().code.empty());
        }
    } else {
        const auto complete = session->WaitResult(receipt->operation_id, 15s); REQUIRE(complete);
        if (!ordinal) CheckReturnedOperation(*complete, path);
        else CheckReturnedFaultNumbers(*complete, path);
        bool terminal = false;
        for (unsigned n = 0; n != 256 && !terminal; ++n) {
            const auto event = (*events)->Next(50ms); REQUIRE(event);
            if (!event->has_value()) {
                // A confirmed final can outlive failed transient notification.
                // Actual worker retirement makes that omission observable; the
                // test records it rather than fabricating an ACK or an event.
                if (ordinal && control.worker_retired.load()) break;
                continue;
            }
            if ((*event)->kind != "operation_completed") continue;
            CHECK((*event)->session_id == id); CHECK((*event)->operation_id == receipt->operation_id);
            CHECK((*event)->turn_id == complete->turn_id);
            const auto payload = nlohmann::json::parse((*event)->payload_json);
            CHECK(payload.at("resultPersisted") == complete->result_persisted);
            const auto status = complete->state == sdk::OperationState::Succeeded ? "success" :
                complete->state == sdk::OperationState::Cancelled ? "cancelled" :
                complete->state == sdk::OperationState::Failed ? "error" : "interrupted";
            CHECK(payload.at("status") == status); CHECK(payload.at("error") == complete->error);
            terminal = true;
        }
        run.terminals = terminal ? 1U : 0U;
        if (!ordinal) REQUIRE(terminal);
        try {
            OwnedRefusalScope scope{port, port.begin(control)}; REQUIRE(scope.active);
            const auto closed = session->Close();
            if (!ordinal) REQUIRE(closed);
            else if (!closed) CHECK_FALSE(closed.error().code.empty());
        } catch (const std::bad_alloc&) {
            REQUIRE(ordinal != 0); REQUIRE(control.refusals.load() == 1);
            const auto retired = session->Close();
            if (!retired) CHECK_FALSE(retired.error().code.empty());
        }
    }
    CHECK(control.backend_returned.load()); CHECK(gate->prepared.load());
    CHECK(gate->destroyed.load()); CHECK_FALSE(gate->timed_out.load()); CHECK(gate->calls.load() == 1);
    REQUIRE(control.worker_retired.load());
    REQUIRE(control.refusals.load() == (ordinal ? 1U : 0U));
    const auto count = control.allocations.load(); REQUIRE(count > 0);
    run.allocations = count; run.phase = control.refused_phase.load();
    if (ordinal) REQUIRE(run.phase > 0);
    const auto operation = session->ReadOperation(receipt->operation_id); REQUIRE(operation);
    if (!ordinal) CheckReturnedOperation(*operation, path);
    else CheckReturnedFaultNumbers(*operation, path);
    run.state = operation->state; run.persisted = operation->result_persisted;
    WriteAllocationMaterial(fixture.material_target / "query.json", OperationMaterial(*operation));
    const auto directory = fixture.SessionDir(id);
    std::ifstream final_file(directory / "operations.jsonl", std::ios::binary); REQUIRE(final_file);
    nlohmann::json final_record;
    for (std::string line; std::getline(final_file, line);) {
        const auto row = nlohmann::json::parse(line);
        if (row.value("kind", "") != "operation.final" || row.value("operationId", "") != receipt->operation_id) continue;
        ++run.finals; final_record = row;
        CHECK(row.at("turnId") == operation->turn_id);
    }
    REQUIRE(run.finals <= 1); // Never a second exception completion over a confirmed final.
    final_file.close();
    std::ifstream result_file(directory / "sdk-results" / (receipt->operation_id + ".json"), std::ios::binary);
    nlohmann::json result_record;
    if (result_file) result_record = nlohmann::json::parse(result_file);
    result_file.close();
    const bool complete_record = run.finals == 1 && result_record.is_object() && result_record.value("complete", false);
    CHECK(operation->result_persisted == complete_record);
    if (operation->result_persisted) {
        const auto status = operation->state == sdk::OperationState::Succeeded ? "success" :
            operation->state == sdk::OperationState::Cancelled ? "cancelled" : "error";
        CHECK(final_record.at("executionStatus") == status);
        CHECK(result_record.at("error") == operation->error);
    }
    if (run.finals == 1 && result_record.is_object()) {
        CHECK(result_record.at("operationId") == operation->operation_id);
        CHECK(result_record.at("turnId") == operation->turn_id);
        CHECK(result_record.at("finalText") == operation->final_text);
        REQUIRE(operation->usage);
        CHECK(result_record.at("usage") == lubancore::detail::usage_result::Encode(*operation->usage));
    }
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(directory / (id + ".jsonl")); REQUIRE(ledger);
    unsigned observations = 0;
    for (const auto& event : ledger->events) {
        if (event.kind != lubancode::trajectory::v3::EventKindV3::ModelUsageObserved) continue;
        ++observations;
        CHECK(event.session_id == id); CHECK(event.turn_id == operation->turn_id);
        if (!operation->usage->attempts.empty()) CHECK(event.request_id == operation->usage->attempts.front().trajectory_request_id);
        CHECK(event.payload.at("numbers") == nlohmann::json::array({11, 7, 13, 17, 3}));
        const auto response = event.payload.value("providerResponseId", nlohmann::json());
        if (!ordinal || !response.is_null()) CHECK(response == "actual-allocation-calibration-response");
    }
    run.observations = observations;
    REQUIRE(observations <= 1);
    if (!ordinal) CHECK(observations == 1);
    for (const auto& attempt : operation->usage->attempts) {
        const auto bound = lubancore::detail::usage_result::ValidateRequestBinding(attempt, *ledger);
        if (!bound) {
            CHECK_FALSE(operation->result_persisted);
            CHECK(operation->state == sdk::OperationState::Indeterminate);
        }
    }
    session.reset();
    std::atomic<unsigned> replays{0};
    auto recovery_options = fixture.Options(); recovery_options.resume_session_id = id;
    recovery_options.custom_tools.push_back(tool_sentinel());
    recovery_options.backend = std::make_unique<RecoveryAllocationBackend>(replays);
    auto resumed = (*runtime)->OpenSession(std::move(recovery_options)); REQUIRE(resumed);
    CHECK((*resumed)->id() == id);
    const auto recovered = (*resumed)->ReadOperation(receipt->operation_id); REQUIRE(recovered);
    WriteAllocationMaterial(fixture.material_target / "recovered.json", OperationMaterial(*recovered));
    if (!ordinal) CheckReturnedOperation(*recovered, path);
    CHECK(recovered->operation_id == operation->operation_id);
    CHECK(recovered->result_persisted == operation->result_persisted);
    if (run.finals == 1 && result_record.is_object()) {
        CHECK(recovered->turn_id == operation->turn_id);
        CHECK(recovered->state == operation->state);
        CHECK(recovered->final_text == operation->final_text);
        REQUIRE(recovered->usage);
        CHECK(lubancore::detail::usage_result::Encode(*recovered->usage) == result_record.at("usage"));
    } else {
        CHECK(recovered->state == sdk::OperationState::Indeterminate);
        CHECK_FALSE(recovered->result_persisted); CHECK(recovered->final_text.empty());
        CHECK_FALSE(recovered->usage); CHECK_FALSE(recovered->error.empty());
    }
    REQUIRE((*resumed)->Close()); REQUIRE((*runtime)->Shutdown()); CHECK(replays.load() == 0);
    REQUIRE(tools.load() == 0);
    fixture.Archive();
    std::cout << "[sdk-returned-allocation-materials] path=" << static_cast<unsigned>(path)
              << " ordinal=" << ordinal << " directory=" << nlohmann::json(platform::PathToUtf8(fixture.material_target)).dump() << '\n';
    return run;
}
}

TEST_CASE("SDK allocation ownership: real shared image and matching host release") {
    const ScopedAllocationTerminationDiagnostic termination_diagnostic;
    const auto port = lubancore::detail::testing::GetAllocationQualificationPort();
    REQUIRE(port.allocate);
    REQUIRE(port.release);
    REQUIRE(port.allocate != HostAllocate);
    REQUIRE(port.release != HostRelease);
    const auto allocate_image = ImageOf(reinterpret_cast<const void*>(port.allocate));
    const auto release_image = ImageOf(reinterpret_cast<const void*>(port.release));
    const auto host_image = ImageOf(reinterpret_cast<const void*>(&HostAllocate));
    const std::filesystem::path expected(LUBANCORE_TEST_SHARED_MODULE);
    std::error_code ec;
    REQUIRE(std::filesystem::equivalent(allocate_image, expected, ec));
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::equivalent(release_image, expected, ec));
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(std::filesystem::equivalent(host_image, expected, ec));
    REQUIRE_FALSE(ec);
    // Preserve this exact loaded SDK for offline hidden-symbol/offset diagnosis.
    // This copy is never loaded, called, or used as a substitute test module.
    const auto module_copy = std::filesystem::path(LUBANCORE_TEST_ALLOCATION_EVIDENCE_ROOT) / "module-image.bin";
    std::filesystem::create_directories(module_copy.parent_path());
    std::filesystem::copy_file(expected, module_copy, std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    std::cout << "[sdk-allocation-module-copy] source=" << nlohmann::json(platform::PathToUtf8(expected)).dump()
              << " copy=" << nlohmann::json(platform::PathToUtf8(module_copy)).dump() << '\n' << std::flush;
    unsigned paths = 0;
    for (unsigned form = 0; form != 8; ++form) {
        const auto kind = static_cast<Kind>(form);
        for (const bool sized : {false, true}) for (const std::size_t size : {0U, 1U, 257U}) {
            for (unsigned route = 0; route != 3; ++route) {
                CAPTURE(form);
                CAPTURE(sized);
                CAPTURE(size);
                CAPTURE(route);
                const std::size_t alignment = 4096;
                const auto allocate = route == 2 ? HostAllocate : port.allocate;
                const auto release = route == 0 ? port.release : route == 1 ? HostRelease : port.release;
                OwnedAllocation value{allocate(size, alignment, kind), release, size, alignment, kind, sized};
                REQUIRE(value.pointer);
                if (form & 4U) CHECK(reinterpret_cast<std::uintptr_t>(value.pointer) % alignment == 0);
                auto* bytes = static_cast<unsigned char*>(value.pointer);
                for (std::size_t i = 0; i != size; ++i) bytes[i] = 0xA5;
                for (std::size_t i = 0; i != size; ++i) CHECK(bytes[i] == 0xA5);
                // Release now, before counting or reporting the successful path.
                value.release(value.pointer, size, alignment, kind, sized);
                value.pointer = nullptr;
                ++paths;
                std::cout << "[sdk-allocation-owner-route] " << form << ' ' << sized << ' ' << size << ' ' << route << '\n';
            }
        }
    }
    CHECK(paths == 144);
    std::cout << "[sdk-allocation-owner] qualified_paths=" << paths
              << " image=" << allocate_image.string() << '\n';
    // Warm all eight actual SDK imports above before inspecting their resolved
    // addresses. The host executable's allocation imports are never patched.
    const auto failure = lubancore::detail::testing::GetAllocationFailurePort();
    REQUIRE(failure.install);
    REQUIRE(failure.restore);
    REQUIRE(failure.begin);
    REQUIRE(failure.end);
    REQUIRE(failure.prepare_returned_backend);
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.install)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.restore)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.begin)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.end)), expected));
    OwnedRefusalImports imports{failure, failure.install()};
    if (!imports.installed) {
        const auto diagnostic = lubancore::detail::testing::GetAllocationInstallationDiagnostic();
        std::cout << "[sdk-module-installation-failure] stage=" << diagnostic.stage
                  << " forms_mask=" << diagnostic.forms_mask << " slots=" << diagnostic.slots
                  << " body_targets=" << diagnostic.body_targets
                  << " body_error=" << diagnostic.body_error
                  << " aliases=" << diagnostic.aliases << " overflow=" << diagnostic.collection_overflow << '\n';
        for (unsigned form = 0; form != 8; ++form) {
            const auto* address = diagnostic.original_addresses[form];
            std::cout << "[sdk-module-original] form=" << form << " address=" << address;
            try { std::cout << " image=" << ImageOf(address).string(); }
            catch (const std::exception& error) { std::cout << " image_error=" << error.what(); }
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION region{};
            const auto start = reinterpret_cast<std::uintptr_t>(address);
            if (VirtualQuery(address, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT &&
                !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                (region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                  PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                start >= reinterpret_cast<std::uintptr_t>(region.BaseAddress) &&
                region.RegionSize >= 16 &&
                start - reinterpret_cast<std::uintptr_t>(region.BaseAddress) <= region.RegionSize - 16) {
                std::cout << " entry_bytes=";
                const auto* bytes = static_cast<const unsigned char*>(address);
                for (unsigned n = 0; n != 16; ++n) std::cout << (n ? "," : "") << static_cast<unsigned>(bytes[n]);
            }
#endif
            std::cout << '\n';
        }
        std::cout.flush();
    }
    REQUIRE(imports.installed);
    const auto installed = lubancore::detail::testing::GetAllocationInstallationDiagnostic();
    CHECK(installed.forms_mask == 255);
    CHECK(((installed.body_targets == 8 && installed.slots == 0 && installed.body_error == 0) ||
           (installed.body_targets == 0 && installed.slots >= 8)));
    std::cout << "[sdk-module-installation] slots=" << installed.slots
              << " body_targets=" << installed.body_targets << " body_error=" << installed.body_error
              << " forms_mask=" << installed.forms_mask << '\n' << std::flush;
    for (unsigned form = 0; form != 8; ++form) {
        CAPTURE(form);
        const auto kind = static_cast<Kind>(form);
        lubancore::detail::testing::AllocationFailureControl baseline;
        OwnedRefusalScope normal{failure, failure.begin(baseline)};
        REQUIRE(normal.active);
        // Nested scopes and import restoration during a live borrow are errors.
        CHECK_FALSE(failure.begin(baseline));
        CHECK_FALSE(failure.restore());
        OwnedAllocation host{HostAllocate(257, 4096, kind), HostRelease, 257, 4096, kind, false};
        REQUIRE(host.pointer);
        const auto host_count = baseline.allocations.load();
        OwnedAllocation sdk{port.allocate(257, 4096, kind), port.release, 257, 4096, kind, false};
        REQUIRE(sdk.pointer);
        normal.End();
        CHECK(host_count == 0);
        CHECK(baseline.allocations.load() == 1);
        CHECK(baseline.refusals.load() == 0);
        CHECK(baseline.last_kind.load() == form);
        // Release real storage with the unchanged corresponding operators.
        sdk.release(sdk.pointer, 257, 4096, kind, false); sdk.pointer = nullptr;
        host.release(host.pointer, 257, 4096, kind, false); host.pointer = nullptr;
        lubancore::detail::testing::AllocationFailureControl refused;
        refused.refuse_ordinal = 1;
        OwnedRefusalScope fault{failure, failure.begin(refused)};
        REQUIRE(fault.active);
        void* returned = nullptr;
        bool bad_alloc = false;
        try { returned = port.allocate(257, 4096, kind); }
        catch (const std::bad_alloc&) { bad_alloc = true; }
        fault.End();
        // If a broken controller allocated anyway, release before assertions.
        if (returned) port.release(returned, 257, 4096, kind, false);
        CHECK(returned == nullptr);
        CHECK(bad_alloc == ((form & 2U) == 0));
        CHECK(refused.allocations.load() == 1);
        CHECK(refused.refusals.load() == 1);
        CHECK(refused.last_kind.load() == form);
        std::cout << "[sdk-module-refusal] form=" << form
                  << " baseline=" << baseline.allocations.load()
                  << " refused=" << refused.refusals.load() << " host=" << host_count << '\n';
    }
    const auto json_port = lubancore::detail::testing::GetJsonRetirementQualificationPort();
    REQUIRE(json_port.make); REQUIRE(json_port.inspect); REQUIRE(json_port.retire);
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(json_port.make)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(json_port.inspect)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(json_port.retire)), expected));
    for (const bool ordered : {false, true}) for (const std::size_t depth : {0U, 32U, 16384U}) {
        CAPTURE(ordered); CAPTURE(depth);
        struct JsonOwner {
            decltype(json_port) port;
            void* value;
            bool ordered;
            ~JsonOwner() { if (value) port.retire(value, ordered); }
        } document{json_port, json_port.make(depth, ordered), ordered};
        REQUIRE(document.value);
        REQUIRE(json_port.inspect(document.value, depth, ordered));
        lubancore::detail::testing::AllocationFailureControl armed;
        armed.refuse_ordinal = 1;
        OwnedRefusalScope scope{failure, failure.begin(armed)};
        REQUIRE(scope.active);
        json_port.retire(document.value, ordered); document.value = nullptr;
        const auto retirement_allocations = armed.allocations.load();
        // An ordinary allocation in the same scope proves retirement neither
        // disabled the control nor silently consumed its refusal ordinal.
        OwnedAllocation sentinel{port.allocate(257, 4096, Kind::ScalarNoThrow), port.release,
                                 257, 4096, Kind::ScalarNoThrow, false};
        scope.End();
        CHECK(retirement_allocations == 0);
        CHECK(sentinel.pointer == nullptr);
        CHECK(armed.allocations.load() == 1); CHECK(armed.refusals.load() == 1);
        CHECK(armed.last_kind.load() == static_cast<unsigned>(Kind::ScalarNoThrow));
        std::cout << "[sdk-json-retirement] ordered=" << ordered << " depth=" << depth
                  << " allocations=" << retirement_allocations << " sentinel_refused=" << armed.refusals.load()
                  << '\n' << std::flush;
    }
    for (const auto path : {ReturnedPath::Success, ReturnedPath::Cancel, ReturnedPath::Close}) {
        const auto baseline = RunReturnedSession(failure, path);
        std::cout << "[sdk-returned-allocation-baseline] path=" << static_cast<unsigned>(path)
                  << " allocations=" << baseline.allocations << " refusals=0 retired=1 recovery_calls=0\n";
        for (std::uint64_t ordinal = 1; ordinal <= baseline.allocations; ++ordinal) {
            CAPTURE(path); CAPTURE(ordinal);
            std::cout << "[sdk-returned-allocation-begin] path=" << static_cast<unsigned>(path)
                      << " ordinal=" << ordinal << '\n' << std::flush;
            const auto point = RunReturnedSession(failure, path, ordinal);
            std::cout << "[sdk-returned-allocation-point] path=" << static_cast<unsigned>(path)
                      << " ordinal=" << ordinal << " allocations=" << point.allocations
                      << " phase=" << point.phase << " refusals=1 retired=1 recovery_calls=0"
                      << " state=" << static_cast<unsigned>(point.state) << " persisted=" << point.persisted
                      << " finals=" << point.finals << " observations=" << point.observations
                      << " terminals=" << point.terminals << '\n';
        }
        std::cout << "[sdk-returned-allocation-sweep] path=" << static_cast<unsigned>(path)
                  << " baseline=" << baseline.allocations << " points=" << baseline.allocations << '\n';
    }
    REQUIRE(failure.restore());
    imports.installed = false;
    // Real allocation still works after restoring all imports.
    OwnedAllocation restored{port.allocate(257, 4096, Kind::Scalar), port.release, 257, 4096, Kind::Scalar, false};
    REQUIRE(restored.pointer);
    for (unsigned form = 0; form != 8; ++form) {
        CAPTURE(form);
        const auto kind = static_cast<Kind>(form);
        OwnedAllocation ordinary{port.allocate(257, 4096, kind), port.release, 257, 4096, kind, false};
        REQUIRE(ordinary.pointer);
        auto* bytes = static_cast<unsigned char*>(ordinary.pointer);
        bytes[0] = 0x35; bytes[256] = 0x71;
        CHECK(bytes[0] == 0x35); CHECK(bytes[256] == 0x71);
    }
    std::cout << "[sdk-module-refusal] qualified_forms=8 imports_restored=1\n";
}
