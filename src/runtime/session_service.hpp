// SessionService(AppServer 接入 Session v3 第一棒:统一服务入口与身份)。
//
// 三端(终端交互、one-shot、app-server)共用的会话服务入口——会话的
// 创建/恢复/关闭、输入接纳与操作去重、typed 域命令执行都从这条路走,
// 协议宿主与终端不再各养一份调度(设计单 §一职责表、§十.1 本棒合同:
// "先收敛 SessionFactory/SessionRuntime/CommandService、输入队列、操作
// 去重和单写者提交;终端、one-shot、AppServer 调同一路")。
//
// 三端接线形态(本棒是搬运收敛,不是重写):
//   - app-server:ThreadRecord 持本服务(HandleThreadStart 建、
//     HandleThreadStop 封、turn/start 经 SubmitInput 接纳、typed 域命令
//     经 ExecuteDomainCommand 执行);
//   - one-shot:AskOnce 持本服务(单发场开张/问题接纳/收口全走服务);
//   - 终端:控制器按值持 SessionRuntime(成员序即寿命序,不换所有权
//     形态),开张装配走 BuildRuntimeOptions 同一折算,收口走
//     CloseRuntime 同一口;输入排队(slash/steer 泵)与终端命令注册表
//     留在 cli 侧,归后续棒(§十.3"输入与恢复闭环")接进服务队列。
//
// v3 写侧开关(v3/session_switch.hpp 接线点 1)不在本层另设闸:开关在
// SessionManager 建场时二选一——开 = V3Writer 首行 system 的 v3 流,
// 关 = 现行 v2 main.jsonl,一字不动。resume 走 ResumeAsNew 两路分派
// (v2 源折叠回放 / v3 源链投影),同样在账本侧,本层只递合同。
//
// 依赖铁律同 SessionRuntime:不 include cli/app/frontend,不读 stdin、
// 不写 stdout/stderr——成败用返回值交账,人话由前端印。
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "api/types.hpp"
#include "approval_mode.hpp"
#include "runtime/command.hpp"
#include "runtime/session_runtime.hpp"
#include "workspace/identity.hpp"

namespace lubancode::runtime {

namespace goal {
class GoalCoordinator;
}
namespace loop {
class LoopScheduler;
}

// ---------------------------------------------------------------------------
// 开张请求(三端同一形状;各端只填自己真有的料,缺省即旧行为)
// ---------------------------------------------------------------------------

struct SessionLaunchRequest {
    // 身份裁决起点(UTF-8 文本)。空 = 进程 current_path。app-server 递
    // 前端指定的 cwd,终端/one-shot 递启动 cwd——四级裁决(commondir→
    // marker→config→cwd)在服务里做一次,不再三处各写一份。
    std::string cwd_utf8;
    // 测试/特殊装配显式递冻好的身份;有值时跳过 cwd 裁决(与
    // SessionRuntime::Options 的语义同款)。
    std::optional<workspace::WorkspaceIdentity> workspace_identity;
    std::string lubancode_version;

    // meta.wire 与会话 id 时间戳底子:终端递,app-server/单发沿旧例留空
    // (start_ts 不自动代填——空就是旧行为)。
    std::string wire_name;
    std::string start_ts;

    ApprovalMode approval_mode = ApprovalMode::Default;

    // --continue 启动路(§10.4):开 start_reason=resume 的新场;source
    // 空 = 取本 workspace 最近一场可恢复的,没有就回落普通开张。
    bool resume_at_launch = false;
    std::string resume_source_session_id;

    // 单发场(单发轨迹断档单):main run 记 run_kind=one_shot,resume
    // 候选排除;manifest.launch_cwd 单发给值,终端/app-server 沿旧空。
    bool one_shot = false;
    std::string launch_cwd;
    trajectory::TrainingPolicy training_policy = trajectory::TrainingPolicy::Metadata;

    // 唯一持久化根:空 = <home>/.lubancode/workspaces(生产默认);app-server
    // 与测试注入。
    std::filesystem::path workspaces_root;

    // v3 场(开关开时)的首行基础 system;空串合法 = 建场时还不知道最终
    // system,第一次模型请求带真 system 时走 §4.3 三步切换。三端现行都
    // 递空,字段立在这让服务成为完整的开张入口。
    std::string v3_system_content;
};

// ---------------------------------------------------------------------------
// SessionService:一场会话的服务柄
// ---------------------------------------------------------------------------

class SessionService {
public:
    // 开张:身份裁决 + SessionRuntime 装配 + 账本打开。开不出账
    // runtime() 为空、launch_error() 有说明——调用方须让会话启动失败,
    // 不回退旧写口(§十七失败合同)。
    explicit SessionService(SessionLaunchRequest request);
    // 不自动封口:各端显式 Close(reason)(terminal "exit"/app-server
    // "thread_stop"/单发 "exit",reason 是现行口径的合同)。
    ~SessionService();

    SessionService(const SessionService&) = delete;
    SessionService& operator=(const SessionService&) = delete;

    // ---- 会话真账宿主 -------------------------------------------------------
    // 空 = 开张失败(见 launch_error)。
    SessionRuntime* runtime() { return runtime_.get(); }
    const SessionRuntime* runtime() const { return runtime_.get(); }
    // 账本捷径(runtime 为空时这里也是空)。
    TrajectorySessionLedger* trajectory();
    const TrajectorySessionLedger* trajectory() const;
    const std::string& launch_error() const { return launch_error_; }
    // 这场是不是 v3 写侧(建场时开关二选一的结果;v2 场恒 false)。
    bool v3_format() const;

    // ---- 输入接纳(§四 input/submit 语义的最小服务面) ----------------------
    struct InputRequest {
        // 幂等键(§4.2:建议所有业务写命令带;超时重发仍用同一个)。空 =
        // 不去重,每发必纳(协议 1.2 面没有键,沿这条;2.0 开面后前端递)。
        std::string client_operation_id;
        std::string text;
        std::vector<api::ImageBlock> images;
    };

    // 接纳回执:accepted/duplicate/conflict 三态。duplicate 时 operation_id/
    // input_id/payload_hash 是首发的原值(同键同载荷返回原操作);conflict
    // 时 error_code=operation_conflict(同键不同载荷),不入队。
    struct InputReceipt {
        bool accepted = false;   // 本次接纳为新操作(已落账入队)
        bool duplicate = false;  // 同键同载荷重发:返回原回执,不重复接纳
        std::string error_code;  // "operation_conflict" / "queue_full" / 空
        std::string operation_id;  // 服务发号(op-<n>)
        std::string input_id;      // 服务发号(in-<n>)
        std::string payload_hash;  // 规范载荷的 SHA-256
    };

    // 提交输入:单写者串行(会话锁内)按 §4.2 次序走——先落操作台账
    // (operations.jsonl,先账),后入队、后出回执(后回执)。崩溃窗口
    // 里账已落而回执未达时,同键重发命中台账,不重复接纳。
    InputReceipt SubmitInput(const InputRequest& input);

    // 泵侧消费:队首取出(FIFO;每端自己的回合泵调)。空 = 没有待处理输入。
    struct QueuedInput {
        std::string text;
        std::vector<api::ImageBlock> images;
        std::string operation_id;  // 接纳时的操作号(与台账对账)
    };
    std::optional<QueuedInput> PopPendingInput();
    std::size_t pending_input_count() const;

    // 规范载荷串(幂等键比对的底):text + 图片字段 US('\x1f')定界拼接。
    // 公开给对照测试:同一操作走 CLI 路与服务路,hash 必须同源。
    static std::string CanonicalInputPayload(const InputRequest& input);

    // ---- typed 域命令(goal/loop/plan;CommandService 共用) ----------------
    // command_label 是落账用的命令名(app-server 递协议方法名,与现行
    // 轨迹 command 事件逐字节同源);coordinator/scheduler 由调用方持有
    // (app-server 按 thread 各一本),空指针 = 该域未装配,回稳定禁用码。
    ClientReceipt ExecuteDomainCommand(const std::string& command_label, const ClientCommand& command,
                                       goal::GoalCoordinator* goal_coordinator,
                                       loop::LoopScheduler* loop_scheduler,
                                       const std::string& cwd_identity, std::int64_t now_ms);

    // ---- 关闭 ----------------------------------------------------------------
    // 封口(session.ended + session.json closed;v3 场按账面事实收口)。
    // reason 空间沿用现行口径:exit/thread_stop/workspace_switch。重复
    // 调用按账本 Close 的现行语义(no_active_session 类错误码如实返回)。
    trajectory::CloseOutcome Close(const std::string& reason);

    // ---- 三端共用的开张/收口折算 ---------------------------------------------
    // cwd/身份/版本 → SessionRuntime::Options(终端按值持 runtime 的那条
    // 路:拿这份 Options 现场构造,与持本服务的两端走同一折算)。
    static SessionRuntime::Options BuildRuntimeOptions(const SessionLaunchRequest& request);

    // 收口的共用薄封(终端那条路用):trajectory 为空时按现行口径给
    // no_active_session 错误码,不崩。
    static trajectory::CloseOutcome CloseRuntime(SessionRuntime& runtime, const std::string& reason);

    // 输入队列的容量上限(资源上限仍可拒绝,§4.1):防无界排队。
    static constexpr std::size_t kMaxPendingInputs = 64;

private:
    // 操作台账(operations.jsonl)的内存镜像:clientOperationId -> 首发回执。
    struct AcceptedOperation {
        std::string operation_id;
        std::string input_id;
        std::string payload_hash;
    };

    // 开张成功后装载:本场的操作台账若在(防御);resume-at-launch 的
    // 直接来源场的台账单跳种进来——新 sessionId 不洗掉旧意图(§4.2
    // "恢复沿来源链识别原键,不能因新 sessionId 又执行一次旧意图")。
    void SeedOperationLedger();

    std::unique_ptr<SessionRuntime> runtime_;
    std::string launch_error_;
    bool v3_format_ = false;
    // resume-at-launch 解析出的直接来源场(空 = 非恢复场)。
    std::string resume_source_session_id_;

    // 单写者提交:操作台账落盘、去重表、输入队列共用一把会话锁。
    mutable std::mutex commit_mutex_;
    std::unordered_map<std::string, AcceptedOperation> operations_;
    std::deque<QueuedInput> pending_inputs_;
    std::uint64_t operation_counter_ = 0;
    // 台账文件句柄(开张成功即持,Close 后拒写)。
    std::optional<std::filesystem::path> operations_path_;
    class OperationsFile;
    std::unique_ptr<OperationsFile> operations_file_;
};

}  // namespace lubancode::runtime
