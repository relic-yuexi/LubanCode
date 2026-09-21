// /package 命令(统一封装单阶段 1/4/6):list/show/doctor 只读版 +
// trust/untrust 信任门(阶段 4:批的是整包内容哈希,重启生效)+ enable/
// disable 启停账与 reload 重折快照(阶段 6:启停在包外,生效在下回装配)。
// scaffold 是后续阶段的事。命令参数拆解是纯函数,单测直接钉;IO(扫描、
// 打印、信任账、启停账)在 cpp 的 handler 一头。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::config {
struct Config;
}
namespace lubancode::package {
struct PackageMount;
struct PackageSnapshot;  // provider 现取的现行快照(定义在 package/mounting.hpp)
}
namespace lubancode::tools {
struct SkillMeta;
}

namespace lubancode::app {

// HC-06(材料收窄,第二小批):/package 的窄材料——扫描五路的根、doctor/
// trust 的包外命名空间来源、会话钉快照与 reload 口,全借用(组合根装配,
// 命令期间由控制器保活);空 = 没递,各动作按旧口径如实降级。
struct PackageCommandContext {
    // <home_lubancode> 根(user 层扫描位 + package-store 选中版本折算)。
    const std::optional<std::string>* home_lubancode = nullptr;
    // --package-dir 攒下的 dev 层目录(InteractiveSessionOptions.package_dirs
    // 的借用;不直接递 opts,域文件不进会话层头)。空 = 没有 dev 层。
    const std::vector<std::string>* dev_package_dirs = nullptr;
    // config 的 mcpServers 键(doctor/trust 的包外 MCP 命名空间)。空 = 跳过。
    const lubancode::config::Config* config = nullptr;
    // 会话技能清单(doctor/trust 的包外 Skill 名)。空 = 跳过。
    const std::vector<lubancode::tools::SkillMeta>* skills = nullptr;
    // 现行快照的供应商(list/show/enable-disable 的挂载状态从这现取)。
    // HC-06 第三小批改口:reload 换档后旧快照会被释放,冻死的挂载指针会
    // 悬垂;provider 每次返回现行 shared_ptr,命令期间由持有者保活(与
    // workflow/agent 域同一纪律)。空 = 没接,各动作按"没有包"降级。
    std::function<std::shared_ptr<const lubancode::package::PackageSnapshot>()> package_snapshot_provider;
    // /package reload 的会话侧执行体:重折快照、原子换档、刷下游,回执行
    // 逐行带回。空 = 没接(纯函数装配),reload 明说接不上。
    std::function<std::vector<std::string>()> reload_packages;
};

enum class PackageCommandAction {
    Invalid,
    List,    // /package list [all|user|project|official|dev]
    Show,    // /package show <id>
    Doctor,  // /package doctor <id|路径>
    Trust,   // /package trust <id>(批准整包内容哈希,重启生效)
    Untrust, // /package untrust <id>(销账)
    Enable,  // /package enable <id>(复启;下回装配生效)
    Disable, // /package disable <id>(停用;挂载一律跳过,下回装配生效)
    Reload,  // /package reload(重扫五路折新快照,折好才换)
};

struct ParsedPackageCommand {
    PackageCommandAction action = PackageCommandAction::Invalid;
    std::optional<std::string> scope_filter;  // List 时:user/project/official/dev;nullopt = all
    std::string target;                       // Show/Doctor/Trust/Enable/Disable 的 id 或路径
    std::string bad_word;                     // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、过滤层、目标。认不得的子命令、show/doctor 缺目标一律
// Invalid,由 handler 统一打用法。
ParsedPackageCommand ParsePackageCommand(const std::string& args);

// /package 的分派位(命令注册表登册用)。HC-06(材料收窄,第二小批)起
// 只吃本域窄材料——Package 域编译不再需要 SlashDispatchContext。
CommandFlow HandleSlashPackage(const PackageCommandContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
