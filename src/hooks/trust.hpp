// hook 信任库:project hooks 的 definition hash 审查账。
//
// 规矩(规格"项目 hook 先审后跑"):
//   1. 未信任的项目 hook 不起进程,启动与 /hooks 里报明;
//   2. 信任按"来源文件路径 + definition hash"记账——命令/参数/脚本路径/
//      handler 类型/timeout/async 任一改,hash 变,信任失效,须重审;
//      仓库挪了地方(source_path 变),同样须重审(保守取边:宁可多问一句,
//      不让陌生目录里的旧信任顺过来);
//   3. 信任记录放用户主目录(<home>/.lubancode/hook-trust.json),绝不写回
//      仓库——仓库里的配置改不动自己的信任账;
//   4. managed hook 由策略信任,不进这本账,普通用户不能在 UI 里关。
//
// 本类不做任何进程操作;坏 JSON 容错读(警告 + 空 账本重开),不崩会话。
// 主线程使用(会话启动装载一次,/hooks 动作时写回),不加锁。
#pragma once

#include <map>
#include <optional>
#include <string>

namespace lubancode::hooks {

class HookTrustStore {
public:
    // 默认路径 <home>/.lubancode/hook-trust.json;home 拿不到(极少见)就
    // 退回内存态(一切信任判定照常,只是不落盘)。
    static std::optional<std::string> DefaultStorePath();

    // 从 path 读;文件不存在 = 空账本(首访,不是错误)。坏 JSON 返回错误串,
    // 同时账本保持空——调用方决定要不要把警告打给用户。
    // path 为空 = 纯内存模式(测试用)。
    static std::pair<HookTrustStore, std::optional<std::string>> Load(const std::optional<std::string>& path);

    bool IsTrusted(const std::string& source_path, const std::string& definition_hash) const;
    // 记一条信任。返回 false = 没有落盘路径(纯内存态仍会记上,返回 true)。
    bool SetTrusted(const std::string& source_path, const std::string& definition_hash, const std::string& command);
    void Untrust(const std::string& source_path, const std::string& definition_hash);

    bool IsDisabled(const std::string& source_path, const std::string& definition_hash) const;
    void SetDisabled(const std::string& source_path, const std::string& definition_hash, bool disabled);

    // 账本里记的命令(信任时快照),/hooks 展示"当初信的是什么"用。没记
    // 过返回空串。
    std::string TrustedCommand(const std::string& source_path, const std::string& definition_hash) const;

    bool dirty() const { return dirty_; }
    // 写回;没有路径或没改过就空操作。失败返回错误串(不抛)。
    std::optional<std::string> Save();

    std::size_t trusted_count() const { return trusted_.size(); }

private:
    static std::string Key(const std::string& source_path, const std::string& definition_hash) {
        return source_path + "\n" + definition_hash;
    }

    std::optional<std::string> path_;
    // key -> {command, trusted_at_unix}
    struct TrustEntry {
        std::string command;
        std::int64_t trusted_at = 0;
    };
    std::map<std::string, TrustEntry> trusted_;
    std::map<std::string, bool> disabled_;
    bool dirty_ = false;
};

}  // namespace lubancode::hooks
