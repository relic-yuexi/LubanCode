// 异步工具 P2 的批次闸门与投递规划 seam(单 §7"主循环与并发"):
// AgentLoop 的工具批次从"逐枚 RunOneTool"拆出一只可装配的闸门口,每枚
// 调用先过"协议模式裁决"(inline 收齐续跑 / job_handle 接单即配 /
// native_deferred 留欠账),完成信封经泵回灌;模型请求边界由
// ResultDeliveryPlanner 选已提交结果。
//
// 这里只有接口;实现与装配在 runtime(AsyncToolRuntime 把
// ToolJobCoordinator/ResultDeliveryPlanner/ProviderToolContractValidator
// 组到一起,各宿主接线,不各造)。TurnWiring 持两只裸指针,不设 = 旧路
// 全 inline,行为与从前一字不差(单测/未接线宿主)。
//
// 派发点两档(单 §7;宿主伪异步指示 2026-09-16 并入):
//   OnAssistantComplete —— 默认保守档:完整 assistant 落账后派发。
//   OnCallItemComplete  —— 流式提前档:SSE 流中单枚 call item 完整
//       (call_id 定型、参数 JSON 收齐、调用证据落账)即派发,宿主继续
//       消费流;仅对执行策略判"可先跑"的工具(只读/幂等 + 无资源键冲突
//       + 权限已在握)。两档由每枚调用的策略合成决定,不是全局开关。
//       雷区(单 §7 原文):不得在 JSON delta 半截、call_id 未定时开跑;
//       流中断而工具已执行 → 恢复按 dispatched 无终态收 unknown_hold,
//       不盲重跑;重复终帧只派发一次。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "api/types.hpp"
#include "tools/tool.hpp"

namespace lubancode::agent {

// 协议模式(单 §4 三种):
//   Inline         —— 宿主等待,本批实际结果齐后配对(所有普通客户端工具)。
//   JobHandle      —— start 交任务,接单结果即配齐调用(跨 provider 通用,
//                     宿主伪异步)。
//   NativeDeferred —— 原调用保持未配对(欠账),最终结果只配原 call_id
//                     (仅能力 verified 的 Responses 路由;unknown/缺项
//                     fail-closed 降级,不许悬空)。
enum class ToolProtocolMode { Inline, JobHandle, NativeDeferred };

// 派发点(两档,见文件头)。
enum class ToolDispatchPoint { OnAssistantComplete, OnCallItemComplete };

// 一枚调用的裁决结果。
struct ToolCallAdjudication {
    ToolProtocolMode mode = ToolProtocolMode::Inline;
    // 派发点由策略合成:job_handle 且策略允许提前 → OnCallItemComplete。
    // inline 恒 OnAssistantComplete(同步队列等回合收口)。
    ToolDispatchPoint dispatch_point = ToolDispatchPoint::OnAssistantComplete;
    // 裁决依据:能力快照事件 id(tool.capability.recorded)或宿主策略来源,
    // 留档用;空 = 无依据(inline 缺省)。
    std::string basis;
    // 降级说明(provider 标了 async 但能力 unknown 按 job_handle/inline
    // 配对时的人话);正常路为空。
    std::string note;
};

// 批次闸门 seam。AgentLoop 在三个时点问它:
//   1. 流中单枚 call item 完整(OnCallItemComplete——提前档探针);
//   2. 完整 assistant 落账后(AdjudicateBatch + TakeJobOrder——批次档);
//   3. 批次收口(PumpBatchBoundary——完成信封回灌口)。
// 实现方负责 dedup(重复终帧只派发一次)与幂等(提前派发过的调用批次
// 时不再重派,只补接单)。
class ToolBatchGate {
public:
    virtual ~ToolBatchGate() = default;

    // 流式提前档探针:单枚 call item 完整时被调(call 来自 assembler 的
    // completed_tool_uses——call_id 已定型、参数已收齐)。返回 true = 已
    // 提前派发(job 已注册派发);false = 不提前(策略不许/权限不在手/
    // 资源键冲突/能力 unknown),该调用照批次档走。调用证据锚
    // (assistant_message_ref)由实现方从注入的解析回调取;解析不到
    // (流还没起账)不派发——证据落不了账不开跑。
    struct StreamCallContext {
        std::string turn_id;
        std::string step_id;
        std::string trajectory_request_id;  // 当前在途请求的账面 id(可空)
    };
    virtual bool OnCallItemComplete(const api::ToolUseBlock& call, const StreamCallContext& context) = 0;

    // 批次裁决:完整 assistant 落账后、批次栅栏(scheduled)之前,对本批
    // 全部调用一次性裁决。返回与入参同长、同序;inline 是缺省。非 inline
    // 的调用不走 bridge 的内联链(协调器自落调用证据链),批次栅栏与
    // RunOneTool 都跳过。
    virtual std::vector<ToolCallAdjudication> AdjudicateBatch(
        const std::vector<api::ToolUseBlock>& calls) = 0;

    // job_handle/native_deferred 接单:交协调器注册派发(注册落稳前不派
    // 发,单 §5)。job_handle 回接单结果——即配这枚调用的 tool_result;
    // native_deferred 回 nullopt(留欠账,业务结果由规划器在下一次请求
    // 边界配原 call)。实现保证幂等:提前派发过的调用这里只补接单链。
    // 失败(拒绝/队满/落账失败)也回 nullopt 并置 error 文案——调用方按
    // inline 兜底真执行,不许留悬空。
    virtual std::optional<tools::Tool::Result> TakeJobOrder(
        const api::ToolUseBlock& call, const ToolCallAdjudication& adjudication) = 0;

    // 完成信封回灌口:批次收口时泵一把协调器(收割 worker 完成信封落
    // 账),新终态翻完成通知入规划器 mailbox(单 §7"完成通知只入
    // mailbox")。请求在途时到的结果留给下次请求边界。
    virtual void PumpBatchBoundary() = 0;
};

// 请求边界投递规划 seam(单 §7):冻结输入前选已提交结果;prepared 落稳
// 后记账;响应收口写接纳/存疑。
class ResultDeliveryPlanner {
public:
    virtual ~ResultDeliveryPlanner() = default;

    // 请求边界(拼请求之前——冻结输入前的唯一时点):选已提交结果,把
    // 选中的投递写成正式 tool 消息落账+接纳(实现方负责),返回给引擎的
    // 只是要入内存史的正式消息(native_deferred 的 function_call_output
    // /欠账配对)。当前请求开始后才到的完成通知留在 mailbox,下一次选取
    // 按稳定提交次序;不修改已冻结输入;待投递结果不跨目标分支。
    virtual std::vector<api::Message> SelectForRequestBoundary() = 0;

    // 请求账已 prepared(request_id 已发号落稳):为本次选中的投递落
    // tool.delivery.prepared(deliveryId/resultRef/resultVersion/信封
    // requestId)。没选中任何投递时是空操作。
    virtual void NoteRequestPrepared(const std::string& request_id) = 0;

    // 响应收口:acknowledged=true 时按证据事件落 tool.delivery.
    // acknowledged(evidenceRef);false(失败/取消/流断)落 tool.delivery.
    // uncertain(reason)。回执丢失不把重试当 exactly-once(单 §6)。
    virtual void NoteResponseOutcome(const std::string& request_id, bool acknowledged) = 0;
};

}  // namespace lubancode::agent
