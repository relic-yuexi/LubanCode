// Token 账本单 A1(事实接线)的旁路落账测试:SampleModel 的公共
// ModelRequestRecorder 口 + TrajectoryBypassBridge 的小 turn 语义。
//
// 覆盖四件事:
//   1. 旁路请求(compact/抽取/起名/doctor 一族)经 SampleModel 落成
//      prepared(purpose)/sent/usage owner/output 全套事件,verify 过;
//   2. provider 没报 usage:owner 事件仍落,token 字段不现(unknown 不冒
//      充 0,§6.1.1);
//   3. UsageProjector 从这些真实事件投出 UsageSample,purpose 对得上
//      (A0 投影器吃 A1 接线的真账);
//   4. replay 折叠:旁路用途的输出不进 effective_conversation(工作产物
//      不是会话历史),main_turn 照旧折叠。
// 全部真 recorder + 假 backend + 临时目录,不出网。
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "accounting/usage_projector.hpp"
#include "agent/sample_model.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
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
    scope.session_id = "20260831-000002-AAAAAA";
    scope.run_id = "main-0001";
    scope.run_kind = trajectory::RunKind::MainSession;
    scope.visibility = {trajectory::Visibility::HostOnly};
    return scope;
}

// 假 backend:provider 号 + 正文 + usage 全给。usage 开关可关(探"没报")。
class ProbeBackend final : public api::Backend {
public:
    bool report_usage = true;
    int calls = 0;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        ++calls;
        on_event(api::MessageStart{"resp-bypass-1", "demo-model"});
        on_event(api::TextDelta{"摘要正文"});
        on_event(api::ContentBlockDone{0});
        if (report_usage) {
            api::MessageDone done;
            done.usage.input_tokens = 1200;
            done.usage.output_tokens = 180;
            done.usage.cache_read_tokens = 40;
            done.usage.output_reasoning_tokens = 60;
            on_event(done);
        } else {
            on_event(api::MessageDone{});
        }
        return {};
    }
};

struct BypassJournal {
    std::filesystem::path dir;
    std::filesystem::path stream;
};

// 开一只 v2 主 stream + 旁路桥,跑一次带 purpose 的采样,收口后回文件名。
BypassJournal RunOneBypassSample(accounting::RequestPurpose purpose, bool report_usage) {
    const auto dir = FreshDir("lubancode-traj-a1-bypass");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(), [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);

    runtime::TrajectoryTurnBridge::Identity identity{"demo", "responses", "host"};
    auto bridge = std::make_unique<runtime::TrajectoryBypassBridge>(*recorder, MainScope(), identity);

    ProbeBackend backend;
    backend.report_usage = report_usage;
    agent::SampleRequest sample;
    sample.model = "demo-model";
    sample.system = "把这段收成摘要。";
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"材料一枚"});
    sample.messages.push_back(std::move(message));
    sample.max_tokens = 512;

    agent::SampleOptions options;
    options.boundary_recorder = bridge.get();
    options.purpose = purpose;
    const agent::SampleResult result = agent::SampleModel(backend, sample, options);
    REQUIRE(result.ok);
    REQUIRE(result.provider_response_id == "resp-bypass-1");
    REQUIRE(bridge->recent_errors().empty());
    return BypassJournal{dir, dir / "main.jsonl"};
}

std::vector<nlohmann::json> EventsOf(const std::filesystem::path& stream) {
    std::vector<nlohmann::json> events;
    const auto lines = trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        events.push_back(nlohmann::json::parse(line, nullptr, false));
    }
    return events;
}

std::vector<trajectory::EventEnvelope> EnvelopesOf(const std::filesystem::path& stream) {
    std::vector<trajectory::EventEnvelope> envelopes;
    for (const auto& parsed : EventsOf(stream)) {
        trajectory::EventEnvelope envelope;
        REQUIRE(!trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value());
        envelopes.push_back(std::move(envelope));
    }
    return envelopes;
}

}  // namespace

TEST_CASE("旁路采样全流:purpose 落 prepared,usage owner 与 output 收口,verify 过") {
    const auto journal = RunOneBypassSample(accounting::RequestPurpose::CompactMap, /*report_usage=*/true);
    const std::vector<nlohmann::json> events = EventsOf(journal.stream);
    std::vector<std::string> kinds;
    const nlohmann::json* prepared = nullptr;
    const nlohmann::json* usage = nullptr;
    for (const auto& event : events) {
        kinds.push_back(event.value("kind", std::string()));
        if (event.value("kind", std::string()) == "model.request.prepared") {
            prepared = &event;
        }
        if (event.value("kind", std::string()) == "model.usage.recorded") {
            usage = &event;
        }
    }
    // 小 turn 语义:scheduled_host 开场,input(请求自己的首条 user 消息)
    // 先于 sent(状态机约束 3),usage owner 先于 output 收口,turn 终态殿后。
    REQUIRE(kinds.size() == 8);
    CHECK(kinds[0] == "run.started");
    CHECK(kinds[1] == "turn.started");
    CHECK(kinds[2] == "input.received");
    CHECK(kinds[3] == "model.request.prepared");
    CHECK(kinds[4] == "model.request.sent");
    CHECK(kinds[5] == "model.usage.recorded");
    CHECK(kinds[6] == "model.output.completed");
    CHECK(kinds[7] == "turn.completed");
    REQUIRE(prepared != nullptr);
    CHECK(prepared->at("payload").value("purpose", std::string()) == "compact_map");
    CHECK(prepared->at("payload").value("model", std::string()) == "demo-model");
    CHECK(prepared->at("payload").contains("request_snapshot_ref"));
    CHECK(prepared->at("payload").contains("request_snapshot_sha256"));
    REQUIRE(usage != nullptr);
    CHECK(usage->at("payload").value("reported_by_provider", false));
    CHECK(usage->at("payload").value("input_tokens", std::int64_t{0}) == 1200);
    CHECK(usage->at("payload").value("provider_response_id", std::string()) == "resp-bypass-1");
    // completed 不复制 usage 正文(v2 规矩)。
    for (const auto& event : events) {
        if (event.value("kind", std::string()) == "model.output.completed") {
            CHECK_FALSE(event.at("payload").contains("usage"));
        }
    }
    const auto report = trajectory::VerifyJournalFile(journal.stream);
    CHECK(report.ok);
}

TEST_CASE("provider 没报:usage owner 仍落,token 字段不现(unknown 不冒充 0)") {
    const auto journal = RunOneBypassSample(accounting::RequestPurpose::TitleRefine,
                                            /*report_usage=*/false);
    const auto events = EventsOf(journal.stream);
    const nlohmann::json* usage = nullptr;
    for (const auto& event : events) {
        if (event.value("kind", std::string()) == "model.usage.recorded") {
            usage = &event;
        }
    }
    REQUIRE(usage != nullptr);
    CHECK_FALSE(usage->at("payload").value("reported_by_provider", true));
    for (const char* field : {"input_tokens", "cache_read_tokens", "cache_creation_tokens",
                              "output_tokens", "reasoning_tokens"}) {
        CHECK_FALSE(usage->at("payload").contains(field));
    }
    const auto report = trajectory::VerifyJournalFile(journal.stream);
    CHECK(report.ok);
}

TEST_CASE("投影:旁路真账投出 UsageSample,purpose/usage_source 对得上") {
    const auto journal = RunOneBypassSample(accounting::RequestPurpose::MemoryExtract,
                                            /*report_usage=*/true);
    const auto projection = accounting::ProjectUsage(EnvelopesOf(journal.stream));
    REQUIRE(projection.ok);
    REQUIRE(projection.samples.size() == 1);
    const auto& sample = projection.samples.front();
    CHECK(sample.purpose == accounting::RequestPurpose::MemoryExtract);
    CHECK(sample.usage_source == accounting::UsageSource::ProviderReported);
    CHECK(sample.request_outcome == "completed");
    REQUIRE(sample.usage.has_value());
    CHECK(sample.usage->input_tokens == 1200);
    CHECK(sample.usage->output_tokens == 180);
    REQUIRE(sample.provider_response_id.has_value());
    CHECK(*sample.provider_response_id == "resp-bypass-1");
    CHECK(sample.incomplete_linkage == false);
    CHECK(sample.run_kind == "main_session");
    REQUIRE(sample.turn_id.has_value());
    CHECK_FALSE(sample.turn_id->empty());
}

TEST_CASE("折叠:旁路输出不进 effective_conversation,main_turn 照旧") {
    const auto journal = RunOneBypassSample(accounting::RequestPurpose::CompactReduce,
                                            /*report_usage=*/true);
    const auto fold = trajectory::FoldStreamReplay(journal.stream);
    REQUIRE(fold.ok());
    // 旁路 turn 的 input 是桥记的宿主材料(user),output 不折成 assistant
    // ——压缩结果是宿主吃掉的工作产物,不是会话历史。
    CHECK(fold.state.effective_conversation.empty());
    REQUIRE(fold.state.requests.size() == 1);
    CHECK(fold.state.requests.front().purpose == "compact_reduce");
    CHECK(fold.state.requests.front().usage_recorded);
    CHECK(fold.state.requests.front().output_state == "completed");
    CHECK(fold.state.turns.size() == 1);
    CHECK(fold.state.turns.front().terminal_state == "turn.completed");

    // 白名单本身:对话轮照旧折,旁路用途全拦,purpose 空的旧账照旧折。
    CHECK(trajectory::PurposeFoldsIntoConversation(""));
    CHECK(trajectory::PurposeFoldsIntoConversation("main_turn"));
    CHECK(trajectory::PurposeFoldsIntoConversation("subagent_turn"));
    CHECK(trajectory::PurposeFoldsIntoConversation("goal_continue"));
    CHECK(trajectory::PurposeFoldsIntoConversation("loop_iteration"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("compact_map"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("compact_reduce"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("memory_extract"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("title_refine"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("doctor_probe"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("insights_model_review"));
    CHECK_FALSE(trajectory::PurposeFoldsIntoConversation("other_host_request"));
}

TEST_CASE("旁路桥一桥一采样:turn 没收口又来 prepared 就拒收") {
    const auto dir = FreshDir("lubancode-traj-a1-bypass-reuse");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(), [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    runtime::TrajectoryTurnBridge::Identity identity{"demo", "responses", "host"};
    runtime::TrajectoryBypassBridge bridge(*recorder, MainScope(), identity);

    api::Request request;
    request.model = "demo-model";
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"第一笔"});
    request.messages.push_back(std::move(message));
    agent::RequestPreparedContext ctx;
    ctx.purpose = accounting::RequestPurpose::DoctorProbe;
    const std::string first = bridge.OnRequestPrepared(request, ctx);
    REQUIRE_FALSE(first.empty());
    // 没走 output 收口(turn 还开着)就复用同一只桥:拒收,不混账。
    CHECK(bridge.OnRequestPrepared(request, ctx).empty());
    bridge.OnRequestSent(first);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"ok"});
    CHECK(bridge.OnOutputCompleted(first, assistant, "end_turn", "resp-x"));
    // 收口之后可以再开第二只小 turn(工厂再调一只的语义由调用方保证;
    // 这里验桥自身收口后状态复位)。
    const std::string second = bridge.OnRequestPrepared(request, ctx);
    CHECK_FALSE(second.empty());
    bridge.OnRequestSent(second);
    bridge.OnOutputFailed(second, "boom");
}
