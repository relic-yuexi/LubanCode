// 轨迹会话账的实现(P0-2 运行时单一写口)。合同见 trajectory_session.hpp
// 文件头;事件 payload 形状照 todos/P0新轨迹记录_可重放与训练投影设计.todo
// §五与 P0-1 的 schema.cpp 逐字段钉死的样子。AR-12 起桥类实现按类边界分住
// 各件:主轮/旁路/子代理/workflow 四桥见 trajectory_*_bridge.cpp,本件留
// 账本主体(开账/换场/resume/查询验账/标题与环境快照/选段器)与桥的装配;
// Impl 定义挪 internal 头 trajectory_session_impl.hpp 跨件共用。

#include "runtime/trajectory_session.hpp"
#include "runtime/v3_tool_result_material.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <string_view>
#include <utility>

#include "accounting/purpose.hpp"   // PurposeName(Token 账本单 A1)
#include "agent/context.hpp"        // EstimateUtf8Tokens:request_snapshot 的 token 估算
#include "agent/context_events.hpp"  // Fingerprint64:prepared 行 inputView 的视图指纹(V3-REAL-06)
#include "config/config.hpp"
#include "hooks/hash.hpp"           // Sha256Hex:request_snapshot 的 parameters_hash
#include "platform/atomic_write.hpp"  // AtomicWriteFile:workflow run 的 definition 快照
#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:子账 payload 账前兜底(UTF-8 清洗门单)
#include "runtime/trajectory_bridge_internal.hpp"  // kJournalEmergencyReserveBytes(容量门共用)
#include "runtime/trajectory_session_impl.hpp"     // Impl(桥类拆出后跨件共用)
#include "tools/path_utils.hpp"     // Utf8ToPath:主目录文本转路径
#include "tools/tool_content.hpp"   // TextContent:富结果块的文本投影
#include "trajectory/safety.hpp"
#include "trajectory/v3/reader.hpp"     // 接线点 1:AdoptSourceSystemV3_ 的源链投影
#include "trajectory/v3/subagent.hpp"    // 接线点 1:子账五步
#include "trajectory/v3/tool_action.hpp"  // 接线点 1:工具操作账
#include "workspace/identity.hpp"  // P0-1:身份裁决(冻结身份的兜底路)

namespace lubancode::runtime {

// trajectory v3 简称(本件内 v3:: 一律指 trajectory::v3;runtime 命名空间
// 下裸写 v3:: 解析不到 trajectory::v3)。
namespace v3 = ::lubancode::trajectory::v3;

// 发布门(P0-2 切换、P0-6 真机门收口):Session 即 Trajectory,没有
// feature/env 选路。开关的尸首已收干净——env 变量 LUBANCODE_TRAJECTORY
// 从此无人认领;配置键 features.trajectory 在 config 层读到即忽略并打
// 一行弃用告警(任何值类型都不拦门,过一两个版本连吞带删)。

// 原实现文件顶部的类型简称集随桥段迁走后,本件保留段(桥装配/账本面)
// 仍用非限定名,同款 using 集留一份。
namespace {

using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::EventLinks;
using trajectory::EventScope;
using trajectory::Origin;
using trajectory::RecordReceipt;
using trajectory::TrainingPolicy;
using trajectory::Visibility;

}  // namespace

// ---------------------------------------------------------------------------
// TrajectorySessionLedger
// ---------------------------------------------------------------------------


// §12.1 user-only 权限:workspace 层与 session 层目录都收紧;设不住须
// 告警(errors 进 /doctor trajectory 的"最近 I/O 错误"账)。
void HardenLedgerDirectories(const trajectory::TrajectoryDirectory& directory,
                             std::vector<std::string>* errors) {
    if (trajectory::HardenDirectoryUserOnly(directory.workspace_dir())) {
        if (!trajectory::HardenDirectoryUserOnly(directory.session_dir())) {
            errors->push_back("permissions:session_dir_harden_failed");
            platform::LogSink::Instance().Error(
                "trajectory", "session 目录无法收紧为 user-only,敏感内容记录有泄露面");
        }
        return;
    }
    errors->push_back("permissions:workspace_dir_harden_failed");
    platform::LogSink::Instance().Error("trajectory",
                                        "workspace 目录无法收紧为 user-only,敏感内容记录有泄露面");
}

std::expected<TrajectorySessionLedger, std::string> TrajectorySessionLedger::Open(Options options) {
    std::filesystem::path home_dir;
    if (options.workspaces_root.empty()) {
        // 会话账是运行数据,落状态根(应用Worker接入单 §4.2):应用根语义
        // =数据根,个人模式与从前同一处。
        const auto state_root = config::StateRootDir();
        if (!state_root.has_value()) {
            return std::unexpected("trajectory.no_home: 找不到主目录,会话账无处落");
        }
        home_dir = tools::Utf8ToPath(*state_root);
        options.workspaces_root = home_dir / "workspaces";
    }
    // P0-1:身份只认装配层递进的冻结 WorkspaceIdentity;空身份才按兜底根
    //(或启动 cwd)现场四级裁决——同仓子目录/linked worktree 不再各立各
    // 的房。子代理与 Gateway 恢复路由调用方显式递身份,不吃这条兜底。
    if (!options.workspace_identity.valid()) {
        std::error_code cwd_ec;
        const std::filesystem::path start = options.workspace_root.empty()
                                                ? std::filesystem::current_path(cwd_ec)
                                                : options.workspace_root;
        if (start.empty()) {
            return std::unexpected("identity.no_boundary: 启动工作目录取不到,身份无从裁决");
        }
        if (home_dir.empty()) {
            const auto state_root = config::StateRootDir();
            if (state_root.has_value()) {
                home_dir = tools::Utf8ToPath(*state_root);
            }
        }
        auto resolved = workspace::ResolveWorkspaceIdentity(start, home_dir);
        if (resolved.has_value()) {
            options.workspace_identity = std::move(*resolved);
        } else if (!options.workspace_root.empty()) {
            // 显式递了根的旧调用(测试):裁决失败退 cwd_fallback 形状。
            options.workspace_identity = workspace::MakeFallbackIdentity(options.workspace_root);
        } else {
            return std::unexpected(resolved.error());
        }
    }
    options.workspace_root = options.workspace_identity.checkout_root;
    trajectory::SessionManagerOptions manager_options;
    manager_options.workspaces_root = options.workspaces_root;
    manager_options.identity = options.workspace_identity;
    manager_options.workspace_root = options.workspace_root;
    manager_options.launch_cwd = options.launch_cwd;
    manager_options.lubancode_version = options.lubancode_version;
    manager_options.approval_mode = options.approval_mode;
    // 单发轨迹断档单:one_shot 场的 main run 单列 run_kind,manifest 与
    // run.started 同源落 one_shot。
    manager_options.main_run_kind =
        options.one_shot ? trajectory::RunKind::OneShot : trajectory::RunKind::MainSession;
    manager_options.recorder.event_schema_version = options.event_schema_version;
    // 接线点 1:v3 建场的首行基础 system(manager 侧 V3Writer::Start 用)。
    manager_options.v3_system_content = options.v3_system_content;
    // T08:主账写者的提交故障注入(测试专用;生产恒空)。
    manager_options.v3_main_io_fault = options.v3_main_io_fault;
    // 子代理空轨迹单 P0-C:main stream 同样走延迟开卷——正式 .jsonl 由
    // 首枚 run.started 提交事务独占创建,开张失败不在盘上留 0 字节文件。
    manager_options.recorder.defer_stream_create = true;

    Impl impl;
    impl.workspaces_root = options.workspaces_root;
    impl.v3_system_content = options.v3_system_content;
    impl.recorder_options.event_schema_version = options.event_schema_version;
    impl.lubancode_version = options.lubancode_version;
    impl.workspace_root_text = platform::PathToUtf8(options.workspace_root);
    impl.training_policy = options.training_policy;
    impl.subagent_start_fault = options.subagent_start_fault;
    impl.workflow_start_fault = options.workflow_start_fault;
    impl.workflow_node_start_fault = options.workflow_node_start_fault;
    impl.manager = std::make_unique<trajectory::SessionManager>(std::move(manager_options));

    // --continue 启动路(§10.4):直接建 start_reason=resume 的新 session,
    // 不先造空 session。没有可恢复场(或源场验不过)回落普通开张,与旧路
    // --continue 的 quiet_if_none 语义一致;真出错(目录坏了开不出新场)
    // 照旧失败退出,不回退旧写口。
    if (options.resume_at_launch) {
        const std::string latest = impl.manager->LatestResumableSessionId();
        // 显式指名的源不受 LatestResumable 的"running 不碰"连坐——那是
        // 自动挑最近场的筛子;接管硬杀场(running、无活锁)是常驻恢复的
        // 正路(V0 受理底线),可恢复性(活锁/one_shot/验卷)由 ResumeAsNew
        // 的七步裁定,失败仍回落普通开张。
        if (!latest.empty() || !options.resume_source_session_id.empty()) {
            // Soul 会话冻结单 P0(§5.3):源场 soul 快照在 ResumeAsNew 之前
            // 先读——材料坏就整个回落普通开张(与"源场验不过回落"同一
            // 拍,--continue 没指名要哪场,不带着坏材料硬恢复),错误记
            // 账可见,不静默换魂。源场 id 与 ResumeAsNew 用同一份(显式
            // 指名时是指名那场,不是最近那场)。
            const std::string source_id =
                options.resume_source_session_id.empty() ? latest : options.resume_source_session_id;
            const auto source_soul = ReadSessionSoulSnapshot(impl.manager->SessionDirOf(source_id));
            if (!source_soul.has_value()) {
                impl.launch_resume_soul_error = source_soul.error();
                platform::LogSink::Instance().Error(
                    "trajectory", "--continue 源场 Soul 快照材料坏,回落普通开张: " + source_soul.error());
            } else if (source_soul->has_value()) {
                impl.launch_resume_soul = **source_soul;
            }
            if (impl.launch_resume_soul_error.empty()) {
            trajectory::ResumeRequest resume;
            resume.source_session_id = source_id;
            resume.interactive = false;  // 启动路没有旧 requested 可指
            const auto resumed = impl.manager->ResumeAsNew(resume);
            if (resumed.error_code.empty()) {
                impl.active = impl.manager->active();
                impl.main_run_id = impl.active->manifest.main_run_id;
                impl.launch_resumed = true;
                // 上下文预算单 P1:控制态整份留底——预算恢复裁决与交互
                // /resume 走同一只仲裁(装配层 ApplyResumedContextWindow)。
                impl.launch_resume_control = resumed.control;
                impl.launch_resume_history = ProjectHistoryFromReplay(
                    [&resumed] {
                        trajectory::ReplayState projection;
                        projection.effective_conversation = resumed.effective_conversation;
                        return projection;
                    }());
                // v3 源:旧史显示投影(P3 接线点 5)。验卷在 ResumeAsNew
                // 已过,这里再读一次是纯读路径(文件小,OS 缓存兜着);
                // 读不动给空 view,不拦 resume 本身。
                if (resumed.source_is_v3) {
                    impl.launch_restored_view = ProjectRestoredHistory(resumed.source_v3_stream);
                }
                TrajectorySessionLedger ledger;
                ledger.impl_ = std::make_unique<Impl>(std::move(impl));
                ledger.BindV3Books_();
                // v3 源:续接场沿用源场生效 system(§4.10 默认;v2 源的
                // 迁移新场保持基础版,三步切换由后续按需走)。
                if (resumed.source_is_v3) {
                    ledger.AdoptSourceSystemV3_(resumed.source_v3_stream);
                }
                HardenLedgerDirectories(ledger.impl_->active->directory, &ledger.io_errors_);
                return ledger;
            }
            // resume 失败回落普通开张:源场坏不拦人开新会话(明错留给
            // /doctor trajectory 查),与旧路 --continue 找不到档不报错同门。
            }
        }
    }
    auto active = impl.manager->LaunchSession();
    if (!active.has_value()) {
        return std::unexpected("trajectory.launch_failed: " + active.error());
    }
    impl.active = *active;
    impl.main_run_id = impl.active->manifest.main_run_id;

    TrajectorySessionLedger ledger;
    ledger.impl_ = std::make_unique<Impl>(std::move(impl));
    ledger.BindV3Books_();
    HardenLedgerDirectories(ledger.impl_->active->directory, &ledger.io_errors_);
    return ledger;
}

TrajectorySessionLedger::TrajectorySessionLedger(TrajectorySessionLedger&&) noexcept = default;
TrajectorySessionLedger::~TrajectorySessionLedger() = default;

void TrajectorySessionLedger::BindV3Books_() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->active != nullptr && impl_->active->is_v3()) {
        v3::V3Writer* writer = &*impl_->active->v3_main;
        if (!impl_->v3_books.has_value() || impl_->v3_books->writer != writer ||
            impl_->v3_books->bound_session_id != impl_->active->session_id()) {
            // 首绑或换场(clear/resume 后 active 换了主账):回到新场的
            // 基础 system(§4.10 的"沿用源场 system"由 AdoptSourceSystemV3_
            // 按切换流程补,不在绑定时偷改)。换场判据加场次 id:active 是
            // std::optional,clear 同址换值时新写者地址与旧写者相同,单比
            // 指针认不出换场,旧 books(含 T12-A 执行阻断)会带进新场。
            impl_->v3_books = V3SessionBooks{};
            impl_->v3_books->bound_session_id = impl_->active->session_id();
            impl_->v3_books->system_content = impl_->v3_system_content;
            impl_->v3_books->settings_version = 1;
        }
        impl_->v3_books->writer = writer;
    } else {
        impl_->v3_books.reset();
    }
}

void TrajectorySessionLedger::AdoptSourceSystemV3_(const std::filesystem::path& source_stream) {
    if (impl_ == nullptr || !impl_->v3_books.has_value()) {
        return;
    }
    // §4.10 默认:resume 沿用旧场生效 system。源是 v3 场时读源链的当前
    // system 正文,与基础版不同就按 §4.3 三步切换——不悄悄把新 system 套
    // 在基础版根上(链根与实际发送必须同拍)。源是 v2 场没有 v3 system
    // 概念,保持基础版,第一次请求照常对表。
    auto ledger = v3::ReadV3Ledger(source_stream);
    if (!ledger.has_value()) {
        return;
    }
    const auto context = v3::ProjectModelContext(*ledger);
    if (context.system_content.empty() || context.system_content == impl_->v3_books->system_content) {
        return;
    }
    nlohmann::json change;
    change["cause"] = "resume_adopted_source_system";
    change["settingsVersion"] = impl_->v3_books->settings_version + 1;
    change["systemChanged"] = true;
    const auto switched = impl_->v3_books->writer->SwitchSystem(
        context.system_content, std::move(change), v3::MessageOrigin::SessionRuntime,
        trajectory::Durability::PowerLoss);
    if (switched.change_event.status != v3::WriteReceipt::Status::Committed ||
        switched.system_message.status != v3::WriteReceipt::Status::Committed ||
        switched.apply_event.status != v3::WriteReceipt::Status::Committed) {
        io_errors_.push_back("trajectory.v3_adopt_source_system_failed");
        return;
    }
    impl_->v3_books->system_content = context.system_content;
    ++impl_->v3_books->settings_version;
}

trajectory::TrajectoryRecorder* TrajectorySessionLedger::main() {
    return impl_ != nullptr && impl_->active != nullptr && impl_->active->main.has_value()
               ? &*impl_->active->main
               : nullptr;
}

std::unique_ptr<TrajectoryTurnBridge> TrajectorySessionLedger::NewTurnBridge(
    TrajectoryTurnBridge::Identity identity) {
    // 接线点 1:v3 场造 v3 模式桥(绑 V3Writer + 会话共享账)。
    if (impl_ != nullptr && impl_->active != nullptr && impl_->active->is_v3()) {
        trajectory::EventScope identity_scope;
        identity_scope.workspace_key = impl_->active->manifest.workspace_key;
        identity_scope.session_id = impl_->active->session_id();
        identity_scope.run_id = impl_->active->manifest.main_run_id;
        auto bridge = std::make_unique<TrajectoryTurnBridge>(
            &*impl_->active->v3_main, &*impl_->v3_books, std::move(identity_scope),
            std::move(identity));
        bridge->SetErrorSink(&io_errors_);
        if (impl_->telemetry_wake != nullptr) {
            bridge->SetCommitWake(impl_->telemetry_wake,
                                  impl_->active->directory.v3_stream_path().filename().generic_string());
        }
        return bridge;
    }
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return nullptr;
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = impl_->training_policy;
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, std::move(scope),
                                                         std::move(identity));
    // 桥按轮把落账错误推进账本的共享环(/doctor trajectory 从这读)。
    bridge->SetErrorSink(&io_errors_);
    // T1 committed wake:挂上后 main stream 每笔提交都投 wake(默认空)。
    if (impl_ != nullptr && impl_->telemetry_wake != nullptr) {
        bridge->SetCommitWake(impl_->telemetry_wake, "main.jsonl");
    }
    return bridge;
}

std::unique_ptr<TrajectoryBypassBridge> TrajectorySessionLedger::NewBypassBridge(
    TrajectoryTurnBridge::Identity identity, accounting::RequestPurpose purpose) {
    // 接线点 1 分期边界(取消误报 ESC 单 Bug 2 收窄一格 + T11-A 扩一格):
    // v3 场给用途有消息合同落点的请求接 v3 旁路桥——memory_extract(内部
    // 回合号/消息 purpose/prepared 合同齐备)与 title_refine(T11-A 起自动
    // 起名的 prompt/assistant 留正式 message,purpose=session_title,§4.34
    // 归首问主回合)。其余用途维持 nullptr:compact 在 v3 有自己的全链运行
    // 时(RunV3Compact),doctor 待接(§四清册在案),不冒进也不硬塞 V2 行。
    if (impl_ != nullptr && impl_->active != nullptr && impl_->active->is_v3()) {
        if (purpose != accounting::RequestPurpose::MemoryExtract &&
            purpose != accounting::RequestPurpose::TitleRefine) {
            return nullptr;
        }
        trajectory::EventScope identity_scope;
        identity_scope.workspace_key = impl_->active->manifest.workspace_key;
        identity_scope.session_id = impl_->active->session_id();
        identity_scope.run_id = impl_->active->manifest.main_run_id;
        auto bridge = std::make_unique<TrajectoryBypassBridge>(
            &*impl_->active->v3_main, &*impl_->v3_books, std::move(identity_scope), std::move(identity),
            purpose);
        bridge->SetErrorSink(&io_errors_);
        if (impl_->telemetry_wake != nullptr) {
            bridge->SetCommitWake(
                impl_->telemetry_wake,
                impl_->active->directory.v3_stream_path().filename().generic_string());
        }
        return bridge;
    }
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr || impl_ == nullptr || impl_->active == nullptr) {
        return nullptr;
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = TrainingPolicy::Metadata;
    auto bridge =
        std::make_unique<TrajectoryBypassBridge>(*recorder, std::move(scope), std::move(identity));
    if (impl_->telemetry_wake != nullptr) {
        bridge->SetCommitWake(impl_->telemetry_wake, "main.jsonl");
    }
    return bridge;
}

void TrajectorySessionLedger::SetTelemetryWake(telemetry::CommitObserver* wake) {
    if (impl_ != nullptr) {
        impl_->telemetry_wake = wake;
    }
}

void TrajectorySessionLedger::NotifyCommitted_() const {
    if (impl_ == nullptr || impl_->telemetry_wake == nullptr || impl_->active == nullptr) {
        return;
    }
    if (!impl_->active->main.has_value()) {
        return;  // v3 场没有 v2 main recorder;wake 由各 v3 桥自己投。
    }
    telemetry::CommitWake wake;
    const trajectory::EventScope& scope = impl_->active->main->base_scope();
    wake.workspace_key = scope.workspace_key;
    wake.session_id = scope.session_id;
    wake.stream_id = "main.jsonl";
    impl_->telemetry_wake->Notify(wake);
}


trajectory::CloseOutcome TrajectorySessionLedger::CloseSession(const std::string& reason) {
    trajectory::CloseRequest request;
    request.reason = reason;
    trajectory::NullClearParticipant participant;
    return impl_->manager->Close(request, &participant);
}

TrajectorySessionLedger::CwdChangeResult TrajectorySessionLedger::HandleCwdChange(
    const workspace::WorkspaceIdentity& new_identity) {
    CwdChangeResult result;
    result.workspace_key = new_identity.workspace_key;
    if (impl_ == nullptr || impl_->manager == nullptr || impl_->active == nullptr) {
        result.error = "trajectory.open_failed: 会话账未开,cwd 变化无处对账";
        return result;
    }
    if (new_identity.workspace_key != impl_->manager->workspace_key()) {
        // 跨 workspace:账一个字不写,交调用方封场换账(§4.5)。
        return result;
    }
    // 同 workspace:cwd.changed 事件 + 检出登记(worktree 进出房各记一笔)。
    PutControl_(trajectory::EventKind::ControlCwdChanged,
                nlohmann::json{{"cwd", platform::PathToUtf8(new_identity.launch_cwd)}});
    if (const auto touched = impl_->manager->RegisterCheckout(new_identity); !touched.has_value()) {
        result.error = touched.error();
    }
    result.same_workspace = true;
    return result;
}

std::string FormatHostDirectoryNoticeText(const std::string& old_cwd_utf8, const std::string& new_cwd_utf8,
                                          const std::string& reason) {
    // 正文按单子 §五 B 的建议格式,补原因行;开头带来源标识。
    return "[宿主通知] 已切换会话工作目录:\n原目录: " + old_cwd_utf8 + "\n当前目录: " + new_cwd_utf8 +
           "\n原因: " + reason + "\n后续相对路径与命令默认在当前目录执行。";
}

std::string TrajectorySessionLedger::RecordHostDirectoryNotice(const std::string& old_cwd_utf8,
                                                               const std::string& new_cwd_utf8,
                                                               const std::string& reason) {
    // 前缀缓存守恒单 §五 B:宿主目录通知进 SessionV3 输入提交链。v2 老账
    // 消费场没有主写者,如实报错不伪造行(新场自 P0-2 起恒 v3,这条只在
    // 旧档旁路里才可能碰到)。
    if (impl_ == nullptr || !impl_->v3_books.has_value() || impl_->v3_books->writer == nullptr) {
        return "cwd_notice.not_v3: 当前会话账没有 v3 主写者,宿主目录通知无处落";
    }
    v3::V3Writer* writer = impl_->v3_books->writer;
    // turnId 必填(schema §1.2 user 消息):主回合开着挂当前回合(工具触发
    // 的 enter/exit 就发生在这一轮),空闲 slash 路自起新号——与 memory
    // .recall 注入同一纪律,不冒充任何真人回合。
    std::string turn_id = OpenMainTurnId().value_or(std::string());
    if (turn_id.empty()) {
        turn_id = writer->NewTurnId();
    }
    const std::string text = FormatHostDirectoryNoticeText(old_cwd_utf8, new_cwd_utf8, reason);
    v3::MessageDraft draft;
    draft.message_id_override = writer->NewMessageId();
    draft.turn_id = turn_id;
    draft.purpose = v3::MessagePurpose::Conversation;
    draft.origin = v3::MessageOrigin::SessionRuntime;  // 宿主来源,不冒充人类输入
    draft.display = v3::DisplayMode::Visible;
    draft.message = nlohmann::json{{"role", "user"}, {"content", text}};
    // 通知是"下一请求必须带上"的输入:两步都按 PowerLoss 落,任何一步没
    // 落稳都报错给调用方(阻断或明确中止),不静默发目录过期的请求。
    const auto committed = writer->AppendMessage(std::move(draft), trajectory::Durability::PowerLoss);
    if (committed.status != v3::WriteReceipt::Status::Committed) {
        return "cwd_notice.append_failed: " + committed.error_code;
    }
    const auto admitted = writer->AdmitMessages({committed.id}, trajectory::Durability::PowerLoss);
    if (admitted.status != v3::WriteReceipt::Status::Committed) {
        // 消息已落盘但没进链:惰性行,链投影不含它——如实报错,调用方按
        // 失败处置(阻断),不拿半截账冒充成功。
        return "cwd_notice.admit_failed: " + admitted.error_code;
    }
    return {};
}

// ---------------------------------------------------------------------------
// P0-3:clear 八步 / resume-as-new / replay 读口
// ---------------------------------------------------------------------------

std::vector<api::Message> ProjectHistoryFromReplay(const trajectory::ReplayState& state) {
    std::vector<api::Message> history;
    for (const auto& message : state.effective_conversation) {
        api::Message projected;
        switch (message.role) {
            case trajectory::ReplayMessage::Role::User:
                projected.role = api::Role::User;
                break;
            case trajectory::ReplayMessage::Role::Assistant:
                projected.role = api::Role::Assistant;
                break;
            case trajectory::ReplayMessage::Role::Tool:
                projected.role = api::Role::User;  // ToolResult 以 user 消息携带(与 hub 回喂同形)
                break;
        }
        if (!message.blocks.is_array()) {
            continue;
        }
        if (message.role == trajectory::ReplayMessage::Role::Tool) {
            // ToolResult:content blocks -> ToolResultBlock,call_id 配对(§11.2)。
            // is_error 随 ReplayMessage 还原(P1-B/FA-02):回喂语义在写侧
            // 已随最终 tool 消息落档,这里原样带回——模型收到的成功/失败
            // 语义写盘再读回不变,不从执行终态重猜。
            api::ToolResultBlock result;
            result.tool_use_id = message.call_id.value_or(std::string());
            result.is_error = message.is_error;
            for (const auto& block : message.blocks) {
                if (block.is_object() && block.value("type", std::string()) == "text" &&
                    block.contains("text")) {
                    result.content = block["text"].get<std::string>();
                    break;
                }
            }
            projected.content.push_back(std::move(result));
            history.push_back(std::move(projected));
            continue;
        }
        for (const auto& block : message.blocks) {
            if (!block.is_object()) {
                continue;
            }
            const std::string type = block.value("type", std::string());
            if (type == "text" && block.contains("text")) {
                api::TextBlock text;
                text.text = block["text"].get<std::string>();
                projected.content.push_back(std::move(text));
            } else if (type == "thinking" && block.contains("text")) {
                api::ThinkingBlock thinking;
                thinking.text = block["text"].get<std::string>();
                thinking.signature = block.value("signature", std::string());
                thinking.responses_item = block.value("responses_item", nlohmann::json(nullptr));
                projected.content.push_back(std::move(thinking));
            } else if (type == "tool_call" && block.contains("call_id") && block.contains("name")) {
                api::ToolUseBlock call;
                call.id = block["call_id"].get<std::string>();
                call.name = block["name"].get<std::string>();
                if (block.contains("arguments")) {
                    call.input = block["arguments"];
                }
                projected.content.push_back(std::move(call));
            }
        }
        history.push_back(std::move(projected));
    }
    return history;
}

trajectory::ClearOutcome TrajectorySessionLedger::ClearSession(
    const trajectory::ClearRequest& request, trajectory::ClearParticipant* participant) {
    if (impl_ == nullptr || impl_->manager == nullptr) {
        trajectory::ClearOutcome outcome;
        outcome.error_code = "clear.no_active_session";
        return outcome;
    }
    const auto outcome = impl_->manager->Clear(request, participant);
    if (outcome.error_code.empty()) {
        // 账本跟着换场:active 指针(manager 内 std::optional 同址换值)、
        // run 号、选段器重置(新场不带旧 selection,§3.3.1)。
        impl_->active = impl_->manager->active();
        if (impl_->active != nullptr) {
            impl_->main_run_id = impl_->active->manifest.main_run_id;
        }
        BindV3Books_();
        impl_->child_terminal_hashes.clear();
        record_selection_ = nullptr;  // 惰性重建(RecordSelectionController)
        environment_captured_ = false;  // 新 run 须重采环境快照
    }
    return outcome;
}

TrajectoryResumeSummary TrajectorySessionLedger::ResumeInteractive(const std::string& source_session_id,
                                                                   const std::string& command_name) {
    TrajectoryResumeSummary summary;
    if (impl_ == nullptr || impl_->manager == nullptr) {
        summary.outcome.error_code = "resume.no_ledger";
        summary.outcome.message = "轨迹账本没开";
        return summary;
    }
    trajectory::SessionManager& manager = *impl_->manager;
    const bool has_active = manager.active() != nullptr;
    trajectory::ResumeRequest request;
    request.source_session_id = source_session_id;
    request.interactive = has_active;  // 交互路:有旧场才有跨 session requested 可指
    request.user_initiated = true;

    // R3:封场前先只读预检源(单段名/目录/格式/one_shot/活锁)——不过
    // 当场报错返回,当前场不封、新场不建、不发模型请求。此前先 Close 再
    // ResumeAsNew,源预检失败时当前场已封回不来。
    if (const auto source_probe = manager.ProbeResumeSource(source_session_id);
        !source_probe.ok()) {
        summary.outcome.error_code = source_probe.error_code;
        summary.outcome.message = source_probe.message;
        return summary;
    }

    // Soul 会话冻结单 P0(§5.3):恢复源场已提交 soul 快照——只认 blob 里
    // 的正文,不凭魂名重读磁盘新默认。源场从未锁定过(nullopt)不挡
    // resume,恢复后按未锁定草稿起步;有快照但材料坏:报错拒绝,当前场
    // 不封、新场不建,不静默换魂。
    std::optional<SessionSoulSnapshot> source_soul;
    {
        const auto soul = ReadSessionSoulSnapshot(manager.SessionDirOf(source_session_id));
        if (!soul.has_value()) {
            summary.outcome.error_code = "resume.soul_snapshot_corrupt";
            summary.outcome.message = "源会话的 Soul 快照材料损坏: " + soul.error();
            return summary;
        }
        source_soul = *soul;
    }

    // 旧场(若有):requested 先 durable,随后 switch_to_resume 封口
    //(§10.4/§14.1 的 clear/resume 例外:旧 main 写 requested 与 terminal)。
    if (has_active) {
        const std::string command_id = "cmd-" + std::to_string(++command_counter_);
        const std::string boundary_operation_id =
            impl_->active->session_id() + ":resume";  // 稳定可追,不落随机
        auto* old_main = impl_->active->main.has_value() ? &*impl_->active->main : nullptr;
        std::string requested_event_id;
        if (old_main != nullptr) {
            trajectory::RecordRequest requested;
            requested.kind = trajectory::EventKind::ControlCommandRequested;
            requested.scope = old_main->base_scope();
            requested.scope.actor = trajectory::Actor::User;
            requested.scope.origin = trajectory::Origin::ExternalUser;
            requested.scope.visibility = {trajectory::Visibility::HostOnly};
            requested.scope.training_policy = trajectory::TrainingPolicy::Exclude;
            requested.payload["command_id"] = command_id;
            requested.payload["command_name"] = command_name;
            requested.payload["action_name"] = command_name;
            requested.payload["effect_class"] = "session_boundary";
            requested.payload["args_ref"] =
                nlohmann::json{{"source_session_id", source_session_id},
                               {"boundary_operation_id", boundary_operation_id}};
            requested.links.correlation_id = boundary_operation_id;
            const auto receipt = old_main->Record(requested, trajectory::Durability::PowerLoss);
            if (receipt.status == trajectory::RecordReceipt::Status::Committed) {
                requested_event_id = receipt.event_id;
            }
        }
        trajectory::CloseRequest close;
        close.reason = "switch_to_resume";
        trajectory::NullClearParticipant participant;
        const auto closed = manager.Close(close, &participant);
        if (!closed.error_code.empty()) {
            summary.outcome.error_code = "resume." + closed.error_code;
            summary.outcome.message = "封旧场失败: " + closed.message;
            return summary;
        }
        request.previous_session_id = closed.session_id;
        if (!requested_event_id.empty()) {
            request.boundary_command.command_id = command_id;
            request.boundary_command.requested_session_id = closed.session_id;
            request.boundary_command.requested_event_id = requested_event_id;
            request.boundary_command.boundary_operation_id = boundary_operation_id;
        } else {
            // requested 落不住(旧账坏):不硬造跨 session 生命周期,按
            // 启动路口径办(§14.1 的开口只在 requested 真落了才走)。
            request.interactive = false;
        }
    }
    summary.outcome = manager.ResumeAsNew(request);
    if (!summary.outcome.error_code.empty()) {
        return summary;
    }
    // 换场成功:账本指到落点场(v3 源=续接的源场,v2 源=迁移新场),选段
    // 器重置,history 折叠投影交出去。
    impl_->active = manager.active();
    if (impl_->active != nullptr) {
        impl_->main_run_id = impl_->active->manifest.main_run_id;
    }
    // v3 books 强制重建:续接源场时 active 是同址换值(新写者地址可能
    // 与旧写者相同,BindV3Books_ 的换场判据认不出),旧 books 里的粘账
    //(T12-A 执行阻断、turn 粘账)不许带过换场点——resume 重开即重建,
    // 与 fork 落点同一合同。system 沿 §4.10 由下方 AdoptSourceSystemV3_
    // 按账面现行版本重采。
    impl_->v3_books.reset();
    BindV3Books_();
    impl_->child_terminal_hashes.clear();
    record_selection_ = nullptr;
    environment_captured_ = false;
    // v3 源:续接场沿用源场生效 system(§4.10 默认,三步切换补账——本场
    // books 从基础版重起步,账上现行 system 与之不同就照 §4.3 切换)。
    if (summary.outcome.source_is_v3) {
        AdoptSourceSystemV3_(summary.outcome.source_v3_stream);
    }
    trajectory::ReplayState projection_state;
    // 投影只需要 effective conversation;从 outcome 的引用直接翻。
    projection_state.effective_conversation = summary.outcome.effective_conversation;
    summary.history = ProjectHistoryFromReplay(projection_state);
    // v3 源:旧史显示投影(P3 接线点 5)——含被压缩原文、hidden 标志与
    // 压缩标记;调用方一次性铺滚动缓冲,不进 live 条目账。
    if (summary.outcome.source_is_v3) {
        summary.restored_view = ProjectRestoredHistory(summary.outcome.source_v3_stream);
    }
    summary.soul_snapshot = std::move(source_soul);
    return summary;
}

std::string TrajectorySessionLedger::LatestResumableSessionId() const {
    return impl_ != nullptr && impl_->manager != nullptr
               ? impl_->manager->LatestResumableSessionId()
               : std::string();
}

std::string TrajectorySessionLedger::UpdateApprovalMode(ApprovalMode mode) {
    if (impl_ == nullptr || impl_->manager == nullptr) {
        return "trajectory.not_open: 轨迹账未开张,切档只在内存生效";
    }
    const auto updated = impl_->manager->UpdateApprovalMode(mode);
    if (!updated.has_value()) {
        return updated.error();
    }
    return std::string();
}

bool TrajectorySessionLedger::resumed_at_launch() const {
    return impl_ != nullptr && impl_->launch_resumed;
}

std::string TrajectorySessionLedger::launch_resume_source_session_id() const {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->launch_resumed) {
        return std::string();
    }
    return impl_->active->manifest.previous_session_id.value_or(std::string());
}

TrajectorySessionLedger::SessionLineageInfo TrajectorySessionLedger::session_lineage() const {
    SessionLineageInfo info;
    if (impl_ == nullptr || impl_->active == nullptr) {
        return info;
    }
    info.start_reason = impl_->active->manifest.start_reason;
    info.previous_session_id =
        impl_->active->manifest.previous_session_id.value_or(std::string());
    return info;
}

std::vector<api::Message> TrajectorySessionLedger::LaunchResumeHistory() const {
    return impl_ != nullptr ? impl_->launch_resume_history : std::vector<api::Message>();
}

std::optional<RestoredHistoryView> TrajectorySessionLedger::LaunchRestoredHistoryView() const {
    return impl_ != nullptr ? impl_->launch_restored_view : std::nullopt;
}

std::optional<SessionSoulSnapshot> TrajectorySessionLedger::LaunchResumeSoulSnapshot() const {
    return impl_ != nullptr ? impl_->launch_resume_soul : std::nullopt;
}

std::string TrajectorySessionLedger::launch_resume_soul_error() const {
    return impl_ != nullptr ? impl_->launch_resume_soul_error : std::string();
}

std::optional<trajectory::ReplayControlState> TrajectorySessionLedger::LaunchResumeControlState() const {
    if (impl_ == nullptr || !impl_->launch_resumed) {
        return std::nullopt;
    }
    return impl_->launch_resume_control;
}

std::string TrajectorySessionLedger::CommitSoulSnapshot(const SessionSoulSnapshot& snapshot) {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return "soul_snapshot.no_active_session: 轨迹账未开张,快照只留内存";
    }
    const auto written = WriteSessionSoulSnapshot(impl_->active->directory.session_dir(), snapshot);
    if (!written.has_value()) {
        io_errors_.push_back("soul_snapshot.write_failed: " + written.error());
        return written.error();
    }
    return std::string();
}

trajectory::ReplayReport TrajectorySessionLedger::FoldMainReplay() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        trajectory::ReplayReport report;
        report.error_code = "replay.no_active_session";
        return report;
    }
    // 接线点 1 收尾棒:v3 场没有 main.jsonl,折叠投影走 ReadV3Ledger +
    // ProjectModelContext 链投影——与 /resume/--continue 同一份
    //(EffectiveConversationFromV3 公开共用,/export、/copy 从这取数,不
    // 各造各账)。ReplayState 只装身份与有效对话:turn/请求步/工具台账是
    // v2 折叠概念,v3 不硬造;验卷不过如实报错,不折半本。
    if (impl_->active->is_v3()) {
        trajectory::ReplayReport report;
        const auto ledger = v3::ReadV3Ledger(impl_->active->directory.v3_stream_path());
        if (!ledger.has_value()) {
            report.error_code = "replay.v3_ledger_failed";
            report.message = ledger.error();
            return report;
        }
        report.state.session_id = ledger->session_id;
        report.state.run_id = ledger->run_id;
        report.state.effective_conversation =
            trajectory::EffectiveConversationFromV3(*ledger, v3::ProjectModelContext(*ledger));
        report.state.integrity.events_folded = ledger->lines;
        if (const auto last = ledger->LastEntry(); last.has_value()) {
            report.state.folded_seq = last->seq;
        }
        return report;
    }
    return trajectory::FoldStreamReplay(impl_->active->directory.main_stream_path());
}

trajectory::SessionVerifyReport TrajectorySessionLedger::VerifySession() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        trajectory::SessionVerifyReport report;
        report.error_code = "verify.no_active_session";
        return report;
    }
    // 接线点 1 收尾棒:v3 场走 VerifyV3SessionDir——主账 v3 卷整卷验链 +
    // v3::WalkSessionTree 递归 subagents/ 子账树,折成与 v2 同形状的报告
    //(此前只验主账,子账树归本棒接上)。
    if (impl_->active->is_v3()) {
        return trajectory::VerifyV3SessionDir(impl_->active->directory.session_dir());
    }
    return trajectory::VerifySessionDir(impl_->active->directory.session_dir());
}

TrajectorySessionLedger::ExactReplay TrajectorySessionLedger::ExactReplayMain() const {
    ExactReplay replay;
    const auto fold = FoldMainReplay();
    if (!fold.ok()) {
        replay.error_code = fold.error_code;
        return replay;
    }
    replay.ok = true;
    replay.state_hash = trajectory::ComputeReplayStateHash(fold.state);
    replay.state = std::move(fold.state);
    return replay;
}

RecordSelectionController& TrajectorySessionLedger::record_selection() {
    if (record_selection_ == nullptr) {
        record_selection_ = std::make_unique<RecordSelectionController>(*this);
    }
    return *record_selection_;
}

// 会话级控制事件(compact/record 一族)的公共落账口。
void TrajectorySessionLedger::PutControl_(trajectory::EventKind kind, nlohmann::json payload) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::CompactRuntime;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        platform::LogSink::Instance().Error(
            "trajectory", std::string("control 落账失败: ") + receipt.error_code);
        return;
    }
    NotifyCommitted_();
}

void TrajectorySessionLedger::RecordCompactRequested(const std::string& trigger, int old_epoch,
                                                     const std::string& input_state_hash) {
    nlohmann::json payload = nlohmann::json{{"trigger", trigger}};
    if (old_epoch > 0) {
        payload["old_epoch"] = static_cast<std::uint64_t>(old_epoch);
    }
    if (!input_state_hash.empty()) {
        payload["input_state_hash"] = input_state_hash;
    }
    PutControl_(trajectory::EventKind::CompactRequested, std::move(payload));
}

void TrajectorySessionLedger::RecordCompactApplied(const std::string& old_state_hash,
                                                   const std::string& new_state_hash,
                                                   std::uint64_t pre_tokens, std::uint64_t post_tokens,
                                                   int new_epoch) {
    PutControl_(trajectory::EventKind::CompactApplied,
                nlohmann::json{{"old_state_hash", old_state_hash},
                               {"new_state_hash", new_state_hash},
                               {"source_event_span", nlohmann::json::array({std::uint64_t{1}, SpanEndSeq()})},
                               {"pre_tokens", pre_tokens},
                               {"post_tokens", post_tokens},
                               {"epoch", static_cast<std::uint64_t>(new_epoch)}});
}

void TrajectorySessionLedger::RecordCompactFailed(const std::string& reason) {
    PutControl_(trajectory::EventKind::CompactFailed, nlohmann::json{{"reason", reason}});
}

// v3 compact 运行时接线(compact 全链单):v3 会话的主写者取用口。
// 接线点 1 已并:开卷装配持有 ActiveSession::v3_main,active 是 v3 场
// 即返回真写者,/compact 与自动压缩的分派门即刻通电;v2 场(或无活
// 场)照旧返回 nullptr 走 v2 老路,一字不动。
trajectory::v3::V3Writer* TrajectorySessionLedger::v3_main_writer() {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3()) {
        return nullptr;
    }
    return &*impl_->active->v3_main;
}

// 异步工具 P2:会话共享账的写者互斥锁(闸门/协调器/规划器共享写者用;
// v2 场 nullptr——不装异步运行时)。
std::shared_ptr<std::recursive_mutex> TrajectorySessionLedger::v3_tool_results_mutex() {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3() ||
        !impl_->v3_books.has_value()) {
        return nullptr;
    }
    return impl_->v3_books->tool_results_mutex;
}

// v3 结果仓统计(V3-REAL-A02):按仓的记账单位现数——res-*.json 一文件
// 一枚逻辑工具结果(枚数可核),字节收全部 res-* 伴生文件。captures 原始
// 捕获仓(capture-*)不混入:那是执行侧原始捕获,不是"工具结果"本体。
std::optional<TrajectorySessionLedger::V3ResultStoreStats> TrajectorySessionLedger::V3ResultStoreStatsOf()
    const {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3()) {
        return std::nullopt;  // 非 v3 场:调用方走旧 artifact 口径
    }
    V3ResultStoreStats stats;
    std::error_code ec;
    const std::filesystem::path artifacts = session_dir() / "artifacts";
    if (!std::filesystem::exists(artifacts, ec)) {
        return stats;  // 仓还没开过(尚无超帽结果):0 枚如实
    }
    for (const auto& entry : std::filesystem::directory_iterator(artifacts, ec)) {
        if (ec) {
            break;
        }
        const std::string name = platform::PathToUtf8(entry.path().filename());
        if (name.rfind("res-", 0) != 0) {
            continue;
        }
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(entry.path(), size_ec);
        if (size_ec) {
            continue;
        }
        stats.total_bytes += static_cast<std::uint64_t>(size);
        if (entry.path().extension() == ".json") {
            ++stats.results;
        }
    }
    return stats;
}

// D3(§5.1.2):compact applied 后的内存换账投影。重读主卷(共享读,
// 不扰单写者)兼作 applied 行的持久化确认——验不过就不换,不拿内存
// 视图顶账;投影与 /resume 的 v3 分支同一套(EffectiveConversation-
// FromV3 公开共用),压缩后的实发形状与链引用天然一致。
std::expected<std::vector<api::Message>, std::string>
TrajectorySessionLedger::ProjectV3ContextHistory() const {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3()) {
        return std::unexpected("compact.swap.not_v3: 内存换账投影只认 v3 主卷");
    }
    const auto ledger = v3::ReadV3Ledger(impl_->active->directory.v3_stream_path());
    if (!ledger.has_value()) {
        return std::unexpected("compact.swap.ledger_unreadable: " + ledger.error());
    }
    trajectory::ReplayState projection_state;
    projection_state.effective_conversation =
        trajectory::EffectiveConversationFromV3(*ledger, v3::ProjectModelContext(*ledger));
    return ProjectHistoryFromReplay(projection_state);
}

std::uint64_t TrajectorySessionLedger::SpanEndSeq() {
    trajectory::TrajectoryRecorder* recorder = main();
    return recorder != nullptr ? recorder->next_seq() : 1;
}

// T12-A(V3-GAP-07 P0):会话级执行阻断的置位/查询。只对 v3 主账场生效;
// 幂等保留首因(第一次失败的原因最重要,后续重复置位不覆盖)。revision
// 取置位时账面 contextRevision——诊断与准入对表用,不参与放行判定
//(放行只有换场重建一条路)。
void TrajectorySessionLedger::BlockV3Execution(const std::string& reason) {
    if (impl_ == nullptr || !impl_->v3_books.has_value()) {
        return;  // 非 v3 场:no-op(v2 无此门)
    }
    if (impl_->v3_books->execution_blocked) {
        return;  // 已阻断:保留首因
    }
    impl_->v3_books->execution_blocked = true;
    impl_->v3_books->execution_block_reason = reason;
    impl_->v3_books->execution_block_revision =
        impl_->v3_books->writer != nullptr ? impl_->v3_books->writer->context().revision : 0;
    io_errors_.push_back("compact.execution_blocked:" + reason);
}

bool TrajectorySessionLedger::V3ExecutionBlocked() const {
    return impl_ != nullptr && impl_->v3_books.has_value() && impl_->v3_books->execution_blocked;
}

std::optional<std::string> TrajectorySessionLedger::OpenMainTurnId() const {
    // T12-C:只认"此刻在跑"的旗——active_main_turn_id 是粘账(收口后
    // 仍指向最近一只),拿它当 parent 就是伪造;idle 压缩如实给 nullopt。
    if (impl_ == nullptr || !impl_->v3_books.has_value() || !impl_->v3_books->main_turn_open) {
        return std::nullopt;
    }
    const std::string& turn_id = impl_->v3_books->active_main_turn_id;
    if (turn_id.empty()) {
        return std::nullopt;
    }
    return turn_id;
}

void TrajectorySessionLedger::PutUserCommand_(trajectory::EventKind kind, nlohmann::json payload) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::User;
    request.scope.origin = trajectory::Origin::ExternalUser;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        platform::LogSink::Instance().Error(
            "trajectory", std::string("command 落账失败: ") + receipt.error_code);
        return;
    }
    NotifyCommitted_();
}

std::string TrajectorySessionLedger::BeginCommand(const std::string& command_name,
                                                  const std::string& action_name,
                                                  const std::string& effect_class) {
    const std::string command_id = "cmd-" + std::to_string(++command_counter_);
    PutUserCommand_(trajectory::EventKind::ControlCommandRequested,
                    nlohmann::json{{"command_id", command_id},
                                   {"command_name", command_name},
                                   {"action_name", action_name},
                                   {"effect_class", effect_class}});
    return command_id;
}

void TrajectorySessionLedger::EndCommand(const std::string& command_id, bool ok,
                                         const std::string& reason) {
    nlohmann::json payload = nlohmann::json{{"command_id", command_id}};
    if (ok) {
        payload["status"] = "ok";
        PutUserCommand_(trajectory::EventKind::ControlCommandCompleted, std::move(payload));
        return;
    }
    payload["reason"] = reason.empty() ? "command_failed" : reason;
    PutUserCommand_(trajectory::EventKind::ControlCommandFailed, std::move(payload));
}

// ---------------------------------------------------------------------------
// P0-4:环境快照 / 排队账 / 容量与存储(§9.1/§5.5/§12.2)
// ---------------------------------------------------------------------------

namespace {

// §9.1 的平台静态材料:os/arch 按 compile target 报,locale/timezone 现读
// (读不出就空串,由 gaps 如实记账,不造假)。
std::string DetectOsName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string DetectArch() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return std::string();
#endif
}

std::string DetectLocale() {
    const char* current = std::setlocale(LC_ALL, nullptr);
    return current != nullptr ? current : std::string();
}

std::string DetectTimezone() {
#if defined(_LIBCPP_VERSION)
    // libc++(macOS 一系)未实现 C++20 tzdb——time_zone 类型都缺,编译期
    // 就炸(CI macos 腿首跑实证)。回落 C 接面:TZ 环境变量优先,否则
    // tzset 后 strftime %Z 拿时区缩写;拿不到空串,gaps 记"不可知",不猜。
    tzset();
    const char* env = std::getenv("TZ");
    if (env != nullptr && *env != '\0') {
        return std::string(env);
    }
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (local != nullptr) {
        char buf[64] = {0};
        if (std::strftime(buf, sizeof(buf), "%Z", local) > 0) {
            return std::string(buf);
        }
    }
    return std::string();
#else
    try {
        const std::chrono::time_zone* zone = std::chrono::current_zone();
        if (zone != nullptr) {
            return std::string(zone->name());
        }
    } catch (...) {
        // 无 tzdata 的机器:空串,gaps 里记"不可知",不猜。
    }
    return std::string();
#endif
}

}  // namespace

std::string TrajectorySessionLedger::CaptureEnvironment(const EnvironmentFacts& facts) {
    if (environment_captured_) {
        return std::string();  // 一场 run 一次,幂等
    }
    // T11-C / V3-GAP-06:v3 场落 session.environment.captured。取材件
    //(GatherGitStatus/BuildEnvironmentCapturePayload/DetermineReplayLevel)
    // 与 v2 同一台机器——快照正文/脱敏/缺口口径一份;差别的只是事件 kind
    // 与载荷键名(camelCase)。捕获时间即信封 timestamp(writer 发);resume
    // 换场后 environment_captured_ 复位,新场重采今天的——旧场的旧行一个
    // 字节不动,读旧档不拿今天环境补昨天事实。
    if (impl_ != nullptr && impl_->active != nullptr && impl_->active->is_v3()) {
        trajectory::BlobStore blobs(impl_->active->directory.artifacts_root());
        trajectory::EnvironmentSnapshotInput input;
        input.lubancode_version = impl_->lubancode_version;
        input.os_name = DetectOsName();
        input.arch = DetectArch();
        input.locale = DetectLocale();
        input.timezone = DetectTimezone();
        input.cwd = platform::PathToUtf8(std::filesystem::current_path());
        input.repository_root = impl_->workspace_root_text;
        input.git = trajectory::GatherGitStatus(impl_->workspace_root_text);
        input.provider = facts.provider;
        input.wire = facts.wire;
        input.model = facts.model;
        input.model_parameters = facts.model_parameters;
        if (!facts.system_prompt.empty()) {
            const auto prompt_ref =
                blobs.Store(facts.system_prompt, "text/markdown", trajectory::Durability::PowerLoss);
            if (prompt_ref.has_value()) {
                input.system_prompt_ref = *prompt_ref;
            }
        }
        input.toolset = facts.toolset;
        input.project_instruction_refs = facts.project_instruction_refs;
        input.loaded_skill_refs = facts.loaded_skill_refs;
        input.plugin_refs = facts.plugin_refs;
        input.config_snapshot_redacted = facts.config_snapshot_redacted;
        input.allowlisted_env = facts.allowlisted_env;

        const auto capture =
            trajectory::BuildEnvironmentCapturePayload(input, blobs, trajectory::Durability::PowerLoss);
        if (!capture.has_value()) {
            const std::string error = capture.error();
            io_errors_.push_back(error);
            platform::LogSink::Instance().Error("trajectory", "环境快照落盘失败: " + error);
            return error;
        }
        v3::EventDraft captured;
        captured.kind = v3::EventKindV3::SessionEnvironmentCaptured;
        captured.payload = nlohmann::json{
            {"snapshotRef", capture->event_payload.value("snapshot_ref", nlohmann::json())},
            {"replayLevel", capture->event_payload.value("replay_level", std::string())},
            {"gaps", capture->event_payload.value("gaps", nlohmann::json::array())},
            // 配置快照是调用方脱敏后的(BuildRedactedConfigSnapshot 的
            // 窄键集);redaction 状态如实声明,读取侧见 false 按缺件处理。
            {"configRedacted", facts.config_snapshot_redacted.is_object()}};
        const auto receipt = impl_->active->v3_main->AppendEvent(
            std::move(captured), trajectory::Durability::PowerLoss);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            const std::string error = "session.environment.captured:" + receipt.error_code;
            io_errors_.push_back(error);
            return receipt.error_code;
        }
        environment_captured_ = true;
        return std::string();
    }
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return "trajectory.no_recorder";
    }
    trajectory::BlobStore blobs(impl_->active->directory.artifacts_root());
    trajectory::EnvironmentSnapshotInput input;
    input.lubancode_version = impl_->lubancode_version;
    input.os_name = DetectOsName();
    input.arch = DetectArch();
    input.locale = DetectLocale();
    input.timezone = DetectTimezone();
    input.cwd = platform::PathToUtf8(std::filesystem::current_path());
    // §3.2:仓库根从账本开张时的 workspace_root 递进(空 = 启动 cwd,如实
    // 记 in_repo=false 的缺口)。
    input.repository_root = impl_->workspace_root_text;
    input.git = trajectory::GatherGitStatus(impl_->workspace_root_text);
    input.provider = facts.provider;
    input.wire = facts.wire;
    input.model = facts.model;
    input.model_parameters = facts.model_parameters;
    if (!facts.system_prompt.empty()) {
        const auto prompt_ref =
            blobs.Store(facts.system_prompt, "text/markdown", trajectory::Durability::PowerLoss);
        if (prompt_ref.has_value()) {
            input.system_prompt_ref = *prompt_ref;
        }
    }
    input.toolset = facts.toolset;
    input.project_instruction_refs = facts.project_instruction_refs;
    input.loaded_skill_refs = facts.loaded_skill_refs;
    input.plugin_refs = facts.plugin_refs;
    input.config_snapshot_redacted = facts.config_snapshot_redacted;
    input.allowlisted_env = facts.allowlisted_env;

    const auto capture =
        trajectory::BuildEnvironmentCapturePayload(input, blobs, trajectory::Durability::PowerLoss);
    if (!capture.has_value()) {
        const std::string error = capture.error();
        io_errors_.push_back(error);
        platform::LogSink::Instance().Error("trajectory", "环境快照落盘失败: " + error);
        return error;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::RunEnvironmentCaptured;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::RecoveryRuntime;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Metadata;
    request.payload = capture->event_payload;
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::PowerLoss);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        io_errors_.push_back("run.environment.captured:" + receipt.error_code);
        return receipt.error_code;
    }
    environment_captured_ = true;
    return std::string();
}

void TrajectorySessionLedger::NoteQueueEnqueued(const std::string& item_id,
                                                const std::string& target_label,
                                                const std::string& reason) {
    nlohmann::json payload{{"item_id", item_id}, {"input_id", item_id}};
    if (!target_label.empty()) {
        payload["enqueue_reason"] = target_label;
    } else if (!reason.empty()) {
        payload["enqueue_reason"] = reason;
    }
    PutUserCommand_(trajectory::EventKind::ControlQueueItemEnqueued, std::move(payload));
}

void TrajectorySessionLedger::NoteQueueDequeued(const std::string& item_id, const std::string& reason) {
    // dequeue 是宿主泵的活(§5.5:宿主落下状态变更,actor=host),不冒充
    // 用户动作。
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    nlohmann::json payload{{"item_id", item_id}, {"input_id", item_id}};
    if (!reason.empty()) {
        payload["reason"] = reason;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::ControlQueueItemDequeued;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::ScheduledHost;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        io_errors_.push_back("control.queue.item.dequeued:" + receipt.error_code);
    }
}

void TrajectorySessionLedger::NoteQueueCancelled(const std::string& item_id, const std::string& reason) {
    PutUserCommand_(trajectory::EventKind::ControlQueueItemCancelled,
                    nlohmann::json{{"item_id", item_id},
                                   {"reason", reason.empty() ? "user_removed" : reason}});
}

void TrajectorySessionLedger::NoteQueueExpired(const std::string& item_id, const std::string& reason) {
    // 过期也是宿主判的(泵的防死循环闸),actor=host。
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    nlohmann::json payload{{"item_id", item_id}};
    if (!reason.empty()) {
        payload["reason"] = reason;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::ControlQueueItemExpired;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::ScheduledHost;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        io_errors_.push_back("control.queue.item.expired:" + receipt.error_code);
    }
}

bool TrajectorySessionLedger::StorageAvailable() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return false;
    }
    return trajectory::HasDiskReserve(impl_->active->directory.session_dir(),
                                      kJournalEmergencyReserveBytes);
}

trajectory::WorkspaceUsageReport TrajectorySessionLedger::WorkspaceUsage() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return trajectory::WorkspaceUsageReport{};
    }
    const std::filesystem::path workspace_dir = impl_->active->directory.workspace_dir();
    // 账本制:目录名是门牌不是 key,报表钉真钥匙。
    return trajectory::ScanWorkspaceUsage(workspace_dir / "sessions", workspace_key());
}

trajectory::WorkspaceDoctorReport TrajectorySessionLedger::BuildDoctorReport() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return trajectory::WorkspaceDoctorReport{};
    }
    const trajectory::TrajectoryDirectory& directory = impl_->active->directory;
    return trajectory::BuildWorkspaceDoctorReport(
        directory.workspace_dir().parent_path(), directory.workspace_dir(), workspace_key(),
        impl_->active->session_id(), io_errors_);
}

std::vector<std::string> TrajectorySessionLedger::recent_io_errors() const {
    return io_errors_;
}

// ---------------------------------------------------------------------------
// RecordSelectionController
// ---------------------------------------------------------------------------

RecordSelectionController::RecordSelectionController(TrajectorySessionLedger& ledger) : ledger_(ledger) {}

std::string RecordSelectionController::Put_(trajectory::EventKind kind, nlohmann::json payload) {
    trajectory::TrajectoryRecorder* recorder = ledger_.main();
    if (recorder == nullptr) {
        return "trajectory.no_recorder";
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = recorder->base_scope();
    // 真人敲 slash:actor=user/origin=external_user(§5.5);annotation
    // 不冒充 conversation user(training_policy=exclude)。
    request.scope.actor = trajectory::Actor::User;
    request.scope.origin = trajectory::Origin::ExternalUser;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        return receipt.error_code;
    }
    if (kind == trajectory::EventKind::RecordSelectionStarted) {
        start_event_hash_ = receipt.event_hash;
    }
    return std::string();
}

std::string RecordSelectionController::Start(const std::string& name, const std::string& goal,
                                             const std::vector<std::string>& variables,
                                             const std::string& acceptance) {
    if (active()) {
        return "record.already_active";
    }
    record_id_ = "record-" + std::to_string(++selection_counter_);
    paused_ = false;
    trajectory::TrajectoryRecorder* recorder = ledger_.main();
    nlohmann::json payload = nlohmann::json{{"record_id", record_id_},
                                            {"scope", "causal_tree"}};
    if (!name.empty()) {
        payload["goal"] = name;  // 名字即选段标注(§14.3:annotation,不进对话)
    }
    if (!goal.empty()) {
        payload["goal"] = goal;
    }
    if (!variables.empty()) {
        payload["variables"] = variables;
    }
    if (!acceptance.empty()) {
        payload["acceptance"] = acceptance;
    }
    if (recorder != nullptr && !recorder->last_event_hash().empty()) {
        payload["start_event_ref"] = nlohmann::json{{"event_hash", recorder->last_event_hash()}};
    }
    return Put_(trajectory::EventKind::RecordSelectionStarted, std::move(payload));
}

std::string RecordSelectionController::Pause() {
    if (!active() || paused_) {
        return "record.not_active";
    }
    const std::string error =
        Put_(trajectory::EventKind::RecordSelectionPaused, nlohmann::json{{"record_id", record_id_}});
    if (error.empty()) {
        paused_ = true;
    }
    return error;
}

std::string RecordSelectionController::Resume() {
    if (!active() || !paused_) {
        return "record.not_active";
    }
    const std::string error =
        Put_(trajectory::EventKind::RecordSelectionResumed, nlohmann::json{{"record_id", record_id_}});
    if (error.empty()) {
        paused_ = false;
    }
    return error;
}

std::string RecordSelectionController::Note(const std::string& text) {
    if (!active()) {
        return "record.not_active";
    }
    nlohmann::json note;
    note["text"] = text;
    return Put_(trajectory::EventKind::RecordSelectionNoteAdded,
                nlohmann::json{{"record_id", record_id_}, {"note_ref", std::move(note)}});
}

std::string RecordSelectionController::Stop(const std::string& verification) {
    if (!active()) {
        return "record.not_active";
    }
    trajectory::TrajectoryRecorder* recorder = ledger_.main();
    nlohmann::json payload = nlohmann::json{{"record_id", record_id_}};
    // 选段只圈 canonical 事件段:起点 hash 到当前末 hash 的整段(暂停区间
    // 仍在 Journal,不造事实缺口,§14.3)。
    payload["included_spans"] = nlohmann::json::array({nlohmann::json{
        {"start_event_hash", start_event_hash_},
        {"end_event_hash", recorder != nullptr ? recorder->last_event_hash() : std::string()}}});
    if (recorder != nullptr && !recorder->last_event_hash().empty()) {
        payload["source_terminal_hashes"] = nlohmann::json::array({recorder->last_event_hash()});
    }
    if (!verification.empty()) {
        // 口述"已验过"只是 claimed verification,不是 fresh evidence(§14.3)。
        payload["claimed_verification"] = verification;
    }
    const std::string error =
        Put_(trajectory::EventKind::RecordSelectionCompleted, std::move(payload));
    if (error.empty()) {
        record_id_.clear();
        paused_ = false;
    }
    return error;
}

std::string RecordSelectionController::Cancel() {
    if (!active()) {
        return "record.not_active";
    }
    const std::string error = Put_(trajectory::EventKind::RecordSelectionCancelled,
                                   nlohmann::json{{"record_id", record_id_}, {"reason", "user_cancel"}});
    if (error.empty()) {
        record_id_.clear();
        paused_ = false;
    }
    return error;
}

const std::string& TrajectorySessionLedger::session_id() const {
    static const std::string empty;
    return impl_ != nullptr && impl_->active != nullptr ? impl_->active->session_id() : empty;
}

std::filesystem::path TrajectorySessionLedger::session_dir() const {
    static const std::filesystem::path empty;
    // ActiveSession::session_dir() 按值回(const ref 会接到临时上)。
    return impl_ != nullptr && impl_->active != nullptr ? impl_->active->session_dir() : empty;
}

std::string TrajectorySessionLedger::workspace_key() const {
    // v3 场 main(v2 recorder)恒空、身份在 v3_main 与内存 manifest
    //(ActiveSession 头注:认 active 一律先看 v3_main)。这条只认 main 的
    // 读面曾让默认 v3 场恒拿空 key——/resume 的 Cwd 范围查询见空 key 直接
    // 空手,列表 0/0,盘上档案全在也列不出(Resume 接入 v3 单 R1 根因)。
    if (impl_ != nullptr && impl_->active != nullptr) {
        if (impl_->active->is_v3()) {
            return impl_->active->manifest.workspace_key;
        }
        if (impl_->active->main.has_value()) {
            return impl_->active->main->base_scope().workspace_key;
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// P0-2:会话读面与 workspace 管理面(命令/app-server 共用)
// ---------------------------------------------------------------------------

std::filesystem::path TrajectorySessionLedger::workspaces_root() const {
    static const std::filesystem::path empty;
    return impl_ != nullptr ? impl_->workspaces_root : empty;
}

trajectory::SessionIndexPage TrajectorySessionLedger::ListWorkspaceSessions(
    const trajectory::SessionIndexQuery& query) const {
    trajectory::SessionIndexQuery effective = query;
    if (!effective.all_workspaces && effective.current_workspace_key.empty()) {
        effective.current_workspace_key = workspace_key();
    }
    return trajectory::QueryWorkspaceSessions(workspaces_root(), effective);
}

std::vector<trajectory::PromptHistoryLine> TrajectorySessionLedger::ReadPromptHistory(
    std::size_t max_lines) const {
    return trajectory::ReadWorkspacePromptHistory(workspaces_root(), workspace_key(), max_lines);
}

std::vector<std::string> TrajectorySessionLedger::MakeTranscriptExcerpt(const std::string& target_id,
                                                                        std::size_t max_half) const {
    if (impl_ != nullptr && impl_->active != nullptr && target_id == this->session_id()) {
        return trajectory::MakeSessionTranscriptExcerpt(impl_->active->session_dir(), max_half);
    }
    // 别的场次:经索引定位目录(跨 workspace 也找得回)。
    trajectory::SessionIndexQuery query;
    query.all_workspaces = true;
    const auto page = trajectory::QueryWorkspaceSessions(workspaces_root(), query);
    for (const auto& summary : page.entries) {
        if (summary.session_id == target_id) {
            return trajectory::MakeSessionTranscriptExcerpt(
                tools::Utf8ToPath(summary.session_dir), max_half);
        }
    }
    return {};
}

std::optional<RestoredTranscriptPage> TrajectorySessionLedger::ReadTranscriptPage(
    const std::string& target_id, const std::optional<std::uint64_t>& before_seq,
    const std::optional<std::uint64_t>& after_seq, std::size_t max_lines) const {
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    // 定位源目录(活场直取,冷场经索引跨 workspace 找),再认 v3 流。
    std::optional<std::filesystem::path> session_dir;
    if (impl_->active != nullptr && target_id == this->session_id()) {
        session_dir = impl_->active->session_dir();
    } else {
        trajectory::SessionIndexQuery query;
        query.all_workspaces = true;
        const auto page = trajectory::QueryWorkspaceSessions(workspaces_root(), query);
        for (const auto& summary : page.entries) {
            if (summary.session_id == target_id) {
                session_dir = tools::Utf8ToPath(summary.session_dir);
                break;
            }
        }
    }
    if (!session_dir.has_value()) {
        return std::nullopt;  // 场找不着:调用方走 v2 老路(那里给空表)
    }
    const auto stream = FindV3HistoryStream(*session_dir);
    if (!stream.has_value()) {
        return std::nullopt;  // v2 场:照旧头尾截断,一字不变
    }
    // 渲染行缓存:场没换、字节数没变,直接切页(不重读不重投)。
    std::error_code size_ec;
    const std::uintmax_t bytes = std::filesystem::file_size(*stream, size_ec);
    if (size_ec) {
        return std::nullopt;
    }
    if (!impl_->transcript_cache.has_value() || impl_->transcript_cache->stream != *stream ||
        impl_->transcript_cache->bytes != bytes) {
        const RestoredHistoryView view = ProjectRestoredHistory(*stream);
        Impl::TranscriptCache cache;
        cache.stream = *stream;
        cache.bytes = bytes;
        cache.lines = RenderRestoredTranscriptLines(view);
        impl_->transcript_cache = std::move(cache);
    }
    return SliceRestoredTranscript(impl_->transcript_cache->lines, before_seq, after_seq, max_lines);
}

std::string TrajectorySessionLedger::ArchiveSessionInWorkspace(const std::string& session_id) const {
    if (impl_ == nullptr) {
        return "session.open_failed: 账本未开";
    }
    const auto outcome = trajectory::ArchiveSessionDir(impl_->manager->workspace_dir(), session_id,
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::system_clock::now()
                                                               .time_since_epoch())
                                                           .count());
    return outcome.ok() ? std::string() : outcome.error_code + ": " + outcome.message;
}

std::string TrajectorySessionLedger::UnarchiveSessionInWorkspace(const std::string& session_id) const {
    if (impl_ == nullptr) {
        return "session.open_failed: 账本未开";
    }
    const auto outcome = trajectory::UnarchiveSessionDir(
        impl_->manager->workspace_dir(), session_id,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return outcome.ok() ? std::string() : outcome.error_code + ": " + outcome.message;
}

std::string TrajectorySessionLedger::DeleteSessionInWorkspace(const std::string& session_id,
                                                              const std::string& reason) const {
    if (impl_ == nullptr) {
        return "session.open_failed: 账本未开";
    }
    if (session_id == this->session_id()) {
        return "session.delete_active: 当前场先 /exit 封口再删";
    }
    const auto outcome = trajectory::DeleteSessionDir(
        impl_->manager->workspace_dir(), session_id, reason,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return outcome.ok() ? std::string() : outcome.error_code + ": " + outcome.message;
}

void TrajectorySessionLedger::RecordTitleChanged(const std::string& title, const std::string& old_title) {
    // v3 场:session.title.applied 落真账(beta.1 反弹二——此前 PutUserCommand_
    // 只认 v2 main,v3 场 /title 是 no-op,标题列恒空)。T11-A 起手动来源
    // 不再伪造 titleGenerationId——manual 行没有生成身份,伪造即造假;
    // 自动精炼的生成身份由 RecordGeneratedTitleApplied 携真号进来。
    if (impl_ != nullptr && impl_->active != nullptr && impl_->active->is_v3()) {
        v3::EventDraft applied;
        applied.kind = v3::EventKindV3::SessionTitleApplied;
        // 事实提交族(同 state.goal.applied):RequiredStatusForKind 查表无
        // 此 kind,不带 status 才过校验——§2.2 的 .applied→done 是文档语义,
        // 校验按穷举表走,带 status 反被"不携带"分支拒。
        applied.payload = nlohmann::json{{"title", title}, {"source", "manual"}};
        if (!old_title.empty()) {
            applied.payload["oldTitle"] = old_title;
        }
        const auto receipt =
            impl_->active->v3_main->AppendEvent(std::move(applied), trajectory::Durability::PowerLoss);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            platform::LogSink::Instance().Error(
                "trajectory", std::string("v3 标题落账失败: ") + receipt.error_code);
        }
        return;
    }
    nlohmann::json payload = nlohmann::json{{"title", title}};
    if (!old_title.empty()) {
        payload["old_title"] = old_title;
    }
    // /title 是真人敲的命令账;自动精炼采纳的标题同走这枚事件(actor
    // 如实分流留给后续批次,先把"标题变过"落成可回放事实)。
    PutUserCommand_(trajectory::EventKind::ControlTitleChanged, std::move(payload));
}

bool TrajectorySessionLedger::RecordContextWindowChanged(std::size_t window_tokens,
                                                          std::size_t old_window_tokens,
                                                          const std::string& provider,
                                                          const std::string& model,
                                                          const std::string& source) {
    if (impl_ == nullptr || impl_->active == nullptr || window_tokens == 0) {
        return false;
    }
    // v3 场:session.context_window.applied(与 session.title.applied 同族的
    // 控制状态事实,camelCase 载荷)。提交失败如实回 false——调用方区分
    // "已持久化可恢复"与"仅本次生效",不装已保存。
    if (impl_->active->is_v3()) {
        v3::EventDraft applied;
        applied.kind = v3::EventKindV3::SessionContextWindowApplied;
        applied.payload = nlohmann::json{{"contextWindow", window_tokens}};
        if (old_window_tokens > 0) {
            applied.payload["oldContextWindow"] = old_window_tokens;
        }
        if (!provider.empty()) {
            applied.payload["provider"] = provider;
        }
        if (!model.empty()) {
            applied.payload["model"] = model;
        }
        if (!source.empty()) {
            applied.payload["source"] = source;
        }
        const auto receipt =
            impl_->active->v3_main->AppendEvent(std::move(applied), trajectory::Durability::PowerLoss);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            platform::LogSink::Instance().Error(
                "trajectory", std::string("v3 窗口预算落账失败: ") + receipt.error_code);
            return false;
        }
        NotifyCommitted_();
        return true;
    }
    // v2 场:control.context_window.changed(值按事件合同写成十进制字符串,
    // 折叠侧 ParseContextWindowPayload 认得)。身份与来路随行,旧读者按
    // 未知字段另账处理。走独立 Record 而非 PutUserCommand_:回执要如实交
    // 给调用方(写不住不能只打日志了事)。
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return false;
    }
    nlohmann::json payload = nlohmann::json{{"context_window", std::to_string(window_tokens)}};
    if (old_window_tokens > 0) {
        payload["old_context_window"] = std::to_string(old_window_tokens);
    }
    if (!provider.empty()) {
        payload["provider"] = provider;
    }
    if (!model.empty()) {
        payload["model"] = model;
    }
    if (!source.empty()) {
        payload["source"] = source;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::ControlContextWindowChanged;
    request.scope = recorder->base_scope();
    // 手动改(/context、面板)是真人动作;开场快照与恢复裁决是宿主动作,
    // actor 如实分流,不把系统行为记成用户输入。
    const bool manual = source.empty() || source == "manual";
    request.scope.actor = manual ? trajectory::Actor::User : trajectory::Actor::Host;
    request.scope.origin =
        manual ? trajectory::Origin::ExternalUser : trajectory::Origin::ScheduledHost;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::PowerLoss);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        platform::LogSink::Instance().Error(
            "trajectory", std::string("窗口预算落账失败: ") + receipt.error_code);
        return false;
    }
    NotifyCommitted_();
    return true;
}

// ---- T11-A:标题来源分家的 v3 其余三路(声明见 trajectory_session.hpp)。

void TrajectorySessionLedger::AppendTitleAppliedV3_(const std::string& title,
                                                    const std::string& old_title,
                                                    std::string_view source,
                                                    const std::string* title_generation_id) {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3()) {
        // v2 场无 source 分路:local/generated 与 manual 同归
        // control.title.changed(老行为一字不动——v2 的标题账不认来源)。
        nlohmann::json payload = nlohmann::json{{"title", title}};
        if (!old_title.empty()) {
            payload["old_title"] = old_title;
        }
        PutUserCommand_(trajectory::EventKind::ControlTitleChanged, std::move(payload));
        return;
    }
    v3::EventDraft applied;
    applied.kind = v3::EventKindV3::SessionTitleApplied;
    if (title_generation_id != nullptr) {
        applied.title_generation_id = *title_generation_id;
    }
    applied.payload = nlohmann::json{{"title", title}, {"source", source}};
    if (!old_title.empty()) {
        applied.payload["oldTitle"] = old_title;
    }
    const auto receipt =
        impl_->active->v3_main->AppendEvent(std::move(applied), trajectory::Durability::PowerLoss);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        platform::LogSink::Instance().Error("trajectory",
                                            std::string("v3 标题落账失败: ") + receipt.error_code);
    }
}

void TrajectorySessionLedger::RecordLocalTitleApplied(const std::string& title,
                                                      const std::string& old_title) {
    AppendTitleAppliedV3_(title, old_title, "local", nullptr);
}

void TrajectorySessionLedger::RecordGeneratedTitleApplied(const std::string& title_generation_id,
                                                          const std::string& title,
                                                          const std::string& old_title) {
    AppendTitleAppliedV3_(title, old_title, "generated", &title_generation_id);
}

void TrajectorySessionLedger::RecordTitleRequested(const std::string& title_generation_id,
                                                   const std::string& model,
                                                   const std::string& provider) {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3()) {
        return;
    }
    v3::EventDraft requested;
    requested.kind = v3::EventKindV3::TitleRequested;
    requested.title_generation_id = title_generation_id;
    nlohmann::json payload{{"task", "session_title_refine"}};
    if (!model.empty()) {
        payload["model"] = model;
    }
    if (!provider.empty()) {
        payload["provider"] = provider;
    }
    requested.payload = std::move(payload);
    const auto receipt = impl_->active->v3_main->AppendEvent(std::move(requested),
                                                             trajectory::Durability::ProcessCrash);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        platform::LogSink::Instance().Error("trajectory",
                                            std::string("v3 标题请求落账失败: ") + receipt.error_code);
    }
}

void TrajectorySessionLedger::RecordTitleExtracted(const std::string& title_generation_id,
                                                   const std::string& title) {
    if (impl_ == nullptr || impl_->active == nullptr || !impl_->active->is_v3()) {
        return;
    }
    v3::EventDraft extracted;
    extracted.kind = v3::EventKindV3::TitleExtracted;
    extracted.title_generation_id = title_generation_id;
    extracted.payload = nlohmann::json{{"title", title}};
    const auto receipt = impl_->active->v3_main->AppendEvent(std::move(extracted),
                                                             trajectory::Durability::ProcessCrash);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        platform::LogSink::Instance().Error("trajectory",
                                            std::string("v3 标题提取落账失败: ") + receipt.error_code);
    }
}

void TrajectorySessionLedger::RecordModeChanged(const std::string& mode, const std::string& reason,
                                                const std::string& old_mode) {
    nlohmann::json payload = nlohmann::json{{"mode", mode}, {"reason", reason}};
    if (!old_mode.empty()) {
        payload["old_mode"] = old_mode;
    }
    PutControl_(trajectory::EventKind::ControlModeChanged, std::move(payload));
}

}  // namespace lubancode::runtime
