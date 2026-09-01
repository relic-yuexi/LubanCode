// migrate_storage_command.hpp 的实现。迁移引擎在 workspace::migrator
//(独立 target lubancode_storage_migrator);这里只做参数拼装与终端报告。
#include "cli/migrate_storage_command.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>

#include "config/config.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"
#include "workspace/storage_contracts.hpp"
#include "workspace/storage_migrator.hpp"

namespace lubancode::cli {

namespace {

std::filesystem::path DefaultHomeLubancode() {
    const auto home = config::HomeLubancodeDir();
    if (!home.has_value()) {
        return {};
    }
    return tools::Utf8ToPath(*home);
}

workspace::migrator::MigratorOptions MakeOptions(const MigrateStorageCommandArgs& args) {
    workspace::migrator::MigratorOptions options;
    options.home_lubancode = DefaultHomeLubancode();
    options.lubancode_version = "lubancode-migrate-storage";
    options.delete_source = args.delete_source;
    options.confirm_delete = args.confirm_delete;
    for (const std::string& root : args.project_roots) {
        const std::string key =
            workspace::migrator::ComputeLegacyProjectKey(platform::Utf8ToPath(root));
        options.extra_project_roots.emplace(key, platform::PathToUtf8(
                                                      platform::Utf8ToPath(root)));
    }
    return options;
}

int ReportStatus(const workspace::migrator::MigratorOptions& options) {
    const auto status = workspace::migrator::QueryStorageMigrationStatus(options);
    std::cout << "storage-v2 迁移账面 —— operations " << status.operations.size() << " 只("
              << status.committed_operations << " 只已 committed)\n";
    for (const auto& operation : status.operations) {
        std::cout << "  " << operation.operation_id << "  " << operation.phase;
        if (operation.phase != "planned" && operation.phase != "unknown") {
            std::cout << "  " << operation.done << "/" << operation.total;
            if (!operation.last_outcome.empty()) {
                std::cout << "  末件 " << operation.last_outcome;
            }
        }
        std::cout << "\n";
    }
    std::cout << "未迁旧会话档 " << status.pending_session_files << " 份;"
              << " 未迁旧项目 Memory " << status.pending_memory_projects << " 处\n";
    for (const std::string& key : status.unmappable_projects) {
        std::cout << "  [!!] 旧项目 " << key
                  << " 算不出目标 workspace(跑 migrate-storage run --project-root <项目根>)\n";
    }
    if (status.pending_session_files == 0 && status.pending_memory_projects == 0 &&
        status.unmappable_projects.empty()) {
        std::cout << "全机旧数据已迁完(或没有旧数据)。\n";
        return 0;
    }
    return 2;
}

}  // namespace

int RunMigrateStorageCommand(const MigrateStorageCommandArgs& args) {
    const workspace::migrator::MigratorOptions options = MakeOptions(args);
    if (options.home_lubancode.empty()) {
        std::cerr << "migrate-storage: 找不到用户主目录,旧档无处寻\n";
        return 1;
    }
    if (args.verb == "status") {
        return ReportStatus(options);
    }
    if (args.verb == "plan") {
        const auto plan = workspace::migrator::PlanStorageMigration(options);
        if (!plan.has_value()) {
            std::cerr << "plan 未过: " << plan.error() << "\n";
            return 2;
        }
        std::cout << "operation " << plan->operation_id << " —— 旧会话档 "
                  << plan->sessions.size() << " 份, 旧项目库 " << plan->memory_projects.size()
                  << " 处; 此前已迁 " << plan->imported_before << " 份\n";
        for (const auto& session : plan->sessions) {
            std::cout << "  " << (session.already_imported ? "[已迁] " : "[待迁] ")
                      << session.source.path << "  ->  " << session.workspace_key
                      << (session.archived ? "  (archive 源,迁入后标 archived)" : "") << "\n";
        }
        for (const auto& project : plan->memory_projects) {
            std::cout << "  memory " << project.old_project_key << "  ->  "
                      << (project.workspace_key.empty() ? "(算不出目标)" : project.workspace_key)
                      << (project.workspace_key.empty() ? "  缺 project.json,用 --project-root 指认"
                                                        : "")
                      << "\n";
        }
        for (const std::string& error : plan->errors) {
            std::cout << "  [!!] " << error << "\n";
        }
        std::cout << "intent.json 已落 migrations/storage-v2/" << plan->operation_id
                  << "/; 跑 migrate-storage run 执行。\n";
        return 0;
    }
    if (args.verb != "run") {
        std::cerr << "用法: lubancode migrate-storage <plan|run|status> [--operation <id>] "
                     "[--project-root <路径>] [--delete-source --yes]\n";
        return 1;
    }

    const auto run = workspace::migrator::RunStorageMigration(options, args.operation_id);
    if (!run.has_value()) {
        std::cerr << "run 未过: " << run.error() << "\n";
        return run.error().find(std::string(workspace::contracts::kErrMigrationDeleteUnverified)) !=
                       std::string::npos
                   ? 3
                   : 2;
    }

    const auto& report = *run;
    std::cout << "operation " << report.operation_id
              << (report.resumed_operation.empty() ? "" : "(续跑) ") << " —— imported "
              << (report.counts.count("imported") ? report.counts.at("imported") : 0)
              << ", already " << (report.counts.count("already_imported")
                                      ? report.counts.at("already_imported")
                                      : 0)
              << ", skipped "
              << (report.counts.count("skipped_unreadable") ? report.counts.at("skipped_unreadable")
                                                            : 0)
              << ", failed " << (report.counts.count("failed") ? report.counts.at("failed") : 0)
              << "\n";
    for (const auto& item : report.items) {
        std::cout << "  [" << item.outcome << "] " << item.source_path;
        if (!item.target_session_id.empty()) {
            std::cout << "  ->  " << item.target_workspace_key << "/" << item.target_session_id;
        }
        if (item.legacy_partial) {
            std::cout << "  (legacy_partial, 缺口 " << item.missing.size() << " 条)";
        }
        std::cout << "\n";
        if (item.outcome == "failed") {
            std::cout << "        " << item.error_code << "\n";
        }
    }
    for (const auto& project : report.memory_projects) {
        std::cout << "  memory " << project.old_project_key << "  ->  "
                  << (project.workspace_key.empty() ? "(算不出目标)" : project.workspace_key) << "  ["
                  << project.outcome << "]\n";
    }
    if (report.source_deleted) {
        std::cout << "已删源档 " << report.deleted_sources.size() << " 份(均复验过):\n";
        for (const std::string& path : report.deleted_sources) {
            std::cout << "  - " << path << "\n";
        }
    }
    if (!report.error_code.empty()) {
        // 故障/中断:账面如实,退出码告知可续跑。
        std::cout << "中断于 " << report.error_text << "(" << report.error_code
                  << "); 重跑 migrate-storage run 续办。\n";
        return 2;
    }
    return 0;
}

}  // namespace lubancode::cli
