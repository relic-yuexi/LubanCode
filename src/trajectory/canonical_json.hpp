// Canonical JSON(§8.1):hash 输入的稳定序列化。
//
// 规矩四条,单测钉字节:
//   1. object key 递归按 UTF-8 字节序排序(std::string::compare 的 memcmp
//      语义,显式 sort,不依赖 nlohmann 对象的内部序);
//   2. 无空白;
//   3. 字符串先过 UTF-8 合法性校验,非法字节拒绝;
//   4. 数字拒绝 NaN/Inf;浮点文本走 nlohmann(vendored 3.11.3)的确定性
//      最短表示,整数走十进制——同一语义对象跨 Windows/Linux 字节一致。
//
// binary 类型(msgpack/cbor 才会产生)拒绝:轨迹正文不认二进制。
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {

// 规范序列化。失败返回错误码与人话;错误码稳定:
//   canonical_json.nan_or_inf          浮点非有限
//   canonical_json.invalid_utf8        字符串含非法 UTF-8
//   canonical_json.binary_unsupported  含 binary 值
//   canonical_json.discarded           传入未初始化 json
std::expected<std::string, std::string> CanonicalJsonDump(const nlohmann::json& value);

// UTF-8 合法性校验(规范序列化用;拒绝过长的 ASCII 编码与孤立项)。
bool IsValidUtf8(std::string_view text);

}  // namespace lubancode::trajectory
