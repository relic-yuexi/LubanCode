// 主动智能巡检(Proactive Heartbeat)引擎原语:
//   1) 语义静默哨兵 HEARTBEAT_OK 判定与双层去重裁决(HEARTBEAT_OK 静默 +
//      正文 SHA-256 未变去重;夹带任何业务文字一律宁投勿吞);
//   2) 工作区常驻巡检清单(.lubancode/HEARTBEAT.md 回退根 HEARTBEAT.md)
//      有界只读装载与有效条目识别;
//   3) 零 Token 前置变化探针(watch_paths 边界校验 + 工作区内确定性指纹)。
//
// 分层纪律:纯 engine 层(与 automation_store 同级),零 runtime/app/cli
// 依赖,零标准流输出,路径与 UTF-8 互转一律走 platform::Utf8ToPath /
// platform::PathToUtf8。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::gateway {

// 模型显式静默哨兵:仅当整段回复(去首尾空白及可选单层反引号/代码块围栏)
// 精确等于该词元时判为"无事发生、不打扰用户"。
inline constexpr std::string_view kHeartbeatSilentToken = "HEARTBEAT_OK";

// 资源与边界硬帽。
inline constexpr std::size_t kMaxHeartbeatWatchPaths = 16;
inline constexpr std::size_t kMaxHeartbeatChecklistBytes = 8 * 1024;
inline constexpr std::size_t kMaxWatchInlineHashBytes = 64 * 1024;
inline constexpr std::size_t kMaxWatchDirEntries = 512;

// 判定模型回复是否为纯静默哨兵 HEARTBEAT_OK。
// 夹带任何其他正文(如 "HEARTBEAT_OK\n但构建红了")一律回 false(宁投勿吞)。
bool IsHeartbeatSilentReply(std::string_view reply_text);

enum class HeartbeatSuppressReason {
    None,           // 有新内容/告警,走正常投递并更新 last_observed_sha
    SilentToken,    // 模型显式回 HEARTBEAT_OK,首拍起即抑制投递
    UnchangedHash,  // 非静默正文但与上次已通知 SHA-256 逐字节相同,抑制重复投递
};

struct HeartbeatDecision {
    HeartbeatSuppressReason suppress = HeartbeatSuppressReason::None;
    // 写入 occurrence.observed 的摘要(非空,保持 assistant_tasks 观察投影契约)。
    std::string observed_sha;
    // 抑制投递时是否同步更新 job.last_observed_sha:
    //   - UnchangedHash:false(原值本就相同);
    //   - SilentToken:若此前 last_observed_sha 非空且不等于静默摘要则为 true,
    //     将水位归位为静默摘要,保证"故障恢复后再复发同文"能再次触发通知。
    bool update_last_observed_on_suppress = false;
    // 抑制时的 occurrence.settled detail 文案。
    std::string settle_detail;

    bool suppressed() const { return suppress != HeartbeatSuppressReason::None; }
};

// 执行路与恢复路共用的 heartbeat 结算裁决纯函数。
HeartbeatDecision EvaluateHeartbeatReply(std::string_view reply_text,
                                         std::string_view last_observed_sha);

// 工作区 HEARTBEAT.md 常驻巡检清单。
struct WorkspaceHeartbeatChecklist {
    bool present = false;               // 清单文件存在且在工作区边界内
    bool has_actionable_items = false;  // 去标题/注释/空白后仍有实质巡检项
    std::string relative_source;        // ".lubancode/HEARTBEAT.md" 或 "HEARTBEAT.md"
    std::string content;                // UTF-8 安全截断后的正文(上限 8 KiB)
};

// 按优先级(.lubancode/HEARTBEAT.md -> HEARTBEAT.md)只读装载清单;零写盘。
WorkspaceHeartbeatChecklist LoadWorkspaceHeartbeatChecklist(
    const std::filesystem::path& workspace_root);

// 校验并规范化 watch_paths:
//   - 上限 16 项;每项非空 UTF-8 相对路径(<= 512 字节);
//   - 拒绝前导 '/'/'\\'、驱动器 ':'、空字节与任何 '..' 段;
//   - 统一分隔符为 '/' 并去重。
// 合法回空串(normalized_out 非空时填入规范化列表);非法回 "automation.watch_invalid: ..."。
std::string ValidateWatchPaths(const std::vector<std::string>& raw_paths,
                               std::vector<std::string>* normalized_out = nullptr);

struct WatchProbeResult {
    std::string fingerprint_sha;  // 全部监控目标的确定性聚合 SHA-256
    std::string summary;          // 供模型阅读的简短目标状态摘要
    std::size_t existing_count = 0;
    std::size_t missing_count = 0;
};

// 对工作区内 watch_paths 计算确定性状态指纹(零模型调用、防软链越界)。
// <= 64 KiB 普通文件直接哈希内容字节,消除毫秒级测试与快改下的 mtime 粒度抖动。
WatchProbeResult ProbeWorkspaceWatch(const std::filesystem::path& workspace_root,
                                     const std::vector<std::string>& watch_paths);

// 执行期拼装主动巡检回合提示词(不修改盘上 job.prompt 原文)。
std::string BuildHeartbeatTurnPrompt(std::string_view base_prompt,
                                     const WorkspaceHeartbeatChecklist& checklist,
                                     const WatchProbeResult* watch_probe = nullptr);

}  // namespace lubancode::gateway
