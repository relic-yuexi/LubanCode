// legacy-storage-migrator:存储 v2 一次性迁移器的独立可执行封存体
//(单子 §7.4/合同 §十:迁移器隔离在 tools/legacy-storage-migrator/,生产
// 二进制外)。命令面与 `lubancode migrate-storage` 同源——引擎都是
// workspace::migrator,这里是给 CI/脚本/收官发行(主程序摘除迁移入口后)
// 用的独立入口。用法:
//
//   legacy-storage-migrator <plan|run|status>
//       [--home <dir>]            默认 ~/.lubancode
//       [--operation <id>]        run 续跑指认
//       [--project-root <path>]   旧项目库缺 project.json 的显式映射(可多枚)
//       [--delete-source --yes]   删源(二次确认;只删已 committed 且复验过的)
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "workspace/storage_migrator.hpp"

namespace {

std::filesystem::path DefaultHome() {
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
#else
    const char* profile = std::getenv("HOME");
#endif
    if (profile == nullptr || *profile == '\0') {
        return {};
    }
    return lubancode::platform::Utf8ToPath(std::string(profile)) / ".lubancode";
}

int Run(const std::string& verb, const lubancode::workspace::migrator::MigratorOptions& options,
        const std::string& operation_id) {
    if (verb == "status") {
        const auto status = lubancode::workspace::migrator::QueryStorageMigrationStatus(options);
        std::cout << "operations " << status.operations.size() << " (committed "
                  << status.committed_operations << "); pending sessions "
                  << status.pending_session_files << ", pending memory "
                  << status.pending_memory_projects << "\n";
        for (const std::string& key : status.unmappable_projects) {
            std::cout << "  unmappable: " << key << "\n";
        }
        return 0;
    }
    if (verb == "plan") {
        const auto plan = lubancode::workspace::migrator::PlanStorageMigration(options);
        if (!plan.has_value()) {
            std::cerr << "plan failed: " << plan.error() << "\n";
            return 2;
        }
        std::cout << "operation " << plan->operation_id << ": sessions " << plan->sessions.size()
                  << ", memory projects " << plan->memory_projects.size() << ", errors "
                  << plan->errors.size() << "\n";
        return 0;
    }
    const auto run =
        lubancode::workspace::migrator::RunStorageMigration(options, operation_id);
    if (!run.has_value()) {
        std::cerr << "run failed: " << run.error() << "\n";
        return 2;
    }
    std::cout << "operation " << run->operation_id << ": imported "
              << (run->counts.count("imported") ? run->counts.at("imported") : 0) << ", failed "
              << (run->counts.count("failed") ? run->counts.at("failed") : 0)
              << ", memory projects " << run->memory_projects.size()
              << ", source_deleted=" << (run->source_deleted ? "true" : "false") << "\n";
    return run->error_code.empty() ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: legacy-storage-migrator <plan|run|status> [--home <dir>] "
                     "[--operation <id>] [--project-root <path>] [--delete-source --yes]\n");
        return 1;
    }
    const std::string verb = argv[1];
    if (verb != "plan" && verb != "run" && verb != "status") {
        std::fprintf(stderr, "unknown verb: %s\n", argv[1]);
        return 1;
    }
    lubancode::workspace::migrator::MigratorOptions options;
    options.lubancode_version = "legacy-storage-migrator";
    std::string operation_id;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--home" && i + 1 < argc) {
            options.home_lubancode = lubancode::platform::Utf8ToPath(argv[++i]);
        } else if (arg == "--operation" && i + 1 < argc) {
            operation_id = argv[++i];
        } else if (arg == "--project-root" && i + 1 < argc) {
            const std::string root = argv[++i];
            const std::string key =
                lubancode::workspace::migrator::ComputeLegacyProjectKey(
                    lubancode::platform::Utf8ToPath(root));
            options.extra_project_roots.emplace(
                key, lubancode::platform::PathToUtf8(lubancode::platform::Utf8ToPath(root)));
        } else if (arg == "--delete-source") {
            options.delete_source = true;
        } else if (arg == "--yes") {
            options.confirm_delete = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 1;
        }
    }
    if (options.home_lubancode.empty()) {
        options.home_lubancode = DefaultHome();
    }
    if (options.home_lubancode.empty()) {
        std::fprintf(stderr, "cannot locate home directory; pass --home <dir>\n");
        return 1;
    }
    return Run(verb, options, operation_id);
}
