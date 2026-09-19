// SHA-256 内核实现(合同见 sha256.hpp)。算法体自 channel/digest.cpp 原样
// 搬来(其尾段/填充写法比 hooks 版的整串拷贝更省),与 hooks/hash.cpp 的
// 输出逐向量相等(见 tests/unit/platform/test_sha256.cpp 的对账)。
// 更新器 C++ 化批一第③单:内核改造成增量形态——Sha256Stream 持状态分块
// 喂,一次性口退为 wrapper。压缩轮、K 常量、填充规则一字未动,动的只是
// 数据怎么进压缩轮。
#include "platform/sha256.hpp"

#include <cassert>
#include <cstring>

namespace lubancode::platform {

namespace {

// FIPS 180-4 的 K 常量(前 64 个素数立方根小数部分)。
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline std::uint32_t RotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

void CompressBlock(std::uint32_t state[8], const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// 大端摘要序列化 + 小写十六进制(FinalHex 与一次性口共用)。
std::string BytesToHex(const std::array<std::byte, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::byte byte : digest) {
        const unsigned value = static_cast<unsigned>(byte);
        out.push_back(kHex[value >> 4]);
        out.push_back(kHex[value & 0x0f]);
    }
    return out;
}

}  // namespace

Sha256Stream::Sha256Stream()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
      buffer_{},
      buffer_size_(0),
      total_bytes_(0),
      finalized_(false) {}

void Sha256Stream::Update(std::span<const std::byte> chunk) {
    assert(!finalized_ && "Sha256Stream: Final 后不可再 Update(合同见头注)");

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk.data());
    std::size_t size = chunk.size();
    total_bytes_ += size;

    // 残余优先:先把 buffer_ 凑满一格,凑满即压、清零。空块(size == 0)
    // 直接落到下面的整块/尾段两段,均为 no-op。
    if (buffer_size_ > 0 && size > 0) {
        const std::size_t fill = (64 - buffer_size_ < size) ? 64 - buffer_size_ : size;
        std::memcpy(buffer_ + buffer_size_, bytes, fill);
        buffer_size_ += fill;
        bytes += fill;
        size -= fill;
        if (buffer_size_ == 64) {
            CompressBlock(state_, buffer_);
            buffer_size_ = 0;
        }
    }

    // 中段整块直压(零拷贝),尾段进 buffer_ 等下一次 Update 或 Final。
    const std::size_t full_blocks = size / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        CompressBlock(state_, bytes + i * 64);
    }
    const std::size_t remainder = size - full_blocks * 64;
    if (remainder > 0) {
        std::memcpy(buffer_, bytes + full_blocks * 64, remainder);
        buffer_size_ = remainder;
    }
}

std::array<std::byte, 32> Sha256Stream::FinalDigest() {
    assert(!finalized_ && "Sha256Stream: 只许 Final 一次(合同见头注)");

    // 尾段 + 0x80 填充 + 长度(64 位大端),不足 64 字节补零,可能多出一个
    // 块——与拆前一次性实现的填充同构,buffer_ 备足了 128 字节。
    buffer_[buffer_size_] = 0x80;
    const std::size_t tail_blocks = (buffer_size_ < 56) ? 1 : 2;
    std::memset(buffer_ + buffer_size_ + 1, 0, tail_blocks * 64 - buffer_size_ - 1);
    const std::uint64_t bit_length = total_bytes_ * 8;
    std::uint8_t* length_slot = buffer_ + tail_blocks * 64 - 8;
    for (int i = 0; i < 8; ++i) {
        length_slot[i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
    }
    for (std::size_t i = 0; i < tail_blocks; ++i) {
        CompressBlock(state_, buffer_ + i * 64);
    }
    finalized_ = true;

    std::array<std::byte, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::byte>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::byte>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::byte>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::byte>(state_[i]);
    }
    return digest;
}

std::string Sha256Stream::FinalHex() {
    return BytesToHex(FinalDigest());
}

// ---- 一次性口:流式内核的薄 wrapper,签名与语义照旧 ------------------------

std::array<std::byte, 32> Sha256Digest(std::span<const std::byte> data) {
    Sha256Stream stream;
    stream.Update(data);
    return stream.FinalDigest();
}

std::string Sha256Hex(std::span<const std::byte> data) {
    Sha256Stream stream;
    stream.Update(data);
    return stream.FinalHex();
}

}  // namespace lubancode::platform
