// CommandService(显示系统剥离单第七步:拆命令服务)。
//
// ClientCommand 的执行体:typed API 在这层兑现,slash 字符串在终端适配
// 层翻成命令(单子"五、slash 命令退回终端适配层")。本步先立 GUI 三条
// 必经路(/model、/resume、审批回答),provider/language/worktree/memory/
// compact/export 分批接——ClientCommandKind 的全集合同已立在 command.hpp,
// 这里逐枚兑现,没兑现的回 unsupported 错误码,不冒充成功。
//
// 三条路的形状:
//   - SetModel:query 出模型清单(ListModels 走注入的 fetcher,GUI 开下拉
//     框、终端开 ChoiceMenu),SetModel 提交选定值——改会话模型、应用目录
//     条目(default_think/context_window/base_instructions)、可选写回配置
//     文件(写回是显式一笔,不藏在切换里追问)。返回值带全清单与当前项,
//     前端自己排版。
//   - ResumeThread:query 出最近若干场(ListSessions),Resume 提交目标
//     (id 或列表序号)——回放历史、接管存档、标题/压缩序号接旧账。回放
//     出的历史与摘要返回给前端,终端照旧重绘,GUI 自己铺。
//   - ResolveApproval/AnswerQuestion:转发 InteractionBroker(四态决定、
//     迟到回答 stale)。
//
// 依赖铁律:合同头 + agent/config/tools 的内核件,不 include cli/app/
// frontend,不打印——结果结构化交账,人话由前端印。

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/model_router.hpp"
#include "agent/session_store.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "runtime/command.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/interaction_broker.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/session_runtime.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// SetModel
// ---------------------------------------------------------------------------

// 模型清单的一项(前端下拉框/菜单的原材料;display 与 current 不拼文案)。
struct ModelListEntry {
    std::string id;           // API 模型名(切换用这个)
    std::string display_name; // 展示名(API 给的;可空)
    bool current = false;     // 是不是会话当前模型
};

struct ModelQueryResult {
    std::vector<ModelListEntry> models;
    std::string current_model;
    bool fetch_failed = false;
    std::string fetch_error;      // fetch_failed 时的人话线索
    std::string config_file_path; // 可写回的配置文件(空 = 没有)
    // 三角色路由表(模型分工):非空时前端可另开一页;结构与
    // agent::ModelRouteTable 同形,这里直接带 JSON,不复制类型。
    nlohmann::json roles_table = nlohmann::json::object();
};

// SetModel 的结果。
struct SetModelResult {
    bool switched = false;      // 没切(清单空/网络败)为 false
    std::string model;          // 切完后的会话模型
    std::string think;          // 目录条目应用后的档位
    bool config_written = false;  // 写回配置文件成功
    std::string error;          // 空 = 成功;非空人话
};

// SetRoleModel 的结果(/model <role> <id>)。
struct SetRoleModelResult {
    bool switched = false;        // 角色名不认/模型名为空为 false
    std::string role;             // 归一后的角色名(normal/cheap/lao)
    std::string model;            // 设置后的模型名
    bool config_written = false;  // 写回配置文件成功
    std::string error;            // 空 = 成功;非空人话(稳定码前缀)
};

// ---------------------------------------------------------------------------
// ResumeThread
// ---------------------------------------------------------------------------

struct ThreadListEntry {
    std::string id;
    std::string started_at;
    std::string cwd;
    std::string title;
    std::string first_user_text;
    std::size_t message_count = 0;
    std::size_t index = 0;  // 列表序号(1 起,Resume 按它点名;与 ListSessions 倒序一致)
};

struct ResumeResult {
    bool resumed = false;
    std::string id;
    std::string error;               // 空 = 成功
    std::size_t restored_messages = 0;  // 回放出的有效消息数
    std::size_t total_lines = 0;        // 全量流水行数(含压缩前)
    int compact_epoch = 0;              // 接旧的压缩序号
    std::string title;                 // 存档里最后一条 title 事件
};

// ---------------------------------------------------------------------------
// CommandService
// ---------------------------------------------------------------------------

class CommandService {
public:
    // fetch_models:模型清单的取数口(终端是真的 ListModels 网络调用;测试
    // 注入假实现)。返回 {id, display} 对;错误交 expected。
    using ModelFetcher =
        std::function<std::expected<std::vector<std::pair<std::string, std::string>>, std::string>()>;

    struct Options {
        config::Config* config = nullptr;                  // 会话可变的运行配置
        const config::ModelCatalog* model_catalog = nullptr; // 模型目录(条目应用)
        std::shared_ptr<std::string> current_model;        // 会话模型(与 backend 栈共内存)
        std::shared_ptr<std::string> current_think;        // 会话档位(同上)
        std::optional<std::string> config_file_path;         // /model 可写回的文件
        std::size_t context_window_tokens = 0;               // 目录条目应用后回填(0 = 不动)
        const agent::ModelRouteTable* roles_table = nullptr; // 可空:路由表原样外带
        ModelFetcher fetch_models;                            // 可空:query 时拿不到清单
        std::string sessions_dir;                             // ListThreads 的扫档目录(空 = 空清单)
    };

    explicit CommandService(Options options);
    ~CommandService();

    CommandService(const CommandService&) = delete;
    CommandService& operator=(const CommandService&) = delete;

    // ---- SetModel -----------------------------------------------------------
    // 裸敲(query):拿清单与当前项,不切。
    ModelQueryResult QueryModels() const;

    // 提交:SetModel.value 是模型 id(空 = 不切,只回清单)。写回配置文件
    // 由 write_config 显式给(终端问一句再传 true;GUI 按钮分立)。写回时
    // active_provider 在场就写 provider 条目的 model(每个 provider 各记各
    // 的模型,切走再切回来还是它;顶层字段会被活跃端镜像压过),条目不在
    // 目标文件里才退回写顶层 model 字段。
    SetModelResult SetModel(const std::string& model_id, bool write_config);

    // 提交角色模型(/model <role> <id>):改内存 shorthand 字段——路由表
    // 每次 Route() 现折 config,下一笔后台小活立即生效。write_config 为
    // true 时经 UpdateRoleModelInConfigFile 落盘(高级段在场且该格已配就
    // 改高级段,否则写 shorthand;文件不存在报错,不代建)。
    SetRoleModelResult SetRoleModel(const std::string& role_name, const std::string& model_id,
                                    bool write_config);

    // ---- ResumeThread ---------------------------------------------------------
    // 列档(sessions_dir 从 runtime 来;limit 常规 20)。
    std::vector<ThreadListEntry> ListThreads(std::size_t limit = 20) const;

    // 恢复:thread_ref 是列表序号(1 起,倒序)或会话 id 或空串(最近一场)。
    // cwd 只在本目录的场子里数(与 ResumeSession 同规矩);直接给 id 全局
    // 能找。成功后 runtime 的存档账被接管,persisted_count/title/compact_
    // epoch 都已接旧账,history 经 loop 换入。
    ResumeResult ResumeThread(agent::AgentLoop& loop, SessionRuntime& runtime,
                              const std::string& thread_ref, const std::string& cwd);

    // ---- 审批/提问回答 --------------------------------------------------------
    // 转发 Broker:迟到/失效回答 ok=false + error=kStaleRequestId,不崩。
    // broker 为空(终端当场问完的实现)恒 false——远端前端才有真 Broker。
    struct InteractionAnswerResult {
        bool ok = false;
        std::string error_code;
        std::string error_message;
    };
    InteractionAnswerResult ResolveApproval(InteractionBroker* broker, const std::string& request_id,
                                            const ApprovalResponse& response) const;
    InteractionAnswerResult AnswerQuestion(InteractionBroker* broker, const std::string& request_id,
                                           const QuestionResponse& response) const;

    // ---- /goal 六命令 + /loop 七命令 + plan.review(typed 兑现) --------------
    // 单子"Runtime 与多前端合同"的执行体:前端只发 typed ClientCommand,
    // 不拼 slash 字符串。goal/loop 的状态机真值由装配层注入(coordinator/
    // scheduler),这里只做命令翻译与回执拼装——不持有、不创建;空指针 =
    // 该域未装配,回 goal_disabled/loop_disabled 的稳定码,不冒充成功。
    // Plan 的审批走 SessionRuntime(调用方给),批准/拒绝须同时匹配 id/
    // revision/hash(不匹配 stale_request_id)。
    //
    // 返回的 ClientReceipt:error_code 是稳定码(goal.*/loop.*/plan.* 或
    // unsupported),payload 带结构化账(GetGoal 的 Status()/ListLoopTasks
    // 的 Snapshot() 折 JSON)。
    ClientReceipt HandleGoalCommand(const ClientCommand& command, goal::GoalCoordinator* coordinator,
                                    const std::string& workspace_root, std::int64_t now_ms);
    ClientReceipt HandleLoopCommand(const ClientCommand& command, loop::LoopScheduler* scheduler,
                                     const std::string& cwd_identity, const std::string& session_id,
                                     std::int64_t now_ms);
    ClientReceipt HandlePlanCommand(const ClientCommand& command, SessionRuntime* runtime);

private:
    Options options_;
};

}  // namespace lubancode::runtime
