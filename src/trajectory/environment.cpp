#include "trajectory/environment.hpp"

#include <algorithm>
#include <atomic>
#include <sstream>

#include "platform/process.hpp"

namespace lubancode::trajectory {
namespace {

// git 子进程的超时上限:diff/status 在正常仓库里毫秒级,给 10s 足够宽松,
// 又不至于在异常仓库(巨型 monorepo、网络文件系统)上无限期挂起主流程。
constexpr int kGitTimeoutMs = 10000;

std::string RunGit(const std::string& repo, const std::vector<std::string>& args, bool* ok) {
    std::vector<std::string> argv{"git"};
    argv.insert(argv.end(), args.begin(), args.end());
    const platform::ProcessResult result =
        platform::RunProcess(argv, kGitTimeoutMs, /*cancel=*/nullptr, /*extra_env=*/{},
                             platform::kDefaultMaxOutputBytes, repo);
    *ok = !result.spawn_failed && !result.timed_out && result.exit_code == 0;
    return result.output;
}

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == '\n' || text[begin] == '\r' || text[begin] == ' ')) {
        ++begin;
    }
    return text.substr(begin);
}

}  // namespace

GitStatus GatherGitStatus(const std::string& repository_root) {
    GitStatus status;
    if (repository_root.empty()) {
        return status;
    }
    bool ok = false;
    const std::string head = RunGit(repository_root, {"rev-parse", "HEAD"}, &ok);
    if (!ok) {
        return status;  // 不在仓库内/无提交:in_repo 留 false,如实报缺口
    }
    status.in_repo = true;
    status.head_sha = Trim(head);

    const std::string branch = RunGit(repository_root, {"rev-parse", "--abbrev-ref", "HEAD"}, &ok);
    if (ok) {
        status.branch = Trim(branch);
    }

    const std::string porcelain =
        RunGit(repository_root, {"status", "--porcelain=v1", "--untracked-files=all"}, &ok);
    if (ok) {
        std::istringstream lines(porcelain);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) {
                continue;
            }
            status.dirty = true;
            // "?? path" 前缀是 untracked;其余(M/A/D/R 等)是已跟踪的改动,
            // 只影响 dirty flag,不进 untracked 清单。
            if (line.rfind("?? ", 0) == 0) {
                status.untracked_files.push_back(line.substr(3));
            }
        }
    }

    if (status.dirty) {
        const std::string diff = RunGit(repository_root, {"diff", "HEAD"}, &ok);
        if (ok) {
            status.dirty_patch = diff;
        }
    }
    return status;
}

const char* ReplayLevelName(ReplayLevel level) {
    switch (level) {
        case ReplayLevel::Exact: return "exact";
        case ReplayLevel::SourceExactEnvironmentPartial: return "source_exact_environment_partial";
        case ReplayLevel::InputOnly: return "input_only";
        case ReplayLevel::Blocked: return "blocked";
    }
    return "blocked";
}

ReplayLevelResult DetermineReplayLevel(const EnvironmentSnapshotInput& input) {
    ReplayLevelResult result;
    const bool source_pinned = input.git.in_repo && !input.git.head_sha.empty() &&
                               (!input.git.dirty || !input.git.dirty_patch.empty());
    const bool environment_pinned = !input.lubancode_version.empty() &&
                                    !input.toolset.toolset_sha256.empty() &&
                                    input.system_prompt_ref.has_value();

    if (!input.git.in_repo) {
        result.gaps.push_back("not_a_git_repository");
    } else if (input.git.head_sha.empty()) {
        result.gaps.push_back("git_head_unavailable");
    } else if (input.git.dirty && input.git.dirty_patch.empty()) {
        result.gaps.push_back("dirty_patch_unavailable");
    }
    if (input.lubancode_version.empty()) {
        result.gaps.push_back("lubancode_version_unavailable");
    }
    if (input.toolset.toolset_sha256.empty()) {
        result.gaps.push_back("toolset_hash_unavailable");
    }
    if (!input.system_prompt_ref.has_value()) {
        result.gaps.push_back("system_prompt_ref_unavailable");
    }

    if (source_pinned && environment_pinned) {
        result.level = ReplayLevel::Exact;
    } else if (source_pinned) {
        result.level = ReplayLevel::SourceExactEnvironmentPartial;
    } else if (!input.cwd.empty()) {
        result.level = ReplayLevel::InputOnly;
    } else {
        result.level = ReplayLevel::Blocked;
        result.gaps.push_back("cwd_unavailable");
    }
    return result;
}

std::expected<EnvironmentCapturePayload, std::string> BuildEnvironmentCapturePayload(
    const EnvironmentSnapshotInput& input, BlobStore& blobs, Durability durability) {
    EnvironmentCapturePayload out;
    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["lubancode_version"] = input.lubancode_version;
    snapshot["build_commit"] = input.build_commit;
    snapshot["build_type"] = input.build_type;
    snapshot["os"] = input.os_name;
    snapshot["arch"] = input.arch;
    snapshot["locale"] = input.locale;
    snapshot["timezone"] = input.timezone;
    snapshot["cwd"] = input.cwd;
    snapshot["repository_root"] = input.repository_root;

    nlohmann::json git = nlohmann::json::object();
    git["in_repo"] = input.git.in_repo;
    git["head_sha"] = input.git.head_sha;
    git["branch"] = input.git.branch;
    git["dirty"] = input.git.dirty;
    if (!input.git.dirty_patch.empty()) {
        auto ref = blobs.Store(input.git.dirty_patch, "text/x-diff", durability);
        if (!ref.has_value()) {
            return std::unexpected("trajectory.environment.dirty_patch_store_failed: " + ref.error());
        }
        git["dirty_patch_ref"] = ref->ToJson();
    }
    if (!input.git.untracked_files.empty()) {
        const nlohmann::json manifest = input.git.untracked_files;
        auto ref = blobs.Store(manifest.dump(), "application/json", durability);
        if (!ref.has_value()) {
            return std::unexpected("trajectory.environment.untracked_manifest_store_failed: " + ref.error());
        }
        git["untracked_manifest_ref"] = ref->ToJson();
    }
    snapshot["git"] = std::move(git);

    snapshot["provider"] = input.provider;
    snapshot["wire"] = input.wire;
    snapshot["model"] = input.model;
    snapshot["model_parameters"] = input.model_parameters;

    if (input.system_prompt_ref.has_value()) {
        snapshot["system_prompt_ref"] = input.system_prompt_ref->ToJson();
    }
    snapshot["toolset"] = nlohmann::json{{"toolset_sha256", input.toolset.toolset_sha256},
                                         {"tool_count", input.toolset.tool_count}};
    snapshot["project_instruction_refs"] = input.project_instruction_refs;
    snapshot["loaded_skill_refs"] = input.loaded_skill_refs;
    snapshot["plugin_refs"] = input.plugin_refs;
    snapshot["config_snapshot_redacted"] = input.config_snapshot_redacted;

    nlohmann::json env = nlohmann::json::object();
    for (const auto& [key, value] : input.allowlisted_env) {
        env[key] = value;
    }
    snapshot["env_allowlist"] = std::move(env);

    const ReplayLevelResult replay_level = DetermineReplayLevel(input);
    snapshot["replay_level"] = ReplayLevelName(replay_level.level);
    snapshot["gaps"] = replay_level.gaps;

    out.full_snapshot = snapshot;

    auto snapshot_ref = blobs.Store(snapshot.dump(), "application/json", durability);
    if (!snapshot_ref.has_value()) {
        return std::unexpected("trajectory.environment.snapshot_store_failed: " + snapshot_ref.error());
    }
    nlohmann::json payload = nlohmann::json::object();
    payload["snapshot_ref"] = snapshot_ref->ToJson();
    payload["replay_level"] = ReplayLevelName(replay_level.level);
    payload["gaps"] = replay_level.gaps;
    out.event_payload = std::move(payload);
    return out;
}

}  // namespace lubancode::trajectory
