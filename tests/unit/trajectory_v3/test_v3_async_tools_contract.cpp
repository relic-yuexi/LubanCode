// 异步工具单 P0 合同测试(单行 schema 层):新事件族 kind 进注册表、
// 全部 statusless(unknown 是执行投影状态,不硬塞信封 status)、按 kind
// 的载荷合同正反例。跨行合同(前驱/重复终态/乱序/身份引用)在
// test_v3_async_tools_ledger.cpp 的 ValidateAsyncToolSequence 钉。
// 本册只钉合同形状:合同与 fixture 已验,生产未接(单 §11 P0)。
#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/schema3.hpp"

using namespace lubancode::trajectory::v3;

namespace {

// 组一行合法 event JSON 的底座(信封字段齐全;多余身份字段各 kind 自行
// 校验,不预设)。
nlohmann::json EventJson(const char* kind, nlohmann::json payload) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260912-120000-AAAAAA";
    json["runId"] = "run-000001";
    json["seq"] = 2;
    json["timestamp"] = "2026-09-12T04:59:25.314Z";
    json["eventId"] = "evt-000001";
    json["kind"] = kind;
    json["actionId"] = "action-000001";
    json["turnId"] = "turn-000001";
    json["stepId"] = "step-000001";
    json["requestId"] = "request-000001";
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

nlohmann::json WireCallRef(bool async = true) {
    return nlohmann::json::object({
        {"provider", "openai"},
        {"wire", "responses"},
        {"callId", "call_A1"},
        {"async", async},
        {"responseId", "resp_1"},
        {"itemId", "item_1"},
    });
}

nlohmann::json RegisteredPayload(const char* mode = "job_handle") {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["attempt"] = 1;
    payload["jobId"] = "job-000001";
    payload["mode"] = mode;
    payload["assistantMessageRef"] = "msg-000002";
    return payload;
}

nlohmann::json DispatchedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["attempt"] = 1;
    payload["jobId"] = "job-000001";
    payload["ownerEpoch"] = "epoch-1";
    return payload;
}

nlohmann::json ObservedPayload(const char* status = "running") {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["jobId"] = "job-000001";
    payload["observedStatus"] = status;
    return payload;
}

nlohmann::json CancelRequestedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["jobId"] = "job-000001";
    payload["reason"] = "user_escape";
    return payload;
}

nlohmann::json DeliveryPreparedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["deliveryId"] = "delivery-000001";
    payload["resultRef"] = "evt-000009";
    payload["resultVersion"] = 1;
    payload["toolMessageRef"] = "msg-000004";
    return payload;
}

nlohmann::json AcknowledgedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["deliveryId"] = "delivery-000001";
    payload["evidenceRef"] = "evt-000012";
    return payload;
}

nlohmann::json UncertainPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["tool_call_id"] = "action-000001";
    payload["deliveryId"] = "delivery-000001";
    payload["reason"] = "response_receipt_lost";
    return payload;
}

nlohmann::json CapabilityPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["basis"] = nlohmann::json::object({
        {"provider", "openai"},
        {"wire", "responses"},
        {"model", "gpt-6"},
        {"endpoint", "https://api.example.com/v1"},
    });
    payload["verdicts"] = nlohmann::json::object({
        {"native_deferred", nlohmann::json::object({{"status", "unknown"}})},
        {"job_handle", nlohmann::json::object({{"status", "verified"}, {"evidence", "host-side"}})},
    });
    return payload;
}

// 解析 + 语义校验一条 event 行;overrides 先覆盖、erases 再删键。
std::optional<Schema3Error> CheckEvent(const char* kind, nlohmann::json payload,
                                       const std::vector<std::string>& erase_keys = {}) {
    nlohmann::json json = EventJson(kind, std::move(payload));
    for (const auto& key : erase_keys) {
        json.erase(key);
    }
    std::string ec, msg;
    auto parsed = EventLine::FromJsonStrict(json, &ec, &msg);
    if (!parsed.has_value()) {
        return Schema3Error{ec, msg};
    }
    return ValidateEventLine(*parsed);
}

bool HasCode(const std::optional<Schema3Error>& error, const char* code) {
    return error.has_value() && error->code == code;
}

}  // namespace

// ---------------------------------------------------------------------------
// kind 注册表与 statusless 合同
// ---------------------------------------------------------------------------

TEST_CASE("异步工具族 kind 进注册表:名字稳定、可往返、全部 statusless") {
    const std::pair<EventKindV3, const char*> table[] = {
        {EventKindV3::ToolJobRegistered, "tool.job.registered"},
        {EventKindV3::ToolJobDispatched, "tool.job.dispatched"},
        {EventKindV3::ToolJobObserved, "tool.job.observed"},
        {EventKindV3::ToolJobCancelRequested, "tool.job.cancel_requested"},
        {EventKindV3::ToolDeliveryPrepared, "tool.delivery.prepared"},
        {EventKindV3::ToolDeliveryAcknowledged, "tool.delivery.acknowledged"},
        {EventKindV3::ToolDeliveryUncertain, "tool.delivery.uncertain"},
        {EventKindV3::ToolCapabilityRecorded, "tool.capability.recorded"},
    };
    for (const auto& [kind, name] : table) {
        CAPTURE(name);
        CHECK(std::string(EventKindV3Name(kind)) == name);
        const auto back = EventKindV3FromName(name);
        REQUIRE(back.has_value());
        CHECK(*back == kind);
        bool in_table = false;
        for (EventKindV3 all : AllEventKindsV3()) {
            if (all == kind) in_table = true;
        }
        CHECK(in_table);
        // 事实行不携带 status(单 §5:registered/dispatched/acknowledged 表示
        // 事件已发生,不等于业务 job 已完成)。
        CHECK_FALSE(RequiredStatusForKind(kind).has_value());
    }
}

TEST_CASE("statusless 合同:带 status 拒;工具/投递族缺 actionId 拒") {
    SUBCASE("observed 带 status") {
        EventLine line;
        line.kind = EventKindV3::ToolJobObserved;
        line.status = OpStatus::Unknown;  // 不硬塞信封 status——unknown 走 payload
        line.action_id = "action-000001";
        line.payload = ObservedPayload("unknown");
        auto error = ValidateEventLine(line);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.status_kind_mismatch");
    }
    SUBCASE("registered 缺 actionId") {
        auto error = CheckEvent("tool.job.registered", RegisteredPayload(), {"actionId"});
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("capability 无信封身份要求:不携 actionId 也过") {
        auto error = CheckEvent("tool.capability.recorded", CapabilityPayload(), {"actionId"});
        CHECK_FALSE(error.has_value());
    }
}

// ---------------------------------------------------------------------------
// tool.job.* 载荷合同
// ---------------------------------------------------------------------------

TEST_CASE("tool.job.registered:mode/引用/wireCallRef/attempt 正反例") {
    SUBCASE("job_handle 正例(无 wireCallRef)") {
        CHECK_FALSE(CheckEvent("tool.job.registered", RegisteredPayload("job_handle")).has_value());
    }
    SUBCASE("native_deferred 正例(带 wireCallRef+async 标记)") {
        nlohmann::json payload = RegisteredPayload("native_deferred");
        payload["wireCallRef"] = WireCallRef(true);
        CHECK_FALSE(CheckEvent("tool.job.registered", payload).has_value());
    }
    SUBCASE("inline 不注册 job:mode 枚举拒") {
        auto error = CheckEvent("tool.job.registered", RegisteredPayload("inline"));
        CHECK(HasCode(error, "schema3.bad_enum"));
    }
    SUBCASE("native_deferred 缺 wireCallRef 拒") {
        auto error = CheckEvent("tool.job.registered", RegisteredPayload("native_deferred"));
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("wireCallRef 缺 async 标记拒") {
        nlohmann::json payload = RegisteredPayload("native_deferred");
        nlohmann::json wire = WireCallRef();
        wire.erase("async");
        payload["wireCallRef"] = wire;
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
    SUBCASE("缺 jobId 拒") {
        nlohmann::json payload = RegisteredPayload();
        payload.erase("jobId");
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("缺 assistantMessageRef(originRef)拒") {
        nlohmann::json payload = RegisteredPayload();
        payload.erase("assistantMessageRef");
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("缺 turnId/stepId(originRef 落信封)拒") {
        auto error = CheckEvent("tool.job.registered", RegisteredPayload(), {"turnId"});
        CHECK(HasCode(error, "schema3.missing_field"));
        error = CheckEvent("tool.job.registered", RegisteredPayload(), {"stepId"});
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("tool_call_id 不等于信封 actionId 拒(§4.15 映射必校)") {
        nlohmann::json payload = RegisteredPayload();
        payload["tool_call_id"] = "action-999999";
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.tool_call_id_mismatch"));
    }
    SUBCASE("attempt 缺失拒(注册挂发起 Action 的执行尝试)") {
        nlohmann::json payload = RegisteredPayload();
        payload.erase("attempt");
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
    SUBCASE("approvalRequired 非 boolean 拒") {
        nlohmann::json payload = RegisteredPayload();
        payload["approvalRequired"] = "yes";
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
    SUBCASE("executionPolicy 已知键类型错拒(字段集拟议待 P1,先钉类型)") {
        nlohmann::json payload = RegisteredPayload();
        payload["executionPolicy"] = nlohmann::json::object({
            {"allow_background", true},
            {"side_effect_class", "non_idempotent"},
            {"resource_keys", nlohmann::json::array({"fs:repo"})},
            {"deadline_ms", 60000},
        });
        CHECK_FALSE(CheckEvent("tool.job.registered", payload).has_value());
        payload["executionPolicy"]["deadline_ms"] = -1;
        auto error = CheckEvent("tool.job.registered", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
}

TEST_CASE("tool.job.dispatched/observed/cancel_requested 载荷正反例") {
    SUBCASE("dispatched 正例") {
        CHECK_FALSE(CheckEvent("tool.job.dispatched", DispatchedPayload()).has_value());
    }
    SUBCASE("dispatched 缺 ownerEpoch 拒(租约代号,细节拟议待 P1)") {
        nlohmann::json payload = DispatchedPayload();
        payload.erase("ownerEpoch");
        auto error = CheckEvent("tool.job.dispatched", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("observed 正例(不带 attempt:观测可由宿主发起)") {
        CHECK_FALSE(CheckEvent("tool.job.observed", ObservedPayload("queued")).has_value());
    }
    SUBCASE("observed 结果引用与版本成对:单边拒") {
        nlohmann::json payload = ObservedPayload("succeeded");
        payload["resultRef"] = "evt-000009";
        auto error = CheckEvent("tool.job.observed", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
        payload["resultVersion"] = 0;  // 成对了但版本从 0 起:拒
        error = CheckEvent("tool.job.observed", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
        payload["resultVersion"] = 1;  // 成对且从 1 起:过
        CHECK_FALSE(CheckEvent("tool.job.observed", payload).has_value());
    }
    SUBCASE("observed 状态枚举外的值拒(投影状态口径)") {
        auto error = CheckEvent("tool.job.observed", ObservedPayload("in_flight"));
        CHECK(HasCode(error, "schema3.bad_enum"));
    }
    SUBCASE("cancel_requested 正例;缺 jobId 拒") {
        CHECK_FALSE(CheckEvent("tool.job.cancel_requested", CancelRequestedPayload()).has_value());
        nlohmann::json payload = CancelRequestedPayload();
        payload.erase("jobId");
        auto error = CheckEvent("tool.job.cancel_requested", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
}

// ---------------------------------------------------------------------------
// tool.delivery.* 载荷合同
// ---------------------------------------------------------------------------

TEST_CASE("tool.delivery.prepared/acknowledged/uncertain 载荷正反例") {
    SUBCASE("prepared 正例(deliveryId+resultRef+resultVersion+信封 requestId)") {
        CHECK_FALSE(CheckEvent("tool.delivery.prepared", DeliveryPreparedPayload()).has_value());
    }
    SUBCASE("prepared 缺信封 requestId(targetRequestId)拒") {
        auto error = CheckEvent("tool.delivery.prepared", DeliveryPreparedPayload(), {"requestId"});
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("prepared 缺 resultVersion 拒") {
        nlohmann::json payload = DeliveryPreparedPayload();
        payload.erase("resultVersion");
        auto error = CheckEvent("tool.delivery.prepared", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
    SUBCASE("prepared resultRef 非引用格式拒") {
        nlohmann::json payload = DeliveryPreparedPayload();
        payload["resultRef"] = "";  // 空串不是合法引用
        auto error = CheckEvent("tool.delivery.prepared", payload);
        CHECK(HasCode(error, "schema3.bad_ref"));
    }
    SUBCASE("acknowledged 正例(带证据引用)") {
        CHECK_FALSE(CheckEvent("tool.delivery.acknowledged", AcknowledgedPayload()).has_value());
    }
    SUBCASE("acknowledged 缺证据拒:没证据不宣称接纳") {
        nlohmann::json payload = AcknowledgedPayload();
        payload.erase("evidenceRef");
        auto error = CheckEvent("tool.delivery.acknowledged", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("uncertain 正例;缺 reason 拒") {
        CHECK_FALSE(CheckEvent("tool.delivery.uncertain", UncertainPayload()).has_value());
        nlohmann::json payload = UncertainPayload();
        payload.erase("reason");
        auto error = CheckEvent("tool.delivery.uncertain", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("uncertain 缺信封 requestId 拒") {
        auto error = CheckEvent("tool.delivery.uncertain", UncertainPayload(), {"requestId"});
        CHECK(HasCode(error, "schema3.missing_field"));
    }
}

// ---------------------------------------------------------------------------
// tool.capability.recorded 载荷合同(§4 能力三态)
// ---------------------------------------------------------------------------

TEST_CASE("tool.capability.recorded:basis 依据留档与 verdicts 三态正反例") {
    SUBCASE("正例(unknown/verified/unsupported 三态齐)") {
        nlohmann::json payload = CapabilityPayload();
        payload["verdicts"]["parallel_tool_calls"] =
            nlohmann::json::object({{"status", "unsupported"},
                                    {"evidence", "conservative-first-phase"}});
        CHECK_FALSE(CheckEvent("tool.capability.recorded", payload).has_value());
    }
    SUBCASE("缺 basis 拒") {
        nlohmann::json payload = CapabilityPayload();
        payload.erase("basis");
        auto error = CheckEvent("tool.capability.recorded", payload);
        CHECK(HasCode(error, "schema3.missing_field"));
    }
    SUBCASE("basis 缺 model 拒(判定依据三项缺一不可)") {
        nlohmann::json payload = CapabilityPayload();
        payload["basis"].erase("model");
        auto error = CheckEvent("tool.capability.recorded", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
    SUBCASE("verdicts 空对象拒") {
        nlohmann::json payload = CapabilityPayload();
        payload["verdicts"] = nlohmann::json::object();
        auto error = CheckEvent("tool.capability.recorded", payload);
        CHECK(HasCode(error, "schema3.bad_type"));
    }
    SUBCASE("verdict 状态枚举外拒") {
        nlohmann::json payload = CapabilityPayload();
        payload["verdicts"]["native_deferred"]["status"] = "maybe";
        auto error = CheckEvent("tool.capability.recorded", payload);
        CHECK(HasCode(error, "schema3.bad_enum"));
    }
}
