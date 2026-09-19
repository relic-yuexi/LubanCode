// tar.gz 侧实现:miniz tinfl 流式解 gzip(自写容器头/CRC32/ISIZE 校验)
// + 自写 tar 头遍历。合同与口径差见 archive.hpp 头注;成员名筛查合同见
// paths.hpp(批一①)。
//
// 流式纪律:原始 gz 字节按 64KB 块进,deflate 输出走 32KB 环形窗
// (TINFL_LZ_DICT_SIZE,与 miniz 自家 reader 同款姿势——环形窗不挂
// NON_WRAPPING 标志,字典回指才不断),解压字节再经一道 ≤32KB 的待取
// 队列喂给 tar 层。任何时刻内存占用都是有界小常数,4GiB 的包也一样。
#include "updater/archive.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "updater/paths.hpp"

namespace lubancode::updater {
namespace detail {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kTarBlockSize = 512;
constexpr std::size_t kRawBufSize = 64 * 1024;
// GNU 'L'/pax 'x'/'g' 元数据读完要进字符串,给个 1MiB 护栏(python 侧
// 不设限,这里收紧:发行包的元数据到不了这个量级,到了就是有人使坏)。
constexpr std::uint64_t kMaxMetaDataBytes = 1 << 20;

// POSIX mode 位(与 zip 侧同款口径;tar 侧只用执行位——链接/设备在
// typeflag 上就拦了,不靠 mode 域)。
constexpr std::uint32_t kModeExecAny = 0111;

// 包内相对路径(UTF-8 字节)转 fs::path,口径同 zip 侧。
fs::path FsPathFromUtf8(std::string_view utf8) {
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char c : utf8) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return fs::path(std::move(u8));
}

std::uint32_t ReadLe32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// ---------------------------------------------------------------------------
// 原始 gz 字节的有界缓冲读口:ReadN/Skip 跨块拼,EnsureAvail 做压实补读
// (残留字节挪到队头再续读,两字节窥视不至于骑在块缝上)。
// ---------------------------------------------------------------------------
class RawReader {
  public:
    explicit RawReader(const fs::path& path)
        : file_(path, std::ios::binary), path_(path) {
        if (file_) {
            std::error_code ec;
            const std::uint64_t size = fs::file_size(path, ec);
            remaining_ = ec ? 0 : size;
        }
    }

    bool Ok() const { return static_cast<bool>(file_); }
    const fs::path& path() const { return path_; }
    // 文件里还没读进缓冲的字节数(tinfl 的 HAS_MORE_INPUT 依据)。
    std::uint64_t unread_in_file() const { return remaining_; }

    // 保证缓冲里至少有 min(n, kRawBufSize) 字节;n ≤ kRawBufSize 时即 n。
    // 到不了返回 false(不挪动已保证的部分)。
    bool EnsureAvail(std::size_t n) {
        while (avail_ < n) {
            if (!Fill()) {
                return false;
            }
        }
        return avail_ >= n;
    }

    const unsigned char* data() const { return buf_.data() + pos_; }
    std::size_t avail() const { return avail_; }
    void Consume(std::size_t n) {
        pos_ += n;
        avail_ -= n;
    }

    // 精确读 n 字节(跨块拼);不足即 false。
    bool ReadN(unsigned char* dst, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            if (avail_ == 0 && !Fill()) {
                return false;
            }
            const std::size_t take = std::min(avail_, n - got);
            std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                      buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + take),
                      dst + got);
            Consume(take);
            got += take;
        }
        return true;
    }

    bool SkipN(std::uint64_t n) {
        while (n > 0) {
            if (avail_ == 0 && !Fill()) {
                return false;
            }
            const std::uint64_t take =
                std::min<std::uint64_t>(avail_, n);
            Consume(static_cast<std::size_t>(take));
            n -= take;
        }
        return true;
    }

  private:
    // 压实 + 续读一块。读到 EOF(0 字节)返回 false。
    bool Fill() {
        if (eof_hit_) {
            return false;
        }
        // 把残留挪到队头,续读补在后面。
        if (pos_ > 0) {
            const std::size_t keep = avail_;
            std::move(buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                      buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + keep),
                      buf_.begin());
            pos_ = 0;
        }
        file_.read(reinterpret_cast<char*>(buf_.data() + avail_),
                   static_cast<std::streamsize>(kRawBufSize - avail_));
        const std::size_t got = static_cast<std::size_t>(file_.gcount());
        if (got == 0) {
            eof_hit_ = true;
            return false;
        }
        avail_ += got;
        remaining_ -= std::min(remaining_, static_cast<std::uint64_t>(got));
        return true;
    }

    std::ifstream file_;
    fs::path path_;
    std::array<unsigned char, kRawBufSize> buf_{};
    std::size_t pos_ = 0;
    std::size_t avail_ = 0;
    std::uint64_t remaining_ = 0;
    bool eof_hit_ = false;
};

// ---------------------------------------------------------------------------
// 解压字节流:多成员 gzip 容器 + CRC32/ISIZE 校验 + 32KB 环形窗流式吹出。
// tar 层只认 ReadExact/Skip/ReadBlock/CleanEof 这几个口。
// ---------------------------------------------------------------------------
class TarGzSource {
  public:
    explicit TarGzSource(const fs::path& path) : raw_(path) {}

    bool Ok() const { return raw_.Ok(); }
    const fs::path& path() const { return raw_.path(); }
    const std::string& error() const { return error_; }

    // 精确读 n 字节;n == 0 直接过。不足 = 坏(错误串已记)。
    bool ReadExact(unsigned char* dst, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            if (!EnsurePending()) {
                return false;
            }
            const std::size_t take = std::min(pending_avail(), n - got);
            std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(head_),
                      pending_.begin() + static_cast<std::ptrdiff_t>(head_ + take),
                      dst + got);
            head_ += take;
            got += take;
        }
        return true;
    }

    // 跳过 n 字节解压字节(数据块对齐补白/元数据弃读)。
    bool Skip(std::uint64_t n) {
        while (n > 0) {
            if (!EnsurePending()) {
                return false;
            }
            const std::uint64_t take =
                std::min<std::uint64_t>(pending_avail(), n);
            head_ += static_cast<std::size_t>(take);
            n -= take;
        }
        return true;
    }

    // 整块读 512。三态:true = 到手;false + CleanEof() 真 = 干净流尾
    // (零字节剩余);false + error() 有串 = 坏/半块截断。先探口再整读,
    // 干净 EOF 不许落"截断"错误串,不然主循环分不了流。
    bool ReadBlock(unsigned char* block) {
        if (!EnsurePending()) {
            return false;
        }
        if (!ReadExact(block, kTarBlockSize)) {
            Fail("tar 流截断:块只到一半");
            return false;
        }
        return true;
    }

    // 干净流尾:无错、待取空、容器收尾完毕。
    bool CleanEof() const {
        return error_.empty() && pending_avail() == 0 && state_ == State::kDone;
    }

  private:
    enum class State { kNeedHeader, kInDeflate, kDone };

    std::size_t pending_avail() const { return pending_.size() - head_; }

    // 记错误(先到先得,后到不覆盖更上游的病因)。
    bool Fail(std::string msg) {
        if (error_.empty()) {
            error_ = std::move(msg);
        }
        return false;
    }

    // 保证待取队列有货;流干净到尾返回 false 且无错。状态机只有这一个
    // 入口转圈:成员收尾(DONE→下一头/结束)必须回这里重判状态,不许在
    // 吹气循环里直接续吹——续吹会把 gzip 头字节喂进 tinfl。
    bool EnsurePending() {
        for (;;) {
            if (pending_avail() > 0) {
                return true;
            }
            if (state_ == State::kDone) {
                return false;  // 干净流尽,无错
            }
            if (state_ == State::kNeedHeader) {
                if (!BeginMember()) {
                    return false;
                }
                continue;
            }
            if (!PumpDeflate()) {
                return false;
            }
        }
    }

    // 吹一轮 tinfl。出货即真(队列有货);DONE 交给 FinishMember 推状态
    // 后返 true 让外层重判;输入不够先返真,下轮入口检查报截断。
    bool PumpDeflate() {
        if (raw_.avail() == 0 && !raw_.EnsureAvail(1)) {
            return Fail("gzip 流截断:deflate 未收尾");
        }
        const std::size_t slot =
            static_cast<std::size_t>(member_out_ & (TINFL_LZ_DICT_SIZE - 1));
        std::size_t out_cap = TINFL_LZ_DICT_SIZE - slot;
        std::size_t in_sz = raw_.avail();
        const mz_uint32 flags =
            raw_.unread_in_file() > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        const tinfl_status st = tinfl_decompress(
            &infl_, raw_.data(), &in_sz, win_.data(), win_.data() + slot,
            &out_cap, flags);
        raw_.Consume(in_sz);

        if (out_cap > 0) {
            member_crc_ = static_cast<mz_uint32>(
                mz_crc32(member_crc_, win_.data() + slot, out_cap));
            member_out_ += out_cap;
            pending_.insert(pending_.end(), win_.data() + slot,
                            win_.data() + slot + out_cap);
            head_ = 0;
            return true;
        }
        switch (st) {
            case TINFL_STATUS_DONE:
                return FinishMember();  // true = 状态已推进,外层重判
            case TINFL_STATUS_NEEDS_MORE_INPUT:
            case TINFL_STATUS_HAS_MORE_OUTPUT:
                // 未出货但没坏:外层重入。真截断在下轮 PumpDeflate 入口
                // 报(缓冲与文件双空才报,缓冲有剩照吹)。
                return true;
            default:
                return Fail("gzip 的 deflate 流损坏");
        }
    }

    // 解析一个 gzip 成员头(魔数/CM/FLG 及四样可选项),重置成员账。
    bool BeginMember() {
        unsigned char head[10];
        if (!raw_.ReadN(head, sizeof(head))) {
            return Fail("gzip 头截断或读不出: " + raw_.path().string());
        }
        if (head[0] != 0x1f || head[1] != 0x8b) {
            return Fail("gzip 成员魔数不对");
        }
        if (head[2] != 8) {
            return Fail("gzip 压缩法不是 deflate");
        }
        const unsigned char flg = head[3];
        if ((flg & 0xE0) != 0) {
            return Fail("gzip 头保留位非零");
        }
        if ((flg & 0x04) != 0) {  // FEXTRA
            unsigned char xlen[2];
            if (!raw_.ReadN(xlen, sizeof(xlen))) {
                return Fail("gzip 头截断:FEXTRA 长度读不出");
            }
            const std::uint64_t extra =
                static_cast<std::uint64_t>(xlen[0]) |
                (static_cast<std::uint64_t>(xlen[1]) << 8);
            if (!raw_.SkipN(extra)) {
                return Fail("gzip 头截断:FEXTRA 域读不出");
            }
        }
        // FNAME/FCOMMENT:NUL 结尾字符串,跳过。
        for (const unsigned char bit : {static_cast<unsigned char>(0x08),
                                        static_cast<unsigned char>(0x10)}) {
            if ((flg & bit) == 0) {
                continue;
            }
            for (;;) {
                unsigned char c = 0;
                if (!raw_.ReadN(&c, 1)) {
                    return Fail("gzip 头截断:名字/注记域读不出");
                }
                if (c == 0) {
                    break;
                }
            }
        }
        if ((flg & 0x02) != 0 && !raw_.SkipN(2)) {  // FHCRC
            return Fail("gzip 头截断:HCRC 读不出");
        }

        tinfl_init(&infl_);
        member_crc_ = 0;
        member_out_ = 0;
        state_ = State::kInDeflate;
        return true;
    }

    // 收一个成员:CRC32/ISIZE 对账,多成员接力或干净收尾。
    bool FinishMember() {
        unsigned char trailer[8];
        if (!raw_.ReadN(trailer, sizeof(trailer))) {
            return Fail("gzip 流截断:缺 CRC/长度尾");
        }
        const std::uint32_t stored_crc = ReadLe32(trailer);
        const std::uint32_t stored_isize = ReadLe32(trailer + 4);
        if (stored_crc != member_crc_) {
            return Fail("gzip CRC 校验失败");
        }
        if (stored_isize != static_cast<std::uint32_t>(member_out_)) {
            return Fail("gzip ISIZE 长度不符");
        }
        if (!raw_.EnsureAvail(1)) {
            state_ = State::kDone;  // 干净到文件尾
            return true;
        }
        // 还有字节:要么是下一个 gzip 成员,要么是来路不明的尾巴。
        if (!raw_.EnsureAvail(2)) {
            return Fail("gzip 尾部拖了不成器的字节");
        }
        const unsigned char* peek = raw_.data();
        if (peek[0] == 0x1f && peek[1] == 0x8b) {
            state_ = State::kNeedHeader;
            return true;
        }
        return Fail("gzip 尾部拖了非 gzip 字节");
    }

    RawReader raw_;
    tinfl_decompressor infl_ = {};
    std::array<unsigned char, TINFL_LZ_DICT_SIZE> win_{};
    std::vector<unsigned char> pending_;
    std::size_t head_ = 0;
    std::uint32_t member_crc_ = 0;
    std::uint64_t member_out_ = 0;
    State state_ = State::kNeedHeader;
    std::string error_;
};

// ---------------------------------------------------------------------------
// tar 头字段解析
// ---------------------------------------------------------------------------

// NUL 结尾域取串(不copy,指进块内)。
std::string_view FieldStr(const unsigned char* p, std::size_t n) {
    std::size_t len = 0;
    while (len < n && p[len] != 0) {
        ++len;
    }
    return std::string_view(reinterpret_cast<const char*>(p), len);
}

// 八进制或 GNU base-256 数字域。八进制容前导空格/NUL、遇空格/NUL 收尾;
// base-256(首字节 0x80 置位)按大端拼,超出 uint64 装得下的位数拒。
// 解析不出格式返回 nullopt。
std::optional<std::uint64_t> ParseNumeric(const unsigned char* p, std::size_t n) {
    if (n == 0) {
        return std::nullopt;
    }
    if ((p[0] & 0x80) != 0) {
        // base-256:首字节的指示位之外全是载荷,n 字节共 8n-1 位。装进
        // uint64 要求超出 64 位的头部全零,即前 n-8 个字节(首字节掩掉
        // 指示位)必须全零;超了拒(python nti 撞 OverflowError,同向)。
        if (n >= 8) {
            for (std::size_t i = 0; i + 8 < n; ++i) {
                const unsigned char b = (i == 0) ? (p[0] & 0x7F) : p[i];
                if (b != 0) {
                    return std::nullopt;
                }
            }
            std::uint64_t v = 0;
            for (std::size_t i = n - 8; i < n; ++i) {
                v = (v << 8) | p[i];
            }
            return v;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v = (v << 8) | (i == 0 ? (p[0] & 0x7F) : p[i]);
        }
        return v;
    }
    std::uint64_t v = 0;
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = p[i];
        if (c == ' ' || c == 0) {
            if (any) {
                break;
            }
            continue;
        }
        if (c < '0' || c > '7') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (v > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 8) {
            return std::nullopt;
        }
        v = v * 8 + digit;
        any = true;
    }
    return v;
}

struct TarHeaderFields {
    std::string_view name;
    std::string_view prefix;
    std::uint64_t size = 0;
    std::uint32_t mode = 0;
    char typeflag = 0;
    bool use_prefix = false;  // POSIX ustar(magic "ustar\0" + version "00")
};

// 校验和 + 魔数 + 各数字域一次过。不合即 nullopt(err 记因)。
std::optional<TarHeaderFields> ParseTarHeader(const unsigned char* block,
                                              std::string& err) {
    // 校验和:chksum 域(148..155)按空格算的无符号和;老 tar 写有符号和,
    // 两种都认(python tarfile 同款双认)。
    std::uint64_t sum_unsigned = 0;
    std::int64_t sum_signed = 0;
    for (std::size_t i = 0; i < kTarBlockSize; ++i) {
        const unsigned char c =
            (i >= 148 && i < 156) ? static_cast<unsigned char>(' ') : block[i];
        sum_unsigned += c;
        sum_signed += static_cast<signed char>(c);
    }
    const auto stored = ParseNumeric(block + 148, 8);
    if (!stored ||
        (*stored != sum_unsigned &&
         static_cast<std::int64_t>(*stored) != sum_signed)) {
        err = "tar 头校验和不对";
        return std::nullopt;
    }
    // 魔数:POSIX("ustar\0"+"00")与 GNU("ustar "+" \0")都认,prefix 只认
    // POSIX 的(version 域 "00");v7 裸 tar 不认(自家包域用不到)。
    if (std::string_view(reinterpret_cast<const char*>(block + 257), 5) != "ustar") {
        err = "tar 头魔数不对";
        return std::nullopt;
    }

    TarHeaderFields f;
    f.name = FieldStr(block, 100);
    f.prefix = FieldStr(block + 345, 155);
    f.typeflag = static_cast<char>(block[156]);
    f.use_prefix = block[262] == 0 && block[263] == '0';
    const auto mode = ParseNumeric(block + 100, 8);
    const auto size = ParseNumeric(block + 124, 12);
    if (!mode || !size) {
        err = "tar 头 mode/size 域解析不出";
        return std::nullopt;
    }
    f.mode = static_cast<std::uint32_t>(*mode);
    f.size = *size;
    return f;
}

// pax 记录串:"<长度> <键>=<值>\n" 连排。解析不出返回 nullopt。
std::optional<std::unordered_map<std::string, std::string>> ParsePaxRecords(
    const std::string& data) {
    std::unordered_map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t sp = data.find(' ', pos);
        if (sp == std::string::npos) {
            return std::nullopt;
        }
        std::uint64_t len = 0;
        for (std::size_t i = pos; i < sp; ++i) {
            const char c = data[i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            len = len * 10 + static_cast<std::uint64_t>(c - '0');
            // 元数据本体已被 1MiB 护栏框住,超界长度必是使坏,先拒后用
            // (防十进制位堆出 uint64 回绕)。
            if (len > data.size()) {
                return std::nullopt;
            }
        }
        // 长度含 "数字+空格" 前缀与结尾换行,且不许越过数据界。
        if (len <= sp - pos + 1 || pos + len > data.size() ||
            data[pos + len - 1] != '\n') {
            return std::nullopt;
        }
        const std::string record =
            data.substr(sp + 1, len - (sp - pos + 1) - 1);
        const std::size_t eq = record.find('=');
        if (eq == std::string::npos || eq == 0) {
            return std::nullopt;
        }
        out[record.substr(0, eq)] = record.substr(eq + 1);
        pos += static_cast<std::size_t>(len);
    }
    return out;
}

// 元数据('L'/'K'/'x'/'g' 的数据域)读全:1MiB 护栏 + 对齐补白照跳。
std::optional<std::string> ReadMetaData(TarGzSource& src, std::uint64_t size,
                                        std::string& err) {
    if (size > kMaxMetaDataBytes) {
        err = "归档元数据超限(" + std::to_string(size) + " 字节)";
        return std::nullopt;
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!src.ReadExact(reinterpret_cast<unsigned char*>(data.data()), data.size())) {
        err = src.error().empty() ? "tar 流截断:元数据读一半" : src.error();
        return std::nullopt;
    }
    const std::uint64_t pad = (kTarBlockSize - size % kTarBlockSize) % kTarBlockSize;
    if (!src.Skip(pad)) {
        err = src.error().empty() ? "tar 流截断:元数据补白读不出" : src.error();
        return std::nullopt;
    }
    return data;
}

std::string TakeNulTerminated(std::string&& data) {
    const std::size_t nul = data.find('\0');
    if (nul != std::string::npos) {
        data.resize(nul);
    }
    return std::move(data);
}

}  // namespace

std::expected<std::uint64_t, std::string> UnpackTarGz(const fs::path& archive,
                                                      const fs::path& dest_dir,
                                                      std::uint64_t cap) {
    TarGzSource src(archive);
    if (!src.Ok()) {
        return std::unexpected("包读不出: " + archive.string());
    }

    std::array<unsigned char, kTarBlockSize> block{};
    std::uint64_t total = 0;
    bool seen_end_marker = false;
    // GNU 'L' 长名/pax 覆写:'L'/'x' 只作用下一枚,'g' 全局垫底。
    std::optional<std::string> gnu_long_name;
    std::unordered_map<std::string, std::string> pax_global;
    std::unordered_map<std::string, std::string> pax_local;

    for (;;) {
        if (!src.ReadBlock(block.data())) {
            if (src.CleanEof()) {
                if (!seen_end_marker) {
                    return std::unexpected("tar 流截断:未见结尾零块");
                }
                break;
            }
            return std::unexpected(
                src.error().empty() ? "tar 流读不出: " + archive.string()
                                    : src.error());
        }

        if (std::all_of(block.begin(), block.end(),
                        [](unsigned char c) { return c == 0; })) {
            // 结尾零块:后续零块照吞,零块后还拖非零尾巴即拒(python 会
            // 静默忽略,这里收紧,见头注)。
            seen_end_marker = true;
            for (;;) {
                if (!src.ReadBlock(block.data())) {
                    if (src.CleanEof()) {
                        break;
                    }
                    return std::unexpected(src.error());
                }
                if (!std::all_of(block.begin(), block.end(),
                                 [](unsigned char c) { return c == 0; })) {
                    return std::unexpected("tar 尾部拖了非零数据");
                }
            }
            break;
        }

        std::string err;
        std::optional<TarHeaderFields> fields = ParseTarHeader(block.data(), err);
        if (!fields) {
            return std::unexpected(err + ": " + archive.string());
        }

        // 元数据头:python 的 tarfile 迭代器吃掉不吐,这里同款——不进
        // 成员筛查,不落盘,只记账。
        if (fields->typeflag == 'L' || fields->typeflag == 'K' ||
            fields->typeflag == 'x' || fields->typeflag == 'g') {
            auto data = ReadMetaData(src, fields->size, err);
            if (!data) {
                return std::unexpected(err);
            }
            switch (fields->typeflag) {
                case 'L':
                    gnu_long_name = TakeNulTerminated(std::move(*data));
                    break;
                case 'K':
                    break;  // 长链接名:链接成员到不了落盘那步,弃读即弃用
                case 'x': {
                    auto records = ParsePaxRecords(*data);
                    if (!records) {
                        return std::unexpected("pax 扩展头解析不出");
                    }
                    pax_local = std::move(*records);
                    break;
                }
                case 'g': {
                    auto records = ParsePaxRecords(*data);
                    if (!records) {
                        return std::unexpected("pax 全局头解析不出");
                    }
                    pax_global = std::move(*records);
                    break;
                }
                default:
                    break;
            }
            continue;
        }

        // 成员名拼装:ustar prefix → GNU 'L' 长名 → pax path= 覆写
        // (覆写完照样要过筛查,邪路名字翻不进盘)。
        std::string name(fields->name);
        if (fields->use_prefix && !fields->prefix.empty()) {
            name = std::string(fields->prefix) + "/" + name;
        }
        if (gnu_long_name) {
            name = std::move(*gnu_long_name);
            gnu_long_name.reset();
        }
        std::unordered_map<std::string, std::string> merged = pax_global;
        for (auto& [k, v] : pax_local) {
            merged[k] = std::move(v);
        }
        pax_local.clear();
        if (const auto it = merged.find("path"); it != merged.end()) {
            name = it->second;
        }
        if (const auto it = merged.find("size"); it != merged.end()) {
            std::uint64_t pax_size = 0;
            bool ok = !it->second.empty();
            for (const char c : it->second) {
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                pax_size = pax_size * 10 + static_cast<std::uint64_t>(c - '0');
            }
            if (!ok) {
                return std::unexpected("pax size= 记录不是十进制数: " + it->second);
            }
            fields->size = pax_size;  // pax 大文件:尺寸以覆写为准
        }

        // 成员名筛查先于一切类型判定(python 同序)。
        const std::optional<std::string> rel = NormalizeMemberName(name);
        if (!rel) {
            return std::unexpected("归档成员路径不合法: " + name);
        }

        if (fields->typeflag == '2' || fields->typeflag == '1') {
            return std::unexpected("归档含链接: " + name);
        }
        if (fields->typeflag == '3' || fields->typeflag == '4' ||
            fields->typeflag == '6') {
            return std::unexpected("归档含设备/管道: " + name);
        }
        if (fields->typeflag != '0' && fields->typeflag != '\0' &&
            fields->typeflag != '5') {
            return std::unexpected("归档成员不是普通文件: " + name);
        }

        const fs::path dest = dest_dir / FsPathFromUtf8(*rel);

        if (fields->typeflag == '5') {
            // 目录成员:建目录走人(尺寸大于 0 的病态目录头照 tarfile 的
            // 偏移账跳过数据块),不计总量。
            const std::uint64_t pad =
                (kTarBlockSize - fields->size % kTarBlockSize) % kTarBlockSize;
            if (!src.Skip(fields->size) || !src.Skip(pad)) {
                return std::unexpected(src.error().empty()
                                           ? "tar 流截断:目录成员读不出"
                                           : src.error());
            }
            std::error_code dirc;
            fs::create_directories(dest, dirc);
            if (dirc) {
                return std::unexpected("目录建不出: " + dest.string() + " (" +
                                       dirc.message() + ")");
            }
            continue;
        }

        // 普通文件:先记账再落盘,超帽整包拒(炸弹在写盘前拦下)。账式
        // 用减法,_declared 巨大也不给 uint64 回绕留门。
        if (fields->size > cap || total > cap - fields->size) {
            return std::unexpected("解压总量超过上限(" + std::to_string(cap) +
                                   " 字节)");
        }
        total += fields->size;

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
        std::array<char, 64 * 1024> io_buf{};
        std::uint64_t remain = fields->size;
        bool stream_ok = true;
        while (remain > 0) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(io_buf.size(), remain));
            if (!src.ReadExact(reinterpret_cast<unsigned char*>(io_buf.data()),
                               chunk)) {
                stream_ok = false;
                break;
            }
            out.write(io_buf.data(), static_cast<std::streamsize>(chunk));
            if (out.bad()) {
                stream_ok = false;
                break;
            }
            remain -= chunk;
        }
        if (!stream_ok) {
            return std::unexpected(
                src.error().empty() ? "成员写不出: " + dest.string() : src.error());
        }
        out.close();
        if (out.bad()) {
            return std::unexpected("成员写不出: " + dest.string());
        }
        const std::uint64_t pad =
            (kTarBlockSize - fields->size % kTarBlockSize) % kTarBlockSize;
        if (!src.Skip(pad)) {
            return std::unexpected(src.error().empty()
                                       ? "tar 流截断:数据补白读不出"
                                       : src.error());
        }

        // POSIX 执行位(同 zip 侧:任一 x 位即全加;Windows 上 no-op)。
        if ((fields->mode & kModeExecAny) != 0) {
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
