#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <filesystem>
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
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Kind = lubancore::detail::testing::AllocationKind;
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
    ReturnedAllocationFixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() / ("sdk-actual-returned-alloc-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial.fetch_add(1)));
        std::filesystem::create_directories(root / "cwd");
        std::filesystem::create_directories(root / "resources");
    }
    ~ReturnedAllocationFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
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
    REQUIRE(attempt.observation); CHECK(attempt.observation->raw_fields.size() == 5);
    CHECK_FALSE(attempt.trajectory_request_id.empty());
}
std::uint64_t CalibrateReturnedSession(lubancore::detail::testing::AllocationFailurePort port, ReturnedPath path) {
    ReturnedAllocationFixture fixture;
    lubancore::detail::testing::AllocationFailureControl control;
    control.require_returned_backend = true;
    auto gate = std::make_shared<ReturnedGate>();
    auto runtime = sdk::Runtime::Create({platform::PathToUtf8(fixture.root / "data"), platform::PathToUtf8(fixture.root / "resources")});
    REQUIRE(runtime);
    auto options = fixture.Options();
    options.backend = std::make_unique<ReturnedAllocationBackend>(gate, port, control);
    auto opened = (*runtime)->OpenSession(std::move(options)); REQUIRE(opened);
    auto session = std::move(*opened); const auto id = session->id();
    auto events = session->Subscribe(); REQUIRE(events);
    std::future<sdk::Result<void>> closing;
    ReleaseReturnedGate release{gate}; // Unblocks before future/session destruction on every assertion failure.
    const auto receipt = session->Submit("actual-allocation-calibration", "calibrate the real SDK reply return");
    REQUIRE(receipt); REQUIRE(WaitReturnedFlag(gate->entered));
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
        REQUIRE(closing.wait_for(15s) == std::future_status::ready); REQUIRE(closing.get());
    } else {
        const auto complete = session->WaitResult(receipt->operation_id, 15s); REQUIRE(complete);
        CheckReturnedOperation(*complete, path);
        bool terminal = false;
        for (unsigned n = 0; n != 256 && !terminal; ++n) {
            const auto event = (*events)->Next(50ms); REQUIRE(event);
            if (!event->has_value() || (*event)->kind != "operation_completed") continue;
            CHECK((*event)->session_id == id); CHECK((*event)->operation_id == receipt->operation_id);
            CHECK((*event)->turn_id == complete->turn_id);
            const auto payload = nlohmann::json::parse((*event)->payload_json);
            CHECK(payload.at("resultPersisted") == true);
            CHECK(payload.at("status") == (path == ReturnedPath::Success ? "success" : "cancelled"));
            terminal = true;
        }
        REQUIRE(terminal); // The actual transient terminal is consumed before Close clears it.
        OwnedRefusalScope scope{port, port.begin(control)}; REQUIRE(scope.active);
        REQUIRE(session->Close());
    }
    CHECK(control.backend_returned.load()); CHECK(gate->prepared.load());
    CHECK(gate->destroyed.load()); CHECK_FALSE(gate->timed_out.load()); CHECK(gate->calls.load() == 1);
    CHECK(control.refusals.load() == 0);
    const auto count = control.allocations.load(); REQUIRE(count > 0);
    const auto operation = session->ReadOperation(receipt->operation_id); REQUIRE(operation);
    CheckReturnedOperation(*operation, path);
    const auto directory = fixture.SessionDir(id);
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(directory / (id + ".jsonl")); REQUIRE(ledger);
    unsigned observations = 0;
    for (const auto& event : ledger->events) {
        if (event.kind != lubancode::trajectory::v3::EventKindV3::ModelUsageObserved) continue;
        ++observations;
        CHECK(event.session_id == id); CHECK(event.turn_id == operation->turn_id);
        CHECK(event.request_id == operation->usage->attempts.front().trajectory_request_id);
        CHECK(event.payload.at("numbers") == nlohmann::json::array({11, 7, 13, 17, 3}));
        CHECK(event.payload.at("providerResponseId") == "actual-allocation-calibration-response");
    }
    CHECK(observations == 1);
    session.reset();
    std::atomic<unsigned> replays{0};
    auto recovery_options = fixture.Options(); recovery_options.resume_session_id = id;
    recovery_options.backend = std::make_unique<RecoveryAllocationBackend>(replays);
    auto resumed = (*runtime)->OpenSession(std::move(recovery_options)); REQUIRE(resumed);
    CHECK((*resumed)->id() == id);
    const auto recovered = (*resumed)->ReadOperation(receipt->operation_id); REQUIRE(recovered);
    CheckReturnedOperation(*recovered, path);
    CHECK(recovered->operation_id == operation->operation_id); CHECK(recovered->turn_id == operation->turn_id);
    REQUIRE((*resumed)->Close()); REQUIRE((*runtime)->Shutdown()); CHECK(replays.load() == 0);
    return count;
}
}

TEST_CASE("SDK allocation ownership: real shared image and matching host release") {
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
    REQUIRE(imports.installed);
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
    for (const auto path : {ReturnedPath::Success, ReturnedPath::Cancel, ReturnedPath::Close}) {
        const auto count = CalibrateReturnedSession(failure, path);
        std::cout << "[sdk-returned-allocation-baseline] path=" << static_cast<unsigned>(path)
                  << " allocations=" << count << " refusals=0 retired=1 recovery_calls=0\n";
    }
    REQUIRE(failure.restore());
    imports.installed = false;
    // Real allocation still works after restoring all imports.
    OwnedAllocation restored{port.allocate(257, 4096, Kind::Scalar), port.release, 257, 4096, Kind::Scalar, false};
    REQUIRE(restored.pointer);
    std::cout << "[sdk-module-refusal] qualified_forms=8 imports_restored=1\n";
}
