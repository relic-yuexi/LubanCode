// zip 侧实现 + UnpackArchive 总入口(魔数分派)。合同与口径差见
// archive.hpp 头注;成员名筛查合同见 paths.hpp(批一①)。
#include "updater/archive.hpp"

#include <miniz.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#include "updater/paths.hpp"

namespace lubancode::updater {
namespace {

namespace fs = std::filesystem;

// updater.py 护栏组(L88-94):总量帽 = 声明包大小 ×6,缺省基数取 4GiB
// 硬顶(MAX_ARCHIVE_BYTES)。python 旧缺省 64MiB,C++ 侧定案取硬顶,见
// archive.hpp 头注。
constexpr std::uint64_t kMaxArchiveBytes = std::uint64_t{4} << 30;
constexpr std::uint64_t kUnpackFactor = 6;

// POSIX mode 位(zip external_attr >> 16 即是;python 侧同款口径)。
constexpr std::uint32_t kModeTypeMask = 0xF000;  // S_IFMT
constexpr std::uint32_t kModeSymlink = 0xA000;   // S_IFLNK
constexpr std::uint32_t kModeDir = 0x4000;       // S_IFDIR
constexpr std::uint32_t kModeExecAny = 0111;     // 任一 x 位

// 包内相对路径(UTF-8 字节)转 fs::path:走 u8string 通道,Windows 上
// 不吃窄字节本地码页的暗亏(ASCII 域两路逐字节一致)。
fs::path FsPathFromUtf8(std::string_view utf8) {
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char c : utf8) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return fs::path(std::move(u8));
}

// mz_zip_reader 的 RAII 门:任何出口都得 end,不然内部堆挂漏。
struct ZipReaderGuard {
    mz_zip_archive* zip;
    ~ZipReaderGuard() { mz_zip_reader_end(zip); }
};

// 逐文件流式出口:miniz 把解压块递进来,ofstream 照单全收。回调合同是
// mz_file_write_func——返回写出的字节数,miniz 拿它对账;写崩返回 0
// 让 miniz 报 WRITE_CALLBACK_FAILED。
std::size_t WriteMemberChunk(void* opaque, mz_uint64 /*file_ofs*/,
                             const void* buf, std::size_t n) {
    auto* out = static_cast<std::ofstream*>(opaque);
    out->write(static_cast<const char*>(buf), static_cast<std::streamsize>(n));
    if (out->bad()) {
        return 0;
    }
    return n;
}

}  // namespace

std::expected<std::uint64_t, std::string> UnpackArchive(
    const fs::path& archive, const fs::path& dest_dir,
    std::optional<std::uint64_t> declared_size) {
    std::error_code ec;
    if (!fs::is_directory(dest_dir, ec)) {
        return std::unexpected("目标目录不存在: " + dest_dir.string());
    }

    // 魔数分派:gzip 头优先(zip 头永远起不了 1f 8b);PK 认 zip;其余拒。
    // 语义与 python 的 is_zipfile→tarfile 顺序在自家包域等价(见头注)。
    std::ifstream probe(archive, std::ios::binary);
    if (!probe) {
        return std::unexpected("包读不出: " + archive.string());
    }
    unsigned char magic[2] = {};
    probe.read(reinterpret_cast<char*>(magic), sizeof(magic));
    if (probe.gcount() < static_cast<std::streamsize>(sizeof(magic))) {
        return std::unexpected("不是 zip 也不是 tar.gz: " + archive.string());
    }
    probe.close();

    // 总量帽:declared×6,防溢出钳顶(声明值大到乘 6 会回绕时按最大值算,
    // 只会过严不会放水)。
    const std::uint64_t basis = declared_size.value_or(kMaxArchiveBytes);
    const std::uint64_t cap =
        basis > (std::numeric_limits<std::uint64_t>::max)() / kUnpackFactor
            ? (std::numeric_limits<std::uint64_t>::max)()
            : basis * kUnpackFactor;

    if (magic[0] == 0x1f && magic[1] == 0x8b) {
        return detail::UnpackTarGz(archive, dest_dir, cap);
    }
    if (magic[0] == 'P' && magic[1] == 'K') {
        return detail::UnpackZip(archive, dest_dir, cap);
    }
    return std::unexpected("不是 zip 也不是 tar.gz: " + archive.string());
}

namespace detail {

std::expected<std::uint64_t, std::string> UnpackZip(const fs::path& archive,
                                                    const fs::path& dest_dir,
                                                    std::uint64_t cap) {
    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
        const mz_zip_error err = mz_zip_get_last_error(&zip);
        return std::unexpected("zip 打不开或损坏: " + archive.string() + " (" +
                               mz_zip_get_error_string(err) + ")");
    }
    ZipReaderGuard guard{&zip};

    std::uint64_t total = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            return std::unexpected("zip 成员目录读不出: " + archive.string() +
                                   " (" +
                                   mz_zip_get_error_string(mz_zip_get_last_error(&zip)) +
                                   ")");
        }
        const std::string_view name(stat.m_filename);

        // 成员名筛查(updater.py screen_member_name 同一契约,批一①落地):
        // 不过即整包拒,先于一切类型判定——python 同序。
        const std::optional<std::string> rel = NormalizeMemberName(name);
        if (!rel) {
            return std::unexpected("归档成员路径不合法: " + std::string(name));
        }

        const std::uint32_t mode = (stat.m_external_attr >> 16) & 0xFFFF;
        if ((mode & kModeTypeMask) == kModeSymlink) {
            return std::unexpected("归档含符号链接: " + std::string(name));
        }
        if (stat.m_is_encrypted) {
            return std::unexpected("归档含加密成员: " + std::string(name));
        }
        if (!stat.m_is_supported) {
            return std::unexpected("归档成员压缩法不支持: " + std::string(name));
        }

        const fs::path dest = dest_dir / FsPathFromUtf8(*rel);

        // 目录成员:建目录走人,不计总量(python 同款)。
        if (stat.m_is_directory || (mode & kModeTypeMask) == kModeDir) {
            std::error_code dirc;
            fs::create_directories(dest, dirc);
            if (dirc) {
                return std::unexpected("目录建不出: " + dest.string() + " (" +
                                       dirc.message() + ")");
            }
            continue;
        }

        // 总量帽:先记账再落盘,超帽整包拒(炸弹在写盘前拦下)。账式用
        // 减法,declared 巨大也不给 uint64 回绕留门。
        if (stat.m_uncomp_size > cap || total > cap - stat.m_uncomp_size) {
            return std::unexpected("解压总量超过上限(" + std::to_string(cap) +
                                   " 字节)");
        }
        total += stat.m_uncomp_size;

        if (dest.has_parent_path()) {
            std::error_code dirc;
            fs::create_directories(dest.parent_path(), dirc);
            if (dirc) {
                return std::unexpected("目录建不出: " + dest.parent_path().string() +
                                       " (" + dirc.message() + ")");
            }
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected("成员写不出: " + dest.string());
        }
        // 逐文件流式口:坏 CRC/坏 deflate/截断都在 miniz 里现形,返回 0。
        if (!mz_zip_reader_extract_to_callback(&zip, i, &WriteMemberChunk, &out, 0)) {
            return std::unexpected("zip 成员解压失败: " + std::string(name) + " (" +
                                   mz_zip_get_error_string(mz_zip_get_last_error(&zip)) +
                                   ")");
        }
        out.close();
        if (out.bad()) {
            return std::unexpected("成员写不出: " + dest.string());
        }

        // POSIX 执行位(updater.py make_executable 同款:任一 x 位即全加)。
        // Windows 上 permissions 对执行位是稳态 no-op,不炸。
        if ((mode & kModeExecAny) != 0) {
            std::error_code permc;
            fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec |
                                         fs::perms::others_exec,
                            fs::perm_options::add, permc);
            if (permc) {
                return std::unexpected("执行位设不上: " + dest.string() + " (" +
                                       permc.message() + ")");
            }
        }
    }
    return total;
}

}  // namespace detail
}  // namespace lubancode::updater
