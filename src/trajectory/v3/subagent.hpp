// subagent 独立账(§4.31-4.33):调用 subagent 是父模型的一次工具 Action;
// 真正运行的子代理有独立会话与自己的账。父账只拥有调用、关联、观察与
// 交付事实;子账拥有其内部模型、工具、hook、compact 与任务执行事实。
//
// 目录合同(§4.31):sessions/<sessionId>/subagents/<child>/<child>.jsonl,
// 每层完整 session 布局,后代可递归;目录说物理位置,父子关系凭账中引用。
//
// 启动五步(§4.32,可恢复的跨文件交接,不声称两份文件原子写):
//   1 父账 subagent.spawn.requested(持久预留 taskId/childSessionRef/
//     attempt/parentActionRef/任务参数)
//   2 建子目录,子账唯一写者写首行 system(带派生来源)+ session.started
//     + 委派 user(origin=parent_agent)+ task.started;落稳返检查点。
//     此前不调模型、不执行工具、不触发副作用 hook。
//   3 父账 subagent.linked(引用子账检查点);关联落稳后子账才开始执行。
//   4 子账独立记账;父账可追加 subagent.observed(只存检查点与展示信息)。
//   5 子终态先在子账落稳,父侧再沿结果仓/选用/tool 消息合同交付。
// 初始化失败:父账 subagent.spawn.failed(phase/reason),不借父账续记。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::trajectory::v3 {

// 子会话引用(§4.31):目录位置不是身份的替代品。
struct ChildSessionRef {
    std::string session_id;
    std::string run_id;
    std::string journal_path;  // 相对 session 根或绝对,展示用;身份在前两键

    nlohmann::json ToJson() const {
        return nlohmann::json::object({{"sessionId", session_id},
                                       {"runId", run_id},
                                       {"journalPath", journal_path}});
    }
};

// 子账固定检查点(§4.31):证明当时观察到子账的哪个持久前缀。
struct ChildCheckpointRef {
    std::string session_id;
    std::string run_id;
    std::uint64_t seq = 0;
    std::string line_hash;

    nlohmann::json ToJson() const {
        return nlohmann::json::object({{"sessionId", session_id},
                                       {"runId", run_id},
                                       {"seq", seq},
                                       {"lineHash", line_hash}});
    }
};

// 父工具引用(§4.31):创建子会话的父 sessionId/runId/turnId/stepId/
// actionId 与声明消息引用。
struct ParentActionRef {
    std::string session_id;
    std::string run_id;
    std::string turn_id;
    std::string step_id;
    std::string action_id;
    std::string declared_message_ref;

    nlohmann::json ToJson() const {
        return nlohmann::json::object({{"sessionId", session_id},
                                       {"runId", run_id},
                                       {"turnId", turn_id},
                                       {"stepId", step_id},
                                       {"actionId", action_id},
                                       {"declaredMessageRef", declared_message_ref}});
    }
};

class SubagentSpawn {
public:
    // 步 1:父账 subagent.spawn.requested(§4.32)。action_id 为父工具调用
    // 身份;attempt 从 1 起,重投复用这一预留身份。
    static SubagentSpawn Request(V3Writer& parent, std::string action_id,
                                 std::string turn_id, std::string step_id,
                                 std::string task_id, ChildSessionRef child,
                                 ParentActionRef parent_ref, nlohmann::json task_args,
                                 nlohmann::json config_snapshot,
                                 Durability durability = Durability::PowerLoss);

    struct BootstrapResult {
        std::optional<V3Writer> child_writer;  // 子账唯一写者(调用方接管)
        ChildCheckpointRef checkpoint;         // 落稳后的子账前缀
        std::string error;
    };

    // 步 2:建子目录并开子账。首行 system 的 systemMeta 带派生来源
    // (parentActionRef/taskId/spawnEventRef 五键含 hash,§4.31"派生来源");
    // 委派任务形成 origin=parent_agent 的真实 user 输入;随后 task.started。
    // 首版默认异步启动(§4.32):linked 落稳即算任务被接纳。
    BootstrapResult BootstrapChild(const V3Writer& parent, std::string_view child_run_id,
                                   std::string_view child_system_content,
                                   std::string_view task_prompt,
                                   Durability durability = Durability::PowerLoss) const;

    // 步 3:父账 subagent.linked(引用子账检查点)。
    WriteReceipt Link(V3Writer& parent, const ChildCheckpointRef& checkpoint,
                      Durability durability = Durability::PowerLoss) const;

    // 步 4:父账 subagent.observed(只保存源检查点与必要展示信息)。
    WriteReceipt Observe(V3Writer& parent, const ChildCheckpointRef& checkpoint,
                         nlohmann::json display_info = nlohmann::json::object(),
                         Durability durability = Durability::ProcessCrash) const;

    // 初始化失败(§4.32):subagent.spawn.failed(phase/reason),停止执行。
    // phase ∈ child_dir|child_first_line|child_init|child_task_input|link。
    WriteReceipt Fail(V3Writer& parent, std::string phase, std::string reason,
                      Durability durability = Durability::PowerLoss) const;

    const std::string& action_id() const { return action_id_; }
    const std::string& task_id() const { return task_id_; }
    const ChildSessionRef& child() const { return child_; }

private:
    SubagentSpawn(std::string action_id, std::string turn_id, std::string step_id,
                  std::string task_id, ChildSessionRef child, ParentActionRef parent_ref,
                  WriteReceipt spawn_receipt);
    std::string action_id_;
    std::string turn_id_;
    std::string step_id_;
    std::string task_id_;
    ChildSessionRef child_;
    ParentActionRef parent_ref_;
    WriteReceipt spawn_receipt_;  // 预留事件回执(id/seq/lineHash 供跨会话五键)
};

}  // namespace lubancode::trajectory::v3
