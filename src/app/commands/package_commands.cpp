// /package 命令的执行体(统一 Package 封装单阶段 1/2)。只读:list 列全账、
// show 看一只包、doctor 诊一只包。不挂任何组件、不写任何文件——发现不等
// 于执行,这一版连信任门都不碰(后续阶段)。doctor 在阶段 2 升级:静态账
// 之外,逐件过原生 parser 诊组件、解包内引用、给 MountPlan 摘要,一样只
// 读。参数拆解是纯函数(ParsePackageCommand,单测钉),这一头只留扫描与
// 打印。
#include "app/commands/package_commands.hpp"
#include "app/version.hpp"  // kVersion:版本号唯一出处
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口
// HC-06(材料收窄,第二小批):不再经注册表头传递,本域要的完整定义直接递。
#include "config/config.hpp"        // Config::mcp_servers(包外命名空间)
#include "package/mounting.hpp"     // PackageMount(会话钉快照)
#include "tools/skill_loader.hpp"   // SkillMeta(包外 Skill 名)

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
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 4:/package 渲染段)
#include "cli/theme.hpp"
#include "platform/console.hpp"  // GetScreenInfo:/package 的框宽同一把尺
#include "platform/paths.hpp"

namespace lubancode::app {

namespace {

namespace frame = lubancode::cli::frame;

// ---- TUI 排版批 4(/package 全族)的公共小件 ------------------------------
//
// 渲染段只调 cli::frame::* 三助手(约定见 docs/development/tui_style.md)。
// 本文件文案是硬编码中文(不走 i18n 表),按批 2 裁量一字不添不改;列名用
// 数据 schema 名(id/scope/state、kind/status/path),句内冒号按 SentenceField
// 拆两列(批 1 裁量);引导下文的尾冒号标题化时剥掉(批 2 裁量 3)。

int PackageFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)),
                        accent};
}

void PrintNotice(const lubancode::cli::Theme& theme, const std::vector<std::string>& sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), PackageFrameWidth()));
}

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
// --package-dir 攒下的目录(组合根递进窄材料)。当前版本/平台供 doctor 的
// compatibility 检查用。
lubancode::package::ScanOptions BuildScanOptions(const PackageCommandContext& ctx) {
    lubancode::package::ScanOptions options;
    if (ctx.home_lubancode != nullptr && ctx.home_lubancode->has_value()) {
        options.user_root = lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "packages";
    }
    options.project_root = std::filesystem::current_path() / ".lubancode" / "packages";
    if (const auto official = lubancode::platform::OfficialPackagesDir();
        official.has_value()) {
        options.official_root = lubancode::platform::Utf8ToPath(*official);
    }
    if (ctx.dev_package_dirs != nullptr) {
        for (const std::string& dir : *ctx.dev_package_dirs) {
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
std::vector<lubancode::package::PackageCandidate> BuildStoreCandidates(const PackageCommandContext& ctx) {
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
    const PackageCommandContext& ctx, const lubancode::package::ScanOptions& options) {
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

// 一只包在 list 主表里的 id 单元格:版本随 id(旧输出同款连排)。
std::string PackageIdCell(const lubancode::package::PackageInventory& inventory) {
    std::string cell = inventory.package_id;
    if (!inventory.version_text.empty()) {
        cell += " " + inventory.version_text;
    }
    return cell;
}

// 一只包在 list 主表里的 state 单元格(含启停合并口径):valid → disabled
// 盖掉;shadowed/invalid 一类另注一笔,两头都看得见。
std::string PackageStateCell(const lubancode::package::PackageInventory& inventory,
                             const std::string& state,
                             const lubancode::package::PackageStateStore* state_store) {
    const bool disabled =
        state_store != nullptr && !state_store->IsEnabled(inventory.package_id);
    if (disabled) {
        return state == "valid" ? "disabled" : state + ";disabled";
    }
    return state;
}

// state 单元格的语义色(批 4):valid → pass;invalid → fail(走 error);
// shadowed/disabled → skip。
frame::CellTone PackageStateTone(const std::string& state_cell) {
    if (state_cell.find("invalid") != std::string::npos) {
        return frame::CellTone::Fail;
    }
    if (state_cell == "valid") {
        return frame::CellTone::Pass;
    }
    return frame::CellTone::Skip;
}

// 一只包在 list 明细表里的长文本(挂载状态/组件计数/信任)。mount 非空时
// 按会话钉快照如实标挂载状态(阶段 3):内容组件挂了几件、代码组件的门禁
//(阶段 4)、现扫到但会话没挂的(启动后才放进目录)注明下回启动才见。
// trust 非空且 code-bearing 时另附一句现查的信任状态(阶段 4)。
std::string PackageDetailCell(const lubancode::package::PackageInventory& inventory,
                              const lubancode::package::PackageMount* mount,
                              const lubancode::package::PackageTrustStore* trust,
                              const lubancode::package::PackageStateStore* state_store) {
    const bool disabled =
        state_store != nullptr && !state_store->IsEnabled(inventory.package_id);
    std::ostringstream out;
    if (mount != nullptr) {
        if (const auto* mounted = mount->Find(inventory.package_id)) {
            out << "已挂载内容组件 " << mounted->mounted_canonical_ids.size() << " 件";
            if (disabled) {
                out << "(已停用;本会话快照钉着启动那折,在跑的不拆,下回装配不再挂)";
            }
            if (mounted->code_trust == lubancode::package::CodeTrustStatus::PendingTrust) {
                out << ";Plugin/MCP/Channel 待信任门";
            } else if (mounted->code_trust == lubancode::package::CodeTrustStatus::Trusted) {
                out << ";Plugin/MCP/Channel 已过信任门(挂载事务随会话启动跑,整包成整包败)";
            }
            out << "  ";
        } else if (inventory.valid) {
            out << (disabled ? "已停用(挂载跳过,连内容组件一件不挂)  "
                             : "未挂载(会话启动后才见;下回启动生效)  ");
        }
    }
    out << "agents:" << inventory.agents.size()
        << " prompts:" << inventory.prompt_profiles.size()
        << " skills:" << inventory.skills.size()
        << " workflows:" << inventory.workflows.size()
        << " plugins:" << inventory.plugins.size()
        << " mcp:" << inventory.mcp_servers.size()
        << " channels:" << inventory.channels.size();
    if (inventory.code_bearing()) {
        out << "  [code-bearing]";
        // 明细里信任一句与主表同行(旧输出第二行的内容并进同一单元格,信息
        // 一字不丢)。
        out << "  信任: " << lubancode::package::DescribeTrustStatus(inventory, trust);
    }
    return out.str();
}

void PrintDiagnostics(const lubancode::cli::Theme& theme,
                      const lubancode::package::PackageInventory& inventory) {
    if (inventory.diagnostics.empty()) {
        PrintNotice(theme, {"诊断:无(干净)"});
        return;
    }
    std::vector<frame::Field> fields{frame::Field{"诊断", std::to_string(inventory.diagnostics.size()) + " 条"}};
    for (const auto& diagnostic : inventory.diagnostics) {
        fields.push_back(frame::Field{"", diagnostic.Format()});
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), PackageFrameWidth()));
}

void PrintComponents(const lubancode::cli::Theme& theme,
                     const lubancode::package::PackageInventory& inventory) {
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
        {"channels", &inventory.channels},
    };
    std::vector<frame::Field> fields;
    for (const auto& group : groups) {
        // 组头:group 名作 key,件数 "(N)" 作 value(旧文案 "agents(N):" 拆
        // 冒号);空组旧文案是 "agents(0): 无",value 记 "(0) 无"。条目是
        // 组下的续行(key 空,值列与七组对齐)。
        std::string head = "(" + std::to_string(group.items->size()) + ")";
        if (group.items->empty()) {
            head += " 无";
        }
        fields.push_back(frame::Field{group.label, head});
        for (const auto& component : *group.items) {
            fields.push_back(frame::Field{"", component.canonical_id + "  (" + component.rel_path + ")"});
        }
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), PackageFrameWidth()));
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
lubancode::package::ExternalNamespaces BuildExternalNamespaces(const PackageCommandContext& ctx) {
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

void PrintUsage(const lubancode::cli::Theme& theme) {
    // 用法块进键值对框(批 4):逐行作 value 行,续行的缩进由值列对齐顶替。
    PrintNotice(theme,
                {"用法: /package list [all|user|project|store|official|dev]",
                 "/package show <id>",
                 "/package doctor <id|路径>",
                 "/package trust <id>    批准整包内容哈希(重启生效;文件一改即失效)",
                 "/package untrust <id>  销信任账",
                 "/package enable <id>   复启(下回启动或 reload 后的装配生效)",
                 "/package disable <id>  停用(挂载一律跳过;扫描发现照旧)",
                 "/package reload        重扫五路折新快照(折好才换;code 组件须新会话)",
                 "只读:list/show 只查静态账;doctor 另诊组件(逐件原生 parser)、引用解析与"
                 "MountPlan 摘要。trust 亮全份审批材料(逐件命令面 + 完整指纹)才落账——"
                 "未信任的 code 组件(Plugin/MCP)一件不挂不启动。启停账在包外"
                 "(~/.lubancode/package-state.json),enable/disable 只落账不拆在跑的。",
                 "store 一路是自进化闭环装进 package-store 的选中版本(/evolve approve 落架,"
                 "/evolve use 点灰度)。"});
    TermOut().flush();
}

void RunPackageList(const lubancode::cli::Theme& theme,
                    const lubancode::package::ScanOptions& options,
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
        PrintNotice(theme, {"警告: " + *trust_error}, frame::FieldAccent::Error);
    }
    const lubancode::package::PackageTrustStore* trust =
        trust_store.has_value() ? &*trust_store : nullptr;
    auto [state_store, state_error] = LoadStateStoreReadOnly();
    if (state_error.has_value()) {
        PrintNotice(theme, {"警告: " + *state_error}, frame::FieldAccent::Error);
    }
    const lubancode::package::PackageStateStore* state =
        state_store.has_value() ? &*state_store : nullptr;
    // 主表:id/scope/state 三列,state 列上 pass/fail/skip 语义色(批 4)。
    // 长字段(挂载/组件计数/信任)另起明细表——塞主表会被列帽截断(批 2
    // "主表+明细表"同款)。
    std::vector<frame::TableColumn> columns;
    columns.push_back({"id"});
    columns.push_back({"scope"});
    columns.push_back({"state"});
    std::vector<frame::TableRow> main_rows;
    std::vector<frame::TableColumn> detail_columns;
    detail_columns.push_back({"id"});
    detail_columns.push_back({"detail"});
    std::vector<frame::TableRow> detail_rows;
    for (const auto& row : rows) {
        if (!scope_matches(row)) continue;
        ++shown;
        const std::string state_text =
            row.shadowed ? "shadowed(被 " + row.shadowed_by + " 遮住)"
                         : (row.inventory.valid ? "valid" : "invalid");
        const std::string state_cell = PackageStateCell(row.inventory, state_text, state);
        main_rows.push_back(frame::TableRow{
            {PackageIdCell(row.inventory),
             lubancode::package::ScopeToString(row.inventory.scope), state_cell},
            {frame::CellTone::Normal, frame::CellTone::Normal, PackageStateTone(state_cell)}});
        detail_rows.push_back(frame::TableRow{
            {PackageIdCell(row.inventory), PackageDetailCell(row.inventory, mount, trust, state)}});
    }
    if (shown == 0) {
        PrintNotice(theme, {"(没有可列的包;放一只 <package-root>/package.yaml 进四层任一层即被发现)"});
        TermOut().flush();
        return;
    }
    EmitFrameLines(frame::RenderTable(
        "Package(五路: dev > project > store > user > official;store 是自进化装架)", columns,
        main_rows, theme, frame::Light(), PackageFrameWidth()));
    EmitFrameLines(
        frame::RenderTable({}, detail_columns, detail_rows, theme, frame::Light(), PackageFrameWidth()));
    TermOut().flush();
}

void RunPackageShow(const lubancode::cli::Theme& theme,
                    const lubancode::package::ScanOptions& options, const std::string& target,
                    const lubancode::package::PackageMount* mount,
                    const std::vector<lubancode::package::PackageCandidate>& candidates) {
    const PackageLookup lookup = LookupPackage(candidates, target);
    if (lookup.winner == nullptr && lookup.shadowed.empty()) {
        PrintNotice(theme, {"没找到包 \"" + target + "\"(按 id 或目录名查;先 /package list 看全账)"},
                    frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }
    const auto inventory = lubancode::package::BuildPackageInventory(*lookup.winner, options);
    auto [trust_store, trust_error] = LoadTrustStoreReadOnly();
    if (trust_error.has_value()) {
        PrintNotice(theme, {"警告: " + *trust_error}, frame::FieldAccent::Error);
    }
    const lubancode::package::PackageTrustStore* trust =
        trust_store.has_value() ? &*trust_store : nullptr;
    auto [state_store, state_error] = LoadStateStoreReadOnly();
    if (state_error.has_value()) {
        PrintNotice(theme, {"警告: " + *state_error}, frame::FieldAccent::Error);
    }
    const lubancode::package::PackageStateStore* state =
        state_store.has_value() ? &*state_store : nullptr;
    const bool disabled = state != nullptr && !state->IsEnabled(inventory.package_id);
    // 头部键值对框:标题 = id + 版本;状态/来源/启停等按句内冒号拆列。
    std::vector<frame::Field> fields;
    fields.push_back(frame::Field{
        "状态", (inventory.valid ? "valid" : "invalid") + (disabled ? "(已停用)" : "") +
                    (inventory.manifest_ok ? "" : "(根清单解析失败)") +
                    (inventory.code_bearing() ? "  [code-bearing]" : "")});
    fields.push_back(frame::Field{
        "来源", "[" + lubancode::package::ScopeToString(inventory.scope) + "] " +
                    lubancode::platform::PathToUtf8(inventory.package_root)});
    fields.push_back(frame::Field{"启停", lubancode::package::DescribeStateStatus(inventory, state)});
    if (inventory.code_bearing()) {
        fields.push_back(frame::Field{"信任", lubancode::package::DescribeTrustStatus(inventory, trust)});
    }
    if (mount != nullptr) {
        if (const auto* mounted = mount->Find(inventory.package_id)) {
            std::string value = "内容组件 " + std::to_string(mounted->mounted_canonical_ids.size()) +
                                " 件已挂(canonical id 见下;会话钉快照)";
            if (mounted->code_trust == lubancode::package::CodeTrustStatus::PendingTrust) {
                value += ";Plugin/MCP 待信任门,一件不挂不执行";
            } else if (mounted->code_trust == lubancode::package::CodeTrustStatus::Trusted) {
                value += ";Plugin/MCP 已过信任门(挂载事务随会话启动跑,整包成整包败)";
            }
            if (disabled) {
                value += ";已停用,本会话照旧跑完,下回装配不再挂";
            }
            fields.push_back(frame::Field{"挂载", value});
        } else if (inventory.valid) {
            fields.push_back(frame::Field{
                "挂载", disabled ? "已停用(挂载跳过,连内容组件一件不挂)"
                                 : "本会话未挂(启动后才放进目录;下回启动生效)"});
        } else {
            fields.push_back(frame::Field{"挂载", "无效包,一件不挂(整包成整包败)"});
        }
    }
    fields.push_back(frame::Field{"内容哈希", inventory.content_hash + "  (盘点文件 " +
                                                       std::to_string(inventory.total_file_count) +
                                                       " 个)"});
    if (!lookup.shadowed.empty()) {
        fields.push_back(frame::Field{"被遮住的候选", "(" + std::to_string(lookup.shadowed.size()) + " 份)"});
        for (const auto* shadow : lookup.shadowed) {
            fields.push_back(frame::Field{"", "[" + lubancode::package::ScopeToString(shadow->scope) +
                                                 "] " + lubancode::platform::PathToUtf8(shadow->package_root)});
        }
    }
    EmitFrameLines(frame::RenderKeyValues(
        inventory.package_id +
            (inventory.version_text.empty() ? "" : " " + inventory.version_text),
        fields, theme, frame::Light(), PackageFrameWidth()));
    PrintComponents(theme, inventory);
    PrintDiagnostics(theme, inventory);
    TermOut().flush();
}

// doctor 的三段新账:组件逐件诊断、引用解析、MountPlan 摘要(只读)。批 4:
// 组件走表格(kind/id/status/path,status 列 pass/fail 色),附注与 issue 进
// 键值对框;引用与 MountPlan 走键值对框。
void PrintAnalyzedComponents(const lubancode::cli::Theme& theme,
                             const lubancode::package::PackageRecord& record) {
    if (record.components.empty()) {
        PrintNotice(theme,
                    {"组件(0 件,逐件过原生 parser;坏件照列,不因第一个错停)",
                     "(没有组件;七类目录里没有可认的件)"});
        return;
    }
    std::vector<frame::TableColumn> columns;
    columns.push_back({"kind"});
    columns.push_back({"id"});
    columns.push_back({"status"});
    columns.push_back({"path"});
    std::vector<frame::TableRow> rows;
    std::vector<frame::Field> notes;
    for (const auto& component : record.components) {
        const bool has_error = component.HasError();
        const std::string status = has_error ? "[error]" : (component.ok ? "ok" : "[error]");
        rows.push_back(frame::TableRow{
            {std::string(lubancode::package::ComponentKindName(component.kind)),
             component.canonical_id, status, component.rel_path},
            {frame::CellTone::Normal, frame::CellTone::Normal,
             has_error ? frame::CellTone::Fail : frame::CellTone::Pass}});
        // 附注(工具数/权限概要/覆盖文件)挂 canonical id 标签对回主表行
        //(批 2 hooks 的 stderr 附注同款);issue 是坏件的明账,同随。
        if (component.kind == lubancode::package::ComponentKind::Plugin && component.plugin.has_value()) {
            std::string note = "(" + std::to_string(component.plugin->tools.size()) + " 件工具)";
            // v2 embedded-lua 的权限概要(§13.5:/plugin 与 /package 摆同一份
            // 权限真账;完整材料看 /package trust 的审批页)。
            if (component.plugin->manifest_version == lubancode::runtime::kPluginManifestVersionV2) {
                note += "  [embedded-lua entry " + component.plugin->runtime_entry + ";网络 " +
                        std::to_string(component.plugin->network_permissions.size()) +
                        " 目的地;Secret " +
                        std::to_string(component.plugin->secret_declarations.size()) + " 件]";
            }
            notes.push_back(frame::Field{component.canonical_id, note});
        }
        if (component.kind == lubancode::package::ComponentKind::PromptProfile &&
            !component.profile_files.empty()) {
            notes.push_back(frame::Field{component.canonical_id,
                                         "(" + std::to_string(component.profile_files.size()) +
                                             " 个覆盖文件)"});
        }
        for (const auto& issue : component.issues) {
            notes.push_back(frame::Field{component.canonical_id, issue.Format(),
                                         frame::FieldAccent::Error});
        }
    }
    EmitFrameLines(frame::RenderTable(
        "组件(" + std::to_string(record.components.size()) +
            " 件,逐件过原生 parser;坏件照列,不因第一个错停)",
        columns, rows, theme, frame::Light(), PackageFrameWidth()));
    if (!notes.empty()) {
        EmitFrameLines(
            frame::RenderKeyValues({}, notes, theme, frame::Light(), PackageFrameWidth()));
    }
}

void PrintReferences(const lubancode::cli::Theme& theme,
                     const lubancode::package::PackageRecord& record) {
    if (record.references.empty()) {
        PrintNotice(theme, {"引用解析(0 条)", "(没有包内引用)"});
        return;
    }
    std::vector<frame::Field> fields{frame::Field{"引用解析", std::to_string(record.references.size()) + " 条"}};
    for (const auto& ref : record.references) {
        fields.push_back(frame::Field{"", ref.Format()});
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), PackageFrameWidth()));
}

void PrintMountPlan(const lubancode::cli::Theme& theme,
                    const lubancode::package::PackageRecord& record) {
    if (!record.mount_plan.has_value()) {
        PrintNotice(theme, {"MountPlan: 不产(整包 invalid,一件也不挂——整包成整包败)"},
                    frame::FieldAccent::Error);
        return;
    }
    const auto& plan = *record.mount_plan;
    std::vector<frame::Field> fields;
    {
        std::string value = std::to_string(plan.entries.size()) + " 件待挂:agent " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::Agent)) +
                            " / prompt " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::PromptProfile)) +
                            " / skill " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::Skill)) +
                            " / workflow " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::Workflow)) +
                            " / plugin " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::Plugin)) +
                            " / mcp " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::McpServer)) +
                            " / channel " +
                            std::to_string(plan.CountKind(lubancode::package::ComponentKind::Channel));
        if (plan.HasCodeBearing()) {
            value += "  [code-bearing," +
                     std::string(lubancode::package::CodeTrustStatusText(plan.code_trust)) + "]";
        }
        fields.push_back(frame::Field{"MountPlan(只读计划,不启动 Plugin 与 MCP)", value});
    }
    for (const auto& entry : plan.entries) {
        std::string line = entry.canonical_id + " -> " + entry.target_table + "  (源 " +
                           entry.source_root + ")";
        if (entry.code_bearing) {
            line += std::string("  [") + (entry.trusted ? "已信任" : "待信任") + "]";
        }
        fields.push_back(frame::Field{"", line});
        for (const auto& tool : entry.tools) {
            fields.push_back(frame::Field{"", "工具 " + tool.wire_name + "  展示 " + tool.display_name});
        }
        if (!entry.depends_on.empty()) {
            std::string joined;
            for (const auto& dep : entry.depends_on) {
                if (!joined.empty()) joined += ", ";
                joined += dep;
            }
            fields.push_back(frame::Field{"", "依赖 " + joined});
        }
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), PackageFrameWidth()));
}

void RunPackageDoctor(const lubancode::cli::Theme& theme,
                      const lubancode::package::ScanOptions& options,
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
        PrintNotice(theme, {"警告: " + *trust_error}, frame::FieldAccent::Error);
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
            PrintNotice(theme, {"没找到包 \"" + target + "\"(doctor 收 id 或包路径)"},
                        frame::FieldAccent::Error);
            TermOut().flush();
            return;
        }
        record = lubancode::package::AnalyzePackage(*lookup.winner, options, ref_index, external,
                                                    &trust_snapshot);
    }

    const auto& inventory = record.inventory;
    std::vector<frame::Field> fields;
    fields.push_back(frame::Field{
        "根清单", (inventory.manifest_ok ? "解析通过(schema 1, SemVer)" : "解析失败") +
                      (inventory.version_text.empty() ? "" : ",version " + inventory.version_text)});
    if (options.current_lubancode.has_value()) {
        fields.push_back(frame::Field{"LubanCode", "当前 " + options.current_lubancode->text +
                                                       ",平台 " + options.current_platform +
                                                       "(写了 compatibility 就检查)"});
    }
    fields.push_back(frame::Field{
        "包根", "[" + lubancode::package::ScopeToString(inventory.scope) + "] " +
                    lubancode::platform::PathToUtf8(inventory.package_root)});
    // 启停一行(阶段 6,doctor 表的"信任、启停与 runtime"项):现查启停账。
    {
        auto [state_store, state_error] = LoadStateStoreReadOnly();
        if (state_error.has_value()) {
            PrintNotice(theme, {"警告: " + *state_error}, frame::FieldAccent::Error);
        }
        fields.push_back(frame::Field{
            "启停", lubancode::package::DescribeStateStatus(
                        inventory, state_store.has_value() ? &*state_store : nullptr)});
    }
    fields.push_back(frame::Field{
        "盘点", "文件 " + std::to_string(inventory.total_file_count) + "(assets " +
                    std::to_string(inventory.assets_file_count) + " / docs " +
                    std::to_string(inventory.docs_file_count) + " / code-bearing " +
                    std::to_string(inventory.code_bearing_file_count) + "),内容哈希 " +
                    inventory.content_hash});
    EmitFrameLines(frame::RenderKeyValues("诊断 " + inventory.package_id, fields, theme,
                                          frame::Light(), PackageFrameWidth()));
    PrintDiagnostics(theme, inventory);
    PrintAnalyzedComponents(theme, record);
    PrintReferences(theme, record);
    PrintMountPlan(theme, record);
    PrintNotice(theme, {"结论: " + std::string(record.valid
                                                    ? "valid(静态账干净;挂载是后续阶段的事)"
                                                    : "invalid(按清单修好再看)")});
    TermOut().flush();
}

// /package trust|untrust <id>(阶段 4):扫描定胜者 -> AnalyzePackage 出全份
// 材料与状态 -> 账务(TrustPackage/UntrustPackage,回执逐行)。落账即时,
// 生效在重启(会话钉快照,阶段 3 语义)。
void RunPackageTrust(const lubancode::cli::Theme& theme, const PackageCommandContext& ctx,
                     const std::string& target, bool trust_action) {
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    const std::vector<lubancode::package::PackageCandidate> candidates =
        lubancode::package::ScanPackages(options);
    const PackageLookup lookup = LookupPackage(candidates, target);
    if (lookup.winner == nullptr && lookup.shadowed.empty()) {
        PrintNotice(theme, {"没找到包 \"" + target +
                            "\"(trust/untrust 按 id 或目录名查;先 /package list 看全账)"},
                    frame::FieldAccent::Error);
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
            PrintNotice(theme, {"警告: " + *load_error}, frame::FieldAccent::Error);
        }
        store = std::move(loaded);
    }
    const lubancode::package::PackageTrustActionResult result =
        trust_action ? lubancode::package::TrustPackage(record, store.has_value() ? &*store : nullptr)
                     : lubancode::package::UntrustPackage(record,
                                                          store.has_value() ? &*store : nullptr);
    if (!result.ok) {
        PrintNotice(theme, {result.error}, frame::FieldAccent::Error);
    } else {
        PrintNotice(theme, result.lines);
    }
    TermOut().flush();
}

// /package enable|disable <id>(阶段 6):扫描定胜者 -> 轻盘点出身份 ->
// 账务(EnableDisablePackage,回执逐行)。落账即时,生效在下回装配(会话
// 钉快照,不拆在跑的)——回执里如实说,另注明本会话快照还给它挂着几件。
void RunPackageEnableDisable(const lubancode::cli::Theme& theme, const PackageCommandContext& ctx,
                             const std::string& target, bool enable) {
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    const std::vector<lubancode::package::PackageCandidate> candidates =
        ScanAllLayers(ctx, options);
    const PackageLookup lookup = LookupPackage(candidates, target);
    if (lookup.winner == nullptr && lookup.shadowed.empty()) {
        PrintNotice(theme, {"没找到包 \"" + target +
                            "\"(enable/disable 按 id 或目录名查;先 /package list 看全账)"},
                    frame::FieldAccent::Error);
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
            PrintNotice(theme, {"警告: " + *load_error}, frame::FieldAccent::Error);
        }
        store = std::move(loaded);
    }
    // 本会话快照给它挂着几件(disable 回执注明"在跑的照旧")。挂载账从
    // provider 现取现行快照(HC-06 第三小批:reload 后旧快照会释放,不冻
    // 指针;本函数持 shared_ptr 期间保活)。
    int mounted_count = -1;
    const std::shared_ptr<const lubancode::package::PackageSnapshot> snapshot =
        ctx.package_snapshot_provider ? ctx.package_snapshot_provider() : nullptr;
    if (snapshot != nullptr) {
        if (const auto* mounted = snapshot->mount().Find(inventory.package_id)) {
            mounted_count = static_cast<int>(mounted->mounted_canonical_ids.size());
        } else {
            mounted_count = 0;
        }
    }
    const lubancode::package::PackageStateActionResult result = lubancode::package::EnableDisablePackage(
        inventory, store.has_value() ? &*store : nullptr, enable, mounted_count);
    if (!result.ok) {
        PrintNotice(theme, {result.error}, frame::FieldAccent::Error);
    } else {
        PrintNotice(theme, result.lines);
    }
    TermOut().flush();
}

// /package reload(阶段 6):会话侧重折快照 + 原子换档 + 刷下游。回执行
//(含折不动的诊断)逐行打印;没接会话执行体(纯函数装配)如实明说。
void RunPackageReload(const lubancode::cli::Theme& theme, const PackageCommandContext& ctx) {
    if (ctx.reload_packages == nullptr) {
        PrintNotice(theme,
                    {"这个装配没接会话 reload 口(单发/无交互栈),折不了新快照。",
                     "重启会话即可按最新目录与启停账装配。"});
        TermOut().flush();
        return;
    }
    PrintNotice(theme, ctx.reload_packages());
    TermOut().flush();
}

}  // namespace

CommandFlow HandleSlashPackage(const PackageCommandContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed) {
    const ParsedPackageCommand command = ParsePackageCommand(parsed.args);
    // 批 4:渲染段走 frame;theme 空指针按 /plugin 先例退 plain。
    const lubancode::cli::Theme theme = ctx.theme != nullptr ? *ctx.theme : lubancode::cli::Theme{};
    const lubancode::package::ScanOptions options = BuildScanOptions(ctx);
    const std::vector<lubancode::package::PackageCandidate> candidates = ScanAllLayers(ctx, options);
    // 挂载账从 provider 现取现行快照(HC-06 第三小批:reload 换档后旧快照
    // 会释放,冻指针会悬垂;本函数持 shared_ptr 期间保活,与旧"会话侧重
    // 指"口径一致)。空 = 没有包。
    const std::shared_ptr<const lubancode::package::PackageSnapshot> package_snapshot =
        ctx.package_snapshot_provider ? ctx.package_snapshot_provider() : nullptr;
    const lubancode::package::PackageMount* const package_mount =
        package_snapshot != nullptr ? &package_snapshot->mount() : nullptr;
    switch (command.action) {
        case PackageCommandAction::List:
            RunPackageList(theme, options, command.scope_filter, package_mount, candidates);
            return CommandFlow::Continue;
        case PackageCommandAction::Show:
            RunPackageShow(theme, options, command.target, package_mount, candidates);
            return CommandFlow::Continue;
        case PackageCommandAction::Doctor:
            RunPackageDoctor(theme, options, BuildExternalNamespaces(ctx), command.target,
                             candidates);
            return CommandFlow::Continue;
        case PackageCommandAction::Trust:
            RunPackageTrust(theme, ctx, command.target, /*trust_action=*/true);
            return CommandFlow::Continue;
        case PackageCommandAction::Untrust:
            RunPackageTrust(theme, ctx, command.target, /*trust_action=*/false);
            return CommandFlow::Continue;
        case PackageCommandAction::Enable:
            RunPackageEnableDisable(theme, ctx, command.target, /*enable=*/true);
            return CommandFlow::Continue;
        case PackageCommandAction::Disable:
            RunPackageEnableDisable(theme, ctx, command.target, /*enable=*/false);
            return CommandFlow::Continue;
        case PackageCommandAction::Reload:
            RunPackageReload(theme, ctx);
            return CommandFlow::Continue;
        case PackageCommandAction::Invalid:
            PrintNotice(theme, {"认不得 \"" + command.bad_word + "\"。"}, frame::FieldAccent::Error);
            PrintUsage(theme);
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
