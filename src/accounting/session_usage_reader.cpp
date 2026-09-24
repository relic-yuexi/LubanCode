#include "accounting/session_usage_reader.hpp"

#include <algorithm>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "accounting/usage_projector.hpp"
#include "platform/paths.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/event.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"

namespace lubancode::accounting {
namespace {

// 目录存在与否(undefined/不存在都算没有)。
bool DirExists(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
}

// 一条 stream 的全部信封(按行序)。空文件给空表(合法:占位未写)。
// 坏行/坏信封 → nullopt,调用方点名,不出半条账。
std::optional<std::vector<trajectory::EventEnvelope>> ParseStream(
    const std::filesystem::path& path) {
    const auto lines = trajectory::ReadJournalLines(path);
    if (!lines.has_value()) {
        return std::nullopt;
    }
    std::vector<trajectory::EventEnvelope> envelopes;
    envelopes.reserve(lines->size());
    for (const auto& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            return std::nullopt;
        }
        trajectory::EventEnvelope envelope;
        if (trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            return std::nullopt;
        }
        envelopes.push_back(std::move(envelope));
    }
    return envelopes;
}

// ---- 两代清单(V3-GAP-01:ListSessionStreams 并出 v2/v3 两代账)----

// v2 布局件:main.jsonl + 平铺 subagents/<run>.jsonl + workflows/
// <run>/{workflow.jsonl, nodes/*.jsonl}(§3.5/§3.6)。只认盘上文件,
// 格式判断归读侧。
std::vector<std::filesystem::path> ListV2LayoutStreams(
    const std::filesystem::path& session_dir) {
    std::vector<std::filesystem::path> streams;
    const auto push_if_file = [&](const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            streams.push_back(path);
        }
    };
    // main.jsonl
    push_if_file(session_dir / "main.jsonl");
    // subagents/<agent_run_id>.jsonl(嵌套同目录,§3.5)
    std::error_code ec;
    const std::filesystem::path subagents = session_dir / "subagents";
    if (std::filesystem::is_directory(subagents, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(subagents, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
                streams.push_back(entry.path());
            }
        }
    }
    // workflows/<run>/{workflow.jsonl, nodes/*.jsonl}(§3.6)
    const std::filesystem::path workflows = session_dir / "workflows";
    if (std::filesystem::is_directory(workflows, ec)) {
        for (const auto& run : std::filesystem::directory_iterator(workflows, ec)) {
            if (!run.is_directory()) {
                continue;
            }
            push_if_file(run.path() / "workflow.jsonl");
            const std::filesystem::path nodes = run.path() / "nodes";
            if (std::filesystem::is_directory(nodes, ec)) {
                for (const auto& node : std::filesystem::directory_iterator(nodes, ec)) {
                    if (node.is_regular_file() && node.path().extension() == ".jsonl") {
                        streams.push_back(node.path());
                    }
                }
            }
        }
    }
    return streams;
}

// v3 子 session 递归收集:subagents/<childId>/<childId>.jsonl,再钻 child
// 自己的 subagents/(父卷同层嵌套,§4.31)。visited 按规范化绝对路径
// 去重防环,深度封顶 8(与 WalkSessionTree 同限)。平铺 .jsonl 文件是
// v2 布局件,归 ListV2LayoutStreams,这里只认目录。
void CollectV3SubagentLedgers(const std::filesystem::path& session_dir, int depth,
                              std::set<std::filesystem::path>& visited,
                              std::vector<std::filesystem::path>& out) {
    if (depth > 8) {
        return;
    }
    std::error_code ec;
    const std::filesystem::path subagents = session_dir / "subagents";
    if (!std::filesystem::is_directory(subagents, ec) || ec) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(subagents, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::filesystem::path child = entry.path();
        std::error_code norm_ec;
        const std::filesystem::path key =
            std::filesystem::absolute(child, norm_ec).lexically_normal();
        if (norm_ec) {
            continue;
        }
        if (!visited.insert(key).second) {
            continue;  // 环/重复:同一子目录只钻一次
        }
        const std::string id = platform::PathToUtf8(child.filename());
        if (id.empty()) {
            continue;
        }
        std::error_code file_ec;
        const std::filesystem::path ledger = child / platform::Utf8ToPath(id + ".jsonl");
        if (std::filesystem::is_regular_file(ledger, file_ec)) {
            out.push_back(ledger);
        }
        CollectV3SubagentLedgers(child, depth + 1, visited, out);
    }
}

// v3 布局件:主账 <sessionId>.jsonl(目录名单段即 session_id;首行
// schemaVersion==3 才算,probe 同口径只读首行不整卷验链)+ 递归子账。
// 主账验不明就当没有 v3 布局(格式裁决归 ReadSessionUsage 的探针)。
std::vector<std::filesystem::path> ListV3LayoutStreams(
    const std::filesystem::path& session_dir) {
    std::vector<std::filesystem::path> streams;
    const std::string id = platform::PathToUtf8(session_dir.filename());
    if (id.empty()) {
        return streams;
    }
    const std::filesystem::path main =
        session_dir / platform::Utf8ToPath(id + ".jsonl");
    const auto first = trajectory::v3::ReadV3FirstLine(main);
    if (!first.has_value() ||
        first->value("schemaVersion", 0) != trajectory::v3::kSchemaVersion) {
        return streams;
    }
    streams.push_back(main);
    std::set<std::filesystem::path> visited;
    CollectV3SubagentLedgers(session_dir, 0, visited, streams);
    return streams;
}

// ---- v3 读法(T06/V3-GAP-01)----

// 树内递归收集:根账 + 递归子 session(WalkSessionTree 已做环检测与
// visited 去重;cycle/missing/unreadable 各自点名,不重复累计)。
void CollectV3TreeUsage(const trajectory::v3::SubagentSessionNode& node, bool is_root,
                        const std::string& root_run_kind, SessionUsageRead& result) {
    if (node.ledger.has_value()) {
        V3UsageProjectorContext context;
        context.is_subagent = !is_root;
        context.run_kind = is_root ? root_run_kind
                                   : trajectory::RunKindName(trajectory::RunKind::Subagent);
        UsageProjection projection = ProjectV3Usage(*node.ledger, context);
        // 验卷已过仍拒投影(投影自身 bug 防线):整树如实降级,不出半份账。
        if (!projection.ok) {
            result.warnings.push_back("usage.v3_stream_rejected: " + node.session_id + ": " +
                                      projection.error_code);
        } else {
            for (const auto& warning : projection.warnings) {
                result.warnings.push_back(warning);
            }
            for (auto& sample : projection.samples) {
                result.samples.push_back(std::move(sample));
            }
        }
    } else if (!is_root) {
        // 根账读不了在 ReadSessionUsageV3 入口整体拒;这里只有子账的缺口。
        result.warnings.push_back("usage.v3_subsession_unreadable: " + node.session_id);
    }
    for (const auto& child : node.children) {
        if (child.link_status == "cycle") {
            // 同一子 session 被再次 spawn:树内已去重,点名不计第二遍。
            result.warnings.push_back("usage.v3_subsession_cycle_dedup: " + child.session_id);
            continue;
        }
        if (child.link_status == "child_missing") {
            result.warnings.push_back("usage.v3_subsession_missing: " + child.session_id);
            continue;
        }
        if (child.link_status == "unreadable") {
            result.warnings.push_back("usage.v3_subsession_unreadable: " + child.session_id);
            continue;
        }
        if (child.link_status != "linked" && child.link_status != "spawned") {
            // not_linked/spawn_failed 但子账可读:执行过就有真实花费,照计,
            // 关联状态点名(不漏识别,也不静默当正常)。
            result.warnings.push_back("usage.v3_subsession_link_status: " + child.session_id +
                                      "(" + child.link_status + ")");
        }
        CollectV3TreeUsage(child, /*is_root=*/false, root_run_kind, result);
    }
}

// ---- 两代并账(V3-GAP-01:v3 为准,旧账只补缺失段)----

// 两代并存时收的旧件:main.jsonl + 平铺 subagents/*.jsonl。workflows
// 不收——编排账照旧 v2 且与 v3 同代并行(V3-GAP-05 另管),并进来只会
// 整卷落进"开账后不计"的噪音,不是"v3 缺失段"。
std::vector<std::filesystem::path> ListLegacyMergeStreams(
    const std::filesystem::path& session_dir) {
    std::vector<std::filesystem::path> streams;
    const auto push_if_file = [&](const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            streams.push_back(path);
        }
    };
    push_if_file(session_dir / "main.jsonl");
    std::error_code ec;
    const std::filesystem::path subagents = session_dir / "subagents";
    if (std::filesystem::is_directory(subagents, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(subagents, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
                streams.push_back(entry.path());
            }
        }
    }
    return streams;
}

// v3 开账时刻:树内各账时间线最早一枚(单写者按序落盘,首行即最早;
// 验卷已过)。全树一行没有 → nullopt——无从划界,也无从双计。
void ConsiderEarliestLine(const trajectory::v3::SubagentSessionNode& node,
                          std::optional<std::int64_t>& earliest) {
    if (node.ledger.has_value() && !node.ledger->timeline.empty()) {
        const auto& entry = node.ledger->timeline.front();
        const std::string& iso =
            entry.is_message ? node.ledger->messages[entry.index].timestamp
                             : node.ledger->events[entry.index].timestamp;
        if (const auto ms = trajectory::v3::ParseV3TimestampMs(iso)) {
            if (!earliest.has_value() || *ms < *earliest) {
                earliest = ms;
            }
        }
    }
    for (const auto& child : node.children) {
        ConsiderEarliestLine(child, earliest);
    }
}

// 旧 main.jsonl/平铺子账并入:按 v3 开账时刻划界,只收之前的段(补缺),
// 之后一笔不计(防双计),并账与弃账各自点名。旧 stream 坏/被拒:点名
// 跳过,v3 部分照读(与 v2 路一场一坏 stream 同口径)。时间认不出(全树
// 无时间线)→ 无从双计,旧账全收。
void MergeLegacyStreams(const trajectory::v3::SubagentSessionNode& tree,
                        const std::filesystem::path& session_dir, SessionUsageRead& result) {
    const auto legacy = ListLegacyMergeStreams(session_dir);
    if (legacy.empty()) {
        return;
    }
    std::optional<std::int64_t> window_start;
    ConsiderEarliestLine(tree, window_start);
    for (const auto& path : legacy) {
        auto envelopes = ParseStream(path);
        if (!envelopes.has_value()) {
            result.warnings.push_back("usage.stream_unreadable: " + path.filename().string());
            continue;
        }
        // 请求 → 该请求最早事件时刻(划界判据;不带 request_id 的事件不参与)。
        std::map<std::string, std::int64_t> request_time;
        for (const auto& envelope : *envelopes) {
            if (!envelope.request_id.has_value()) {
                continue;
            }
            const auto [it, inserted] =
                request_time.emplace(*envelope.request_id, envelope.wall_time_ms);
            if (!inserted && envelope.wall_time_ms < it->second) {
                it->second = envelope.wall_time_ms;
            }
        }
        UsageProjection projection = ProjectUsage(*envelopes);
        if (!projection.ok) {
            result.warnings.push_back("usage.stream_rejected: " + path.filename().string() +
                                      ": " + projection.error_code);
            continue;
        }
        for (const auto& warning : projection.warnings) {
            result.warnings.push_back(warning);
        }
        int kept = 0;
        int dropped = 0;
        for (auto& sample : projection.samples) {
            const auto it = request_time.find(sample.request_id);
            if (window_start.has_value() && it != request_time.end() &&
                it->second >= *window_start) {
                dropped += 1;
                continue;
            }
            kept += 1;
            if (result.workspace_key.empty()) {
                result.workspace_key = sample.workspace_key;
            }
            result.samples.push_back(std::move(sample));
        }
        if (kept > 0) {
            result.warnings.push_back("usage.legacy_merged: " + path.filename().string() + ": " +
                                      std::to_string(kept) + " 笔(v3 开账前段,补缺并入)");
        }
        if (dropped > 0) {
            result.warnings.push_back("usage.legacy_overlap_dropped: " +
                                      path.filename().string() + ": " + std::to_string(dropped) +
                                      " 笔落在 v3 开账后,不计(防双计)");
        }
    }
}

SessionUsageRead ReadSessionUsageV3(const std::filesystem::path& session_dir,
                                    const std::filesystem::path& stream) {
    SessionUsageRead result;
    result.format = "v3";
    // session.json 与 v2 路径同款:读不到不拦账,status=unknown 点名。
    std::string run_kind = trajectory::RunKindName(trajectory::RunKind::MainSession);
    if (const auto manifest = trajectory::ReadSessionJson(session_dir)) {
        result.session_id = manifest->session_id;
        result.workspace_key = manifest->workspace_key;
        result.status = manifest->status;
        if (!manifest->run_kind.empty()) {
            run_kind = manifest->run_kind;
        }
    } else {
        result.status = "unknown";
        result.warnings.push_back("usage.session_manifest_missing: " + session_dir.string());
    }
    // 主账验卷不过 = 整场读不了:ok=false,不拿空样本伪装零消耗(T06)。
    const auto tree = trajectory::v3::WalkSessionTree(stream);
    if (!tree.ledger.has_value()) {
        // 错误详情再验一遍拿回来(坏账本来就要人工修,双读一次可接受)。
        const auto own = trajectory::v3::ReadV3Ledger(stream);
        result.error_code = "usage.v3_ledger_unreadable";
        result.message = "v3 主账验卷不过,不产残账: " + platform::PathToUtf8(stream) +
                         " (" + (own.has_value() ? std::string("tree walk 失败") : own.error()) +
                         ")";
        if (result.session_id.empty()) {
            result.session_id = session_dir.filename().string();
        }
        return result;
    }
    if (result.session_id.empty()) {
        result.session_id = tree.ledger->session_id.empty()
                                ? session_dir.filename().string()
                                : tree.ledger->session_id;
    }
    // 子 session 递归:环/缺/坏各自点名,树内去重不重复计费。
    CollectV3TreeUsage(tree, /*is_root=*/true, run_kind, result);
    // workspace_key:v3 信封不带,统一回填 manifest 的(manifest 缺则空,
    // 警告已点名)。
    for (auto& sample : result.samples) {
        if (sample.workspace_key.empty()) {
            sample.workspace_key = result.workspace_key;
        }
    }
    // 两代并账:旧 main.jsonl/平铺子账在就并(v3 为准,只补开账前段)。
    // 正经 v3 场没有这些文件,零开销零警告,行为与并账前一字不差。
    MergeLegacyStreams(tree, session_dir, result);
    result.ok = true;
    return result;
}

}  // namespace

std::optional<std::vector<std::filesystem::path>> ListSessionStreams(
    const std::filesystem::path& session_dir) {
    if (!DirExists(session_dir)) {
        return std::nullopt;
    }
    // 两代并出(V3-GAP-01):v2 布局件 + v3 主账/递归子账,字典序。
    // 清单只认盘上文件;怎么读、怎么去重归 ReadSessionUsage。
    std::vector<std::filesystem::path> streams = ListV2LayoutStreams(session_dir);
    const auto v3_streams = ListV3LayoutStreams(session_dir);
    streams.insert(streams.end(), v3_streams.begin(), v3_streams.end());
    std::sort(streams.begin(), streams.end());
    return streams;
}

SessionUsageRead ReadSessionUsage(const std::filesystem::path& session_dir) {
    SessionUsageRead result;
    // ---- 格式分派(T06/V3-GAP-01):先探 v3,再走 v2 老路。----
    // 目录不存在仍按"没有这场 session"报(不算账错)。
    const auto probe = trajectory::v3::ProbeV3SessionStream(session_dir);
    switch (probe.status) {
        case trajectory::v3::V3StreamProbe::Status::V3Stream:
            return ReadSessionUsageV3(session_dir, probe.stream);
        case trajectory::v3::V3StreamProbe::Status::FormatConflict: {
            // 两种主账并存(V3-GAP-01):<id>.jsonl 首行验明 schemaVersion==3
            // → v3 为准并账(旧账只补 v3 开账前段,不双计);验不明(异
            // 版本/坏首行)仍按冲突拒读,不猜格式。
            const std::string id = platform::PathToUtf8(session_dir.filename());
            const std::filesystem::path v3_main =
                session_dir / platform::Utf8ToPath(id + ".jsonl");
            const auto first = trajectory::v3::ReadV3FirstLine(v3_main);
            if (first.has_value() &&
                first->value("schemaVersion", 0) == trajectory::v3::kSchemaVersion) {
                return ReadSessionUsageV3(session_dir, v3_main);
            }
            result.error_code = "usage.session_format_conflict";
            result.message =
                "两种主账打架,不敢认:" + probe.detail + "(main.jsonl 与 <id>.jsonl 并存)";
            result.session_id = session_dir.filename().string();
            return result;
        }
        case trajectory::v3::V3StreamProbe::Status::NotV3Schema:
            // <id>.jsonl 在但 schemaVersion 不是 3:异种/旧格式账,给可识别
            // 的 unsupported 状态(单子 §一:不伪装为空会话),不猜格式。
            result.error_code = "usage.session_format_unsupported";
            result.message = "不认的账格式:" + probe.detail;
            result.session_id = session_dir.filename().string();
            return result;
        case trajectory::v3::V3StreamProbe::Status::BadFirstLine:
        case trajectory::v3::V3StreamProbe::Status::EmptyFirstLine:
            result.error_code = "usage.session_format_unreadable";
            result.message = "主账首行读不出:" + probe.detail;
            result.session_id = session_dir.filename().string();
            return result;
        case trajectory::v3::V3StreamProbe::Status::NotSessionDir:
        case trajectory::v3::V3StreamProbe::Status::V2Layout:
        case trajectory::v3::V3StreamProbe::Status::StreamMissing:
        default:
            break;  // 走既有 v2 路径(含目录不存在 → session_not_found)
    }

    result.format = "v2";
    const auto streams = ListSessionStreams(session_dir);
    if (!streams.has_value()) {
        result.error_code = "usage.session_not_found";
        result.message = "没有这场 session:" + session_dir.string();
        result.session_id = session_dir.filename().string();
        return result;
    }
    // session.json:静态材料。读不到不拦账(旧目录/半路拷贝),status 写
    // unknown,封口与否调用方按 provisional 处理。
    if (const auto manifest = trajectory::ReadSessionJson(session_dir)) {
        result.session_id = manifest->session_id;
        result.workspace_key = manifest->workspace_key;
        result.status = manifest->status;
    } else {
        result.status = "unknown";
        result.warnings.push_back("usage.session_manifest_missing: " + session_dir.string());
    }

    for (const auto& path : *streams) {
        auto envelopes = ParseStream(path);
        if (!envelopes.has_value()) {
            result.warnings.push_back("usage.stream_unreadable: " + path.filename().string());
            continue;
        }
        UsageProjection projection = ProjectUsage(*envelopes);
        if (!projection.ok) {
            result.warnings.push_back("usage.stream_rejected: " + path.filename().string() + ": " +
                                      projection.error_code);
            continue;
        }
        for (const auto& warning : projection.warnings) {
            result.warnings.push_back(warning);
        }
        for (auto& sample : projection.samples) {
            if (result.workspace_key.empty()) {
                result.workspace_key = sample.workspace_key;
            }
            if (result.session_id.empty()) {
                result.session_id = sample.session_id;
            }
            result.samples.push_back(std::move(sample));
        }
    }
    // session.json 与 sample 都没给名字(全空 stream 的光杆目录):目录名兜底,
    // 报告至少有个可指认的场次名。
    if (result.session_id.empty()) {
        result.session_id = session_dir.filename().string();
    }
    result.ok = true;
    return result;
}

}  // namespace lubancode::accounting
