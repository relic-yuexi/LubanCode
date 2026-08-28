// /evolve 命令的执行体(自进化闭环阶段 1)。status = 扫五路账本采观察 +
// 落观察账(只追加)+ 报账面;list = 按指纹聚类列账;show = 看一条观察,
// 指回来源 ID 与原始账文件。不生成 Package、不装任何东西、不改各家源账。
#include "app/commands/evolve_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:统一走输出端口

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include "evolution/collector.hpp"
#include "evolution/observation_store.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;

namespace {

std::string Trimmed(std::string s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
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

// 观察账根:<home>/.lubancode/evolution/observations(README"观察账"节)。
std::filesystem::path BuildStoreRoot(SlashDispatchContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "evolution" / "observations";
}

// 五路账本的输入装配:全从分派材料借,缺哪路就空着哪路(采集器对空根自然跳过)。
lubancode::evolution::CollectSources BuildCollectSources(SlashDispatchContext& ctx) {
    lubancode::evolution::CollectSources sources;
    if (ctx.recordings_root != nullptr) {
        sources.recordings_root = *ctx.recordings_root;
    }
    if (ctx.home_lubancode != nullptr && ctx.home_lubancode->has_value()) {
        sources.workflow_runs_root =
            lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "workflow-runs";
    }
    if (ctx.sessions_dir != nullptr) {
        sources.sessions_dir = *ctx.sessions_dir;
    }
    if (ctx.project_memory != nullptr) {
        // 已接受条目,按层各递一份。授权与开关的判断仍在 ProjectMemory 一处
        //(ListUserEntries 没开授权自然给空表)。
        lubancode::evolution::MemoryLayer project_layer;
        project_layer.entries = ctx.project_memory->ListEntries();
        project_layer.layer_label = "project";
        project_layer.dir_utf8 = lubancode::platform::PathToUtf8(ctx.project_memory->memory_dir());
        sources.memory_layers.push_back(std::move(project_layer));

        lubancode::evolution::MemoryLayer user_layer;
        user_layer.entries = ctx.project_memory->ListUserEntries();
        user_layer.layer_label = "user";
        user_layer.dir_utf8 =
            lubancode::platform::PathToUtf8(ctx.project_memory->user_memory_dir());
        sources.memory_layers.push_back(std::move(user_layer));
    }
    return sources;
}

// 一行截断(完整 UTF-8 边界),列表行用。
std::string Ellipsize(std::string text, std::size_t cap) {
    if (text.size() <= cap) {
        return text;
    }
    std::size_t end = cap;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "…";
}

void PrintUsage() {
    TermOut() << "用法: /evolve status\n"
                 "      /evolve list [run|goal|recording|tooltrace|memory|all]\n"
                 "      /evolve show <观察id>\n"
                 "阶段 1 只读:采集观察、聚类列账、追根查看;不生成 Package,不装任何东西。\n";
    TermOut().flush();
}

// ---- status:采集 + 落账 + 账面 ----
void RunEvolveStatus(SlashDispatchContext& ctx) {
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    if (store_root.empty()) {
        TermOut() << "没有主目录(.lubancode),观察账无处落。\n";
        TermOut().flush();
        return;
    }
    lubancode::evolution::ObservationStore store(store_root);

    const lubancode::evolution::CollectSources sources = BuildCollectSources(ctx);
    lubancode::evolution::CollectReport report;
    const std::vector<lubancode::evolution::EvolutionObservation> collected =
        lubancode::evolution::CollectObservations(sources, &report);

    std::size_t appended = 0;
    std::size_t duplicates = 0;
    std::size_t suppressed = 0;
    std::string error;
    for (const lubancode::evolution::EvolutionObservation& observation : collected) {
        const auto result = store.Append(observation);
        if (!result.has_value()) {
            error = result.error();
            continue;
        }
        switch (*result) {
            case lubancode::evolution::ObservationStore::AppendStatus::Appended: ++appended; break;
            case lubancode::evolution::ObservationStore::AppendStatus::DuplicateId: ++duplicates; break;
            case lubancode::evolution::ObservationStore::AppendStatus::SuppressedRejected:
                ++suppressed;
                break;
        }
    }

    const std::vector<lubancode::evolution::EvolutionObservation> ledger = store.Load();
    std::map<std::string, int> clusters;  // fingerprint -> 条数
    std::map<std::string, int> by_source;
    for (const auto& observation : ledger) {
        ++clusters[observation.fingerprint];
        ++by_source[lubancode::evolution::ToString(observation.source)];
    }

    TermOut() << "自进化观察账(阶段 1:只观察,不生成 Package):\n";
    TermOut() << "  采集: 录制件 " << report.recordings_scanned << "(跳过半截 "
              << report.recordings_skipped << "),workflow run " << report.runs_scanned
              << ",会话 " << report.sessions_scanned << "(读不动 " << report.sessions_unreadable
              << "),memory " << report.memory_entries << "\n";
    TermOut() << "  落账: 新增 " << appended << ",重采跳过 " << duplicates << ",被拒压下 "
              << suppressed << "\n";
    TermOut() << "  账面: 观察 " << ledger.size() << " 条,同类簇 " << clusters.size() << " 个";
    for (const auto& [source, count] : by_source) {
        TermOut() << "," << source << " " << count;
    }
    TermOut() << "\n";
    TermOut() << "  账本: " << lubancode::platform::PathToUtf8(store.observations_file()) << "\n";
    if (!error.empty()) {
        TermOut() << "  警告: 有观察没落住账(" << error << ")\n";
    }
    TermOut().flush();
}

// ---- list:按指纹聚类 ----
void RunEvolveList(SlashDispatchContext& ctx, const std::string& source_filter) {
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    lubancode::evolution::ObservationStore store(store_root);
    const std::vector<lubancode::evolution::EvolutionObservation> ledger = store.Load();
    if (ledger.empty()) {
        TermOut() << "观察账是空的(先 /evolve status 采集一回)。\n";
        TermOut().flush();
        return;
    }

    struct Cluster {
        std::vector<const lubancode::evolution::EvolutionObservation*> items;
    };
    std::map<std::string, Cluster> clusters;
    for (const lubancode::evolution::EvolutionObservation& observation : ledger) {
        const bool source_matches =
            source_filter.empty() || source_filter == "all" ||
            lubancode::evolution::ToString(observation.source) == source_filter;
        if (source_matches) {
            clusters[observation.fingerprint].items.push_back(&observation);
        }
    }

    // 簇按条数倒序(同类经验最多的排前),条数同按指纹字典序(输出稳定)。
    std::vector<std::pair<std::string, Cluster>> rows(clusters.begin(), clusters.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second.items.size() != b.second.items.size()) {
            return a.second.items.size() > b.second.items.size();
        }
        return a.first < b.first;
    });

    TermOut() << "观察账(按同类指纹聚类," << ledger.size() << " 条 / " << rows.size()
              << " 簇):\n";
    for (const auto& [fingerprint, cluster] : rows) {
        const auto& first = *cluster.items.front();
        TermOut() << "  " << fingerprint << "  x" << cluster.items.size() << "  ["
                  << lubancode::evolution::ToString(first.source) << " "
                  << lubancode::evolution::ToString(first.outcome) << "]  "
                  << Ellipsize(first.summary, 72) << "\n";
        if (cluster.items.size() > 1) {
            TermOut() << "    同类: ";
            for (std::size_t i = 0; i < cluster.items.size() && i < 6; ++i) {
                if (i > 0) TermOut() << " ";
                TermOut() << cluster.items[i]->id;
            }
            if (cluster.items.size() > 6) {
                TermOut() << " +" << (cluster.items.size() - 6);
            }
            TermOut() << "\n";
        }
    }
    TermOut().flush();
}

// ---- show:一条观察的全文与证据指回 ----
void RunEvolveShow(SlashDispatchContext& ctx, const std::string& target) {
    lubancode::evolution::ObservationStore store(BuildStoreRoot(ctx));
    const auto found = store.Find(target);
    if (!found.has_value()) {
        TermOut() << "没找到观察 \"" << target << "\"(先 /evolve list 看指纹与 id)\n";
        TermOut().flush();
        return;
    }
    const lubancode::evolution::EvolutionObservation& observation = *found;
    TermOut() << observation.id << "  [" << lubancode::evolution::ToString(observation.source)
              << " " << lubancode::evolution::ToString(observation.outcome) << "]\n";
    TermOut() << "  来源: " << lubancode::evolution::ToString(observation.source) << " "
              << observation.source_id << "\n";
    TermOut() << "  原始账: " << (observation.source_ref.empty() ? "(无)" : observation.source_ref)
              << "\n";
    TermOut() << "  指纹: " << observation.fingerprint << "\n";
    TermOut() << "  摘要: " << observation.summary << "\n";
    if (!observation.created_at.empty()) {
        TermOut() << "  时间: " << observation.created_at << "\n";
    }
    if (!observation.details.empty()) {
        TermOut() << "  账目: " << observation.details.dump() << "\n";
    }
    if (!observation.evidence.empty()) {
        TermOut() << "  证据(" << observation.evidence.size() << " 条):\n";
        for (const lubancode::evolution::EvidenceRef& ref : observation.evidence) {
            TermOut() << "    " << ref.ref << "  -- " << ref.note << "\n";
        }
    }
    TermOut().flush();
}

}  // namespace

// ---------------- 纯解析(单测钉) ----------------

ParsedEvolveCommand ParseEvolveCommand(const std::string& args) {
    ParsedEvolveCommand parsed;
    const std::string trimmed = Trimmed(args);
    if (trimmed.empty()) {
        parsed.action = EvolveCommandAction::Status;  // 裸 /evolve = status
        return parsed;
    }
    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string word = space == std::string::npos ? trimmed : trimmed.substr(0, space);
    const std::string rest =
        space == std::string::npos ? std::string() : Trimmed(trimmed.substr(space + 1));
    const std::string lower = ToLowerAscii(word);
    if (lower == "status") {
        parsed.action = EvolveCommandAction::Status;
        return parsed;
    }
    if (lower == "list") {
        parsed.action = EvolveCommandAction::List;
        if (!rest.empty()) {
            const std::string scope = ToLowerAscii(rest);
            if (scope == "all" || scope == "run" || scope == "goal" || scope == "recording" ||
                scope == "tooltrace" || scope == "memory") {
                parsed.source_filter = scope;
            } else {
                parsed.action = EvolveCommandAction::Invalid;
                parsed.bad_word = rest;
            }
        }
        return parsed;
    }
    if (lower == "show") {
        if (rest.empty()) {
            parsed.action = EvolveCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = EvolveCommandAction::Show;
        parsed.target = rest;
        return parsed;
    }
    parsed.action = EvolveCommandAction::Invalid;
    parsed.bad_word = word;
    return parsed;
}

// ---------------- 执行 ----------------

CommandFlow HandleSlashEvolve(SlashDispatchContext& ctx,
                              const lubancode::cli::ParsedSlashCommand& parsed) {
    const ParsedEvolveCommand command = ParseEvolveCommand(parsed.args);
    switch (command.action) {
        case EvolveCommandAction::Status:
            RunEvolveStatus(ctx);
            return CommandFlow::Continue;
        case EvolveCommandAction::List:
            RunEvolveList(ctx, command.source_filter);
            return CommandFlow::Continue;
        case EvolveCommandAction::Show:
            RunEvolveShow(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Invalid:
            TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
