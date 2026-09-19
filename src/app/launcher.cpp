// 固定启动器实现(GitHubRelease自动更新单 P1,§六)。布局识别是纯函数
// (单测直接打);转发走 platform::RunAttachedProcess(附着式:不捕获、
// 不加 Job,子进程与用户直连)。
#include "app/launcher.hpp"

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::app::launcher {

namespace {

#ifdef _WIN32
constexpr const char* kExeName = "lubancode.exe";
#else
constexpr const char* kExeName = "lubancode";
#endif

std::optional<std::string> ReadCurrentPointer(const std::filesystem::path& current_json) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(current_json, ec) || ec) return std::nullopt;
    std::ifstream in(current_json, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        if (!parsed.contains("schema") || !parsed["schema"].is_number_integer() ||
            parsed["schema"].get<int>() != 1) {
            return std::nullopt;
        }
        if (!parsed.contains("current") || !parsed["current"].is_string()) return std::nullopt;
        const std::string current = parsed["current"].get<std::string>();
        if (current.empty() || current.find('/') != std::string::npos ||
            current.find('\\') != std::string::npos || current == "." || current == "..") {
            return std::nullopt;  // 目录名单段名,别的形状一律不信
        }
        return current;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

LayoutInfo ClassifyExecutionLayout(const std::filesystem::path& exe) {
    LayoutInfo info;
    std::error_code ec;
    const std::filesystem::path exe_dir = exe.parent_path();
    if (exe_dir.empty()) return info;

    // versions/<v>/ 里的真实 EXE:正常运行,安装根往上提一层
    if (exe_dir.parent_path().filename() == "versions") {
        const std::filesystem::path root = exe_dir.parent_path().parent_path();
        info.layout = Layout::VersionedEntry;
        info.install_root = root;
        return info;
    }

    // 根位启动器:current.json + versions/ 在旁
    const std::filesystem::path current_json = exe_dir / "current.json";
    if (std::filesystem::is_regular_file(current_json, ec) && !ec &&
        std::filesystem::is_directory(exe_dir / "versions", ec) && !ec) {
        auto current = ReadCurrentPointer(current_json);
        if (current.has_value()) {
            info.layout = Layout::RootLauncher;
            info.install_root = exe_dir;
            info.current = *current;
            info.current_exe = exe_dir / "versions" / platform::Utf8ToPath(*current) /
                               platform::Utf8ToPath(kExeName);
            return info;
        }
    }

    // 旧平铺安装:exe 旁边有包件(skills/manifest/受管更新助手)
    if (std::filesystem::is_directory(exe_dir / "skills", ec) ||
        std::filesystem::is_regular_file(exe_dir / "manifest.json", ec) ||
        std::filesystem::is_directory(exe_dir / "updater", ec)) {
        info.layout = Layout::FlatInstall;
        info.install_root = exe_dir;
        return info;
    }
    return info;
}

std::optional<int> MaybeRelayToCurrent(const std::vector<std::string>& args) {
    const auto exe = platform::ExecutablePath();
    if (!exe.has_value() || exe->empty()) return std::nullopt;
    const LayoutInfo info = ClassifyExecutionLayout(*exe);
    if (info.layout != Layout::RootLauncher) return std::nullopt;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(info.current_exe, ec) || ec) {
        std::fprintf(stderr,
                     "[launcher] current 指针指向的版本不在: %s\n"
                     "[launcher] 处理: lubancode update --rollback 切回上次可用整包,"
                     "或 lubancode update status 看安装账。\n",
                     platform::PathToUtf8(info.current_exe).c_str());
        return 1;
    }

    std::vector<std::string> child_argv;
    child_argv.reserve(args.size());
    child_argv.push_back(platform::PathToUtf8(info.current_exe));
    for (std::size_t i = 1; i < args.size(); ++i) {
        child_argv.push_back(args[i]);
    }
    std::string error;
    const int exit_code = platform::RunAttachedProcess(child_argv, &error);
    if (exit_code < 0) {
        std::fprintf(stderr, "[launcher] 转发失败: %s\n", error.c_str());
        return 1;
    }
    return exit_code;
}

}  // namespace lubancode::app::launcher
