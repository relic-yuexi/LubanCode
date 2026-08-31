// 文件权限与路径安全(P0 新轨迹记录单 §12.1,P0-4)。
//
// 三件事:
//   IsSafeSingleSegment       用户递进来的名字(session ref/record id/export
//                             名)先过单段名校验——拒绝 ".."、绝对路径逃逸、
//                             分隔符、盘符、通配符与 Windows 设备名。
//   IsContainedCanonicalPath  canonical-path containment:child 解析后必须仍
//                             在 root 之下;配合 ContainsSymlinkOrReparse
//                             拒绝重解析点/symlink 越界(§12.1)。
//   HardenUserOnly            目录 0700/文件 0600(POSIX),Windows 建
//                             user-only DACL(只许当前用户,PROTECTED 不并
//                             入继承 ACE)。设不住返回 false,调用方须告警。
//
// 依赖铁律:trajectory 纯库,只认标准库 + 平台头,不 include app/cli/runtime。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace lubancode::trajectory {

// 用户 supplied 名字的单段校验。合法字符 [A-Za-z0-9._-],不许打头的点、
// 不许路径分隔符与盘符冒号、不许全点、长度 1..128,另拒 Windows 保留
// 设备名(con/prn/aux/nul/com1-9/lpt1-9,大小写不敏感)。
bool IsSafeSingleSegment(std::string_view name);

// child 解析(weakly_canonical)后是否仍在 root 之下。两者任一解析失败
// 给 false(宁拒勿放)。只比路径分量,不区分大小写的平台已由 canonical
// 输出统一。
bool IsContainedCanonicalPath(const std::filesystem::path& child, const std::filesystem::path& root);

// root 到 child 之间是否夹着重解析点/symlink(含 child 自身)。不存在的
// 前缀跳过(还没建出来的路径谈不上逃逸);查询失败保守给 true(当有
// 逃逸嫌疑处理)。
bool ContainsSymlinkOrReparse(const std::filesystem::path& root, const std::filesystem::path& child);

// 目录收紧成 user-only:POSIX chmod 0700;Windows 置 PROTECTED user-only
// DACL(GA 只给当前用户,子项继承)。失败给 false——§12.1"权限设不住,
// 启动须告警"。
bool HardenDirectoryUserOnly(const std::filesystem::path& dir);

// 文件收紧成 user-only:POSIX chmod 0600;Windows 置 PROTECTED user-only
// DACL(不继承父项之外的 ACE)。
bool HardenFileUserOnly(const std::filesystem::path& file);

// 双重防线(§12.1"rename 前再核目标仍在 session 根内"):child 既要在
// root 的 canonical 包含域内,root 到 child 之间又不得夹重解析点。
bool IsSafeContainedPath(const std::filesystem::path& child, const std::filesystem::path& root);

}  // namespace lubancode::trajectory
