// 异步工具单 P2 册:ProviderToolContractValidator(§4 末能力闸)——
// endpoint/wire/model/工具声明/配置合成能力判定;unknown/缺项 fail-closed
// 拒 native_deferred;快照载荷形状(basis + verdicts)合 P0 载荷合同。
#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/provider_tool_contract.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/schema3.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

const ToolContractVerdict* VerdictOf(const std::vector<ToolContractVerdict>& verdicts,
                                     const std::string& capability) {
    for (const auto& verdict : verdicts) {
        if (verdict.capability == capability) {
            return &verdict;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("无探针证据:native_deferred 判 unknown(fail-closed,不按模型名猜)") {
    ProviderToolContract contract;
    contract.provider = "openai";
    contract.wire = "responses";
    contract.model = "gpt-6-astra";
    contract.call_marked_async = true;
    const auto verdicts = EvaluateProviderToolContract(contract);
    const auto* native = VerdictOf(verdicts, "native_deferred");
    REQUIRE(native != nullptr);
    CHECK(native->status == "unknown");
    CHECK(native->evidence.find("无外部探针证据") != std::string::npos);
}

TEST_CASE("探针 verified 但 wire 不是 responses:unsupported(原生只在 responses 路由)") {
    ProviderToolContract contract;
    contract.provider = "anthropic";
    contract.wire = "anthropic";
    contract.model = "claude-x";
    contract.call_marked_async = true;
    contract.native_probe_status = "verified";
    contract.native_probe_evidence = "probe-run-1";
    const auto verdicts = EvaluateProviderToolContract(contract);
    CHECK(VerdictOf(verdicts, "native_deferred")->status == "unsupported");
}

TEST_CASE("探针 verified 但调用没带 async 标记:unknown(标记不符按同步配对)") {
    ProviderToolContract contract;
    contract.provider = "openai";
    contract.wire = "responses";
    contract.model = "gpt-6";
    contract.call_marked_async = false;
    contract.native_probe_status = "verified";
    contract.native_probe_evidence = "probe-run-1";
    const auto verdicts = EvaluateProviderToolContract(contract);
    CHECK(VerdictOf(verdicts, "native_deferred")->status == "unknown");
}

TEST_CASE("全件满足(responses+async 标记+verified 探针)才判 verified") {
    ProviderToolContract contract;
    contract.provider = "openai";
    contract.wire = "responses";
    contract.model = "gpt-6";
    contract.call_marked_async = true;
    contract.native_probe_status = "verified";
    contract.native_probe_evidence = "probe-run-1";
    const auto verdicts = EvaluateProviderToolContract(contract);
    const auto* native = VerdictOf(verdicts, "native_deferred");
    REQUIRE(native != nullptr);
    CHECK(native->status == "verified");
    CHECK(native->evidence == "probe-run-1");
}

TEST_CASE("job_handle 宿主侧行为:默认 verified,装配明示禁才 unsupported") {
    ProviderToolContract contract;
    contract.provider = "openai";
    contract.wire = "chat";
    const auto verdicts = EvaluateProviderToolContract(contract);
    CHECK(VerdictOf(verdicts, "job_handle")->status == "verified");
    contract.job_handle_disabled = true;
    CHECK(VerdictOf(EvaluateProviderToolContract(contract), "job_handle")->status ==
          "unsupported");
}

TEST_CASE("parallel_tool_calls:首期保守禁(unknown 留档),显式启用才 verified") {
    ProviderToolContract contract;
    const auto verdicts = EvaluateProviderToolContract(contract);
    CHECK(VerdictOf(verdicts, "parallel_tool_calls")->status == "unknown");
    contract.parallel_tool_calls_enabled = true;
    CHECK(VerdictOf(EvaluateProviderToolContract(contract), "parallel_tool_calls")->status ==
          "verified");
}

TEST_CASE("快照载荷:basis 三件 + verdicts 逐项三态,过 P0 载荷校验") {
    ProviderToolContract contract;
    contract.provider = "openai";
    contract.wire = "responses";
    contract.model = "gpt-6";
    contract.endpoint = "https://api.example.com/v1";
    const auto verdicts = EvaluateProviderToolContract(contract);
    const nlohmann::json payload = ContractSnapshotPayload(contract, verdicts);
    REQUIRE(payload.contains("basis"));
    CHECK(payload["basis"]["provider"] == "openai");
    CHECK(payload["basis"]["wire"] == "responses");
    CHECK(payload["basis"]["model"] == "gpt-6");
    CHECK(payload["basis"]["endpoint"] == "https://api.example.com/v1");
    REQUIRE(payload.contains("verdicts"));
    REQUIRE(payload["verdicts"].is_object());
    CHECK_FALSE(payload["verdicts"].empty());
    // P0 逐 kind 载荷校验器原样吃这份(tool.capability.recorded 的合同)。
    nlohmann::json event = nlohmann::json::object({
        {"kind", "tool.capability.recorded"},
        {"eventId", "event-000001"},
        {"seq", 1},
        {"timestamp", 1},
        {"turnId", nullptr},
        {"payload", payload},
    });
    std::string ec;
    std::string msg;
    auto parsed = trajectory::v3::EventLine::FromJsonStrict(event, &ec, &msg);
    REQUIRE_MESSAGE(parsed.has_value(), ec + ": " + msg);
    const auto error = trajectory::v3::ValidateEventLine(*parsed);
    CHECK_FALSE(error.has_value());
}
