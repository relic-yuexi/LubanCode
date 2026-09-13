// 常驻总装 V0 第二件事:Gateway 域关联事实的合同册(单行 schema 层)。
// 与 tool.job 族同规矩:kind 进注册表、statusless(事实行不带信封
// status)、按 kind 的载荷合同正反例。生产装配归 V1,本册只钉合同形状。
//   - gateway.work.bound:work↔turn 绑定事实(恢复器凭 workId 反查原轮,
//     不另派新轮);workId/sourceKind/sourceId/ownerEpoch 必带且非空,
//     attempt 从 1 起,信封 turnId 必带,inputRef 可选(带则合法引用)。
//   - reply.selection.committed:回复选择提交(selectionId 恢复后不变,
//     不重新散列投递身份);selectionId/deliveryTarget/formatVersion 非空,
//     ordinal 从 1 起,sourceMessageRef 必带,artifactRef 六键指最终正文
//     原件,completedEventRef 可选,信封 turnId 必带。
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
    json["sessionId"] = "20260913-090000-GW0001";
    json["runId"] = "run-000001";
    json["seq"] = 2;
    json["timestamp"] = "2026-09-13T01:00:00.000Z";
    json["eventId"] = "evt-000001";
    json["kind"] = kind;
    json["turnId"] = "turn-000001";
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

nlohmann::json WorkBoundPayload() {
    return nlohmann::json::object({
        {"workId", "gw:default:ingress:qqa-114514"},
        {"sourceKind", "channel_ingress"},
        {"sourceId", "qq:b-10086:evt-9"},
        {"ownerEpoch", "gw-18f2-3c-1"},
        {"attempt", 1},
        {"inputRef", "evt-000001"},
    });
}

nlohmann::json ReplySelectionPayload() {
    return nlohmann::json::object({
        {"selectionId", "sel-000001"},
        {"deliveryTarget", "local:file"},
        {"formatVersion", "text/plain@1"},
        {"ordinal", 1},
        {"sourceMessageRef", "msg-000042"},
        {"artifactRef",
         nlohmann::json::object({
             {"artifactId", "art-000001"},
             {"kind", "report"},
             {"path", "artifacts/replies/sel-000001.txt"},
             {"sha256", std::string(64, 'a')},
             {"bytes", 128},
             {"mediaType", "text/plain"},
         })},
        {"completedEventRef", "evt-000040"},
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

TEST_CASE("注册表:gateway 域两个 kind 在册且 statusless") {
    CHECK(EventKindV3FromName("gateway.work.bound") == EventKindV3::GatewayWorkBound);
    CHECK(EventKindV3FromName("reply.selection.committed") == EventKindV3::ReplySelectionCommitted);
    CHECK_FALSE(RequiredStatusForKind(EventKindV3::GatewayWorkBound).has_value());
    CHECK_FALSE(RequiredStatusForKind(EventKindV3::ReplySelectionCommitted).has_value());
    bool in_all = false;
    for (EventKindV3 kind : AllEventKindsV3()) {
        if (kind == EventKindV3::GatewayWorkBound || kind == EventKindV3::ReplySelectionCommitted) {
            in_all = true;
        }
    }
    CHECK(in_all);
}

TEST_CASE("gateway.work.bound:正例过,缺载荷字段/缺 turnId/坏 attempt 拒") {
    CHECK_FALSE(CheckEvent("gateway.work.bound", WorkBoundPayload()).has_value());

    // 四枚身份字段逐一缺:missing_field。
    for (const char* key : {"workId", "sourceKind", "sourceId", "ownerEpoch"}) {
        auto payload = WorkBoundPayload();
        payload.erase(key);
        CHECK(HasCode(CheckEvent("gateway.work.bound", std::move(payload)),
                      "schema3.missing_field"));
    }
    // attempt 从 1 起。
    {
        auto payload = WorkBoundPayload();
        payload["attempt"] = 0;
        CHECK(HasCode(CheckEvent("gateway.work.bound", std::move(payload)), "schema3.bad_type"));
    }
    {
        auto payload = WorkBoundPayload();
        payload.erase("attempt");
        CHECK(HasCode(CheckEvent("gateway.work.bound", std::move(payload)), "schema3.bad_type"));
    }
    // 信封 turnId 必带(预留 turn 身份是恢复反查的锚)。
    CHECK(HasCode(CheckEvent("gateway.work.bound", WorkBoundPayload(), {"turnId"}),
                  "schema3.missing_field"));
    // inputRef 可选;带则须为合法引用(非空 string 或五键对象)。
    {
        auto payload = WorkBoundPayload();
        payload["inputRef"] = "";
        CHECK(HasCode(CheckEvent("gateway.work.bound", std::move(payload)), "schema3.bad_ref"));
    }
    {
        auto payload = WorkBoundPayload();
        payload.erase("inputRef");  // 缺省合法
        CHECK_FALSE(CheckEvent("gateway.work.bound", std::move(payload)).has_value());
    }
    // 空串 workId 也拒(CheckStringField 非空)。
    {
        auto payload = WorkBoundPayload();
        payload["workId"] = "";
        CHECK(HasCode(CheckEvent("gateway.work.bound", std::move(payload)), "schema3.bad_type"));
    }
}

TEST_CASE("reply.selection.committed:正例过,缺字段/坏 ordinal/坏 artifactRef 拒") {
    CHECK_FALSE(CheckEvent("reply.selection.committed", ReplySelectionPayload()).has_value());

    for (const char* key : {"selectionId", "deliveryTarget", "formatVersion"}) {
        auto payload = ReplySelectionPayload();
        payload.erase(key);
        CHECK(HasCode(CheckEvent("reply.selection.committed", std::move(payload)),
                      "schema3.missing_field"));
    }
    // ordinal 从 1 起(同轮多段回复的次序)。
    {
        auto payload = ReplySelectionPayload();
        payload["ordinal"] = 0;
        CHECK(HasCode(CheckEvent("reply.selection.committed", std::move(payload)),
                      "schema3.bad_type"));
    }
    // sourceMessageRef 必带且为合法引用。
    {
        auto payload = ReplySelectionPayload();
        payload.erase("sourceMessageRef");
        CHECK(HasCode(CheckEvent("reply.selection.committed", std::move(payload)),
                      "schema3.missing_field"));
    }
    // artifactRef 六键齐全,sha256 须 64 位十六进制。
    {
        auto payload = ReplySelectionPayload();
        payload["artifactRef"]["sha256"] = "not-hex";
        CHECK(HasCode(CheckEvent("reply.selection.committed", std::move(payload)),
                      "schema3.bad_ref"));
    }
    {
        auto payload = ReplySelectionPayload();
        payload.erase("artifactRef");
        CHECK(HasCode(CheckEvent("reply.selection.committed", std::move(payload)),
                      "schema3.missing_field"));
    }
    // completedEventRef 可选;缺省合法。
    {
        auto payload = ReplySelectionPayload();
        payload.erase("completedEventRef");
        CHECK_FALSE(CheckEvent("reply.selection.committed", std::move(payload)).has_value());
    }
    // 信封 turnId 必带。
    CHECK(HasCode(CheckEvent("reply.selection.committed", ReplySelectionPayload(), {"turnId"}),
                  "schema3.missing_field"));
}
