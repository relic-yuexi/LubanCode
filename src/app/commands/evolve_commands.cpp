// /evolve 命令的执行体(自进化闭环阶段 1/2/3/4/5)。status = 扫五路账本采
// 观察 + 落观察账(只追加)+ 报账面;list = 按指纹聚类列账 + 候选区;show =
// 看一条观察或一只候选,指回来源。阶段 2:propose/diff/reject。阶段 3:test
// 跑评测五道门。阶段 4:approve 出批准页并原子装 store、use 点名 canary、
// promote 晋升、rollback 切回。阶段 5:propose 聚同指纹簇起草(够门槛出组合
// 候选,否则最小 Skill-only),diff/show 分档展示,批准页亮复杂度代价——
// 迁移全走 EvolutionCoordinator(唯一写口),这里只递材料、只打印。
#include "app/commands/evolve_commands.hpp"
#include "memory/project_memory.hpp"  // /evolve 的分层账(ctx.project_memory)
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 5c:status/list/show/propose 渲染段)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:统一走输出端口
#include "cli/theme.hpp"          // Theme/ResolveTheme/DetectConsoleCapability
#include "platform/console.hpp"   // GetScreenInfo:/evolve 的框宽同一把尺

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "evolution/adapters.hpp"  // ObservationsFromRecording(聚簇按指纹)
#include "evolution/collector.hpp"
#include "evolution/coordinator.hpp"
#include "evolution/eval.hpp"
#include "evolution/observation_store.hpp"
#include "evolution/suggest.hpp"  // 阶段 7:五门判定、开关账、命中账
#include "platform/paths.hpp"
#include "skills/workflow_recorder.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;

namespace {

namespace frame = lubancode::cli::frame;

// TUI 排版批 5c(/evolve)的公共小件,与批 2-4 各命令族同款:渲染段只调
// cli::frame::* 三助手;文案是既有硬编码中文(不走 i18n 表),按批 2 裁量
// 一字不添不改;句内冒号按 SentenceField 拆两列(批 1 裁量)。

// 会话主题没递(裸 CLI/测试)就按终端能力起板(批 2 裁量 4):管道/重定向
// 自然降 plain,测试进程里钉的就是 plain 形状。
lubancode::cli::Theme EvolveTheme(const EvolveCommandContext& ctx) {
    if (ctx.theme != nullptr) {
        return *ctx.theme;
    }
    return lubancode::cli::ResolveTheme(std::string(),
                                        lubancode::cli::DetectConsoleCapability().colors_enabled);
}

int EvolveFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

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

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{Trimmed(sentence.substr(0, colon)), Trimmed(sentence.substr(colon + 1)),
                        accent};
}

// 反馈/错误通知:句子进键值对框(句内冒号拆列;错误走 error 语义色)。
void PrintEvolveNotice(const lubancode::cli::Theme& theme,
                       const std::vector<std::string>& sentences,
                       frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    for (const std::string& line :
         frame::RenderKeyValues({}, fields, theme, frame::Light(), EvolveFrameWidth())) {
        TermOut() << line << "\n";
    }
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// 观察账根:<home>/.lubancode/evolution/observations(README"观察账"节)。
std::filesystem::path BuildStoreRoot(const EvolveCommandContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "evolution" / "observations";
}

// evolution 根:<home>/.lubancode/evolution——观察账在它的 observations/
// 里,阶段 7 的开关账(suggest.json)与命中账(suggest.jsonl)落在它根上。
std::filesystem::path BuildEvolutionRoot(const EvolveCommandContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "evolution";
}

// 候选仓根:<home>/.lubancode/package-candidates(README"候选目录"节)。
// 与正式 Package 目录(packages/、package-store/)分开——四层扫描不扫这里,
// /package list 天然看不见候选(防偷装靠的是目录分仓,不是名单过滤)。
std::filesystem::path BuildCandidateRoot(const EvolveCommandContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "package-candidates";
}

// version store 根:<home>/.lubancode/package-store(README"晋升、灰度与
// 回滚"节)。批准后版本原子落这里;active/canary 指针在 channels.json。
// (与上面的 BuildStoreRoot 是两处账:那边是观察账,这边是版本仓。)
std::filesystem::path BuildVersionStoreRoot(const EvolveCommandContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "package-store";
}

// 五路账本的输入装配:全从分派材料借,缺哪路就空着哪路(采集器对空根自然跳过)。
lubancode::evolution::CollectSources BuildCollectSources(const EvolveCommandContext& ctx) {
    lubancode::evolution::CollectSources sources;
    if (ctx.recordings_root != nullptr) {
        sources.recordings_root = *ctx.recordings_root;
    }
    if (ctx.home_lubancode != nullptr && ctx.home_lubancode->has_value()) {
        sources.workflow_runs_root =
            lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "workflow-runs";
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
                 "      /evolve show <观察id|候选id>\n"
                 "      /evolve propose <recording-id|observation-id>\n"
                 "      /evolve diff <candidate-id>\n"
                 "      /evolve reject <candidate-id> [reason]\n"
                 "      /evolve test <candidate-id>\n"
                 "      /evolve approve <candidate-id>\n"
                 "      /evolve use <candidate-id>\n"
                 "      /evolve promote <candidate-id>\n"
                 "      /evolve rollback <package-id> [version]\n"
                 "      /evolve suggest [on|off]\n"
                 "阶段 1 只读观察;阶段 2 从录制起草最小 content-only 候选(propose)、看\n"
                 "diff、拒绝(reject);阶段 3 评测(test):静态门+回放+留出+基线对照,账只追加;\n"
                 "阶段 4 批准与灰度(approve/use/promote/rollback):批准只认当前哈希,批准后\n"
                 "原子落 package-store,点名 canary 新会话生效、旧任务照旧,回滚不删版本不抹账;\n"
                 "阶段 5 propose 升级:同指纹簇攒够门槛(>=2 场独立任务且成功路序列同形)出\n"
                 "组合候选(Skill+Workflow,全场工具面同形再加 Agent),否则照旧最小 Skill-only\n"
                 "包;组合件须过 AnalyzePackage,过不了降档 Skill-only,不硬塞;评测与被测\n"
                 "workflow 分家,批准页亮复杂度代价;阶段 6 代码档草稿:簇内同求而无人成功的\n"
                 "工具恰一件出 process Plugin 草稿、>=2 件出 MCP server 草稿(同判据两路,\n"
                 "零进程零挂载);阶段 7 有限自动建议(suggest,缺省关):开着时 status 采集后\n"
                 "按五门判定亮一行建议,只提示不自动起草,拒绝后不死缠,命中率与接受率可查。\n"
                 "code-bearing 候选(带 Plugin/MCP)不走 approve,另过 Package trust。CI 入口:\n"
                 "  lubancode evolve test <候选目录> --baseline <父包目录> --json\n";
    TermOut().flush();
}

// ---- 阶段 7:有限自动建议的提示回合(只提示,不起草) ----
// 开着才跑:聚同指纹簇 -> 挡门指纹集(观察账拒绝指纹 + 候选仓既有候选)
// -> 五门判定 -> 过门的亮一行建议、记一笔 shown。只有本次采集真进了
// 新观察的簇才提示——没新材料不唠叨;关着的时候这本账一字不写。
void RunSuggestionPass(const EvolveCommandContext& ctx,
                       const std::vector<std::string>& new_observation_ids) {
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    const std::filesystem::path evolution_root = BuildEvolutionRoot(ctx);
    if (store_root.empty() || evolution_root.empty()) {
        return;
    }
    lubancode::evolution::ObservationStore observations(store_root);
    std::map<std::string, std::vector<const lubancode::evolution::EvolutionObservation*>> clusters;
    for (const lubancode::evolution::EvolutionObservation& observation : observations.Load()) {
        clusters[observation.fingerprint].push_back(&observation);
    }
    const std::vector<std::string> blocked = lubancode::evolution::CollectBlockedFingerprints(
        observations, lubancode::evolution::CandidateStore(BuildCandidateRoot(ctx)));
    lubancode::evolution::SuggestLedger suggest_ledger(evolution_root / "suggest.jsonl");
    int shown = 0;
    for (const auto& [fingerprint, cluster] : clusters) {
        bool has_new = false;
        for (const lubancode::evolution::EvolutionObservation* observation : cluster) {
            if (std::find(new_observation_ids.begin(), new_observation_ids.end(),
                          observation->id) != new_observation_ids.end()) {
                has_new = true;
                break;
            }
        }
        if (!has_new) {
            continue;  // 没进新材料的簇不提示:拒绝后不死缠之外的第二道安静门
        }
        std::vector<lubancode::evolution::EvolutionObservation> members;
        for (const lubancode::evolution::EvolutionObservation* observation : cluster) {
            members.push_back(*observation);
        }
        const lubancode::evolution::SuggestionVerdict verdict =
            lubancode::evolution::AssessSuggestion(members, blocked);
        if (!verdict.eligible) {
            continue;
        }
        TermOut() << "建议(阶段 7,只提示不自动起草):指纹 " << fingerprint << " 已有 "
                  << verdict.independent_tasks << " 场独立任务同形走通("
                  << Ellipsize(verdict.summary, 56) << ")。" << verdict.benefit_line << "\n";
        TermOut() << "  点头起草: /evolve propose " << verdict.representative_obs_id
                  << "(照旧走候选/评测/批准,一道门都不少)\n";
        lubancode::evolution::SuggestEvent event;
        event.type = "shown";
        event.fingerprint = verdict.fingerprint;
        event.cluster_size = verdict.cluster_size;
        event.benefit = verdict.benefit_line;
        event.obs_id = verdict.representative_obs_id;
        if (const auto append_error = suggest_ledger.Append(event); append_error.has_value()) {
            TermOut() << "  警告: 命中账没记上(" << *append_error << ")\n";
        }
        ++shown;
    }
    if (shown == 0) {
        TermOut() << "建议(阶段 7):本次新材料里没有过五门的簇,不提示。\n";
    }
}

// ---- status:采集 + 落账 + 账面(+ 阶段 7:开着时顺手提示一回) ----
void RunEvolveStatus(const EvolveCommandContext& ctx) {
    const lubancode::cli::Theme theme = EvolveTheme(ctx);
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    if (store_root.empty()) {
        PrintEvolveNotice(theme, {"没有主目录(.lubancode),观察账无处落。"},
                          frame::FieldAccent::Error);
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
    std::vector<std::string> new_observation_ids;  // 本次真落账的观察(建议回合用)
    for (const lubancode::evolution::EvolutionObservation& observation : collected) {
        const auto result = store.Append(observation);
        if (!result.has_value()) {
            error = result.error();
            continue;
        }
        switch (*result) {
            case lubancode::evolution::ObservationStore::AppendStatus::Appended:
                ++appended;
                new_observation_ids.push_back(observation.id);
                break;
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

    EvolveStatusModel model;
    model.recordings_scanned = report.recordings_scanned;
    model.recordings_skipped = report.recordings_skipped;
    model.runs_scanned = report.runs_scanned;
    model.memory_entries = report.memory_entries;
    model.appended = appended;
    model.duplicates = duplicates;
    model.suppressed = suppressed;
    model.ledger_size = ledger.size();
    model.cluster_count = clusters.size();
    for (const auto& [source, count] : by_source) {
        model.by_source.emplace_back(source, count);
    }
    model.observations_file = lubancode::platform::PathToUtf8(store.observations_file());
    model.error = error;
    for (const std::string& line : FormatEvolveStatusLines(model, theme, EvolveFrameWidth())) {
        TermOut() << line << "\n";
    }
    // 阶段 7:开着才提示(缺省关;关着时连命中账都不写,更不另收材料)。
    // 提示回合是逐簇短句,保持原样平铺(批 2 裁量 2:框只装结论)。
    if (lubancode::evolution::LoadSuggestEnabled(BuildEvolutionRoot(ctx))) {
        RunSuggestionPass(ctx, new_observation_ids);
    }
    TermOut().flush();
}

// ---- suggest:看/开关有限自动建议(阶段 7;缺省关闭是铁律) ----
void RunEvolveSuggest(const EvolveCommandContext& ctx, const std::string& arg) {
    const std::filesystem::path evolution_root = BuildEvolutionRoot(ctx);
    if (evolution_root.empty()) {
        TermOut() << "没有主目录(.lubancode),建议开关无处落。\n";
        TermOut().flush();
        return;
    }
    if (arg == "on" || arg == "off") {
        const bool enabling = arg == "on";
        if (const auto error =
                lubancode::evolution::SaveSuggestEnabled(evolution_root, enabling);
            error.has_value()) {
            TermOut() << *error << "\n";
            TermOut().flush();
            return;
        }
        TermOut() << (enabling ? "有限自动建议已开。" : "有限自动建议已关(缺省态)。") << "\n";
        TermOut() << "  语义: 开着时 /evolve status 采集后按五门判定亮一行建议——只提示"
                     "候选机会,不自动起草、测试或安装;点头才走 /evolve propose。\n";
        TermOut() << "  关着时:不评估建议、不写命中账,不为建议另收材料(观察账照旧,"
                     "那是阶段 1 的显式采集)。\n";
        TermOut().flush();
        return;
    }
    // 裸跑:开关状态 + 门槛(可 inspect)+ 命中率与接受率账(现算)。
    const bool enabled = lubancode::evolution::LoadSuggestEnabled(evolution_root);
    const lubancode::evolution::SuggestThresholds thresholds;
    TermOut() << "有限自动建议(阶段 7):" << (enabled ? "开" : "关(缺省)") << "\n";
    TermOut() << "  五门门槛(inspect):独立任务 >= " << thresholds.min_independent_tasks
              << ";带验证证据的独立成功 >= " << thresholds.min_independent_successes
              << ";形状步数 >= " << thresholds.min_shape_steps
              << ";同指纹无在途/被拒候选;收益说得清(Memory 装不下的整套做法)\n";
    const lubancode::evolution::SuggestLedger ledger(evolution_root / "suggest.jsonl");
    const lubancode::evolution::SuggestLedger::Stats stats = ledger.ComputeStats();
    // 命中率的分母:观察账上当前达到门一的簇数(现状口径,inspect 时现算)。
    int opportunities = 0;
    {
        std::map<std::string, std::set<std::string>> cluster_sources;
        for (const lubancode::evolution::EvolutionObservation& observation :
             lubancode::evolution::ObservationStore(BuildStoreRoot(ctx)).Load()) {
            cluster_sources[observation.fingerprint].insert(observation.source_id);
        }
        for (const auto& [fingerprint, sources] : cluster_sources) {
            if (static_cast<int>(sources.size()) >= thresholds.min_independent_tasks) {
                ++opportunities;
            }
        }
    }
    TermOut() << "  命中账: 提示 " << stats.shown_events << " 笔/" << stats.shown_fingerprints
              << " 指纹;接受 " << stats.accepted_events << " 笔/" << stats.accepted_fingerprints
              << " 指纹\n";
    TermOut() << "  命中率: " << stats.shown_fingerprints << " / " << opportunities
              << "(出过提示的指纹 / 当前账上达到门一的簇数)\n";
    if (stats.acceptance_rate < 0.0) {
        TermOut() << "  接受率: (还没出过提示,不算)\n";
    } else {
        char rate[32]{};
        std::snprintf(rate, sizeof(rate), "%.0f%%", stats.acceptance_rate * 100.0);
        TermOut() << "  接受率: " << stats.accepted_fingerprints << " / "
                  << stats.shown_fingerprints << " = " << rate
                  << "(出过提示又真起草的指纹 / 出过提示的指纹)\n";
    }
    TermOut() << "  账本: " << lubancode::platform::PathToUtf8(evolution_root / "suggest.jsonl")
              << "\n";
    if (!enabled) {
        TermOut() << "  注: 当前关着——上面的账是历史累计,新提示不再产生。\n";
    }
    TermOut().flush();
}

// ---- list:按指纹聚类 ----
void RunEvolveList(const EvolveCommandContext& ctx, const std::string& source_filter) {
    const lubancode::cli::Theme theme = EvolveTheme(ctx);
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    lubancode::evolution::ObservationStore store(store_root);
    const std::vector<lubancode::evolution::EvolutionObservation> ledger = store.Load();
    if (ledger.empty()) {
        PrintEvolveNotice(theme, {"观察账是空的(先 /evolve status 采集一回)。"});
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

    std::vector<EvolveClusterRow> cluster_rows;
    for (const auto& [fingerprint, cluster] : rows) {
        const auto& first = *cluster.items.front();
        EvolveClusterRow row;
        row.fingerprint = fingerprint;
        row.count = "x" + std::to_string(cluster.items.size());
        row.source = lubancode::evolution::ToString(first.source);
        row.outcome = lubancode::evolution::ToString(first.outcome);
        row.summary = Ellipsize(first.summary, 72);
        if (cluster.items.size() > 1) {
            std::ostringstream peers;
            for (std::size_t i = 0; i < cluster.items.size() && i < 6; ++i) {
                if (i > 0) peers << " ";
                peers << cluster.items[i]->id;
            }
            if (cluster.items.size() > 6) {
                peers << " +" << (cluster.items.size() - 6);
            }
            row.peers = peers.str();
        }
        cluster_rows.push_back(std::move(row));
    }
    for (const std::string& line :
         FormatEvolveClusterLines(ledger.size(), cluster_rows, theme, EvolveFrameWidth())) {
        TermOut() << line << "\n";
    }

    // ---- 候选区(阶段 2):观察与候选同一张账面看 ----
    const std::filesystem::path candidate_root = BuildCandidateRoot(ctx);
    if (!candidate_root.empty()) {
        const std::vector<lubancode::evolution::CandidateSummary> candidates =
            lubancode::evolution::CandidateStore(candidate_root).LoadAll();
        if (!candidates.empty()) {
            std::vector<EvolveCandidateRow> candidate_rows;
            for (const lubancode::evolution::CandidateSummary& candidate : candidates) {
                EvolveCandidateRow row;
                row.candidate_id = candidate.candidate_id;
                row.state = lubancode::evolution::ToString(candidate.state);
                row.package_id = candidate.package_id;
                if (candidate.record.has_value()) {
                    row.objective = Ellipsize(candidate.record->objective, 56);
                }
                candidate_rows.push_back(std::move(row));
            }
            for (const std::string& line : FormatEvolveCandidateListLines(
                     candidate_rows, theme, EvolveFrameWidth())) {
                TermOut() << line << "\n";
            }
        }
    }
    TermOut().flush();
}

// ---- show:一条观察的全文与证据指回;候选则回指来源 ----
void RunEvolveShowCandidate(const EvolveCommandContext& ctx, const std::string& target);
void RunEvolveShow(const EvolveCommandContext& ctx, const std::string& target) {
    // 候选 id(cand- 起头)走候选页;其余按观察 id 查。
    if (target.rfind("cand-", 0) == 0) {
        RunEvolveShowCandidate(ctx, target);
        return;
    }
    const lubancode::cli::Theme theme = EvolveTheme(ctx);
    lubancode::evolution::ObservationStore store(BuildStoreRoot(ctx));
    const auto found = store.Find(target);
    if (!found.has_value()) {
        PrintEvolveNotice(theme, {"没找到观察 \"" + target + "\"(先 /evolve list 看指纹与 id)"},
                          frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }
    const lubancode::evolution::EvolutionObservation& observation = *found;
    EvolveObservationModel model;
    model.id = observation.id;
    model.source = lubancode::evolution::ToString(observation.source);
    model.outcome = lubancode::evolution::ToString(observation.outcome);
    model.source_id = observation.source_id;
    model.source_ref = observation.source_ref;
    model.fingerprint = observation.fingerprint;
    model.summary = observation.summary;
    model.created_at = observation.created_at;
    if (!observation.details.empty()) {
        model.details_json = observation.details.dump();
    }
    for (const lubancode::evolution::EvidenceRef& ref : observation.evidence) {
        model.evidence.emplace_back(ref.ref, ref.note);
    }
    for (const std::string& line :
         FormatEvolveObservationLines(model, theme, EvolveFrameWidth())) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

// ---- 评测摘要(show 页与 test 页共用):通过几项、没测什么、比基线贵多少 ----
void PrintEvalSummary(const lubancode::evolution::EvalSummary& summary, const char* indent) {
    TermOut() << indent << "静态门 " << (summary.static_gate.fail > 0 ? "fail" : "pass")
              << ";replay pass " << summary.replay.pass << " / fail " << summary.replay.fail
              << " / 跳过 " << summary.replay.skipped << ";holdout pass " << summary.holdout.pass
              << " / fail " << summary.holdout.fail << " / 跳过 " << summary.holdout.skipped
              << ";baseline " << (summary.baseline.total() == 0
                                      ? "未跑"
                                      : "pass " + std::to_string(summary.baseline.pass) + " / fail " +
                                            std::to_string(summary.baseline.fail))
              << "\n";
    TermOut() << indent << "通过 " << summary.checks_passed << " 项检查,失败 "
              << summary.checks_failed << ",跳过 " << summary.checks_skipped
              << "(人工验收/缺夹具)\n";
    if (!summary.unverified.empty()) {
        TermOut() << indent << "没测到: ";
        for (std::size_t i = 0; i < summary.unverified.size(); ++i) {
            TermOut() << (i > 0 ? "," : "") << summary.unverified[i];
        }
        TermOut() << "\n";
    }
    if (summary.has_baseline_metrics) {
        const auto cost = [](const lubancode::evolution::MetricDelta& delta,
                             const char* name) {
            std::string text = name;
            text += " " + std::to_string(static_cast<std::int64_t>(delta.candidate)) + " 对 " +
                    std::to_string(static_cast<std::int64_t>(delta.baseline));
            if (delta.delta == 0) {
                text += "(持平)";
            } else {
                text += "(" + (delta.delta > 0 ? std::string("+") : std::string()) +
                        std::to_string(static_cast<std::int64_t>(delta.delta));
                if (delta.baseline > 0) {
                    text += "," + (delta.delta_pct > 0 ? std::string("+") : std::string()) +
                            std::to_string(delta.delta_pct) + "%";
                }
                text += ")";
            }
            return text;
        };
        TermOut() << indent << "对照基线 "
                  << (summary.baseline_ref.empty() ? "(未记)" : summary.baseline_ref) << ":"
                  << cost(summary.tool_calls, "tool calls") << "," << cost(summary.tokens, "tokens")
                  << "," << cost(summary.wall_clock_ms, "墙钟ms") << ","
                  << cost(summary.permission_prompts, "确认") << ","
                  << cost(summary.workspace_writes, "写入") << "\n";
    } else {
        TermOut() << indent << "对照基线: 基线侧没有指标账,代价对照缺(未测)\n";
    }
    if (!summary.has_holdout) {
        TermOut() << indent << "(无留出任务:只可标 experimental,不可自动建议晋升稳定版)\n";
    }
}

// ---- show 候选页:演化账 + 批准账 + 状态 + 来源回指 ----
void RunEvolveShowCandidate(const EvolveCommandContext& ctx, const std::string& target) {
    const lubancode::cli::Theme theme = EvolveTheme(ctx);
    lubancode::evolution::CandidateStore store(BuildCandidateRoot(ctx));
    const auto found = store.Find(target);
    if (!found.has_value()) {
        PrintEvolveNotice(theme, {"没找到候选 \"" + target + "\"(先 /evolve list 看候选区)"},
                          frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }
    EvolveCandidatePageModel model;
    model.candidate_id = found->candidate_id;
    model.state = lubancode::evolution::ToString(found->state);
    model.package_id = found->package_id;
    model.dir_utf8 = lubancode::platform::PathToUtf8(found->dir);
    model.content_hash = found->content_hash;
    // 形状照盘上现状说(代码草稿[Plugin 或 MCP]/组合/最小),复杂度顺带亮一笔。
    {
        const lubancode::evolution::ComplexityCost cost =
            lubancode::evolution::ComputeComplexityCost(found->dir / "package");
        if (!cost.shape.empty()) {
            model.has_shape = true;
            // 三目链包进 std::string 再相加:裸 char* + char* 编译不过(纪律警钟)。
            model.shape =
                std::string(cost.shape == "code-draft"
                                ? (cost.has_mcp ? "code-bearing-draft(MCP server 草稿 + skill)"
                                                : "code-bearing-draft(process Plugin 草稿 + skill)")
                                : (cost.shape == "combination" ? "组合包" : "最小 Skill-only 包")) +
                ";复杂度 " + cost.SummaryLine();
        }
    }
    if (found->record.has_value()) {
        const lubancode::evolution::EvolutionRecord& record = *found->record;
        model.has_record = true;
        std::ostringstream version;
        version << record.candidate_version;
        if (record.parent.has_value()) {
            version << "(父版 " << record.parent->version << " " << record.parent->content_hash
                    << ")";
        } else {
            version << "(无父版,与空对照)";
        }
        model.candidate_version = version.str();
        model.objective = Ellipsize(record.objective, 96);
        // 来源回指:稳定来源 ID -> 观察账 id(可再 /evolve show 追到原始账)。
        std::ostringstream sources;
        bool any_source = false;
        for (const std::string& id : record.sources.recording_ids) {
            sources << (any_source ? ", " : "") << "recording " << id << " = 观察 "
                    << lubancode::evolution::MakeObservationId(
                           lubancode::evolution::ObservationSource::Recording, id);
            any_source = true;
        }
        for (const std::string& id : record.sources.run_ids) {
            sources << (any_source ? ", " : "") << "run " << id;
            any_source = true;
        }
        for (const std::string& id : record.sources.goal_ids) {
            sources << (any_source ? ", " : "") << "goal " << id;
            any_source = true;
        }
        for (const std::string& id : record.sources.memory_ids) {
            sources << (any_source ? ", " : "") << "memory " << id;
            any_source = true;
        }
        if (!any_source) {
            sources << "(演化账未记来源)";
        }
        model.sources = sources.str();
        model.generator = record.generator.provider + " / " + record.generator.model + " / " +
                          record.generator.prompt_revision;
        std::ostringstream changes;
        changes << "新增组件 ";
        for (const std::string& item : record.changes.components_added) {
            changes << item << " ";
        }
        changes << "权限差异 " << record.changes.permissions_added.size() << " 条,新工具 "
                << record.changes.tools_added.size() << " 件";
        model.changes = changes.str();
        // 阶段 6:代码档草稿的权限差异逐条亮(一条一权,env 只记名;帽 8 条)。
        for (std::size_t i = 0; i < record.changes.tools_added.size() && i < 8; ++i) {
            model.tools_added.push_back(record.changes.tools_added[i]);
        }
        for (std::size_t i = 0; i < record.changes.permissions_added.size() && i < 8; ++i) {
            model.permissions_added.push_back(record.changes.permissions_added[i]);
        }
        model.created_at = record.created_at.empty() ? "(未记)" : record.created_at;
    }
    if (found->approval.has_value()) {
        model.has_approval = true;
        std::ostringstream approval;
        approval << found->approval->tier << " / " << found->approval->status;
        if (found->approval->decision.has_value()) {
            approval << "(由 " << found->approval->decision->decided_by << " 于 "
                     << found->approval->decision->decided_at << " 决定;指纹 "
                     << found->approval->decision->fingerprint << ")";
        }
        model.approval = approval.str();
    }
    // ---- 评测账摘要(阶段 3):通过几项、没测什么、比基线贵多少 ----
    const std::vector<lubancode::evolution::EvalResultLine> results =
        lubancode::evolution::LoadEvalResults(found->dir / "eval-results.jsonl");
    model.has_eval = !results.empty();
    model.eval_rows = results.size();
    for (const std::string& line :
         FormatEvolveCandidatePageLines(model, theme, EvolveFrameWidth())) {
        TermOut() << line << "\n";
    }
    // 评测摘要是 PrintEvalSummary 的多行正文(与 test/approve 共用),长正文
    // 不塞框(批 1 裁量 3),在框外原样跟出。
    if (!results.empty()) {
        const lubancode::evolution::EvalSummary summary =
            lubancode::evolution::SummarizeEvalLedger(results);
        PrintEvalSummary(summary, "    ");
    }
    PrintEvolveNotice(theme, {"下一步: /evolve diff " + found->candidate_id +
                                  ";评测入账后 /evolve approve " + found->candidate_id +
                                  " 出批准页(只认当前哈希)"});
    TermOut().flush();
}

// ---- propose:从一场录制起草候选;簇够门槛出组合包,否则最小 Skill-only ----
// 目标认两种:录制件 id,或观察 id(obs- 起头,须是 recording 来源)。
// 阶段 5:先按观察账聚同 fingerprint 簇(>=2 场独立任务),再交
// ProposeFromCluster——两把尺(成功路序列同形/全场工具面同形)在起草器里
// 判;账上没有同类或只有这一场,簇就只有点名场,照旧 Skill-only。
void RunEvolvePropose(const EvolveCommandContext& ctx, const std::string& target) {
    const lubancode::cli::Theme theme = EvolveTheme(ctx);
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    const std::filesystem::path candidate_root = BuildCandidateRoot(ctx);
    if (store_root.empty() || candidate_root.empty()) {
        PrintEvolveNotice(theme, {"没有主目录(.lubancode),候选无处落。"},
                          frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }
    if (ctx.recordings_root == nullptr) {
        PrintEvolveNotice(theme, {"没有录制件根,找不到录制。"}, frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }

    // 目标 -> 录制件 id。
    std::string recording_id = target;
    lubancode::evolution::ObservationStore observations(store_root);
    if (target.rfind("obs-", 0) == 0) {
        const auto found = observations.Find(target);
        if (!found.has_value()) {
            PrintEvolveNotice(theme, {"没找到观察 \"" + target + "\"(先 /evolve status 采集一回)"},
                              frame::FieldAccent::Error);
            TermOut().flush();
            return;
        }
        if (found->source != lubancode::evolution::ObservationSource::Recording) {
            PrintEvolveNotice(theme, {"观察 \"" + target + "\" 的来源是 " +
                                          lubancode::evolution::ToString(found->source) +
                                          ",propose 只从 recording 起草。"},
                              frame::FieldAccent::Error);
            TermOut().flush();
            return;
        }
        recording_id = found->source_id;
    }
    if (recording_id.find('/') != std::string::npos ||
        recording_id.find('\\') != std::string::npos ||
        recording_id.find("..") != std::string::npos) {
        PrintEvolveNotice(theme, {"录制件 id 只认单段目录名: \"" + recording_id + "\""},
                          frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }

    // 录制件盘点(盘上现查,不靠观察账转述)。
    const std::vector<lubancode::skills::RecordingStatus> recordings =
        lubancode::skills::ListRecordings(*ctx.recordings_root);
    const auto find_recording = [&](const std::string& id) ->
        std::optional<lubancode::skills::RecordingStatus> {
        for (const lubancode::skills::RecordingStatus& item : recordings) {
            if (item.id == id) {
                return item;
            }
        }
        return std::nullopt;
    };
    const auto named = find_recording(recording_id);
    if (!named.has_value()) {
        PrintEvolveNotice(
            theme, {"找不到录制件 \"" + recording_id + "\"(先 /record 录一回,再 /evolve status)"},
            frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }

    // ---- 聚簇:点名场在前,账上同指纹的独立任务随后 ----
    std::vector<lubancode::evolution::ClusterTaskMaterial> cluster;
    {
        lubancode::evolution::ClusterTaskMaterial head;
        head.status = *named;
        head.events = lubancode::skills::ReadRecordingEvents(named->dir);
        cluster.push_back(std::move(head));
    }
    std::string fingerprint;
    {
        const std::vector<lubancode::evolution::EvolutionObservation> made =
            lubancode::evolution::ObservationsFromRecording({cluster.front().status,
                                                             cluster.front().events});
        if (!made.empty()) {
            fingerprint = made.front().fingerprint;
        }
    }
    int cluster_skipped = 0;
    if (!fingerprint.empty()) {
        for (const lubancode::evolution::EvolutionObservation& observation : observations.Load()) {
            if (cluster.size() >= 8) {
                break;  // 簇帽:起草看形状,八场足矣
            }
            if (observation.source != lubancode::evolution::ObservationSource::Recording ||
                observation.fingerprint != fingerprint) {
                continue;
            }
            if (observation.source_id == recording_id) {
                continue;  // 点名场已在
            }
            const auto other = find_recording(observation.source_id);
            if (!other.has_value() || !other->finished) {
                ++cluster_skipped;  // 录制件没了或没录完:不进簇
                continue;
            }
            lubancode::evolution::ClusterTaskMaterial material;
            material.status = *other;
            material.events = lubancode::skills::ReadRecordingEvents(other->dir);
            cluster.push_back(std::move(material));
        }
    }

    lubancode::evolution::EvolutionCoordinator coordinator(candidate_root, &observations);
    const auto result = coordinator.ProposeFromCluster(cluster);
    if (!result.has_value()) {
        PrintEvolveNotice(theme, {"起草失败: " + result.error()}, frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }
    EvolveProposeModel model;
    model.candidate = result->candidate_id + "  [" + result->package_id + " " +
                      result->candidate_version + "]";
    model.content_hash = result->content_hash;
    model.dir_utf8 = lubancode::platform::PathToUtf8(result->candidate_dir);
    // 分档亮形:代码草稿(Plugin 或 MCP)、组合还是最小包,簇多大,组件几件。
    if (result->code_draft) {
        if (result->mcp_draft) {
            model.shape =
                "code-bearing-draft(MCP server 草稿)——mcp.yaml + server 脚手架 + Skill,簇内 " +
                std::to_string(result->cluster_size) + " 场任务同求 " +
                std::to_string(result->wanted_tools.size()) + " 件不存在的工具(\"" +
                result->wanted_tool +
                "\" 等;现有工具办不了,录到的是 registry.unknown_tool 失败;同求多件,封一只 "
                "server 合账)";
            model.rule = "零进程零挂载,不自动启用;server 是未实现占位,补实现走人工审查线";
        } else {
            model.shape =
                "code-bearing-draft(process Plugin 草稿)——plugin.json + runner 脚手架 + "
                "Skill,簇内 " +
                std::to_string(result->cluster_size) + " 场任务同求工具 \"" + result->wanted_tool +
                "\"(现有工具办不了,录到的是 registry.unknown_tool 失败)";
            model.rule = "零进程零挂载,不自动启用;runner 是未实现占位,补实现走人工审查线";
        }
    } else if (result->shape == "combination") {
        model.shape = "组合候选(簇 " + std::to_string(result->cluster_size) +
                      " 场同形任务;两把尺过门)——workflow" +
                      (result->agent_drafted ? " + Agent" : "") + " + Skill";
    } else {
        model.shape = "最小 Skill-only 包(默认答案;Agent 是升档不是标配)";
    }
    {
        std::ostringstream components;
        for (std::size_t i = 0; i < result->component_paths.size(); ++i) {
            components << (i > 0 ? ", " : "") << result->component_paths[i];
        }
        if (result->code_draft) {
            components << "(code-bearing:新工具 " << result->tools_added.size()
                       << " 件,新权限 " << result->permissions_added.size() << " 条)";
            for (const std::string& tool : result->tools_added) {
                model.tools_added.push_back(tool);
            }
            for (const std::string& perm : result->permissions_added) {
                model.permissions_added.push_back(perm);
            }
            model.gate =
                "code-bearing 候选不自动晋升——评测可跑(全静态,零进程),/evolve approve 会明拒"
                "并指路 /package trust 人工审查线";
        } else {
            components << "(content-only,无进程无网络)";
        }
        model.components = components.str();
    }
    if (!result->downgrade_note.empty()) {
        model.downgrade = Ellipsize(result->downgrade_note, 160);
    }
    model.cluster_skipped = cluster_skipped;
    model.next_step = "/evolve diff " + result->candidate_id + "(分档看形状)或 /evolve test " +
                      result->candidate_id + "(评测五道门)";
    for (const std::string& line :
         FormatEvolveProposeLines(model, theme, EvolveFrameWidth())) {
        TermOut() << line << "\n";
    }
    // 阶段 7:点了头的建议记一笔接受账(同一指纹只记头一笔;接受是真动作,
    // 与建议开关无关——关着建议也能起草,账照实记)。失败警告在框后明说。
    if (!fingerprint.empty()) {
        lubancode::evolution::SuggestLedger suggest_ledger(BuildEvolutionRoot(ctx) / "suggest.jsonl");
        if (suggest_ledger.HasOpenSuggestion(fingerprint)) {
            lubancode::evolution::SuggestEvent event;
            event.type = "accepted";
            event.fingerprint = fingerprint;
            event.candidate_id = result->candidate_id;
            if (const auto append_error = suggest_ledger.Append(event); append_error.has_value()) {
                PrintEvolveNotice(theme, {"警告: 接受账没记上(" + *append_error + ")"},
                                  frame::FieldAccent::Error);
            }
        }
    }
    TermOut().flush();
}

// ---- diff:与父版或空对照 ----
void RunEvolveDiff(const EvolveCommandContext& ctx, const std::string& target) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr);
    const auto result = coordinator.Diff(target);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "候选 " << result->candidate_id << "(" << result->package_id << ")对照 "
              << result->baseline << ":\n";
    TermOut() << "  形状: "
              << (result->shape == "code-draft"
                      ? (result->mcp_summary.empty() ? "code-bearing-draft(process Plugin 草稿 + skill)"
                                                     : "code-bearing-draft(MCP server 草稿 + skill)")
                      : (result->shape == "combination" ? "组合包(workflow/agent + skill)"
                                                        : "最小 Skill-only 包"))
              << "\n";
    TermOut() << "  新增文件(" << result->added.size() << "):\n";
    for (const lubancode::evolution::EvolutionCoordinator::DiffFile& file : result->added) {
        TermOut() << "    + " << file.rel << "  [" << file.kind << "]  " << file.size << " 字节  "
                  << file.hash.substr(0, 19) << "\n";
    }
    if (!result->skill_summary.empty()) {
        TermOut() << "  SKILL 正文摘要:\n" << result->skill_summary;  // 摘要自带换行
    } else {
        TermOut() << "  (包里没有 skills/*/SKILL.md)\n";
    }
    // ---- 阶段 5:组合件分档摘要 ----
    if (!result->workflow_summary.empty()) {
        TermOut() << "  Workflow:\n    " << result->workflow_summary << "\n";
        for (const std::string& failure : result->workflow_failures) {
            TermOut() << "    失败路: " << failure << "\n";
        }
    }
    if (!result->agent_summary.empty()) {
        TermOut() << "  Agent:\n    " << result->agent_summary << "\n";
    }
    // ---- 阶段 6:代码档草稿与权限差异,如实亮 ----
    if (!result->plugin_summary.empty()) {
        TermOut() << "  Plugin 草稿:\n    " << result->plugin_summary << "\n";
    }
    if (!result->mcp_summary.empty()) {
        TermOut() << "  MCP 草稿:\n    " << result->mcp_summary << "\n";
    }
    if (!result->permission_lines.empty()) {
        TermOut() << "  权限差异(批准页须单列):\n";
        for (const std::string& line : result->permission_lines) {
            TermOut() << "    " << line << "\n";
        }
    }
    if (result->shape == "combination") {
        TermOut() << "  注: 组合件只经静态校验与来源回放夹具;评测执行不起它"
                     "(评测与被测分家)\n";
    }
    if (result->shape == "code-draft") {
        TermOut() << "  注: 草稿零进程零挂载,评测只有静态检查;启用走 /package trust "
                     "人工审查线,/evolve approve 首版明拒自动晋升\n";
    }
    TermOut().flush();
}

// ---- reject:落 rejected,指纹进拒绝账 ----
void RunEvolveReject(const EvolveCommandContext& ctx, const std::string& target, const std::string& reason) {
    lubancode::evolution::ObservationStore observations(BuildStoreRoot(ctx));
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), &observations);
    const auto result = coordinator.Reject(target, reason);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "候选 " << target << " 已拒绝。\n";
    TermOut() << "  指纹: " << result->fingerprint << "(同类不再进观察账,不再被劝)\n";
    TermOut() << "  目录: " << lubancode::platform::PathToUtf8(result->candidate_dir) << "(账保留,不删)\n";
    TermOut().flush();
}

// ---- test:跑评测五道门,账只追加,状态经 Coordinator 迁移(阶段 3)----
void RunEvolveTest(const EvolveCommandContext& ctx, const std::string& target) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr);
    const auto result = coordinator.Test(target);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "评测 " << result->candidate_id << "(" << result->package_id << "),账只追加:\n";
    TermOut() << "  状态: " << result->state_before << " -> " << result->state_after;
    if (result->transitioned_validated) {
        TermOut() << "(静态门全绿)";
    }
    if (result->transitioned_evaluated) {
        TermOut() << "(五道门入账)";
    }
    if (!result->transitioned_validated && !result->transitioned_evaluated &&
        result->state_before == result->state_after) {
        TermOut() << "(状态未动";
        if (!result->static_gate.pass()) {
            TermOut() << ",静态门有错";
        }
        TermOut() << ")";
    }
    TermOut() << "\n";
    if (!result->plan_loaded) {
        TermOut() << "  计划: 读不出(" << result->plan_error << "),只跑了静态门\n";
    }
    // 静态门的发现与错误:密钥/绝对路径发现即 error,亮出来。
    TermOut() << "  静态门: " << (result->static_gate.pass() ? "pass" : "fail")
              << "(doctor " << (result->static_gate.doctor_valid ? "valid" : "invalid")
              << ",诊断 error " << result->static_gate.diagnostics_errors << "/warning "
              << result->static_gate.diagnostics_warnings << ",组件 " <<
        result->static_gate.components_ok << "/" << result->static_gate.components_total
              << " ok,扫描发现 " << result->static_gate.findings.size() << " 处)\n";
    for (const lubancode::evolution::ScanFinding& finding : result->static_gate.findings) {
        TermOut() << "    [" << finding.kind << "] " << finding.path << ":" << finding.line << "  "
                  << finding.detail << "\n";
    }
    for (const std::string& error : result->static_gate.errors) {
        TermOut() << "    [doctor] " << Ellipsize(error, 100) << "\n";
    }
    // 逐门结果:通过几项、没测什么。
    TermOut() << "  结果(" << result->appended.size() << " 行入账):\n";
    for (const lubancode::evolution::EvalResultLine& line : result->appended) {
        if (line.gate == "static") {
            continue;
        }
        TermOut() << "    " << line.gate << " " << line.outcome << "  " << line.task_id << "  (检查 "
                  << line.checks.size() << " 项,tool calls " << line.metrics.tool_calls
                  << ",墙钟 " << line.metrics.wall_clock_ms << "ms,写入 "
                  << line.metrics.workspace_writes << ")\n";
        for (const lubancode::evolution::CheckResult& check : line.checks) {
            TermOut() << "      " << (check.skipped ? "跳过" : (check.pass ? "过" : "败")) << "  "
                      << check.kind << "  " << Ellipsize(check.detail, 72) << "\n";
        }
        for (const std::string& note : line.notes) {
            TermOut() << "      注: " << Ellipsize(note, 88) << "\n";
        }
    }
    TermOut() << "  汇总(确定性;Evaluator 首版=结构化汇总,未接模型):\n";
    PrintEvalSummary(result->run_summary, "    ");
    TermOut() << "  账本: " << lubancode::platform::PathToUtf8(result->candidate_dir /
                                                              "eval-results.jsonl")
              << "\n";
    TermOut() << "  下一步: /evolve show " << result->candidate_id << ";批得动便 /evolve approve "
              << result->candidate_id << "\n";
    TermOut().flush();
}

// ---- approve:出批准页,验门装 store(阶段 4) ----
void RunEvolveApprove(const EvolveCommandContext& ctx, const std::string& target) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr,
                                                           BuildVersionStoreRoot(ctx));
    const auto result = coordinator.Approve(target);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }

    // ---- 批准页(README §十清单;这份材料就是批准绑定的账面) ----
    const auto& brief = result->brief;
    TermOut() << "批准页 " << brief.candidate_id << "(content-only):\n";
    TermOut() << "  Package: " << brief.package_id << "\n";
    TermOut() << "  候选版本: " << brief.candidate_version << "(" << brief.parent_line << ")\n";
    TermOut() << "  内容哈希: " << brief.content_hash << "(批准只认当前哈希;文件变过即作废)\n";
    TermOut() << "  来源: ";
    if (brief.source_lines.empty()) {
        TermOut() << "(演化账未记来源)";
    } else {
        for (std::size_t i = 0; i < brief.source_lines.size() && i < 6; ++i) {
            TermOut() << (i > 0 ? ", " : "") << brief.source_lines[i];
        }
    }
    TermOut() << "\n";
    TermOut() << "  组件: 新增 " << brief.components_added.size() << ",改 "
              << brief.components_changed.size() << ",删 " << brief.components_removed.size()
              << "\n";
    for (const std::string& item : brief.components_added) {
        TermOut() << "    + " << item << "\n";
    }
    TermOut() << "  权限差异: ";
    if (brief.permissions_added.empty() && brief.tools_added.empty()) {
        TermOut() << "无新增工具、进程、网络、env 与文件权限(content-only)\n";
    } else {
        TermOut() << "新工具 " << brief.tools_added.size() << " 件,新权限 "
                  << brief.permissions_added.size() << " 条(须单列审批)\n";
        for (const std::string& item : brief.tools_added) {
            TermOut() << "    tool " << item << "\n";
        }
        for (const std::string& item : brief.permissions_added) {
            TermOut() << "    perm " << item << "\n";
        }
    }
    if (brief.eval_summary.has_value()) {
        TermOut() << "  评测(账只追加):\n";
        PrintEvalSummary(*brief.eval_summary, "    ");
        if (!brief.eval_task_ids.empty()) {
            TermOut() << "    任务样例: ";
            for (std::size_t i = 0; i < brief.eval_task_ids.size() && i < 6; ++i) {
                TermOut() << (i > 0 ? ", " : "") << brief.eval_task_ids[i];
            }
            TermOut() << "\n";
        }
    }
    // 阶段 5:复杂度代价照实亮——组合包比最小 Skill 包多出的组件数与维护面。
    if (brief.complexity.has_value() && !brief.complexity->shape.empty()) {
        TermOut() << "  复杂度代价: " << brief.complexity->SummaryLine() << "\n";
        if (brief.complexity->extra_components > 0) {
            TermOut() << "    (不是组件越多越容易晋升;批的是这份代价换来的编排)\n";
        }
    }
    TermOut() << "  安装位置: " << lubancode::platform::PathToUtf8(result->version_dir)
              << (result->already_present ? "(已在,未重装)" : "(staging 复算哈希 + 静态门后原子落)")
              << "\n";
    TermOut() << "  灰度办法: /evolve use " << brief.candidate_id
              << "(点名 canary;新会话生效,旧任务照旧)\n";
    TermOut() << "  回滚目标: " << brief.rollback_target_line << "\n";
    TermOut() << "已批准并装架: " << brief.package_id << " " << result->installed_version
              << " -> staged(store 里有账,/package list 可见)\n";
    TermOut().flush();
}

// ---- use:点名 canary(阶段 4) ----
void RunEvolveUse(const EvolveCommandContext& ctx, const std::string& target) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr,
                                                           BuildVersionStoreRoot(ctx));
    const auto result = coordinator.Use(target);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "已点名 canary: " << result->package_id << " " << result->version << "\n";
    TermOut() << "  目录: " << lubancode::platform::PathToUtf8(result->version_dir) << "\n";
    TermOut() << "  语义: 新会话/新任务用新版本;在跑会话钉着旧快照,照旧到收场\n";
    TermOut() << "  下一步: /evolve promote " << target << "(晋升 active)或 /evolve rollback "
              << result->package_id << "(切回)\n";
    TermOut().flush();
}

// ---- promote:canary -> active(阶段 4) ----
void RunEvolvePromote(const EvolveCommandContext& ctx, const std::string& target) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr,
                                                           BuildVersionStoreRoot(ctx));
    const auto result = coordinator.Promote(target);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "已晋升 active: " << result->package_id << " " << result->version << "\n";
    TermOut() << "  目录: " << lubancode::platform::PathToUtf8(result->version_dir) << "\n";
    TermOut() << "  语义: 新会话起拿这枚版本;旧任务钉旧快照;canary 指针清空\n";
    TermOut() << "  下一步: /evolve rollback " << result->package_id
              << "(切回父版或指定版;版本与账一枚不删)\n";
    TermOut().flush();
}

// ---- rollback:切回父版或指定版(阶段 4) ----
void RunEvolveRollback(const EvolveCommandContext& ctx, const std::string& target,
                       const std::string& version) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr,
                                                           BuildVersionStoreRoot(ctx));
    const auto result = coordinator.Rollback(target, version);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "已回滚: " << result->package_id;
    if (!result->from_version.empty()) {
        TermOut() << "(原 " << result->from_version << ")";
    }
    if (result->to_version.has_value()) {
        TermOut() << " -> " << *result->to_version << "\n";
    } else {
        TermOut() << " -> 撤下(无父版可回;包不再挂载)\n";
    }
    for (const std::string& candidate_id : result->rolled_back_candidates) {
        TermOut() << "  候选 " << candidate_id << " -> rolled_back\n";
    }
    TermOut() << "  账目: 版本一枚不删,候选/评测/批准/迁移账一笔不抹;新会话拿旧版,\n"
                 "    在跑会话钉着自己的快照照旧跑完\n";
    TermOut().flush();
}

}  // namespace

// ---------------- 纯渲染(TUI 排版批 5c,单测钉) ----------------

std::vector<std::string> FormatEvolveStatusLines(const EvolveStatusModel& model,
                                                 const lubancode::cli::Theme& theme, int width) {
    std::vector<frame::Field> fields;
    {
        std::ostringstream out;
        out << "录制件 " << model.recordings_scanned << "(跳过半截 " << model.recordings_skipped
            << "),workflow run " << model.runs_scanned << ",memory " << model.memory_entries;
        fields.push_back(frame::Field{"采集", out.str()});
    }
    {
        std::ostringstream out;
        out << "新增 " << model.appended << ",重采跳过 " << model.duplicates << ",被拒压下 "
            << model.suppressed;
        fields.push_back(frame::Field{"落账", out.str()});
    }
    {
        std::ostringstream out;
        out << "观察 " << model.ledger_size << " 条,同类簇 " << model.cluster_count << " 个";
        for (const auto& [source, count] : model.by_source) {
            out << "," << source << " " << count;
        }
        fields.push_back(frame::Field{"账面", out.str()});
    }
    fields.push_back(frame::Field{"账本", model.observations_file});
    if (!model.error.empty()) {
        fields.push_back(
            frame::Field{"警告", "有观察没落住账(" + model.error + ")", frame::FieldAccent::Error});
    }
    return frame::RenderKeyValues("自进化观察账(阶段 1:只观察,不生成 Package)", fields,
                                  theme, frame::Light(), width);
}

std::vector<std::string> FormatEvolveClusterLines(std::size_t ledger_size,
                                                  const std::vector<EvolveClusterRow>& rows,
                                                  const lubancode::cli::Theme& theme, int width) {
    std::vector<frame::TableColumn> columns;
    columns.push_back(frame::TableColumn{"fingerprint"});
    columns.push_back(frame::TableColumn{"n", 0, /*align_right=*/true});
    columns.push_back(frame::TableColumn{"source"});
    columns.push_back(frame::TableColumn{"outcome"});
    columns.push_back(frame::TableColumn{"summary"});
    columns.push_back(frame::TableColumn{"ids"});
    std::vector<frame::TableRow> table_rows;
    table_rows.reserve(rows.size());
    for (const EvolveClusterRow& row : rows) {
        table_rows.push_back(frame::TableRow{
            {row.fingerprint, row.count, row.source, row.outcome, row.summary, row.peers}, {}});
    }
    std::ostringstream title;
    title << "观察账(按同类指纹聚类," << ledger_size << " 条 / " << rows.size() << " 簇)";
    return frame::RenderTable(title.str(), columns, table_rows, theme, frame::Light(), width);
}

std::vector<std::string> FormatEvolveCandidateListLines(const std::vector<EvolveCandidateRow>& rows,
                                                        const lubancode::cli::Theme& theme,
                                                        int width) {
    std::vector<frame::TableColumn> columns;
    columns.push_back(frame::TableColumn{"id"});
    columns.push_back(frame::TableColumn{"state"});
    columns.push_back(frame::TableColumn{"package"});
    columns.push_back(frame::TableColumn{"objective"});
    std::vector<frame::TableRow> table_rows;
    table_rows.reserve(rows.size());
    for (const EvolveCandidateRow& row : rows) {
        table_rows.push_back(
            frame::TableRow{{row.candidate_id, row.state, row.package_id, row.objective}, {}});
    }
    std::ostringstream title;
    title << "候选仓(" << rows.size() << " 只,均在候选区,未进 /package)";
    return frame::RenderTable(title.str(), columns, table_rows, theme, frame::Light(), width);
}

std::vector<std::string> FormatEvolveObservationLines(const EvolveObservationModel& model,
                                                      const lubancode::cli::Theme& theme,
                                                      int width) {
    std::vector<frame::Field> fields;
    fields.push_back(frame::Field{"来源", model.source + " " + model.source_id});
    fields.push_back(
        frame::Field{"原始账", model.source_ref.empty() ? "(无)" : model.source_ref});
    fields.push_back(frame::Field{"指纹", model.fingerprint});
    fields.push_back(frame::Field{"摘要", model.summary});
    if (!model.created_at.empty()) {
        fields.push_back(frame::Field{"时间", model.created_at});
    }
    if (!model.details_json.empty()) {
        fields.push_back(frame::Field{"账目", model.details_json});
    }
    std::vector<std::string> lines = frame::RenderKeyValues(
        model.id + "  [" + model.source + " " + model.outcome + "]", fields, theme,
        frame::Light(), width);
    if (!model.evidence.empty()) {
        std::vector<frame::TableColumn> columns;
        columns.push_back(frame::TableColumn{"ref"});
        columns.push_back(frame::TableColumn{"note"});
        std::vector<frame::TableRow> rows;
        rows.reserve(model.evidence.size());
        for (const auto& [ref, note] : model.evidence) {
            rows.push_back(frame::TableRow{{ref, note}, {}});
        }
        for (const std::string& line :
             frame::RenderTable("证据(" + std::to_string(model.evidence.size()) + " 条)",
                                columns, rows, theme, frame::Light(), width)) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::vector<std::string> FormatEvolveCandidatePageLines(const EvolveCandidatePageModel& model,
                                                        const lubancode::cli::Theme& theme,
                                                        int width) {
    std::vector<frame::Field> fields;
    fields.push_back(frame::Field{"目录", model.dir_utf8});
    fields.push_back(
        frame::Field{"整包哈希", model.content_hash.empty() ? "(package/ 缺失)" : model.content_hash});
    if (model.has_shape) {
        fields.push_back(frame::Field{"形状", model.shape});
    }
    if (model.has_record) {
        fields.push_back(frame::Field{"候选版本", model.candidate_version});
        fields.push_back(frame::Field{"目标", model.objective});
        fields.push_back(frame::Field{"来源", model.sources});
        fields.push_back(frame::Field{"生成器", model.generator});
        fields.push_back(frame::Field{"改动", model.changes});
        for (const std::string& tool : model.tools_added) {
            fields.push_back(frame::Field{"", "tool " + tool});
        }
        for (const std::string& perm : model.permissions_added) {
            fields.push_back(frame::Field{"", "perm " + perm});
        }
        if (!model.tools_added.empty() || !model.permissions_added.empty()) {
            fields.push_back(frame::Field{
                "", "code-bearing-draft:零进程零挂载,不自动启用;补实现走 /package trust 人工审查线"});
        }
        fields.push_back(frame::Field{"起草于", model.created_at});
    }
    fields.push_back(frame::Field{
        "批准账", model.has_approval ? model.approval : std::string("(缺——候选不完整)")});
    fields.push_back(frame::Field{
        "评测账", model.has_eval ? std::to_string(model.eval_rows) + " 行,只追加"
                                 : "空(先 /evolve test " + model.candidate_id + ")"});
    return frame::RenderKeyValues(
        model.candidate_id + "  [" + model.state + "]  " + model.package_id, fields, theme,
        frame::Light(), width);
}

std::vector<std::string> FormatEvolveProposeLines(const EvolveProposeModel& model,
                                                  const lubancode::cli::Theme& theme, int width) {
    std::vector<frame::Field> fields;
    fields.push_back(frame::Field{"候选", model.candidate});
    fields.push_back(frame::Field{"整包哈希", model.content_hash});
    fields.push_back(frame::Field{"目录", model.dir_utf8});
    fields.push_back(frame::Field{"形状", model.shape});
    if (!model.rule.empty()) {
        fields.push_back(frame::Field{"草稿规矩", model.rule});
    }
    fields.push_back(frame::Field{"组件", model.components});
    for (const std::string& tool : model.tools_added) {
        fields.push_back(frame::Field{"", "tool " + tool});
    }
    for (const std::string& perm : model.permissions_added) {
        fields.push_back(frame::Field{"", "perm " + perm});
    }
    if (!model.gate.empty()) {
        fields.push_back(frame::Field{"档位门", model.gate});
    }
    if (!model.downgrade.empty()) {
        fields.push_back(frame::Field{"降档", model.downgrade});
    }
    if (model.cluster_skipped > 0) {
        fields.push_back(frame::Field{
            "注", "簇外另有 " + std::to_string(model.cluster_skipped) +
                      " 条同指纹观察找不到可读录制件,未进簇"});
    }
    fields.push_back(frame::Field{"下一步", model.next_step});
    return frame::RenderKeyValues("候选已落(只进候选仓,/package 看不见它)", fields, theme,
                                  frame::Light(), width);
}

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
    if (lower == "propose" || lower == "diff" || lower == "test" || lower == "approve" ||
        lower == "use" || lower == "promote") {
        if (rest.empty()) {
            parsed.action = EvolveCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = lower == "propose"   ? EvolveCommandAction::Propose
                        : lower == "diff"    ? EvolveCommandAction::Diff
                        : lower == "test"    ? EvolveCommandAction::Test
                        : lower == "approve" ? EvolveCommandAction::Approve
                        : lower == "use"     ? EvolveCommandAction::Use
                                             : EvolveCommandAction::Promote;
        parsed.target = rest;
        return parsed;
    }
    if (lower == "rollback") {
        // rollback <package-id> [version]:目标取第一词,第二词(若有)是版本。
        const std::size_t target_space = rest.find_first_of(" \t");
        const std::string rollback_target =
            target_space == std::string::npos ? rest : rest.substr(0, target_space);
        if (rollback_target.empty()) {
            parsed.action = EvolveCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = EvolveCommandAction::Rollback;
        parsed.target = rollback_target;
        if (target_space != std::string::npos) {
            const std::size_t version_space =
                rest.find_first_of(" \t", target_space + 1);
            parsed.target_extra = version_space == std::string::npos
                                      ? Trimmed(rest.substr(target_space + 1))
                                      : rest.substr(target_space + 1,
                                                   version_space - target_space - 1);
        }
        return parsed;
    }
    if (lower == "reject") {
        // reject <candidate-id> [reason]:目标取第一词,余下整段是理由。
        const std::size_t target_space = rest.find_first_of(" \t");
        const std::string reject_target =
            target_space == std::string::npos ? rest : rest.substr(0, target_space);
        if (reject_target.empty()) {
            parsed.action = EvolveCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = EvolveCommandAction::Reject;
        parsed.target = reject_target;
        parsed.reason =
            target_space == std::string::npos ? std::string() : Trimmed(rest.substr(target_space + 1));
        return parsed;
    }
    if (lower == "suggest") {
        // suggest [on|off]:裸跑 = 看状态、门槛与命中账;on/off = 开关。
        parsed.action = EvolveCommandAction::Suggest;
        if (!rest.empty()) {
            const std::string flag = ToLowerAscii(rest);
            if (flag == "on" || flag == "off") {
                parsed.suggest_arg = flag;
            } else {
                parsed.action = EvolveCommandAction::Invalid;
                parsed.bad_word = rest;
            }
        }
        return parsed;
    }
    parsed.action = EvolveCommandAction::Invalid;
    parsed.bad_word = word;
    return parsed;
}

// ---------------- 执行 ----------------

CommandFlow HandleSlashEvolve(const EvolveCommandContext& ctx,
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
        case EvolveCommandAction::Propose:
            RunEvolvePropose(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Diff:
            RunEvolveDiff(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Reject:
            RunEvolveReject(ctx, command.target, command.reason);
            return CommandFlow::Continue;
        case EvolveCommandAction::Test:
            RunEvolveTest(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Approve:
            RunEvolveApprove(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Use:
            RunEvolveUse(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Promote:
            RunEvolvePromote(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Rollback:
            RunEvolveRollback(ctx, command.target, command.target_extra);
            return CommandFlow::Continue;
        case EvolveCommandAction::Suggest:
            RunEvolveSuggest(ctx, command.suggest_arg);
            return CommandFlow::Continue;
        case EvolveCommandAction::Invalid:
            TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

// ---------------- CI 非交互入口:luban evolve test <候选目录> ----------------

namespace {

// JSON 面:一份报告 -> 逐项 + 汇总 + unverified 清单(stdout 吐,CI 吃)。
nlohmann::json BuildEvolveTestJson(const lubancode::evolution::EvolutionCoordinator::TestReport& report,
                                   const std::string& candidate_dir_utf8) {
    nlohmann::json out;
    out["schema"] = 1;
    out["tool"] = "lubancode evolve test";
    nlohmann::json candidate;
    candidate["candidate_id"] = report.candidate_id;
    candidate["package_id"] = report.package_id;
    candidate["content_hash"] = report.content_hash;
    candidate["dir"] = candidate_dir_utf8;
    candidate["state_before"] = report.state_before;
    candidate["state_after"] = report.state_after;
    out["candidate"] = candidate;
    out["plan"] = {{"loaded", report.plan_loaded}, {"error", report.plan_error}};
    nlohmann::json st;
    st["outcome"] = report.static_gate.pass() ? "pass" : "fail";
    st["doctor_valid"] = report.static_gate.doctor_valid;
    st["diagnostics_errors"] = report.static_gate.diagnostics_errors;
    st["diagnostics_warnings"] = report.static_gate.diagnostics_warnings;
    st["components_total"] = report.static_gate.components_total;
    st["components_ok"] = report.static_gate.components_ok;
    st["errors"] = report.static_gate.errors;
    nlohmann::json findings = nlohmann::json::array();
    for (const lubancode::evolution::ScanFinding& finding : report.static_gate.findings) {
        findings.push_back(finding.ToJson());
    }
    st["findings"] = findings;
    out["static"] = st;
    nlohmann::json gates = nlohmann::json::array();
    for (const lubancode::evolution::EvalResultLine& line : report.appended) {
        if (line.gate == "static") {
            continue;
        }
        nlohmann::json item;
        item["gate"] = line.gate;
        item["task_id"] = line.task_id;
        item["outcome"] = line.outcome;
        item["metrics"] = line.metrics.ToJson();
        item["unverified"] = line.unverified;
        item["notes"] = line.notes;
        nlohmann::json checks = nlohmann::json::array();
        for (const lubancode::evolution::CheckResult& check : line.checks) {
            checks.push_back(check.ToJson());
        }
        item["checks"] = checks;
        gates.push_back(std::move(item));
    }
    out["gates"] = gates;
    nlohmann::json summary;
    summary["totals"] = {{"pass", report.run_summary.checks_passed},
                         {"fail", report.run_summary.checks_failed},
                         {"skipped", report.run_summary.checks_skipped}};
    summary["gates"] = {
        {"static", {{"pass", report.run_summary.static_gate.pass},
                    {"fail", report.run_summary.static_gate.fail},
                    {"skipped", report.run_summary.static_gate.skipped}}},
        {"replay", {{"pass", report.run_summary.replay.pass},
                    {"fail", report.run_summary.replay.fail},
                    {"skipped", report.run_summary.replay.skipped}}},
        {"holdout", {{"pass", report.run_summary.holdout.pass},
                     {"fail", report.run_summary.holdout.fail},
                     {"skipped", report.run_summary.holdout.skipped}}},
        {"baseline", {{"pass", report.run_summary.baseline.pass},
                      {"fail", report.run_summary.baseline.fail},
                      {"skipped", report.run_summary.baseline.skipped}}}};
    summary["unverified"] = report.run_summary.unverified;
    summary["holdout_present"] = report.run_summary.has_holdout;
    nlohmann::json cost;
    const auto metric_json = [](const lubancode::evolution::MetricDelta& delta) {
        return nlohmann::json{{"candidate", delta.candidate},
                              {"baseline", delta.baseline},
                              {"has_baseline", delta.has_baseline},
                              {"delta", delta.delta},
                              {"delta_pct", delta.delta_pct}};
    };
    cost["tool_calls"] = metric_json(report.run_summary.tool_calls);
    cost["tokens"] = metric_json(report.run_summary.tokens);
    cost["wall_clock_ms"] = metric_json(report.run_summary.wall_clock_ms);
    cost["permission_prompts"] = metric_json(report.run_summary.permission_prompts);
    cost["workspace_writes"] = metric_json(report.run_summary.workspace_writes);
    cost["success_rate"] = metric_json(report.run_summary.success_rate);
    cost["acceptance_rate"] = metric_json(report.run_summary.acceptance_rate);
    summary["cost_vs_baseline"] = cost;
    summary["baseline_ref"] = report.run_summary.baseline_ref;
    // 阶段 5:复杂度代价(组合包 vs 最小 Skill 包的组件数与维护面)。
    if (report.run_summary.complexity.has_value()) {
        summary["complexity"] = report.run_summary.complexity->ToJson();
    }
    summary["verdict_text"] = BuildDeterministicVerdict(report.run_summary);
    out["summary"] = summary;
    out["exit_code"] = report.exit_code;
    return out;
}

}  // namespace

int RunEvolveTestCommand(const EvolveTestArgs& args) {
    const std::filesystem::path candidate_dir = lubancode::platform::Utf8ToPath(args.candidate_dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate_dir, ec) || ec) {
        if (args.json) {
            nlohmann::json error;
            error["schema"] = 1;
            error["tool"] = "lubancode evolve test";
            error["error"] = "候选目录不存在: " + args.candidate_dir;
            error["exit_code"] = 2;
            std::cout << error.dump(2) << "\n";
        } else {
            std::cerr << "候选目录不存在: " << args.candidate_dir << "\n";
        }
        return 2;
    }

    lubancode::evolution::EvolutionCoordinator::TestOptions options;
    if (!args.baseline_dir.empty()) {
        options.baseline_package_dir = lubancode::platform::Utf8ToPath(args.baseline_dir);
    }
    // TestDir 直收目录;coordinator 的仓根只作兜底(候选目录上一层的上一层)。
    lubancode::evolution::EvolutionCoordinator coordinator(
        candidate_dir.parent_path().parent_path(), nullptr);
    const auto report = coordinator.TestDir(candidate_dir, options);
    if (!report.has_value()) {
        if (args.json) {
            nlohmann::json error;
            error["schema"] = 1;
            error["tool"] = "lubancode evolve test";
            error["error"] = report.error();
            error["exit_code"] = 2;
            std::cout << error.dump(2) << "\n";
        } else {
            std::cerr << report.error() << "\n";
        }
        return 2;
    }

    if (args.json) {
        std::cout << BuildEvolveTestJson(*report, args.candidate_dir).dump(2) << "\n";
        std::cout.flush();
        return report->exit_code;
    }

    // 人话面:与 /evolve test 同一页(少一层会话上下文)。
    TermOut() << "评测 " << report->candidate_id << "(" << report->package_id << "):\n";
    TermOut() << "  状态: " << report->state_before << " -> " << report->state_after << "\n";
    TermOut() << "  静态门: " << (report->static_gate.pass() ? "pass" : "fail") << "(doctor "
              << (report->static_gate.doctor_valid ? "valid" : "invalid") << ",扫描发现 "
              << report->static_gate.findings.size() << " 处)\n";
    for (const lubancode::evolution::ScanFinding& finding : report->static_gate.findings) {
        TermOut() << "    [" << finding.kind << "] " << finding.path << ":" << finding.line << "  "
                  << finding.detail << "\n";
    }
    TermOut() << "  入账 " << report->appended.size() << " 行;\n";
    PrintEvalSummary(report->run_summary, "  ");
    TermOut() << "确定性判词:\n" << BuildDeterministicVerdict(report->run_summary);
    TermOut() << "退出码 " << report->exit_code << "(0 全过 / 1 有 fail / 2 夹具缺失)\n";
    TermOut().flush();
    return report->exit_code;
}

}  // namespace lubancode::app
