// 阶段 7"有限自动建议"的实现。全文只读观察账与候选仓、只写开关账与
// 命中账——起草、测试、安装一件不碰,那是 EvolutionCoordinator 的笔。
#include "evolution/suggest.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include "platform/paths.hpp"

namespace lubancode::evolution {

namespace {

std::string IsoNowUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// 五门判定
// ---------------------------------------------------------------------------

int ShapeStepsOf(const EvolutionObservation& observation) {
    const char* keys[] = {"tools", "nodes"};
    for (const char* key : keys) {
        if (observation.details.is_object() && observation.details.contains(key) &&
            observation.details.at(key).is_array()) {
            return static_cast<int>(observation.details.at(key).size());
        }
    }
    return 0;
}

SuggestionVerdict AssessSuggestion(const std::vector<EvolutionObservation>& cluster,
                                   const std::vector<std::string>& blocked,
                                   const SuggestThresholds& thresholds) {
    SuggestionVerdict verdict;
    if (cluster.empty()) {
        verdict.why_not.push_back("簇是空的,无从谈起");
        return verdict;
    }
    verdict.fingerprint = cluster.front().fingerprint;
    verdict.cluster_size = static_cast<int>(cluster.size());
    verdict.summary = cluster.front().summary;
    verdict.representative_obs_id = cluster.front().id;
    verdict.shape_steps = ShapeStepsOf(cluster.front());

    // 门一:独立任务证据——不同 source_id 的观察数。
    std::set<std::string> source_ids;
    for (const EvolutionObservation& observation : cluster) {
        source_ids.insert(observation.source_id);
    }
    verdict.independent_tasks = static_cast<int>(source_ids.size());
    verdict.gate_tasks = verdict.independent_tasks >= thresholds.min_independent_tasks;
    if (!verdict.gate_tasks) {
        verdict.why_not.push_back("只有 " + std::to_string(verdict.independent_tasks) +
                                  " 场独立任务(< " + std::to_string(thresholds.min_independent_tasks) +
                                  ");同场重试折在同一条观察里,凑不成独立证据");
    }

    // 门二:输入、产物、验收大体同形。指纹 = 目标口述 + 验收口述 + 折叠
    // 工具序列的归一哈希(日期/网址/绝对路径在归一化里抽象掉),同指纹
    // 即同形;簇里还得真有一条非空形状,空形状无从同形。
    bool same_fingerprint = true;
    for (const EvolutionObservation& observation : cluster) {
        if (observation.fingerprint != verdict.fingerprint) {
            same_fingerprint = false;
            break;
        }
    }
    verdict.gate_shape = same_fingerprint && verdict.shape_steps >= thresholds.min_shape_steps;
    if (!verdict.gate_shape) {
        verdict.why_not.push_back(same_fingerprint
                                      ? "簇里没有带形状的观察(工具/节点序列为空),同形无从判"
                                      : "簇内指纹不一致(调用方聚错了簇)");
    }

    // 门三:非偶然——不同 source_id 的成功观察数(recording 的 success 必有
    // 验证证据,阶段 1 采集器把过关;两场各自走通才算路子,单场成功可能
    // 是撞上的)。
    std::set<std::string> success_sources;
    for (const EvolutionObservation& observation : cluster) {
        if (observation.outcome == ObservationOutcome::Success) {
            success_sources.insert(observation.source_id);
        }
    }
    verdict.independent_successes = static_cast<int>(success_sources.size());
    verdict.gate_not_accidental =
        verdict.independent_successes >= thresholds.min_independent_successes;
    if (!verdict.gate_not_accidental) {
        verdict.why_not.push_back("带验证证据的独立成功只有 " +
                                  std::to_string(verdict.independent_successes) +
                                  " 场(< " + std::to_string(thresholds.min_independent_successes) +
                                  ");一次成功可能是偶然,不劝");
    }

    // 门四:无同 fingerprint 的 pending/rejected 候选(拒绝后不死缠;已
    // 提炼过的指纹也不再劝)。
    if (std::find(blocked.begin(), blocked.end(), verdict.fingerprint) != blocked.end()) {
        verdict.gate_no_pending_or_rejected = false;
        verdict.why_not.push_back("同指纹已有在途或被拒的候选(观察账拒绝指纹账与候选仓一并挡门),"
                                 "内容未变不再劝");
    }

    // 门五:能说明比现有 Memory、Skill 或 Package 多解决什么。簇的形状里
    // 有做法(>=min_shape_steps 步)才值得起包;落哪一档(Skill/组合/代码
    // 草稿)是起草器两把尺的事,提示只说清"Memory 装不下这套做法"。
    verdict.gate_benefit = verdict.shape_steps >= thresholds.min_shape_steps;
    if (verdict.gate_benefit) {
        verdict.benefit_line =
            "Memory 只装得下一句事实,这套 " + std::to_string(verdict.shape_steps) +
            " 步的做法装不下——起包(Skill/组合,档位起草器定)换项目可复用;"
            "比现有件多解决的是整套步骤,不是一条偏好";
    } else {
        verdict.why_not.push_back("这簇没有工具/节点序列,是一句事实或偏好,Memory 就装得下,"
                                 "不值得起包");
    }

    verdict.eligible = verdict.gate_tasks && verdict.gate_shape && verdict.gate_not_accidental &&
                       verdict.gate_no_pending_or_rejected && verdict.gate_benefit;
    return verdict;
}

std::vector<std::string> CollectBlockedFingerprints(const ObservationStore& observations,
                                                    const CandidateStore& candidates) {
    std::set<std::string> blocked;
    // 观察账的拒绝指纹:内容未变的同款不再进账,也不再劝。
    for (const RejectedFingerprint& rejected : observations.LoadRejected()) {
        if (!rejected.fingerprint.empty()) {
            blocked.insert(rejected.fingerprint);
        }
    }
    // 候选仓:来源观察还查得到的,按指纹挡门。在途挡(别催),active/
    // staged 挡(已提炼),rejected 挡(与拒绝账同源双保险);查不到来源
    // 指纹的候选不硬猜(来源账被清是另一回事,不该在这里编)。
    const std::vector<EvolutionObservation> ledger = observations.Load();
    for (const CandidateSummary& candidate : candidates.LoadAll()) {
        if (!candidate.record.has_value()) {
            continue;
        }
        for (const std::string& recording_id : candidate.record->sources.recording_ids) {
            const std::string observation_id =
                MakeObservationId(ObservationSource::Recording, recording_id);
            for (const EvolutionObservation& observation : ledger) {
                if (observation.id == observation_id && !observation.fingerprint.empty()) {
                    blocked.insert(observation.fingerprint);
                    break;
                }
            }
        }
    }
    return std::vector<std::string>(blocked.begin(), blocked.end());
}

// ---------------------------------------------------------------------------
// 开关账:缺省关闭是铁律——读不出一律 false。
// ---------------------------------------------------------------------------

bool LoadSuggestEnabled(const std::filesystem::path& evolution_root) {
    std::error_code ec;
    const std::filesystem::path file = evolution_root / "suggest.json";
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        return false;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const nlohmann::json parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    if (!parsed.contains("enabled") || !parsed.at("enabled").is_boolean()) {
        return false;
    }
    return parsed.at("enabled").get<bool>();
}

std::optional<std::string> SaveSuggestEnabled(const std::filesystem::path& evolution_root,
                                              bool enabled) {
    std::error_code ec;
    std::filesystem::create_directories(evolution_root, ec);
    nlohmann::json state;
    state["schema"] = 1;
    state["enabled"] = enabled;
    std::ofstream output(evolution_root / "suggest.json", std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return "开关账写不动: " + lubancode::platform::PathToUtf8(evolution_root / "suggest.json");
    }
    output << state.dump(2) << "\n";
    if (!output.good()) {
        return "开关账没写完整: " + lubancode::platform::PathToUtf8(evolution_root / "suggest.json");
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 命中账:只追加 JSONL。
// ---------------------------------------------------------------------------

namespace {

std::optional<SuggestEvent> ParseSuggestEvent(const std::string& line) {
    const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    auto get_string = [&parsed](const char* key) {
        return parsed.contains(key) && parsed.at(key).is_string()
                   ? parsed.at(key).get<std::string>()
                   : std::string();
    };
    if (!parsed.contains("schema") || !parsed.at("schema").is_number_integer() ||
        parsed.at("schema").get<int>() != 1) {
        return std::nullopt;
    }
    SuggestEvent event;
    event.type = get_string("type");
    event.fingerprint = get_string("fingerprint");
    event.at = get_string("at");
    event.benefit = get_string("benefit");
    event.obs_id = get_string("obs_id");
    event.candidate_id = get_string("candidate_id");
    if (parsed.contains("cluster_size") && parsed.at("cluster_size").is_number_integer()) {
        event.cluster_size = parsed.at("cluster_size").get<int>();
    }
    if (event.type != "shown" && event.type != "accepted") {
        return std::nullopt;
    }
    if (event.fingerprint.empty()) {
        return std::nullopt;
    }
    return event;
}

}  // namespace

SuggestLedger::SuggestLedger(std::filesystem::path file) : file_(std::move(file)) {}

std::optional<std::string> SuggestLedger::Append(const SuggestEvent& event) {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    nlohmann::json line;
    line["schema"] = 1;
    line["type"] = event.type;
    line["fingerprint"] = event.fingerprint;
    line["at"] = event.at.empty() ? IsoNowUtc() : event.at;
    if (event.type == "shown") {
        line["cluster_size"] = event.cluster_size;
        line["benefit"] = event.benefit;
        line["obs_id"] = event.obs_id;
    } else {
        line["candidate_id"] = event.candidate_id;
    }
    std::ofstream output(file_, std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return "命中账写不动: " + lubancode::platform::PathToUtf8(file_);
    }
    output << line.dump() << "\n";
    if (!output.good()) {
        return "命中账没写完整: " + lubancode::platform::PathToUtf8(file_);
    }
    return std::nullopt;
}

std::vector<SuggestEvent> SuggestLedger::Load() const {
    std::vector<SuggestEvent> events;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file_, ec) || ec) {
        return events;
    }
    std::ifstream input(file_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (const auto parsed = ParseSuggestEvent(line); parsed.has_value()) {
            events.push_back(std::move(*parsed));
        }
    }
    return events;
}

SuggestLedger::Stats SuggestLedger::ComputeStats() const {
    Stats stats;
    std::set<std::string> shown;
    std::set<std::string> accepted;
    for (const SuggestEvent& event : Load()) {
        if (event.type == "shown") {
            ++stats.shown_events;
            shown.insert(event.fingerprint);
        } else if (event.type == "accepted") {
            ++stats.accepted_events;
            accepted.insert(event.fingerprint);
        }
    }
    stats.shown_fingerprints = static_cast<int>(shown.size());
    stats.accepted_fingerprints = static_cast<int>(accepted.size());
    if (stats.shown_fingerprints > 0) {
        stats.acceptance_rate =
            static_cast<double>(stats.accepted_fingerprints) /
            static_cast<double>(stats.shown_fingerprints);
    }
    return stats;
}

bool SuggestLedger::HasOpenSuggestion(const std::string& fingerprint) const {
    bool shown = false;
    for (const SuggestEvent& event : Load()) {
        if (event.fingerprint != fingerprint) {
            continue;
        }
        if (event.type == "shown") {
            shown = true;
        } else if (event.type == "accepted") {
            return false;  // 已记过接受,不再记第二笔
        }
    }
    return shown;
}

}  // namespace lubancode::evolution
