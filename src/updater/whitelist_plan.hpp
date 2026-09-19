// 更新助手 C++ 化·批一第②单:白名单覆盖决策表(纯函数层)。
//
// 行为权威是 install.ps1 终稿(PR #140,分支 codex/legacy-install-migration
// 的 scripts/install.ps1):Build-PackageFilePlan 把新包 manifest 过一遍
// 白名单(包根文件表 ∪ 受管树),同名盘面普通文件充当"上次官方"侧,再递
// 给 Build-FilePlan 出动作清单。本文件是那张表的 C++ 投影,语义逐条对齐,
// 别单边发明:
//   - 白名单口径出 install.ps1 Build-PackageFilePlan(L1014-1036):
//     路径整名命中包根文件表 $rootFiles,或路径带 '/' 且首段命中受管树
//     $script:ManagedTrees(L57)。两张常量表已照终稿提取进 whitelist_plan.cpp,
//     出处注明在表头;
//   - 同名盘面普通文件:hash 与清单同 → skip-current(已是新版内容);异 →
//     replace 且先备份——改过也换(install.ps1 头注 L19 白名单口径;Build-
//     FilePlan 该分支的 reason 文案"官方未改,换新"是旧三表语境,白名单
//     语境下 old 侧就是盘面 hash,比较恒真,文案取头注口径);
//   - 清单没有的盘面文件不进计划:无 retire,目录只合并,用户自建件全留
//     (install.ps1 头注 L20-21);
//   - 用户数据路径(config.toml/.env/.lubancode//.agents/,判 IsUserDataPath,
//     批一①的口径)即使清单误收也 keep-user-data 不写不删——对应 Build-
//     FilePlan L541-546 的头一道闸("用户配置和用户数据从不参与安装,即使
//     来源或旧清单误收了它们")。PS 流水线里这类路径其实在白名单过滤那步
//     就被丢了(进不了 allowed),两边殊途同归,此处按闸的声明语义放在
//     白名单判定之前,更防御;
//   - 盘面目录/reparse 挡住白名单路径 → conflict-kind / conflict-reparse
//     (Build-FilePlan L547-563:目录与 reparse 都不写不删,reparse 优先于
//     is_dir 归类)。PlanBlocks 对应 install.ps1 退出码 3(Invoke-Install
//     L1184-1189:conflicts 非空即"路径冲突,未覆盖"退 3;conflicts 的
//     收集口径是 Invoke-ResourceApply 里的 conflict-* 条目)。
// 口径差(如实声明,不算单边发明):
//   - PS 哈希表键/-contains/-match 一律大小写不敏感;此处键与表比对全走
//     FoldKey(批一①,随平台:Windows/macOS 折叠,Linux 原样)。Windows 上
//     与 PS 逐行为一致;Linux 盘面大小写敏感,异写就是两个路径——与
//     install_plan.py 读侧同口径;
//   - Build-FilePlan 十一动作里的 missing-kept / conflict-modified /
//     conflict-collision / retire / keep-modified-retired 在白名单语境下
//     构造性不可达(old 侧只含新包同名盘面普通文件,hd==old.sha256 恒真),
//     本表不设这些动作;keep-unknown(盘面孤儿件)按上一条"不进计划"同
//     样不设——Action 只留六枚;
//   - PS 还会把盘面孤儿 reparse 点名 conflict-reparse(留观,且计入退出码
//     3 的 conflicts)。本表按派工口径只对"挡住白名单路径"的目录/reparse
//     出 Conflict*,盘面孤儿件(含孤儿 reparse)一概不进计划——批三 apply
//     层若要复刻 PS 的孤儿 reparse 阻断,须在盘面扫描侧另补一道;
//   - 计划条目按 new_map 的键序(FoldKey 字节升序)产出;PS 是 Sort-Object
//     (文化序)。只影响报告行的先后,不影响决策。
// 纯函数零 IO:不算 hash、不碰盘——盘面内容比对只用 DiskEntry 里调用方填
// 的 sha256(nullopt 或空串视为"内容未知",按需 Replace 处理并注明 reason;
// PS 此处是惰性算 hash,算 hash 是批三 apply 层的事)。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "updater/manifest.hpp"

namespace lubancode::updater {

// 覆盖动作(install.ps1 Build-FilePlan 动作名的 C++ 化;白名单语境可达的
// 六枚,见文件头口径差第二条):
//   SkipCurrent   已是新版内容,不动;
//   Replace       同名先备份再换新(backup 恒 true);
//   InstallNew    新版新增,落盘;
//   KeepUserData  用户配置/数据,不写不删;
//   ConflictKind  盘面是目录挡住白名单路径,阻断;
//   ConflictReparse 盘面是链接/reparse 挡住白名单路径,阻断。
enum class Action {
    SkipCurrent,
    Replace,
    InstallNew,
    KeepUserData,
    ConflictKind,
    ConflictReparse
};

// 计划里的一条:目标路径(留清单原始拼写,给人看与落盘)+ 动作 + 是否先
// 备份 + 人读缘由(文案口径见文件头,PS 对齐处照抄,分叉处注明)。
struct PlanEntry {
    std::string path;
    Action action;
    bool backup = false;
    std::string reason;
};

// 盘面实况一条(调用方扫盘填好递进来,本层不碰盘):
//   abspath  绝对路径,批三 apply 层备份/换新用,决策只读键;
//   is_dir   盘面是目录;
//   reparse  盘面是链接/reparse 点(挡路即阻断,绝不去动);
//   sha256   盘面文件内容指纹,惰性算——调用方没算就留 nullopt,决策按
//            "内容未知"走 Replace(先备份)。
struct DiskEntry {
    std::filesystem::path abspath;
    bool is_dir = false;
    bool reparse = false;
    std::optional<std::string> sha256;
};

// 白名单覆盖决策表:new_map = 新包 manifest(键必须 FoldKey(path),通常
// 由 ParseManifestText 产出),disk = 安装目录盘面实况(键同样必须
// FoldKey(相对路径),两侧同折叠口径才对得上)。逐清单条目出计划:
// 白名单外与盘面孤儿件不出条目。零 IO,可重入。
std::vector<PlanEntry> BuildWhitelistPlan(
    const std::map<std::string, ManifestEntry>& new_map,
    const std::map<std::string, DiskEntry>& disk);

// 计划是否阻断安装:有 conflict-kind / conflict-reparse 即 true,对应
// install.ps1 的退出码 3。空计划与纯 skip/replace/install 计划不阻断。
bool PlanBlocks(const std::vector<PlanEntry>& plan);

}  // namespace lubancode::updater
