#include "trajectory/blob_store.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <limits>
#include <cerrno>
#include <vector>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "platform/bounded_read.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/safety.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lubancode::trajectory {
namespace {

// 文件内容落盘:ProcessCrash 档 fflush 即可;PowerLoss 档加 FlushFileBuffers
// /fsync(§7.4)。
bool FlushFileDurable(std::FILE* file, Durability durability) {
    if (std::fflush(file) != 0) {
        return false;
    }
    if (durability != Durability::PowerLoss) {
        return true;
    }
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return FlushFileBuffers(handle) != FALSE;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

// New typed receipts only confirm PowerLoss after native directory flush and
// close succeed. The old wrapper retains its documented best-effort boundary.
bool FlushDirectoryChecked(const std::filesystem::path& dir) {
#ifdef _WIN32
    const HANDLE handle =
        CreateFileW(dir.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    return flushed && closed;
#else
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool flushed = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    return flushed && closed;
#endif
}

std::uint64_t NextTmpCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1) + 1;
}

}  // namespace

BlobStore::BlobStore(std::filesystem::path root, BlobStoreOptions options)
    : root_(std::move(root)), options_(options) {}

nlohmann::json BlobRef::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["sha256"] = sha256;
    json["size"] = size;
    json["media_type"] = media_type;
    json["encoding"] = encoding;
    json["compression"] = compression;
    return json;
}

std::optional<BlobRef> BlobRef::FromJson(const nlohmann::json& json) {
    if (!MatchesShape(json)) {
        return std::nullopt;
    }
    BlobRef ref;
    ref.sha256 = json.at("sha256").get<std::string>();
    ref.size = json.at("size").get<std::uint64_t>();
    ref.media_type = json.at("media_type").get<std::string>();
    ref.encoding = json.at("encoding").get<std::string>();
    ref.compression = json.at("compression").get<std::string>();
    return ref;
}

bool BlobRef::MatchesShape(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 5) {
        return false;
    }
    static constexpr const char* kKeys[5] = {"sha256", "size", "media_type", "encoding",
                                             "compression"};
    for (const char* key : kKeys) {
        if (!json.contains(key)) {
            return false;
        }
    }
    return json.at("sha256").is_string() && json.at("media_type").is_string() &&
           json.at("encoding").is_string() && json.at("compression").is_string() &&
           json.at("size").is_number_unsigned() &&
           IsHex64(json.at("sha256").get<std::string>());
}

std::expected<BlobRef, std::string> BlobStore::Store(std::string_view data, std::string media_type,
                                                     Durability durability) {
    const CasDurability required = durability == Durability::PowerLoss ? CasDurability::PowerLoss :
        durability == Durability::ProcessCrash ? CasDurability::ProcessCrash : CasDurability::Buffered;
    CasReference reference{{"legacy-file", "legacy-file"}, hooks::Sha256Hex(data),
        static_cast<std::uint64_t>(data.size()), std::move(media_type)};
    const auto receipt = StoreDetailed({reference, data, required});
    // Compatibility only: the old API promised best-effort directory flushing.
    // New Memory consumers use StoreDetailed through their owned capability and
    // require the requested actual durability; this wrapper is not SPI proof.
    if (receipt.state != CasCommitState::Committed)
        return std::unexpected(receipt.error.code + ": " + receipt.error.message);
    return BlobRef{reference.sha256, reference.bytes, reference.media_type, "utf-8", "none"};
}

std::optional<std::string> BlobStore::ReadVerified(const BlobRef& ref) const {
    if (ref.size >= (std::numeric_limits<std::size_t>::max)()) return std::nullopt;
    auto bytes = ReadBoundedVerified({{"legacy-file", "legacy-file"}, ref.sha256, ref.size, ref.media_type},
        static_cast<std::size_t>(ref.size));
    if (!bytes) return std::nullopt;
    return std::move(*bytes);
}

std::expected<std::string, CasError> BlobStore::ReadBoundedVerified(
    const CasReference& ref, std::size_t cap) const {
    if (root_.empty() || !IsHex64(ref.sha256) || ref.bytes > cap ||
        cap == (std::numeric_limits<std::size_t>::max)())
        return std::unexpected(CasError{"cas.invalid_read", {}});
    const auto path = PathFor(ref.sha256);
    if (!IsSafeContainedPath(path, root_)) return std::unexpected(CasError{"cas.path_escape", {}});
    auto bytes = platform::ReadBoundedRegularFile(path, cap);
    if (!bytes) return std::unexpected(CasError{"cas." + bytes.error(), {}});
    if (bytes->size() != ref.bytes || hooks::Sha256Hex(*bytes) != ref.sha256)
        return std::unexpected(CasError{"cas.read_mismatch", {}});
    return bytes;
}

CasWriteReceipt BlobStore::StoreDetailed(const CasWriteRequest& request, const FileCasFault& fault) {
    const auto& ref = request.reference;
    auto fail = [&](CasCommitState state, std::string code, std::string text = {}) {
        return CasWriteReceipt{state, ref, std::nullopt, {std::move(code), std::move(text)}};
    };
    if (root_.empty() || !IsHex64(ref.sha256) || ref.sha256 != hooks::Sha256Hex(request.bytes) ||
        ref.bytes != request.bytes.size() || ref.media_type.empty() ||
        ref.media_type.find('\0') != std::string::npos || !platform::IsValidUtf8(ref.media_type))
        return fail(CasCommitState::NotCommitted, "cas.invalid_request");
    const auto target = PathFor(ref.sha256);
    if (!IsSafeContainedPath(target, root_)) return fail(CasCommitState::NotCommitted, "cas.path_escape");
    std::error_code error;
    std::vector<std::filesystem::path> new_directories;
    for (auto current = target.parent_path(); current != root_.parent_path() && !current.empty(); current = current.parent_path()) {
        const auto status = std::filesystem::symlink_status(current, error);
        if (error && error != std::errc::no_such_file_or_directory)
            return fail(CasCommitState::NotCommitted, "cas.directory_failed", error.message());
        if (status.type() == std::filesystem::file_type::not_found) new_directories.push_back(current);
        error.clear();
    }
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return fail(CasCommitState::NotCommitted, "cas.directory_failed", error.message());
    if (!IsSafeContainedPath(target, root_)) return fail(CasCommitState::NotCommitted, "cas.path_escape");

    const auto confirm = [&]() {
        CasWriteReceipt receipt{CasCommitState::Committed, ref, CasDurability::ProcessCrash, {}};
        if (request.required == CasDurability::PowerLoss) {
            if (!FlushDirectoryChecked(target.parent_path())) {
                receipt.error = {"cas.durability_unconfirmed", "published directory was not confirmed"};
                return receipt;
            }
            for (const auto& created : new_directories)
                if (!FlushDirectoryChecked(created.parent_path())) {
                    receipt.error = {"cas.durability_unconfirmed", "new directory parent was not confirmed"};
                    return receipt;
                }
            receipt.confirmed_durability = CasDurability::PowerLoss;
        }
        return receipt;
    };
    const auto reuse = [&]() {
        auto bytes = ReadBoundedVerified(ref, request.bytes.size());
        if (!bytes || *bytes != request.bytes)
            return fail(CasCommitState::NotCommitted, "cas.existing_corrupt", bytes ? std::string() : bytes.error().code);
        if (request.required == CasDurability::PowerLoss) {
#ifdef _WIN32
            std::FILE* file = _wfsopen(target.c_str(), L"r+b", _SH_DENYNO);
#else
            std::FILE* file = std::fopen(target.c_str(), "r+b");
#endif
            const bool flushed = file && FlushFileDurable(file, Durability::PowerLoss);
            const bool closed = file ? std::fclose(file) == 0 : false;
            if (!flushed || !closed)
                return CasWriteReceipt{CasCommitState::Committed, ref, CasDurability::ProcessCrash,
                    {"cas.durability_unconfirmed", "existing file was verified but flush/close was not confirmed"}};
        }
        return confirm();
    };
    const auto target_status = std::filesystem::symlink_status(target, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return fail(CasCommitState::NotCommitted, "cas.target_failed", error.message());
    if (target_status.type() != std::filesystem::file_type::not_found) return reuse();
    error.clear();
    auto temporary = target;
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = ::getpid();
#endif
    temporary += ".tmp-" + std::to_string(pid) + "-" + std::to_string(NextTmpCounter());
    struct RemoveTemporary {
        std::filesystem::path path;
        bool owned = false;
        ~RemoveTemporary() { if (owned) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
    } remove{temporary};
#ifdef _WIN32
    std::FILE* file = _wfsopen(temporary.c_str(), L"wbx", _SH_DENYNO);
    remove.owned = file != nullptr;
#else
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    remove.owned = fd >= 0;
    std::FILE* file = fd < 0 ? nullptr : ::fdopen(fd, "wb");
    if (fd >= 0 && !file) ::close(fd);
#endif
    if (!file) return fail(CasCommitState::NotCommitted, "cas.tmp_open_failed");
    const auto native_required = request.required == CasDurability::PowerLoss ? Durability::PowerLoss : Durability::ProcessCrash;
    bool wrote = request.bytes.empty() || std::fwrite(request.bytes.data(), 1, request.bytes.size(), file) == request.bytes.size();
    if (wrote) wrote = FlushFileDurable(file, native_required);
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) return fail(CasCommitState::NotCommitted, "cas.tmp_write_or_close_failed");
    if (fault) if (auto injected = fault(FileCasBoundary::AfterNativeClose))
        return fail(CasCommitState::NotCommitted, "cas.test_before_publish_rejected", *injected);
    if (!IsSafeContainedPath(target, root_)) return fail(CasCommitState::NotCommitted, "cas.path_escape");
    // No replace-existing publication. If another valid writer won, verify its
    // entire entity; an existing corrupt object is never silently repaired.
#ifdef _WIN32
    const bool published = MoveFileExW(temporary.c_str(), target.c_str(), 0) != FALSE;
    const auto publish_error = published ? ERROR_SUCCESS : GetLastError();
    const bool already_exists = publish_error == ERROR_ALREADY_EXISTS || publish_error == ERROR_FILE_EXISTS;
#else
    const bool published = ::link(temporary.c_str(), target.c_str()) == 0;
    const int publish_error = published ? 0 : errno;
    const bool already_exists = publish_error == EEXIST;
#endif
    if (!published) {
        if (already_exists) return reuse();
        return fail(CasCommitState::NotCommitted, "cas.publish_failed", std::to_string(publish_error));
    }
    std::filesystem::remove(temporary, error); // A cleanup fault may leave an orphan; it never unpublishes target.
    if (fault) if (auto injected = fault(FileCasBoundary::AfterPublish))
        return CasWriteReceipt{CasCommitState::Committed, ref, CasDurability::ProcessCrash,
            {"cas.test_publish_confirmation_failed", *injected}};
    return confirm();
}

std::filesystem::path BlobStore::PathFor(std::string_view sha256) const {
    // 前 2 字符一层分桶(§3.1 的 ab/<full-sha256>)。
    std::string prefix(sha256.substr(0, 2));
    return root_ / "sha256" / platform::Utf8ToPath(prefix) / platform::Utf8ToPath(std::string(sha256));
}

}  // namespace lubancode::trajectory
