// 渠道侧摘要件(多渠道消息接入单阶段 2)。
//
// 用途两处,都不是密码学防线本体:
//   - pairing code 落盘前压 hash(configuration.md §6"存 hash 不存明文";
//     防的是文件里直接躺明文 code,不防定向暴力——code 本身短命数分钟);
//   - 第三级去重指纹的 parts digest(message-contracts.md §3——指纹只作
//     短窗去重,不冒充永久 id)。
//
// SHA-256 算法内核统一住在 platform/sha256.cpp(src 收口审计 P2:此前
// hooks 与 channel 各养一份);这里只做领域命名与 byte-span 入口组装。
// 依赖铁律沿 channel 库:只用标准库与 platform,不 include
// app/cli/runtime/agent/package。
#include "channel/digest.hpp"

#include "platform/sha256.hpp"  // SHA-256 算法内核(字节序/十六进制小写合同冻结)

namespace lubancode::channel {

std::string Sha256Hex(std::string_view data) {
    return platform::Sha256Hex(data);
}

std::string Sha256Hex(const std::byte* data, std::size_t size) {
    return platform::Sha256Hex(std::span<const std::byte>(data, size));
}

}  // namespace lubancode::channel
