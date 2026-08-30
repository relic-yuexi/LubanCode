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

// ---------------------------------------------------------------------------
// 按目标文件作用域的结构化解析(AGENTS.md 作用域单 P0):上面那根拼接
// 字符串是它向旧调用方的投影;真账在这里——文档、链、指纹、诊断分开
// (单子 §六"所有权重画")。
// ---------------------------------------------------------------------------

// 链上的一份指令文档:同层选中的那一份(非空 override 压过 AGENTS.md,
// 空文件跳过——机械规矩与旧 loader 一字不差)。
struct InstructionDocument {
    std::filesystem::path source_path;  // 选中的文件(AGENTS.override.md 或 AGENTS.md)
    std::filesystem::path scope_dir;    // 文件所在目录 = 这份规则的作用域
    bool is_override = false;           // 同层 override 压过 AGENTS.md 的记号
    std::string content;                // 剥首尾空白后的正文(全文,不做截断)
    std::string sha256;                 // 正文摘要(指纹的原料)
    std::size_t bytes = 0;              // 正文字节数
};

// 解析过程中值得亮出来的一笔账。P0 只记两类:空文件跳过、同层遮蔽;
// 读失败/坏 UTF-8/越界 symlink 的分档是 P1(诊断命令面),不在此冒进。
struct InstructionDiagnostic {
    std::filesystem::path path;
    std::string code;      // "empty_skipped" / "shadowed_same_directory"
    std::string message;
};

// 一条作用域链:project_root 到目标父目录之间,每层至多一份文档,
// 次序固定 root -> nearest(父先子后,离目标最近的在最后、优先级最高,
// 与 agents.md 开放格式的共同语义一致)。
struct InstructionChain {
    std::filesystem::path project_root;
    std::filesystem::path target_path;              // 归一化后的目标(文件或目录)
    std::vector<InstructionDocument> documents;     // root -> nearest,全文无截断
    std::vector<InstructionDiagnostic> diagnostics; // 空文件/遮蔽等账
    std::string fingerprint;                        // 链指纹:同一组作用域文档 = 同一指纹
    // 旧口径的拼接串(与 LoadProjectInstructions 逐字节同格式,含 32 KiB
    // 帽与 UTF-8 安全截断)——baseline 注入与零退化回归都吃这份投影。
    std::string content;
    std::vector<std::filesystem::path> sources;     // documents 的路径投影
    bool truncated = false;                         // content 投影撞了字节帽
};

// 结构化解析器(单子 §六:某目标路径受哪些文档管,唯一 owner 是它)。
// 无会话状态、无缓存(P1 才做 path+size+mtime 快筛),const 全程可并跑,
// 主代理/AgentTool/Workflow 共享同一份实例,各自持自己的已见指纹账。
class ProjectInstructionResolver {
public:
    ProjectInstructionResolver() = default;
    explicit ProjectInstructionResolver(std::size_t max_bytes) : max_bytes_(max_bytes) {}

    // 机械顺序固定:FindProjectRoot(target) -> root -> target 的父目录。
    // target 是目录取自身,是文件取其父;每一层同层先看非空
    // AGENTS.override.md、再看非空 AGENTS.md。目标不在任何 Git 仓里时,
    // 以目标父目录为根(与 FindProjectRoot 的 fallback 同一规矩)。
    InstructionChain ResolveForPath(const std::filesystem::path& target) const;

    std::size_t max_bytes() const { return max_bytes_; }

private:
    std::size_t max_bytes_ = kDefaultProjectInstructionsMaxBytes;
};

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
