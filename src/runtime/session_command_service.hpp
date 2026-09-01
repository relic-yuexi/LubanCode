// SessionCommandService(会话管理器单第六步;P0-2 换到 workspace 新账):
// runtime 侧的会话查询与搬删命令服务。前端(TUI/Web/Tauri/app-server)只
// 发 typed ClientCommand,拿即时 ClientReceipt——不再各自扫 JSONL、各自
// 碰 filesystem。
//
// 职责(单子"代码边界"一节;P0-2 数据源换 workspace 索引/管理面):
//   thread.list       -> trajectory::QueryWorkspaceSessions(可重建索引)
//   thread.archive    -> trajectory::ArchiveSessionDir(lifecycle + 状态图)
//   thread.unarchive  -> trajectory::UnarchiveSessionDir
//   thread.delete     -> trajectory::DeleteSessionDir(payload.confirm 才动手)
//
// 确认策略:delete 的确认归调用方(终端走确认屏,GUI 走自己的对话框),
// 协议不替人决定;没带 confirm 一律 confirmation_required 拒绝,不动盘。
//
// 事件:归档/反归档成功后由调用方发 thread.updated(payload 带 state),
// 删除成功后发 thread.deleted——服务本身不持 EventSink,长活事件归
// Runtime 装配层(与 ClientReceipt 的即时语义分层)。
//
// 依赖铁律:runtime 不 include cli/app;trajectory 是中立库,这里引它
// 不破层次。workspaces_root 与当前 workspace_key 由装配层注入。

#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/command.hpp"

namespace lubancode::runtime {

// 服务的即时回执。error_code 用稳定码(not_found/confirmation_required/
// invalid_request/io_error,详见实现)。
struct SessionCommandOutcome {
    bool accepted = true;
    std::string error_code;
    std::string error_message;
    nlohmann::json payload = nlohmann::json::object();  // thread.list 的清单等

    ClientReceipt ToReceipt() const {
        ClientReceipt receipt;
        receipt.accepted = accepted;
        receipt.error_code = error_code;
        receipt.error_message = error_message;
        receipt.payload = payload;
        return receipt;
    }
};

// 会话查询/搬删服务。构造时定唯一持久化根与当前 workspace;线程模型与
// 索引/管理面相同(调用方串行调,内部不另起线程)。
class SessionCommandService {
public:
    struct Options {
        std::filesystem::path workspaces_root;  // ~/.lubancode/workspaces;空 = 没账可用
        std::string workspace_key;              // scope=cwd 时的当前 workspace key
    };

    // workspaces_root 空 = 没有会话账可用(list 给空表,搬删一律拒绝)。
    explicit SessionCommandService(Options options);
    SessionCommandService(const SessionCommandService&) = delete;
    SessionCommandService& operator=(const SessionCommandService&) = delete;
    ~SessionCommandService();

    // thread.list:payload 查询形状(全可选,缺省 cwd|active|updated|
    // 空 search|不截):
    //   {"scope":"cwd"|"all", "state":"active"|"archived",
    //    "sort":"updated"|"created", "search":"...",
    //    "cursor":0, "limit":20}
    // scope=cwd 即当前 workspace(同仓子目录/linked worktree 一把钥匙,
    // 不再按 meta.cwd 逐场筛)。receipt.payload =
    // {"threads":[{...SessionSummary...}], "total":N}。
    SessionCommandOutcome ListThreads(const nlohmann::json& query_payload) const;

    // thread.archive / thread.unarchive:thread_id 按完整 id 解(协议层
    // 已经是稳定 id,引用消歧是终端/CLI 的活,不进协议)。id 在哪个
    // workspace 由索引定位(跨 workspace 也搬得动)。
    SessionCommandOutcome ArchiveThread(const std::string& thread_id);
    SessionCommandOutcome UnarchiveThread(const std::string& thread_id);

    // thread.delete:payload.confirm == true 才动手;否则
    // confirmation_required,盘上不动。
    SessionCommandOutcome DeleteThread(const std::string& thread_id, const nlohmann::json& payload);

    // ClientCommand 的总入口(kind 只认上面四枚;别的给 invalid_request,
    // 不吞)。返回即时回执。
    ClientReceipt HandleCommand(const ClientCommand& command);

    const std::filesystem::path& workspaces_root() const { return options_.workspaces_root; }

private:
    Options options_;
};

// SessionSummary(agent 侧) -> JSON(协议形状;字段名与 app-server 的
// thread/list 对齐用 camelCase,终端/Web/Tauri 共吃这一碗)。
nlohmann::json SessionSummaryToJson(const std::string& id, const std::string& title,
                                    const std::string& first_user_text, const std::string& cwd,
                                    const std::string& model, const std::string& created_at,
                                    const std::string& updated_at, std::uint64_t message_count,
                                    const std::string& state, const std::string& health);

}  // namespace lubancode::runtime
