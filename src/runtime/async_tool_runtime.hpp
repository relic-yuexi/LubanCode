// AsyncToolRuntime(异步工具单 P2 总装):把 ToolJobCoordinator(P1)、
// ResultDeliveryPlanner(P2)、ProviderToolContractValidator(P2 能力闸)
// 组装成一只会话级运行时,实现 agent::ToolBatchGate seam。各宿主(终端/
// one-shot/子代理/AppServer/Workflow)从这里接同一套闸门与规划器,不
// 各造(单 §7"拟拆职责")。
//
// 模型可见面:job_get/job_wait/job_cancel 三枚工具(RegisterJobTools)
// 挂宿主注册表;start 不单设——白名单工具的普通调用由批次闸门裁决接单
// (接单结果即配那枚调用,单 §8)。
//
// 生产缺省:tools 白名单空 = 全部调用 inline,行为与从前一字不差;
// native_probe 不注 = native_deferred 永远 fail-closed 降级(P3 原生
// 试点才真验)。测试/假后端剧本注入白名单与探针证据。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/async_tool_seam.hpp"
#include "runtime/result_delivery_planner.hpp"
#include "runtime/provider_tool_contract.hpp"
#include "tools/tool_job_coordinator.hpp"

namespace lubancode::runtime {

// 白名单里一枚工具的宿主策略(单 §4 执行策略 + 派发点两档)。
struct AsyncToolPolicy {
    tools::JobExecutionPolicy execution;
    // 派发点:OnCallItemComplete(流式提前档)只对"可先跑"的策略放行
    // ——side_effect_class=read_only 且未声明 resource_keys(只读无键,
    // 不抢串行位);其余一律 OnAssistantComplete(同步队列等回合收口)。
    agent::ToolDispatchPoint dispatch_point = agent::ToolDispatchPoint::OnAssistantComplete;
    // 只接 provider 标了 async 的调用(原生声明工具;P2 假后端剧本用)。
    bool require_call_async_mark = false;
};

struct AsyncToolRuntimeOptions {
    // 能力合成的依据(provider/wire/model 进快照 basis)。
    std::string provider;
    std::string wire;
    std::string model;
    std::string endpoint;
    // 异步白名单:工具名 -> 策略。空 = 全 inline(生产缺省)。
    std::map<std::string, AsyncToolPolicy> tools;
    // 外部探针证据(P2 不注 = unknown = fail-closed;测试注入 verified)。
    std::optional<std::string> native_probe_status;
    std::optional<std::string> native_probe_evidence;
    bool job_handle_disabled = false;
    tools::ToolJobCoordinator::Options coordinator;
};

class AsyncToolRuntime final {
public:
    struct Hooks {
        // 会话 v3 单写者与共享锁(与主桥/协调器同一份;宿主保证寿命)。
        trajectory::v3::V3Writer* writer = nullptr;
        std::shared_ptr<std::recursive_mutex> writer_mutex;
        // 当前回合号(规划器落 tool 消息的信封用;可空 = 落账时按通知自带)。
        std::function<std::string()> current_turn_id;
        // request_id -> 响应证据事件 id(投递 acknowledged 的 evidenceRef)。
        std::function<std::optional<std::string>(const std::string&)> response_evidence;
        // request_id -> 流式预留的 assistant messageId(提前档调用证据锚)。
        std::function<std::optional<std::string>(const std::string&)> reserved_assistant_message_id;
        // provider call id -> 账面声明上下文(action/message/turn/step)。
        std::function<std::optional<tools::JobStartRequest>(const std::string&)> call_origin_resolver;
        // 权鉴闸门(宿主审批链桥);空 = fail-closed 全拒。
        tools::JobAuthorizationGate auth;
        // worker 执行体(宿主从工具注册表桥:解析工具+execute);空 = 不
        // 派发(入队的 job 等宿主补 executor——P2 装配必给)。
        tools::JobExecutor executor;
    };

    static std::unique_ptr<AsyncToolRuntime> Create(Hooks hooks, AsyncToolRuntimeOptions options);

    // 每轮开拍前钉当前轮桥:证据/声明册/回合号的查询口走它(轮桥按轮
    // 新建,运行时按会话活;没钉 = 桥面查询全空,闸门提前档不派发、
    // acknowledged 落 uncertain——如实,不冒充)。
    void InstallTurnBridge(class TrajectoryTurnBridge* bridge);
    // 每轮开拍前刷新模型身份(能力快照的 basis;会话中途切模型照实换,
    // 快照只在首次裁决落一次,切换后的能力重验归 P3 探针面)。
    void NoteModelIdentity(const std::string& provider, const std::string& model);

    // TurnWiring 接线口(宿主各装配点拿这两只指针钉进 wiring)。
    agent::ToolBatchGate* gate();
    agent::ResultDeliveryPlanner* planner();
    // 模型可见 job 工具族的协调器柄(RegisterJobTools 用;shared 保序)。
    std::shared_ptr<tools::ToolJobCoordinator> coordinator();

    // 恢复(宿主 resume/开卷后调一次):协调器账态注入(PlanRecovery +
    // AdoptRecovery,P1)+ 规划器欠账重建(native 终态未配 → mailbox,
    // 下次请求边界补投递不重跑)。
    void RestoreFromLedger();

    // 诊断。
    std::size_t early_dispatched_count() const;

private:
    AsyncToolRuntime() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 宿主通用接线(终端/one-shot/网关共用):给会话挂一只零策略(dormant)
// 异步运行时——批次闸门/投递规划/协调器共享会话 v3 写者,能力快照照实落
// fail-closed 判定;tools 白名单空 = 全 inline,权鉴 fail-closed,不派发,
// 行为与从前一字不差。开异步工具须装配层补白名单+权鉴桥+executor(P3
// 原生试点)。幂等(已挂不动);v2 场/账没开 false。
bool AttachDefaultAsyncToolRuntime(class SessionRuntime& session, const std::string& wire_name);

}  // namespace lubancode::runtime
