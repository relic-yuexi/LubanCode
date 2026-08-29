// PackageSource 与四层扫描、稳定文件盘点、内容哈希、越界校验、近似目录
// 名(统一 Package 封装单阶段 1)。发现不等于执行:这一层只查只诊,不挂任
// 何组件——盘点出的组件只是"账上有名",原生 parser 归阶段 2 各自去跑。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "package/manifest.hpp"
#include "package/semver.hpp"

namespace lubancode::package {

// ---------------------------------------------------------------------------
// 来源与四层扫描(单子 §八)。同 package id 的优先级:
//   CLI dev(--package-dir) > project > user > official
// 每层都是"包目录的父目录":层下每个直接子目录是一只 Package。被盖住的
// 版本仍进诊断账,不静默丢弃。
// ---------------------------------------------------------------------------
enum class PackageScope { Official, User, Project, Dev };

// 数值越大越优先。Dev 3 > Project 2 > User 1 > Official 0。
int ScopePrecedence(PackageScope scope);
std::string ScopeToString(PackageScope scope);

struct ScanOptions {
    std::optional<std::filesystem::path> official_root;  // <official>/packages
    std::optional<std::filesystem::path> user_root;      // ~/.lubancode/packages
    std::optional<std::filesystem::path> project_root;   // <project>/.lubancode/packages
    std::vector<std::filesystem::path> dev_roots;        // --package-dir,可重复
    // doctor 的兼容性检查要用"当前 LubanCode 版本与平台"。版本由 app 层
    // 从唯一出处(version.hpp)喂进来,core 层不抄写字面量;platform 取
    // "windows"/"linux"/"macos"。
    std::optional<SemVer> current_lubancode;
    std::string current_platform;
};

// 一份扫描候选:某层下的一只包目录。manifest 解析失败/缺清单也进账
// (valid=false + manifest_error),list/doctor 才有的说。
struct PackageCandidate {
    PackageScope scope = PackageScope::User;
    std::filesystem::path layer_root;    // 所在层的 packages 目录
    std::filesystem::path package_root;  // layer_root/<子目录>
    std::string dir_name;                // 子目录名(UTF-8)
    std::optional<PackageManifest> manifest;
    std::optional<ManifestError> manifest_error;  // 缺清单/解析失败的账
};

// 四层扫描。层内按目录名 UTF-8 排序;层间按优先级从高到低。目录不存在
// 的层安静跳过。子目录里没有 package.yaml 的进账(manifest_error 记账),
// 不当异常。
std::vector<PackageCandidate> ScanPackages(const ScanOptions& options);

// ---------------------------------------------------------------------------
// 盘点(单子 §5.2)。只读扫描包根,不写回源目录。盘点须稳定:先规范路径
// (UTF-8、'/' 分隔的包内相对路径),再按 UTF-8 字节序排序——文件系统枚举
// 次序不配当 ID、覆盖次序或哈希次序。
// ---------------------------------------------------------------------------
struct PackageDiagnostic {
    enum class Kind { Info, Warning, Error };
    Kind kind = Kind::Info;
    std::string path;      // 相对路径或目录名;包根级问题留空
    std::string message;

    std::string Format() const;
};

struct PackageComponent {
    std::string local_id;      // browser-tester
    std::string canonical_id;  // moontide.browser-suite:browser-tester(单子 §七)
    std::string rel_path;      // agents/browser-tester.yaml 或 skills/browser-testing
};

struct PackageInventory {
    std::string package_id;       // 解析失败时用目录名兜底(list 才指着说话)
    std::string version_text;     // 原文;解析失败为空
    std::filesystem::path package_root;
    PackageScope scope = PackageScope::User;
    std::string content_hash;     // 64 位十六进制;盘点文件一个字节变,它就变
    std::size_t total_file_count = 0;
    std::vector<PackageComponent> agents;
    std::vector<PackageComponent> prompt_profiles;
    std::vector<PackageComponent> skills;
    std::vector<PackageComponent> workflows;
    std::vector<PackageComponent> plugins;
    std::vector<PackageComponent> mcp_servers;
    std::size_t assets_file_count = 0;
    std::size_t docs_file_count = 0;
    std::size_t code_bearing_file_count = 0;  // 单子 §9.2 的静态版:按目录与扩展名认
    std::vector<PackageDiagnostic> diagnostics;
    bool manifest_ok = false;  // package.yaml 解析成功
    bool valid = false;        // manifest_ok 且无 Error 级诊断

    bool code_bearing() const { return code_bearing_file_count > 0; }
};

// 六类标准组件目录名(单子 §四:必须在包根,不递归猜)。
std::vector<std::string> StandardComponentDirs();
// 顶层保留名单:标准组件目录 + assets/docs + 根文件(package.yaml、
// README.md、LICENSE)。名单外的是"未知顶层目录",给 doctor 提示。
std::vector<std::string> ReservedTopLevelNames();

// 对一份候选做只读盘点。manifest 解析失败的候选也盘(诊断账里说清),
// 组件目录照认——全账查清的验收线在这。
PackageInventory BuildPackageInventory(const PackageCandidate& candidate,
                                       const ScanOptions& options = ScanOptions{});

// 轻扫六类组件目录:只认目录与入口文件名,不读内容、不算哈希。四层扫描
// 之外另要一份"这只包有哪些组件"的账时用(阶段 2 的跨包引用索引、
// MountPlan 的 source root 清点)。diagnostics 可空;给了就照盘点口径收
// "缺入口文件"一类的 warning。
std::vector<PackageComponent> ListPackageComponents(const std::filesystem::path& package_root,
                                                    const std::string& package_id,
                                                    std::vector<PackageDiagnostic>* diagnostics = nullptr);

// ---------------------------------------------------------------------------
// 路径安全(单子 §十三 doctor 的"相对路径与符号链接越界")。纯函数,单测
// 直接喂构造串。
// ---------------------------------------------------------------------------
enum class PathIssue { None, Empty, Absolute, ParentEscape };

// 包内相对路径的静态合法性:'..' 段、绝对路径(POSIX '/' 与 Windows 盘符/
// UNC)、空串都算越界;'\' 按 Windows 写法当分隔符认,防 "agents\..\.."
// 这类混拼。
PathIssue CheckPackageRelativePath(std::string_view rel_utf8);
std::string_view PathIssueText(PathIssue issue);

// 近似目录名:与六个标准组件目录(含 assets/docs)小写化后编辑距离 1..2,
// 且本身不在保留名单——"拼错 skill/、workflow/ 这类近似名要明报"(单子
// §四)。精确命中保留名单的不算。返回最近的标准名,没有返回空。
std::string NearMissStandardDir(std::string_view dir_name);

}  // namespace lubancode::package
