// /evolve 命令的执行体(自进化闭环阶段 1/2/3/4)。status = 扫五路账本采观察 +
// 落观察账(只追加)+ 报账面;list = 按指纹聚类列账 + 候选区;show = 看一
// 条观察或一只候选,指回来源。阶段 2:propose/diff/reject。阶段 3:test 跑
// 评测五道门。阶段 4:approve 出批准页并原子装 store、use 点名 canary、
// promote 晋升、rollback 切回——迁移全走 EvolutionCoordinator(唯一写口),
// 这里只递材料、只打印。
#include "app/commands/evolve_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:统一走输出端口

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "evolution/collector.hpp"
#include "evolution/coordinator.hpp"
#include "evolution/eval.hpp"
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

// version store 根:<home>/.lubancode/package-store(README"晋升、灰度与
// 回滚"节)。批准后版本原子落这里;active/canary 指针在 channels.json。
// (与上面的 BuildStoreRoot 是两处账:那边是观察账,这边是版本仓。)
std::filesystem::path BuildVersionStoreRoot(SlashDispatchContext& ctx) {
    if (ctx.home_lubancode == nullptr || !ctx.home_lubancode->has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(**ctx.home_lubancode) / "package-store";
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
                 "      /evolve test <candidate-id>\n"
                 "      /evolve approve <candidate-id>\n"
                 "      /evolve use <candidate-id>\n"
                 "      /evolve promote <candidate-id>\n"
                 "      /evolve rollback <package-id> [version]\n"
                 "阶段 1 只读观察;阶段 2 从录制起草最小 content-only 候选(propose)、看\n"
                 "diff、拒绝(reject);阶段 3 评测(test):静态门+回放+留出+基线对照,账只追加;\n"
                 "阶段 4 批准与灰度(approve/use/promote/rollback):批准只认当前哈希,批准后\n"
                 "原子落 package-store,点名 canary 新会话生效、旧任务照旧,回滚不删版本不抹账。\n"
                 "code-bearing 候选(带 Plugin/MCP)不走 approve,另过 Package trust。CI 入口:\n"
                 "  lubancode evolve test <候选目录> --baseline <父包目录> --json\n";
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
        const auto cost = [&summary](const lubancode::evolution::MetricDelta& delta,
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
    // ---- 评测账摘要(阶段 3):通过几项、没测什么、比基线贵多少 ----
    const std::vector<lubancode::evolution::EvalResultLine> results =
        lubancode::evolution::LoadEvalResults(found->dir / "eval-results.jsonl");
    if (results.empty()) {
        TermOut() << "  评测账: 空(先 /evolve test " << found->candidate_id << ")\n";
    } else {
        const lubancode::evolution::EvalSummary summary =
            lubancode::evolution::SummarizeEvalLedger(results);
        TermOut() << "  评测账(" << results.size() << " 行,只追加):\n";
        PrintEvalSummary(summary, "    ");
    }
    TermOut() << "  下一步: /evolve diff " << found->candidate_id << ";评测入账后 /evolve approve "
              << found->candidate_id << " 出批准页(只认当前哈希)\n";
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
    TermOut() << "  下一步: /evolve test " << result->candidate_id << "(评测五道门)或 /evolve show "
              << result->candidate_id << "\n";
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

// ---- test:跑评测五道门,账只追加,状态经 Coordinator 迁移(阶段 3)----
void RunEvolveTest(SlashDispatchContext& ctx, const std::string& target) {
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
void RunEvolveApprove(SlashDispatchContext& ctx, const std::string& target) {
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
void RunEvolveUse(SlashDispatchContext& ctx, const std::string& target) {
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
void RunEvolvePromote(SlashDispatchContext& ctx, const std::string& target) {
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
void RunEvolveRollback(SlashDispatchContext& ctx, const std::string& target,
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
