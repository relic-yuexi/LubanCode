// IM 入口的最近选择偏好(§6.2):独立的、可版本化的小档,只存标识
//({channel_id, account_id} 与各平台最近账号),不存 AppID、密钥、token
// 或聊天内容。
//
// 边界:CLI 界面独占这份偏好;Gateway 运行状态、平台连接状态都不进这里。
// 落位 <配置根>/im-preferences.json,按应用参数根隔离(个人/受管不串号)。
// 原子保存(同目录临时件 + 原子替换);损坏/丢失只回空偏好,调用方回
// 选择流程,不阻止已有配置使用。非交互命令不写本档。
#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace lubancode::config {

struct ImRecentSelection {
    std::string channel_id;
    std::string account_id;
    bool present() const { return !channel_id.empty() && !account_id.empty(); }
};

struct ImPreferences {
    std::optional<ImRecentSelection> last;             // 上次使用的平台/账号
    std::map<std::string, std::string> recent_account;  // 平台 -> 最近账号
};

class ImPreferenceStore {
public:
    explicit ImPreferenceStore(std::filesystem::path file);

    // <HomeLubancodeDir>/im-preferences.json;拿不到主目录返回 nullopt。
    static std::optional<std::filesystem::path> DefaultFilePath();

    // 读档:文件不在/坏了/版本不认 → 空偏好(不抛、不报错打断启动)。
    ImPreferences Load() const;

    // 原子写回(dump(2) + AtomicWriteFile)。目录不在会建。失败带人话
    //(expected 语义:有值 = 成功;错误在 unexpected)。
    std::expected<void, std::string> Save(const ImPreferences& preferences) const;

    const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path file_;
};

}  // namespace lubancode::config
