// 内容寻址 Blob 仓(§8.2):超限正文与大二进制只落 blob,事件带 BlobRef。
//
// 布局照 §3.1:<session>/artifacts/sha256/<hash 前 2 字符>/<全 hash>。
// 落盘次序:同仓临时文件 → 算 sha256 → flush(PowerLoss 档连 fsync)→
// 原子 rename 到内容地址 →(PowerLoss 档尽力 flush 目录)。同 hash 已在
// 仓内直接复用——内容寻址天然幂等。事件不可指向尚未落稳的 blob。
//
// 内联上限(默认 32 KiB,可配)由 recorder 在提交时执行;仓本身只管存取。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "trajectory/event.hpp"

namespace lubancode::trajectory {

// §8.2 BlobRef:事件里引用 blob 的唯一形状。
struct BlobRef {
    std::string sha256;                     // 64 位十六进制小写
    std::uint64_t size = 0;                 // 内容字节数
    std::string media_type = "text/plain";  // "application/json"、"image/png" 等
    std::string encoding = "utf-8";
    std::string compression = "none";

    nlohmann::json ToJson() const;
    static std::optional<BlobRef> FromJson(const nlohmann::json& json);
    // 是否 BlobRef 形状(五键齐全、hash 合法)。
    static bool MatchesShape(const nlohmann::json& json);
};

struct BlobStoreOptions {
    // 超过它的字符串正文不内联进事件,offload 成 blob(§8.2,可配)。
    std::uint64_t inline_limit = 32 * 1024;
};

class BlobStore {
public:
    // 未开的仓:root 为空,Store 一律失败(recorder Start 后不会处于此态)。
    BlobStore() = default;
    // root 即 recorder 钉死的 session_artifact_root(<session>/artifacts)。
    BlobStore(std::filesystem::path root, BlobStoreOptions options = BlobStoreOptions{});

    const std::filesystem::path& root() const { return root_; }
    std::uint64_t inline_limit() const { return options_.inline_limit; }

    // 存一段正文,返回 BlobRef。失败给人话(目录建不起、写不稳、rename
    // 失败)。PowerLoss 档对 blob 文件 fsync 并尽力 flush 目录。
    std::expected<BlobRef, std::string> Store(std::string_view data, std::string media_type,
                                              Durability durability);

    // 读回并核 sha256:hash 不合给 nullopt(被改/被截一律拒供)。
    std::optional<std::string> ReadVerified(const BlobRef& ref) const;

    // 内容地址:<root>/sha256/<hash[0:2]>/<hash>。
    std::filesystem::path PathFor(std::string_view sha256) const;

private:
    std::filesystem::path root_;
    BlobStoreOptions options_;
};

}  // namespace lubancode::trajectory
