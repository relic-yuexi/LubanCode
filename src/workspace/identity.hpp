// Workspace 统一存储 P0-1:唯一的 WorkspaceIdentityResolver。
//
// 《Workspace 统一存储、旧 Session 退场与分级 Memory 迁移》§四:项目身份
// 只由这只 resolver 裁决。trajectory 与 memory 都只吃结果,不反向各自探
// Git、各算各的 key。四级裁决(§4.2,最近边界胜):
//   1. 最近的 Git 仓库:解析 .git 目录/文件,再解 commondir;
//      identity_root 取 common git dir——主树与 linked worktree 共 workspace。
//   2. 不在 Git 内:最近的 .lubancode/workspace.json(marker)以它声明的
//      稳定 id 定界。
//   3. 没有 marker:最近的 .lubancode/config.json 所在目录。
//   4. 四处皆无:启动 cwd,identity_kind=cwd_fallback。不得一路爬到用户
//      主目录猜成大项目。
//
// workspace_key(§4.3):<safe-display-name>-<first16(SHA256(seed))>。
// seed 前缀三选一(git:/marker:/path:),常量在 storage_contracts.hpp。
// Windows 折叠 ASCII 大小写、统一正斜杠并解绝对规范路径;POSIX 保留
// 大小写。basename 只给人看,不参与唯一性裁决;Git remote URL 不作身份。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::workspace {

// 裁决结果。identity_kind 四值封闭(contracts::kIdentityKind*),不加
// 第五种;字段名与 §4.1/合同 §九的中英对照一致。
struct WorkspaceIdentity {
    std::string workspace_key;
    std::string display_name;
    std::string identity_kind;   // git_common | explicit_marker | config_root | cwd_fallback
    std::filesystem::path project_root;    // 用户眼前的项目根
    std::filesystem::path identity_root;   // 算 key 的规范根
    std::filesystem::path checkout_root;   // 当前 checkout 根;非 Git 时等于 project_root
    std::filesystem::path git_common_dir;  // 非 Git 时为空
    std::filesystem::path launch_cwd;      // 启动工作目录

    bool git() const { return !git_common_dir.empty(); }
    bool valid() const { return !workspace_key.empty(); }
};

// 唯一入口。cwd 是裁决起点;home_lubancode 按单子 §4.1 签名保留(身份
// 本身不依赖 home,新根接线在 P0-2)。失败只在 cwd 无法规范化时发生,
// 错误文本点明路径(§9.2 身份算不出:启动失败,不落 unknown/ 总筐)。
std::expected<WorkspaceIdentity, std::string> ResolveWorkspaceIdentity(
    const std::filesystem::path& cwd, const std::filesystem::path& home_lubancode);

// cwd_fallback 形状的直造身份:测试与"只递 workspace_root"的旧调用
// 兜底用(seed 按 path: 前缀)。生产入口仍是 ResolveWorkspaceIdentity。
WorkspaceIdentity MakeFallbackIdentity(const std::filesystem::path& root);

// ---------------------------------------------------------------------------
// 纯函数部件(自 trajectory/memory 的平级实现收编;测试与迁移器直接吃)
// ---------------------------------------------------------------------------

// 规范化:绝对路径 + lexically_normal + 正斜杠 + 去尾斜杠 + Windows 盘符
// 折叠 ASCII 小写。POSIX 保留大小写。hash 输入先过这道。
std::string NormalizeIdentityPathText(const std::filesystem::path& path);

// key = <safe(display_name)>-<first16(SHA256(seed))>。SafeName:非法字节
// 折为 '-',截 48 字节,空串回 "project"(memory 侧原 SafeName 语义)。
std::string ComputeWorkspaceKeyFromSeed(std::string_view seed, std::string_view display_name);

// 解析一处目录的 .git:目录直取;.git 文件读 "gitdir:" 再解 commondir
// (linked worktree/submodule 与主树同归 common git dir)。非 Git 目录给
// nullopt。原 memory/project_memory.cpp 的同名私件,P0-1 收编于此。
std::optional<std::filesystem::path> ResolveGitCommonDir(const std::filesystem::path& directory);

// 读 marker(.lubancode/workspace.json)声明的稳定 workspace_id。坏 JSON、
// 缺键、非串、超长都给 nullopt(doctor 对账与迁移器复用同一读法)。
std::optional<std::string> ReadMarkerWorkspaceId(const std::filesystem::path& marker_path);

}  // namespace lubancode::workspace
