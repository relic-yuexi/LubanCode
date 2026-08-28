// AgentCatalog 的实现:三层扫描、稳定排序、重名检查、覆盖账。
#include "agent/agent_catalog.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "platform/paths.hpp"

namespace lubancode::agent {

namespace {

// 一层扫描出来的一条原始记录(还没跨层合并)。
struct LayerRecord {
    std::string name;  // 解析成功的定义名;失败退回文件 stem
    std::optional<AgentDefinition> definition;
    std::vector<AgentDefinitionIssue> issues;
    AgentSourceLayer layer;
    std::string file;       // UTF-8 路径;码内注册的写 "(builtin)"
    bool conflict = false;  // 同层重名:整组不可用,不静默挑一个
};

std::string LayerLabel(AgentSourceLayer layer) {
    switch (layer) {
        case AgentSourceLayer::Builtin: return "builtin";
        case AgentSourceLayer::User: return "user";
        case AgentSourceLayer::Project: return "project";
    }
    return "?";
}

// 只认本层目录下的 *.yaml(单子 4.1:一 Agent 一文件,不套子目录、不递归
// ——那会把 Skill/Plugin 的家当混进来)。文件名按节序排好再解析,枚举顺序
// 无关紧要(单子测试账"扫描次序确定")。目录不存在 = 这层没有,静默空手。
std::vector<std::filesystem::path> CollectYamlFiles(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::filesystem::path normalized = dir.lexically_normal();
    if (!std::filesystem::is_directory(normalized, ec) || ec) {
        return {};
    }
    std::vector<std::pair<std::string, std::filesystem::path>> found;
    for (const auto& entry : std::filesystem::directory_iterator(normalized, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path path = entry.path();
        if (path.extension() != ".yaml") {
            continue;
        }
        found.emplace_back(platform::PathToUtf8(path.filename()), path);
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::filesystem::path> out;
    out.reserve(found.size());
    for (auto& [name, path] : found) {
        out.push_back(std::move(path));
    }
    return out;
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 同层重名检查(单子 4.2/6.2):按生效名分组,同组全部标 conflict、各记
// 一条冲突 error,来源清单进 load_errors——报冲突来源,不静默挑一个。
void ReportSameLayerConflicts(std::vector<LayerRecord>& records, std::vector<std::string>& load_errors) {
    std::unordered_map<std::string, std::vector<std::size_t>> by_name;
    for (std::size_t i = 0; i < records.size(); ++i) {
        by_name[records[i].name].push_back(i);
    }
    for (auto& [name, indexes] : by_name) {
        if (indexes.size() < 2) {
            continue;
        }
        std::string sources;
        for (const std::size_t index : indexes) {
            sources += sources.empty() ? records[index].file : ("; " + records[index].file);
        }
        for (const std::size_t index : indexes) {
            records[index].conflict = true;
            records[index].issues.push_back(AgentDefinitionIssue{
                "name", "同层重名 \"" + name + "\":" + sources + "(须改名或删掉一份)", -1, -1, false});
        }
        load_errors.push_back(LayerLabel(records[indexes.front()].layer) + " 层重名 \"" + name + "\":" + sources);
    }
}

// 扫一层磁盘目录:逐文件解析成 LayerRecord(不做重名检查——builtin 层要
// 与码内记录合并后再查,单独查会重复报账)。
std::vector<LayerRecord> ParseLayerFiles(const std::optional<std::filesystem::path>& dir,
                                         AgentSourceLayer layer) {
    std::vector<LayerRecord> records;
    if (!dir.has_value()) {
        return records;
    }
    for (const std::filesystem::path& path : CollectYamlFiles(*dir)) {
        const std::string file_utf8 = platform::PathToUtf8(path);
        const std::string stem = platform::PathToUtf8(path.stem());
        const auto text = ReadFileText(path);
        if (!text.has_value()) {
            records.push_back(LayerRecord{stem, std::nullopt,
                                          {AgentDefinitionIssue{"(file)", "读不到文件内容", -1, -1, false}},
                                          layer, file_utf8});
            continue;
        }
        AgentDefinitionParseResult parsed = ParseAgentDefinitionYaml(*text, file_utf8);
        if (parsed.definition.has_value()) {
            // 名不符:以 name 为准,给 warning(单子 4.2;日后可升为错误)。
            if (parsed.definition->name != stem) {
                parsed.issues.push_back(AgentDefinitionIssue{
                    "name", "文件名 \"" + stem + ".yaml\" 与 name \"" + parsed.definition->name +
                                "\" 不一致,以 name 为准", -1, -1, /*warning=*/true});
            }
            records.push_back(LayerRecord{parsed.definition->name, std::move(parsed.definition),
                                          std::move(parsed.issues), layer, file_utf8});
        } else {
            records.push_back(LayerRecord{stem, std::nullopt, std::move(parsed.issues), layer, file_utf8});
        }
    }
    return records;
}

// 扫一层并查同层重名(user/project 用;builtin 层手工合并码内记录后自查)。
std::vector<LayerRecord> ScanLayer(const std::optional<std::filesystem::path>& dir, AgentSourceLayer layer,
                                   std::vector<std::string>& load_errors) {
    std::vector<LayerRecord> records = ParseLayerFiles(dir, layer);
    ReportSameLayerConflicts(records, load_errors);
    return records;
}

}  // namespace

std::string ToString(AgentSourceLayer layer) {
    return LayerLabel(layer);
}

std::string AgentCatalogEntry::FirstError() const {
    for (const AgentDefinitionIssue& issue : issues) {
        if (!issue.warning) {
            return issue.Format(file);
        }
    }
    return std::string();
}

const AgentCatalogEntry* AgentCatalog::Find(const std::string& name) const {
    for (const AgentCatalogEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const AgentCatalogEntry*> AgentCatalog::Available() const {
    std::vector<const AgentCatalogEntry*> out;
    for (const AgentCatalogEntry& entry : entries) {
        if (entry.available) {
            out.push_back(&entry);
        }
    }
    return out;
}

AgentDefinition BuiltinGeneralPurposeDefinition() {
    AgentDefinition def;
    def.name = "general-purpose";
    // 描述与 src/prompts/tools/<语言>/agent.md 的 persona.general 一口径:
    // 写何时派它出场,不写宣传话。
    def.description = "搜索、分析并完成多步任务;可读写文件、跑命令,默认子代理类型。";
    return def;
}

AgentDefinition BuiltinExploreDefinition() {
    AgentDefinition def;
    def.name = "Explore";  // 名称先保留(单子"兼容与发布"),带大写、不过 kebab-case 闸
    def.description = "快速搜索、阅读并分析代码库的只读代理;不改文件,结论带具体文件位置。";
    // allow 表如实记 AgentTool::ExploreAllows 放行的那五枚(阶段 1 只登账,
    // 不接线,运行时的只读仍由 Explore 专用工具表管)。
    def.tools.allow = {"read_file", "search", "web_fetch", "web_search", "lsp"};
    def.permissions_mode = "read_only";
    return def;
}

AgentCatalog LoadAgentCatalog(const AgentCatalogScanRoots& roots) {
    AgentCatalog catalog;

    // builtin 层 = 码内定义(general-purpose/Explore,单子 6.2 的地板)+
    // 嵌入式资源目录(通常没有);同层重名规矩同样管(磁盘想盖码内的
    // general-purpose 属同层撞车,整组不可用)。
    std::vector<LayerRecord> builtin;
    builtin.push_back(LayerRecord{"general-purpose", BuiltinGeneralPurposeDefinition(), {},
                                  AgentSourceLayer::Builtin, "(builtin)"});
    builtin.push_back(LayerRecord{"Explore", BuiltinExploreDefinition(), {}, AgentSourceLayer::Builtin,
                                  "(builtin)"});
    {
        std::vector<LayerRecord> disk = ParseLayerFiles(roots.builtin_dir, AgentSourceLayer::Builtin);
        builtin.insert(builtin.end(), std::make_move_iterator(disk.begin()),
                       std::make_move_iterator(disk.end()));
        ReportSameLayerConflicts(builtin, catalog.load_errors);
    }
    std::vector<LayerRecord> user = ScanLayer(roots.user_dir, AgentSourceLayer::User, catalog.load_errors);
    std::vector<LayerRecord> project = ScanLayer(roots.project_dir, AgentSourceLayer::Project,
                                                 catalog.load_errors);

    // 合并:同名取最高层(project > user > builtin),所以按优先级从高到低
    // 喂。高层的条目即使不可用也占住名字(显式覆盖,坏了也不许静默退回
    // 低层——单子"unavailable Agent 不得静默退回"同款骨气);低层来源进
    // shadowed_sources,账序 = 优先级从高到低(紧挨其下的层在前)。
    struct MergeSlot {
        const LayerRecord* chosen = nullptr;
        std::vector<const LayerRecord*> shadowed;
    };
    std::map<std::string, MergeSlot> merged;
    const auto feed = [&merged](const std::vector<LayerRecord>& records) {
        for (const LayerRecord& record : records) {
            MergeSlot& slot = merged[record.name];
            if (slot.chosen == nullptr) {
                slot.chosen = &record;
            } else {
                slot.shadowed.push_back(&record);
            }
        }
    };
    feed(project);
    feed(user);
    feed(builtin);

    catalog.entries.reserve(merged.size());
    for (const auto& [name, slot] : merged) {
        AgentCatalogEntry entry;
        entry.name = name;
        entry.layer = slot.chosen->layer;
        entry.file = slot.chosen->file;
        entry.definition = slot.chosen->definition;
        entry.issues = slot.chosen->issues;
        entry.available = slot.chosen->definition.has_value() && !slot.chosen->conflict;
        for (const LayerRecord* shadow : slot.shadowed) {
            entry.shadowed_sources.push_back(shadow->file);
        }
        catalog.entries.push_back(std::move(entry));
    }
    // entries 天然按 name 排(std::map 节序);再走一遍稳定排序把这条写成
    // 显式规矩,防后人改容器。
    std::stable_sort(catalog.entries.begin(), catalog.entries.end(),
                     [](const AgentCatalogEntry& a, const AgentCatalogEntry& b) { return a.name < b.name; });
    return catalog;
}

}  // namespace lubancode::agent
