// SessionCommandService(会话管理器单第六步):runtime 侧的会话查询与
// 搬删命令服务。前端(TUI/Web/Tauri/app-server)只发 typed ClientCommand,
// 拿即时 ClientReceipt——不再各自扫 JSONL、各自碰 filesystem。
//
// 职责(单子"代码边界"一节):
//   thread.list       -> SessionCatalog 查询,回结构化 SessionSummary 数组
//   thread.archive    -> SessionLifecycle.ArchiveSession
//   thread.unarchive  -> SessionLifecycle.UnarchiveSession
//   thread.delete     -> SessionLifecycle.DeleteSession(payload.confirm 才动手)
//
// 确认策略:delete 的确认归调用方(终端走确认屏,GUI 走自己的对话框),
// 协议不替人决定;没带 confirm 一律 confirmation_required 拒绝,不动盘。
//
// 事件:归档/反归档成功后由调用方发 thread.updated(payload 带 state),
// 删除成功后发 thread.deleted——服务本身不持 EventSink,长活事件归
// Runtime 装配层(与 ClientReceipt 的即时语义分层)。
//
// 依赖铁律:runtime 不 include cli/app;agent::SessionCatalog 与
// agent::SessionLifecycle 是中立层(agent/ 不反向依赖 runtime),这里引
// 它们不破层次。thread_id_to_path 形状的会话存档根由装配层注入。

#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/command.hpp"

namespace lubancode::agent {
class SessionCatalog;
class SessionLifecycle;
}  // namespace lubancode::agent

namespace lubancode::runtime {

// 服务的即时回执。error_code 用 SessionLifecycle 的稳定码
// (not_found/ambiguous/confirmation_required/path_outside_root/
// target_exists/io_error/invalid_request)。
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

// 会话查询/搬删服务。构造时定会话根目录;线程模型与 catalog/lifecycle
// 相同(调用方串行调,内部不另起线程)。
class SessionCommandService {
public:
    // sessions_dir 空 = 没有会话存档可用(list 给空表,搬删一律拒绝)。
    explicit SessionCommandService(std::string sessions_dir);
    ~SessionCommandService();

    // thread.list:payload 查询形状(全可选,缺省 cwd|active|updated|
    // 空 search|不截):
    //   {"scope":"cwd"|"all", "state":"active"|"archived",
    //    "sort":"updated"|"created", "search":"...",
    //    "cwd":"...", "cursor":0, "limit":20}
    // receipt.payload = {"threads":[{...SessionSummary...}], "total":N}。
    // 相对时间不在这层算——协议层留稳定时间串,前端按自己的 locale 画
    // (单子"代码边界 SessionCatalog"定死)。
    SessionCommandOutcome ListThreads(const nlohmann::json& query_payload) const;

    // thread.archive / thread.unarchive:thread_id 按完整 id 解(协议层
    // 已经是稳定 id,引用消歧是终端/CLI 的活,不进协议)。
    SessionCommandOutcome ArchiveThread(const std::string& thread_id);
    SessionCommandOutcome UnarchiveThread(const std::string& thread_id);

    // 会话内 /archive、/delete 的 Windows 句柄闸:当前会话的 append 句柄
    // 还开着,搬删之前先经回调收柄。转发给 SessionLifecycle::SetActiveFile
    // ——宿主(InteractiveSession)知道句柄在谁手里,这里不猜。
    void SetActiveFile(std::string active_file, std::function<bool(const std::string&)> flush_close);

    // thread.delete:payload.confirm == true 才动手;否则
    // confirmation_required,盘上不动。
    SessionCommandOutcome DeleteThread(const std::string& thread_id, const nlohmann::json& payload);

    // ClientCommand 的总入口(kind 只认上面四枚;别的给 invalid_request,
    // 不吞)。返回即时回执。
    ClientReceipt HandleCommand(const ClientCommand& command);

    const std::string& sessions_dir() const { return sessions_dir_; }

private:
    std::string sessions_dir_;
    std::unique_ptr<agent::SessionLifecycle> lifecycle_;
};

// SessionSummary(agent 侧) -> JSON(协议形状;字段名与 app-server 的
// thread/list 对齐用 camelCase,终端/Web/Tauri 共吃这一碗)。
nlohmann::json SessionSummaryToJson(const std::string& id, const std::string& title,
                                    const std::string& first_user_text, const std::string& cwd,
                                    const std::string& model, const std::string& created_at,
                                    const std::string& updated_at, std::uint64_t message_count,
                                    const std::string& state, const std::string& health);

}  // namespace lubancode::runtime
