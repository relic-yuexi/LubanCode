// 更新助手 C++ 化·批三第②单:平铺交接层——完整备份、旧启动器让位与
// 回滚。SV-03 收敛后本模块是平铺交接文件机械的唯一实现,引擎(engine.cpp)
// 只编排事务并把 FlatFailure 映射为退出码。
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
//     白名单口径下本就 conflict 留观,不搬无损)。SV-03 收敛前引擎自带
//     的 copy_file 副本走 copy_symlinks 保链接,收敛后整仓只剩本政策;
//   - python restore_flat_legacy 的 park 是裸 rename,失败炸恢复链且
//     current 不摘——docstring"恢复不成也要摘掉指针"在 python 侧未兑现。
//     本模块兑现它:恢复链任一步失败也不早退,摘完 current 再返回失败
//     (必摘指针的硬保证优先,现场保留给人工诊断);
//   - 错误是类型化结果 FlatFailure(分类 + 人话),退出码语义归引擎;
//     rename 失败的文案带系统错误串,引擎据此停 needs-review。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "updater/layout.hpp"

namespace lubancode::updater {

// 平铺交接失败的分类:NeedsReview = 用户清障(退出进程/挪走占位)后重跑
// lubancode update 可续上(staging 留档不重下);Failed = 不可续的硬失败。
// 只标事实,不给退出码——退出码归引擎的事务层。
enum class FlatHandoverError { NeedsReview, Failed };

// 一笔平铺交接失败:message 是给人看的完整一句(进日志与事务账)。
struct FlatFailure {
    FlatHandoverError kind = FlatHandoverError::Failed;
    std::string message;
};

// 平铺迁移完整备份:受管树(全递归)+ 安装根顶层文件(排除记录件)原样
// 进备份。先落 backups/<txn>/.flat-staging-<pid> 再改名成 backups/<txn>/flat
// (python 两步走的同款:备份集名换入是原子的);改名不成留原地,返回值
// 就是真实落点,账上照记。没有任何可备的东西时返回 nullopt
// (python activate 的 any(isdir) 守卫)。
//   install_root  安装根;
//   managed_trees 受管树名表;
//   record_files  根级记录件排除表(manifest.json/install-state.json);
//   txn_id        事务 id(backups/<txn> 的 <txn>)。
std::expected<std::optional<std::filesystem::path>, FlatFailure> FlatFullBackup(
    const std::filesystem::path& install_root,
    const std::vector<std::string>& managed_trees,
    const std::vector<std::string>& record_files,
    const std::string& txn_id);

// 平铺 -> 版本化交接:旧根 EXE rename 进 backups/<txn>/legacy(已停过一
// 次的先清位),挪不进明报 NeedsReview(文案照 python:多半被运行中的
// 进程/杀软锁着,绝不强杀);新版 EXE(version_dir 里的根位同名件)拷到
// 根位当固定启动器,POSIX 补执行位;version_dir/updater 树按文件覆盖同步
// 到根 updater(不清不删)。安装根与版本目录重叠时拒绝交接(python 同款
// 守卫,Failed)。legacy 备份目录建不成也是 NeedsReview(用户清障后重跑
// 续上);其余失败(版本目录无启动器/拷贝/同步)是 Failed。
std::expected<void, FlatFailure> HandoverFlatLauncher(const LayoutPaths& paths,
                                                      const std::string& txn_id,
                                                      const std::filesystem::path& version_dir);

// 回滚:backups/<txn>/legacy 里的旧 EXE 挪回根位(根位现有件停到
// backups/<txn>/launcher-parked-<名>,python 同款),POSIX 补执行位;
// current.json 无条件摘除(不在也 silently 过)。legacy 里没有旧 EXE ->
// 不动根位,只摘指针,返回 false;恢复成功返回 true。恢复链任一步失败
// (park 挪不动/拷贝失败)也不早退:摘完 current 再返回 unexpected——
// "恢复不成也要摘掉指针"是硬保证,失败串进 FlatFailure,现场留给人工
// 诊断(引擎侧据此转 needs-review)。
std::expected<bool, FlatFailure> RestoreFlatLegacy(const LayoutPaths& paths,
                                                   const std::string& txn_id);

}  // namespace lubancode::updater
