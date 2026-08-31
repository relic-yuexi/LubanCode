// Token 账本单 A1(事实接线)的验收冒烟:flag 开的会话跑一轮真实工具
// 回合(真 AgentLoop + 假后端),每一次 model request 都能按 request_id 对
// 上 prepared(purpose=main_turn + request_snapshot_ref 带 PromptManifest)、
// sent、output、usage owner(§16 A1 验收线);ProjectUsage 从真 Journal 投
// 出 UsageSample,manifest 从 prepared 事件解出后与 A0 冻结的 schema 对得
// 上(FromJsonStrict 过)。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "accounting/usage_projector.hpp"
#include "agent/agent.hpp"
#include "agent/prompt_manifest.hpp"
#include "agent/prompt_assembler.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "agent/resolved_prompt_builder.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/schema.hpp"

using namespace lubancode;
using trajectory::Durability;
using trajectory::RecordReceipt;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

trajectory::EventScope MainScope() {
    trajectory::EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260831-000003-AAAAAA";
    scope.run_id = "main-0001";
    scope.run_kind = trajectory::RunKind::MainSession;
    scope.visibility = {trajectory::Visibility::HostOnly};
    return scope;
}

// 假后端:两次请求(第一轮 tool_use,第二轮纯文本收口),usage 真报,
// provider 号每轮不同。
class TurnBackend final : public api::Backend {
public:
    int calls = 0;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        ++calls;
        if (calls == 1) {
            on_event(api::MessageStart{"resp-smoke-1", "test-model"});
            on_event(api::ToolUseStart{0, "call-1", "fake_read"});
            on_event(api::ToolUseInputDelta{0, "{\"path\":\"README.md\"}"});
            on_event(api::ContentBlockDone{0});
            api::MessageDone done;
            done.stop_reason = "tool_use";
            done.usage.input_tokens = 900;
            done.usage.output_tokens = 80;
            done.usage.cache_read_tokens = 1200;
            done.usage_reported = true;
            on_event(done);
            return {};
        }
        on_event(api::MessageStart{"resp-smoke-2", "test-model"});
        on_event(api::TextDelta{"读完了,共 42 行。"});
        on_event(api::ContentBlockDone{0});
        api::MessageDone done;
        done.stop_reason = "end_turn";
        done.usage.input_tokens = 1500;
        done.usage.output_tokens = 30;
        done.usage_reported = true;
        on_event(done);
        return {};
    }
};

class FakeTool final : public tools::Tool {
public:
    std::string name() const override { return "fake_read"; }
    std::string description() const override { return "smoke fake tool"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"共 42 行。", false}; }
};

}  // namespace

TEST_CASE("A1 冒烟:真实工具回合每笔请求 prepared/sent/output/usage/purpose 全对上") {
    const auto dir = FreshDir("lubancode-traj-a1-smoke");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(), [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    runtime::TrajectoryTurnBridge::Identity identity{"demo", "responses", "terminal"};
    auto bridge = std::make_unique<runtime::TrajectoryTurnBridge>(*recorder, MainScope(), identity);
    bridge->BeginTurn("turn-0001", "external_user");
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"读一下 README 并数行数"});
    bridge->RecordInput(user);

    // 真实拼装现场:ResolvedPromptBuilder 的底账进 AgentProfile(与
    // interactive_session_assembly.cpp 的装配同一条路),AgentLoop 落
    // prepared 时带真 manifest。
    agent::PromptOptions prompt_options;
    prompt_options.cwd = "D:/demo";
    prompt_options.current_date = "2026-08-31";
    const agent::ResolvedPromptBase base = agent::BuildResolvedPromptBase(prompt_options);

    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = base.text;
    profile.resolved_prompt_base = base;
    profile.purpose = accounting::RequestPurpose::MainTurn;
    profile.model_instructions = "模型目录指令一枚";

    TurnBackend backend;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::Agent loop(backend, registry, profile);

    agent::TurnWiring wiring;
    wiring.boundary_recorder = bridge.get();
    const auto outcome = loop.Run("读一下 README 并数行数", wiring);
    REQUIRE(outcome.has_value());
    REQUIRE(backend.calls == 2);
    bridge->EndTurn(true, false, "done");
    REQUIRE(bridge->recent_errors().empty());
    const auto verify = trajectory::VerifyJournalFile(dir / "main.jsonl");
    CHECK(verify.ok);

    // 逐事件对账:两笔请求各有 prepared/sent/usage/output,五件齐。
    const auto lines = trajectory::ReadJournalLines(dir / "main.jsonl");
    REQUIRE(lines.has_value());
    std::vector<trajectory::EventEnvelope> envelopes;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE(!parsed.is_discarded());
        trajectory::EventEnvelope envelope;
        REQUIRE(!trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value());
        envelopes.push_back(std::move(envelope));
    }
    std::map<std::string, int> per_request;
    std::map<std::string, const nlohmann::json*> prepared_payloads;
    for (const auto& envelope : envelopes) {
        if (!envelope.request_id.has_value()) {
            continue;
        }
        switch (envelope.kind) {
            case trajectory::EventKind::ModelRequestPrepared:
                ++per_request[*envelope.request_id];
                prepared_payloads[*envelope.request_id] = &envelope.payload;
                break;
            case trajectory::EventKind::ModelRequestSent:
            case trajectory::EventKind::ModelUsageRecorded:
            case trajectory::EventKind::ModelOutputCompleted:
                ++per_request[*envelope.request_id];
                break;
            default:
                break;
        }
    }
    REQUIRE(per_request.size() == 2);
    for (const auto& [request_id, count] : per_request) {
        INFO("request_id=", request_id);
        CHECK(count == 4);  // prepared + sent + usage owner + output completed
    }

    // 两笔 prepared 都带 purpose=main_turn 与 request_snapshot_ref;manifest
    // 解得出 A0 冻结的 schema(FromJsonStrict 过)。
    for (const auto& [request_id, payload] : prepared_payloads) {
        INFO("request_id=", request_id);
        REQUIRE(payload != nullptr);
        CHECK(payload->value("purpose", std::string()) == "main_turn");
        REQUIRE(payload->contains("request_snapshot_ref"));
        const auto snapshot = agent::RequestSnapshotMetadata::FromJsonStrict(
            payload->at("request_snapshot_ref"), nullptr);
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->content_policy == "metadata_only");
        CHECK(snapshot->request_shape.model == "test-model");
        CHECK(snapshot->request_shape.message_count > 0);
        CHECK(snapshot->request_shape.tool_count == 1);
        const auto manifest = agent::PromptManifest::FromJsonStrict(
            payload->at("request_snapshot_ref").at("prompt_manifest"), nullptr);
        REQUIRE(manifest.has_value());
        CHECK_FALSE(manifest->resolved_prompt_hash.empty());
        CHECK(manifest->resolved_prompt_tokens_estimated > 0);
        CHECK_FALSE(manifest->stable_prefix_hash.empty());
        REQUIRE_FALSE(manifest->segments.empty());
        CHECK(manifest->soul.name == "default");
        CHECK_FALSE(manifest->model_instructions.hash.empty());  // 模型目录指令真叠了
    }

    // 投影:真 Journal 投出两笔 UsageSample,purpose/cache 账齐。
    const auto projection = accounting::ProjectUsage(envelopes);
    REQUIRE(projection.ok);
    REQUIRE(projection.samples.size() == 2);
    CHECK(projection.samples[0].purpose == accounting::RequestPurpose::MainTurn);
    CHECK(projection.samples[0].usage_source == accounting::UsageSource::ProviderReported);
    REQUIRE(projection.samples[0].usage.has_value());
    CHECK(projection.samples[0].usage->cache_read_tokens == 1200);
    CHECK(projection.samples[0].request_outcome == "completed");
    CHECK(projection.samples[1].usage->input_tokens == 1500);
    CHECK(projection.samples[0].incomplete_linkage == false);

    // 折叠:主会话请求照旧折进 effective_conversation(main_turn 白名单)。
    // user(input.received)+ assistant(tool_use)与 assistant(终稿)各折一条
    // ——tool_result 走工具台账,不进对话。
    const auto fold = trajectory::FoldStreamReplay(dir / "main.jsonl");
    REQUIRE(fold.ok());
    CHECK(fold.state.effective_conversation.size() == 3);
    CHECK(fold.state.requests.size() == 2);
    for (const auto& step : fold.state.requests) {
        CHECK(step.purpose == "main_turn");
        CHECK(step.usage_recorded);
    }
}
