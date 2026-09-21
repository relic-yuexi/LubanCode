// TrajectorySessionLedger::Impl 的完整定义(AR-12 机械拆分的内部头,不
// 对外):桥类拆出多个 .cpp 后,子代理/workflow 的开账装配(Trajectory
// SessionLedger 的成员函数实现)与账本主体分住各件,Impl 定义自
// trajectory_session.cpp 挪到此处共用,一字未动。仅供 src/runtime 内部
// include;对外面仍是 Pimpl(trajectory_session.hpp 只见前向声明)。
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/types.hpp"
#include "runtime/session_soul.hpp"
#include "runtime/trajectory_history_view.hpp"
#include "runtime/trajectory_session.hpp"  // 外围类声明(嵌套 Impl 的定义点)
#include "runtime/trajectory_turn_bridge.hpp"  // V3SessionBooks
#include "telemetry/wake.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"           // ReplayControlState
#include "trajectory/session_manager.hpp"  // SessionManager/ActiveSession

namespace lubancode::runtime {

struct TrajectorySessionLedger::Impl {
    std::unique_ptr<trajectory::SessionManager> manager;
    std::filesystem::path workspaces_root;  // P0-2:唯一持久化根(查询/管理面用)
    trajectory::ActiveSession* active = nullptr;
    trajectory::RecorderOptions recorder_options;
    // v3 建场的基础 system(接线点 1):传给 manager 开首行,并作为共享
    // 账的初版正文(第一次请求带真 system 时走三步切换)。
    std::string v3_system_content;
    // 轮桥/子代理账的默认 training_policy(单发轨迹断档单:单发 Exclude,
    // 交互 Metadata——NewTurnBridge/SpawnSubagent 从这取,不再写死)。
    trajectory::TrainingPolicy training_policy = trajectory::TrainingPolicy::Metadata;
    std::string main_run_id;
    std::string lubancode_version;
    std::string workspace_root_text;  // UTF-8,环境快照与 git 状态取材用
    // 子代理账:run_id -> 终态 hash(Finish 时填,父账边界引用用)。
    std::map<std::string, std::string> child_terminal_hashes;
    std::uint64_t subagent_counter = 0;
    // 测试故障注入(生产恒空;子代理空轨迹单 5.1):子账首枚 run.started
    // 提交前问一次。
    std::function<std::optional<std::string>()> subagent_start_fault;
    // workflow 编排单同款:编排账/node 账首枚 run.started 提交前问一次。
    std::function<std::optional<std::string>()> workflow_start_fault;
    std::function<std::optional<std::string>()> workflow_node_start_fault;
    // --continue 启动路的 resume 投影(没 resume 为空)。
    bool launch_resumed = false;
    std::vector<api::Message> launch_resume_history;
    // 上下文预算单 P1:--continue 折叠出的控制态(resume 成功才有值)。
    std::optional<trajectory::ReplayControlState> launch_resume_control;
    // Soul 会话冻结单 P0:启动路 resume 带回的源场 soul 快照(nullopt =
    // 源场未锁定过魂);launch_resume_soul_error 非空 = 源场快照材料坏
    //(那种情况 resume 整个回落普通开张,不带着坏材料硬恢复)。
    std::optional<SessionSoulSnapshot> launch_resume_soul;
    std::string launch_resume_soul_error;
    // v3 源的旧史显示投影(P3;v2 源/没 resume 为 nullopt)。
    std::optional<RestoredHistoryView> launch_restored_view;
    // 接线点 1:v3 写模式的会话共享账(active 是 v3 场时有值;clear/resume
    // 换场后 BindV3Books 重绑)。
    std::optional<V3SessionBooks> v3_books;
    // T1 committed wake(§25.4):装配层挂 TelemetryService;默认空。
    telemetry::CommitObserver* telemetry_wake = nullptr;
    // Ctrl+T 浮层 v3 分页缓存(P3 第二棒):最近一场的渲染行,绑定源流
    // 路径与字节数——场没换、文件没长,翻页只切缓存,不重读重投(§4.10
    // "缓存须绑定源 hash";字节数判据与 session_index 的 v3 指纹同款,
    // append-only 账字节数变即内容变)。mutable:ReadTranscriptPage 是
    // const 读面。
    struct TranscriptCache {
        std::filesystem::path stream;
        std::uintmax_t bytes = 0;
        std::vector<RestoredTranscriptLine> lines;
    };
    mutable std::optional<TranscriptCache> transcript_cache;
};

}  // namespace lubancode::runtime
