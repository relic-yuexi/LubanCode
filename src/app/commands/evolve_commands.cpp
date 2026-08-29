// /evolve 命令的执行体(自进化闭环阶段 1/2)。status = 扫五路账本采观察 +
// 落观察账(只追加)+ 报账面;list = 按指纹聚类列账 + 候选区;show = 看一
// 条观察或一只候选,指回来源。阶段 2 加三条:propose = 从一场录制起草最小
// content-only 候选(EvolutionCoordinator 唯一写口);diff = 与父版或空对照;
// reject = 落 rejected 并把指纹进拒绝账。不进 PackageCatalog、不装任何东西、
// 不改各家源账。
#include "app/commands/evolve_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:统一走输出端口

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "evolution/collector.hpp"
#include "evolution/coordinator.hpp"
#include "evolution/observation_store.hpp"
#include "platform/paths.hpp"
#include "skills/workflow_recorder.hpp"

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

// 候选仓根:<home>/.lubancode/package-candidates(README"候选目录"节)。
// 与正式 Package 目录(packages/、package-store/)分开——四层扫描不扫这里,
// /package list 天然看不见候选(防偷装靠的是目录分仓,不是名单过滤)。
std::filesystem::path BuildCandidateRoot(SlashDispatchContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "package-candidates";
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
                 "      /evolve show <观察id|候选id>\n"
                 "      /evolve propose <recording-id|observation-id>\n"
                 "      /evolve diff <candidate-id>\n"
                 "      /evolve reject <candidate-id> [reason]\n"
                 "阶段 1 只读观察;阶段 2 从录制起草最小 content-only 候选(propose)、看\n"
                 "diff、拒绝(reject)。候选只落候选仓,不进 PackageCatalog,装不进 /package。\n";
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

    // ---- 候选区(阶段 2):观察与候选同一张账面看 ----
    const std::filesystem::path candidate_root = BuildCandidateRoot(ctx);
    if (!candidate_root.empty()) {
        const std::vector<lubancode::evolution::CandidateSummary> candidates =
            lubancode::evolution::CandidateStore(candidate_root).LoadAll();
        if (!candidates.empty()) {
            TermOut() << "\n候选仓(" << candidates.size() << " 只,均在候选区,未进 /package):\n";
            for (const lubancode::evolution::CandidateSummary& candidate : candidates) {
                TermOut() << "  " << candidate.candidate_id << "  ["
                          << lubancode::evolution::ToString(candidate.state) << "]  "
                          << candidate.package_id << "  ";
                if (candidate.record.has_value()) {
                    TermOut() << Ellipsize(candidate.record->objective, 56);
                }
                TermOut() << "\n";
            }
        }
    }
    TermOut().flush();
}

// ---- show:一条观察的全文与证据指回;候选则回指来源 ----
void RunEvolveShowCandidate(SlashDispatchContext& ctx, const std::string& target);
void RunEvolveShow(SlashDispatchContext& ctx, const std::string& target) {
    // 候选 id(cand- 起头)走候选页;其余按观察 id 查。
    if (target.rfind("cand-", 0) == 0) {
        RunEvolveShowCandidate(ctx, target);
        return;
    }
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

// ---- show 候选页:演化账 + 批准账 + 状态 + 来源回指 ----
void RunEvolveShowCandidate(SlashDispatchContext& ctx, const std::string& target) {
    lubancode::evolution::CandidateStore store(BuildCandidateRoot(ctx));
    const auto found = store.Find(target);
    if (!found.has_value()) {
        TermOut() << "没找到候选 \"" << target << "\"(先 /evolve list 看候选区)\n";
        TermOut().flush();
        return;
    }
    TermOut() << found->candidate_id << "  [" << lubancode::evolution::ToString(found->state)
              << "]  " << found->package_id << "\n";
    TermOut() << "  目录: " << lubancode::platform::PathToUtf8(found->dir) << "\n";
    TermOut() << "  整包哈希: " << (found->content_hash.empty() ? "(package/ 缺失)" : found->content_hash)
              << "\n";
    if (found->record.has_value()) {
        const lubancode::evolution::EvolutionRecord& record = *found->record;
        TermOut() << "  候选版本: " << record.candidate_version;
        if (record.parent.has_value()) {
            TermOut() << "(父版 " << record.parent->version << " " << record.parent->content_hash
                      << ")";
        } else {
            TermOut() << "(无父版,与空对照)";
        }
        TermOut() << "\n";
        TermOut() << "  目标: " << Ellipsize(record.objective, 96) << "\n";
        // 来源回指:稳定来源 ID -> 观察账 id(可再 /evolve show 追到原始账)。
        TermOut() << "  来源: ";
        bool any_source = false;
        for (const std::string& id : record.sources.recording_ids) {
            TermOut() << (any_source ? ", " : "") << "recording " << id << " = 观察 "
                      << lubancode::evolution::MakeObservationId(
                             lubancode::evolution::ObservationSource::Recording, id);
            any_source = true;
        }
        for (const std::string& id : record.sources.run_ids) {
            TermOut() << (any_source ? ", " : "") << "run " << id;
            any_source = true;
        }
        for (const std::string& id : record.sources.goal_ids) {
            TermOut() << (any_source ? ", " : "") << "goal " << id;
            any_source = true;
        }
        for (const std::string& id : record.sources.memory_ids) {
            TermOut() << (any_source ? ", " : "") << "memory " << id;
            any_source = true;
        }
        if (!any_source) {
            TermOut() << "(演化账未记来源)";
        }
        TermOut() << "\n";
        TermOut() << "  生成器: " << record.generator.provider << " / " << record.generator.model
                  << " / " << record.generator.prompt_revision << "\n";
        TermOut() << "  改动: 新增组件 ";
        for (const std::string& item : record.changes.components_added) {
            TermOut() << item << " ";
        }
        TermOut() << "权限差异 " << record.changes.permissions_added.size() << " 条,新工具 "
                  << record.changes.tools_added.size() << " 件\n";
        TermOut() << "  起草于: " << (record.created_at.empty() ? "(未记)" : record.created_at) << "\n";
    }
    if (found->approval.has_value()) {
        TermOut() << "  批准账: " << found->approval->tier << " / " << found->approval->status;
        if (found->approval->decision.has_value()) {
            TermOut() << "(由 " << found->approval->decision->decided_by << " 于 "
                      << found->approval->decision->decided_at << " 决定;指纹 "
                      << found->approval->decision->fingerprint << ")";
        }
        TermOut() << "\n";
    } else {
        TermOut() << "  批准账: (缺——候选不完整)\n";
    }
    TermOut() << "  下一步: /evolve diff " << found->candidate_id << ";评测在阶段 3 接\n";
    TermOut().flush();
}

// ---- propose:从一场录制起草最小 content-only 候选 ----
// 目标认两种:录制件 id,或观察 id(obs- 起头,须是 recording 来源)。
void RunEvolvePropose(SlashDispatchContext& ctx, const std::string& target) {
    const std::filesystem::path store_root = BuildStoreRoot(ctx);
    const std::filesystem::path candidate_root = BuildCandidateRoot(ctx);
    if (store_root.empty() || candidate_root.empty()) {
        TermOut() << "没有主目录(.lubancode),候选无处落。\n";
        TermOut().flush();
        return;
    }
    if (ctx.recordings_root == nullptr) {
        TermOut() << "没有录制件根,找不到录制。\n";
        TermOut().flush();
        return;
    }

    // 目标 -> 录制件 id。
    std::string recording_id = target;
    lubancode::evolution::ObservationStore observations(store_root);
    if (target.rfind("obs-", 0) == 0) {
        const auto found = observations.Find(target);
        if (!found.has_value()) {
            TermOut() << "没找到观察 \"" << target << "\"(先 /evolve status 采集一回)\n";
            TermOut().flush();
            return;
        }
        if (found->source != lubancode::evolution::ObservationSource::Recording) {
            TermOut() << "观察 \"" << target << "\" 的来源是 "
                      << lubancode::evolution::ToString(found->source)
                      << ",阶段 2 只从 recording 起草。\n";
            TermOut().flush();
            return;
        }
        recording_id = found->source_id;
    }
    if (recording_id.find('/') != std::string::npos ||
        recording_id.find('\\') != std::string::npos ||
        recording_id.find("..") != std::string::npos) {
        TermOut() << "录制件 id 只认单段目录名: \"" << recording_id << "\"\n";
        TermOut().flush();
        return;
    }

    // 找录制件目录(盘上现查,不靠观察账转述)。
    std::optional<lubancode::skills::RecordingStatus> status;
    for (const lubancode::skills::RecordingStatus& item :
         lubancode::skills::ListRecordings(*ctx.recordings_root)) {
        if (item.id == recording_id) {
            status = item;
            break;
        }
    }
    if (!status.has_value()) {
        TermOut() << "找不到录制件 \"" << recording_id << "\"(先 /record 录一回,再 /evolve status)\n";
        TermOut().flush();
        return;
    }

    lubancode::evolution::EvolutionCoordinator coordinator(candidate_root, &observations);
    const auto result = coordinator.ProposeRecording(
        *status, lubancode::skills::ReadRecordingEvents(status->dir));
    if (!result.has_value()) {
        TermOut() << "起草失败: " << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "候选已落(只进候选仓,/package 看不见它):\n";
    TermOut() << "  候选: " << result->candidate_id << "  [" << result->package_id << " "
              << result->candidate_version << "]\n";
    TermOut() << "  整包哈希: " << result->content_hash << "\n";
    TermOut() << "  目录: " << lubancode::platform::PathToUtf8(result->candidate_dir) << "\n";
    TermOut() << "  组件: " << result->skill_rel_path << "(content-only,无进程无网络)\n";
    TermOut() << "  下一步: /evolve show " << result->candidate_id << " 或 /evolve diff "
              << result->candidate_id << ";评测在阶段 3 接\n";
    TermOut().flush();
}

// ---- diff:与父版或空对照 ----
void RunEvolveDiff(SlashDispatchContext& ctx, const std::string& target) {
    lubancode::evolution::EvolutionCoordinator coordinator(BuildCandidateRoot(ctx), nullptr);
    const auto result = coordinator.Diff(target);
    if (!result.has_value()) {
        TermOut() << result.error() << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "候选 " << result->candidate_id << "(" << result->package_id << ")对照 "
              << result->baseline << ":\n";
    TermOut() << "  新增文件(" << result->added.size() << "):\n";
    for (const lubancode::evolution::EvolutionCoordinator::DiffFile& file : result->added) {
        TermOut() << "    + " << file.rel << "  " << file.size << " 字节  "
                  << file.hash.substr(0, 19) << "\n";
    }
    if (!result->skill_summary.empty()) {
        TermOut() << "  SKILL 正文摘要:\n" << result->skill_summary;  // 摘要自带换行
    } else {
        TermOut() << "  (包里没有 skills/*/SKILL.md)\n";
    }
    TermOut().flush();
}

// ---- reject:落 rejected,指纹进拒绝账 ----
void RunEvolveReject(SlashDispatchContext& ctx, const std::string& target, const std::string& reason) {
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
    if (lower == "propose" || lower == "diff") {
        if (rest.empty()) {
            parsed.action = EvolveCommandAction::Invalid;
            parsed.bad_word = word;
            return parsed;
        }
        parsed.action = lower == "propose" ? EvolveCommandAction::Propose : EvolveCommandAction::Diff;
        parsed.target = rest;
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
        case EvolveCommandAction::Propose:
            RunEvolvePropose(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Diff:
            RunEvolveDiff(ctx, command.target);
            return CommandFlow::Continue;
        case EvolveCommandAction::Reject:
            RunEvolveReject(ctx, command.target, command.reason);
            return CommandFlow::Continue;
        case EvolveCommandAction::Invalid:
            TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
