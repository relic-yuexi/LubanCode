// Package 启停账(统一封装单阶段 6):PackageStateStore + /package
// enable|disable 的账务。
//
// 规矩(单子 §12.2"启停账在包外",契约 packages.md §3/§9):
//   1. package.yaml 不写 enabled——清单不装启停状态,写了按未知字段报错;
//      启停另记用户主目录 <home>/.lubancode/package-state.json(照
//      package-trust.json 惯例,绝不写回包目录);
//   2. 账上只记改过启停的包:enabled=false 即停,true 即复启;账上没有
//      的包一概视作启用(缺省启用,不替全世界的包记账);
//   3. 一条记录绑"哪个包、启还是停、哪层哪版、何时改的"——版本与层只是
//      随账记的显示项,启停按包 id 生效(四层同 id 是同一只包,遮蔽归
//      选版,启停靠人);
//   4. 停用的包:扫描发现照旧(list/doctor 可见),挂载一律跳过——连内容
//      组件一件都不挂(mounting 吃 PackageStateSnapshot);list 标 disabled;
//   5. 生效时机照会话钉快照(§12.3):enable/disable 只动账,不拆在跑的
//      会话——下回启动(或 /package reload 重折快照)后的新装配才见。
//
// 本模块只记账,不挂不卸任何组件。坏 JSON 容错读(警告 + 空账重开,视作
// 全启用),不崩会话;主线程使用(启动扫描/命令各一次),不加锁。
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "package/inventory.hpp"  // PackageInventory(账务吃它的身份账)

namespace lubancode::package {

// ---------------------------------------------------------------------------
// 会话钉快照用的只读视图:挂载与 reload 吃这个,不碰盘。默认构造 = 全
// 启用(没有账 = 没人停过谁)。
// ---------------------------------------------------------------------------
struct PackageStateSnapshot {
    std::set<std::string> disabled;  // 账上停用的包 id

    bool IsEnabled(const std::string& package_id) const { return disabled.count(package_id) == 0; }
};

// ---------------------------------------------------------------------------
// 启停账本。
// ---------------------------------------------------------------------------
struct PackageStateEntry {
    std::string package_id;
    bool enabled = true;      // false = 停用;true = 复启
    std::string version;      // 改动那刻的版本(显示用;启停按 id 生效)
    std::string scope;        // 改动那刻包在哪一层(显示用)
    std::string changed_at_unix;
};

class PackageStateStore {
public:
    // 默认路径 <home>/.lubancode/package-state.json;home 拿不到返回
    // nullopt(纯内存模式,记了不落盘)。
    static std::optional<std::string> DefaultStatePath();

    // 从 path 读;文件不存在 = 空账(首访,不是错误)。坏 JSON 返回错误
    // 串,账按空白重开(视作全启用)——启停是"别挂谁"的账,不是放行账,
    // 读不动时按缺省启用续跑,警告亮给用户。path 为空 = 纯内存模式。
    static std::pair<PackageStateStore, std::optional<std::string>> Load(
        const std::optional<std::string>& path);

    // 账上没有 = 启用。
    bool IsEnabled(const std::string& package_id) const;
    std::optional<PackageStateEntry> Find(const std::string& package_id) const;
    const std::map<std::string, PackageStateEntry>& entries() const { return states_; }

    // 记一条启停。与账上同态(已停用再 disable、已启用再 enable)返回
    // false 不动账;真有变化才落账并原子写盘(内部自 Save)。
    bool SetEnabled(const std::string& package_id, const std::string& version,
                    const std::string& scope, bool enabled);

    PackageStateSnapshot Snapshot() const;
    std::optional<std::string> Save();

private:
    std::optional<std::string> path_;
    std::map<std::string, PackageStateEntry> states_;  // key = package id
    bool dirty_ = false;
};

// ---------------------------------------------------------------------------
// /package enable|disable 的账务:结论逐行带出,命令层只管打印(回执风格
// 照 TrustPackage 的五样先例,收窄成启停要的三样:身份、去向、生效时机)。
// ok=false 时 error 有话,lines 为空。store 为 nullptr = 找不到主目录,
// 记账动作明拒;纯内存 store 也能记,只是 Save 无处落,回执里如实说。
// ---------------------------------------------------------------------------
struct PackageStateActionResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> lines;
};

// /package enable|disable <id> 的执行侧(幂等——同态重记只回执,不重写)。
// 回执如实说"下回启动生效"(会话钉快照,不拆在跑的);reload 重折后的
// 新装配同样照新账走。session_mounted_count:本会话快照还给这只包挂着几
// 件内容组件(disable 回执注明"在跑的照旧"),没接会话快照传 -1 不提。
PackageStateActionResult EnableDisablePackage(const PackageInventory& inventory,
                                              PackageStateStore* store, bool enable,
                                              int session_mounted_count = -1);

// 启停状态的一句话(/package list、show 用):
//   - 启用:不提(缺省态不刷屏);
//   - 停用:"已停用(挂载跳过,连内容组件一件不挂)"。
std::string DescribeStateStatus(const PackageInventory& inventory, const PackageStateStore* store);

}  // namespace lubancode::package
