// Package 信任(统一封装单阶段 4):PackageTrustStore + 信任门 + /package
// trust|untrust 的材料与账务。
//
// 规矩(单子 §九"发现不等于执行",与 plugins 单第 8 步的 Plugin 信任同
// 一条路):
//   1. 信任只锚整包内容哈希(阶段 1 的盘点算法,platform/dir_fingerprint
//      的 Package v1 材料):包内任一文件改一个字节,哈希就变,IsTrusted
//      即 false,旧信任失效,须重批;
//   2. 账本落用户主目录 <home>/.lubancode/package-trust.json(照 plugin-
//      trust.json 惯例),绝不写回包目录——仓库里的包改不动自己的信任账;
//   3. 信任门只管外来层:project 与 dev(--package-dir)。user/official 是
//      用户亲手放/官方发布的,视作已安装来源,不经门(§9.2;与 /plugin
//      trust 同尺:用户主目录的插件不审);
//   4. 批准记录绑"哪个包、哪个版本、哪枚哈希、何时批的"——版本只是随
//      账记的显示项(package.yaml 本身在哈希里,版本变哈希必变),比对
//      只认 id + 哈希;
//   5. 未过门的包:code 组件(Plugin/MCP)一件不挂不执行,content 组件
//      照挂(阶段 3 语义不变);依赖未信任 code 组件的 Agent、Workflow
//      标 unavailable(mounting 的连坐账);
//   6. 批准后重启生效——会话钉快照(阶段 3 语义),运行中的会话不换账。
//
// 本模块不做任何执行;坏 JSON 容错读(警告 + 空账本重开),不崩会话。
// 主线程使用(启动扫描/命令各一次),不加锁。
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "package/catalog.hpp"     // PackageRecord(审批材料吃它)
#include "package/inventory.hpp"   // PackageScope

namespace lubancode::package {

// 信任门只管外来层(§9.2):project 与 dev 要批,user/official 放置即信任。
bool ScopeRequiresTrust(PackageScope scope);

// ---------------------------------------------------------------------------
// 会话钉快照用的只读视图:挂载与分析吃这个,不碰盘。默认构造 = 没人有
// 信任(测试与"找不到主目录"的兜底)。
// ---------------------------------------------------------------------------
struct PackageTrustSnapshot {
    std::set<std::string> keys;  // "<package_id>\n<content_hash>"

    bool IsTrusted(const std::string& package_id, const std::string& content_hash) const;
};

// ---------------------------------------------------------------------------
// 批准账本。
// ---------------------------------------------------------------------------
struct PackageTrustEntry {
    std::string package_id;
    std::string version;        // 批准时的版本(显示用;比对只认哈希)
    std::string content_hash;   // 整包内容哈希(64 hex)
    std::string scope;          // 批准时包在哪一层(显示用)
    std::string trusted_at_unix;
};

class PackageTrustStore {
public:
    // 默认路径 <home>/.lubancode/package-trust.json;home 拿不到返回
    // nullopt(纯内存模式,记了不落盘)。
    static std::optional<std::string> DefaultStorePath();

    // 从 path 读;文件不存在 = 空账本(首访,不是错误)。坏 JSON 返回错误
    // 串,账本保持空(重新审一遍比带着一本读不动的账继续跑更安全)。
    // path 为空 = 纯内存模式(测试用)。
    static std::pair<PackageTrustStore, std::optional<std::string>> Load(
        const std::optional<std::string>& path);

    bool IsTrusted(const std::string& package_id, const std::string& content_hash) const;
    // 该包名下最新批的一条(任意哈希):给 show 认"批过、但哈希已对不上"。
    std::optional<PackageTrustEntry> Latest(const std::string& package_id) const;
    // 记一条批准。内部自 Save;纯内存态照记,只是没有落盘处。
    void SetTrusted(const std::string& package_id, const std::string& version,
                    const std::string& content_hash, const std::string& scope);
    // 销掉该包名下全部批准(含哈希已对不上的陈账)。返回销了哪些。
    std::vector<PackageTrustEntry> Untrust(const std::string& package_id);

    std::size_t trusted_count() const { return trusted_.size(); }
    std::optional<std::string> Save();
    PackageTrustSnapshot Snapshot() const;

private:
    static std::string Key(const std::string& package_id, const std::string& content_hash) {
        return package_id + "\n" + content_hash;
    }

    std::optional<std::string> path_;
    std::map<std::string, PackageTrustEntry> trusted_;  // key = id + "\n" + hash
    bool dirty_ = false;
};

// ---------------------------------------------------------------------------
// /package trust|untrust 的材料与账务:结论逐行带出,命令层只管打印(回执
// 风格照 /plugin trust 的五样先例:身份行、来源行、逐件命令面、文件数 +
// 完整指纹、结论行)。ok=false 时 error 有话,lines 为空。store 由调用方
// 持有(可为 nullptr = 找不到主目录,记账动作明拒);纯内存 store 也能批,
// 只是 Save 无处落,回执里如实说。
// ---------------------------------------------------------------------------
struct PackageTrustActionResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> lines;
};

// 审批材料(纯展示,不动账):包身份、包根、code 组件逐件(插件命令与工
// 具、MCP 的 command/args/env 形状与网络声明)、文件数、完整内容指纹。
std::vector<std::string> BuildPackageApprovalLines(const PackageRecord& record);

// /package trust <id> 的执行侧:亮材料后落账(幂等——同枚哈希重批只回执,
// 不重记)。content-only 与免审层(user/official)不动账,话里说明。
PackageTrustActionResult TrustPackage(const PackageRecord& record, PackageTrustStore* store);

// /package untrust <id> 的执行侧:销该包全部批准;没批过就如实说。
PackageTrustActionResult UntrustPackage(const PackageRecord& record, PackageTrustStore* store);

// 信任状态的一句话(/package list、show 用):
//   - 无 code 组件:不经信任门;
//   - 免审层:视作已安装来源;
//   - 已信任 / 未信任 / 哈希失效(指路重批)。
// store 为 nullptr 时按"没账本"说话(有 code 组件即未信任)。
std::string DescribeTrustStatus(const PackageInventory& inventory, const PackageTrustStore* store);

}  // namespace lubancode::package
