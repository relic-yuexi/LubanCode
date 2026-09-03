// SHA-256(FIPS 180-4)的仓内唯一算法内核(src 收口审计 P2:hooks/hash.cpp
// 与 channel/digest.cpp 各养一份 64 常量/填充/压缩轮/十六进制,算法参与
// 内容寻址、配对、幂等与信任边界,两份各修各的迟早漂移)。搬来中立
// platform 层,Hooks 与 Channel 只做领域命名与输入组装。
//
// 合同冻结(迁移不得让存量 key 失配):
//   - 摘要字节序:FIPS 大端(状态字按 32 位大端序列化);
//   - 十六进制一律小写、64 字符;
//   - 消息按字节精确处理,含 NUL。
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace lubancode::platform {

// 摘要原语:span<const std::byte> 是内核口,业务层拿 string_view 薄口喂。
std::array<std::byte, 32> Sha256Digest(std::span<const std::byte> data);
std::string Sha256Hex(std::span<const std::byte> data);  // 小写 64 字符

inline std::array<std::byte, 32> Sha256Digest(std::string_view data) {
    return Sha256Digest(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                                   data.size()));
}

inline std::string Sha256Hex(std::string_view data) {
    return Sha256Hex(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                                data.size()));
}

}  // namespace lubancode::platform
