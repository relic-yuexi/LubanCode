// /package 命令的执行体(统一 Package 封装单阶段 1/2)。只读:list 列全账、
// show 看一只包、doctor 诊一只包。不挂任何组件、不写任何文件——发现不等
// 于执行,这一版连信任门都不碰(后续阶段)。doctor 在阶段 2 升级:静态账
// 之外,逐件过原生 parser 诊组件、解包内引用、给 MountPlan 摘要,一样只
// 读。参数拆解是纯函数(ParsePackageCommand,单测钉),这一头只留扫描与
// 打印。
#include "app/commands/package_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "app/version.hpp"                     // kVersion:版本号唯一出处
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <cctype>

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "package/catalog.hpp"
#include "package/inventory.hpp"
#include "package/manifest.hpp"
#include "package/semver.hpp"
#include "package/state.hpp"
#include "package/trust.hpp"
#include "evolution/promoter.hpp"  // VersionStore(阶段 4:store 选中版本并进 /package 账面)
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

// 信任账的只读装载(show/list/doctor 摆状态用;读不动只警告,按空白续
// ——重新审一遍比带着一本读不动的账继续跑更安全)。
std::pair<std::optional<lubancode::package::PackageTrustStore>, std::optional<std::string>>
LoadTrustStoreReadOnly() {
    const auto path = lubancode::package::PackageTrustStore::DefaultStorePath();
    if (!path.has_value()) {
        return {std::nullopt, std::nullopt};
    }
    auto [store, error] = lubancode::package::PackageTrustStore::Load(path);
    return {std::move(store), std::move(error)};
}

// 启停账的只读装载(阶段 6;读不动只警告,按全启用续——启停是"别挂谁"
// 的账,不是放行账,缺省态是启用)。
std::pair<std::optional<lubancode::package::PackageStateStore>, std::optional<std::string>>
LoadStateStoreReadOnly() {
    const auto path = lubancode::package::PackageStateStore::DefaultStatePath();
    if (!path.has_value()) {
        return {std::nullopt, std::nullopt};
    }
    auto [store, error] = lubancode::package::PackageStateStore::Load(path);
    return {std::move(store), std::move(error)};
}

// evolution store 的选中版本(阶段 4):active/canary 指针指到的那枚折成
// scope=Store 的现成候选,并进 /package 的账面——四层扫描之外第五路,哈希
// 验完好与 tamper 都在列(list 是发现账;挂载侧只收完好的)。
std::vector<lubancode::package::PackageCandidate> BuildStoreCandidates(SlashDispatchContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return {};
    }
    const lubancode::evolution::VersionStore store(
        lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "package-store");
    return store.ScanSelectedCandidates();
}

// 四层扫描 + store 选中,按优先级从高到低稳排(dev > project > store >
// user > official):list 的行序、LookupPackage 的胜者判定共用这一份。
std::vector<lubancode::package::PackageCandidate> ScanAllLayers(
    SlashDispatchContext& ctx, const lubancode::package::ScanOptions& options) {
    std::vector<lubancode::package::PackageCandidate> candidates =
        lubancode::package::ScanPackages(options);
    std::vector<lubancode::package::PackageCandidate> store = BuildStoreCandidates(ctx);
    if (!store.empty()) {
        const auto insert_at = std::find_if(
            candidates.begin(), candidates.end(), [](const auto& scanned) {
                return lubancode::package::ScopePrecedence(scanned.scope) <
                       lubancode::package::ScopePrecedence(lubancode::package::PackageScope::Store);
            });
        candidates.insert(insert_at, std::make_move_iterator(store.begin()),
                          std::make_move_iterator(store.end()));
    }
    return candidates;
}

// 一只包的"列表行":id、版本、来源、状态、六类组件计数、code-bearing。
// mount 非空时按会话钉快照如实标挂载状态(阶段 3):内容组件挂了几件、
// 代码组件的门禁(阶段 4)、现扫到但会话没挂的(启动后才放进目录)注明
// 下回启动才见。state_store 非空时现查启停账(阶段 6):停用的标 disabled
// ——扫描发现照旧,挂载跳过;本会话快照仍挂着的如实注明(下回装配不再
// 挂)。trust 非空时另摆一行现查的信任状态。
std::string DescribePackage(const lubancode::package::PackageInventory& inventory,
                            const std::string& state,
                            const lubancode::package::PackageMount* mount,
                            const lubancode::package::PackageTrustStore* trust,
                            const lubancode::package::PackageStateStore* state_store) {
    const bool disabled =
        state_store != nullptr && !state_store->IsEnabled(inventory.package_id);
    std::ostringstream out;
    out << "  " << inventory.package_id;
    if (!inventory.version_text.empty()) {
        out << " " << inventory.version_text;
    }
    out << " [" << lubancode::package::ScopeToString(inventory.scope) << "] ";
    if (disabled) {
        // valid → disabled 盖掉;shadowed/invalid 一类另注一笔,两头都看得见。
        out << (state == "valid" ? "disabled" : state + ";disabled");
    } else {
        out << state;
    }
    if (mount != nullptr) {
        if (const auto* mounted = mount->Find(inventory.package_id)) {
            out << "  已挂载内容组件 " << mounted->mounted_canonical_ids.size() << " 件";
            if (disabled) {
                out << "(已停用;本会话快照钉着启动那折,在跑的不拆,下回装配不再挂)";
            }
            if (mounted->code_trust == lubancode::package::CodeTrustStatus::PendingTrust) {
                out << ";Plugin/MCP 待信任门";
            } else if (mounted->code_trust == lubancode::package::CodeTrustStatus::Trusted) {
                out << ";Plugin/MCP 已过信任门(挂载事务随会话启动跑,整包成整包败)";
            }
        } else if (inventory.valid) {
            out << (disabled ? "  已停用(挂载跳过,连内容组件一件不挂)"
                             : "  未挂载(会话启动后才见;下回启动生效)");
        }
    }
    out << "  agents:" << inventory.agents.size()
        << " prompts:" << inventory.prompt_profiles.size()
        << " skills:" << inventory.skills.size()
        << " workflows:" << inventory.workflows.size()
        << " plugins:" << inventory.plugins.size()
        << " mcp:" << inventory.mcp_servers.size();
    if (inventory.code_bearing()) {
        out << "  [code-bearing]";
    }
    if (inventory.code_bearing()) {
        out << "\n    信任: " << lubancode::package::DescribeTrustStatus(inventory, trust);
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
// 返回 {胜者候选, 被遮住的候选列表}。
struct PackageLookup {
    const lubancode::package::PackageCandidate* winner = nullptr;
    std::vector<const lubancode::package::PackageCandidate*> shadowed;
};

PackageLookup LookupPackage(const std::vector<lubancode::package::PackageCandidate>& candidates,
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
        return lookup;  // winner 空,调用方据此报"没找到"
    }
    lookup.winner = matches.front();
    for (const auto* match : matches) {
        if (lubancode::package::ScopePrecedence(match->scope) >
            lubancode::package::ScopePrecedence(lookup.winner->scope)) {
            lookup.winner = match;
        }
    }
    for (const auto* match : matches) {
        if (match != lookup.winner) {
            lookup.shadowed.push_back(match);
        }
    }
    return lookup;
}

// 引用解析的包外既有名(单子 §七:Resolver 先本包,再看外部命名空间)。
// 有就喂:config.json 的 mcpServers 键、已扫到的 Skill、builtin Agent 两名。
lubancode::package::ExternalNamespaces BuildExternalNamespaces(SlashDispatchContext& ctx) {
    lubancode::package::ExternalNamespaces external;
    if (ctx.config != nullptr) {
        for (const auto& [name, server] : ctx.config->mcp_servers) {
            (void)server;
            external.mcp_servers.insert(name);
        }
    }
    if (ctx.skills != nullptr) {
        for (const auto& meta : *ctx.skills) {
            external.skills.insert(meta.name);
        }
    }
    // builtin Agent 码内注册两名(agent_catalog.hpp 的口径);user/project 层
    // 的自定义 Agent 目录这层不扫,短名引用它们按悬空报、写全名或挪进包。
    external.agents.insert("general-purpose");
    external.agents.insert("Explore");
    return external;
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
                scope == "dev" || scope == "store") {
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
    if (lower == "trust" || lower == "untrust") {
        if (rest.empty()) {
            parsed.action = PackageCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = lower == "trust" ? PackageCommandAction::Trust : PackageCommandAction::Untrust;
        parsed.target = rest;
        return parsed;
    }
    if (lower == "enable" || lower == "disable") {
        if (rest.empty()) {
            parsed.action = PackageCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = lower == "enable" ? PackageCommandAction::Enable : PackageCommandAction::Disable;
        parsed.target = rest;
        return parsed;
    }
    if (lower == "reload") {
        if (!rest.empty()) {
            parsed.action = PackageCommandAction::Invalid;
            parsed.bad_word = rest;
            return parsed;
        }
        parsed.action = PackageCommandAction::Reload;
        return parsed;
    }
    parsed.action = PackageCommandAction::Invalid;
    parsed.bad_word = word;
    return parsed;
}

// ---------------- 执行 ----------------

namespace {

void PrintUsage() {
    TermOut() << "用法: /package list [all|user|project|store|official|dev]\n"
                 "      /package show <id>\n"
                 "      /package doctor <id|路径>\n"
                 "      /package trust <id>    批准整包内容哈希(重启生效;文件一改即失效)\n"
                 "      /package untrust <id>  销信任账\n"
                 "      /package enable <id>   复启(下回启动或 reload 后的装配生效)\n"
                 "      /package disable <id>  停用(挂载一律跳过;扫描发现照旧)\n"
                 "      /package reload        重扫五路折新快照(折好才换;code 组件须新会话)\n"
                 "只读:list/show 只查静态账;doctor 另诊组件(逐件原生 parser)、引用解析与\n"
                 "MountPlan 摘要。trust 亮全份审批材料(逐件命令面 + 完整指纹)才落账——\n"
                 "未信任的 code 组件(Plugin/MCP)一件不挂不启动。启停账在包外\n"
                 "(~/.lubancode/package-state.json),enable/disable 只落账不拆在跑的。\n"
                 "store 一路是自进化闭环装进 package-store 的选中版本(/evolve approve 落架,\n"
                 "/evolve use 点灰度)。\n";
    TermOut().flush();
}

void RunPackageList(const lubancode::package::ScanOptions& options,
                    const std::optional<std::string>& scope_filter,
                    const lubancode::package::PackageMount* mount,
                    const std::vector<lubancode::package::PackageCandidate>& candidates) {

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
    auto [trust_store, trust_error] = LoadTrustStoreReadOnly();
    if (trust_error.has_value()) {
        TermOut() << "警告: " << *trust_error << "\n";
    }
    const lubancode::package::PackageTrustStore* trust =
        trust_store.has_value() ? &*trust_store : nullptr;
    auto [state_store, state_error] = LoadStateStoreReadOnly();
    if (state_error.has_value()) {
        TermOut() << "警告: " << *state_error << "\n";
    }
    const lubancode::package::PackageStateStore* state =
        state_store.has_value() ? &*state_store : nullptr;
    TermOut() << "Package(五路: dev > project > store > user > official;store 是自进化装架):\n";
    for (const auto& row : rows) {
        if (!scope_matches(row)) continue;
        ++shown;
        if (row.shadowed) {
            TermOut() << DescribePackage(row.inventory,
                                         "shadowed(被 " + row.shadowed_by + " 遮住)", mount, trust, state);
        } else {
            TermOut() << DescribePackage(row.inventory,
                                         row.inventory.valid ? "valid" : "invalid", mount, trust, state);
        }
        TermOut() << "\n";
    }
    if (shown == 0) {
        TermOut() << "  (没有可列的包;放一只 <package-root>/package.yaml 进四层任一层即被发现)\n";
    }
    TermOut().flush();
}

void RunPackageShow(const lubancode::package::ScanOptions& options, const std::string& target,
                    const lubancode::package::PackageMount* mount,
                    const std::vector<lubancode::package::PackageCandidate>& candidates) {
    const PackageLookup lookup = LookupPackage(candidates, target);
    if (lookup.winner == nullptr && lookup.shadowed.empty()) {
        TermOut() << "没找到包 \"" << target << "\"(按 id 或目录名查;先 /package list 看全账)\n";
        TermOut().flush();
        return;
    }
    const auto inventory = lubancode::package::BuildPackageInventory(*lookup.winner, options);
    auto [trust_store, trust_error] = LoadTrustStoreReadOnly();
    if (trust_error.has_value()) {
        TermOut() << "警告: " << *trust_error << "\n";
    }
    const lubancode::package::PackageTrustStore* trust =
        trust_store.has_value() ? &*trust_store : nullptr;
    auto [state_store, state_error] = LoadStateStoreReadOnly();
    if (state_error.has_value()) {
        TermOut() << "警告: " << *state_error << "\n";
    }
    const lubancode::package::PackageStateStore* state =
        state_store.has_value() ? &*state_store : nullptr;
    const bool disabled = state != nullptr && !state->IsEnabled(inventory.package_id);
    TermOut() << inventory.package_id
              << (inventory.version_text.empty() ? "" : " " + inventory.version_text) << "\n";
    TermOut() << "  状态: " << (inventory.valid ? "valid" : "invalid")
              << (disabled ? "(已停用)" : "")
              << (inventory.manifest_ok ? "" : "(根清单解析失败)")
              << (inventory.code_bearing() ? "  [code-bearing]" : "") << "\n";
    TermOut() << "  来源: [" << lubancode::package::ScopeToString(inventory.scope) << "] "
              << lubancode::platform::PathToUtf8(inventory.package_root) << "\n";
    TermOut() << "  启停: " << lubancode::package::DescribeStateStatus(inventory, state) << "\n";
    if (inventory.code_bearing()) {
        TermOut() << "  信任: " << lubancode::package::DescribeTrustStatus(inventory, trust) << "\n";
    }
    if (mount != nullptr) {
        if (const auto* mounted = mount->Find(inventory.package_id)) {
            TermOut() << "  挂载: 内容组件 " << mounted->mounted_canonical_ids.size()
                      << " 件已挂(canonical id 见下;会话钉快照)";
            if (mounted->code_trust == lubancode::package::CodeTrustStatus::PendingTrust) {
                TermOut() << ";Plugin/MCP 待信任门,一件不挂不执行";
            } else if (mounted->code_trust == lubancode::package::CodeTrustStatus::Trusted) {
                TermOut() << ";Plugin/MCP 已过信任门(挂载事务随会话启动跑,整包成整包败)";
            }
            if (disabled) {
                TermOut() << ";已停用,本会话照旧跑完,下回装配不再挂";
            }
            TermOut() << "\n";
        } else if (inventory.valid) {
            TermOut() << (disabled ? "  挂载: 已停用(挂载跳过,连内容组件一件不挂)\n"
                                   : "  挂载: 本会话未挂(启动后才放进目录;下回启动生效)\n");
        } else {
            TermOut() << "  挂载: 无效包,一件不挂(整包成整包败)\n";
        }
    }
    TermOut() << "  内容哈希: " << inventory.content_hash << "  (盘点文件 "
              << inventory.total_file_count << " 个)\n";
    if (!lookup.shadowed.empty()) {
        TermOut() << "  被遮住的候选(" << lookup.shadowed.size() << " 份):\n";
        for (const auto* shadow : lookup.shadowed) {
            TermOut() << "    [" << lubancode::package::ScopeToString(shadow->scope) << "] "
                      << lubancode::platform::PathToUtf8(shadow->package_root) << "\n";
        }
    }
    PrintComponents(inventory);
    PrintDiagnostics(inventory);
    TermOut().flush();
}

// doctor 的三段新账:组件逐件诊断、引用解析、MountPlan 摘要(只读)。
void PrintAnalyzedComponents(const lubancode::package::PackageRecord& record) {
    TermOut() << "  组件(" << record.components.size()
              << " 件,逐件过原生 parser;坏件照列,不因第一个错停):\n";
    if (record.components.empty()) {
        TermOut() << "    (没有组件;六类目录里没有可认的件)\n";
        return;
    }
    for (const auto& component : record.components) {
        const bool has_error = component.HasError();
        TermOut() << "    " << std::string(lubancode::package::ComponentKindName(component.kind))
                  << "  " << component.canonical_id << "  "
                  << (has_error ? "[error]" : (component.ok ? "ok" : "[error]")) << "  "
                  << component.rel_path;
        if (component.kind == lubancode::package::ComponentKind::Plugin && component.plugin.has_value()) {
            TermOut() << "  (" << component.plugin->tools.size() << " 件工具)";
            // v2 embedded-lua 的权限概要(§13.5:/plugin 与 /package 摆同一份
            // 权限真账;完整材料看 /package trust 的审批页)。
            if (component.plugin->manifest_version == lubancode::runtime::kPluginManifestVersionV2) {
                TermOut() << "  [embedded-lua entry " << component.plugin->runtime_entry << ";网络 "
                          << component.plugin->network_permissions.size() << " 目的地;Secret "
                          << component.plugin->secret_declarations.size() << " 件]";
            }
        }
        if (component.kind == lubancode::package::ComponentKind::PromptProfile &&
            !component.profile_files.empty()) {
            TermOut() << "  (" << component.profile_files.size() << " 个覆盖文件)";
        }
        TermOut() << "\n";
        for (const auto& issue : component.issues) {
            TermOut() << "      " << issue.Format() << "\n";
        }
    }
}

void PrintReferences(const lubancode::package::PackageRecord& record) {
    TermOut() << "  引用解析(" << record.references.size() << " 条):\n";
    if (record.references.empty()) {
        TermOut() << "    (没有包内引用)\n";
        return;
    }
    for (const auto& ref : record.references) {
        TermOut() << "    " << ref.Format() << "\n";
    }
}

void PrintMountPlan(const lubancode::package::PackageRecord& record) {
    if (!record.mount_plan.has_value()) {
        TermOut() << "  MountPlan: 不产(整包 invalid,一件也不挂——整包成整包败)\n";
        return;
    }
    const auto& plan = *record.mount_plan;
    TermOut() << "  MountPlan(只读计划,不启动 Plugin 与 MCP):\n";
    TermOut() << "    " << plan.entries.size() << " 件待挂:agent " << plan.CountKind(lubancode::package::ComponentKind::Agent)
              << " / prompt " << plan.CountKind(lubancode::package::ComponentKind::PromptProfile)
              << " / skill " << plan.CountKind(lubancode::package::ComponentKind::Skill)
              << " / workflow " << plan.CountKind(lubancode::package::ComponentKind::Workflow)
              << " / plugin " << plan.CountKind(lubancode::package::ComponentKind::Plugin)
              << " / mcp " << plan.CountKind(lubancode::package::ComponentKind::McpServer);
    if (plan.HasCodeBearing()) {
        TermOut() << "  [code-bearing," << lubancode::package::CodeTrustStatusText(plan.code_trust)
                  << "]";
    }
    TermOut() << "\n";
    for (const auto& entry : plan.entries) {
        TermOut() << "    " << entry.canonical_id << " -> " << entry.target_table << "  (源 "
                  << entry.source_root << ")";
        if (entry.code_bearing) {
            const char* gate = entry.trusted ? "已信任" : "待信任";
            TermOut() << "  [" << gate << "]";
        }
        TermOut() << "\n";
        for (const auto& tool : entry.tools) {
            TermOut() << "      工具 " << tool.wire_name << "\n"
                      << "         展示 " << tool.display_name << "\n";
        }
        if (!entry.depends_on.empty()) {
            TermOut() << "      依赖 " << [&] {
                std::string joined;
                for (const auto& dep : entry.depends_on) {
                    if (!joined.empty()) joined += ", ";
                    joined += dep;
                }
                return joined;
            }() << "\n";
        }
    }
}

void RunPackageDoctor(const lubancode::package::ScanOptions& options,
                      const lubancode::package::ExternalNamespaces& external, const std::string& target,
                      const std::vector<lubancode::package::PackageCandidate>& scanned) {
    // doctor 收 id 或路径:路径存在(目录)就当包根直接诊,否则按 id 查。
    std::optional<lubancode::package::PackageCandidate> direct;
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
            direct = std::move(candidate);
        }
    }

    const std::vector<lubancode::package::PackageCandidate> candidates = scanned;
    // 跨包全名引用的对账索引:扫描账里"已存在包"的账(store 选中也在内)。
    const lubancode::package::PackageRefIndex ref_index =
        lubancode::package::BuildPackageRefIndex(candidates);
    // 信任账的只读快照:code 件在 plan 里的门禁标随它定(阶段 4)。
    auto [trust_store, trust_error] = LoadTrustStoreReadOnly();
    if (trust_error.has_value()) {
        TermOut() << "警告: " << *trust_error << "\n";
    }
    const lubancode::package::PackageTrustSnapshot trust_snapshot =
        trust_store.has_value() ? trust_store->Snapshot()
                                : lubancode::package::PackageTrustSnapshot{};

    lubancode::package::PackageRecord record;
    if (direct.has_value()) {
        record = lubancode::package::AnalyzePackage(*direct, options, ref_index, external,
                                                    &trust_snapshot);
    } else {
        const PackageLookup lookup = LookupPackage(candidates, target);
        if (lookup.winner == nullptr && lookup.shadowed.empty()) {
            TermOut() << "没找到包 \"" << target << "\"(doctor 收 id 或包路径)\n";
            TermOut().flush();
            return;
        }
        record = lubancode::package::AnalyzePackage(*lookup.winner, options, ref_index, external,
                                                    &trust_snapshot);
    }

    const auto& inventory = record.inventory;
    TermOut() << "诊断 " << inventory.package_id << ":\n";
    TermOut() << "  根清单: " << (inventory.manifest_ok ? "解析通过(schema 1, SemVer)"
                                                          : "解析失败")
              << (inventory.version_text.empty() ? "" : ",version " + inventory.version_text) << "\n";
    if (options.current_lubancode.has_value()) {
        TermOut() << "  LubanCode: 当前 " << options.current_lubancode->text
                  << ",平台 " << options.current_platform << "(写了 compatibility 就检查)\n";
    }
    TermOut() << "  包根: [" << lubancode::package::ScopeToString(inventory.scope) << "] "
              << lubancode::platform::PathToUtf8(inventory.package_root) << "\n";
    // 启停一行(阶段 6,doctor 表的"信任、启停与 runtime"项):现查启停账。
    {
        auto [state_store, state_error] = LoadStateStoreReadOnly();
        if (state_error.has_value()) {
            TermOut() << "警告: " << *state_error << "\n";
        }
        TermOut() << "  启停: "
                  << lubancode::package::DescribeStateStatus(
                         inventory, state_store.has_value() ? &*state_store : nullptr)
                  << "\n";
    }
    TermOut() << "  盘点: 文件 " << inventory.total_file_count << "(assets "
              << inventory.assets_file_count << " / docs " << inventory.docs_file_count
              << " / code-bearing " << inventory.code_bearing_file_count
              << "),内容哈希 " << inventory.content_hash << "\n";
    PrintDiagnostics(inventory);
    PrintAnalyzedComponents(record);
    PrintReferences(record);
    PrintMountPlan(record);
    TermOut() << "  结论: " << (record.valid ? "valid(静态账干净;挂载是后续阶段的事)"
                                             : "invalid(按清单修好再看)") << "\n";
    TermOut().flush();
}

// /package trust|untrust <id>(阶段 4):扫描定胜者 -> AnalyzePackage 出全份
// 材料与状态 -> 账务(TrustPackage/UntrustPackage,回执逐行)。落账即时,
// 生效在重启(会话钉快照,阶段 3 语义)。
void RunPackageTrust(SlashDispatchContext& ctx, const std::string& target, bool trust_action) {
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    const std::vector<lubancode::package::PackageCandidate> candidates =
        lubancode::package::ScanPackages(options);
    const PackageLookup lookup = LookupPackage(candidates, target);
    if (lookup.winner == nullptr && lookup.shadowed.empty()) {
        TermOut() << "没找到包 \"" << target
                  << "\"(trust/untrust 按 id 或目录名查;先 /package list 看全账)\n";
        TermOut().flush();
        return;
    }
    const lubancode::package::PackageRefIndex ref_index =
        lubancode::package::BuildPackageRefIndex(candidates);
    const lubancode::package::PackageRecord record = lubancode::package::AnalyzePackage(
        *lookup.winner, options, ref_index, BuildExternalNamespaces(ctx));

    // 信任账:主目录一份,交互与管道同一出入口。读不动警告 + 空白续。
    std::optional<lubancode::package::PackageTrustStore> store;
    if (const auto path = lubancode::package::PackageTrustStore::DefaultStorePath();
        path.has_value()) {
        auto [loaded, load_error] = lubancode::package::PackageTrustStore::Load(path);
        if (load_error.has_value()) {
            TermOut() << "警告: " << *load_error << "\n";
        }
        store = std::move(loaded);
    }
    const lubancode::package::PackageTrustActionResult result =
        trust_action ? lubancode::package::TrustPackage(record, store.has_value() ? &*store : nullptr)
                     : lubancode::package::UntrustPackage(record,
                                                          store.has_value() ? &*store : nullptr);
    if (!result.ok) {
        TermOut() << result.error << "\n";
    } else {
        for (const std::string& line : result.lines) {
            TermOut() << line << "\n";
        }
    }
    TermOut().flush();
}

// /package enable|disable <id>(阶段 6):扫描定胜者 -> 轻盘点出身份 ->
// 账务(EnableDisablePackage,回执逐行)。落账即时,生效在下回装配(会话
// 钉快照,不拆在跑的)——回执里如实说,另注明本会话快照还给它挂着几件。
void RunPackageEnableDisable(SlashDispatchContext& ctx, const std::string& target, bool enable) {
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    const std::vector<lubancode::package::PackageCandidate> candidates =
        ScanAllLayers(ctx, options);
    const PackageLookup lookup = LookupPackage(candidates, target);
    if (lookup.winner == nullptr && lookup.shadowed.empty()) {
        TermOut() << "没找到包 \"" << target
                  << "\"(enable/disable 按 id 或目录名查;先 /package list 看全账)\n";
        TermOut().flush();
        return;
    }
    const lubancode::package::PackageInventory inventory =
        lubancode::package::BuildPackageInventory(*lookup.winner, options);

    // 启停账:主目录一份,交互与管道同一出入口。读不动警告 + 按全启用续。
    std::optional<lubancode::package::PackageStateStore> store;
    if (const auto path = lubancode::package::PackageStateStore::DefaultStatePath();
        path.has_value()) {
        auto [loaded, load_error] = lubancode::package::PackageStateStore::Load(path);
        if (load_error.has_value()) {
            TermOut() << "警告: " << *load_error << "\n";
        }
        store = std::move(loaded);
    }
    // 本会话快照给它挂着几件(disable 回执注明"在跑的照旧")。
    int mounted_count = -1;
    if (ctx.package_mount != nullptr) {
        if (const auto* mounted = ctx.package_mount->Find(inventory.package_id)) {
            mounted_count = static_cast<int>(mounted->mounted_canonical_ids.size());
        } else {
            mounted_count = 0;
        }
    }
    const lubancode::package::PackageStateActionResult result = lubancode::package::EnableDisablePackage(
        inventory, store.has_value() ? &*store : nullptr, enable, mounted_count);
    if (!result.ok) {
        TermOut() << result.error << "\n";
    } else {
        for (const std::string& line : result.lines) {
            TermOut() << line << "\n";
        }
    }
    TermOut().flush();
}

// /package reload(阶段 6):会话侧重折快照 + 原子换档 + 刷下游。回执行
//(含折不动的诊断)逐行打印;没接会话执行体(纯函数装配)如实明说。
void RunPackageReload(SlashDispatchContext& ctx) {
    if (ctx.reload_packages == nullptr) {
        TermOut() << "这个装配没接会话 reload 口(单发/无交互栈),折不了新快照。\n"
                     "重启会话即可按最新目录与启停账装配。\n";
        TermOut().flush();
        return;
    }
    for (const std::string& line : ctx.reload_packages()) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

}  // namespace

CommandFlow HandleSlashPackage(SlashDispatchContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed) {
    const ParsedPackageCommand command = ParsePackageCommand(parsed.args);
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    const std::vector<lubancode::package::PackageCandidate> candidates = ScanAllLayers(ctx, options);
    switch (command.action) {
        case PackageCommandAction::List:
            RunPackageList(options, command.scope_filter, ctx.package_mount, candidates);
            return CommandFlow::Continue;
        case PackageCommandAction::Show:
            RunPackageShow(options, command.target, ctx.package_mount, candidates);
            return CommandFlow::Continue;
        case PackageCommandAction::Doctor:
            RunPackageDoctor(options, BuildExternalNamespaces(ctx), command.target, candidates);
            return CommandFlow::Continue;
        case PackageCommandAction::Trust:
            RunPackageTrust(ctx, command.target, /*trust_action=*/true);
            return CommandFlow::Continue;
        case PackageCommandAction::Untrust:
            RunPackageTrust(ctx, command.target, /*trust_action=*/false);
            return CommandFlow::Continue;
        case PackageCommandAction::Enable:
            RunPackageEnableDisable(ctx, command.target, /*enable=*/true);
            return CommandFlow::Continue;
        case PackageCommandAction::Disable:
            RunPackageEnableDisable(ctx, command.target, /*enable=*/false);
            return CommandFlow::Continue;
        case PackageCommandAction::Reload:
            RunPackageReload(ctx);
            return CommandFlow::Continue;
        case PackageCommandAction::Invalid:
            TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
