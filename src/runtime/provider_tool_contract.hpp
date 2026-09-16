// ProviderToolContractValidator(异步工具单 P2;§4 末"能力闸"):
// endpoint/wire/model/工具声明/运行配置合成能力判定。三态
// unknown/verified/unsupported;unknown/缺项 fail-closed 拒 native_deferred
// ——不按模型名猜支持,不把兼容网关当官方端(单 §4)。
//
// 判定是纯合成(P2 无真探针):外部证据(probe)由装配注入——生产装配
// 不注(全部 unknown,native 永不放行,归 P3 原生试点真验),测试/探针
// 面注入 verified/unsupported。快照落账(tool.capability.recorded,basis
// 留档判定依据)由 AsyncToolRuntime 做,这里只出形状。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 合成判定的输入(单 §4"选择条件是 endpoint/wire/model/工具声明/运行
// 配置共同满足")。
struct ProviderToolContract {
    std::string provider;   // provider 名(配置层)
    std::string wire;       // responses|anthropic|chat|gemini
    std::string model;      // 模型 id(不按名猜支持,只入依据)
    std::string endpoint;   // 可空:已知的 API 端点(兼容网关判据之一)
    bool tool_declares_async = false;  // 工具声明侧 async(P3 原生声明;P2 恒 false)
    bool call_marked_async = false;    // 调用证据带 async(回包 function_call.async)
    // 外部探针证据(P2 不注 = unknown):native_deferred 只有 verified 才放行。
    std::optional<std::string> native_probe_status;   // verified|unsupported(缺 = unknown)
    std::optional<std::string> native_probe_evidence; // 探针依据描述(event id/探针记录)
    // job_handle 是宿主侧行为,不依赖 provider 协议;只有装配明示 unsupported
    // 才禁(读取侧 DecideAsyncModes 同一口径)。
    bool job_handle_disabled = false;
    // parallel_tool_calls 与 Multi-agent/PTC 排斥口径(单 §4:原生首期保守
    // 禁 parallel_tool_calls)——留档进快照,P3 执法。
    bool parallel_tool_calls_enabled = false;
};

struct ToolContractVerdict {
    std::string capability;  // native_deferred|job_handle|parallel_tool_calls
    std::string status;      // unknown|verified|unsupported
    std::string evidence;    // 人话依据(落快照)
};

// 合成判定(纯函数):
//   native_deferred —— wire==responses && call_marked_async && 探针 verified
//                      才 verified;缺项/unknown 一律 unknown(fail-closed,
//                      闸门只认 verified)。
//   job_handle      —— 装配明示禁才 unsupported,否则 verified(宿主侧行为)。
//   parallel_tool_calls —— enabled 才 verified(单 §4 保守禁用留档)。
std::vector<ToolContractVerdict> EvaluateProviderToolContract(const ProviderToolContract& contract);

// 判定快照的载荷形状(basis + verdicts;落 tool.capability.recorded 用,
// P0 schema §四 异步工具条目的载荷合同)。
nlohmann::json ContractSnapshotPayload(const ProviderToolContract& contract,
                                       const std::vector<ToolContractVerdict>& verdicts);

}  // namespace lubancode::runtime
