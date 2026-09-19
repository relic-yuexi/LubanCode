// Sha256Stream(更新器 C++ 化批一第③单)的流式合同回归 + miniz 链路冒烟。
// 合同(见 platform/sha256.hpp 头注):同一份数据按任意方式分块流式哈出
// 的摘要,必须与一次性 Sha256Hex 完全相等——分块只进尾段缓冲,不进算法。
// miniz 冒烟是给批二解包层打底:mz_zip_writer 在临时目录造一枚两文件的
// 小 zip(一压一存,踩 tinfl/tdef 与 store 两条路),mz_zip_reader 读回
// 文件名与字节,证明链路真编进来、真能用。
#include <doctest/doctest.h>
#include <miniz.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "platform/sha256.hpp"

namespace {

using lubancode::platform::Sha256Digest;
using lubancode::platform::Sha256Hex;
using lubancode::platform::Sha256Stream;

// 流式按 chunk_size 切块喂完;最后一块允许不足额。整块切法即"一次
// Update + Final",与一次性 wrapper 同路径。
std::string StreamHex(const std::string& data, std::size_t chunk_size) {
    Sha256Stream stream;
    for (std::size_t off = 0; off < data.size(); off += chunk_size) {
        const std::size_t n = std::min(chunk_size, data.size() - off);
        stream.Update(std::string_view(data).substr(off, n));
    }
    return stream.FinalHex();
}

// 确定性伪载荷:字节走满混合位,压缩轮吃的是真数据不是同字符重复。
std::string MixedPayload(std::size_t size) {
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<char>((i * 31 + (i >> 8) * 7) & 0xff));
    }
    return out;
}

}  // namespace

TEST_CASE("Sha256Stream: 任意分块 == 一次性(NIST 一百万 a 向量)") {
    const std::string data(1'000'000, 'a');
    const std::string expected =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    CHECK(Sha256Hex(data) == expected);
    CHECK(StreamHex(data, 1) == expected);
    CHECK(StreamHex(data, 7) == expected);
    CHECK(StreamHex(data, 64 * 1024) == expected);
    CHECK(StreamHex(data, data.size()) == expected);
}

TEST_CASE("Sha256Stream: 已知向量——空输入与 abc 各钉一例") {
    const std::string empty_hex =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    // 空输入:零次 Update 直接 Final,与一次性空串同摘要。
    {
        Sha256Stream stream;
        CHECK(stream.FinalHex() == empty_hex);
    }
    // 空块 Update 也是合法输入,不改摘要。
    CHECK(StreamHex("", 1) == empty_hex);
    CHECK(Sha256Hex("") == empty_hex);

    const std::string abc_hex =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(Sha256Hex("abc") == abc_hex);
    CHECK(StreamHex("abc", 1) == abc_hex);
    CHECK(StreamHex("abc", 2) == abc_hex);
    CHECK(StreamHex("abc", 100) == abc_hex);

    // 单字节。
    const std::string one(1, 'x');
    CHECK(StreamHex(one, 1) == Sha256Hex(one));
}

TEST_CASE("Sha256Stream: 跨块边界 63/64/65 字节,六种切法互证") {
    for (const std::size_t size : {std::size_t{63}, std::size_t{64}, std::size_t{65}}) {
        const std::string data = MixedPayload(size);
        const std::string expected = Sha256Hex(data);
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{63},
                                        std::size_t{64}, std::size_t{65}, std::size_t{128}}) {
            INFO("size=", size, " chunk=", chunk);
            CHECK(StreamHex(data, chunk) == expected);
        }
    }
}

TEST_CASE("Sha256Stream: string_view 便利重载与 byte span 口同摘要") {
    const std::string data = MixedPayload(1000);
    const std::string expected = Sha256Hex(data);

    Sha256Stream via_view;
    via_view.Update(std::string_view(data));
    CHECK(via_view.FinalHex() == expected);

    Sha256Stream via_bytes;
    via_bytes.Update(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()), data.size()));
    CHECK(via_bytes.FinalHex() == expected);

    // 混着喂:一口 string_view 一口 byte span,摘要不动摇。
    Sha256Stream mixed;
    mixed.Update(std::string_view(data).substr(0, 333));
    mixed.Update(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()) + 333, data.size() - 333));
    CHECK(mixed.FinalHex() == expected);
}

TEST_CASE("Sha256Stream: 大载荷多块,奇数块切法照等") {
    const std::string data = MixedPayload(64 * 40 + 7);
    const std::string expected = Sha256Hex(data);
    CHECK(StreamHex(data, 1) == expected);
    CHECK(StreamHex(data, 13) == expected);
    CHECK(StreamHex(data, 64 * 1024) == expected);
    CHECK(StreamHex(data, data.size()) == expected);
}

TEST_CASE("Sha256Stream: FinalDigest 32 字节与一次性口逐字节相等") {
    const std::string data = MixedPayload(300);
    Sha256Stream stream;
    stream.Update(data);
    const auto digest = stream.FinalDigest();
    const auto expected = Sha256Digest(data);
    REQUIRE(digest.size() == 32);
    REQUIRE(expected.size() == 32);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        CHECK(digest[i] == expected[i]);
    }
}

TEST_CASE("miniz 冒烟: mz_zip_writer 造两文件 zip,mz_zip_reader 读回") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lubancode-updater-miniz-smoke";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path zip_path = dir / "smoke.zip";

    // 两枚载荷:文本走 deflate(踩 tdef 压缩/tinfl 解压),二进制走 store
    // 直存(读回路径的另一条)。
    std::string text_payload;
    for (int i = 0; i < 200; ++i) {
        text_payload += "updater smoke line " + std::to_string(i) + "\n";
    }
    const std::string bin_payload = MixedPayload(4096);

    {
        mz_zip_archive zip = {};
        REQUIRE(mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0));
        // 压缩档位是 miniz 的匿名枚举(signed),mz_uint 形参收它 MSVC /W4 报
        // C4245,显式转一道。
        constexpr mz_uint kDeflate = static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION);
        constexpr mz_uint kStore = static_cast<mz_uint>(MZ_NO_COMPRESSION);
        REQUIRE(mz_zip_writer_add_mem(&zip, "docs/hello.txt", text_payload.data(),
                                      text_payload.size(), kDeflate));
        REQUIRE(mz_zip_writer_add_mem(&zip, "payload.bin", bin_payload.data(),
                                      bin_payload.size(), kStore));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        mz_zip_writer_end(&zip);
    }

    {
        mz_zip_archive zip = {};
        REQUIRE(mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0));
        REQUIRE(mz_zip_reader_get_num_files(&zip) == 2);

        char name[256];
        REQUIRE(mz_zip_reader_get_filename(&zip, 0, name, sizeof(name)));
        CHECK(std::string(name) == "docs/hello.txt");
        REQUIRE(mz_zip_reader_get_filename(&zip, 1, name, sizeof(name)));
        CHECK(std::string(name) == "payload.bin");

        std::size_t out_size = 0;
        void* text_out =
            mz_zip_reader_extract_file_to_heap(&zip, "docs/hello.txt", &out_size, 0);
        REQUIRE(text_out != nullptr);
        CHECK(out_size == text_payload.size());
        CHECK(std::memcmp(text_out, text_payload.data(), out_size) == 0);
        mz_free(text_out);

        void* bin_out = mz_zip_reader_extract_file_to_heap(&zip, "payload.bin", &out_size, 0);
        REQUIRE(bin_out != nullptr);
        CHECK(out_size == bin_payload.size());
        CHECK(std::memcmp(bin_out, bin_payload.data(), out_size) == 0);
        mz_free(bin_out);

        mz_zip_reader_end(&zip);
    }
}
