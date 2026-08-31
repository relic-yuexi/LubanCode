// 环境快照与重现等级(P0 新轨迹记录单 §9.1/§9.2,P0-4 环境与证据)。
//
// 两段拆开:
//   GatherGitStatus     真 IO(shell 出 git),给固定 repository_root。
//   BuildEnvironmentCapturePayload  纯拼装 + blob 落盘,给已收集好的
//                                   EnvironmentSnapshotInput——单测只钉
//                                   这段的输出形状,不必真跑 git。
//   DetermineReplayLevel 纯函数,§9.2 四档由输入齐全度推导,不猜。
//
// run.environment.captured 的事件 payload 只要三键(snapshot_ref/
// replay_level/gaps,schema.cpp 钉死);完整快照落一份 blob,大小与内容
// 不受信封内联上限约束。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/blob_store.hpp"
#include "trajectory/event.hpp"

namespace lubancode::trajectory {

// git 状态(§9.1"git HEAD / branch / dirty flag / dirty patch blob ref /
// untracked file manifest ref")。in_repo=false 时其余字段留空,不是错误
// ——启动 cwd 本就可能不在仓库里。
struct GitStatus {
    bool in_repo = false;
    std::string head_sha;
    std::string branch;
    bool dirty = false;
    std::string dirty_patch;                  // git diff HEAD 全量原文(未截断)
    std::vector<std::string> untracked_files;  // git status 的 untracked 路径清单
};

// 真 shell 出 git(平台 IO)。repository_root 为空或不是 git 仓库给
// in_repo=false,不抛错——调用方据此在 gaps 里如实记账,不拿假数据充数。
GitStatus GatherGitStatus(const std::string& repository_root);

// 工具定义集合的规范摘要(§9.1"工具定义集合引用")。调用方按规范 JSON
// (工具名 + input_schema,按名排序拼接)算好 sha256 与计数,trajectory
// 层不认 tools::ToolRegistry(依赖方向:trajectory 不上探 app/runtime)。
struct ToolsetSummary {
    std::string toolset_sha256;
    std::uint64_t tool_count = 0;
};

// 环境快照的输入面。git 由调用方跑 GatherGitStatus 现取;其余字段从
// runtime/config 侧收集好整包递进来——trajectory 层只管拼装与落盘。
struct EnvironmentSnapshotInput {
    std::string lubancode_version;
    std::string build_commit;
    std::string build_type;
    std::string os_name;
    std::string arch;
    std::string locale;
    std::string timezone;
    std::string cwd;
    std::string repository_root;  // 空 = 不在 Git 仓库内(§3.2 回退启动 cwd)
    GitStatus git;

    std::string provider;
    std::string wire;
    std::string model;
    nlohmann::json model_parameters = nlohmann::json::object();

    std::optional<BlobRef> system_prompt_ref;  // 已落盘的 system prompt blob
    ToolsetSummary toolset;
    std::vector<std::string> project_instruction_refs;  // 已落盘的项目指令 blob sha256 清单
    std::vector<std::string> loaded_skill_refs;
    nlohmann::json plugin_refs = nlohmann::json::array();  // [{"id":..., "version":...}]

    // 已脱敏的配置快照(调用方过滤密钥字段;trajectory 层不做二次脱敏,
    // 只管落盘——脱敏合同在 config 装配层)。
    nlohmann::json config_snapshot_redacted = nlohmann::json::object();

    // 环境变量 allowlist 命中项(§9.1"环境变量只记 allowlist")。
    std::vector<std::pair<std::string, std::string>> allowlisted_env;
};

// §9.2 工作区重现等级。
enum class ReplayLevel { Exact, SourceExactEnvironmentPartial, InputOnly, Blocked };
const char* ReplayLevelName(ReplayLevel level);

struct ReplayLevelResult {
    ReplayLevel level = ReplayLevel::Blocked;
    std::vector<std::string> gaps;  // 人话缺口清单(稳定码风格,不翻译)
};

// 纯函数:source(git HEAD + dirty patch 是否齐全)与 environment(版本/
// 工具集/system prompt 是否齐全)两轴都站住才判 Exact;只 source 站住判
// SourceExactEnvironmentPartial;两边都缺但至少知道 cwd 判 InputOnly;
// 连 cwd 都没有判 Blocked(理论上不会发生,防御性兜底)。
ReplayLevelResult DetermineReplayLevel(const EnvironmentSnapshotInput& input);

// 组出 run.environment.captured 的事件 payload(snapshot_ref/replay_level/
// gaps 三键)。full_snapshot 是落 blob 前的完整快照(§9.1 全字段清单),
// dirty_patch/untracked 超过内联量就先各自落一份 blob,snapshot 里只留
// 引用——顶层快照 blob 不因为一份大 diff 被撑爆。
struct EnvironmentCapturePayload {
    nlohmann::json event_payload;
    nlohmann::json full_snapshot;
};
std::expected<EnvironmentCapturePayload, std::string> BuildEnvironmentCapturePayload(
    const EnvironmentSnapshotInput& input, BlobStore& blobs, Durability durability);

}  // namespace lubancode::trajectory
