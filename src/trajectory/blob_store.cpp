#include "trajectory/blob_store.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
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
    if (!IsHex64(hash)) {
        // 防御性:hash 出自自家的 Sha256Hex,这条真触发说明哈希层坏了。
        return std::unexpected("blob hash 形状不合法");
    }
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

    // 临时文件同目录,保证 rename 不跨文件系统。create-new 语义:占位成功
    // 才写(碰撞给唯一计数器名,理论不可达);POSIX 侧直接以 0600 落地,
    // Windows 侧由 session 根的 PROTECTED user-only DACL 继承(§12.1)。
    std::filesystem::path tmp = target;
    tmp += ".tmp-" + std::to_string(static_cast<unsigned long long>(NextTmpCounter()));
    {
        std::FILE* file = nullptr;
#ifdef _WIN32
        // _SH_DENYNO:同 fopen_s 默认不共享的坑,写入期间允许只读探测。
        // "wbx"(create-new):已被预置的临时名直接失败,不覆盖(§12.1)。
        file = _wfsopen(tmp.c_str(), L"wbx", _SH_DENYNO);
        if (file == nullptr) {
            return std::unexpected("blob 临时文件打不开: " + platform::PathToUtf8(tmp));
        }
#else
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return std::unexpected("blob 临时文件打不开: " + platform::PathToUtf8(tmp));
        }
        file = ::fdopen(fd, "wb");
        if (file == nullptr) {
            ::close(fd);
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

    // rename 前再核目标仍在仓根内(§12.1"rename 前再核"):目标虽由合法
    // hash 拼出,分桶目录若被换成重解析点,这里当场拦下。
    if (!IsSafeContainedPath(target, root_)) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected("blob 目标路径越界: " + platform::PathToUtf8(target));
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
