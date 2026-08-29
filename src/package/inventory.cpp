// 四层扫描与稳定盘点的实现(统一 Package 封装单阶段 1)。要点:
//   - 盘点只读,不写回源目录;
//   - 文件账按规范化的 UTF-8 相对路径排序,枚举次序不掺和;
//   - 内容哈希吃"路径+大小+逐文件 sha256",文件改一个字节整包哈希就变
//     (复用 hooks 的自含 SHA-256,它是 hook/plugin 信任链的锚,同一份);
//   - symlink/junction 一律记账不进哈希(包根只读、账要完整——发现可疑
//     不等于执行,但必须看得见)。
#include "package/inventory.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "hooks/hash.hpp"
#include "platform/paths.hpp"

namespace lubancode::package {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

// 包内相对路径(UTF-8、'/' 分隔、已规范化)。用纯词法的 lexically_relative
// 而不是 relative():后者走 weakly_canonical,会顺藤解析 symlink,越界软链
// 会算出怪路径;这里枚举自包根,前缀必然匹配,词法相对就足够且更便宜。
std::string RelUtf8(const std::filesystem::path& root, const std::filesystem::path& file) {
    const std::u8string u8 = file.lexically_relative(root).generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::optional<std::string> ReadFileBytes(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool HasEntry(const std::filesystem::path& dir, const char* name) {
    std::error_code ec;
    const std::filesystem::path candidate = dir / name;
    return std::filesystem::exists(candidate, ec) && !ec;
}

// code-bearing 的扩展名迹象(单子 §9.2 静态版):二进制库、可执行、脚本。
// plugins/ 与 mcp/ 目录下的文件不问扩展名全算(组件本体),这里管的是散在
// 别处的可执行迹象。
bool HasCodeExtension(const std::string& rel_path) {
    static const char* kExts[] = {".dll",  ".so",   ".dylib", ".exe", ".lua",  ".py",
                                  ".js",   ".mjs",  ".cmd",   ".bat", ".ps1",  ".sh"};
    const std::size_t dot = rel_path.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = rel_path.substr(dot);
    for (const char* candidate : kExts) {
        if (ext == candidate) return true;
    }
    return false;
}

// Levenshtein(小写化后比)。近似目录名检测用,量小(O(n*m),n,m<=64)。
std::size_t EditDistance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            const std::size_t del = prev[j] + 1;
            const std::size_t ins = cur[j - 1] + 1;
            cur[j] = std::min({sub, del, ins});
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// 可疑链接:真 symlink 走 is_symlink;Windows 的 junction(reparse point)
// 在 MSVC STL 里不算 symlink,得拿文件属性单独认——junction 与 symlink
// 同样能把账指到包外,一律当越界软链报。
bool IsSuspiciousLink(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec) return false;  // 状态读不动:不算链接,后续自然报读不动
    if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;  // junction / mount point / 其它 reparse point
    }
#endif
    return false;
}

}  // namespace

std::vector<std::string> StandardComponentDirs() {
    return {"agents", "prompts", "skills", "workflows", "plugins", "mcp"};
}

std::vector<std::string> ReservedTopLevelNames() {
    return {"agents", "prompts", "skills", "workflows", "plugins", "mcp", "assets", "docs",
            "package.yaml", "README.md", "LICENSE"};
}

int ScopePrecedence(PackageScope scope) {
    switch (scope) {
        case PackageScope::Dev: return 3;
        case PackageScope::Project: return 2;
        case PackageScope::User: return 1;
        case PackageScope::Official: return 0;
    }
    return 0;
}

std::string ScopeToString(PackageScope scope) {
    switch (scope) {
        case PackageScope::Dev: return "dev";
        case PackageScope::Project: return "project";
        case PackageScope::User: return "user";
        case PackageScope::Official: return "official";
    }
    return "?";
}

std::string PackageDiagnostic::Format() const {
    std::string out;
    switch (kind) {
        case Kind::Error: out += "[error] "; break;
        case Kind::Warning: out += "[warn]  "; break;
        case Kind::Info: out += "[info]  "; break;
    }
    if (!path.empty()) {
        out += path;
        out += ": ";
    }
    out += message;
    return out;
}

// ---------------------------------------------------------------------------
// 四层扫描
// ---------------------------------------------------------------------------

namespace {

struct LayerSpec {
    PackageScope scope;
    std::optional<std::filesystem::path> root;
};

// 扫一层:直接子目录排序后逐只看 package.yaml。有清单就解析,解析失败或
// 缺清单都记账——"放进目录 -> 立刻被发现"之后,"格式无错 -> 立刻可列可
// 诊"靠的就是这份账(单子 §九)。
void ScanLayer(const LayerSpec& layer, std::vector<PackageCandidate>& out) {
    if (!layer.root.has_value()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(*layer.root, ec) || ec) return;

    std::vector<std::filesystem::path> dirs;
    for (const auto& entry : std::filesystem::directory_iterator(*layer.root, ec)) {
        if (ec) break;
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec) && !entry_ec) dirs.push_back(entry.path());
        entry_ec.clear();
    }
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return PathToUtf8(a) < PathToUtf8(b);
    });

    for (const auto& dir : dirs) {
        PackageCandidate candidate;
        candidate.scope = layer.scope;
        candidate.layer_root = *layer.root;
        candidate.package_root = dir;
        const std::u8string name_u8 = dir.filename().u8string();
        candidate.dir_name = std::string(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());

        const std::filesystem::path manifest_path = dir / "package.yaml";
        std::error_code read_ec;
        if (std::filesystem::is_regular_file(manifest_path, read_ec) && !read_ec) {
            const auto text = ReadFileBytes(manifest_path);
            if (!text.has_value()) {
                candidate.manifest_error =
                    ManifestError{"package.yaml", 0, "读不动(权限或占用)"};
            } else {
                auto parsed = ParsePackageManifest(*text);
                if (parsed.has_value()) {
                    candidate.manifest = std::move(*parsed);
                } else {
                    candidate.manifest_error = std::move(parsed.error());
                }
            }
        } else {
            candidate.manifest_error =
                ManifestError{"package.yaml", 0, "缺 package.yaml(包根必须有根清单)"};
        }
        out.push_back(std::move(candidate));
    }
}

}  // namespace

std::vector<PackageCandidate> ScanPackages(const ScanOptions& options) {
    std::vector<PackageCandidate> out;
    // 从低到低扫,最后倒序排:优先级高的排前。
    ScanLayer(LayerSpec{PackageScope::Official, options.official_root}, out);
    ScanLayer(LayerSpec{PackageScope::User, options.user_root}, out);
    ScanLayer(LayerSpec{PackageScope::Project, options.project_root}, out);
    for (const auto& dev : options.dev_roots) {
        ScanLayer(LayerSpec{PackageScope::Dev, dev}, out);
    }
    std::stable_sort(out.begin(), out.end(), [](const PackageCandidate& a, const PackageCandidate& b) {
        return ScopePrecedence(a.scope) > ScopePrecedence(b.scope);
    });
    return out;
}

// ---------------------------------------------------------------------------
// 路径安全
// ---------------------------------------------------------------------------

std::string_view PathIssueText(PathIssue issue) {
    switch (issue) {
        case PathIssue::None: return "没问题";
        case PathIssue::Empty: return "空路径";
        case PathIssue::Absolute: return "绝对路径(不许越出包根)";
        case PathIssue::ParentEscape: return "含 .. 段(不许越出包根)";
    }
    return "?";
}

PathIssue CheckPackageRelativePath(std::string_view rel_utf8) {
    if (rel_utf8.empty()) return PathIssue::Empty;
    // POSIX 绝对路径与 Windows 盘符/UNC。
    if (rel_utf8.front() == '/' || rel_utf8.front() == '\\') return PathIssue::Absolute;
    if (rel_utf8.size() >= 2 && rel_utf8[1] == ':' &&
        ((rel_utf8[0] >= 'a' && rel_utf8[0] <= 'z') || (rel_utf8[0] >= 'A' && rel_utf8[0] <= 'Z'))) {
        return PathIssue::Absolute;
    }
    // 按 '/' 与 '\' 切段。'.' 段(当前目录)放行,无害;'..' 段越界。
    std::size_t begin = 0;
    while (begin <= rel_utf8.size()) {
        const std::size_t end = rel_utf8.find_first_of("/\\", begin);
        const std::size_t stop = end == std::string_view::npos ? rel_utf8.size() : end;
        const std::string_view seg = rel_utf8.substr(begin, stop - begin);
        if (seg == "..") return PathIssue::ParentEscape;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return PathIssue::None;
}

std::string NearMissStandardDir(std::string_view dir_name) {
    if (dir_name.empty()) return {};
    const std::string lower = ToLowerAscii(dir_name);
    for (const std::string& reserved : ReservedTopLevelNames()) {
        if (lower == reserved) return {};  // 保留名单本身就是合法名
    }
    std::string best;
    std::size_t best_distance = 3;  // 距离 > 2 不报
    for (const std::string& candidate :
         {"agents", "prompts", "skills", "workflows", "plugins", "mcp", "assets", "docs"}) {
        const std::size_t distance = EditDistance(lower, candidate);
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// 盘点
// ---------------------------------------------------------------------------

namespace {

// 一枚盘点文件:规范化的相对路径 + 大小 + 逐文件哈希。
struct InventoryFile {
    std::string rel;
    std::uintmax_t size = 0;
    std::string hash;  // 空 = 读不动
    bool code_bearing = false;
};

PackageComponent MakeComponent(const std::string& package_id, const std::string& local_id,
                               const std::string& rel_path) {
    PackageComponent component;
    component.local_id = local_id;
    component.canonical_id = package_id + ":" + local_id;
    component.rel_path = rel_path;
    return component;
}

void CollectFiles(const std::filesystem::path& root, std::vector<InventoryFile>& files,
                  std::vector<PackageDiagnostic>& diagnostics) {
    std::error_code ec;
    // 包根本身是链接:整只包可疑,先记账再继续(账要完整)。
    if (IsSuspiciousLink(root)) {
        diagnostics.push_back(
            {PackageDiagnostic::Kind::Error, "", "包根本身是符号链接/junction(可能越出包根)"});
    }
    // follow_directory_symlink 不开:symlink 目录不递归,只当一枚可疑项记账。
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec);
    if (ec) {
        diagnostics.push_back(
            {PackageDiagnostic::Kind::Error, "", "包根遍历不动: " + ec.message()});
        return;
    }
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        const std::filesystem::path current = it->path();
        std::error_code entry_ec;
        // symlink 与 Windows junction(reparse point)都从这里现形:报 Error,
        // 不进文件账、不进哈希——信任账锚的是真实文件,软链可以指到包外。
        if (IsSuspiciousLink(current)) {
            diagnostics.push_back({PackageDiagnostic::Kind::Error, RelUtf8(root, current),
                                   "符号链接/junction 不许进包(可能越出包根)"});
            it.disable_recursion_pending();
            it.increment(entry_ec);
            continue;
        }
        const std::filesystem::file_status status = it->symlink_status(entry_ec);
        if (entry_ec) {
            diagnostics.push_back({PackageDiagnostic::Kind::Warning, RelUtf8(root, current),
                                   "状态读不动: " + entry_ec.message()});
            it.increment(entry_ec);
            continue;
        }
        if (status.type() == std::filesystem::file_type::regular) {
            InventoryFile file;
            file.rel = RelUtf8(root, current);
            file.size = it->file_size(entry_ec);
            if (entry_ec) {
                file.size = 0;
                entry_ec.clear();
            }
            file.code_bearing = HasCodeExtension(file.rel) || file.rel.rfind("plugins/", 0) == 0 ||
                                file.rel.rfind("mcp/", 0) == 0;
            files.push_back(std::move(file));
        } else if (status.type() != std::filesystem::file_type::directory) {
            diagnostics.push_back({PackageDiagnostic::Kind::Warning, RelUtf8(root, current),
                                   "非常规文件(fifo/socket/设备),不进盘点"});
        }
        it.increment(entry_ec);
        if (entry_ec) break;
    }
    std::sort(files.begin(), files.end(),
              [](const InventoryFile& a, const InventoryFile& b) { return a.rel < b.rel; });
}

}  // namespace

// 六类组件目录的盘点(包级导出:引用索引与 MountPlan 也吃这份轻账)。
// 规矩(单子 §四):目录必须在包根;一件 Skill/Workflow/Plugin/MCP 各占
// 一层目录,入口文件各有其名;Agent 是 agents/*.yaml;Prompt Profile 沿用
// prompts/profiles/<profile>/。入口文件缺了不吞——进诊断账。
std::vector<PackageComponent> ListPackageComponents(const std::filesystem::path& root,
                                                    const std::string& package_id,
                                                    std::vector<PackageDiagnostic>* diagnostics) {
    std::vector<PackageComponent> skills, workflows, plugins, mcp_servers, agents, profiles;

    const auto scan_dir_components = [&](const char* dir_name, const char* entry_file,
                                         std::vector<PackageComponent>& out,
                                         bool entry_required) {
        const std::filesystem::path dir = root / dir_name;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) || ec) return;
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            children.push_back(entry.path());
        }
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
            return PathToUtf8(a) < PathToUtf8(b);
        });
        for (const auto& child : children) {
            std::error_code child_ec;
            if (!std::filesystem::is_directory(child, child_ec) || child_ec) continue;
            const std::u8string name_u8 = child.filename().u8string();
            const std::string name(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());
            const std::string rel = std::string(dir_name) + "/" + name;
            if (HasEntry(child, entry_file)) {
                out.push_back(MakeComponent(package_id, name, rel));
            } else if (entry_required && diagnostics != nullptr) {
                diagnostics->push_back(
                    {PackageDiagnostic::Kind::Warning, rel,
                     std::string("缺入口文件 ") + entry_file + ",组件不成立"});
            }
        }
    };
    scan_dir_components("skills", "SKILL.md", skills, true);
    scan_dir_components("workflows", "workflow.yaml", workflows, true);
    scan_dir_components("plugins", "plugin.json", plugins, true);
    scan_dir_components("mcp", "mcp.yaml", mcp_servers, true);

    // agents/*.yaml:根下的一层文件即一份 Agent 定义(单子 §四)。
    {
        const std::filesystem::path dir = root / "agents";
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec) && !ec) {
            std::vector<std::string> names;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                std::error_code entry_ec;
                if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
                const std::string rel = RelUtf8(root, entry.path());
                if (!rel.ends_with(".yaml")) continue;
                std::string name = rel.substr(strlen("agents/"));
                const std::size_t dot = name.rfind('.');
                if (dot != std::string::npos) name = name.substr(0, dot);
                if (name.empty()) continue;
                names.push_back(name);
            }
            std::sort(names.begin(), names.end());
            for (const std::string& name : names) {
                agents.push_back(MakeComponent(package_id, name, "agents/" + name + ".yaml"));
            }
        }
    }

    // prompts/profiles/<profile>/:目录名即 profile 名。
    {
        const std::filesystem::path dir = root / "prompts" / "profiles";
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec) && !ec) {
            std::vector<std::string> names;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                std::error_code entry_ec;
                if (!entry.is_directory(entry_ec) || entry_ec) continue;
                const std::u8string name_u8 = entry.path().filename().u8string();
                names.emplace_back(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());
            }
            std::sort(names.begin(), names.end());
            for (const std::string& name : names) {
                profiles.push_back(MakeComponent(package_id, name, "prompts/profiles/" + name));
            }
        }
    }

    // 并成一份账,次序固定:agents, prompt_profiles, skills, workflows,
    // plugins, mcp_servers(与 PackageInventory 的字段序一致)。
    std::vector<PackageComponent> out;
    out.reserve(agents.size() + profiles.size() + skills.size() + workflows.size() +
                plugins.size() + mcp_servers.size());
    out.insert(out.end(), agents.begin(), agents.end());
    out.insert(out.end(), profiles.begin(), profiles.end());
    out.insert(out.end(), skills.begin(), skills.end());
    out.insert(out.end(), workflows.begin(), workflows.end());
    out.insert(out.end(), plugins.begin(), plugins.end());
    out.insert(out.end(), mcp_servers.begin(), mcp_servers.end());
    return out;
}

namespace {

void CollectComponents(const std::filesystem::path& root, const std::string& package_id,
                       PackageInventory& inventory) {
    std::vector<PackageComponent> listed = ListPackageComponents(root, package_id,
                                                                 &inventory.diagnostics);
    for (auto& component : listed) {
        const std::string& prefix = component.rel_path.substr(0, component.rel_path.find('/'));
        if (prefix == "agents") {
            inventory.agents.push_back(std::move(component));
        } else if (prefix == "prompts") {
            inventory.prompt_profiles.push_back(std::move(component));
        } else if (prefix == "skills") {
            inventory.skills.push_back(std::move(component));
        } else if (prefix == "workflows") {
            inventory.workflows.push_back(std::move(component));
        } else if (prefix == "plugins") {
            inventory.plugins.push_back(std::move(component));
        } else if (prefix == "mcp") {
            inventory.mcp_servers.push_back(std::move(component));
        }
    }
}

void CollectTopLevel(const std::filesystem::path& root, PackageInventory& inventory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) return;
    std::set<std::string> reserved;
    for (const std::string& name : ReservedTopLevelNames()) reserved.insert(name);

    std::vector<std::pair<std::string, bool>> entries;  // (名字, 是目录)
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        const std::u8string name_u8 = entry.path().filename().u8string();
        entries.emplace_back(std::string(reinterpret_cast<const char*>(name_u8.data()), name_u8.size()),
                             entry.is_directory(ec));
        ec.clear();
    }
    std::sort(entries.begin(), entries.end());
    static const std::set<std::string> kRootFiles = {"package.yaml", "README.md", "LICENSE"};
    for (const auto& [name, is_dir] : entries) {
        if (reserved.count(name) > 0) {
            // 根文件(package.yaml 等)必须是文件,标准目录(agents/assets 等)
            // 必须是目录——位置对类型不对,整包 invalid。
            if (kRootFiles.count(name) > 0 && is_dir) {
                inventory.diagnostics.push_back({PackageDiagnostic::Kind::Error, name,
                                                 "根文件位置上是目录,应是文件"});
            } else if (kRootFiles.count(name) == 0 && !is_dir) {
                inventory.diagnostics.push_back(
                    {PackageDiagnostic::Kind::Error, name, "标准目录位置上该是目录,实际是文件"});
            }
            continue;
        }
        const std::string near_miss = NearMissStandardDir(name);
        if (!near_miss.empty()) {
            inventory.diagnostics.push_back({PackageDiagnostic::Kind::Warning, name,
                                             "疑似拼错,想写 \"" + near_miss + "\"?该目录不自动加载"});
        } else if (is_dir) {
            inventory.diagnostics.push_back(
                {PackageDiagnostic::Kind::Info, name, "未知顶层目录(保留,不自动加载)"});
        } else {
            inventory.diagnostics.push_back(
                {PackageDiagnostic::Kind::Info, name, "未知顶层文件(保留,不自动加载)"});
        }
    }
}

}  // namespace

PackageInventory BuildPackageInventory(const PackageCandidate& candidate, const ScanOptions& options) {
    PackageInventory inventory;
    inventory.package_root = candidate.package_root;
    inventory.scope = candidate.scope;

    // 清单:ScanPackages 已解析的直取;没解析过的候选(doctor 直指路径的
    // 用法)就地补读一次。
    std::optional<PackageManifest> manifest = candidate.manifest;
    std::optional<ManifestError> manifest_error = candidate.manifest_error;
    if (!manifest.has_value() && !manifest_error.has_value()) {
        const auto text = ReadFileBytes(candidate.package_root / "package.yaml");
        if (!text.has_value()) {
            manifest_error = ManifestError{"package.yaml", 0, "缺 package.yaml(包根必须有根清单)"};
        } else {
            auto parsed = ParsePackageManifest(*text);
            if (parsed.has_value()) {
                manifest = std::move(*parsed);
            } else {
                manifest_error = std::move(parsed.error());
            }
        }
    }

    if (manifest.has_value()) {
        inventory.manifest_ok = true;
        inventory.package_id = manifest->id;
        inventory.version_text = manifest->version.text;
    } else {
        inventory.package_id = candidate.dir_name;  // 目录名兜底,list/doctor 指着说话
        inventory.diagnostics.push_back(
            {PackageDiagnostic::Kind::Error, "package.yaml",
             manifest_error.has_value() ? manifest_error->Format() : "根清单缺失"});
    }

    // 文件账 + 内容哈希(先盘文件,哈希吃全账)。
    std::vector<InventoryFile> files;
    CollectFiles(candidate.package_root, files, inventory.diagnostics);
    std::ostringstream hash_input;
    hash_input << "luban-package-v1\n";
    for (InventoryFile& file : files) {
        const auto bytes = ReadFileBytes(candidate.package_root / Utf8ToPath(file.rel));
        if (!bytes.has_value()) {
            inventory.diagnostics.push_back({PackageDiagnostic::Kind::Error, file.rel, "读不动,不进哈希"});
            file.hash = "(unreadable)";
            continue;
        }
        file.hash = hooks::Sha256Hex(*bytes);
        hash_input << file.rel << '\t' << file.size << '\t' << file.hash << '\n';
        ++inventory.total_file_count;
        if (file.code_bearing) ++inventory.code_bearing_file_count;
        if (file.rel.rfind("assets/", 0) == 0) ++inventory.assets_file_count;
        if (file.rel.rfind("docs/", 0) == 0) ++inventory.docs_file_count;
    }
    inventory.content_hash = hooks::Sha256Hex(hash_input.str());

    CollectComponents(candidate.package_root, inventory.package_id, inventory);
    CollectTopLevel(candidate.package_root, inventory);

    // 目录名与 id 对不上:提示(单子测试账"目录名与 id 不同的 warning")。
    // 惯例是目录名取 id 的最后一段(moontide.browser-suite -> browser-suite)。
    if (manifest.has_value()) {
        const std::size_t last_dot = manifest->id.rfind('.');
        const std::string id_tail =
            last_dot == std::string::npos ? manifest->id : manifest->id.substr(last_dot + 1);
        if (!candidate.dir_name.empty() && candidate.dir_name != manifest->id &&
            candidate.dir_name != id_tail) {
            inventory.diagnostics.push_back(
                {PackageDiagnostic::Kind::Warning, candidate.dir_name,
                 "目录名与 id 对不上(id " + manifest->id + ");按 id 记账,目录名只是住址"});
        }
        // compatibility:写了就严格检查(单子 §5.1)。
        if (manifest->compatibility_lubancode.has_value() && options.current_lubancode.has_value()) {
            if (!VersionSatisfies(*options.current_lubancode, *manifest->compatibility_lubancode)) {
                inventory.diagnostics.push_back(
                    {PackageDiagnostic::Kind::Warning, "compatibility.lubancode",
                     "声明范围 " + manifest->compatibility_lubancode->text + ",当前 " +
                         options.current_lubancode->text + " 不满足"});
            }
        }
        if (!manifest->compatibility_platforms.empty() && !options.current_platform.empty()) {
            bool on_list = false;
            for (const std::string& platform : manifest->compatibility_platforms) {
                if (platform == options.current_platform) on_list = true;
            }
            if (!on_list) {
                inventory.diagnostics.push_back(
                    {PackageDiagnostic::Kind::Warning, "compatibility.platforms",
                     "声明平台不含当前 " + options.current_platform});
            }
        }
    }

    inventory.valid = inventory.manifest_ok;
    for (const auto& diagnostic : inventory.diagnostics) {
        if (diagnostic.kind == PackageDiagnostic::Kind::Error) {
            inventory.valid = false;
        }
    }
    return inventory;
}

}  // namespace lubancode::package
