#include "trajectory/session_recovery_view.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <string_view>

#include "hooks/hash.hpp"
#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::trajectory {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kPlanBytes = 4096, kReportBytes = 524288;
constexpr std::size_t kResultBytes = 67108864, kReportTotal = 67108864, kReportEntries = 4096;

bool SafeId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
bool SafeText(const std::string& value) {
    return value.find('\0') == std::string::npos && platform::IsValidUtf8(value);
}
bool Consume(std::size_t& used, std::size_t count, std::optional<std::size_t> cap) {
    if (count > static_cast<std::size_t>(-1) - used || (cap && (used > *cap || count > *cap - used))) return false;
    used += count;
    return true;
}
struct Accounting { std::size_t bytes = 0, names = 0, entries = 0, results = 0, reports = 0; };
std::size_t Remaining(std::size_t per_entity, std::size_t used, std::optional<std::size_t> cap) {
    return cap ? (std::min)(per_entity, used <= *cap ? *cap - used : 0) : per_entity;
}
fs::path CanonicalLogical(const fs::path& path, std::error_code& ec) {
    auto real = fs::canonical(platform::FileIoPath(path), ec);
#ifdef _WIN32
    // directory_iterator on an extended I/O path returns that spelling. Keep
    // canonical comparisons in one logical DOS/UNC namespace, never in refs.
    const auto native = real.native();
    if (native.starts_with(L"\\\\?\\UNC\\")) real = fs::path(L"\\\\" + native.substr(8));
    else if (native.starts_with(L"\\\\?\\") && native.size() >= 7 && native[5] == L':')
        real = fs::path(native.substr(4));
#endif
    return real;
}
bool Inside(const fs::path& path, const fs::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p)
        if (p == path.end() || *p != *r) return false;
    return true;
}
std::expected<RecoveryValue, std::string> ReadMetadata(const fs::path& path,
    const fs::path& root, std::size_t cap) {
    std::error_code ec;
    const auto status = fs::symlink_status(platform::FileIoPath(path), ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return RecoveryValue{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected("recovery.metadata_invalid_file");
    const auto real = CanonicalLogical(path, ec);
    if (ec || !Inside(real, root)) return std::unexpected("recovery.metadata_path_escape");
    auto bytes = platform::ReadBoundedRegularFile(platform::FileIoPath(real), cap);
    if (!bytes) return std::unexpected("recovery.metadata." + bytes.error());
    if (!SafeText(*bytes)) return std::unexpected("recovery.invalid_text");
    return RecoveryValue{RecoveryReadState::Value, std::move(*bytes), {}};
}

std::expected<void, std::string> CaptureDirectory(SessionRecoveryView& view,
    const fs::path& session_dir, const fs::path& root, RecoveryKeyKind kind,
    RecoveryDirectory& listing, const RecoveryCaptureRequest& request, Accounting& accounting) {
    const auto path = session_dir / (kind == RecoveryKeyKind::SdkResult ? "sdk-results" : "sdk-memory-recalls");
    std::error_code ec;
    const auto status = fs::symlink_status(platform::FileIoPath(path), ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) return {};
    if (ec || fs::is_symlink(status) || !fs::is_directory(status)) return std::unexpected("recovery.metadata_invalid_directory");
    const auto real = CanonicalLogical(path, ec);
    if (ec || real != root / path.filename()) return std::unexpected("recovery.metadata_path_escape");
    listing.present = true;
    fs::directory_iterator iterator(platform::FileIoPath(real), ec), end;
    if (ec) return std::unexpected("recovery.enumeration_failed");
    while (iterator != end) {
        if ((kind == RecoveryKeyKind::MemoryRecall && listing.entries.size() >= kReportEntries) ||
            (kind == RecoveryKeyKind::SdkResult && request.limits && listing.entries.size() >= request.limits->result_directory_entries) ||
            !Consume(accounting.entries, 1, request.limits ? std::optional(request.limits->view_directory_entries) : std::nullopt))
            return std::unexpected("recovery.directory_limit");
        const auto file = real / iterator->path().filename();
        const auto file_status = iterator->symlink_status(ec);
        if (ec) return std::unexpected("recovery.enumeration_failed");
        const auto name = platform::PathToUtf8(file.filename());
        if (!SafeText(name)) return std::unexpected("recovery.invalid_name");
        if ((request.limits && name.size() > request.limits->directory_name_bytes) ||
            !Consume(accounting.names, name.size(), request.limits ? std::optional(request.limits->directory_name_total_bytes) : std::nullopt) ||
            !Consume(accounting.bytes, name.size(), request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt))
            return std::unexpected("recovery.name_limit");
        const bool temporary = file.extension() == ".tmp";
        listing.entries.push_back({name, fs::is_regular_file(file_status) && !fs::is_symlink(file_status), temporary});
        // Enumerate tmp and preserve its type, without adopting or reading it.
        if (!temporary) {
            const auto id = platform::PathToUtf8(file.stem());
            if (file.extension() != ".json" || !SafeId(id)) return std::unexpected("recovery.metadata_invalid_name");
            auto& used = kind == RecoveryKeyKind::SdkResult ? accounting.results : accounting.reports;
            const auto group_cap = kind == RecoveryKeyKind::SdkResult
                ? (request.limits ? std::optional(request.limits->result_total_bytes) : std::nullopt)
                : std::optional(kReportTotal);
            auto cap = Remaining(kind == RecoveryKeyKind::SdkResult ? kResultBytes : kReportBytes, used, group_cap);
            cap = Remaining(cap, accounting.bytes, request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt);
            auto bytes = ReadMetadata(file, root, cap);
            if (!bytes) return std::unexpected(bytes.error());
            if (!Consume(used, bytes->bytes.size(), group_cap) ||
                !Consume(accounting.bytes, bytes->bytes.size(), request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt))
                return std::unexpected("recovery.view_limit");
            if (!view.values.emplace(RecoveryKey{kind, id}, std::move(*bytes)).second)
                return std::unexpected("recovery.duplicate_key");
        }
        iterator.increment(ec);
        if (ec) return std::unexpected("recovery.enumeration_failed");
    }
    std::sort(listing.entries.begin(), listing.entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return {};
}
} // namespace

bool ValidRecoveryReadLimits(const RecoveryReadLimits& limits) {
    const auto stream = [](const auto& value) { return value.max_bytes && value.max_lines && value.max_line_bytes; };
    return stream(limits.journal) && stream(limits.operations) && limits.result_total_bytes &&
        limits.view_total_bytes && limits.result_directory_entries && limits.view_directory_entries &&
        limits.directory_name_bytes && limits.directory_name_total_bytes;
}
const RecoveryValue* SessionRecoveryView::Find(RecoveryKeyKind kind, const std::string& operation_id) const {
    const auto found = values.find({kind, operation_id});
    return found == values.end() ? nullptr : &found->second;
}
std::expected<std::vector<std::string>, std::string> RecoveryStreamLines(
    std::string_view bytes, const std::optional<RecoveryStreamReadLimits>& limits, bool skip_empty_lines) {
    if (limits && (bytes.size() > limits->max_bytes || !limits->max_lines || !limits->max_line_bytes))
        return std::unexpected("recovery.stream_limit");
    if (!bytes.empty() && bytes.back() != '\n') return std::unexpected("recovery.truncated_tail");
    std::vector<std::string> lines;
    std::size_t start = 0, physical_lines = 0;
    while (start < bytes.size()) {
        const auto end = bytes.find('\n', start);
        if (end == std::string_view::npos) return std::unexpected("recovery.truncated_tail");
        if (limits && (end - start > limits->max_line_bytes || physical_lines >= limits->max_lines))
            return std::unexpected("recovery.stream_limit");
        ++physical_lines;
        if (end == start && !skip_empty_lines) return std::unexpected("recovery.empty_line");
        if (end > start) lines.emplace_back(bytes.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::expected<void, std::string> CheckRecoveryView(const SessionRecoveryView& view,
    const RecoveryCaptureRequest& request) {
    if (view.workspace_key.empty() || !SafeId(view.session_id) || view.stream != "main" ||
        (request.limits && !ValidRecoveryReadLimits(*request.limits))) return std::unexpected("recovery.invalid_scope_or_limits");
    const auto* main = view.Find(RecoveryKeyKind::MainV3);
    if (!main || main->state != RecoveryReadState::Value || main->bytes.empty()) return std::unexpected("recovery.main_missing");
    if (view.snapshot_token != hooks::Sha256Hex(main->bytes)) return std::unexpected("recovery.snapshot_mismatch");
    std::size_t total = 0, report_total = 0, result_total = 0, name_total = 0, entries_total = 0;
    const auto view_cap = request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt;
    for (const auto& [key, value] : view.values) {
        const bool keyed = key.kind == RecoveryKeyKind::SdkResult || key.kind == RecoveryKeyKind::MemoryRecall;
        if ((keyed && !SafeId(key.operation_id)) || (!keyed && !key.operation_id.empty())) return std::unexpected("recovery.invalid_key");
        if (!request.memory_metadata && key.kind != RecoveryKeyKind::MainV3) return std::unexpected("recovery.unselected_metadata");
        if (!value.error.empty() || (value.state != RecoveryReadState::Absent && value.state != RecoveryReadState::Value) ||
            (value.state == RecoveryReadState::Absent && !value.bytes.empty())) return std::unexpected("recovery.invalid_read_state");
        if (!SafeText(value.bytes)) return std::unexpected("recovery.invalid_text");
        if (!Consume(total, value.bytes.size(), view_cap)) return std::unexpected("recovery.view_limit");
        if (key.kind == RecoveryKeyKind::MemoryPlan && value.bytes.size() > kPlanBytes) return std::unexpected("recovery.plan_limit");
        if (key.kind == RecoveryKeyKind::MemoryRecall && (value.bytes.size() > kReportBytes || !Consume(report_total, value.bytes.size(), kReportTotal)))
            return std::unexpected("recovery.report_limit");
        if (key.kind == RecoveryKeyKind::SdkResult && (value.bytes.size() > kResultBytes ||
            !Consume(result_total, value.bytes.size(), request.limits ? std::optional(request.limits->result_total_bytes) : std::nullopt)))
            return std::unexpected("recovery.result_limit");
        if (key.kind == RecoveryKeyKind::MainV3 || key.kind == RecoveryKeyKind::Operations) {
            const auto limits = request.limits ? std::optional(key.kind == RecoveryKeyKind::MainV3 ? request.limits->journal : request.limits->operations) : std::nullopt;
            auto lines = RecoveryStreamLines(value.bytes, limits, key.kind == RecoveryKeyKind::MainV3);
            if (!lines) return std::unexpected(lines.error());
        }
    }
    const auto check_listing = [&](const RecoveryDirectory& list, RecoveryKeyKind kind) -> std::expected<void, std::string> {
        if (!list.present && !list.entries.empty()) return std::unexpected("recovery.invalid_directory_state");
        if (!request.memory_metadata && list.present) return std::unexpected("recovery.unselected_metadata");
        if ((kind == RecoveryKeyKind::MemoryRecall && list.entries.size() > kReportEntries) ||
            (kind == RecoveryKeyKind::SdkResult && request.limits && list.entries.size() > request.limits->result_directory_entries) ||
            !Consume(entries_total, list.entries.size(), request.limits ? std::optional(request.limits->view_directory_entries) : std::nullopt))
            return std::unexpected("recovery.directory_limit");
        std::set<std::string> names, ids;
        for (const auto& entry : list.entries) {
            if (entry.name.empty() || !SafeText(entry.name) || entry.name.find_first_of("/\\") != std::string::npos || !names.insert(entry.name).second)
                return std::unexpected("recovery.invalid_name");
            if ((request.limits && entry.name.size() > request.limits->directory_name_bytes) ||
                !Consume(name_total, entry.name.size(), request.limits ? std::optional(request.limits->directory_name_total_bytes) : std::nullopt) ||
                !Consume(total, entry.name.size(), view_cap)) return std::unexpected("recovery.name_limit");
            const auto name = fs::u8path(entry.name);
            if (entry.temporary != (name.extension() == ".tmp")) return std::unexpected("recovery.invalid_directory_type");
            if (entry.temporary) continue;
            const auto id = platform::PathToUtf8(name.stem());
            const auto* bytes = view.Find(kind, id);
            if (!entry.regular || name.extension() != ".json" || !SafeId(id) || !ids.insert(id).second ||
                !bytes || bytes->state != RecoveryReadState::Value) return std::unexpected("recovery.incomplete_roster");
        }
        for (const auto& [key, value] : view.values)
            if (key.kind == kind && !ids.contains(key.operation_id)) return std::unexpected("recovery.unlisted_value");
        return {};
    };
    auto results = check_listing(view.results, RecoveryKeyKind::SdkResult);
    if (!results) return results;
    auto reports = check_listing(view.reports, RecoveryKeyKind::MemoryRecall);
    if (!reports) return reports;
    if (request.memory_metadata && (!view.Find(RecoveryKeyKind::MemoryPlan) || !view.Find(RecoveryKeyKind::Operations)))
        return std::unexpected("recovery.incomplete_roster");
    return {};
}

std::expected<SessionRecoveryCapture, std::string> CaptureSessionRecovery(
    const fs::path& session_dir, std::string workspace_key, std::string session_id,
    const RecoveryCaptureRequest& request, const SessionRecoveryFactory& factory) {
    if (request.limits && !ValidRecoveryReadLimits(*request.limits)) return std::unexpected("recovery.invalid_limits");
    if (workspace_key.empty() || !SafeId(session_id)) return std::unexpected("recovery.invalid_scope_or_limits");
    auto main = JournalOwner::CaptureExisting(session_dir / (session_id + ".jsonl"),
        request.limits ? std::optional((std::min)(request.limits->journal.max_bytes, request.limits->view_total_bytes)) : std::nullopt);
    if (!main) return std::unexpected(main.error());
    const auto fail = [&](std::string error) -> std::expected<SessionRecoveryCapture, std::string> {
        auto closed = main->Close();
        if (!closed) error += "; " + closed.error();
        return std::unexpected(std::move(error));
    };
    try {
        SessionRecoveryView view;
        view.workspace_key = std::move(workspace_key);
        view.session_id = std::move(session_id);
        view.snapshot_token = hooks::Sha256Hex(main->bytes());
        Accounting accounting;
        accounting.bytes = main->bytes().size();
        // The domain factory sees owned values, never the native handle. It
        // cannot replace the actual captured main bytes or its native identity.
        view.values.emplace(RecoveryKey{RecoveryKeyKind::MainV3, {}}, RecoveryValue{RecoveryReadState::Value, main->bytes(), {}});
        if (request.memory_metadata) {
            std::error_code ec;
            const auto root = CanonicalLogical(session_dir, ec);
            if (ec) return fail("recovery.metadata_path_escape");
            auto plan = ReadMetadata(session_dir / "sdk-memory-plan.json", root,
                Remaining(kPlanBytes, accounting.bytes, request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt));
            if (!plan) return fail(plan.error());
            if (!Consume(accounting.bytes, plan->bytes.size(), request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt))
                return fail("recovery.view_limit");
            view.values.emplace(RecoveryKey{RecoveryKeyKind::MemoryPlan, {}}, std::move(*plan));
            // Operations have no old aggregate cap; optional limits use the native
            // chunked reader, then retain strict metadata no-follow semantics.
            const auto operations_path = session_dir / "operations.jsonl";
            const auto op_status = fs::symlink_status(platform::FileIoPath(operations_path), ec);
            if (ec == std::errc::no_such_file_or_directory || (!ec && op_status.type() == fs::file_type::not_found)) {
                view.values.emplace(RecoveryKey{RecoveryKeyKind::Operations, {}}, RecoveryValue{});
            } else {
                if (ec || fs::is_symlink(op_status) || !fs::is_regular_file(op_status)) return fail("recovery.metadata_invalid_file");
                const auto real = CanonicalLogical(operations_path, ec);
                if (ec || !Inside(real, root)) return fail("recovery.metadata_path_escape");
                const auto op_cap = request.limits
                    ? std::optional(Remaining(request.limits->operations.max_bytes, accounting.bytes, request.limits->view_total_bytes))
                    : std::nullopt;
                auto op = JournalFileAnchor::ReadExisting(real, op_cap, false);
                if (!op) return fail(op.error());
                auto closed = op->anchor->Close();
                if (!closed) return fail(closed.error());
                if (!Consume(accounting.bytes, op->bytes.size(), request.limits ? std::optional(request.limits->view_total_bytes) : std::nullopt))
                    return fail("recovery.view_limit");
                view.values.emplace(RecoveryKey{RecoveryKeyKind::Operations, {}}, RecoveryValue{RecoveryReadState::Value, std::move(op->bytes), {}});
            }
            auto results = CaptureDirectory(view, session_dir, root, RecoveryKeyKind::SdkResult, view.results, request, accounting);
            if (!results) return fail(results.error());
            auto reports = CaptureDirectory(view, session_dir, root, RecoveryKeyKind::MemoryRecall, view.reports, request, accounting);
            if (!reports) return fail(reports.error());
        }
        auto valid = CheckRecoveryView(view, request);
        if (!valid) return fail(valid.error());
        if (request.expected_main) {
            const auto& expected = *request.expected_main;
            const auto* actual = view.Find(RecoveryKeyKind::MainV3);
            if (expected.workspace_key != view.workspace_key || expected.session_id != view.session_id ||
                expected.stream != view.stream || !actual || actual->state != RecoveryReadState::Value ||
                !actual->error.empty() || actual->bytes.size() != expected.bytes ||
                hooks::Sha256Hex(actual->bytes) != expected.sha256)
                return fail("recovery.prepare_source_changed");
        }
        if (factory) {
            auto adapted = factory(view);
            if (!adapted) return fail(adapted.error());
            if (adapted->workspace_key != view.workspace_key || adapted->session_id != view.session_id || adapted->stream != view.stream ||
                adapted->snapshot_token != view.snapshot_token ||
                adapted->results != view.results || adapted->reports != view.reports || adapted->values.size() != view.values.size())
                return fail("recovery.reference_mismatch");
            for (const auto& [key, value] : view.values) {
                const auto* candidate = adapted->Find(key.kind, key.operation_id);
                if (!candidate || candidate->state != value.state) return fail("recovery.reference_mismatch");
            }
            const auto* candidate_main = adapted->Find(RecoveryKeyKind::MainV3);
            const auto* reference_main = view.Find(RecoveryKeyKind::MainV3);
            if (!candidate_main || candidate_main->bytes != reference_main->bytes)
                return fail("recovery.reference_mismatch");
            valid = CheckRecoveryView(*adapted, request);
            if (!valid) return fail(valid.error());
            view = std::move(*adapted);
        }
        auto anchor = main->native_anchor();
        return SessionRecoveryCapture{std::move(view), std::move(anchor), std::move(*main)};
    } catch (const std::exception& error) {
        return fail(std::string("recovery.capture_exception:") + error.what());
    } catch (...) {
        return fail("recovery.capture_exception");
    }
}
} // namespace lubancode::trajectory
