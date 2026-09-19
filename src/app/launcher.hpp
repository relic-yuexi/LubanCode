// 固定启动器(GitHubRelease自动更新单 P1,§六):安装根上的 lubancode(.exe)
// 是"自举式"启动器——同一个二进制,发现自己站在根位(current.json 与
// versions/ 在旁)就按指针把参数原样转发给 versions/<current>/ 里的真实
// EXE;发现自己本来就住在 versions/<v>/ 里则正常运行。程序从自己的版本
// 目录定位随包资源(skills/web 等按 exe 同目录解析,已有逻辑不动)。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::app::launcher {

// 本进程在安装布局里的位置。
enum class Layout {
    Standalone,     // 裸 exe(源码构建/便携):不是任何安装布局
    FlatInstall,    // 旧平铺安装:exe 与 skills/manifest 同目录
    VersionedEntry, // versions/<v>/ 里的真实 EXE:正常运行
    RootLauncher,   // 安装根上的固定启动器:按 current.json 转发
};

struct LayoutInfo {
    Layout layout = Layout::Standalone;
    std::filesystem::path install_root;  // VersionedEntry/RootLauncher/FlatInstall 的安装根
    std::string current;                 // RootLauncher:current.json 指向的版本目录名
    std::filesystem::path current_exe;   // RootLauncher:要转发到的版本 EXE
};

// 纯分类:只看路径形状与旁边有没有 current.json/versions/,不打印不启动。
// 拆出来给单测与 update 子命令共用(定位安装根)。
LayoutInfo ClassifyExecutionLayout(const std::filesystem::path& exe);

// 启动器转发入口:RunCli 最先调它。本进程是根位启动器时,把 args 原样
// (argv[0] 换成版本 EXE 的绝对路径)附着转发——参数、工作目录、环境、
// 标准输入输出、退出码、Ctrl+C 行为全保留——返回子进程退出码;不是启动器
// 返回 nullopt,调用方走正常启动。指针坏了(指向的版本目录不在)不猜:
// 明确报错退 1,留给 lubancode update --rollback / status 处理。
std::optional<int> MaybeRelayToCurrent(const std::vector<std::string>& args);

}  // namespace lubancode::app::launcher
