// 更新助手 C++ 化·批二第③单:文件占用/重解析点/进程 EXE 归属的平台补件。
// 语义真源 scripts/updater.py:
//   - exe_in_use(L728-744):Windows 对运行中的 EXE 以独占写打开会吃
//     sharing violation——当占用;探测不了也当占用(保守)。POSIX 的
//     unlink/改名不影响运行进程,恒不占;
//   - version_in_use_posix(L747-766):Linux 的 /proc/<pid>/exe 指进版本
//     目录 = 运行中;别的平台认不出(macOS 无 /proc):恒 false;
//   - IsReparsePoint 是白名单决策表(whitelist_plan)的盘面实探补件:
//     MSVC STL 的 is_symlink 对 junction 报 false,专查必须走
//     GetFileAttributesW & FILE_ATTRIBUTE_REPARSE_POINT。
#pragma once

#include <filesystem>

namespace lubancode::platform {

// 文件被占用(不可替换)吗:
//   Windows —— CreateFileW(GENERIC_WRITE, 独占)探 sharing violation;
//             打不开的任何形态(含探不到、权限、不存在)一律当占用,
//             保守优先(python exe_in_use 口径:探测不了也当占用);
//   POSIX —— 恒 false(rename/unlink 不影响运行中的进程)。
bool IsFileLockedForWrite(const std::filesystem::path& path);

// 路径是重解析点(symlink/junction 等)吗:
//   Windows —— GetFileAttributesW & FILE_ATTRIBUTE_REPARSE_POINT 专查
//             (is_symlink 对 junction 报 false,不能只靠它);查不到属性
//             按不是(路径不在了,谈不上挡路);
//   POSIX —— 恒 false(无重解析点机制)。
bool IsReparsePoint(const std::filesystem::path& path);

// 进程 <pid> 的 EXE 是 <exe_path> 吗:
//   POSIX —— 读 /proc/<pid>/exe 比对(绝对路径规整后相等;" (deleted)"
//             后缀剥掉再比);/proc 不在或读不出 -> false(认不出)。
//   Windows —— 恒 false(占用判定走上面的独占写探测,EXE 归属归
//             sharing violation 一并覆盖)。
bool ProcessHoldsExe(unsigned long pid, const std::filesystem::path& exe_path);

}  // namespace lubancode::platform
