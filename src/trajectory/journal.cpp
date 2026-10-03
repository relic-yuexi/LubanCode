#include "trajectory/journal.hpp"

#include <cstdio>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <exception>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/schema.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lubancode::trajectory {
namespace {

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

}  // namespace

struct JournalFileAnchor::Impl {
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool CloseNative() {
        return file == INVALID_HANDLE_VALUE ||
            CloseHandle(std::exchange(file, INVALID_HANDLE_VALUE)) != FALSE;
    }
#else
    int file = -1;
    struct stat identity{};
    bool CloseNative() { return file < 0 || ::close(std::exchange(file, -1)) == 0; }
#endif
    ~Impl() { (void)CloseNative(); }
};

JournalFileAnchor::JournalFileAnchor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
JournalFileAnchor::JournalFileAnchor(JournalFileAnchor&&) noexcept = default;
JournalFileAnchor& JournalFileAnchor::operator=(JournalFileAnchor&&) noexcept = default;
JournalFileAnchor::~JournalFileAnchor() = default;

std::expected<void, std::string> JournalFileAnchor::Close() {
    if (impl_ && !impl_->CloseNative()) {
#ifdef _WIN32
        return std::unexpected("recovery.anchor_close_failed:" + std::to_string(GetLastError()));
#else
        return std::unexpected("recovery.anchor_close_failed:" + std::to_string(errno));
#endif
    }
    return {};
}

std::expected<JournalFileAnchor::Capture, std::string> JournalFileAnchor::ReadExisting(
    const std::filesystem::path& path, std::optional<std::size_t> max_bytes, bool follow_path) {
    // Allocate ownership before opening a native handle. Later exceptions use
    // the same checked failure cleanup, including allocation during byte reads.
    auto anchor = std::shared_ptr<JournalFileAnchor>(new JournalFileAnchor(std::make_unique<Impl>()));
    auto& impl = anchor->impl_;
    const auto fail = [&](std::string error) -> std::expected<Capture, std::string> {
        if (!impl->CloseNative()) {
#ifdef _WIN32
            error += ";recovery.close_failed:" + std::to_string(GetLastError());
#else
            error += ";recovery.close_failed:" + std::to_string(errno);
#endif
        }
        return std::unexpected(std::move(error));
    };
    try {
#ifdef _WIN32
    impl->file = CreateFileW(platform::FileIoPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, follow_path ? 0 : FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl->file == INVALID_HANDLE_VALUE)
        return fail("recovery.open_failed:" + std::to_string(GetLastError()));
    if (GetFileType(impl->file) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(impl->file, &impl->identity) ||
        (impl->identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (!follow_path && (impl->identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))
        return fail("recovery.nonregular");
#else
    impl->file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (follow_path ? 0 : O_NOFOLLOW));
    if (impl->file < 0)
        return fail("recovery.open_failed:" + std::to_string(errno));
    if (::fstat(impl->file, &impl->identity) != 0 || !S_ISREG(impl->identity.st_mode))
        return fail("recovery.nonregular");
#endif
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        std::size_t request = buffer.size();
        if (max_bytes) {
            if (bytes.size() > *max_bytes) return fail("recovery.byte_limit");
            request = (std::min)(request, *max_bytes - bytes.size());
            if (request == 0) request = 1; // Probe EOF without cap + 1 arithmetic.
        }
        std::size_t count = 0;
#ifdef _WIN32
        DWORD read = 0;
        if (!ReadFile(impl->file, buffer.data(), static_cast<DWORD>(request), &read, nullptr))
            return fail("recovery.read_failed:" + std::to_string(GetLastError()));
        count = read;
#else
        ssize_t read;
        do { read = ::read(impl->file, buffer.data(), request); } while (read < 0 && errno == EINTR);
        if (read < 0) return fail("recovery.read_failed:" + std::to_string(errno));
        count = static_cast<std::size_t>(read);
#endif
        if (!count) break;
        if ((max_bytes && count > *max_bytes - bytes.size()) ||
            count > bytes.max_size() - bytes.size()) return fail("recovery.byte_limit");
        bytes.append(buffer.data(), count);
    }
    return Capture{std::move(bytes), std::move(anchor)};
    } catch (const std::exception& error) {
        return fail(std::string("recovery.read_exception:") + error.what());
    } catch (...) {
        return fail("recovery.read_exception");
    }
}

std::expected<JournalWriter, std::string> JournalWriter::OpenExistingVerified(
    const std::filesystem::path& path, std::string_view prefix, const JournalFileAnchor& anchor) {
    if (!anchor.impl_) return std::unexpected("recovery.anchor_missing");
    int descriptor = -1;
#ifdef _WIN32
    if (anchor.impl_->file == INVALID_HANDLE_VALUE) return std::unexpected("recovery.anchor_closed");
    const errno_t opened = _wsopen_s(&descriptor, platform::FileIoPath(path).c_str(),
        _O_RDWR | _O_APPEND | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, 0);
    if (opened != 0) return std::unexpected("recovery.append_open_failed:" + std::to_string(opened));
    const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool regular = file != INVALID_HANDLE_VALUE && GetFileType(file) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(file, &identity) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    const auto& old = anchor.impl_->identity;
    const bool same = regular && identity.dwVolumeSerialNumber == old.dwVolumeSerialNumber &&
        identity.nFileIndexHigh == old.nFileIndexHigh && identity.nFileIndexLow == old.nFileIndexLow;
    const auto close_descriptor = [&] { return _close(descriptor); };
#else
    if (anchor.impl_->file < 0) return std::unexpected("recovery.anchor_closed");
    descriptor = ::open(path.c_str(), O_RDWR | O_APPEND | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) return std::unexpected("recovery.append_open_failed:" + std::to_string(errno));
    struct stat identity{};
    const bool regular = ::fstat(descriptor, &identity) == 0 && S_ISREG(identity.st_mode);
    const bool same = regular && identity.st_dev == anchor.impl_->identity.st_dev &&
        identity.st_ino == anchor.impl_->identity.st_ino;
    const auto close_descriptor = [&] { return ::close(descriptor); };
#endif
    if (!same) {
        std::string error = "recovery.object_changed";
        if (close_descriptor() != 0) error += ";recovery.close_failed:" + std::to_string(errno);
        return std::unexpected(std::move(error));
    }
#ifdef _WIN32
    std::FILE* stream = _fdopen(descriptor, "a+b");
#else
    std::FILE* stream = ::fdopen(descriptor, "a+b");
#endif
    if (!stream) {
        const int error = errno;
        std::string message = "recovery.fdopen_failed:" + std::to_string(error);
        if (close_descriptor() != 0) message += ";recovery.close_failed:" + std::to_string(errno);
        return std::unexpected(std::move(message));
    }
    const auto fail = [&](std::string error) -> std::expected<JournalWriter, std::string> {
        if (std::fclose(stream) != 0) error += ";recovery.close_failed:" + std::to_string(errno);
        return std::unexpected(std::move(error));
    };
    if (std::fseek(stream, 0, SEEK_SET) != 0) return fail("recovery.seek_failed");
    std::array<char, 4096> buffer{};
    std::size_t consumed = 0;
    while (consumed < prefix.size()) {
        const auto request = (std::min)(buffer.size(), prefix.size() - consumed);
        const auto count = std::fread(buffer.data(), 1, request, stream);
        if (count != request || std::string_view(buffer.data(), count) != prefix.substr(consumed, count))
            return fail(std::ferror(stream) ? "recovery.prefix_read_failed" : "recovery.prefix_changed");
        consumed += count;
    }
    if (std::fread(buffer.data(), 1, 1, stream) != 0) return fail("recovery.prefix_extended");
    if (std::ferror(stream)) return fail("recovery.prefix_read_failed");
    std::clearerr(stream);
    if (std::fseek(stream, 0, SEEK_END) != 0) return fail("recovery.seek_failed");
    JournalWriter writer;
    writer.file_ = stream;
    writer.path_ = path;
    return writer;
}

std::string ComputeEventHash(std::string_view prev_hash,
                             std::string_view canonical_event_without_event_hash) {
    std::string material;
    material.reserve(prev_hash.size() + canonical_event_without_event_hash.size());
    material.append(prev_hash);
    material.append(canonical_event_without_event_hash);
    return hooks::Sha256Hex(material);
}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept
    : path_(std::move(other.path_)),
      file_(std::exchange(other.file_, nullptr)),
      line_count_(std::exchange(other.line_count_, 0)),
      broken_(std::exchange(other.broken_, false)) {}

JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept {
    if (this != &other) {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
        path_ = std::move(other.path_);
        file_ = std::exchange(other.file_, nullptr);
        line_count_ = std::exchange(other.line_count_, 0);
        broken_ = std::exchange(other.broken_, false);
    }
    return *this;
}

JournalWriter::~JournalWriter() { (void)Close(); }

std::expected<JournalWriter, std::string> JournalWriter::Open(const std::filesystem::path& path,
                                                              OpenMode mode) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    // _SH_DENYNO:写入期间 verify/inspect 仍可只读同打开(fopen_s 默认
    // 不共享,读端 ifstream 会吃 EACCES)。
    const wchar_t* flags = mode == OpenMode::CreateNew ? L"wbx" : L"ab";
    file = _wfsopen(path.c_str(), flags, _SH_DENYNO);
    if (file == nullptr) {
        return std::unexpected("journal 打不开(create-new 撞名或路径无效): " +
                               platform::PathToUtf8(path));
    }
#else
    const char* flags = mode == OpenMode::CreateNew ? "wbx" : "ab";
    file = std::fopen(path.c_str(), flags);
    if (file == nullptr) {
        return std::unexpected("journal 打不开(create-new 撞名或路径无效): " +
                               platform::PathToUtf8(path));
    }
#endif
    JournalWriter writer;
    writer.path_ = path;
    writer.file_ = file;
    // 行数只在 CreateNew 语义下精确;Append 由调用方(recovery 路径)自证
    // 既有事件数并续发 seq,本件不偷偷扫文件。
    return writer;
}

bool JournalWriter::AppendLine(std::string_view line, Durability durability) {
    if (broken_ || file_ == nullptr || line.empty()) {
        return false;
    }
    const bool wrote_body = std::fwrite(line.data(), 1, line.size(), file_) == line.size();
    const bool wrote_newline = std::fwrite("\n", 1, 1, file_) == 1;
    if (!wrote_body || !wrote_newline || !FlushFileDurable(file_, durability)) {
        broken_ = true;
        return false;
    }
    ++line_count_;
    return true;
}

bool JournalWriter::Close() {
    if (file_ != nullptr && std::fclose(std::exchange(file_, nullptr)) != 0) {
        broken_ = true;
    }
    return !broken_;
}

std::expected<std::string, std::string> JournalWriter::ComputeJournalSha256(
    const std::filesystem::path& path) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (file == nullptr) {
        return std::unexpected("journal 读不开: " + platform::PathToUtf8(path));
    }
#else
    file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::unexpected("journal 读不开: " + platform::PathToUtf8(path));
    }
#endif
    std::string data;
    char buffer[65536];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.append(buffer, read);
    }
    std::fclose(file);
    return hooks::Sha256Hex(data);
}

std::optional<std::vector<std::string>> ReadJournalLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

JournalVerifyReport VerifyJournalFile(const std::filesystem::path& path) {
    JournalVerifyReport report;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        report.error_code = "verify.open_failed";
        report.message = "journal 读不开: " + platform::PathToUtf8(path);
        return report;
    }

    std::string prev_hash(kGenesisHash);
    std::uint64_t expected_seq = 1;
    // 一条 stream 只得一个 schema major(Token 账本单 §6.1.1):v1/v2 混写
    // 的文件整本拒绝,不让读者跳过不认识的 usage 事件后报一只残账。
    int stream_schema_version = 0;
    std::string line;
    std::uint64_t line_number = 0;
    bool truncated = false;
    while (std::getline(file, line)) {
        ++line_number;
        // getline 后 eof 为真说明这行不以 '\n' 收尾:尾行被截(§16.3)。
        const bool ends_with_newline = !file.eof();
        if (!ends_with_newline) {
            truncated = true;
        }
        if (line.empty()) {
            report.error_code = "verify.empty_line";
            report.message = "第 " + std::to_string(line_number) + " 行是空行";
            return report;
        }

        nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            report.error_code = "verify.bad_json";
            report.message = "第 " + std::to_string(line_number) + " 行不是合法 JSON";
            return report;
        }
        // canonical round-trip:字节必须逐字一致(写盘行即是规范形)。
        const auto canonical = CanonicalJsonDump(parsed);
        if (!canonical.has_value() || *canonical != line) {
            report.error_code = "verify.not_canonical";
            report.message = "第 " + std::to_string(line_number) + " 行不是规范字节";
            return report;
        }
        EventEnvelope envelope;
        if (auto error = ParseAndValidateEventLine(parsed, &envelope)) {
            report.error_code = error->error_code;
            report.message = "第 " + std::to_string(line_number) + " 行: " + error->message;
            return report;
        }
        if (envelope.seq != expected_seq) {
            report.error_code = "verify.seq_gap";
            report.message = "第 " + std::to_string(line_number) + " 行 seq 期望 " +
                             std::to_string(expected_seq) + " 实得 " + std::to_string(envelope.seq);
            return report;
        }
        if (stream_schema_version == 0) {
            stream_schema_version = envelope.schema_version;
        } else if (stream_schema_version != envelope.schema_version) {
            report.error_code = "verify.schema_version_mixed";
            report.message = "第 " + std::to_string(line_number) +
                             " 行 schema_version 与首行不同:一条 stream 不混 v1/v2";
            return report;
        }
        if (envelope.prev_hash != prev_hash) {
            report.error_code = "verify.chain_broken";
            report.message = "第 " + std::to_string(line_number) + " 行 prev_hash 接不上链";
            return report;
        }
        // 重算 event_hash:去掉 event_hash 键后 canonical 再算。
        nlohmann::json without_hash = parsed;
        without_hash.erase("event_hash");
        const auto canonical_without_hash = CanonicalJsonDump(without_hash);
        if (!canonical_without_hash.has_value()) {
            report.error_code = "verify.canonical_failed";
            report.message = "第 " + std::to_string(line_number) + " 行 canonical 序列化失败";
            return report;
        }
        const std::string recomputed =
            ComputeEventHash(envelope.prev_hash, *canonical_without_hash);
        if (recomputed != envelope.event_hash) {
            report.error_code = "verify.hash_mismatch";
            report.message = "第 " + std::to_string(line_number) + " 行 event_hash 对不上";
            return report;
        }

        if (report.events == 0) {
            report.first_event_hash = envelope.event_hash;
        }
        report.last_event_hash = envelope.event_hash;
        prev_hash = envelope.event_hash;
        ++expected_seq;
        ++report.events;
    }

    if (truncated) {
        // 尾行截断:前面完整事件照常可 replay,但整本判 incomplete,明报
        // 而不伪造终态(§16.3)。
        report.truncated_tail = true;
        report.error_code = "verify.truncated_tail";
        report.message = "尾行缺换行符,判截断";
        return report;
    }
    if (report.events == 0) {
        report.error_code = "verify.empty_journal";
        report.message = "journal 没有任何事件";
        return report;
    }
    report.ok = true;
    return report;
}

}  // namespace lubancode::trajectory
