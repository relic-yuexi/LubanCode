// provider_tool_contract.hpp 的实现。纯合成,不接网络、不落账。
#include "runtime/provider_tool_contract.hpp"

namespace lubancode::runtime {
namespace {

bool ProbeVerified(const ProviderToolContract& contract) {
    return contract.native_probe_status.has_value() &&
           *contract.native_probe_status == "verified";
}

}  // namespace

std::vector<ToolContractVerdict> EvaluateProviderToolContract(const ProviderToolContract& contract) {
    std::vector<ToolContractVerdict> verdicts;

    // native_deferred:全部条件共同满足才 verified(单 §4)。能力取
    // unknown/verified/unsupported;unknown 默认不用 native_deferred——
    // 闸门侧 DecideAsyncModes 只认 verified,这里的 status 如实合成。
    ToolContractVerdict native;
    native.capability = "native_deferred";
    if (!contract.native_probe_status.has_value()) {
        native.status = "unknown";
        native.evidence = "无外部探针证据(P2 无真探针,归 P3 原生试点)";
    } else if (*contract.native_probe_status == "verified") {
        if (contract.wire != "responses") {
            native.status = "unsupported";
            native.evidence = "探针 verified 但 wire=" + contract.wire + "(原生异步只在 responses 路由)";
        } else if (!contract.call_marked_async) {
            native.status = "unknown";
            native.evidence = "探针 verified 但本次调用证据未带 async 标记";
        } else {
            native.status = "verified";
            native.evidence = contract.native_probe_evidence.value_or("probe");
        }
    } else {
        native.status = "unsupported";
        native.evidence = contract.native_probe_evidence.value_or("probe: unsupported");
    }
    verdicts.push_back(std::move(native));

    // job_handle:宿主侧行为,不依赖 provider 协议;只有装配明示禁。
    ToolContractVerdict job_handle;
    job_handle.capability = "job_handle";
    job_handle.status = contract.job_handle_disabled ? "unsupported" : "verified";
    job_handle.evidence = contract.job_handle_disabled
                              ? std::string("装配明示禁用 job_handle")
                              : std::string("宿主侧任务服务(P1 ToolJobCoordinator),不依赖 provider 协议");
    verdicts.push_back(std::move(job_handle));

    // parallel_tool_calls:原生首期保守禁用(单 §4;与 API Multi-agent
    // mode/PTC 排斥),只有显式启用才 verified。留档给 P3 执法。
    ToolContractVerdict parallel;
    parallel.capability = "parallel_tool_calls";
    parallel.status = contract.parallel_tool_calls_enabled ? "verified" : "unknown";
    parallel.evidence = contract.parallel_tool_calls_enabled
                            ? std::string("装配显式启用(与 PTC/Multi-agent 排斥自查归 P3)")
                            : std::string("原生首期保守禁用(单 §4)");
    verdicts.push_back(std::move(parallel));
    return verdicts;
}

nlohmann::json ContractSnapshotPayload(const ProviderToolContract& contract,
                                       const std::vector<ToolContractVerdict>& verdicts) {
    nlohmann::json basis = nlohmann::json::object({
        {"provider", contract.provider},
        {"wire", contract.wire},
        {"model", contract.model},
    });
    if (!contract.endpoint.empty()) {
        basis["endpoint"] = contract.endpoint;
    }
    if (contract.tool_declares_async) {
        basis["toolDeclarationAsync"] = true;
    }
    nlohmann::json verdict_map = nlohmann::json::object();
    for (const ToolContractVerdict& verdict : verdicts) {
        verdict_map[verdict.capability] =
            nlohmann::json::object({{"status", verdict.status}, {"evidence", verdict.evidence}});
    }
    return nlohmann::json::object({{"basis", std::move(basis)}, {"verdicts", std::move(verdict_map)}});
}

}  // namespace lubancode::runtime
