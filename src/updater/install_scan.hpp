// 更新助手 C++ 化·批三第②单:盘面扫描层——安装根实况入账、惰性 hash
// 与孤儿 reparse 阻断。
//
// 行为权威 install.ps1 终稿(PR #140,codex/legacy-install-migration)的
// Get-InstallDiskState / Get-DiskHash,语义同源 scripts/install_plan.py 的
// scan_disk / disk_hash:
//   - 扫描域:受管树全递归(不跟进链接/reparse——防目录链接把枚举带出
//     安装目录)+ 安装根顶层文件(排除记录件)。树内目录也入账(类型
//     错位要靠它点名),根级普通目录不入账(PS Get-ChildItem -File 与
//     python listdir+isfile 同口径——用户目录不在资产域);
//   - 键 = FoldKey(相对路径)(批一①),与 BuildWhitelistPlan 的盘面侧
//     同折叠口径;abspath/is_dir/reparse 齐,reparse 探测走
//     platform::IsReparsePoint(MSVC is_symlink 对 junction 报 false);
//   - sha256 惰性:扫描不算 hash(PS $Entry.sha256 起手 $null),要用时
//     HashDiskEntry 流式补;
//   - 孤儿 reparse 阻断:PS/python 决策表(Build-FilePlan L547-563 /
//     build_plan 的 is_dir-or-reparse 分支)对盘面 reparse 件"即便清单
//     不认识也点名留观(绝不去动)",计入退出码 3 的 conflicts。批一②
//     的 BuildWhitelistPlan 只对"挡住白名单路径"的 reparse 出
//     ConflictReparse(见 whitelist_plan.hpp 头注口径差第三条),清单外
//     的孤儿 reparse 归本层补:OrphanReparseConflicts 逐枚点名,引擎侧
//     与 PlanBlocks 合流成阻断。
// 口径差(如实声明,不算单边发明):
//   - 受管树根自身是 reparse(junction/symlink)时整棵跳过不扫——
//     install_plan.py scan_disk 的口径(os.path.islink(dest) 即 continue);
//     install.ps1 只查 Container 会跟进去,两边终稿本就分叉,取保守侧
//     (不跟进链接);
//   - 根级顶层扫描收"文件 + 任何 reparse 件"(install_plan.py 口径:
//     islink 先于 isfile 判);install.ps1 的 -File 滤掉目录形 reparse,
//     属 PS 侧疏漏,目录链接恰是最要留观的,不学;
//   - 哈希读不动(权限/占着/半路 IO 错)时 PS/python 把该盘面件改标
//     reparse 归 conflict——那一步在决策侧,本层只回 nullopt,接线侧
//     (批三③)照"读不动当非常规文件"的口径处置,见 HashDiskEntry 注。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "updater/manifest.hpp"
#include "updater/whitelist_plan.hpp"

namespace lubancode::updater {

// 扫安装根盘面实况,键 = FoldKey(相对路径):
//   install_root   安装根(不存在或不是目录 -> 空 map,照 PS 提前返回);
//   managed_trees  受管树名表(skills/docs/web/libexec/licenses/updater),
//                  逐棵 install_root/<tree> 全递归,树根本身是 reparse
//                  则整棵跳过(口径差见文件头);
//   root_files     根级顶层扫描的记录件排除表(install.ps1 $RecordFiles /
//                  install_plan.py RECORD_FILES 口径:manifest.json 与
//                  install-state.json——记账件不是资产)。除此之外根级
//                  顶层文件与 reparse 件全量入账,普通目录不入账。
// 只读不写,可重入;sha256 一律 nullopt(惰性,HashDiskEntry 补)。
std::map<std::string, DiskEntry> ScanInstallDisk(
    const std::filesystem::path& install_root,
    const std::vector<std::string>& managed_trees,
    const std::vector<std::string>& root_files);

// 流式算一枚盘面文件的 sha256(Sha256Stream 分块喂,不整读进内存)。
// 不是常规文件/打不开/读失败 -> nullopt。接线侧的补账口诀照 PS
// Get-DiskHash / python disk_hash:只在"非 reparse 非目录"的条目上算;
// 算不出(nullopt)就把该条目按非常规文件归 conflict-reparse 处理,
// 不当普通件换新。
std::optional<std::string> HashDiskEntry(const std::filesystem::path& abspath);

// 盘面条目是"链接形"(reparse 点或 symlink)吗——DiskEntry.reparse 的
// 探测口,apply/交接层的"不跟进不覆写"守卫也用它。Windows 上
// platform::IsReparsePoint(GetFileAttributesW)补 MSVC is_symlink 对
// junction 报 false 的漏;POSIX 上 is_symlink 是独眼(python scan_disk 的
// os.path.islink 口径),IsReparsePoint 恒 false。探测不了(路径不在)
// 按不是。
bool IsLinkLike(const std::filesystem::path& path);

// 孤儿 reparse 阻断:盘面上不在新包清单(new_map 键 = FoldKey)里的
// reparse 件,逐枚点名 ConflictReparse("盘面是链接/reparse,不写不删",
// install.ps1 Build-FilePlan 对 n=null 且 reparse 的留观分支)。用户数据
// 路径(config.toml/.env/.lubancode//.agents/,IsUserDataPath 口径)不点
// ——PS/python 决策表里用户数据闸在最前,误收的用户数据即便长得像链接
// 也只 keep-user-data。清单内的 reparse 由 BuildWhitelistPlan 出
// ConflictReparse,不在这里重复点名。点名的 path 用盘面原拼写(键在
// 折叠平台上是小写串,原拼写留给人看);取不到相对拼写时退回键。
// 空返回 = 无孤儿。零写盘。
std::vector<PlanEntry> OrphanReparseConflicts(
    const std::filesystem::path& install_root,
    const std::map<std::string, DiskEntry>& disk,
    const std::map<std::string, ManifestEntry>& new_map);

}  // namespace lubancode::updater
