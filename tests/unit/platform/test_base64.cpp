// platform::Base64Encode 的向量回归(src 收口审计 P2:标准 Base64 此前
// 五处手写——tool_result_images/ws_frames/clipboard_posix/run_command/
// image_input,各留一份三字节分组循环)。向量按审计单 §三 P2:
//   - RFC 4648 官方向量;
//   - 空串、含 NUL、1/2/3 字节尾巴;
//   - 大载荷(图片量级,顺带钉预分配不炸);
//   - 与两处仍导出的旧口(cli::Base64Encode、app_server::ws::Base64Encode)
//     逐样本对账——迁移期间是真对账,迁移后旧口成了 platform 的薄口,
//     断言继续钉住两处领域口的输出合同(载荷不变)。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app_server/ws_frames.hpp"
#include "cli/image_input.hpp"
#include "platform/base64.hpp"

namespace {

void CheckAgreesWithOldSites(const std::string& bytes) {
    const std::string kernel = lubancode::platform::Base64Encode(std::string_view(bytes));
    CHECK(kernel == lubancode::cli::Base64Encode(std::vector<std::uint8_t>(bytes.begin(), bytes.end())));
    CHECK(kernel == lubancode::app_server::ws::Base64Encode(std::string_view(bytes)));
}

std::vector<std::byte> BytesOf(const std::string& s) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(s.data()),
                                  reinterpret_cast<const std::byte*>(s.data()) + s.size());
}

}  // namespace

TEST_CASE("Base64Encode: RFC 4648 官方向量") {
    using lubancode::platform::Base64Encode;
    CHECK(Base64Encode(std::string_view("")) == "");
    CHECK(Base64Encode(std::string_view("f")) == "Zg==");
    CHECK(Base64Encode(std::string_view("fo")) == "Zm8=");
    CHECK(Base64Encode(std::string_view("foo")) == "Zm9v");
    CHECK(Base64Encode(std::string_view("foob")) == "Zm9vYg==");
    CHECK(Base64Encode(std::string_view("fooba")) == "Zm9vYmE=");
    CHECK(Base64Encode(std::string_view("foobar")) == "Zm9vYmFy");
}

TEST_CASE("Base64Encode: 1/2/3 字节尾巴逐格") {
    using lubancode::platform::Base64Encode;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef";
    // 连着切:每 3 字节一组出 4 字符,尾巴按 1/2 字节补 '='。
    for (std::size_t len = 0; len <= 10; ++len) {
        const std::string in = alphabet.substr(0, len);
        const std::string out = Base64Encode(std::string_view(in));
        CHECK(out.size() == (len + 2) / 3 * 4);
        if (len % 3 == 1) {
            CHECK(out.substr(out.size() - 2) == "==");
        } else if (len % 3 == 2) {
            CHECK(out.substr(out.size() - 1) == "=");
        } else {
            CHECK(out.find('=') == std::string::npos);
        }
    }
}

TEST_CASE("Base64Encode: 含 NUL 字节与高位字节(符号性坑)") {
    using lubancode::platform::Base64Encode;
    std::string with_nul = "a";
    with_nul += '\0';
    with_nul += "b";
    // 0x61 0x00 0x62 -> YQBi
    CHECK(Base64Encode(std::string_view(with_nul)) == "YQBi");

    std::string high_bytes;
    high_bytes += static_cast<char>(0xFF);
    high_bytes += static_cast<char>(0xFE);
    high_bytes += static_cast<char>(0xFD);
    // 0xFF 0xFE 0xFD -> //79
    CHECK(Base64Encode(std::string_view(high_bytes)) == "//79");

    std::string lone_nul(1, '\0');
    CHECK(Base64Encode(std::string_view(lone_nul)) == "AA==");

    // span 口与 string_view 口同一结果。
    CHECK(lubancode::platform::Base64Encode(BytesOf(with_nul)) == "YQBi");
}

TEST_CASE("Base64Encode: 大载荷(图片量级)对账旧实现") {
    std::string big;
    big.reserve(300 * 1024);
    for (std::size_t i = 0; i < 300 * 1024; ++i) {
        big.push_back(static_cast<char>((i * 251 + 7) & 0xff));
    }
    const std::string encoded = lubancode::platform::Base64Encode(std::string_view(big));
    CHECK(encoded.size() == (big.size() + 2) / 3 * 4);
    CheckAgreesWithOldSites(big);
}

TEST_CASE("Base64Encode: 典型载荷样本对账旧实现(载荷迁移不变)") {
    CheckAgreesWithOldSites("");
    CheckAgreesWithOldSites("M");
    CheckAgreesWithOldSites("Ma");
    CheckAgreesWithOldSites("Man");
    CheckAgreesWithOldSites(std::string("ping-pong-payload\x01\x02\x03"));
}
