#include "channel/qq/qq_spool.hpp"

#include <algorithm>
#include <cstdio>

#include "platform/atomic_write.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lubancode::channel::qq {

namespace {

// 按平台拿宽口/字节口(同 platform/atomic_write 的口径:Windows 窄口走 ACP,
// 非 ASCII 路径开错)。
std::FILE* OpenReadBinary(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

}  // namespace

bool IsValidSpoolDeliveryId(const std::string& delivery_id) {
    if (delivery_id.empty() || delivery_id.size() > 128) {
        return false;
    }
    for (const char c : delivery_id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return delivery_id != "." && delivery_id != "..";
}

std::expected<QqSpoolStore, std::string> QqSpoolStore::Open(
    const std::filesystem::path& pending_dir) {
    std::error_code ec;
    std::filesystem::create_directories(pending_dir, ec);
    if (ec) {
        return std::unexpected("spool mkdir failed: " + ec.message());
    }
    return QqSpoolStore(pending_dir);
}

std::optional<std::string> QqSpoolStore::AppendPending(const std::string& delivery_id,
                                                       const nlohmann::json& event_json) {
    if (append_fault_for_test_.load()) {
        return "spool write failed: injected disk full (test)";
    }
    if (!IsValidSpoolDeliveryId(delivery_id)) {
        return "spool delivery id invalid";
    }
    const auto target = pending_dir_ / (delivery_id + ".json");
    const std::string serialized = event_json.dump();
    const auto written = platform::AtomicWriteFile(target, serialized,
                                                   platform::WriteDurability::
                                                       ProcessCrashDurability);
    if (!written.has_value()) {
        return "spool write failed: " + written.error().code + " " + written.error().message;
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, nlohmann::json>> QqSpoolStore::ListPending() const {
    std::vector<std::pair<std::string, nlohmann::json>> out;
    std::error_code ec;
    if (!std::filesystem::exists(pending_dir_, ec)) {
        return out;
    }
    std::vector<std::string> ids;
    for (const auto& entry : std::filesystem::directory_iterator(pending_dir_, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (filename.size() <= 5 || filename.substr(filename.size() - 5) != ".json") {
            continue;
        }
        ids.push_back(filename.substr(0, filename.size() - 5));
    }
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids) {
        std::error_code read_ec;
        const auto size = std::filesystem::file_size(pending_dir_ / (id + ".json"), read_ec);
        if (!read_ec && size > 16 * 1024 * 1024) {
            continue;  // 超帽脏文件:跳过,不炸整个重投
        }
        std::FILE* file = OpenReadBinary(pending_dir_ / (id + ".json"));
        if (file == nullptr) {
            continue;
        }
        std::string content(read_ec ? 0 : static_cast<std::size_t>(size), '\0');
        const std::size_t got =
            content.empty() ? 0 : std::fread(content.data(), 1, content.size(), file);
        std::fclose(file);
        content.resize(got);
        const auto parsed = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            continue;  // 坏账:跳过(诊断口径由调用方对账 pending_count)
        }
        out.emplace_back(id, std::move(parsed));
    }
    return out;
}

std::optional<std::string> QqSpoolStore::RemoveAcked(const std::string& delivery_id) {
    if (!IsValidSpoolDeliveryId(delivery_id)) {
        return std::nullopt;  // 未知形状:幂等无操作
    }
    std::error_code ec;
    const auto target = pending_dir_ / (delivery_id + ".json");
    if (!std::filesystem::exists(target, ec)) {
        return std::nullopt;
    }
    if (!std::filesystem::remove(target, ec) || ec) {
        return "spool remove failed: " + ec.message();
    }
    return std::nullopt;
}

std::size_t QqSpoolStore::pending_count() const {
    return ListPending().size();
}

}  // namespace lubancode::channel::qq
