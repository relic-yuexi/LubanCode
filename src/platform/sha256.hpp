// SHA-256(FIPS 180-4)的仓内唯一算法内核(src 收口审计 P2:hooks/hash.cpp
// 与 channel/digest.cpp 各养一份 64 常量/填充/压缩轮/十六进制,算法参与
// 内容寻址、配对、幂等与信任边界,两份各修各的迟早漂移)。搬来中立
// platform 层,Hooks 与 Channel 只做领域命名与输入组装。
//
// 合同冻结(迁移不得让存量 key 失配):
//   - 摘要字节序:FIPS 大端(状态字按 32 位大端序列化);
//   - 十六进制一律小写、64 字符;
//   - 消息按字节精确处理,含 NUL。
//
// 流式合同(更新器 C++ 化批一第③单):Sha256Stream 对同一份数据按任意
// 方式分块——1 字节、任意奇数块、64KB、整块,乃至零次 Update 的空输入
// ——累加出的摘要,必须与一次性 Sha256Digest/Sha256Hex 完全相等。分块
// 只进尾段缓冲,不进算法。后续更新器拿它哈 4GiB 硬顶的整包:一块一块
// 喂,绝不整读进内存。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lubancode::platform {

// 流式摘要:增量状态 + 尾段缓冲,Update 次数与块大小均不限(空块也是
// 合法输入,不改摘要)。FinalHex/FinalDigest 终结后不可再 Update,也
// 不可二次 Final——再调是编程错误,debug 断言拦截,release 下的行为
// 不做约定。
class Sha256Stream {
 public:
  Sha256Stream();

  void Update(std::span<const std::byte> chunk);
  void Update(std::string_view chunk) {  // 便利重载:业务层薄口
    Update(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(chunk.data()), chunk.size()));
  }

  // 终结并出小写 64 字符十六进制(与 Sha256Hex 同格式)。
  std::string FinalHex();
  // 终结并出 32 字节原始摘要——一次性口 Sha256Digest 的芯。
  std::array<std::byte, 32> FinalDigest();

 private:
  std::uint32_t state_[8];     // FIPS 180-4 的八个工作变量
  std::uint8_t buffer_[128];   // 尾段缓冲:Update 只用前 64;Final 填充可能占两块
  std::size_t buffer_size_;    // buffer_ 已存字节数,恒 < 64(满即压清零)
  std::uint64_t total_bytes_;  // 累计消息长度(bit 长度在 Final 时乘 8 展开)
  bool finalized_;
};

// 摘要原语:span<const std::byte> 是内核口,业务层拿 string_view 薄口喂。
// 一次性口是流式内核的薄 wrapper(一次 Update + Final),对外签名与语义
// 不变——拆前的存量向量(tests/unit/platform/test_sha256.cpp)继续钉着。
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
