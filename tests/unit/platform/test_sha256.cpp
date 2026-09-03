// platform::Sha256Digest/Sha256Hex 的向量回归(src 收口审计 P2:SHA-256
// 算法此前 hooks/hash.cpp 与 channel/digest.cpp 各养一份,内核搬
// platform 后两处只留薄口)。向量按审计单 §三 P2:
//   - 公开向量(NIST/FIPS):空串、"abc"、56 字节串、一百万 'a';
//   - 55/56/63/64 字节填充边界(各造一串,与两份旧实现逐项对账);
//   - 大载荷(跨多块);
//   - 含 NUL 字节的载荷。
// 对账断言:platform 内核 == hooks 旧口 == channel 旧口——迁移前后摘要
// 字节序与十六进制大小写不得漂移,存量 key 不失配。
#include <doctest/doctest.h>

#include <string>

#include "channel/digest.hpp"
#include "hooks/hash.hpp"
#include "platform/sha256.hpp"

namespace {

// 三口对同一输入逐项相等(迁移期间旧实现还在,这是真对账;迁移后 hooks/
// channel 成了 platform 的薄口,断言继续钉住两处领域口的输出合同)。
void CheckThreeAgree(std::string_view data) {
    const std::string p = lubancode::platform::Sha256Hex(data);
    const std::string h = lubancode::hooks::Sha256Hex(data);
    const std::string c = lubancode::channel::Sha256Hex(data);
    CHECK(p == h);
    CHECK(p == c);
    CHECK(p.size() == 64);
}

std::string Repeat(char c, std::size_t n) {
    return std::string(n, c);
}

}  // namespace

TEST_CASE("Sha256Hex: NIST/FIPS 公开向量") {
    using lubancode::platform::Sha256Hex;
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha256Hex(Repeat('a', 1'000'000)) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Sha256Hex: 55/56/63/64 字节填充边界逐格对账") {
    CheckThreeAgree(Repeat('x', 55));  // 55 -> 单块填充,长度顶格前最后一格
    CheckThreeAgree(Repeat('x', 56));  // 56 -> 长度字段挤到第二块
    CheckThreeAgree(Repeat('x', 63));  // 63 -> 0x80 一进就跨界
    CheckThreeAgree(Repeat('x', 64));  // 64 -> 整块后再补一块全填充
    CheckThreeAgree(Repeat('x', 65));
    // 混合内容边界(不是同字符重复,压缩轮吃的是真混合位)。
    std::string mixed;
    for (std::size_t i = 0; i < 64; ++i) {
        mixed.push_back(static_cast<char>((i * 37 + 11) & 0xff));
    }
    for (std::size_t cut : {0U, 1U, 54U, 55U, 56U, 57U, 62U, 63U, 64U, 65U}) {
        CheckThreeAgree(mixed.substr(0, cut));
    }
}

TEST_CASE("Sha256Hex: 含 NUL 字节的载荷按字节精确处理") {
    std::string with_nul = "ab";
    with_nul += '\0';
    with_nul += "cd";
    with_nul += '\0';
    CheckThreeAgree(with_nul);
    // NUL 打头/收尾。
    std::string nul_only(3, '\0');
    CheckThreeAgree(nul_only);
}

TEST_CASE("Sha256Hex: 大载荷(多块)对账") {
    std::string big;
    big.reserve(64 * 40 + 7);
    for (int i = 0; i < 40; ++i) {
        for (int j = 0; j < 64; ++j) {
            big.push_back(static_cast<char>((i * 31 + j * 7) & 0xff));
        }
    }
    big += "tail123";
    CheckThreeAgree(big);
}

TEST_CASE("Sha256Digest: 原始 32 字节摘要与十六进制口一致") {
    const std::string_view data = "digest raw";
    const auto digest = lubancode::platform::Sha256Digest(data);
    REQUIRE(digest.size() == 32);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const std::byte b : digest) {
        const unsigned v = static_cast<unsigned>(b);
        hex.push_back(kHex[v >> 4]);
        hex.push_back(kHex[v & 0x0f]);
    }
    CHECK(hex == lubancode::platform::Sha256Hex(data));
    CHECK(hex == lubancode::hooks::Sha256Hex(data));
}
