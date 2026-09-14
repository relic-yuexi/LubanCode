// 提示组合事实行(应用Worker接入单 §五 134)的合同册:kind 注册、
// statusless、按 kind 的载荷合同正反例(与 gateway 合同册同规矩)。
// 生产接线在 app-server 部署档组合路(server.cpp 落账,agent_wiring 出账),
// 本册只钉 schema 层合同:promptSnapshotId/agentRef/segments 的形状与
// 次序连续性。
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

nlohmann::json EventJson(const char* kind, nlohmann::json payload) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260914-090000-PC0001";
    json["runId"] = "run-000001";
    json["seq"] = 3;
    json["timestamp"] = "2026-09-14T01:00:00.000Z";
    json["eventId"] = "evt-000002";
    json["kind"] = kind;
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

nlohmann::json CompositionPayload() {
    return nlohmann::json::object({
        {"promptSnapshotId", std::string(64, 'a')},
        {"agentRef", "research"},
        {"segments",
         nlohmann::json::array({
             nlohmann::json::object({{"order", 0},
                                     {"refPath", "core/10-identity.md"},
                                     {"origin", "user profile"},
                                     {"source", "/materials/prompts/profiles/research/core/10-identity.md"},
                                     {"contentSha256", std::string(64, 'b')}}),
             nlohmann::json::object({{"order", 1},
                                     {"refPath", "(runtime environment)"},
                                     {"origin", "runtime environment"},
                                     {"source", ""},
                                     {"contentSha256", std::string(64, 'c')}}),
             nlohmann::json::object({{"order", 2},
                                     {"refPath", "modes/default.md"},
                                     {"origin", "embedded host policy"},
                                     {"source", ""},
                                     {"contentSha256", std::string(64, 'd')}}),
         })},
    });
}

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

TEST_CASE("注册表:prompt.composition.applied 在册且 statusless") {
    CHECK(EventKindV3FromName("prompt.composition.applied") == EventKindV3::PromptCompositionApplied);
    CHECK_FALSE(RequiredStatusForKind(EventKindV3::PromptCompositionApplied).has_value());
    bool in_all = false;
    for (EventKindV3 kind : AllEventKindsV3()) {
        if (kind == EventKindV3::PromptCompositionApplied) {
            in_all = true;
        }
    }
    CHECK(in_all);
    // 带 status 反被拒(§2.2"不携带"分支,同 state.goal.applied 族)。
    nlohmann::json json = EventJson("prompt.composition.applied", CompositionPayload());
    json["status"] = "done";
    std::string ec, msg;
    auto parsed = EventLine::FromJsonStrict(json, &ec, &msg);
    if (parsed.has_value()) {
        CHECK(HasCode(ValidateEventLine(*parsed), "schema3.status_kind_mismatch"));
    }
}

TEST_CASE("prompt.composition.applied:正例过,缺字段/坏形状拒") {
    CHECK_FALSE(CheckEvent("prompt.composition.applied", CompositionPayload()).has_value());

    // agentRef 空串合法(档没点名走默认人格)。
    {
        auto payload = CompositionPayload();
        payload["agentRef"] = "";
        CHECK_FALSE(CheckEvent("prompt.composition.applied", std::move(payload)).has_value());
    }
    // segments 空数组合法(防御形状;生产组合恒非空)。
    {
        auto payload = CompositionPayload();
        payload["segments"] = nlohmann::json::array();
        CHECK_FALSE(CheckEvent("prompt.composition.applied", std::move(payload)).has_value());
    }
    // 顶层字段逐一缺:missing_field / bad_type。
    CHECK(HasCode(CheckEvent("prompt.composition.applied", CompositionPayload(), {"payload"}),
                  "schema3.missing_field"));
    {
        auto payload = CompositionPayload();
        payload.erase("promptSnapshotId");
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
    {
        auto payload = CompositionPayload();
        payload["promptSnapshotId"] = "not-hex";
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
    {
        auto payload = CompositionPayload();
        payload.erase("agentRef");
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.missing_field"));
    }
    {
        auto payload = CompositionPayload();
        payload.erase("segments");
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.missing_field"));
    }
    // 段字段缺:missing_field。
    {
        auto payload = CompositionPayload();
        payload["segments"][1].erase("contentSha256");
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.missing_field"));
    }
    // 次序不连续(order 从 1 起)拒。
    {
        auto payload = CompositionPayload();
        payload["segments"][0]["order"] = 1;
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
    // 段 hash 非 hex64 拒。
    {
        auto payload = CompositionPayload();
        payload["segments"][0]["contentSha256"] = "zz";
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
    // refPath 空 / origin 空拒。
    {
        auto payload = CompositionPayload();
        payload["segments"][2]["refPath"] = "";
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
    {
        auto payload = CompositionPayload();
        payload["segments"][2]["origin"] = "";
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
    // source 须为串(嵌入段空串合法;非串拒)。
    {
        auto payload = CompositionPayload();
        payload["segments"][0]["source"] = 42;
        CHECK(HasCode(CheckEvent("prompt.composition.applied", std::move(payload)),
                      "schema3.bad_type"));
    }
}
