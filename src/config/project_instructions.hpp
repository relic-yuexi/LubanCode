#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
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
// 按目标文件作用域的结构化解析(AGENTS.md 作用域单 P0/P1):上面那根拼接
// 字符串是它向旧调用方的投影;真账在这里——文档、链、指纹、诊断分开
// (单子 §六"所有权重画")。
// ---------------------------------------------------------------------------

// 链上的一份指令文档:同层选中的那一份(非空 override 压过 AGENTS.md,
// 空文件跳过——机械规矩与旧 loader 一字不差)。
struct InstructionDocument {
    std::filesystem::path source_path;  // 选中的文件(override/AGENTS/fallback)
    std::filesystem::path scope_dir;    // 文件所在目录 = 这份规则的作用域
    bool is_override = false;           // 同层 override 压过 AGENTS.md 的记号
    bool is_fallback = false;           // P2-2:显式配置的 fallback 文件名命中
    bool is_global = false;             // P2-1:~/.lubancode/AGENTS.md 全局层
    std::string content;                // 剥首尾空白后的正文(全文,不做截断)
    std::string sha256;                 // 正文摘要(指纹的原料)
    std::size_t bytes = 0;              // 正文字节数
};

// 解析过程中值得亮出来的一笔账。P0 记两类(空文件/同层遮蔽);P1 起分型
// 齐全(单子 §九)——I/O 错、坏 UTF-8、越界 symlink、超预算、fallback
// 命中、迁移提示各有各的 code,不再把"打不开"与"空文件"混成"没发现"。
struct InstructionDiagnostic {
    std::filesystem::path path;
    std::string code;      // 见下方 kInstructionDiagnosticCodes 的取值表
    std::string message;
};

// 诊断分型(单子 §九"必须区分"的清单;symlink 侧按 §10.3 细分)。
// outside_project 并未单列:目标永远按它自己的项目根解析(根随目标走,
// 没有"目标在项目外"这条岔路),真正越界的只有 symlink 一途,归
// symlink_outside_project。
inline constexpr const char* kInstructionDiagnosticCodes[] = {
    "empty_skipped",          // 文件在但剥空白后为空,跳过
    "shadowed_same_directory",  // 同层非空 override 压住了这份 AGENTS.md
    "read_error",             // 文件在却读不动(权限/短暂 I/O 错)
    "invalid_utf8",           // 内容不是合法 UTF-8,拒收(不进提示词)
    "symlink_outside_project",  // 符号链接解析到项目外,拒读
    "symlink_broken",         // 符号链接断链/成环,读不动
    "symlink_inside_project",  // 链到项目内:允许,账里记 link 与真实路径
    "over_budget",            // 拼装投影撞了字节帽,有文档没装下
    "fallback_used",          // P2-2:fallback 文件名命中,该层取了它
    "migration_hint",         // P2-3:发现别家工具的规则文件,只提示不读
};

// 一条作用域链:project_root 到目标父目录之间,每层至多一份文档,
// 次序固定 root -> nearest(父先子后,离目标最近的在最后、优先级最高,
// 与 agents.md 开放格式的共同语义一致)。
struct InstructionChain {
    std::filesystem::path project_root;
    std::filesystem::path target_path;              // 归一化后的目标(文件或目录)
    std::vector<InstructionDocument> documents;     // root -> nearest,全文无截断
    std::vector<InstructionDiagnostic> diagnostics; // 空文件/遮蔽/读错/超限等账
    std::string fingerprint;                        // 链指纹:同一组作用域文档 = 同一指纹
    // 旧口径的拼接串(与 LoadProjectInstructions 逐字节同格式,含 32 KiB
    // 帽与 UTF-8 安全截断)——baseline 注入与零退化回归都吃这份投影。
    std::string content;
    std::vector<std::filesystem::path> sources;     // documents 的路径投影
    bool truncated = false;                         // content 投影撞了字节帽
    // P1-4:字节帽下没装进 content 投影的整份文档(截断点上的那份被腰斩,
    // 记在 truncated,不在这里)。/instructions 亮账用。
    std::vector<std::filesystem::path> dropped_for_budget;
};

// Resolver 的装配口径(作用域单 P1/P2):
//   max_bytes                拼装投影的字节帽(默认 32 KiB)
//   fallback_filenames       P2-2:每层在 override/AGENTS 都没命中时按序
//                            取第一份非空;默认空 = 不启用
//   global_instructions_path P2-1:~/.lubancode/AGENTS.md;存在且非空时作为
//                            优先级最低的一层垫在 project root 之前
//   file_reader              读文件的 seam:默认实读磁盘;单测注入失败,
//                            将来隔离 worktree 的路径映射也走这里
struct ProjectInstructionResolverOptions {
    std::size_t max_bytes = kDefaultProjectInstructionsMaxBytes;
    std::vector<std::string> fallback_filenames;
    std::optional<std::filesystem::path> global_instructions_path;
    std::function<std::optional<std::string>(const std::filesystem::path&)> file_reader;
};

// 结构化解析器(单子 §六:某目标路径受哪些文档管,唯一 owner 是它)。
// 主代理/AgentTool/Workflow 共享同一份实例,各自持自己的已见指纹账。
//
// P1 缓存(单子 §十一 P1-5):文档正文按 path + size + mtime 快筛、内容
// hash 落锤——stat 未变直接用缓存正文与摘要;stat 一变即重读重哈希,
// 旧指纹自然作废(指纹只认内容)。外部编辑不必常驻监听,下一次
// Resolve 的 stat 快筛就能发现(P1-6 的惰性刷新)。锁只罩缓存表,
// ResolveForPath 仍全程 const 可并跑。
class ProjectInstructionResolver {
public:
    ProjectInstructionResolver() = default;
    explicit ProjectInstructionResolver(std::size_t max_bytes);
    explicit ProjectInstructionResolver(ProjectInstructionResolverOptions options);

    // 机械顺序固定:FindProjectRoot(target) -> root -> target 的父目录。
    // target 是目录取自身,是文件取其父;每一层同层先看非空
    // AGENTS.override.md、再看非空 AGENTS.md,都没有再看 fallback 名单
    //(P2-2,显式配置才生效)。目标不在任何 Git 仓里时,以目标父目录为根
    //(与 FindProjectRoot 的 fallback 同一规矩)。
    InstructionChain ResolveForPath(const std::filesystem::path& target) const;

    std::size_t max_bytes() const { return options_.max_bytes; }

    // 诊断/测试用:缓存里现存几份文档正文。
    std::size_t cached_documents() const;

private:
    // 读一枚文档的分型结果(P1-3):打不开、坏 UTF-8、为空各归各账,
    // 不再静默降成"没指令"。Missing 也兜"文件在但被 symlink 边界拒了"
    // 之外的"不在"——不参选、不记错。
    enum class ReadStatus { Missing, Ok, Empty, ReadError, InvalidUtf8 };
    struct DocumentRead {
        ReadStatus status = ReadStatus::Missing;
        std::string content;  // Ok:剥首尾空白后的全文
        std::string sha256;   // Ok:正文摘要
    };

    struct CachedDocument {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        std::string content;  // 剥空白后的正文
        std::string sha256;
    };

    static DocumentRead ReadOnDisk(const std::filesystem::path& path);
    DocumentRead LoadDocument(const std::filesystem::path& path) const;
    void CollectDirectory(const std::filesystem::path& dir, InstructionChain& chain) const;
    static void MakeDocument(InstructionChain& chain, const std::filesystem::path& file,
                             const std::filesystem::path& dir, bool is_override,
                             const DocumentRead& read);

    ProjectInstructionResolverOptions options_;
    mutable std::mutex cache_mutex_;
    mutable std::map<std::string, CachedDocument> cache_;
};

// 会话级装配口径(交互/单发同一只):fallback 名单从配置来(默认空),
// 全局层取 ~/.lubancode/AGENTS.md(主目录没有就是 nullopt,行为与从前
// 一字不差)。
ProjectInstructionResolverOptions SessionResolverOptions(
    const std::vector<std::string>& fallback_filenames = {});

// ---------------------------------------------------------------------------
// 展示面纯函数(P1-1,/instructions 与 /doctor instructions 同一份账):
// 只排版 chain 里已有的账,不碰文件系统、不泄正文(单子 §12.5"输出不
// 泄露文件正文;展开正文须另有明确操作")。
// ---------------------------------------------------------------------------

// /instructions 的账目表:根/目标/上限/逐文档行(类型、字节数、摘要前
// 8 位、nearest 标注)/合计/指纹/装载状态。诊断行另走
// FormatInstructionDiagnosticLines。
std::vector<std::string> FormatInstructionChainLines(const InstructionChain& chain,
                                                     std::size_t max_bytes);

// 诊断行(逐条 [code] 路径: 说明)。/instructions 与 /doctor instructions
// 共用;chain 无诊断时返回空。
std::vector<std::string> FormatInstructionDiagnosticLines(const InstructionChain& chain);

// 字节帽的计费口径说明(P1-7:文档、实现、诊断、测试同一套话):帽管
// "段间分隔 + 来源标题 + 正文"的合计;外层 "# Project Instructions" 与
// 固定说明(~100 B)在帽外另加——最终串可略超帽,超出量即这截包装。
std::string InstructionBudgetAccountingNote(std::size_t max_bytes);

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
