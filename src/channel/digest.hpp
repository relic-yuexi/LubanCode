// 渠道侧摘要件(多渠道消息接入单阶段 2)。
//
// 用途两处,都不是密码学防线本体:
//   - pairing code 落盘前压 hash(configuration.md §6"存 hash 不存明文";
//     防的是文件里直接躺明文 code,不防定向暴力——code 本身短命数分钟);
//   - 第三级去重指纹的 parts digest(message-contracts.md §3——指纹只作
//     短窗去重,不冒充永久 id)。
//
// 仓里没有现成 SHA-256(platform 只给了进程/路径件),这里落一份自包含
// 实现(FIPS 180-4,公有域算法的标准写法)。依赖铁律沿 channel 库:只用
// 标准库,不 include app/cli/runtime/agent/package。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::channel {

// SHA-256(bytes) 的小写十六进制(64 字符)。
std::string Sha256Hex(std::string_view data);
std::string Sha256Hex(const std::byte* data, std::size_t size);

}  // namespace lubancode::channel
