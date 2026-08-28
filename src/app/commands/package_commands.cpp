// /package 命令的执行体(统一 Package 封装单阶段 1)。只读:list 列全账、
// show 看一只包、doctor 诊一只包。不挂任何组件、不写任何文件——发现不等
// 于执行,这一版连信任门都不碰(后续阶段)。参数拆解是纯函数(Parse-
// PackageCommand,单测钉),这一头只留扫描与打印。
#include "app/commands/package_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "app/version.hpp"                     // kVersion:版本号唯一出处
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <cctype>

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "package/inventory.hpp"
#include "package/manifest.hpp"
#include "package/semver.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

namespace {

std::string Trimmed(std::string s) {
    std::size_t begin = 0;
    while (begin < s.size() &&
           std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

const char* CurrentPlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// 装配四层扫描的输入。user/project/official 三层各有出处,dev 层是
// --package-dir 攒下的目录(cli_app 递进来)。当前版本/平台供 doctor 的
// compatibility 检查用。
lubancode::package::ScanOptions BuildScanOptions(SlashDispatchContext& ctx) {
    lubancode::package::ScanOptions options;
    if (ctx.home_lubancode != nullptr && ctx.home_lubancode->has_value()) {
        options.user_root = lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "packages";
    }
    options.project_root = std::filesystem::current_path() / ".lubancode" / "packages";
    if (const auto official = lubancode::platform::OfficialPackagesDir();
        official.has_value()) {
        options.official_root = lubancode::platform::Utf8ToPath(*official);
    }
    if (ctx.opts != nullptr) {
        for (const std::string& dir : ctx.opts->package_dirs) {
            if (!dir.empty()) {
                options.dev_roots.push_back(lubancode::platform::Utf8ToPath(dir));
            }
        }
    }
    if (const auto version = lubancode::package::ParseSemVer(kVersion);
        version.has_value()) {
        options.current_lubancode = *version;
    }
    options.current_platform = CurrentPlatform();
    return options;
}

// 一只包的"列表行":id、版本、来源、状态、六类组件计数、code-bearing。
std::string DescribePackage(const lubancode::package::PackageInventory& inventory,
                            const std::string& state) {
    std::ostringstream out;
    out << "  " << inventory.package_id;
    if (!inventory.version_text.empty()) {
        out << " " << inventory.version_text;
    }
    out << " [" << lubancode::package::ScopeToString(inventory.scope) << "] " << state;
    out << "  agents:" << inventory.agents.size()
        << " prompts:" << inventory.prompt_profiles.size()
        << " skills:" << inventory.skills.size()
        << " workflows:" << inventory.workflows.size()
        << " plugins:" << inventory.plugins.size()
        << " mcp:" << inventory.mcp_servers.size();
    if (inventory.code_bearing()) {
        out << "  [code-bearing]";
    }
    return out.str();
}

void PrintDiagnostics(const lubancode::package::PackageInventory& inventory) {
    if (inventory.diagnostics.empty()) {
        TermOut() << "  诊断:无(干净)\n";
        return;
    }
    TermOut() << "  诊断(" << inventory.diagnostics.size() << " 条):\n";
    for (const auto& diagnostic : inventory.diagnostics) {
        TermOut() << "    " << diagnostic.Format() << "\n";
    }
}

void PrintComponents(const lubancode::package::PackageInventory& inventory) {
    const struct {
        const char* label;
        const std::vector<lubancode::package::PackageComponent>* items;
    } groups[] = {
        {"agents", &inventory.agents},
        {"prompt_profiles", &inventory.prompt_profiles},
        {"skills", &inventory.skills},
        {"workflows", &inventory.workflows},
        {"plugins", &inventory.plugins},
        {"mcp_servers", &inventory.mcp_servers},
    };
    for (const auto& group : groups) {
        TermOut() << "  " << group.label << "(" << group.items->size() << "):";
        if (group.items->empty()) {
            TermOut() << " 无\n";
            continue;
        }
        TermOut() << "\n";
        for (const auto& component : *group.items) {
            TermOut() << "    " << component.canonical_id << "  (" << component.rel_path << ")\n";
        }
    }
}

// id(或精确目录名)在全部候选里选份:优先级高的胜,其余进 shadowed 账。
// 返回 {胜者盘点, 被遮住的候选列表}。
struct PackageLookup {
    lubancode::package::PackageInventory winner;
    std::vector<lubancode::package::PackageCandidate> shadowed;
};

PackageLookup LookupPackage(const std::vector<lubancode::package::PackageCandidate>& candidates,
                            const lubancode::package::ScanOptions& options,
                            const std::string& id) {
    // 全部同名候选(id 或目录名)先收齐,再按四层优先级定胜者——被盖住的
    // 版本仍进账,show 要能列出所有候选(单子 §八)。
    std::vector<const lubancode::package::PackageCandidate*> matches;
    for (const auto& candidate : candidates) {
        const bool id_match = candidate.manifest.has_value() && candidate.manifest->id == id;
        const bool dir_match = candidate.dir_name == id;
        if (id_match || dir_match) {
            matches.push_back(&candidate);
        }
    }
    PackageLookup lookup;
    if (matches.empty()) {
        return lookup;  // package_root 空,调用方据此报"没找到"
    }
    const lubancode::package::PackageCandidate* winner = matches.front();
    for (const auto* match : matches) {
        if (lubancode::package::ScopePrecedence(match->scope) >
            lubancode::package::ScopePrecedence(winner->scope)) {
            winner = match;
        }
    }
    lookup.winner = lubancode::package::BuildPackageInventory(*winner, options);
    for (const auto* match : matches) {
        if (match != winner) {
            lookup.shadowed.push_back(*match);
        }
    }
    return lookup;
}

}  // namespace

// ---------------- 纯解析(单测钉) ----------------

ParsedPackageCommand ParsePackageCommand(const std::string& args) {
    ParsedPackageCommand parsed;
    const std::string trimmed = Trimmed(args);
    if (trimmed.empty()) {
        parsed.action = PackageCommandAction::List;  // 裸 /package = list all
        return parsed;
    }
    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string word = space == std::string::npos ? trimmed : trimmed.substr(0, space);
    const std::string rest = space == std::string::npos ? std::string()
                                                        : Trimmed(trimmed.substr(space + 1));
    const std::string lower = ToLowerAscii(word);
    if (lower == "list") {
        parsed.action = PackageCommandAction::List;
        if (!rest.empty()) {
            const std::string scope = ToLowerAscii(rest);
            if (scope == "all" || scope == "user" || scope == "project" || scope == "official" ||
                scope == "dev") {
                parsed.scope_filter = scope;
            } else {
                parsed.action = PackageCommandAction::Invalid;
                parsed.bad_word = rest;
            }
        }
        return parsed;
    }
    if (lower == "show" || lower == "doctor") {
        if (rest.empty()) {
            parsed.action = PackageCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = lower == "show" ? PackageCommandAction::Show : PackageCommandAction::Doctor;
        parsed.target = rest;
        return parsed;
    }
    parsed.action = PackageCommandAction::Invalid;
    parsed.bad_word = word;
    return parsed;
}

// ---------------- 执行 ----------------

namespace {

void PrintUsage() {
    TermOut() << "用法: /package list [all|user|project|official|dev]\n"
                 "      /package show <id>\n"
                 "      /package doctor <id|路径>\n"
                 "阶段 1 只读:list/show/doctor 只查只诊,不挂任何组件。\n";
    TermOut().flush();
}

void RunPackageList(const lubancode::package::ScanOptions& options,
                    const std::optional<std::string>& scope_filter) {
    const std::vector<lubancode::package::PackageCandidate> candidates =
        lubancode::package::ScanPackages(options);

    // 同 id 分组,组内最高优先级为胜者,其余 shadowed。
    struct Row {
        lubancode::package::PackageInventory inventory;
        bool shadowed = false;
        std::string shadowed_by;
    };
    std::vector<Row> rows;
    std::map<std::string, int> best_precedence;  // id -> 胜者优先级
    std::map<std::string, std::string> best_scope;
    for (const auto& candidate : candidates) {
        const std::string id =
            candidate.manifest.has_value() ? candidate.manifest->id : candidate.dir_name;
        const int precedence = lubancode::package::ScopePrecedence(candidate.scope);
        const auto known = best_precedence.find(id);
        if (known == best_precedence.end() || precedence > known->second) {
            best_precedence[id] = precedence;
            best_scope[id] = lubancode::package::ScopeToString(candidate.scope);
        }
    }
    for (const auto& candidate : candidates) {
        const std::string id =
            candidate.manifest.has_value() ? candidate.manifest->id : candidate.dir_name;
        Row row;
        row.inventory = lubancode::package::BuildPackageInventory(candidate, options);
        const bool shadowed =
            lubancode::package::ScopePrecedence(candidate.scope) < best_precedence[id];
        row.shadowed = shadowed;
        row.shadowed_by = best_scope[id];
        rows.push_back(std::move(row));
    }

    const auto scope_matches = [&](const Row& row) {
        if (!scope_filter.has_value() || *scope_filter == "all") return true;
        return lubancode::package::ScopeToString(row.inventory.scope) == *scope_filter;
    };

    std::size_t shown = 0;
    TermOut() << "Package(四层: dev > project > user > official):\n";
    for (const auto& row : rows) {
        if (!scope_matches(row)) continue;
        ++shown;
        if (row.shadowed) {
            TermOut() << DescribePackage(row.inventory,
                                         "shadowed(被 " + row.shadowed_by + " 遮住)");
        } else {
            TermOut() << DescribePackage(row.inventory,
                                         row.inventory.valid ? "valid" : "invalid");
        }
        TermOut() << "\n";
    }
    if (shown == 0) {
        TermOut() << "  (没有可列的包;放一只 <package-root>/package.yaml 进四层任一层即被发现)\n";
    }
    TermOut().flush();
}

void RunPackageShow(const lubancode::package::ScanOptions& options, const std::string& target) {
    const std::vector<lubancode::package::PackageCandidate> candidates =
        lubancode::package::ScanPackages(options);
    const PackageLookup lookup = LookupPackage(candidates, options, target);
    if (lookup.winner.package_root.empty() && lookup.shadowed.empty()) {
        TermOut() << "没找到包 \"" << target << "\"(按 id 或目录名查;先 /package list 看全账)\n";
        TermOut().flush();
        return;
    }
    const auto& inventory = lookup.winner;
    TermOut() << inventory.package_id
              << (inventory.version_text.empty() ? "" : " " + inventory.version_text) << "\n";
    TermOut() << "  状态: " << (inventory.valid ? "valid" : "invalid")
              << (inventory.manifest_ok ? "" : "(根清单解析失败)")
              << (inventory.code_bearing() ? "  [code-bearing]" : "") << "\n";
    TermOut() << "  来源: [" << lubancode::package::ScopeToString(inventory.scope) << "] "
              << lubancode::platform::PathToUtf8(inventory.package_root) << "\n";
    TermOut() << "  内容哈希: " << inventory.content_hash << "  (盘点文件 "
              << inventory.total_file_count << " 个;阶段 1 只读,不挂载)\n";
    if (!lookup.shadowed.empty()) {
        TermOut() << "  被遮住的候选(" << lookup.shadowed.size() << " 份):\n";
        for (const auto& shadow : lookup.shadowed) {
            TermOut() << "    [" << lubancode::package::ScopeToString(shadow.scope) << "] "
                      << lubancode::platform::PathToUtf8(shadow.package_root) << "\n";
        }
    }
    PrintComponents(inventory);
    PrintDiagnostics(inventory);
    TermOut().flush();
}

void RunPackageDoctor(const lubancode::package::ScanOptions& options, const std::string& target) {
    // doctor 收 id 或路径:路径存在(目录)就当包根直接诊,否则按 id 查。
    std::optional<lubancode::package::PackageInventory> inventory;
    std::error_code ec;
    const std::filesystem::path as_path = lubancode::platform::Utf8ToPath(target);
    if (target.find('/') != std::string::npos || target.find('\\') != std::string::npos ||
        (target.size() >= 2 && target[1] == ':')) {
        if (std::filesystem::is_directory(as_path, ec) && !ec) {
            lubancode::package::PackageCandidate candidate;
            candidate.scope = lubancode::package::PackageScope::Dev;  // 直诊不参与遮蔽
            candidate.layer_root = as_path.parent_path();
            candidate.package_root = as_path;
            candidate.dir_name = lubancode::platform::PathToUtf8(as_path.filename());
            inventory = lubancode::package::BuildPackageInventory(candidate, options);
        }
    }
    if (!inventory.has_value()) {
        const std::vector<lubancode::package::PackageCandidate> candidates =
            lubancode::package::ScanPackages(options);
        const PackageLookup lookup = LookupPackage(candidates, options, target);
        if (lookup.winner.package_root.empty() && lookup.shadowed.empty()) {
            TermOut() << "没找到包 \"" << target << "\"(doctor 收 id 或包路径)\n";
            TermOut().flush();
            return;
        }
        inventory = lookup.winner;
    }

    TermOut() << "诊断 " << inventory->package_id << ":\n";
    TermOut() << "  根清单: " << (inventory->manifest_ok ? "解析通过(schema 1, SemVer)"
                                                          : "解析失败")
              << (inventory->version_text.empty() ? "" : ",version " + inventory->version_text) << "\n";
    if (options.current_lubancode.has_value()) {
        TermOut() << "  LubanCode: 当前 " << options.current_lubancode->text
                  << ",平台 " << options.current_platform << "(写了 compatibility 就检查)\n";
    }
    TermOut() << "  包根: [" << lubancode::package::ScopeToString(inventory->scope) << "] "
              << lubancode::platform::PathToUtf8(inventory->package_root) << "\n";
    TermOut() << "  盘点: 文件 " << inventory->total_file_count << "(assets "
              << inventory->assets_file_count << " / docs " << inventory->docs_file_count
              << " / code-bearing " << inventory->code_bearing_file_count
              << "),内容哈希 " << inventory->content_hash << "\n";
    PrintDiagnostics(*inventory);
    TermOut() << "  结论: " << (inventory->valid ? "valid(静态账干净;挂载是后续阶段的事)"
                                                 : "invalid(按清单修好再看)") << "\n";
    TermOut().flush();
}

}  // namespace

CommandFlow HandleSlashPackage(SlashDispatchContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed) {
    const ParsedPackageCommand command = ParsePackageCommand(parsed.args);
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    switch (command.action) {
        case PackageCommandAction::List:
            RunPackageList(options, command.scope_filter);
            return CommandFlow::Continue;
        case PackageCommandAction::Show:
            RunPackageShow(options, command.target);
            return CommandFlow::Continue;
        case PackageCommandAction::Doctor:
            RunPackageDoctor(options, command.target);
            return CommandFlow::Continue;
        case PackageCommandAction::Invalid:
            TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
