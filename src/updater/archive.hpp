// 更新助手 C++ 化·批二第①单:zip / tar.gz 双格式安全解包器。
//
// 语义唯一真源:scripts/updater.py 的 unpack_archive(约 L514-571)——
// 成员名逐一过筛查、拒链接/设备/管道成员、解压总量帽、POSIX 执行位,
// 逐条对齐,别单边发明。口径差(如实声明,严方向不放松):
//   - 格式分派按魔数:0x1f 8b 走 tar.gz,PK 头走 zip,其余拒。python 先
//     is_zipfile 再 tarfile,自家包两头不混,分派等价;
//   - 总量帽 = declared_size×6(updater.py MAX_UNPACK_FACTOR);declared_size
//     缺省按 4GiB 硬顶(updater.py MAX_ARCHIVE_BYTES)当基数。python 侧旧
//     缺省 64MiB,C++ 侧按批二派工单定案取硬顶——引擎层给不出声明大小时
//     只剩这道闸,取宽不取严会开洞;
//   - tar 收尾比 python 严:未见结尾零块就干净到 EOF 的,按截断拒;零块
//     之后还拖非零尾巴的也拒。真 tar 必有结尾零块,python 的宽容只在
//     容忍残缺包,收紧不算发明;
//   - tar typeflag 只放行普通文件('0'/'\0')与目录('5')。python 把
//     contiguous '7' 与 GNU sparse 'S' 也当普通文件抽,这里拒——发行包
//     不该有这两种成员,混进来就该停下问人;
//   - 加密 zip 成员 python 侧炸裸异常,这里改整包拒,人话报错。
//
// 流式纪律(铁律):zip 走 mz_zip_reader 逐文件回调口,gzip/tar 逐块过
// tinfl,任何成员都不整读进内存——4GiB 硬顶的包也得在 64KB 量级的缓冲
// 里拆完。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace lubancode::updater {

// 解包发行包到 dest_dir。任何成员名不过 NormalizeMemberName、链接/设备/
// 管道成员、解压总量超 declared_size×6(缺省按 4GiB 硬顶算)→ 整包拒。
// 成功返回解压后总字节数(目录成员不计)。dest_dir 须已存在(staging
// 目录由引擎层造,这里不代建)。失败时错误串是人话中文,直接可进日志。
std::expected<std::uint64_t, std::string> UnpackArchive(
    const std::filesystem::path& archive, const std::filesystem::path& dest_dir,
    std::optional<std::uint64_t> declared_size);

namespace detail {

// 内部缝,测试册与后续批次不许碰:UnpackArchive 算好总量帽后按魔数把
// 活分派到这两头。archive_zip.cpp 认 zip,archive_tar.cpp 认 tar.gz。
// 前置同 UnpackArchive(dest_dir 已存在,cap 已含 ×6)。
std::expected<std::uint64_t, std::string> UnpackZip(
    const std::filesystem::path& archive, const std::filesystem::path& dest_dir,
    std::uint64_t cap);
std::expected<std::uint64_t, std::string> UnpackTarGz(
    const std::filesystem::path& archive, const std::filesystem::path& dest_dir,
    std::uint64_t cap);

}  // namespace detail
}  // namespace lubancode::updater
