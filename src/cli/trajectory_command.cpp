// `lubancode trajectory` 子命令实现(P0-3)。合同见 trajectory_command.hpp。
#include "cli/trajectory_command.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "cli/frame_notice.hpp"  // 批 7:usage/gc/verify/replay/export 走 frame
#include "config/config.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/harness.hpp"
#include "trajectory/harness_exporter.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/safety.hpp"
#include "trajectory/training_exporter.hpp"
#include "trajectory/usage_gc.hpp"
#include "workspace/index.hpp"  // 账本制:key 反查房门

namespace lubancode::cli {

namespace {

// P0-2:唯一项目持久化根(原 ~/.lubancode/trajectories 迁
// ~/.lubancode/workspaces;旧根零读零写,不设暗门)。P1(应用Worker接入单
// §4.2)起落状态根:应用根语义=数据根,个人模式与从前同一处。
std::filesystem::path DefaultWorkspacesRoot() {
    const auto state_root = config::StateRootDir();
    if (!state_root.has_value()) {
        return {};
    }
    return tools::Utf8ToPath(*state_root) / "workspaces";
}

// 在 <key>/sessions/ 下找 <session_id> 的目录。找不到给空。
std::filesystem::path FindSessionDir(const std::filesystem::path& root, const std::string& session_id) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return {};
    }
    for (const auto& workspace : std::filesystem::directory_iterator(root, ec)) {
        const auto candidate = workspace.path() / "sessions" / platform::Utf8ToPath(session_id);
        if (std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

// §12.2 容量/CI 档:usage / gc / doctor 吃 workspace-key(单段名,先过
// 安全校验)。<key> 找不到给空。账本制:目录名是门牌不是 key,按各房
// workspace.json 反查。
std::filesystem::path FindWorkspaceDir(const std::filesystem::path& root, const std::string& key) {
    if (!trajectory::IsSafeSingleSegment(key)) {
        return {};
    }
    return workspace::index::ResolveDirByWorkspaceKey(root, key).value_or(std::filesystem::path());
}

// usage:workspace 四笔容量账逐 session 报(只报账,不动文件系统)。
// 批 7:头句进键值对框(标题=命令名 usage),逐 session 走表格(字节/文件
// 数列右对齐);journal=/blobs= 等前缀升为列头,值与文案一字不丢。
int RunUsageReport(const std::filesystem::path& workspace_dir, const std::string& key) {
    const auto report = trajectory::ScanWorkspaceUsage(workspace_dir / "sessions", key);
    const Theme theme = CliTheme();
    EmitFrameLines(frame::RenderKeyValues(
        "usage",
        {SentenceField("workspace " + key + " —— " + std::to_string(report.sessions.size()) +
                       " 场 session, 共 " + std::to_string(report.total_bytes / (1024 * 1024)) +
                       " MiB")},
        theme, frame::Light(), CliFrameWidth()));
    if (!report.sessions.empty()) {
        std::vector<frame::TableColumn> columns;
        columns.push_back({"session"});
        columns.push_back({"journal", 0, /*align_right=*/true});
        columns.push_back({"blobs", 0, /*align_right=*/true});
        columns.push_back({"rebuildable", 0, /*align_right=*/true});
        columns.push_back({"derived", 0, /*align_right=*/true});
        columns.push_back({"files", 0, /*align_right=*/true});
        std::vector<frame::TableRow> rows;
        for (const auto& session : report.sessions) {
            rows.push_back(frame::TableRow{{session.session_id,
                                            std::to_string(session.journal_bytes) + "B",
                                            std::to_string(session.referenced_blob_bytes) + "B",
                                            std::to_string(session.rebuildable_bytes) + "B",
                                            std::to_string(session.derived_bytes) + "B",
                                            std::to_string(session.total_files()) + " 个"}});
        }
        EmitFrameLines(frame::RenderTable("workspace " + key, columns, rows, theme, frame::Light(),
                                          CliFrameWidth()));
    }
    PrintNotice(theme, {"canonical JSONL 与 artifacts/ 引用 blob 不在自动清理面;"
                        "删它们只走显式的 session delete。"});
    TermOut().flush();
    return 0;
}

// gc:§12.2 定死次序 temp→index→checkpoint→derived;--derived-only 才真删,
// 默认 dry-run 只报账。批 7:逐 session 走表格(失败行 Fail 色),错误明细
// 带着既有 [!!] 记号进列表;"-" 是空列占位符(schema 符号,不是文案)。
int RunGc(const std::filesystem::path& workspace_dir, const std::string& key, bool derived_only) {
    const auto sessions_dir = workspace_dir / "sessions";
    std::error_code ec;
    if (!std::filesystem::is_directory(sessions_dir, ec)) {
        std::cerr << "workspace " << key << " 没有 sessions 目录\n";
        return 1;
    }
    const Theme theme = CliTheme();
    int exit_code = 0;
    std::vector<frame::TableRow> rows;
    std::vector<std::string> error_lines;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_dir, ec)) {
        if (ec) break;
        std::error_code dir_ec;
        if (!entry.is_directory(dir_ec) || dir_ec) {
            continue;
        }
        const auto result = trajectory::RunSessionGc(
            entry.path(), derived_only ? trajectory::GcScope::DerivedOnly
                                       : trajectory::GcScope::DryRun);
        std::string deleted = "-";
        std::string failed = "-";
        frame::CellTone tone = frame::CellTone::Normal;
        if (result.applied) {
            deleted = std::to_string(result.deleted_bytes / 1024) + " KiB / " +
                      std::to_string(result.deleted_files) + " 个文件";
            if (!result.errors.empty()) {
                failed = std::to_string(result.errors.size()) + " 项";
                tone = frame::CellTone::Fail;
                exit_code = 2;
            }
        }
        rows.push_back(frame::TableRow{
            {entry.path().filename().string(),
             std::to_string(result.plan.reclaimable_bytes / 1024) + " KiB(" +
                 std::to_string(result.plan.items.size()) + " 项)",
             deleted, failed},
            {tone}});
        for (const auto& error : result.errors) {
            error_lines.push_back("[!!] " + error);
        }
    }
    if (!rows.empty()) {
        std::vector<frame::TableColumn> columns;
        columns.push_back({"session"});
        columns.push_back({"reclaim", 0, /*align_right=*/true});
        columns.push_back({"deleted", 0, /*align_right=*/true});
        columns.push_back({"failed", 0, /*align_right=*/true});
        EmitFrameLines(frame::RenderTable("gc", columns, rows, theme, frame::Light(),
                                          CliFrameWidth()));
    }
    if (!error_lines.empty()) {
        std::vector<frame::ListRow> error_rows;
        for (const std::string& error : error_lines) {
            error_rows.push_back(frame::ListRow{error, {}, {}, frame::Bullet::None});
        }
        EmitFrameLines(
            frame::RenderList({}, error_rows, theme, frame::Light(), CliFrameWidth()));
    }
    if (!derived_only) {
        PrintNotice(theme, {"dry-run 只报账;真清加 --derived-only。"});
    }
    TermOut().flush();
    return exit_code;
}

// doctor:与 /doctor trajectory 同一份只读聚合(metrics.hpp 的共用格式)。
// 批 7裁量:FormatWorkspaceDoctorReport 的行形状归领域层定(有册钉着,
// /doctor trajectory 同源共用),本批不套框——只收口输出端口(std::cout
// -> TermOut,同一 stdout 目的地,落盘字节不变)。
int RunDoctor(const std::filesystem::path& workspace_dir, const std::string& key) {
    const auto report = trajectory::BuildWorkspaceDoctorReport(
        workspace_dir.parent_path(), workspace_dir, key, std::nullopt, {});
    for (const std::string& line : trajectory::FormatWorkspaceDoctorReport(report)) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
    return 0;
}

// export --format harness-v1(One-shot 轨迹指定输出单 §七补导路):把
// session 账投影成便携 JSONL,落 --output 点名的路径(缺省 session 目录
// 下的 exports/harness-v1/)。退出码照 trajectory 命令族:0/1/2。
int RunHarnessExport(const std::filesystem::path& root, const TrajectoryCommandArgs& args) {
    if (args.session_id.empty()) {
        std::cerr << "缺 session id: lubancode trajectory export <session-id>"
                     " --format harness-v1 [--output <文件路径>]\n";
        return 1;
    }
    const auto session_dir = FindSessionDir(root, args.session_id);
    if (session_dir.empty()) {
        std::cerr << "找不到 session " << args.session_id << "(在 " << platform::PathToUtf8(root)
                  << " 的 workspaces/*/sessions/ 下没有)\n";
        return 1;
    }
    const auto target = args.output_path.empty()
                            ? session_dir / "exports" / "harness-v1" / "trajectory.jsonl"
                            : tools::Utf8ToPath(args.output_path);
    // 补导路不知道原进程退出码,给 null——诚实,不拿 0 冒充。
    const trajectory::HarnessExportOptions options;
    const auto report =
        trajectory::ExportSessionHarnessV1(session_dir, target, options, std::nullopt);
    if (!report.ok()) {
        std::cerr << "harness-v1 导出未过(" << report.error_code << "): " << report.message
                  << "\n";
        return report.error_code == "export.no_session_dir" ||
                       report.error_code == "export.no_streams"
                   ? 1
                   : 2;
    }
    // 收据行(与 one-shot --output 的 stderr 收据同一形状,机器可解析)。
    std::cout << "[harness-export] schema=" << report.schema << " schema_version="
              << report.schema_version << " session_id=" << report.session_id
              << " records=" << report.records << " sha256=" << report.sha256
              << " path=" << platform::PathToUtf8(report.target) << "\n";
    return 0;
}

// export/export-workspace(P0-5 §十一/§十四):Journal 只读投影成 training-v1
// 数据集。trajectory 关的会话没账可导——报空退 1,不造假。
int RunExport(const std::filesystem::path& root, const TrajectoryCommandArgs& args) {
    if (args.format == "harness-v1") {
        // export --format harness-v1 走专门的便携 JSONL 投影(补导路)。
        return RunHarnessExport(root, args);
    }
    if (args.format != "training-v1") {
        std::cerr << "只认 --format training-v1 或 harness-v1,不认 \"" << args.format << "\"\n";
        return 1;
    }
    const trajectory::TrainingExportOptions options;  // 默认档:reasoning 剔除、
    // blob 回读 1 MiB 帽、写盘 16 MiB 磁盘门(§11.6/§12.2)。
    const auto report = args.verb == "export"
                            ? trajectory::ExportSessionTrainingV1(
                                  FindSessionDir(root, args.session_id), options)
                            : trajectory::ExportWorkspaceTrainingV1(
                                  FindWorkspaceDir(root, args.session_id), options);
    if (!report.ok()) {
        std::cerr << "training-v1 导出未过(" << report.error_code << "): " << report.message
                  << "\n";
        return report.error_code == "export.no_session_dir" ||
                       report.error_code == "export.no_streams"
                   ? 1
                   : 2;
    }
    // 批 7:头句拆标题+两键,route 计数逐键;排除缘由(尾冒号剥掉)做列
    // 表标题;落盘句整句进键值对。信息一字不丢,只是不再平铺。
    const Theme theme = CliTheme();
    std::vector<frame::Field> fields;
    fields.push_back({"episode", std::to_string(report.episodes) + " 枚"});
    fields.push_back({"stream", std::to_string(report.streams) + " 份"});
    for (const char* route : {"success", "failure", "partial", "excluded"}) {
        const auto it = report.counts.find(route);
        fields.push_back({std::string(route) + ".jsonl",
                          std::to_string(it != report.counts.end() ? it->second : 0) + " 枚"});
    }
    EmitFrameLines(frame::RenderKeyValues("training-v1 导出完成", fields, theme, frame::Light(),
                                          CliFrameWidth()));
    if (!report.exclusion_reasons.empty()) {
        std::vector<frame::ListRow> rows;
        for (const auto& [reason, count] : report.exclusion_reasons) {
            rows.push_back(frame::ListRow{reason, "×" + std::to_string(count), {},
                                          frame::Bullet::None});
        }
        EmitFrameLines(
            frame::RenderList("排除缘由", rows, theme, frame::Light(), CliFrameWidth()));
    }
    PrintNotice(theme, {"落盘 " + platform::PathToUtf8(report.export_dir) +
                        (args.verb == "export-workspace"
                             ? std::string("(逐 session 各自落,此为末场;manifest.json 记 "
                                           "config hash、过滤规则与逐文件 sha256)")
                             : std::string("(manifest.json 记 config hash、过滤规则与逐文件 "
                                           "sha256)")) +
                        "(派生物,可删可重算)"});
    TermOut().flush();
    return 0;
}

int RunTrajectoryCommand(const TrajectoryCommandArgs& args) {
    if (args.verb == "export" || args.verb == "export-workspace") {
        if (args.session_id.empty()) {
            std::cerr << "缺 id: lubancode trajectory " << args.verb
                      << (args.verb == "export" ? " <session-id>" : " <workspace-key>")
                      << " --format training-v1\n";
            return 1;
        }
        const auto root = args.trajectories_root.empty() ? DefaultWorkspacesRoot()
                                                         : tools::Utf8ToPath(args.trajectories_root);
        if (root.empty()) {
            std::cerr << "找不到主目录,轨迹账无处寻\n";
            return 1;
        }
        return RunExport(root, args);
    }
    if (args.verb == "usage" || args.verb == "gc" || args.verb == "doctor") {
        if (args.session_id.empty()) {
            std::cerr << "缺 workspace key: lubancode trajectory " << args.verb
                      << " <workspace-key>\n";
            return 1;
        }
        const auto root = args.trajectories_root.empty() ? DefaultWorkspacesRoot()
                                                         : tools::Utf8ToPath(args.trajectories_root);
        if (root.empty()) {
            std::cerr << "找不到主目录,轨迹账无处寻\n";
            return 1;
        }
        const auto workspace_dir = FindWorkspaceDir(root, args.session_id);
        if (workspace_dir.empty()) {
            std::cerr << "找不到 workspace " << args.session_id << "(单段名,不带路径)\n";
            return 1;
        }
        if (args.verb == "usage") {
            return RunUsageReport(workspace_dir, args.session_id);
        }
        if (args.verb == "gc") {
            return RunGc(workspace_dir, args.session_id, args.gc_derived_only);
        }
        return RunDoctor(workspace_dir, args.session_id);
    }
    if (args.verb != "verify" && args.verb != "replay" && args.verb != "harness-replay") {
        std::cerr << "用法: lubancode trajectory "
                     "<verify|replay|harness-replay|usage|gc|doctor|export|export-workspace> "
                     "<session-id|workspace-key>\n";
        return 1;
    }
    if (args.session_id.empty()) {
        std::cerr << "缺 session id: lubancode trajectory " << args.verb << " <session-id>\n";
        return 1;
    }
    const auto root = args.trajectories_root.empty() ? DefaultWorkspacesRoot()
                                                     : tools::Utf8ToPath(args.trajectories_root);
    if (root.empty()) {
        std::cerr << "找不到主目录,轨迹账无处寻\n";
        return 1;
    }
    const auto session_dir = FindSessionDir(root, args.session_id);
    if (session_dir.empty()) {
        std::cerr << "找不到 session " << args.session_id << "(在 " << platform::PathToUtf8(root)
                  << " 的 workspaces/*/sessions/ 下没有)\n";
        return 1;
    }

    if (args.verb == "verify") {
        const auto report = trajectory::VerifySessionDir(session_dir);
        // 批 7:头句进键值对框(标题=命令名 verify,结论句 accent 按过/未过
        // 上语义色);streams 与 child_edges 各一表,[ok]/[!!] 既有记号保留
        // 在 state 列,Pass/Fail 色。
        const Theme theme = CliTheme();
        EmitFrameLines(frame::RenderKeyValues(
            "verify",
            {frame::Field{"session " + args.session_id,
                          report.ok ? "verify 通过" : "verify 未过(" + report.error_code + ")",
                          report.ok ? frame::FieldAccent::Pass : frame::FieldAccent::Error}},
            theme, frame::Light(), CliFrameWidth()));
        if (!report.streams.empty()) {
            std::vector<frame::TableColumn> columns;
            columns.push_back({"state"});
            columns.push_back({"path"});
            columns.push_back({"events", 0, /*align_right=*/true});
            columns.push_back({"terminal"});
            columns.push_back({"error"});
            std::vector<frame::TableRow> rows;
            for (const auto& stream : report.streams) {
                rows.push_back(frame::TableRow{
                    {stream.ok ? "[ok]" : "[!!]", stream.relative_path,
                     std::to_string(stream.events) + " 事件",
                     stream.run_terminal ? "终态 " + stream.terminal_kind : std::string("未收口"),
                     stream.error_code},
                    {stream.ok ? frame::CellTone::Pass : frame::CellTone::Fail}});
            }
            EmitFrameLines(
                frame::RenderTable({}, columns, rows, theme, frame::Light(), CliFrameWidth()));
        }
        if (!report.child_edges.empty()) {
            std::vector<frame::TableColumn> columns;
            columns.push_back({"state"});
            columns.push_back({"edge"});
            columns.push_back({"child hash"});
            columns.push_back({"error"});
            std::vector<frame::TableRow> rows;
            for (const auto& edge : report.child_edges) {
                rows.push_back(frame::TableRow{
                    {edge.error_code.empty() ? "[ok]" : "[!!]",
                     "edge " + edge.parent_run_id + " -> " + edge.child_run_id +
                         (edge.background_spawn ? "(后台)" : std::string()),
                     "子终态 hash " + edge.child_terminal_hash.substr(0, 12), edge.error_code},
                    {edge.error_code.empty() ? frame::CellTone::Pass : frame::CellTone::Fail}});
            }
            EmitFrameLines(
                frame::RenderTable({}, columns, rows, theme, frame::Light(), CliFrameWidth()));
        }
        TermOut().flush();
        return report.ok ? 0 : 2;
    }

    const auto stream = session_dir / "main.jsonl";
    if (!std::filesystem::exists(stream)) {
        std::cerr << "session 目录里没有 main.jsonl\n";
        return 1;
    }
    if (args.verb == "replay") {
        const auto fold = trajectory::FoldStreamReplay(stream);
        if (!fold.ok()) {
            std::cerr << "exact replay 未过(" << fold.error_code << "): " << fold.message << "\n";
            return 2;
        }
        // 批 7:折叠账逐对拆进键值对框(头句的 "exact replay" 升为标题,
        // 其余标签/数值原样两列对齐),信息一字不丢。
        const auto& state = fold.state;
        const Theme theme = CliTheme();
        std::vector<frame::Field> fields;
        fields.push_back({"state hash",
                          trajectory::ComputeReplayStateHash(state).substr(0, 16) + "…"});
        fields.push_back({"折叠", std::to_string(state.integrity.events_folded) + " 事件"});
        fields.push_back({"turn", std::to_string(state.turns.size())});
        fields.push_back({"请求步", std::to_string(state.requests.size())});
        fields.push_back({"工具", std::to_string(state.tools.size())});
        fields.push_back({"证据", std::to_string(state.evidence.size())});
        fields.push_back(
            {"对话投影", std::to_string(state.effective_conversation.size()) + " 条"});
        fields.push_back({"悬空工具",
                          std::to_string(state.integrity.dangling_tools) +
                              (state.integrity.unknown_side_effects ? "(含未知副作用)" : "")});
        fields.push_back(
            {"run", state.run_terminal_state.empty() ? std::string("未收口")
                                                     : state.run_terminal_state});
        fields.push_back({"start_reason", state.start_reason});
        EmitFrameLines(
            frame::RenderKeyValues("exact replay", fields, theme, frame::Light(), CliFrameWidth()));
        TermOut().flush();
        return 0;
    }

    // harness-replay:录制桩重放,divergence 立即报。
    const auto report = trajectory::RunHarnessReplay(stream);
    if (!report.ok) {
        std::cerr << "harness replay 未过(" << report.error_code << ")";
        if (report.divergence.has_value()) {
            std::cerr << "  divergence@" << report.divergence->stage << "/"
                      << report.divergence->step_id << ": " << report.divergence->reason
                      << "  期望 " << report.divergence->expected.substr(0, 16)
                      << "  实得 " << report.divergence->actual.substr(0, 16);
        }
        std::cerr << "\n";
        return 2;
    }
    // 批 7:三对账进键值对框(标题=头句里的 "harness replay")。
    {
        const Theme theme = CliTheme();
        EmitFrameLines(frame::RenderKeyValues(
            "harness replay",
            {{"模型步", std::to_string(report.model_steps_consumed)},
             {"工具步", std::to_string(report.tool_steps_consumed)},
             {"state hash", report.replay_state_hash.substr(0, 16) + "…"}},
            theme, frame::Light(), CliFrameWidth()));
        TermOut().flush();
    }
    return 0;
}

}  // namespace lubancode::cli
