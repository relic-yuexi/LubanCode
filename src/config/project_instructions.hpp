#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace lubancode::config {

constexpr std::size_t kDefaultProjectInstructionsMaxBytes = 32 * 1024;

// 项目根发现(自定义 Agent 单阶段 1 起对外):从 cwd 往上找 .git,找不到
// 就用 cwd 本身。项目级配置/Agent 目录都从这里起算,不各自猜 cwd。
std::filesystem::path FindProjectRoot(const std::filesystem::path& cwd);

struct ProjectInstructions {
    std::filesystem::path project_root;
    std::vector<std::filesystem::path> sources;
    std::string content;
    bool truncated = false;
};

// 从 Git 根走到 cwd。每层优先 AGENTS.override.md，其次 AGENTS.md；空文件
// 跳过，越靠近 cwd 的内容越晚注入，因而优先级越高。
ProjectInstructions LoadProjectInstructions(
    const std::filesystem::path& cwd,
    std::size_t max_bytes = kDefaultProjectInstructionsMaxBytes);

enum class InitProjectInstructionsStatus { Created, AlreadyExists, Error };

struct InitProjectInstructionsResult {
    InitProjectInstructionsStatus status = InitProjectInstructionsStatus::Error;
    std::filesystem::path path;
    std::string error;
};

// 在 Git 根（无 Git 仓库则在 cwd）创建 AGENTS.md。已有 AGENTS.md 或
// AGENTS.override.md 时不覆盖。
InitProjectInstructionsResult InitializeProjectInstructions(const std::filesystem::path& cwd);

}  // namespace lubancode::config
