#include "trajectory/blob_store.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
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

// 目录项落盘(PowerLoss 档 rename 后尽力而为;不支持目录 flush 的文件系统
// 只当没发生,不误伤提交)。
void FlushDirectoryBestEffort(const std::filesystem::path& dir) {
#ifdef _WIN32
    const HANDLE handle =
        CreateFileW(dir.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    FlushFileBuffers(handle);
    CloseHandle(handle);
#else
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
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
    const std::string hash = hooks::Sha256Hex(data);
    const std::filesystem::path target = PathFor(hash);
    if (std::filesystem::exists(target)) {
        // 内容寻址幂等:同 hash 直接复用既有 blob。
        return BlobRef{hash, static_cast<std::uint64_t>(data.size()), std::move(media_type),
                       "utf-8", "none"};
    }

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected("blob 目录建不起: " + platform::PathToUtf8(target.parent_path()) +
                               ": " + ec.message());
    }

    // 临时文件同目录,保证 rename 不跨文件系统。
    std::filesystem::path tmp = target;
    tmp += ".tmp-" + std::to_string(static_cast<unsigned long long>(NextTmpCounter()));
    {
        std::FILE* file = nullptr;
#ifdef _WIN32
        // _SH_DENYNO:同 fopen_s 默认不共享的坑,写入期间允许只读探测。
        file = _wfsopen(tmp.c_str(), L"wb", _SH_DENYNO);
        if (file == nullptr) {
            return std::unexpected("blob 临时文件打不开: " + platform::PathToUtf8(tmp));
        }
#else
        file = std::fopen(tmp.c_str(), "wb");
        if (file == nullptr) {
            return std::unexpected("blob 临时文件打不开: " + platform::PathToUtf8(tmp));
        }
#endif
        bool ok = true;
        if (!data.empty()) {
            ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
        }
        if (ok) {
            ok = FlushFileDurable(file, durability);
        }
        std::fclose(file);
        if (!ok) {
            std::filesystem::remove(tmp, ec);
            return std::unexpected("blob 临时文件写不稳: " + platform::PathToUtf8(tmp));
        }
    }

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected("blob rename 失败: " + platform::PathToUtf8(target) + ": " +
                               ec.message());
    }
    if (durability == Durability::PowerLoss) {
        FlushDirectoryBestEffort(target.parent_path());
    }
    return BlobRef{hash, static_cast<std::uint64_t>(data.size()), std::move(media_type), "utf-8",
                   "none"};
}

std::optional<std::string> BlobStore::ReadVerified(const BlobRef& ref) const {
    const std::filesystem::path path = PathFor(ref.sha256);
    std::FILE* file = nullptr;
#ifdef _WIN32
    file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (file == nullptr) {
        return std::nullopt;
    }
#else
    file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
#endif
    std::string data;
    char buffer[65536];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.append(buffer, read);
    }
    std::fclose(file);
    if (hooks::Sha256Hex(data) != ref.sha256 || data.size() != ref.size) {
        return std::nullopt;
    }
    return data;
}

std::filesystem::path BlobStore::PathFor(std::string_view sha256) const {
    // 前 2 字符一层分桶(§3.1 的 ab/<full-sha256>)。
    std::string prefix(sha256.substr(0, 2));
    return root_ / "sha256" / platform::Utf8ToPath(prefix) / platform::Utf8ToPath(std::string(sha256));
}

}  // namespace lubancode::trajectory
