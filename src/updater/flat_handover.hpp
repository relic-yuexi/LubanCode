// 更新助手 C++ 化·批三第②单:平铺交接层——完整备份、旧启动器让位与
// 回滚。
//
// 语义真源 scripts/updater.py(§五.4 平铺迁移与 §六进程协调):
//   - full_backup(L386-402) + activate 平铺分支(L1253-1267):受管树 +
//     根顶层文件(排除记录件)完整备份,先落 staging 再改名进
//     backups/<txn>/flat(改名不成留原地,账上照记)——"宁可多备份";
//   - handover_flat_launcher(L896-921):旧根 EXE rename 进
//     backups/<txn>/legacy(Windows 对运行中映像允许 rename,运行中的
//     进程不受影响);挪不进(被锁)明报不强杀,停 needs-review 等用户
//     退出后重跑续上;新版 EXE 拷到根位当固定启动器(POSIX 补执行位);
//     版本目录里的 updater 树按文件覆盖同步到根(sync_updater_tree,
//     L880-895:不清不删——本更新器自己就住在那棵树里);
//   - restore_flat_legacy(L924-942):回滚路——旧 EXE 挪回根位(根位新
//     启动器停到 backups/<txn>/launcher-parked-<名>),current.json 摘掉
//     (平铺旧 EXE 不认指针,留着会把后续启动当启动器空转),恢复不成
//     也照摘。
// 口径差(如实声明):
//   - 树备份不复制 reparse 件也不跟进(python copytree(symlinks=True)
//     保链接;Windows 造符号链接要特权、junction 无标准拷贝口,链接件在
//     白名单口径下本就 conflict 留观,不搬无损);
//   - rename 失败的文案带系统错误串,引擎侧据此停 needs-review(退出码
//     语义归引擎)。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "updater/layout.hpp"

namespace lubancode::updater {

// 平铺迁移完整备份:受管树(全递归)+ 安装根顶层文件(排除记录件)原样
// 进备份。先落 backups/<txn>/.flat-staging-<pid> 再改名成 backups/<txn>/flat
// (python 两步走的同款:备份集名换入是原子的);改名不成留原地,返回值
// 就是真实落点,账上照记。没有任何可备的东西时返回 nullopt
// (python activate 的 any(isdir) 守卫)。
//   install_root  安装根;
//   managed_trees 受管树名表;
//   record_files  根级记录件排除表(manifest.json/install-state.json);
//   txn_id        事务 id(backups/<txn> 的 <txn>)。
std::expected<std::optional<std::filesystem::path>, std::string> FlatFullBackup(
    const std::filesystem::path& install_root,
    const std::vector<std::string>& managed_trees,
    const std::vector<std::string>& record_files,
    const std::string& txn_id);

// 平铺 -> 版本化交接:旧根 EXE rename 进 backups/<txn>/legacy(已停过一
// 次的先清位),挪不进明报(文案照 python:多半被运行中的进程/杀软锁着,
// 绝不强杀);新版 EXE(version_dir 里的根位同名件)拷到根位当固定启动
// 器,POSIX 补执行位;version_dir/updater 树按文件覆盖同步到根 updater
// (不清不删)。安装根与版本目录重叠时拒绝交接(python 同款守卫)。
std::expected<void, std::string> HandoverFlatLauncher(const LayoutPaths& paths,
                                                      const std::string& txn_id,
                                                      const std::filesystem::path& version_dir);

// 回滚:backups/<txn>/legacy 里的旧 EXE 挪回根位(根位现有件停到
// backups/<txn>/launcher-parked-<名>,python 同款),POSIX 补执行位;
// current.json 无条件摘除(不在也 silently 过——python 的 try/except
// OSError: pass)。legacy 里没有旧 EXE -> 不动根位,只摘指针,返回
// false;恢复成功返回 true。
std::expected<bool, std::string> RestoreFlatLegacy(const LayoutPaths& paths,
                                                   const std::string& txn_id);

}  // namespace lubancode::updater
