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
#include "agent/session_store.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "runtime/command.hpp"
#include "runtime/interaction_broker.hpp"
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
    // 由 write_config 显式给(终端问一句再传 true;GUI 按钮分立)。
    SetModelResult SetModel(const std::string& model_id, bool write_config);

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

private:
    Options options_;
};

}  // namespace lubancode::runtime
