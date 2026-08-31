// 确定性 trace/span id 的实现。合同见 identity.hpp 文件头。
//
// HMAC-SHA256 用 hooks::Sha256Hex 的十六进制面拼标准构型:内层摘要先
// hex->bytes 还原再进外层,结果是真 HMAC-SHA256 的前缀,不是自造变体。
#include "telemetry/identity.hpp"

#include <array>
#include <cstddef>
#include <string_view>

#include "hooks/hash.hpp"

namespace lubancode::telemetry {
namespace {

// SHA-256 分组长(HMAC 的 key 补齐长度)。
constexpr std::size_t kSha256Block = 64;

// 十六进制字符串(偶数长)还原成原始字节。
std::string HexToBytes(std::string_view hex) {
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = nibble(hex[i]);
        const int low = nibble(hex[i + 1]);
        if (high < 0 || low < 0) {
            break;  // 坏输入截断;调用方只喂自己产的 hex
        }
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    return bytes;
}

// 标准 HMAC-SHA256,返回 64 位十六进制小写。
std::string HmacSha256Hex(std::string_view key, std::string_view message) {
    // key 补齐到分组长:超长先 hash(其字节串长恰为 32)。
    std::string normalized(key);
    if (normalized.size() > kSha256Block) {
        normalized = HexToBytes(lubancode::hooks::Sha256Hex(normalized));
    }
    normalized.resize(kSha256Block, '\0');

    std::string ipad(kSha256Block, '\0');
    std::string opad(kSha256Block, '\0');
    for (std::size_t i = 0; i < kSha256Block; ++i) {
        const auto byte = static_cast<unsigned char>(normalized[i]);
        ipad[i] = static_cast<char>(byte ^ 0x36);
        opad[i] = static_cast<char>(byte ^ 0x5c);
    }
    const std::string inner_hex = lubancode::hooks::Sha256Hex(ipad + std::string(message));
    return lubancode::hooks::Sha256Hex(opad + HexToBytes(inner_hex));
}

bool IsLowerHex(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string DeriveTraceId(std::string_view projection_key, std::string_view session_id,
                          std::string_view run_id) {
    const std::string message =
        "trace|" + std::string(session_id) + "|" + std::string(run_id);
    return HmacSha256Hex(projection_key, message).substr(0, 32);
}

std::string DeriveSpanId(std::string_view projection_key, std::string_view start_event_id,
                         std::string_view span_role) {
    const std::string message =
        "span|" + std::string(start_event_id) + "|" + std::string(span_role);
    return HmacSha256Hex(projection_key, message).substr(0, 16);
}

bool IsValidTraceId(std::string_view trace_id) {
    return trace_id.size() == 32 && IsLowerHex(trace_id) &&
           trace_id.find_first_not_of('0') != std::string_view::npos;
}

bool IsValidSpanId(std::string_view span_id) {
    return span_id.size() == 16 && IsLowerHex(span_id) &&
           span_id.find_first_not_of('0') != std::string_view::npos;
}

std::string FormatTraceParent(std::string_view trace_id, std::string_view span_id,
                              bool sampled) {
    return std::string("00-") + std::string(trace_id) + "-" + std::string(span_id) + "-" +
           (sampled ? "01" : "00");
}

}  // namespace lubancode::telemetry
