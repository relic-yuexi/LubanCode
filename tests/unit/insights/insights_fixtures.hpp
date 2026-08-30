// 十二场合成轨迹夹具(Token 账本单 §15.2 A0)。
//
// 全走真 TrajectoryRecorder(v2 为主,legacy 一场走 v1),固定钟,不接真实
// runtime、不烧模型。/insights 管线的 byte-stable golden 与 usage 投影的
// 行为断言都吃这批夹具。夹具是测试代码,不算产品能力。
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "trajectory/recorder.hpp"

namespace lubancode::insights_fixtures {

using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::EventScope;
using trajectory::Origin;
using trajectory::RecordReceipt;
using trajectory::RunKind;
using trajectory::TrajectoryRecorder;
using trajectory::Visibility;

class FixedClock : public trajectory::RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

// 一笔 usage 的入参。reported=false 时 owner 事件只带 attempt 与
// reported_by_provider=false,token 字段不出现。
struct UsageSpec {
    bool reported = true;
    std::int64_t input = 0;
    std::int64_t cache_read = 0;
    std::int64_t cache_creation = 0;
    std::int64_t output = 0;
    std::int64_t reasoning = 0;
    std::string provider_response_id;
    std::optional<int> cache_epoch;
};

// 一条 stream 的写夹具:把 recorder 的提交流程包成场景动作。
class FixtureStream {
public:
    FixtureStream(const std::filesystem::path& stream_path, const std::filesystem::path& artifacts,
                  std::string workspace_key, std::string session_id, std::string run_id,
                  RunKind kind, int schema_version, const FixedClock& clock)
        : schema_version_(schema_version) {
        trajectory::RecorderOptions options;
        options.event_schema_version = schema_version;
        auto started = TrajectoryRecorder::Start(stream_path, artifacts, Base(std::move(workspace_key),
                                                                       std::move(session_id),
                                                                       std::move(run_id), kind),
                                                 options, &clock);
        recorder_ = std::move(started).value();
    }

    TrajectoryRecorder& recorder() { return *recorder_; }

    void StartRun(const std::string& reason = "process_launch") {
        auto receipt = recorder_->WriteRunStarted(nlohmann::json{{"start_reason", reason}},
                                                  Durability::PowerLoss);
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    void StartTurn(const std::string& turn, const std::string& trigger = "external_user") {
        auto receipt = Put(EventKind::TurnStarted, TurnScope(turn),
                           nlohmann::json{{"trigger", trigger}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        EventScope scope = TurnScope(turn);
        scope.actor = Actor::User;
        scope.origin = Origin::ExternalUser;
        scope.visibility = {Visibility::UserVisible, Visibility::ModelInput};
        receipt = Put(EventKind::InputReceived, scope,
                      nlohmann::json{{"input_id", "input-" + turn},
                                     {"content", nlohmann::json::array({"text"})},
                                     {"channel", "terminal"},
                                     {"sender", nlohmann::json{{"kind", "local_user"}}}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    // 一份完整模型往返:prepared -> sent -> usage owner(v2) -> output 终态。
    // fail=true 走 ModelOutputFailed(带 attempt),cancelled 同理。
    void ModelExchange(const std::string& turn, const std::string& request, const std::string& purpose,
                       const UsageSpec& usage, bool with_tool_call = false,
                       const std::string& call_id = "", int attempt = 1, bool fail = false,
                       bool cancelled = false,
                       const nlohmann::json& snapshot_ref = nlohmann::json()) {
        EventScope prep = TurnScope(turn, request);
        prep.actor = Actor::Model;
        prep.origin = Origin::ProviderModel;
        nlohmann::json payload{{"model", "gpt-5.6-sol"},
                               {"provider", "ccmoon"},
                               {"wire", "responses"},
                               {"message_refs", nlohmann::json::array()},
                               {"purpose", purpose}};
        if (!snapshot_ref.is_null()) {
            payload["request_snapshot_ref"] = snapshot_ref;
        }
        auto prepared = Put(EventKind::ModelRequestPrepared, prep, std::move(payload));
        REQUIRE(prepared.status == RecordReceipt::Status::Committed);
        auto sent = Put(EventKind::ModelRequestSent, TurnScope(turn, request),
                        nlohmann::json{{"prepared_event_id", prepared.event_id},
                                       {"attempt", static_cast<std::uint64_t>(attempt)}});
        REQUIRE(sent.status == RecordReceipt::Status::Committed);
        // usage owner(不分成败,provider 报了就记)。
        nlohmann::json usage_payload{{"attempt", static_cast<std::uint64_t>(attempt)},
                                     {"reported_by_provider", usage.reported}};
        if (usage.reported) {
            usage_payload["input_tokens"] = usage.input;
            usage_payload["cache_read_tokens"] = usage.cache_read;
            usage_payload["cache_creation_tokens"] = usage.cache_creation;
            usage_payload["output_tokens"] = usage.output;
            usage_payload["reasoning_tokens"] = usage.reasoning;
            if (usage.cache_epoch.has_value()) {
                usage_payload["cache_epoch"] = static_cast<std::uint64_t>(*usage.cache_epoch);
            }
        }
        if (!usage.provider_response_id.empty()) {
            usage_payload["provider_response_id"] = usage.provider_response_id;
        }
        if (schema_version_ > 1) {
            auto usage_receipt = Put(EventKind::ModelUsageRecorded, TurnScope(turn, request),
                                     std::move(usage_payload));
            REQUIRE(usage_receipt.status == RecordReceipt::Status::Committed);
        }
        nlohmann::json blocks = nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                                      {"text", "先查文件"}}});
        if (with_tool_call) {
            blocks.push_back(nlohmann::json{{"type", "tool_call"},
                                            {"call_id", call_id},
                                            {"name", "read_file"},
                                            {"arguments", nlohmann::json{{"path", "src/a.cpp"}}}});
        }
        if (fail) {
            auto receipt = Put(EventKind::ModelOutputFailed, TurnScope(turn, request),
                               nlohmann::json{{"reason", "provider_error"},
                                              {"attempt", static_cast<std::uint64_t>(attempt)}});
            REQUIRE(receipt.status == RecordReceipt::Status::Committed);
            return;
        }
        if (cancelled) {
            auto receipt = Put(EventKind::ModelOutputCancelled, TurnScope(turn, request),
                               nlohmann::json{{"reason", "user_interrupt"}});
            REQUIRE(receipt.status == RecordReceipt::Status::Committed);
            return;
        }
        nlohmann::json completed{{"output_id", "output-" + request},
                                 {"blocks", blocks},
                                 {"stop_reason", with_tool_call ? "tool_use" : "end_turn"}};
        if (schema_version_ == 1 && !usage_null_v1_) {
            // v1:usage 挂 completed(v2 不复制)。
            nlohmann::json usage_json = nlohmann::json::object();
            if (usage.reported) {
                usage_json["input_tokens"] = usage.input;
                usage_json["cache_read_tokens"] = usage.cache_read;
                usage_json["cache_creation_tokens"] = usage.cache_creation;
                usage_json["output_tokens"] = usage.output;
                usage_json["output_reasoning_tokens"] = usage.reasoning;
            }
            completed["usage"] = std::move(usage_json);
        }
        auto receipt = Put(EventKind::ModelOutputCompleted, TurnScope(turn, request),
                           std::move(completed));
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    void RunTool(const std::string& turn, const std::string& call_id, bool error = false) {
        auto receipt = Put(EventKind::ToolExecutionPlanned, TurnScope(turn, "", call_id),
                           nlohmann::json{{"call_id", call_id}, {"tool_name", "read_file"}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        nlohmann::json effective{{"call_id", call_id},
                                 {"tool_name", "read_file"},
                                 {"source_kind", "builtin"},
                                 {"effect_class", "read_only"},
                                 {"effective_arguments", nlohmann::json::object()},
                                 {"effective_arguments_sha256", std::string(64, '0')}};
        receipt = Put(EventKind::ToolInputEffective, TurnScope(turn, "", call_id), std::move(effective));
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        receipt = Put(EventKind::ToolExecutionStarted, TurnScope(turn, "", call_id),
                      nlohmann::json{{"call_id", call_id}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        if (error) {
            receipt = Put(EventKind::ToolExecutionFailed, TurnScope(turn, "", call_id),
                          nlohmann::json{{"reason", "missing_file"}, {"duration_ms", 3}});
            REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        } else {
            receipt = Put(EventKind::ToolExecutionFinished, TurnScope(turn, "", call_id),
                          nlohmann::json{{"outcome", "succeeded"}, {"duration_ms", 5}});
            REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        }
        receipt = Put(EventKind::ToolResultCommitted, TurnScope(turn, "", call_id),
                      nlohmann::json{{"call_id", call_id},
                                     {"content", nlohmann::json::array({"text"})},
                                     {"is_error", error}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    void RecordVerification(const std::string& turn, const std::string& verification_id,
                            bool passed) {
        EventScope scope = TurnScope(turn);
        scope.actor = Actor::Verifier;
        scope.origin = Origin::VerifierHost;
        auto receipt = Put(EventKind::VerificationRecorded, scope,
                           nlohmann::json{{"verification_id", verification_id},
                                          {"kind", "build"},
                                          {"passed", passed},
                                          {"producer", "ctest"}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    void AssessOutcome(const std::string& turn, const std::string& outcome) {
        auto receipt = Put(EventKind::OutcomeAssessed, TurnScope(turn),
                           nlohmann::json{{"outcome", outcome}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    void EndTurn(const std::string& turn, bool completed = true) {
        auto receipt = Put(completed ? EventKind::TurnCompleted : EventKind::TurnFailed,
                           TurnScope(turn), completed
                                               ? nlohmann::json{{"outcome", "passed"}}
                                               : nlohmann::json{{"reason", "incomplete"}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }

    // run 终态(+main stream 补 session.ended),关柄。
    void Seal(const std::string& reason = "normal_exit") {
        auto receipt = recorder_->FinishRun(EventKind::RunCompleted, reason, Durability::PowerLoss);
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        if (recorder_->base_scope().run_kind == RunKind::MainSession) {
            receipt = recorder_->EndSession(reason, std::nullopt, "clean", Durability::PowerLoss);
            REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        }
        (void)recorder_->Close();
    }

    RecordReceipt Put(EventKind kind, EventScope scope, nlohmann::json payload) {
        trajectory::RecordRequest request;
        request.kind = kind;
        request.scope = std::move(scope);
        request.payload = std::move(payload);
        return recorder_->Record(std::move(request), Durability::ProcessCrash);
    }

    void set_v1_usage_null() { usage_null_v1_ = true; }

private:
    static EventScope Base(std::string workspace_key, std::string session_id, std::string run_id,
                           RunKind kind) {
        EventScope scope;
        scope.workspace_key = std::move(workspace_key);
        scope.session_id = std::move(session_id);
        scope.run_id = std::move(run_id);
        scope.run_kind = kind;
        scope.visibility = {Visibility::HostOnly};
        return scope;
    }

    EventScope TurnScope(const std::string& turn, std::optional<std::string> request = std::nullopt,
                         std::optional<std::string> call = std::nullopt) {
        EventScope scope = recorder_->base_scope();
        if (!turn.empty()) {
            scope.turn_id = turn;
        }
        scope.request_id = std::move(request);
        scope.call_id = std::move(call);
        return scope;
    }

    std::optional<TrajectoryRecorder> recorder_;
    int schema_version_ = 2;
    bool usage_null_v1_ = false;  // v1 夹具造"provider 没报"时 completed 不带 usage 键
};

// 一间 session 的产物账。
struct FixtureSession {
    std::filesystem::path dir;
    std::string session_id;
    std::vector<std::filesystem::path> streams;
    bool sealed = true;   // false = active(未封口)
};

inline std::filesystem::path PrepareDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    return dir;
}

}  // namespace lubancode::insights_fixtures
