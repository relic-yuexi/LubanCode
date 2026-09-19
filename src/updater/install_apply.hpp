// 更新助手 C++ 化·批三第②单:白名单 apply 落地层——按决策表动盘、
// 备份先行的耐用拷贝与安装账回填。
//
// 行为权威 install.ps1 终稿(PR #140)的 Invoke-ResourceApply /
// Write-InstallRecords,同源 scripts/install_plan.py 的 apply_plan /
// write_records,白名单语境(批一②六动作)的投影:
//   - Replace:先备份(备份件 = backup_root/<清单相对路径>,目录按需递归
//     建)再换新;盘上没有同名普通文件就没有备份可做(PS:Test-Path
//     -PathType Leaf 且非 reparse 才备);
//   - InstallNew:直落;目录按需递归建;
//   - SkipCurrent / KeepUserData:零动作;
//   - ConflictKind / ConflictReparse:零动作,点名进报告(PS 计入退出码
//     3 的 conflicts);
//   - 落地写是"拷贝 + fsync"(文件数据落盘;目录条目落盘交给调用方的
//     事务账,事务账走 layout/atomic_write 的原子写路)。同源同目标不搬
//     自己(PS/python 的 samefile 守卫);
//   - 路径合法性:计划条目路径过 ValidRelpath(批一①),不合法/越界/
//     父路径被文件或 reparse 占住 -> 整单报错不落地(PS Get-DestPathFor
//     的 throw -> Invoke-Install 退 1);
//   - 目标位被目录/reparse 占住(盘面比计划时又变了)-> 点名 conflict
//     不硬写(与计划层 ConflictKind/ConflictReparse 同归)。
// 口径差(如实声明):
//   - PS 的 retire 分支与空父目录收尾在白名单语境构造性不可达(无
//     retire),本层不设;
//   - install-state 走 layout 层写路(WriteInstallState,schema 2),
//     manifest_provenance 由该路恒落 "official-package"——C++ 更新器只
//     装官方包(manifest 原件照抄恰是这个口径);install.ps1 的
//     generated-from-source 分支是本地开发现场建清单的口子,不走这里;
//   - 退出码语义留给引擎:本层只回 expected,错误文案即 PS try/catch 里
//     那句人话。
#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "updater/layout.hpp"
#include "updater/whitelist_plan.hpp"

namespace lubancode::updater {

// 落地时点名的冲突一条(PS conflicts 条目同形状:path/kind/backup)。
// kind 是动作名串("conflict-kind"/"conflict-reparse"),backup 是备份根
// 相对安装根的显示路径(给账和日志看)。
struct ApplyConflict {
    std::string path;
    std::string kind;
    std::string backup;
};

// apply 摘要:动了什么、备了什么、点名了什么。备份根开单即建(PS
// New-BackupRoot 先建后用),零备份的空单也留个空根——PS 同款。
struct ApplyReport {
    std::filesystem::path backup_root;
    std::vector<std::string> replaced;    // 换新落地的清单相对路径
    std::vector<std::string> installed;   // 新增落地的清单相对路径
    std::vector<std::string> backed_up;   // 备份过的盘面件相对路径
    std::vector<ApplyConflict> conflicts; // 点名冲突(计划内 + 落地期发现)
};

// 按白名单计划落地:
//   plan        BuildWhitelistPlan 的产出(六动作);
//   disk        扫描层盘面账(键 = FoldKey;接线侧对账用——备份与占位的
//               判定以落地时实况为准,PS/python apply 同口径);
//   install_root 安装根;
//   pkg_dir     新包根(manifest 相对路径的取件侧);
//   backup_root 备份根(调用方给的 backups/<txn> 之类;开单即建)。
// 硬错(拷贝/建目录失败、路径不合法、父路径被占)整单 unexpected 返回,
// 已落的部分不回滚——回滚是引擎的事(备份都在)。冲突不硬错,进报告。
std::expected<ApplyReport, std::string> ApplyWhitelistPlan(
    const std::vector<PlanEntry>& plan,
    const std::map<std::string, DiskEntry>& disk,
    const std::filesystem::path& install_root,
    const std::filesystem::path& pkg_dir,
    const std::filesystem::path& backup_root);

// 拷贝 + 数据落盘(分块流拷,末尾 fflush + fsync/_commit):apply 与平铺
// 交接共用的耐用拷贝原语。父目录不代建(调用方 create_directories),
// 目标存在则整份覆写,目标不存在则新建;同源同目标由调用方先挡(不搬
// 自己)。src 不是常规文件/打不开/读失败/写失败 -> unexpected 带人话。
std::expected<void, std::string> CopyFileDurable(const std::filesystem::path& src,
                                                 const std::filesystem::path& dst);

// apply 完回填安装账(PS Write-InstallRecords 的白名单语境投影):
//   - 包内 manifest.json 原件逐字节照抄进安装根 manifest.json(同文件
//     不搬自己);包里没有 -> unexpected(C++ 更新器流程包必带官方清单,
//     generated-from-source 是 install.ps1 本地开发口子,不走这里);
//   - state.manifest 填清单 json 原样;state.version/platform/channel
//     留空时照清单的同名字段补(caller 给了就尊重 caller);
//   - install-state.json 走 layout 层写路(WriteInstallState,schema 2,
//     原子写),installed_at_utc 取 now seam,manifest_provenance 由该路
//     恒落 "official-package"(见文件头口径差)。
std::expected<void, std::string> BackfillInstallState(const LayoutPaths& paths,
                                                      const std::filesystem::path& pkg_dir,
                                                      InstallState state,
                                                      const UtcNowFn& now = {});

}  // namespace lubancode::updater
