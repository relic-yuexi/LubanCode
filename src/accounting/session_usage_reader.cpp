#include "accounting/session_usage_reader.hpp"

#include <algorithm>
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
    result.ok = true;
    return result;
}

}  // namespace

std::optional<std::vector<std::filesystem::path>> ListSessionStreams(
    const std::filesystem::path& session_dir) {
    if (!DirExists(session_dir)) {
        return std::nullopt;
    }
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
        case trajectory::v3::V3StreamProbe::Status::FormatConflict:
            result.error_code = "usage.session_format_conflict";
            result.message =
                "两种主账打架,不敢认:" + probe.detail + "(main.jsonl 与 <id>.jsonl 并存)";
            result.session_id = session_dir.filename().string();
            return result;
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
