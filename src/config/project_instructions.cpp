#include "config/project_instructions.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

#include "config/config.hpp"              // HomeLubancodeDir:全局层的落点
#include "hooks/hash.hpp"                 // Sha256Hex:文档与链指纹的原料
#include "platform/paths.hpp"             // Utf8ToPath/PathToUtf8:路径串 <-> path
#include "platform/text_encoding.hpp"    // IsValidUtf8:坏编码拒收,不进提示词

namespace lubancode::config {

namespace {

std::filesystem::path AbsoluteNormal(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

// 路径的 UTF-8 写法:generic 形(正斜杠)。P0 起投影标题就用这份格式,
// 换成 native 形会让 Windows 上 "## Instructions from ..." 的字节变样,
// 破坏零退化——此处与展示面统一走 generic,别改回 native。
std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool RegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

// child 是否落在 root 里(含 root 自身)。两侧都过一遍 weakly_canonical
// 再按 generic 串比对(Windows 盘符大小写不敏感,小写后比):Windows 的
// 短名(C:\Users\MOONTI~1)与规范名(C:\Users\moontidef)指同一路径,只比
// 原始串会把项目内判成项目外——canonical 化把两边拉到同一写法。
bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& root) {
    std::error_code child_ec;
    std::error_code root_ec;
    const std::filesystem::path canonical_child = std::filesystem::weakly_canonical(child, child_ec);
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, root_ec);
    if (child_ec || root_ec) {
        return false;
    }
    std::string c = PathUtf8(canonical_child);
    std::string r = PathUtf8(canonical_root);
#ifdef _WIN32
    const auto lower = [](char ch) { return static_cast<char>(::tolower(static_cast<unsigned char>(ch))); };
    std::transform(c.begin(), c.end(), c.begin(), lower);
    std::transform(r.begin(), r.end(), r.begin(), lower);
#endif
    if (c == r) {
        return true;
    }
    if (c.size() <= r.size() || c.compare(0, r.size(), r) != 0) {
        return false;
    }
    const bool root_slash = !r.empty() && r.back() == '/';  // 盘根一类的 "D:/"
    return root_slash || c[r.size()] == '/';
}

std::string TrimInstructionText(const std::string& text) {
    std::string copy = text;
    copy.erase(std::remove(copy.begin(), copy.end(), '\r'), copy.end());
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    std::size_t begin = 0;
    while (begin < copy.size() && whitespace(copy[begin])) {
        ++begin;
    }
    std::size_t end = copy.size();
    while (end > begin && whitespace(copy[end - 1])) {
        --end;
    }
    return copy.substr(begin, end - begin);
}

std::size_t Utf8PrefixLength(const std::string& text, std::size_t limit) {
    std::size_t length = (std::min)(limit, text.size());
    if (length == text.size()) {
        return length;
    }
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}

std::vector<std::filesystem::path> DirectoriesRootToCwd(const std::filesystem::path& root,
                                                        const std::filesystem::path& cwd) {
    std::vector<std::filesystem::path> reversed;
    std::filesystem::path current = AbsoluteNormal(cwd);
    while (!current.empty()) {
        reversed.push_back(current);
        if (current == root) {
            break;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current || parent.empty()) {
            break;
        }
        current = parent;
    }
    if (reversed.empty() || reversed.back() != root) {
        return {root};
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

void Note(InstructionChain& chain, const std::filesystem::path& path, const char* code,
          const std::string& message) {
    InstructionDiagnostic note;
    note.path = path;
    note.code = code;
    note.message = message;
    chain.diagnostics.push_back(std::move(note));
}

// 链指纹:项目根 + 每份文档的(scope_dir, 正文摘要)依序入料。指纹认
// "哪几份规则管这个作用域",不认目标文件本身——同一最近 AGENTS.md 之下
// 的兄弟文件指纹相同(§7.4 按 fingerprint 分组的依据);文档内容一变,
// 指纹即变,旧确认作废。全局层也是链上的一份(scope_dir 即它的目录),
// 同一套料,不另开小灶。
std::string ChainFingerprint(const std::filesystem::path& root,
                             const std::vector<InstructionDocument>& docs) {
    std::string material;
    material += "root:" + PathUtf8(root) + "\n";
    for (const InstructionDocument& doc : docs) {
        material += "doc:" + PathUtf8(doc.scope_dir) + ":" + doc.sha256 + "\n";
    }
    return hooks::Sha256Hex(material);
}

// 别家工具的规则文件名(P2-3 迁移提示):只提示,不自动读。
constexpr const char* kMigrationFilenames[] = {"AGENT.md", "CLAUDE.md", "GEMINI.md"};

// stat 快筛的原料:size + mtime。stat 不动(文件没了/不让看)按 miss 走,
// 让调用方现读现报。
struct FileStamp {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
};

std::optional<FileStamp> StatFile(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return FileStamp{size, mtime};
}

// symlink 边界(单子 §10.3):链到项目内允许(账里记 link 与真实路径),
// 链到项目外拒读,断链/成环明报。scope 仍取 link 所在目录,hash 按真实
// 文件的内容计(读链自然读到目标,这里只审边界)。
enum class SymlinkVerdict { PlainFile, Allow, Deny };

SymlinkVerdict ScreenSymlink(const std::filesystem::path& file, const std::filesystem::path& root,
                             InstructionChain& chain) {
    std::error_code ec;
    if (!std::filesystem::is_symlink(file, ec) || ec) {
        return SymlinkVerdict::PlainFile;
    }
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(file, ec);
    if (ec || !RegularFile(resolved)) {
        Note(chain, file, "symlink_broken",
             std::string("符号链接断链或成环,读不动") +
                 (ec ? "(错误: " + ec.message() + ")" : "(解析到: " + PathUtf8(resolved) + ")"));
        return SymlinkVerdict::Deny;
    }
    if (!IsWithin(resolved, root)) {
        Note(chain, file, "symlink_outside_project",
             "符号链接解析到项目外(" + PathUtf8(resolved) +
                 "),拒读——指令文档不得把项目外的正文拉进来");
        return SymlinkVerdict::Deny;
    }
    Note(chain, file, "symlink_inside_project",
         "符号链接解析到项目内: " + PathUtf8(resolved) + "(正文按真实文件计,作用域按 link 所在目录计)");
    return SymlinkVerdict::Allow;
}

std::string BuildScaffold(const std::filesystem::path& root) {
    std::vector<std::string> build;
    std::vector<std::string> test;
    bool commands_are_shell = true;
    if (RegularFile(root / "CMakeLists.txt")) {
        build = {"cmake -S . -B build", "cmake --build build"};
        test = {"ctest --test-dir build --output-on-failure"};
    } else if (RegularFile(root / "package.json")) {
        build = {"npm install", "npm run build"};
        test = {"npm test"};
    } else if (RegularFile(root / "Cargo.toml")) {
        build = {"cargo build"};
        test = {"cargo test"};
    } else if (RegularFile(root / "go.mod")) {
        build = {"go build ./..."};
        test = {"go test ./..."};
    } else if (RegularFile(root / "pyproject.toml") || RegularFile(root / "pytest.ini")) {
        build = {"python -m pip install -e ."};
        test = {"python -m pytest"};
    } else if (RegularFile(root / "Makefile") || RegularFile(root / "makefile")) {
        build = {"make"};
        test = {"make test"};
    } else {
        commands_are_shell = false;
        build = {"Read `README.md` and repository configuration, then record the supported build command here."};
        test = {"Run the narrowest relevant test first, then the full suite before handoff."};
    }

    std::ostringstream out;
    out << "# Repository Guidelines\n\n"
        << "## Project Layout\n\n"
        << "- Read `README.md` and nearby source files before changing code.\n"
        << "- Keep changes inside the module that owns the behavior.\n"
        << "- Do not edit generated files unless the repository documents that workflow.\n\n"
        << "## Build and Test\n\n";
    for (const std::string& command : build) {
        out << "- Build: " << (commands_are_shell ? "`" : "") << command
            << (commands_are_shell ? "`" : "") << "\n";
    }
    for (const std::string& command : test) {
        out << "- Test: " << (commands_are_shell ? "`" : "") << command
            << (commands_are_shell ? "`" : "") << "\n";
    }
    out << "\n## Working Agreements\n\n"
        << "- Preserve existing style and public behavior unless the task calls for a change.\n"
        << "- Keep patches focused. Leave unrelated user changes in place.\n"
        << "- Add or update tests when behavior changes.\n"
        << "- Report the commands run and any checks that could not be completed.\n";
    return out.str();
}

}  // namespace

// 项目根发现:头文件里导出(自定义 Agent 单阶段 1 起,Agent Catalog 的
// 项目层也从这里起算),实现原样。
std::filesystem::path FindProjectRoot(const std::filesystem::path& cwd) {
    std::filesystem::path current = AbsoluteNormal(cwd);
    std::error_code ec;
    if (!std::filesystem::is_directory(current, ec)) {
        current = current.parent_path();
    }
    const std::filesystem::path fallback = current;
    while (!current.empty()) {
        ec.clear();
        if (std::filesystem::exists(current / ".git", ec) && !ec) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current || parent.empty()) {
            break;
        }
        current = parent;
    }
    return fallback;
}

ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd, std::size_t max_bytes) {
    // 零退化投影(AGENTS.md 作用域单 P0):字符串 loader 不再自持解析逻辑,
    // 同一只 Resolver 出账。root->cwd 的目录序、同层 override、空文件跳过、
    // 32 KiB 帽与拼接格式逐字节照旧——既有测试(unit.config.project_
    // instructions)钉的就是这份投影。全局层与 fallback 名单是会话级装配
    // 才喂的口径(SessionResolverOptions),这条旧口不带,行为与从前一致。
    const InstructionChain chain = ProjectInstructionResolver(max_bytes).ResolveForPath(cwd);
    ProjectInstructions result;
    result.project_root = chain.project_root;
    result.sources = chain.sources;
    result.content = chain.content;
    result.truncated = chain.truncated;
    return result;
}

// ---------------------------------------------------------------------------
// 结构化 Resolver(单子 §六/§七 P0;P1 起带缓存与分型诊断)
// ---------------------------------------------------------------------------

ProjectInstructionResolver::ProjectInstructionResolver(std::size_t max_bytes) {
    options_.max_bytes = max_bytes;
}

ProjectInstructionResolver::ProjectInstructionResolver(ProjectInstructionResolverOptions options)
    : options_(std::move(options)) {}

std::size_t ProjectInstructionResolver::cached_documents() const {
    const std::lock_guard<std::mutex> lock(cache_mutex_);
    return cache_.size();
}

// seam 缺省的实读:二进制全文。打不开/读坏如实报,不装成空文件。
ProjectInstructionResolver::DocumentRead ProjectInstructionResolver::ReadOnDisk(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {ReadStatus::ReadError, std::string(), std::string()};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        return {ReadStatus::ReadError, std::string(), std::string()};
    }
    const std::string text = buffer.str();
    if (!lubancode::platform::IsValidUtf8(text)) {
        return {ReadStatus::InvalidUtf8, std::string(), std::string()};
    }
    const std::string trimmed = TrimInstructionText(text);
    if (trimmed.empty()) {
        return {ReadStatus::Empty, std::string(), std::string()};
    }
    return {ReadStatus::Ok, trimmed, hooks::Sha256Hex(trimmed)};
}

// 带缓存的文档读取(P1-5):path + size + mtime 快筛——stat 未变直接用
// 缓存的正文与摘要;stat 一变(或没缓存)现读。只缓存成功(Ok)的读,
// 空文件/坏编码/读错每次现读现报,不把一时之错钉死在缓存里。外部编辑
// 的惰性发现(P1-6)就落在这:下一次 Resolve 的 stat 必然看见 mtime 变了。
ProjectInstructionResolver::DocumentRead ProjectInstructionResolver::LoadDocument(
    const std::filesystem::path& path) const {
    const std::string key = PathUtf8(path);
    const std::optional<FileStamp> stamp = StatFile(path);
    if (stamp.has_value()) {
        const std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end() && it->second.size == stamp->size && it->second.mtime == stamp->mtime) {
            return {ReadStatus::Ok, it->second.content, it->second.sha256};
        }
    }
    DocumentRead read;
    if (options_.file_reader) {
        const std::optional<std::string> raw = options_.file_reader(path);
        if (!raw.has_value()) {
            read.status = ReadStatus::ReadError;
        } else if (!lubancode::platform::IsValidUtf8(*raw)) {
            read.status = ReadStatus::InvalidUtf8;
        } else {
            const std::string trimmed = TrimInstructionText(*raw);
            if (trimmed.empty()) {
                read.status = ReadStatus::Empty;
            } else {
                read.status = ReadStatus::Ok;
                read.content = trimmed;
                read.sha256 = hooks::Sha256Hex(trimmed);
            }
        }
    } else {
        read = ReadOnDisk(path);
    }
    if (read.status == ReadStatus::Ok && stamp.has_value()) {
        const std::lock_guard<std::mutex> lock(cache_mutex_);
        CachedDocument entry;
        entry.size = stamp->size;
        entry.mtime = stamp->mtime;
        entry.content = read.content;
        entry.sha256 = read.sha256;
        cache_[key] = std::move(entry);
    }
    return read;
}

void ProjectInstructionResolver::MakeDocument(InstructionChain& chain,
                                              const std::filesystem::path& file,
                                              const std::filesystem::path& dir, bool is_override,
                                              const DocumentRead& read) {
    InstructionDocument doc;
    doc.source_path = file;
    doc.scope_dir = dir;
    doc.is_override = is_override;
    doc.content = read.content;
    doc.sha256 = read.sha256;
    doc.bytes = read.content.size();
    chain.documents.push_back(std::move(doc));
}

// 一层目录的文档选举:非空 override 压过非空 AGENTS.md,两枚主名都没
// 命中(不在/读错/空)才轮到 fallback 名单(P2-2,按序取第一份非空)。
// 诊断分型(P1-3)在这里记:read_error/invalid_utf8/empty_skipped/
// shadowed/fallback_used;迁移提示(P2-3)顺手一并扫。symlink 越界/断链
// 的账 ScreenSymlink 已记,这里只把该文件按"不参选"处理。
void ProjectInstructionResolver::CollectDirectory(const std::filesystem::path& dir,
                                                  InstructionChain& chain) const {
    const std::filesystem::path override_file = dir / "AGENTS.override.md";
    const std::filesystem::path agents_file = dir / "AGENTS.md";

    // 读一枚候选:在 → symlink 边界 → 内容。返回有没有这份文件。
    // 断链要单独审:is_regular_file 跟链走,目标不在时它只说"没有这份",
    // 会把断链静默降成"没发现"——单子 §10.3 明令断链要明报。
    const auto probe = [&chain, this](const std::filesystem::path& file, DocumentRead& out) {
        out = DocumentRead{};
        std::error_code link_ec;
        const bool is_link = std::filesystem::is_symlink(file, link_ec) && !link_ec;
        if (!is_link && !RegularFile(file)) {
            return false;  // 普通不在:不参选也不记错
        }
        if (is_link) {
            if (ScreenSymlink(file, chain.project_root, chain) == SymlinkVerdict::Deny) {
                return true;  // 链在,但越界/断链被审拒——不参选,账已记
            }
        }
        out = LoadDocument(file);
        if (out.status == ReadStatus::ReadError) {
            Note(chain, file, "read_error", "文件在却读不动(权限/短暂 I/O 错),跳过并往后选");
        } else if (out.status == ReadStatus::InvalidUtf8) {
            Note(chain, file, "invalid_utf8", "内容不是合法 UTF-8,拒收(不进提示词)");
        }
        return true;
    };

    DocumentRead override_read;
    const bool has_override = probe(override_file, override_read);
    const bool override_ok = override_read.status == ReadStatus::Ok;
    DocumentRead agents_read;
    const bool has_agents = probe(agents_file, agents_read);

    if (has_override && override_read.status == ReadStatus::Empty) {
        Note(chain, override_file, "empty_skipped", "AGENTS.override.md 为空,跳过并回落同层 AGENTS.md");
    }
    if (override_ok && agents_read.status == ReadStatus::Ok) {
        Note(chain, agents_file, "shadowed_same_directory",
             "同层存在非空 AGENTS.override.md,这份 AGENTS.md 被遮蔽");
    }
    if (has_agents && agents_read.status == ReadStatus::Empty) {
        Note(chain, agents_file, "empty_skipped", "AGENTS.md 为空,跳过");
    }

    if (override_ok) {
        MakeDocument(chain, override_file, dir, /*is_override=*/true, override_read);
    } else if (agents_read.status == ReadStatus::Ok) {
        MakeDocument(chain, agents_file, dir, /*is_override=*/false, agents_read);
    } else {
        for (const std::string& name : options_.fallback_filenames) {
            if (name.empty() || name == "AGENTS.md" || name == "AGENTS.override.md") {
                continue;  // 主名不进 fallback 名单(解析层已拒,这里再守一道)
            }
            const std::filesystem::path fallback_file = dir / name;
            DocumentRead fallback_read;
            if (!probe(fallback_file, fallback_read)) {
                continue;
            }
            if (fallback_read.status == ReadStatus::Empty) {
                Note(chain, fallback_file, "empty_skipped", "fallback 文件为空,跳过并看名单下一份");
                continue;
            }
            if (fallback_read.status != ReadStatus::Ok) {
                continue;  // 读错/坏编码的账 probe 已记;名单看下一份
            }
            MakeDocument(chain, fallback_file, dir, /*is_override=*/false, fallback_read);
            chain.documents.back().is_fallback = true;
            Note(chain, fallback_file, "fallback_used",
                 "本层 override/AGENTS.md 都没命中,取显式配置的 fallback: " + name);
            break;
        }
    }

    // 迁移提示(P2-3):发现别家工具的规则文件只提示,不自动读——默认把
    // 四五套规则全拼进提示词只会平白制造冲突,要不要迁移由用户定。
    for (const char* name : kMigrationFilenames) {
        const std::filesystem::path foreign = dir / name;
        if (RegularFile(foreign)) {
            Note(chain, foreign, "migration_hint",
                 std::string("发现 ") + name +
                     "——这是别家工具的规则文件,LubanCode 只读 AGENTS.md / AGENTS.override.md"
                     "(或显式配置的 fallback 名单),不自动多读;要迁移请把规矩并进 AGENTS.md");
        }
    }
}

InstructionChain ProjectInstructionResolver::ResolveForPath(const std::filesystem::path& target) const {
    InstructionChain chain;
    // 目标归一:文件取父目录起链,目录取自身(§12.1 两种输入)。
    std::error_code ec;
    std::filesystem::path anchor = AbsoluteNormal(target);
    if (!std::filesystem::is_directory(anchor, ec)) {
        anchor = anchor.parent_path();
    }
    chain.target_path = AbsoluteNormal(target);
    chain.project_root = FindProjectRoot(anchor);

    // 每层选文档:非空 AGENTS.override.md 压过非空 AGENTS.md,主名没命中
    // 再看 fallback 名单——与旧 loader 同一张机械表,顺带记分型诊断。
    for (const std::filesystem::path& dir : DirectoriesRootToCwd(chain.project_root, anchor)) {
        CollectDirectory(dir, chain);
    }

    // 全局层(P2-1):~/.lubancode/AGENTS.md 存在且非空时,作为优先级最低
    // 的一层垫在最前。分工口径(单子 P2 评估,详见 todo):system prompt
    // 管身份与协议,SOUL 管口吻风格,全局 AGENTS 管跨仓库的工作法——它
    // 压在项目根规矩之下,项目层永远能盖过它。文件不在/为空/坏掉:零层,
    // 行为与从前一字不差(默认构造与旧口 LoadProjectInstructions 都不带
    // 全局层,既有单测不受主目录里有没有这份文件影响)。
    if (options_.global_instructions_path.has_value()) {
        const std::filesystem::path global_file = *options_.global_instructions_path;
        if (RegularFile(global_file)) {
            const std::filesystem::path global_scope = global_file.parent_path();
            if (ScreenSymlink(global_file, global_scope, chain) != SymlinkVerdict::Deny) {
                const DocumentRead read = LoadDocument(global_file);
                if (read.status == ReadStatus::Ok) {
                    InstructionDocument doc;
                    doc.source_path = global_file;
                    doc.scope_dir = global_scope;
                    doc.is_global = true;
                    doc.content = read.content;
                    doc.sha256 = read.sha256;
                    doc.bytes = read.content.size();
                    chain.documents.insert(chain.documents.begin(), std::move(doc));
                } else if (read.status == ReadStatus::ReadError) {
                    Note(chain, global_file, "read_error", "全局指令文件在却读不动,跳过");
                } else if (read.status == ReadStatus::InvalidUtf8) {
                    Note(chain, global_file, "invalid_utf8", "全局指令文件不是合法 UTF-8,拒收");
                }
            }
        }
    }

    chain.fingerprint = ChainFingerprint(chain.project_root, chain.documents);

    // 旧口径的 content 投影:格式、字节帽、截断点与从前一字不差(单测
    // unit.config.project_instructions 的四条用例钉住这层兼容)。P1 只在
    // 帽外多记两笔账:哪份被腰斩(truncated)、哪些整份没装下
    //(dropped_for_budget + over_budget 诊断)——不再静默。
    std::size_t fully_appended = 0;
    if (options_.max_bytes == 0) {
        chain.truncated = true;  // 与旧 loader 同口径:零预算即视作截断
    } else {
        for (const InstructionDocument& doc : chain.documents) {
            const std::string heading = "## Instructions from " + PathUtf8(doc.source_path) + "\n\n";
            const std::string separator = chain.content.empty() ? std::string() : "\n\n";
            const std::size_t needed = separator.size() + heading.size() + doc.content.size();
            if (chain.content.size() + needed > options_.max_bytes) {
                const std::size_t remaining =
                    options_.max_bytes > chain.content.size() ? options_.max_bytes - chain.content.size() : 0;
                const std::string combined = separator + heading + doc.content;
                chain.content.append(combined, 0, Utf8PrefixLength(combined, remaining));
                chain.truncated = true;
                chain.sources.push_back(doc.source_path);
                break;
            }
            chain.content += separator + heading + doc.content;
            chain.sources.push_back(doc.source_path);
            ++fully_appended;
        }
    }
    if (chain.truncated && !chain.documents.empty()) {
        for (std::size_t i = fully_appended + 1; i < chain.documents.size(); ++i) {
            chain.dropped_for_budget.push_back(chain.documents[i].source_path);
        }
        std::string detail = PathUtf8(chain.documents[fully_appended].source_path) + " 在帽内被腰斩";
        for (const std::filesystem::path& dropped : chain.dropped_for_budget) {
            detail += ";" + PathUtf8(dropped) + " 整份未装";
        }
        Note(chain, chain.documents[fully_appended].source_path, "over_budget",
             "拼装投影撞了字节帽(" + std::to_string(options_.max_bytes) + " bytes):" + detail +
                 ";细账见 /instructions");
    }
    if (!chain.content.empty()) {
        chain.content = "# Project Instructions\n\n"
                        "Follow these repository instructions. Files nearer the working directory take precedence.\n\n" +
                        chain.content;
    }
    return chain;
}

// ---------------------------------------------------------------------------
// 展示面纯函数(P1-1):/instructions 与 /doctor instructions 的同一份账。
// 只排版 chain 已有的账,不碰文件系统、不泄正文。
// ---------------------------------------------------------------------------

std::vector<std::string> FormatInstructionChainLines(const InstructionChain& chain,
                                                     std::size_t max_bytes) {
    std::vector<std::string> lines;
    lines.push_back("项目根: " + PathUtf8(chain.project_root));
    lines.push_back("目标: " + PathUtf8(chain.target_path));
    lines.push_back("上限: " + std::to_string(max_bytes) + " bytes");
    if (chain.documents.empty()) {
        lines.push_back("已加载: (无——链上没有任何指令文档,写入不过作用域闸)");
    } else {
        lines.push_back("已加载(父目录在前,离目标最近的在最后、优先级最高):");
        std::size_t total = 0;
        // nearest 标注给最后一份"项目内"文档;全局层垫在最前,永不当 nearest。
        std::size_t nearest_index = chain.documents.size();
        for (std::size_t i = chain.documents.size(); i-- > 0;) {
            if (!chain.documents[i].is_global) {
                nearest_index = i;
                break;
            }
        }
        for (std::size_t i = 0; i < chain.documents.size(); ++i) {
            const InstructionDocument& doc = chain.documents[i];
            total += doc.bytes;
            const char* kind = doc.is_global    ? "GLOBAL"
                               : doc.is_override ? "OVERRIDE"
                               : doc.is_fallback ? "FALLBACK"
                                                 : "AGENTS";
            std::string line = "  " + std::to_string(i + 1) + ". " + PathUtf8(doc.source_path) +
                               "  [" + kind + "]  " + std::to_string(doc.bytes) + " B  sha256:" +
                               doc.sha256.substr(0, 8);
            if (i == nearest_index) {
                line += "  <- 离目标最近";
            }
            lines.push_back(std::move(line));
        }
        lines.push_back("合计: " + std::to_string(total) + " B(正文,不含标题包装)");
        lines.push_back("指纹: " + chain.fingerprint.substr(0, 16));
    }
    if (chain.truncated) {
        std::string detail = "预算内装不下全部文档";
        if (!chain.sources.empty()) {
            detail = PathUtf8(chain.sources.back()) + " 在帽内被腰斩";
            for (const std::filesystem::path& dropped : chain.dropped_for_budget) {
                detail += ";" + PathUtf8(dropped) + " 整份未装";
            }
        }
        lines.push_back("状态: 截断(不完整)—— " + detail +
                        "。基线投影只装得下这么多,不得当作\"全部已加载\";"
                          "写前闸按整份文档计,装不下的链直接拒写(见 /doctor instructions)");
    } else {
        lines.push_back("状态: 完整");
    }
    return lines;
}

std::vector<std::string> FormatInstructionDiagnosticLines(const InstructionChain& chain) {
    std::vector<std::string> lines;
    for (const InstructionDiagnostic& note : chain.diagnostics) {
        lines.push_back("  - [" + note.code + "] " + PathUtf8(note.path) + ": " + note.message);
    }
    return lines;
}

std::string InstructionBudgetAccountingNote(std::size_t max_bytes) {
    return "计费口径: 上限 " + std::to_string(max_bytes) +
           " bytes 管\"段间分隔 + 来源标题(## Instructions from ...) + 正文\"的合计;"
           "拼完后再在最前头加 \"# Project Instructions\" 与一行固定说明(约 100 bytes)——这截包装不计入帽,"
           "最终串可略超上限,超出量即包装本身。系统提示里的基线投影沿这条旧口径(保零退化,超限标\"截断\"并分账);"
           "写前闸(active write chain)另按\"整份文档\"计:链在预算内装不下即拒写并明说,不腰斩。";
}

ProjectInstructionResolverOptions SessionResolverOptions(const std::vector<std::string>& fallback_filenames) {
    ProjectInstructionResolverOptions options;
    options.fallback_filenames = fallback_filenames;
    const std::optional<std::string> home = HomeLubancodeDir();
    if (home.has_value()) {
        options.global_instructions_path =
            lubancode::platform::Utf8ToPath(*home) / "AGENTS.md";
    }
    return options;
}

InitProjectInstructionsResult InitializeProjectInstructions(const std::filesystem::path& cwd) {
    InitProjectInstructionsResult result;
    const std::filesystem::path root = FindProjectRoot(cwd);
    const std::filesystem::path override_file = root / "AGENTS.override.md";
    const std::filesystem::path agents_file = root / "AGENTS.md";
    if (RegularFile(override_file)) {
        result.status = InitProjectInstructionsStatus::AlreadyExists;
        result.path = override_file;
        return result;
    }
    if (RegularFile(agents_file)) {
        result.status = InitProjectInstructionsStatus::AlreadyExists;
        result.path = agents_file;
        return result;
    }

    std::ofstream file(agents_file, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        result.path = agents_file;
        result.error = "cannot open file for writing";
        return result;
    }
    file << BuildScaffold(root);
    if (!file.good()) {
        result.path = agents_file;
        result.error = "failed while writing file";
        return result;
    }
    result.status = InitProjectInstructionsStatus::Created;
    result.path = agents_file;
    return result;
}

}  // namespace lubancode::config
