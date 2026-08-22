// WorkflowCatalog 实现(自然语言编排单第 1 批)。

#include "workflow/catalog.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "platform/paths.hpp"

namespace lubancode::workflow {

namespace {

// UTF-8 解码一个码点;非法序列返回 false(pos 不推进)。
bool NextCodePoint(const std::string& s, std::size_t& pos, std::uint32_t& cp) {
    if (pos >= s.size()) return false;
    const auto b0 = static_cast<unsigned char>(s[pos]);
    if (b0 < 0x80) {
        cp = b0;
        pos += 1;
        return true;
    }
    int extra = 0;
    std::uint32_t value = 0;
    if ((b0 & 0xE0) == 0xC0) { extra = 1; value = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; value = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; value = b0 & 0x07; }
    else return false;
    if (pos + static_cast<std::size_t>(extra) >= s.size()) return false;
    for (int i = 1; i <= extra; ++i) {
        const auto bi = static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
        if ((bi & 0xC0) != 0x80) return false;
        value = (value << 6) | (bi & 0x3F);
    }
    cp = value;
    pos += static_cast<std::size_t>(extra) + 1;
    return true;
}

void ScanScope(const std::filesystem::path& root, WorkflowScope scope, std::vector<CatalogEntry>& out) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;
    std::vector<std::filesystem::path> dirs;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_directory(ec)) dirs.push_back(entry.path());
        ec.clear();
    }
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return lubancode::platform::PathToUtf8(a) < lubancode::platform::PathToUtf8(b);
    });
    for (const auto& dir : dirs) {
        CatalogEntry item;
        item.scope = scope;
        item.dir = dir;
        auto parsed = LoadWorkflowDefinition(dir / "workflow.yaml");
        if (parsed.has_value()) {
            item.definition = std::move(*parsed);
            item.content_hash = ContentHash(item.definition);
        } else {
            item.broken = true;
            item.issues = std::move(parsed.error());
            // 目录名当 id 兜底,list 至少能指着它说话。
            item.definition.id = lubancode::platform::PathToUtf8(dir.filename());
        }
        out.push_back(std::move(item));
    }
}

}  // namespace

std::string ToString(WorkflowScope scope) {
    return scope == WorkflowScope::Project ? "project" : "home";
}

bool IsValidAlias(const std::string& alias) {
    if (alias.empty() || alias.size() > 64) return false;
    std::size_t pos = 0;
    std::uint32_t cp = 0;
    while (pos < alias.size()) {
        if (!NextCodePoint(alias, pos, cp)) return false;
        if (cp < 0x80) {
            const bool ok = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
                            cp == '-' || cp == '_';
            if (!ok) return false;
        } else {
            // 非 ASCII:排除控制区(C0/C1)、空白与全角空白。其余(含中文、
            // 假名、重音字母)按"正常文字"放行。
            if (cp < 0xA0 || cp == 0x2028 || cp == 0x2029 || cp == 0x3000) return false;
        }
    }
    return true;
}

bool IsValidWorkflowId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

const CatalogEntry* Catalog::Find(const std::string& id) const {
    // entries 里项目级遮用户级:同 id 只留项目那份(LoadCatalog 已处理),
    // 顺序找第一个即对。
    for (const auto& entry : entries) {
        if (!entry.broken && entry.definition.id == id) return &entry;
    }
    return nullptr;
}

const CatalogEntry* Catalog::FindByAlias(const std::string& alias) const {
    if (disabled_aliases.count(alias) > 0) return nullptr;
    for (const auto& entry : entries) {
        if (!entry.broken && entry.definition.enabled && entry.definition.alias == alias) return &entry;
    }
    return nullptr;
}

void DetectAliasConflicts(Catalog& catalog, const std::vector<std::string>& skill_names,
                          const std::vector<std::string>& builtin_slash_names) {
    std::map<std::string, int> alias_owner_count;
    std::map<std::string, std::string> alias_first_owner;
    for (const auto& entry : catalog.entries) {
        if (entry.broken || entry.definition.alias.empty()) continue;
        const std::string& alias = entry.definition.alias;
        if (!IsValidAlias(alias)) {
            catalog.disabled_aliases[alias] = "alias 含空白/斜杠/控制符等非法字符";
            continue;
        }
        ++alias_owner_count[alias];
        alias_first_owner.emplace(alias, entry.definition.id);
    }
    for (const auto& [alias, count] : alias_owner_count) {
        if (count > 1) {
            catalog.conflicts.push_back(AliasConflict{alias, "workflow", alias_first_owner[alias] + " 等 " +
                                                                      std::to_string(count) + " 份"});
            catalog.disabled_aliases[alias] = "多份 workflow 共用同一 alias";
        }
    }
    for (const auto& entry : catalog.entries) {
        if (entry.broken || entry.definition.alias.empty()) continue;
        const std::string& alias = entry.definition.alias;
        for (const auto& skill : skill_names) {
            if (alias == skill) {
                catalog.conflicts.push_back(AliasConflict{alias, "skill", skill});
                catalog.disabled_aliases[alias] = "与 skill 撞名: " + skill;
            }
        }
        for (const auto& builtin : builtin_slash_names) {
            std::string name = builtin;
            if (!name.empty() && name[0] == '/') name = name.substr(1);
            if (alias == name) {
                catalog.conflicts.push_back(AliasConflict{alias, "builtin", builtin});
                catalog.disabled_aliases[alias] = "与内建 slash 命令撞名: " + builtin;
            }
        }
    }
}

Catalog LoadCatalog(const std::optional<std::filesystem::path>& project_root,
                    const std::optional<std::filesystem::path>& user_root) {
    Catalog catalog;
    std::vector<CatalogEntry> project_entries;
    std::vector<CatalogEntry> user_entries;
    if (project_root.has_value()) {
        ScanScope(*project_root / ".lubancode" / "workflows", WorkflowScope::Project, project_entries);
    }
    if (user_root.has_value()) {
        ScanScope(*user_root / ".lubancode" / "workflows", WorkflowScope::User, user_entries);
    }
    // 项目级遮用户级:同 id 只留项目份,但被遮的那份要在 conflicts 里露脸
    // (单子:须在 list/show 标来源,不能静默遮住)。
    std::set<std::string> project_ids;
    for (const auto& entry : project_entries) {
        if (!entry.broken) project_ids.insert(entry.definition.id);
    }
    for (auto& entry : user_entries) {
        if (!entry.broken && project_ids.count(entry.definition.id) > 0) {
            catalog.conflicts.push_back(AliasConflict{entry.definition.id, "shadowed",
                                                      "项目级同 id 遮住用户级(已按规矩取项目级)"});
            continue;
        }
        catalog.entries.push_back(std::move(entry));
    }
    for (auto& entry : project_entries) {
        catalog.entries.push_back(std::move(entry));
    }
    // 项目在前,稳住 Find 的顺序假设。
    std::stable_sort(catalog.entries.begin(), catalog.entries.end(),
                     [](const CatalogEntry& a, const CatalogEntry& b) {
                         return a.scope == WorkflowScope::Project && b.scope == WorkflowScope::User;
                     });
    return catalog;
}

}  // namespace lubancode::workflow
