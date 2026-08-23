// 项目插件信任库(plugins 单第 8 步):项目级 .lubancode/plugins/ 的
// manifest + 文件 hash 审批账。
//
// 规矩(单子「零配置与兼容」,与 hooks 的 project trust 同一条路):
//   1. 项目目录里的插件是外来代码,"放进目录"就是执行代码——首次见到
//      须按 manifest + 全部文件内容的 hash 审批,不装作普通配置;
//   2. 信任按"插件目录路径 + content hash"记账:任一文件改了,hash 变,
//      信任失效,须重审;
//   3. 信任记录放用户主目录(<home>/.lubancode/plugin-trust.json),绝不
//      写回仓库——仓库里的插件改不动自己的信任账;
//   4. 用户主目录的插件(~/.lubancode/plugins/)是用户亲手放的,不进这本
//      账(与 hooks 的 user-level 同待遇);只有项目级的才要审。
//
// 本类不做任何执行;坏 JSON 容错读(警告 + 空账本重开),不崩会话。
// 主线程使用(扫描装载一次),不加锁。
#pragma once

#include <map>
#include <optional>
#include <string>

namespace lubancode::config {

class PluginTrustStore {
public:
    // 默认路径 <home>/.lubancode/plugin-trust.json;home 拿不到就退回内存态。
    static std::optional<std::string> DefaultStorePath();

    // 从 path 读;文件不存在 = 空账本(首访,不是错误)。坏 JSON 返回错误
    // 串,同时账本保持空。path 为空 = 纯内存模式(测试用)。
    static std::pair<PluginTrustStore, std::optional<std::string>> Load(const std::optional<std::string>& path);

    bool IsTrusted(const std::string& plugin_path, const std::string& content_hash) const;
    // 记一条信任。返回 false = 没有落盘路径(纯内存态仍会记上)。
    bool SetTrusted(const std::string& plugin_path, const std::string& content_hash,
                    const std::string& description);
    void Untrust(const std::string& plugin_path, const std::string& content_hash);

    bool IsDisabled(const std::string& plugin_path, const std::string& content_hash) const;
    void SetDisabled(const std::string& plugin_path, const std::string& content_hash, bool disabled);

    bool dirty() const { return dirty_; }
    std::optional<std::string> Save();

    std::size_t trusted_count() const { return trusted_.size(); }

private:
    static std::string Key(const std::string& plugin_path, const std::string& content_hash) {
        return plugin_path + "\n" + content_hash;
    }

    std::optional<std::string> path_;
    struct TrustEntry {
        std::string description;
        std::string trusted_at_unix;
    };
    std::map<std::string, TrustEntry> trusted_;
    std::map<std::string, bool> disabled_;
    bool dirty_ = false;
};

}  // namespace lubancode::config
