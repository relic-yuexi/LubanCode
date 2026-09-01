// WorkspaceIdentityResolver 的实现(P0-1)。裁决顺序与 key 算法见头注释
// 与单子 §四;本文件是生产里唯一算 workspace_key 的地方。
#include "workspace/identity.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "workspace/storage_contracts.hpp"

namespace lubancode::workspace {
namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;
using platform::Utf8ToPath;

fs::path AbsoluteNormal(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

std::string Trim(std::string value) {
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin = 0;
    while (begin < value.size() && whitespace(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string ReadSmallFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool RegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

std::string SafeName(std::string_view value, std::size_t max_bytes) {
    std::string out;
    out.reserve(value.size() < max_bytes ? value.size() : max_bytes);
    bool dash = false;
    for (const unsigned char byte : value) {
        if (out.size() >= max_bytes) {
            break;
        }
        if (byte >= 0x80 || std::isalnum(byte) != 0 || byte == '_' || byte == '-') {
            out.push_back(static_cast<char>(byte));
            dash = false;
        } else if (!dash && !out.empty()) {
            out.push_back('-');
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "project" : out;
}

std::string PathBasenameText(const fs::path& path) {
    // basename 一律从规范化文本切最后一段非空路径,不问平台 fs::path 语义
    //(POSIX 把反斜杠当普通字符;规范文本已统一正斜杠,两边同口径)。
    std::string basename;
    std::istringstream stream(NormalizeIdentityPathText(path));
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty() && segment != "." && segment != "..") {
            basename = segment;
        }
    }
    return basename;
}

}  // namespace

std::string NormalizeIdentityPathText(const fs::path& path) {
    fs::path normalized = fs::absolute(path).lexically_normal();
    std::string text = PathToUtf8(normalized);
    // 统一正斜杠、去尾斜杠。Windows 文件系统大小写不敏感:整串折叠 ASCII
    // 小写,免得 D:/Work 与 d:/work 各立一间 workspace。POSIX 大小写敏感,
    // 保持原样。
    for (char& c : text) {
        if (c == '\\') {
            c = '/';
        }
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
#endif
    }
    if (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

std::string ComputeWorkspaceKeyFromSeed(std::string_view seed, std::string_view display_name) {
    const std::string hash = hooks::Sha256Hex(seed);
    return SafeName(display_name, 48) + "-" + hash.substr(0, 16);
}

std::optional<std::string> ReadMarkerWorkspaceId(const fs::path& marker_path) {
    const std::string text = ReadSmallFile(marker_path);
    if (text.empty()) {
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::nullopt;
    }
    const auto it = json.find("workspace_id");
    if (it == json.end() || !it->is_string()) {
        return std::nullopt;
    }
    std::string id = it->get<std::string>();
    // 坏 JSON、缺键、非串、超长、空串都给 nullopt——marker 只负责定界,
    // 不给身份留暗门(§9.3);裁决路上 nullopt 等于"没有这级 marker"。
    if (id.empty() || id.size() > 128) {
        return std::nullopt;
    }
    return id;
}

std::optional<fs::path> ResolveGitCommonDir(const fs::path& directory) {
    const fs::path dot_git = directory / ".git";
    std::error_code ec;
    if (fs::is_directory(dot_git, ec) && !ec) {
        return AbsoluteNormal(dot_git);
    }
    if (!RegularFile(dot_git)) {
        return std::nullopt;
    }
    // .git 文件(linked worktree/submodule):"gitdir: <path>"。
    const std::string marker = Trim(ReadSmallFile(dot_git));
    constexpr std::string_view prefix = "gitdir:";
    if (!marker.starts_with(prefix)) {
        return std::nullopt;
    }
    fs::path git_dir = Utf8ToPath(Trim(marker.substr(prefix.size())));
    if (git_dir.is_relative()) {
        git_dir = directory / git_dir;
    }
    git_dir = AbsoluteNormal(git_dir);
    // commondir:worktree 的 gitdir 指向 <main>/.git/worktrees/<x>,它自己
    // 记一条相对引用回到 common git dir;submodule 的 gitdir 没有 commondir
    // 文件,它本身就是身份根(不被父仓吞掉,§4.2 最近边界胜)。
    const fs::path common_file = git_dir / "commondir";
    if (!RegularFile(common_file)) {
        return git_dir;
    }
    fs::path common = Utf8ToPath(Trim(ReadSmallFile(common_file)));
    if (common.is_relative()) {
        common = git_dir / common;
    }
    return AbsoluteNormal(common);
}

std::expected<WorkspaceIdentity, std::string> ResolveWorkspaceIdentity(const fs::path& cwd,
                                                                       const fs::path& home_lubancode) {
    // home 层止步(§4.2 第 5 级):用户主目录下的 .lubancode/ 是全局件
    // (config/目录册),不是项目边界;爬到用户主目录(home_lubancode 的父)
    // 这一层不看、不再上爬——否则 temp/桌面一类路径会被 ~/.lubancode/
    // config.json 吸成一只大项目。
    const fs::path home_stop =
        home_lubancode.empty() ? fs::path() : AbsoluteNormal(home_lubancode).parent_path();
    fs::path current = AbsoluteNormal(cwd);
    std::error_code ec;
    if (!fs::is_directory(current, ec)) {
        current = current.parent_path();
    }
    if (current.empty()) {
        return std::unexpected("identity.no_boundary: 工作目录解析不出,起点: " +
                               PathToUtf8(AbsoluteNormal(cwd)));
    }
    const fs::path launch_cwd = current;

    // 一次向上爬,记三种最近边界(§4.2 优先级:git > marker > config;
    // git 的判定是整条祖先链上有没有 .git,嵌套仓取最近一层,不爬向更外层)。
    fs::path git_checkout_root;
    fs::path marker_root;
    std::optional<std::string> marker_workspace_id;
    fs::path config_root;
    bool saw_git = false;
    bool saw_marker = false;
    bool saw_config = false;
    for (fs::path walk = current;; walk = walk.parent_path()) {
        if (!home_stop.empty() && AbsoluteNormal(walk) == home_stop) {
            break;  // home 层是全局件,不是项目边界;到此为止
        }
        if (!saw_git && ResolveGitCommonDir(walk).has_value()) {
            git_checkout_root = walk;
            saw_git = true;
        }
        if (!saw_marker && RegularFile(walk / ".lubancode" / "workspace.json")) {
            marker_root = walk;
            marker_workspace_id = ReadMarkerWorkspaceId(walk / ".lubancode" / "workspace.json");
            saw_marker = true;
        }
        if (!saw_config && RegularFile(walk / ".lubancode" / "config.json")) {
            config_root = walk;
            saw_config = true;
        }
        const fs::path parent = walk.parent_path();
        if (parent.empty() || parent == walk) {
            break;
        }
    }

    WorkspaceIdentity identity;
    identity.launch_cwd = launch_cwd;
    std::string seed;
    if (saw_git) {
        // 1+2 级:Git 取 common git dir;主树与 linked worktree 同 key。
        identity.identity_kind = std::string(contracts::kIdentityKindGitCommon);
        identity.git_common_dir = *ResolveGitCommonDir(git_checkout_root);
        identity.checkout_root = AbsoluteNormal(git_checkout_root);
        identity.project_root = identity.checkout_root;
        identity.identity_root = identity.git_common_dir;
        seed = std::string(contracts::kSeedPrefixGit) + NormalizeIdentityPathText(identity.git_common_dir);
        // 显示前缀跟 common git dir 走:linked worktree 的 checkout 目录名
        // 各不相同,显示名若跟 checkout 走,同仓两房会裂成两个显示名。
        identity.display_name = PathBasenameText(identity.git_common_dir.parent_path());
    } else if (saw_marker && marker_workspace_id.has_value()) {
        // 3 级:marker 声明的稳定 id 定界,seed 不含路径。显示名也跟 id 走
        // ——同 id 的两处目录(团队显式并账语义)由此得出同一把钥匙;目录
        // basename 只在 git/config/cwd 三态下作显示前缀。
        identity.identity_kind = std::string(contracts::kIdentityKindExplicitMarker);
        identity.project_root = AbsoluteNormal(marker_root);
        identity.checkout_root = identity.project_root;
        identity.identity_root = identity.project_root;
        seed = std::string(contracts::kSeedPrefixMarker) + *marker_workspace_id;
        identity.display_name = *marker_workspace_id;
    } else if (saw_config) {
        // 4 级:config 所在目录。
        identity.identity_kind = std::string(contracts::kIdentityKindConfigRoot);
        identity.project_root = AbsoluteNormal(config_root);
        identity.checkout_root = identity.project_root;
        identity.identity_root = identity.project_root;
        seed = std::string(contracts::kSeedPrefixPath) +
               NormalizeIdentityPathText(identity.project_root);
        identity.display_name = PathBasenameText(identity.project_root);
    } else {
        // 5 级:启动 cwd。不爬向用户主目录(爬到也只认最近边界,无边界
        // 就是 cwd)。
        identity.identity_kind = std::string(contracts::kIdentityKindCwdFallback);
        identity.project_root = launch_cwd;
        identity.checkout_root = launch_cwd;
        identity.identity_root = launch_cwd;
        seed = std::string(contracts::kSeedPrefixPath) + NormalizeIdentityPathText(launch_cwd);
        identity.display_name = PathBasenameText(launch_cwd);
    }
    if (identity.display_name.empty()) {
        identity.display_name = "project";
    }
    identity.workspace_key = ComputeWorkspaceKeyFromSeed(seed, identity.display_name);
    return identity;
}

WorkspaceIdentity MakeFallbackIdentity(const fs::path& root) {
    WorkspaceIdentity identity;
    identity.identity_kind = std::string(contracts::kIdentityKindCwdFallback);
    identity.project_root = AbsoluteNormal(root);
    identity.checkout_root = identity.project_root;
    identity.identity_root = identity.project_root;
    identity.launch_cwd = identity.project_root;
    identity.display_name = PathBasenameText(identity.project_root);
    if (identity.display_name.empty()) {
        identity.display_name = "project";
    }
    identity.workspace_key = ComputeWorkspaceKeyFromSeed(
        std::string(contracts::kSeedPrefixPath) + NormalizeIdentityPathText(identity.project_root),
        identity.display_name);
    return identity;
}

}  // namespace lubancode::workspace
