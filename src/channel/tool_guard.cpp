// 受保护路径闸实现(QQ 机器人接入单 Q0)。合同见 tool_guard.hpp;
// canonical 比对复用 platform::PathComparisonKey(weakly_canonical -> UTF-8
// -> 正斜杠 -> 折小写),与 worktree 隔离闸同一比较口径。
#include "channel/tool_guard.hpp"

#include <filesystem>

#include "channel/manager.hpp"   // DefaultChannelsStateRoot:渠道状态树唯一口
#include "config/config.hpp"     // HomeLubancodeDir/StateRootDir
#include "platform/paths.hpp"   // PathComparisonKey/Utf8ToPath/PathToUtf8

namespace lubancode::channel {

ChannelProtectedPaths DefaultChannelProtectedPaths() {
    ChannelProtectedPaths out;
    // 全局 config.json(参数根,材料):模型 API key 与渠道 secret 引用都
    // 住这里——走 HomeLubancodeDir(应用根语义=参数根)。
    if (const auto home = config::HomeLubancodeDir(); home.has_value()) {
        const std::filesystem::path home_path = platform::Utf8ToPath(*home);
        out.files.push_back(platform::PathToUtf8(home_path / "config.json"));
    }
    // 以下是运行状态,随状态根走(应用根语义=数据根;个人布局状态根与
    // 材料根同目录,护的路径与从前逐字节一致)。
    if (const auto state = config::StateRootDir(); state.has_value()) {
        // 渠道状态根:账号凭据/锁/ingress 账/pairing 记录(credentials.json.enc
        // 首版不生成,但树整体受保护)。与 gateway run 装配同一棵树。
        out.roots.push_back(platform::PathToUtf8(DefaultChannelsStateRoot()));
        // Package/插件信任账:改一行就能放恶意 sidecar/插件进门。
        const std::filesystem::path state_path = platform::Utf8ToPath(*state);
        out.files.push_back(platform::PathToUtf8(state_path / "package-trust.json"));
        out.files.push_back(platform::PathToUtf8(state_path / "plugin-trust.json"));
        // rg-stage 是随包 rg 的用户级落点,防调包。
        out.roots.push_back(platform::PathToUtf8(state_path / "rg-stage"));
    }
    return out;
}

namespace {

bool KeyInProtectedRoot(const std::string& key, const std::string& root_key) {
    if (root_key.empty()) {
        return false;
    }
    return key == root_key || key.rfind(root_key + "/", 0) == 0;
}

bool PathBlocked(const std::string& utf8_path, const ChannelProtectedPaths& protected_paths,
                 std::string* why) {
    const std::string key = platform::PathComparisonKey(platform::Utf8ToPath(utf8_path));
    for (const std::string& file : protected_paths.files) {
        if (key == platform::PathComparisonKey(platform::Utf8ToPath(file))) {
            *why = file;
            return true;
        }
    }
    for (const std::string& root : protected_paths.roots) {
        if (KeyInProtectedRoot(key, platform::PathComparisonKey(platform::Utf8ToPath(root)))) {
            *why = root;
            return true;
        }
    }
    return false;
}

}  // namespace

std::string ChannelToolPathBlocked(const std::string& tool_name, const nlohmann::json& input,
                                   const ChannelProtectedPaths& protected_paths,
                                   const std::string& default_search_root) {
    if (protected_paths.files.empty() && protected_paths.roots.empty()) {
        return std::string();
    }
    // 认得的路径入参:read_file.path(必填)、search.path(可缺,缺省走
    // default_search_root)。其余工具没有本地路径入参,不走这道闸。
    const bool takes_path = tool_name == "read_file" || tool_name == "search";
    if (!takes_path) {
        return std::string();
    }
    std::string path;
    if (const auto it = input.find("path"); it != input.end() && it->is_string()) {
        path = it->get<std::string>();
    } else if (tool_name == "search" && !default_search_root.empty()) {
        // search 不带 path:从会话 cwd 递归搜。cwd 若落在受保护根里
        //(比如有人把 gateway 起在 ~/.lubancode 下),同样拦。
        path = default_search_root;
    }
    if (path.empty()) {
        return std::string();
    }
    std::string why;
    if (PathBlocked(path, protected_paths, &why)) {
        return "渠道会话的读文件/搜索工具不得触碰受保护路径(" + why +
               ":账号凭据与全局密钥配置所在)。请换一个工作路径。";
    }
    return std::string();
}

}  // namespace lubancode::channel
