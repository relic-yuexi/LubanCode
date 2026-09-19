// 更新助手 C++ 化·批二第①单:zip + tar.gz 安全解包器(UnpackArchive)
// 的集成回归册。语义对齐 scripts/updater.py 的 unpack_archive,矩阵照
// 派工单:路径穿越/链接/设备/FIFO 拒、炸弹帽、GNU 长名、pax 覆写、坏
// CRC、截断、正常包逐文件字节+执行位对账、zip64 认。
//
// 夹具全在册内现场造,不进 fixtures/:
//   - zip:mz_zip_writer(deflate 路,writer 包)+ 手写 STORED 头
//     (external_attr 全控:符号链接案/保留名案/执行位案/条目级 zip64 案
//     ——中央目录 0xFFFFFFFF 占位 + zip64 扩展域,与真 4GiB 条目走的是
//     reader 同一条替换路径,不必真造 4GiB);外加 MZ_ZIP_FLAG_WRITE_ZIP64
//     强制的 EOCD64 包。
//   - tar.gz:测试内 tar 头写入器(全控 typeflag/mode/prefix/长名/pax/
//     base-256)+ miniz tdefl 封 gzip 容器;坏包就地改字节。
#include <doctest/doctest.h>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "updater/archive.hpp"
#include "updater/paths.hpp"

namespace {

namespace fs = std::filesystem;
using lubancode::updater::UnpackArchive;

// ---------------------------------------------------------------------------
// 基础件
// ---------------------------------------------------------------------------

// 确定性伪载荷:字节走满混合位,deflate 路吃的是真数据。
std::string MixedPayload(std::size_t size) {
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<char>((i * 31 + (i >> 8) * 7) & 0xff));
    }
    return out;
}

fs::path MakeCaseDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "lubancode-updater-unpack" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// POSIX 上真验执行位;Windows 上 permissions 对执行位是 no-op,恒 false,
// 相关断言由调用侧 #if 摘掉。
bool HasExecBit(const fs::path& path) {
#if !defined(_WIN32)
    std::error_code ec;
    const fs::perms pr = fs::status(path, ec).permissions();
    return !ec && (pr & fs::perms::owner_exec) != fs::perms::none;
#else
    (void)path;
    return false;
#endif
}

void PutLe16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
void PutLe32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 24) & 0xff));
}
void PutLe64(std::string& out, std::uint64_t v) {
    PutLe32(out, static_cast<std::uint32_t>(v & 0xffffffff));
    PutLe32(out, static_cast<std::uint32_t>(v >> 32));
}

// ---------------------------------------------------------------------------
// 手写 STORED zip:external_attr / zip64 域全控
// ---------------------------------------------------------------------------

struct StoredZipEntry {
    std::string name;
    std::uint32_t posix_mode = 0100644;  // external_attr >> 16(文件缺省)
    std::string data;
    bool zip64 = false;  // 中央目录 0xFFFFFFFF 占位 + zip64 扩展域
};

std::string BuildStoredZip(const std::vector<StoredZipEntry>& entries) {
    struct CentralRec {
        std::string name;
        std::uint32_t mode = 0;
        std::uint32_t crc = 0;
        std::uint64_t size = 0;
        std::uint64_t local_ofs = 0;
        bool zip64 = false;
    };
    std::string out;
    std::vector<CentralRec> cdir;
    for (const StoredZipEntry& e : entries) {
        CentralRec rec;
        rec.name = e.name;
        rec.mode = e.posix_mode;
        rec.size = e.data.size();
        rec.crc = static_cast<std::uint32_t>(mz_crc32(
            0, reinterpret_cast<const mz_uint8*>(e.data.data()), e.data.size()));
        rec.local_ofs = out.size();
        rec.zip64 = e.zip64;
        cdir.push_back(rec);

        // 本地头:zip64 条目带 16 字节扩展域(解压/压缩尺寸),32 位域清零。
        std::string local_extra;
        if (e.zip64) {
            PutLe16(local_extra, 0x0001);
            PutLe16(local_extra, 16);
            PutLe64(local_extra, rec.size);
            PutLe64(local_extra, rec.size);
        }
        PutLe32(out, 0x04034b50);                       // 本地头签名
        PutLe16(out, e.zip64 ? 45 : 20);                // 需要的版本
        PutLe16(out, 0);                                // 通用位标志
        PutLe16(out, 0);                                // STORED
        PutLe16(out, 0);                                // 时间
        PutLe16(out, 0);                                // 日期
        PutLe32(out, rec.crc);
        PutLe32(out, e.zip64 ? 0 : static_cast<std::uint32_t>(rec.size));
        PutLe32(out, e.zip64 ? 0 : static_cast<std::uint32_t>(rec.size));
        PutLe16(out, static_cast<std::uint16_t>(e.name.size()));
        PutLe16(out, static_cast<std::uint16_t>(local_extra.size()));
        out += e.name;
        out += local_extra;
        out += e.data;
    }

    const std::uint64_t cdir_ofs = out.size();
    std::string cdir_buf;
    for (const CentralRec& rec : cdir) {
        // 中央 zip64 扩展域:占位 0xFFFFFFFF 的域(解压/压缩尺寸)按
        // 顺序各给 8 字节真值;本地头偏移不占位就不给。
        std::string central_extra;
        if (rec.zip64) {
            PutLe16(central_extra, 0x0001);
            PutLe16(central_extra, 16);
            PutLe64(central_extra, rec.size);
            PutLe64(central_extra, rec.size);
        }
        PutLe32(cdir_buf, 0x02014b50);                  // 中央头签名
        PutLe16(cdir_buf, (3u << 8) | 45u);             // made by:unix/4.5
        PutLe16(cdir_buf, rec.zip64 ? 45 : 20);         // 需要的版本
        PutLe16(cdir_buf, 0);                           // 通用位标志
        PutLe16(cdir_buf, 0);                           // STORED
        PutLe16(cdir_buf, 0);                           // 时间
        PutLe16(cdir_buf, 0);                           // 日期
        PutLe32(cdir_buf, rec.crc);
        PutLe32(cdir_buf,
                rec.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(rec.size));
        PutLe32(cdir_buf,
                rec.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(rec.size));
        PutLe16(cdir_buf, static_cast<std::uint16_t>(rec.name.size()));
        PutLe16(cdir_buf, static_cast<std::uint16_t>(central_extra.size()));
        PutLe16(cdir_buf, 0);                           // 注记长度
        PutLe16(cdir_buf, 0);                           // 起始盘号
        PutLe16(cdir_buf, 0);                           // 内部属性
        PutLe32(cdir_buf, rec.mode << 16);              // 外部属性 = POSIX mode
        PutLe32(cdir_buf, static_cast<std::uint32_t>(rec.local_ofs));
        cdir_buf += rec.name;
        cdir_buf += central_extra;
    }
    out += cdir_buf;

    PutLe32(out, 0x06054b50);                           // EOCD 签名
    PutLe16(out, 0);
    PutLe16(out, 0);
    PutLe16(out, static_cast<std::uint16_t>(entries.size()));
    PutLe16(out, static_cast<std::uint16_t>(entries.size()));
    PutLe32(out, static_cast<std::uint32_t>(cdir_buf.size()));
    PutLe32(out, static_cast<std::uint32_t>(cdir_ofs));
    PutLe16(out, 0);
    return out;
}

// ---------------------------------------------------------------------------
// tar 头写入器 + gzip 封装
// ---------------------------------------------------------------------------

class TarBuilder {
  public:
    void AddDir(const std::string& name, std::uint32_t mode = 0040755) {
        PutHeader(name, mode, 0, '5');
    }

    void AddFile(const std::string& name, std::string_view data,
                 std::uint32_t mode = 0100644, char typeflag = '0') {
        PutHeader(name, mode, data.size(), typeflag);
        PutData(data);
    }

    // ustar prefix:name 放不下/放得下都由调用侧拼。
    void AddFileWithPrefix(const std::string& name, const std::string& prefix,
                           std::string_view data, std::uint32_t mode = 0100644) {
        PutHeader(name, mode, data.size(), '0', {}, prefix);
        PutData(data);
    }

    // GNU 'L':长名给下一枚成员。
    void AddGnuLongName(const std::string& long_name) {
        const std::string data = long_name + std::string(1, '\0');
        PutHeader("././@LongLink", 0100644, data.size(), 'L');
        PutData(data);
    }

    // GNU 'K':长链接名(链接成员进不了落盘,这里只验消费不炸)。
    void AddGnuLongLinkName(const std::string& long_link) {
        const std::string data = long_link + std::string(1, '\0');
        PutHeader("././@LongLink", 0100644, data.size(), 'K');
        PutData(data);
    }

    // pax 扩展头:'x' 作用下一枚,'g' 全局。
    void AddPax(const std::vector<std::pair<std::string, std::string>>& records,
                char type = 'x') {
        const std::string data = BuildPaxRecords(records);
        PutHeader("pax-header", 0100644, data.size(), type);
        PutData(data);
    }

    // 基尺寸走 GNU base-256(size 域首字节 0x80 置位)。
    void AddFileBase256Size(const std::string& name, std::string_view data,
                            std::uint32_t mode = 0100644) {
        PutHeader(name, mode, data.size(), '0', {}, {}, true);
        PutData(data);
    }

    // pax size= 覆写案:头里 size 写 0,数据块按真实载荷写(读侧按
    // pax 记录的尺寸收)。
    void AddHeaderForPaxSized(const std::string& name, std::string_view data,
                              std::uint32_t mode = 0100644) {
        PutHeader(name, mode, 0, '0');
        PutData(data);
    }

    // 链接/设备/FIFO 类成员:只出头(尺寸 0;拒收判型用不着数据块)。
    void AddMemberWithLinkName(const std::string& name, char typeflag,
                               const std::string& linkname) {
        PutHeader(name, 0100644, 0, typeflag, linkname);
    }

    // 病态目录头:类型 '5' 但带数据块(tarfile 按偏移账跳过)。
    void AddDirWithPayload(const std::string& name, std::string_view payload) {
        PutHeader(name, 0040755, payload.size(), '5');
        PutData(payload);
    }

    void AddEnd(std::size_t extra_zero_blocks = 0) {
        out_.append((2 + extra_zero_blocks) * 512, '\0');
    }

    const std::string& bytes() const { return out_; }
    std::string take() { return std::move(out_); }

  private:
    static std::string BuildPaxRecords(
        const std::vector<std::pair<std::string, std::string>>& records) {
        std::string out;
        for (const auto& [key, value] : records) {
            const std::string payload = key + "=" + value + "\n";
            std::size_t len = payload.size() + 2;
            for (;;) {
                const std::size_t digits = std::to_string(len).size();
                if (digits + 1 + payload.size() == len) {
                    break;
                }
                len = digits + 1 + payload.size();
            }
            out += std::to_string(len) + " " + payload;
        }
        return out;
    }

    void PutOctalField(unsigned char* dst, std::size_t width, std::uint64_t v) {
        // 经典形:前导 0 的八进制 + NUL。
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%0*llo",
                      static_cast<int>(width - 1),
                      static_cast<unsigned long long>(v));
        std::memcpy(dst, buf, width);
        dst[width - 1] = 0;
    }

    void PutHeader(const std::string& name, std::uint32_t mode,
                   std::uint64_t size, char typeflag,
                   std::string_view linkname = {}, std::string_view prefix = {},
                   bool base256_size = false) {
        std::array<unsigned char, 512> block{};
        REQUIRE(name.size() <= 99);
        REQUIRE(prefix.size() <= 154);
        std::memcpy(block.data(), name.data(), name.size());
        PutOctalField(block.data() + 100, 8, mode);
        PutOctalField(block.data() + 108, 8, 0);   // uid
        PutOctalField(block.data() + 116, 8, 0);   // gid
        if (base256_size) {
            // GNU base-256:12 字节,首字节 0x80,余 11 字节大端。
            block[124] = 0x80;
            for (int i = 0; i < 11; ++i) {
                block[125 + i] =
                    static_cast<unsigned char>((size >> (8 * (10 - i))) & 0xff);
            }
        } else {
            PutOctalField(block.data() + 124, 12, size);
        }
        PutOctalField(block.data() + 136, 12, 0);  // mtime
        block[156] = static_cast<unsigned char>(typeflag);
        if (!linkname.empty()) {
            REQUIRE(linkname.size() <= 99);
            std::memcpy(block.data() + 157, linkname.data(), linkname.size());
        }
        // POSIX ustar 魔数("ustar\0" + "00"),prefix 域才被认。
        std::memcpy(block.data() + 257, "ustar\0", 6);
        block[263] = '0';
        block[264] = '0';
        if (!prefix.empty()) {
            std::memcpy(block.data() + 345, prefix.data(), prefix.size());
        }
        // 校验和:chksum 域(148..155)按 8 个空格算的无符号和,写
        // "%06o\0 "——读侧同口径(spec 如此,按零算会差 256)。
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < block.size(); ++i) {
            const unsigned char c = (i >= 148 && i < 156)
                                         ? static_cast<unsigned char>(' ')
                                         : block[i];
            sum += c;
        }
        PutOctalField(block.data() + 148, 7, sum);
        block[155] = ' ';
        out_.append(reinterpret_cast<const char*>(block.data()), block.size());
    }

    void PutData(std::string_view data) {
        out_.append(data);
        const std::size_t pad = (512 - data.size() % 512) % 512;
        out_.append(pad, '\0');
    }

    std::string out_;
};

// gzip 容器:头(可选 FNAME)+ tdefl 原生 deflate + CRC32/ISIZE 尾。
std::string GzipCompress(const std::string& raw, const char* fname = nullptr) {
    std::string out(10, '\0');
    out[0] = static_cast<char>(0x1f);
    out[1] = static_cast<char>(0x8b);
    out[2] = 8;                                        // deflate
    out[3] = static_cast<char>(fname ? 0x08 : 0);      // FNAME
    out[9] = 3;                                        // OS:unix
    if (fname != nullptr) {
        out += std::string(fname) + std::string(1, '\0');
    }
    std::vector<char> buf(raw.size() + raw.size() / 16 + 4096);
    const std::size_t comp = tdefl_compress_mem_to_mem(
        buf.data(), buf.size(), raw.data(), raw.size(), TDEFL_DEFAULT_MAX_PROBES);
    REQUIRE(comp != 0);
    out.append(buf.data(), comp);
    const std::uint32_t crc = static_cast<std::uint32_t>(mz_crc32(
        0, reinterpret_cast<const mz_uint8*>(raw.data()), raw.size()));
    PutLe32(out, crc);
    PutLe32(out, static_cast<std::uint32_t>(raw.size() & 0xffffffffu));
    return out;
}

// 多成员 gzip:把 raw 劈成两段各自成成员(tar 块骑缝也认)。
std::string GzipCompressMulti(const std::string& raw, std::size_t split) {
    REQUIRE(split < raw.size());
    return GzipCompress(raw.substr(0, split)) + GzipCompress(raw.substr(split));
}

}  // namespace

// ---------------------------------------------------------------------------
// zip:正常路(writer 造,deflate + store 双法)
// ---------------------------------------------------------------------------

TEST_CASE("UnpackArchive(zip): writer 正常包逐文件字节对账,总量只计文件") {
    const fs::path dir = MakeCaseDir("zip-writer-normal");
    const fs::path zip_path = dir / "pkg.zip";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    const std::string readme = "lubancode update smoke\nline two\n";
    const std::string payload = MixedPayload(70000);  // 越过 64KB 缓冲边界
    const std::uint64_t expect_total =
        readme.size() + payload.size() + 0 /* empty.txt */;

    {
        mz_zip_archive zip = {};
        REQUIRE(mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0));
        constexpr mz_uint kDeflate = static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION);
        constexpr mz_uint kStore = static_cast<mz_uint>(MZ_NO_COMPRESSION);
        REQUIRE(mz_zip_writer_add_mem(&zip, "docs/", "", 0, kStore));
        REQUIRE(mz_zip_writer_add_mem(&zip, "docs/readme.txt", readme.data(),
                                      readme.size(), kDeflate));
        REQUIRE(mz_zip_writer_add_mem(&zip, "bin/payload.dat", payload.data(),
                                      payload.size(), kStore));
        REQUIRE(mz_zip_writer_add_mem(&zip, "empty.txt", "", 0, kDeflate));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        mz_zip_writer_end(&zip);
    }

    const auto res = UnpackArchive(zip_path, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(*res == expect_total);
    CHECK(ReadBytes(dest / "docs" / "readme.txt") == readme);
    CHECK(ReadBytes(dest / "bin" / "payload.dat") == payload);
    CHECK(ReadBytes(dest / "empty.txt").empty());
    CHECK_FALSE(HasExecBit(dest / "bin" / "payload.dat"));  // 0644 无 x 位
}

TEST_CASE("UnpackArchive(zip): 强制 zip64 包(EOCD64)认,读回仍对账") {
    const fs::path dir = MakeCaseDir("zip-force64");
    const fs::path zip_path = dir / "pkg64.zip";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);
    const std::string data = MixedPayload(3000);

    {
        mz_zip_archive zip = {};
        REQUIRE(mz_zip_writer_init_file_v2(&zip, zip_path.string().c_str(), 0,
                                           MZ_ZIP_FLAG_WRITE_ZIP64));
        constexpr mz_uint kDeflate = static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION);
        REQUIRE(mz_zip_writer_add_mem(&zip, "big/entry.bin", data.data(),
                                      data.size(), kDeflate));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        mz_zip_writer_end(&zip);
    }
    // 夹具自证:这真是 zip64(EOCD64 在场),不是普通包冒名。
    {
        mz_zip_archive zip = {};
        REQUIRE(mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0));
        CHECK(mz_zip_is_zip64(&zip));
        mz_zip_reader_end(&zip);
    }

    const auto res = UnpackArchive(zip_path, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(*res == data.size());
    CHECK(ReadBytes(dest / "big" / "entry.bin") == data);
}

TEST_CASE("UnpackArchive(zip): 条目级 zip64 域(0xFFFFFFFF 占位+扩展域)认") {
    const fs::path dir = MakeCaseDir("zip-entry64");
    const fs::path zip_path = dir / "entry64.zip";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);
    const std::string data = MixedPayload(1234);

    WriteBytes(zip_path,
               BuildStoredZip({{"deep/zip64.bin", 0100644, data, true}}));
    // 夹具自证:条目走了 zip64 扩展域替换路,尺寸读回真值。
    {
        mz_zip_archive zip = {};
        REQUIRE(mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0));
        CHECK(mz_zip_is_zip64(&zip));
        mz_zip_archive_file_stat stat = {};
        REQUIRE(mz_zip_reader_file_stat(&zip, 0, &stat));
        CHECK(stat.m_uncomp_size == data.size());
        mz_zip_reader_end(&zip);
    }

    const auto res = UnpackArchive(zip_path, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(*res == data.size());
    CHECK(ReadBytes(dest / "deep" / "zip64.bin") == data);
}

TEST_CASE("UnpackArchive(zip): 符号链接成员整包拒") {
    const fs::path dir = MakeCaseDir("zip-symlink");
    const fs::path zip_path = dir / "evil.zip";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    // 0o120777:S_IFLNK + 777,python zipfile 同款判法。
    WriteBytes(zip_path, BuildStoredZip({{"target.txt", 0100644, "ok", false},
                                         {"hook", 0120777, "target.txt", false}}));
    const auto res = UnpackArchive(zip_path, dest, std::nullopt);
    REQUIRE_FALSE(res.has_value());
    CHECK_MESSAGE(res.error().find("符号链接") != std::string::npos, res.error());
}

TEST_CASE("UnpackArchive(zip): 路径穿越/绝对/盘符/保留名/反斜杠一律拒") {
    const fs::path dir = MakeCaseDir("zip-traversal");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);
    const std::string payload = "x";

    const std::vector<std::string> evil_names = {
        "../evil.txt",      // 父目录穿越
        "/etc/passwd",      // 绝对路径
        "C:/evil.txt",      // 盘符
        "..\\evil.txt",     // 反斜杠折正斜杠后仍是穿越
        "com1.md",          // Windows 保留名(stem 命中)
        "a/../../evil.txt", // 中段穿越
    };
    for (std::size_t i = 0; i < evil_names.size(); ++i) {
        INFO("name=", evil_names[i]);
        const fs::path zip_path = dir / ("evil-" + std::to_string(i) + ".zip");
        WriteBytes(zip_path,
                   BuildStoredZip({{evil_names[i], 0100644, payload, false}}));
        const auto res = UnpackArchive(zip_path, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("路径不合法") != std::string::npos,
                      res.error());
    }
}

TEST_CASE("UnpackArchive(zip): STORED 条目执行位(external_attr)落盘") {
    const fs::path dir = MakeCaseDir("zip-execbit");
    const fs::path zip_path = dir / "exec.zip";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    WriteBytes(zip_path, BuildStoredZip({{"runner.sh", 0100755, "#!/bin/sh\n", false},
                                         {"plain.txt", 0100644, "data", false}}));
    const auto res = UnpackArchive(zip_path, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(ReadBytes(dest / "runner.sh") == "#!/bin/sh\n");
#if !defined(_WIN32)
    CHECK(HasExecBit(dest / "runner.sh"));
    CHECK_FALSE(HasExecBit(dest / "plain.txt"));
#endif
}

TEST_CASE("UnpackArchive(zip): 坏 CRC 与截断包拒") {
    const fs::path dir = MakeCaseDir("zip-corrupt");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);
    const std::string payload = MixedPayload(5000);

    // 坏 CRC:STORED 数据区(30 + 名长)翻一字节。
    {
        std::string zip = BuildStoredZip({{"data.bin", 0100644, payload, false}});
        constexpr std::size_t kNameLen = 8;  // "data.bin"
        zip[30 + kNameLen + 7] ^= 0x40;
        const fs::path p = dir / "bad-crc.zip";
        WriteBytes(p, zip);
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("解压失败") != std::string::npos, res.error());
    }
    // 截断:剁掉尾巴,EOCD 没了,开门就拒。
    {
        std::string zip = BuildStoredZip({{"data.bin", 0100644, payload, false}});
        zip.resize(zip.size() - 30);
        const fs::path p = dir / "truncated.zip";
        WriteBytes(p, zip);
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("打不开或损坏") != std::string::npos,
                      res.error());
    }
}

TEST_CASE("UnpackArchive(zip): 炸弹帽(小 declared_size 注入)拒") {
    const fs::path dir = MakeCaseDir("zip-bomb");
    const fs::path zip_path = dir / "bomb.zip";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    WriteBytes(zip_path, BuildStoredZip({{"big.bin", 0100644, MixedPayload(100000), false}}));
    // declared 1000 → 帽 6000,首枚成员就爆。
    const auto res = UnpackArchive(zip_path, dest, std::uint64_t{1000});
    REQUIRE_FALSE(res.has_value());
    CHECK_MESSAGE(res.error().find("总量超过上限") != std::string::npos, res.error());
}

// ---------------------------------------------------------------------------
// tar.gz:正常路与矩阵
// ---------------------------------------------------------------------------

TEST_CASE("UnpackArchive(tar.gz): 正常包逐文件字节+执行位对账,总量只计文件") {
    const fs::path dir = MakeCaseDir("targz-normal");
    const fs::path tgz = dir / "pkg.tar.gz";
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    const std::string script = "#!/bin/sh\necho hi\n";
    const std::string doc = "release notes\n";
    const std::string payload = MixedPayload(200000);  // 越过 32KB 窗/64KB 缓冲多层
    TarBuilder tb;
    tb.AddDir("bin/");
    tb.AddDir("share/doc/");
    tb.AddFile("bin/lubancode", script, 0100755);
    tb.AddFile("share/doc/notes.txt", doc, 0100644);
    tb.AddFile("share/big.bin", payload, 0100644);
    tb.AddFile("share/empty", "", 0100644);
    tb.AddEnd();
    WriteBytes(tgz, GzipCompress(tb.take()));

    const std::uint64_t expect_total =
        script.size() + doc.size() + payload.size() + 0;
    const auto res = UnpackArchive(tgz, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(*res == expect_total);
    CHECK(ReadBytes(dest / "bin" / "lubancode") == script);
    CHECK(ReadBytes(dest / "share" / "doc" / "notes.txt") == doc);
    CHECK(ReadBytes(dest / "share" / "big.bin") == payload);
    CHECK(ReadBytes(dest / "share" / "empty").empty());
#if !defined(_WIN32)
    CHECK(HasExecBit(dest / "bin" / "lubancode"));
    CHECK_FALSE(HasExecBit(dest / "share" / "doc" / "notes.txt"));
#endif
}

TEST_CASE("UnpackArchive(tar.gz): 路径穿越/绝对/盘符/保留名拒,点段与空段折叠认") {
    const fs::path dir = MakeCaseDir("targz-traversal");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    const std::vector<std::string> evil_names = {
        "../evil.txt", "/abs/evil.txt", "C:/evil.txt", "a/../b.txt", "com1.md",
    };
    for (std::size_t i = 0; i < evil_names.size(); ++i) {
        TarBuilder tb;
        tb.AddFile(evil_names[i], "x");
        tb.AddEnd();
        const fs::path tgz = dir / ("evil-" + std::to_string(i) + ".tar.gz");
        WriteBytes(tgz, GzipCompress(tb.take()));
        const auto res = UnpackArchive(tgz, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("路径不合法") != std::string::npos, res.error());
    }

    // 合法折叠:./x 与 dir//x 折完照落。
    TarBuilder tb;
    tb.AddFile("./folded.txt", "folded");
    tb.AddFile("dir//nested.txt", "nested");
    tb.AddEnd();
    const fs::path ok_tgz = dir / "fold.tar.gz";
    WriteBytes(ok_tgz, GzipCompress(tb.take()));
    const auto ok = UnpackArchive(ok_tgz, dest, std::nullopt);
    REQUIRE(ok.has_value());
    CHECK(ReadBytes(dest / "folded.txt") == "folded");
    CHECK(ReadBytes(dest / "dir" / "nested.txt") == "nested");
}

TEST_CASE("UnpackArchive(tar.gz): 符号链接/硬链接/设备/FIFO/contiguous 拒") {
    const fs::path dir = MakeCaseDir("targz-types");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    struct Case {
        const char* label;
        char typeflag;
        std::string linkname;
        const char* want_error;
    };
    const std::vector<Case> cases = {
        {"symlink", '2', "target.txt", "归档含链接"},
        {"hardlink", '1', "target.txt", "归档含链接"},
        {"chardev", '3', "", "归档含设备/管道"},
        {"blockdev", '4', "", "归档含设备/管道"},
        {"fifo", '6', "", "归档含设备/管道"},
        {"contiguous", '7', "", "归档成员不是普通文件"},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        INFO("case=", c.label);
        TarBuilder tb;
        tb.AddFile("target.txt", "x");
        // 链接成员带 linkname 走真头;设备/FIFO 不带。typeflag 走
        // AddFile 的第五参,linkname 由 PutHeader 的链接名参数塞进去。
        tb.AddMemberWithLinkName("member", c.typeflag, c.linkname);
        tb.AddEnd();
        const fs::path tgz = dir / ("type-" + std::to_string(i) + ".tar.gz");
        WriteBytes(tgz, GzipCompress(tb.take()));
        const auto res = UnpackArchive(tgz, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find(c.want_error) != std::string::npos,
                      res.error());
    }
}

TEST_CASE("UnpackArchive(tar.gz): GNU 'L' 长名认,'K' 长链接名不碍事") {
    const fs::path dir = MakeCaseDir("targz-gnulong");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    // 长名 > 100 字符,name 域装不下,得走 'L'。总长压在 260 内,
    // Windows 长路径不必劳驾注册表。
    const std::string segment(60, 'a');
    const std::string long_name = segment + "/" + segment + "/leaf.txt";
    REQUIRE(long_name.size() > 100);

    TarBuilder tb;
    tb.AddGnuLongName(long_name);
    tb.AddFile("placeholder-name-field", "long-name-payload");
    tb.AddGnuLongLinkName("ignored-link-target");
    tb.AddFile("plain.txt", "plain");
    tb.AddEnd();
    const fs::path tgz = dir / "gnulong.tar.gz";
    WriteBytes(tgz, GzipCompress(tb.take()));

    const auto res = UnpackArchive(tgz, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(ReadBytes(dest / fs::path(long_name)) == "long-name-payload");
    CHECK(ReadBytes(dest / "plain.txt") == "plain");
}

TEST_CASE("UnpackArchive(tar.gz): pax path= 覆写认,覆写后再过筛查") {
    const fs::path dir = MakeCaseDir("targz-pax");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    // 覆写成良名:落在覆写名,原名不出现。
    {
        TarBuilder tb;
        tb.AddPax({{"path", "renamed/dir/file.txt"}});
        tb.AddFile("original-name.txt", "payload");
        tb.AddEnd();
        const fs::path tgz = dir / "pax-rename.tar.gz";
        WriteBytes(tgz, GzipCompress(tb.take()));
        const auto res = UnpackArchive(tgz, dest, std::nullopt);
        REQUIRE(res.has_value());
        CHECK(ReadBytes(dest / "renamed" / "dir" / "file.txt") == "payload");
        CHECK_FALSE(fs::exists(dest / "original-name.txt"));
    }
    // 覆写成邪名:筛查在覆写之后,整包拒。
    {
        TarBuilder tb;
        tb.AddPax({{"path", "../escaped.txt"}});
        tb.AddFile("original-name.txt", "payload");
        tb.AddEnd();
        const fs::path tgz = dir / "pax-evil.tar.gz";
        WriteBytes(tgz, GzipCompress(tb.take()));
        const auto res = UnpackArchive(tgz, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("路径不合法") != std::string::npos, res.error());
    }
    // pax size= 覆写:头里 0,记录里 5,按 5 读。
    {
        TarBuilder tb;
        tb.AddPax({{"size", "5"}});
        tb.AddHeaderForPaxSized("sized.txt", "hello", 0100644);
        tb.AddEnd();
        const fs::path tgz = dir / "pax-size.tar.gz";
        WriteBytes(tgz, GzipCompress(tb.take()));
        const auto res = UnpackArchive(tgz, dest, std::nullopt);
        REQUIRE(res.has_value());
        CHECK(ReadBytes(dest / "sized.txt") == "hello");
        CHECK(*res == 5);
    }
}

TEST_CASE("UnpackArchive(tar.gz): ustar prefix 拼接与 base-256 尺寸认") {
    const fs::path dir = MakeCaseDir("targz-prefix-b256");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    TarBuilder tb;
    tb.AddFileWithPrefix("leaf.txt", "deep/dir", "prefix-payload");
    tb.AddFileBase256Size("b256.bin", "base-256-payload");
    tb.AddEnd();
    const fs::path tgz = dir / "prefix.tar.gz";
    WriteBytes(tgz, GzipCompress(tb.take()));

    const auto res = UnpackArchive(tgz, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(ReadBytes(dest / "deep" / "dir" / "leaf.txt") == "prefix-payload");
    CHECK(ReadBytes(dest / "b256.bin") == "base-256-payload");
}

TEST_CASE("UnpackArchive(tar.gz): 多成员 gzip 与 FNAME 头认") {
    const fs::path dir = MakeCaseDir("targz-multimember");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    TarBuilder tb;
    tb.AddFile("first.txt", MixedPayload(1500));  // 骑缝块:700 处劈
    tb.AddFile("second.txt", "second-payload");
    tb.AddEnd();
    const std::string tar_bytes = tb.take();
    // 700 不是 512 的倍数,成员数据块骑在 gzip 成员缝上。
    const fs::path tgz = dir / "multi.tar.gz";
    WriteBytes(tgz, GzipCompressMulti(tar_bytes, 700));
    const auto res = UnpackArchive(tgz, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(ReadBytes(dest / "first.txt") == MixedPayload(1500));
    CHECK(ReadBytes(dest / "second.txt") == "second-payload");

    // FNAME 头(tar czf 常见形)照认。
    const fs::path tgz_named = dir / "named.tar.gz";
    WriteBytes(tgz_named, GzipCompress(tar_bytes, "fixture.tar"));
    fs::create_directories(dest / "2");
    const auto res2 = UnpackArchive(tgz_named, dest / "2", std::nullopt);
    REQUIRE(res2.has_value());
    CHECK(ReadBytes(dest / "2" / "second.txt") == "second-payload");
}

TEST_CASE("UnpackArchive(tar.gz): 坏 CRC/坏 ISIZE/截断流/截断 tar 全拒") {
    const fs::path dir = MakeCaseDir("targz-corrupt");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    TarBuilder tb;
    tb.AddFile("data.bin", MixedPayload(3000));
    tb.AddEnd();
    const std::string good = GzipCompress(tb.take());

    // 坏 CRC:尾 8 字节里前 4 是 CRC,翻它。
    {
        std::string bad = good;
        bad[bad.size() - 8] ^= 0x01;
        const fs::path p = dir / "bad-crc.tar.gz";
        WriteBytes(p, bad);
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("CRC") != std::string::npos, res.error());
    }
    // 坏 ISIZE:后 4 字节翻它。
    {
        std::string bad = good;
        bad[bad.size() - 4] ^= 0x01;
        const fs::path p = dir / "bad-isize.tar.gz";
        WriteBytes(p, bad);
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("ISIZE") != std::string::npos, res.error());
    }
    // 截断流:剁 10 字节,deflate 或尾账必炸。
    {
        std::string bad = good;
        bad.resize(bad.size() - 10);
        const fs::path p = dir / "cut.tar.gz";
        WriteBytes(p, bad);
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
    }
    // 截断 tar:gzip 合法但成员后没有结尾零块(python 会宽容,C++ 侧拒)。
    {
        TarBuilder cut;
        cut.AddFile("data.bin", MixedPayload(3000));
        const fs::path p = dir / "no-end.tar.gz";
        WriteBytes(p, GzipCompress(cut.take()));
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("结尾零块") != std::string::npos, res.error());
    }
    // 坏头校验和:第二枚成员的 mtime 域翻一字节。
    {
        TarBuilder bad;
        bad.AddFile("first.txt", "one");
        bad.AddFile("second.txt", "two");
        bad.AddEnd();
        std::string tar = bad.take();
        // 第二枚头起于 512+512(首枚 512 头 + 0 数据 + 0 补白);mtime 域
        // 偏移 136。
        tar[1024 + 136] ^= 0x01;
        const fs::path p = dir / "bad-chksum.tar.gz";
        WriteBytes(p, GzipCompress(tar));
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("校验和") != std::string::npos, res.error());
    }
}

TEST_CASE("UnpackArchive(tar.gz): 零块后拖非零尾巴拒,病态目录头照跳数据") {
    const fs::path dir = MakeCaseDir("targz-tail");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    // 结尾零块后再拖 600 字节垃圾(重封 gzip,合法流)。
    {
        TarBuilder tb;
        tb.AddFile("ok.txt", "ok");
        tb.AddEnd();
        std::string tar = tb.take() + std::string(600, '\x7f');
        const fs::path p = dir / "tail-garbage.tar.gz";
        WriteBytes(p, GzipCompress(tar));
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE_FALSE(res.has_value());
        CHECK_MESSAGE(res.error().find("非零数据") != std::string::npos, res.error());
    }
    // 目录头带 512 字节数据:目录建出,数据照偏移账跳过,里面是空的。
    {
        TarBuilder tb;
        tb.AddDirWithPayload("weird-dir/", std::string(512, 'j'));
        tb.AddFile("after.txt", "after");
        tb.AddEnd();
        const fs::path p = dir / "dir-payload.tar.gz";
        WriteBytes(p, GzipCompress(tb.take()));
        const auto res = UnpackArchive(p, dest, std::nullopt);
        REQUIRE(res.has_value());
        CHECK(fs::is_directory(dest / "weird-dir"));
        CHECK(fs::is_empty(dest / "weird-dir"));
        CHECK(ReadBytes(dest / "after.txt") == "after");
    }
}

TEST_CASE("UnpackArchive(tar.gz): 炸弹帽(小 declared_size 注入)拒") {
    const fs::path dir = MakeCaseDir("targz-bomb");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    TarBuilder tb;
    tb.AddFile("bomb.bin", MixedPayload(200000));
    tb.AddEnd();
    const fs::path tgz = dir / "bomb.tar.gz";
    WriteBytes(tgz, GzipCompress(tb.take()));
    const auto res = UnpackArchive(tgz, dest, std::uint64_t{1000});
    REQUIRE_FALSE(res.has_value());
    CHECK_MESSAGE(res.error().find("总量超过上限") != std::string::npos, res.error());
}

// ---------------------------------------------------------------------------
// 入口分派与合同
// ---------------------------------------------------------------------------

TEST_CASE("UnpackArchive: 不是 zip 也不是 tar.gz 拒;dest_dir 缺席拒") {
    const fs::path dir = MakeCaseDir("dispatch");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    const fs::path junk = dir / "junk.bin";
    WriteBytes(junk, "hello world, definitely not an archive");
    const auto res = UnpackArchive(junk, dest, std::nullopt);
    REQUIRE_FALSE(res.has_value());
    CHECK_MESSAGE(res.error().find("不是 zip 也不是 tar.gz") != std::string::npos,
                  res.error());

    // 目标目录不存在:引擎层造 staging,这里只报不代建。
    TarBuilder tb;
    tb.AddFile("x.txt", "x");
    tb.AddEnd();
    const fs::path tgz = dir / "pkg.tar.gz";
    WriteBytes(tgz, GzipCompress(tb.take()));
    const auto res2 = UnpackArchive(tgz, dir / "missing-dest", std::nullopt);
    REQUIRE_FALSE(res2.has_value());
    CHECK_MESSAGE(res2.error().find("目标目录不存在") != std::string::npos,
                  res2.error());
}

TEST_CASE("UnpackArchive(tar.gz): 空包(只有结尾零块)总量为零") {
    const fs::path dir = MakeCaseDir("targz-empty");
    const fs::path dest = dir / "out";
    fs::create_directories(dest);

    TarBuilder tb;
    tb.AddEnd();
    const fs::path tgz = dir / "empty.tar.gz";
    WriteBytes(tgz, GzipCompress(tb.take()));
    const auto res = UnpackArchive(tgz, dest, std::nullopt);
    REQUIRE(res.has_value());
    CHECK(*res == 0);
    CHECK(fs::is_empty(dest));
}
