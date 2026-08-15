// definition hash 用的 SHA-256(自含实现,不引第三方)。hook 信任按定义
// 哈希记账——命令一改,哈希变,project 信任失效。这层是供应链信任的锚,
// 不用玩具散列(FNV 之类随手工造碰撞),用正经 SHA-256。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lubancode::hooks {

// 返回 64 个十六进制字符的小写串。
std::string Sha256Hex(std::string_view data);

inline std::string DefinitionHashShort(const std::string& hash) {
    return hash.size() > 12 ? hash.substr(0, 12) : hash;
}

}  // namespace lubancode::hooks
