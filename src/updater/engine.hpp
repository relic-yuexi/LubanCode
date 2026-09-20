// 更新助手 C++ 化·批三第①单(引擎主链):五动词事务引擎。
//
// 语义唯一真源 scripts/updater.py:
//   - update —— cmd_update(L1185-1287)的编排逐段照搬:锁 -> FindResumable
//     (同 digest 续跑,异目标 superseded 作废)-> 磁盘预检(余量 256MiB,
//     need=asset×3,estimate_need/disk_preflight)-> downloading
//     (DownloadWithResume 或本地包拷贝 copy_local_archive)-> verified ->
//     UnpackArchive -> verify_package -> staged -> 所有权预检(冲突 ->
//     needs-review 退 2,staging 留档续跑)-> 平铺完整备份 -> activate
//     (waiting-for-idle 持久化 rollback_current/layout_before;activating:
//     staging/<txn>/pkg 单次 rename 进 versions/<ver>-<sha8>,目录 fsync,
//     原子写 current.json;平铺再走 handover_flat_launcher:旧根 EXE rename
//     进 backups/<txn>/legacy——Windows 对运行中映像允许改名,绝不强杀;
//     挪不动停 needs-review)-> probe 探新版 -> healthy -> 写 install-state
//     (schema 2)-> committed -> 清 staging -> gc;
//   - 异常分流照 python L1272-1287:指针已换且 state∈{activating,healthy}
//     -> rollback_after_activation_failure(先探针旧版本目录,探不过 ->
//     restore 平铺旧 EXE 并必摘 current.json)退 3;否则 failed 清 staging
//     退 1。needs-review/rolled-back 直通,不走 failed 收尾;
//   - rollback —— cmd_rollback(L1290-1341):先探针 previous,过了才换指针;
//   - gc —— gc_versions(L795-847):keep = current+previous+在途目标+运行中
//     版本(Windows 走 IsFileLockedForWrite,POSIX 走 ProcessHoldsExe 扫
//     /proc);删前清单外文件抢救进 backups/stranded-<名>/;
//   - status —— cmd_status(L1031-1071):--json 输出 layout/current/
//     previous/installed/pending_transactions,CanonicalJsonDump 形状;
//   - plan —— cmd_plan(L1115-1168):预演,不动安装。
//
// 接线缝(给批三③):
//   - PrecheckFn 是所有权预检缝:批三③把 install_scan + whitelist_plan 接
//     进来,引擎只认它的裁定(nullptr = 无冲突直过,单测用);错串 = 冲突
//     详情,进 needs-review 的人话报错与事务账 blocking 字段(现行落
//     [{"detail": <错串>}] 一条,③给出结构化条目后再对齐 python 的
//     {path, action} 形);
//   - EngineArgs 照 CLI 参数拼:verb 从子命令来,digest 缺省时配
//     from_archive 自算摘要(collect_target 同款),release_id/asset_id 零值
//     与空串按"未给"折 nullopt,tag 缺省 "v"+version,exe_version 缺省同
//     version,channel 缺省 "stable"。
//
// 口径差(如实声明):
//   - install-state 的 installer 记账写 "lubancode-updater"(python 写
//     "updater.py")——账面身份如实,谁装的写谁;
//   - needs-review 报错文案把 python 的冲突计数换成 seam 错串直陈
//     (seam 只回一份详情,计数归③);
//   - restore_flat_legacy 的 launcher-parked 挪移是尽力而为(python 裸
//     rename,失败会炸恢复链;这里吞错继续,必摘指针那条硬保证不变);
//   - gc 读不懂旧版本目录里的 manifest.json 时按"全清单外"处理——
//     清单外文件全部抢救后才删(python read_manifest_file 直接退程,
//     这里选保守)。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace lubancode::updater {

// 人读行出口:引擎拼 "[download] N MiB"、"[activate] current -> …" 这类
// 进度/结果行递出来,CLI 侧决定落 stdout 还是 stderr。行不保证单行——
// status --json 的整份 CanonicalJsonDump 走一次调用。空 sink 丢弃输出。
using ProgressSink = std::function<void(std::string_view line)>;

struct EngineArgs {
    std::string verb;  // status | plan | update | rollback | gc
    std::filesystem::path install_root;
    std::string repo;  // "owner/name",仅拼资产 URL
    std::string version, tag, exe_version, channel;
    std::uint64_t release_id = 0, asset_id = 0;
    std::string asset_name, digest;  // sha256:<hex>
    std::optional<std::uint64_t> asset_size;
    std::optional<std::filesystem::path> from_archive;  // --from 本地包(自算摘要)
    bool json = false;                                    // status --json
};

// 所有权预检缝:批三③把 install_scan+whitelist_plan 接进来;引擎只认它的
// 裁定。错串 = 冲突详情 -> needs-review。
using PrecheckFn = std::function<std::expected<void, std::string>(
    const std::filesystem::path& pkg_dir, const std::filesystem::path& install_root)>;

// 跑一次更新助手。返回进程退出码:0 成功;1 failed(旧版继续可用);
// 2 needs-review(冲突/交接未完,等用户处理);3 rolled-back(已恢复旧版)。
// precheck 为 nullptr 时视为"无冲突直过"(单测用)。
int RunUpdaterEngine(const EngineArgs& args, const ProgressSink& sink,
                     PrecheckFn precheck = nullptr);

}  // namespace lubancode::updater
