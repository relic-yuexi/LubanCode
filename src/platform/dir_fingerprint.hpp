// 目录内容指纹的共用底座(统一 Package 封装单阶段 4 抽出):Plugin 的
// project-trust 指纹(0.26.73 起冻在 plugin-trust.json)与 Package 的整包
// 盘点指纹(阶段 1 冻,Package 信任锚的就是它)先前各写一套,如今同一把
// 尺——文件枚举稳定、逐文件内容全量进料、SHA-256 出 64 位十六进制:
//   - 枚举稳定:常规文件按 UTF-8 相对路径字节序排,文件系统的枚举次序
//     不配当身份、覆盖次序或哈希次序;
//   - 全量敏感:改一个字节、添删改名一个文件,指纹必变;
//   - 锚同一份 SHA-256 实现(hooks/hash 的自含实现,信任链只有这一个)。
// 两种材料的拼法各自冻结(见各函数注记),抽出不改行为——老账本上的
// 指纹一枚都不能失效。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace lubancode::platform {

// 常规文件的稳定枚举(相对路径 UTF-8,按渲染后的字节序排好)。分隔符是
// 平台原生的(Plugin v1 冻结的口径,Windows 上是 '\\');Package 侧要
// '/' 分隔的规范相对路径,自己走 inventory 的盘点,不吃这个枚举。follow_
// symlinks = true 按目标状态收(Plugin 旧账的规矩:指到常规文件的 symlink
// 跟着收);false 时 symlink/junction 一律不收。目录本身打不开返回错误人话。
std::expected<std::vector<std::string>, std::string> ListRegularFilesUtf8Sorted(
    const std::filesystem::path& dir, bool follow_symlinks);

// Plugin v1 指纹材料(0.26.73 冻结):逐文件 rel + '\0' + 原始字节 + '\0',
// 无头无尾拼接后过 SHA-256。任一文件读不动返回错误——信任账宁可批不了,
// 不可装作没看见。
std::expected<std::string, std::string> PluginDirFingerprintV1(const std::filesystem::path& dir);

// Package v1 指纹材料的文件账(统一封装单阶段 1 冻结):rel、大小、逐文件
// sha256。files 须已按 rel 字节序排好、只含读得动的文件(读不动的由调用
// 方记账,不进材料)。
struct LedgerFile {
    std::string rel_utf8;
    std::uintmax_t size = 0;
    std::string sha256;
};

// Package v1 指纹材料:头行 "luban-package-v1\n",逐文件
// rel '\t' size '\t' sha256 '\n',整体过 SHA-256。
std::string PackageLedgerFingerprintV1(const std::vector<LedgerFile>& files);

// 单文件内容的 SHA-256(64 位小写十六进制);读不动返回空串,由调用方
// 决定记账口径。
std::string FileSha256Hex(const std::filesystem::path& path);

}  // namespace lubancode::platform
