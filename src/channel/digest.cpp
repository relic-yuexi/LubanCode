#include "channel/digest.hpp"

namespace lubancode::channel {

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

std::string ToHexLower(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

std::array<std::uint8_t, 32> Sha256Raw(const std::uint8_t* data, std::size_t size) {
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const std::size_t full_blocks = size / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        CompressBlock(state, data + i * 64);
    }

    // 尾段 + 0x80 填充 + 长度(64 位大端),不足 64 字节补零,可能多出一个块。
    std::uint8_t tail[128] = {};
    const std::size_t remainder = size - full_blocks * 64;
    for (std::size_t i = 0; i < remainder; ++i) {
        tail[i] = data[full_blocks * 64 + i];
    }
    tail[remainder] = 0x80;
    const std::size_t tail_blocks = (remainder < 56) ? 1 : 2;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8;
    std::uint8_t* length_slot = tail + tail_blocks * 64 - 8;
    for (int i = 0; i < 8; ++i) {
        length_slot[i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
    }
    for (std::size_t i = 0; i < tail_blocks; ++i) {
        CompressBlock(state, tail + i * 64);
    }

    std::array<std::uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

}  // namespace

std::string Sha256Hex(std::string_view data) {
    return Sha256Hex(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

std::string Sha256Hex(const std::byte* data, std::size_t size) {
    return ToHexLower(Sha256Raw(reinterpret_cast<const std::uint8_t*>(data), size));
}

}  // namespace lubancode::channel
