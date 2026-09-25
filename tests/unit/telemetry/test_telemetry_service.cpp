// TelemetryService 测试(端云协同可观测单 §26 生命周期/§14 投影读路/
// §18.5 崩溃恢复/§8.5 默认关闭,实施分期 T1 验收线"丢 wake、杀进程、截
// active segment、填满磁盘,Agent 仍能运行;投影可补账或明确 quarantine"):
//   - 装配门:flag 关零副作用;telemetry 开 trajectory 关 → 明拒;
//   - committed wake -> 增量投影 -> sealed 段 + cursor 推进(只在 durable 后);
//   - 丢 wake:周期 tick 补账(§14.1);
//   - 崩溃修复:cursor 文件丢(seal 后没写成)→ 重开修前推,不重投;
//   - cursor 孤儿:超前无账 → 停投报错,服务不倒;
//   - 满盘/IO 坏:degraded 停收,Agent 侧 Notify 照常;
//   - projection_key 归属:state.json 持有、重启沿用;projector 版本不
//     兼容 → generation+1 换钥匙另开账(§27.2);
//   - final flush:开着的 span 在 Stop 后按 missing 收口入账。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "telemetry/service.hpp"
#include "telemetry/spool.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/writer.hpp"

#ifdef _WIN32
#include <share.h>
#endif

using namespace lubancode::telemetry;
using namespace lubancode::trajectory;
namespace v3 = lubancode::trajectory::v3;

namespace {

class FixedClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

// 一场最小 session 的 Journal 桩(照 projector 测试的 Harness 裁剪)。
struct JournalFixture {
    FixedClock clock;
    std::filesystem::path session_dir;
    std::optional<TrajectoryRecorder> recorder;
    std::string workspace_key = "ws-000000000000";
    std::string session_id = "20260901-000000-TEL01";
    std::string run_id = "main-tel-1";

    explicit JournalFixture(const char* tag) {
        session_dir = std::filesystem::temp_directory_path() /
                      ("lubancode-tel-svc-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(session_dir, ec);
        std::filesystem::create_directories(session_dir / "artifacts", ec);
        EventScope scope;
        scope.workspace_key = workspace_key;
        scope.session_id = session_id;
        scope.run_id = run_id;
        scope.run_kind = RunKind::MainSession;
        scope.visibility = {Visibility::HostOnly};
        auto started = TrajectoryRecorder::Start(session_dir / "main.jsonl",
                                                 session_dir / "artifacts", scope,
                                                 RecorderOptions{}, &clock);
        REQUIRE(started.has_value());
        recorder = std::move(*started);
        const RecordReceipt run_started = recorder->WriteRunStarted(
            nlohmann::json{{"start_reason", "process_launch"}}, Durability::ProcessCrash);
        REQUIRE(run_started.status == RecordReceipt::Status::Committed);
    }

    EventScope Scope(std::optional<std::string> turn, std::optional<std::string> request,
                     std::optional<std::string> call) {
        EventScope scope;
        scope.workspace_key = workspace_key;
        scope.session_id = session_id;
        scope.run_id = run_id;
        scope.visibility = {Visibility::HostOnly};
        scope.turn_id = std::move(turn);
        scope.request_id = std::move(request);
        scope.call_id = std::move(call);
        return scope;
    }

    RecordReceipt Put(EventKind kind, EventScope scope, nlohmann::json payload) {
        RecordRequest request;
        request.kind = kind;
        request.scope = std::move(scope);
        request.payload = std::move(payload);
        RecordReceipt receipt = recorder->Record(std::move(request), Durability::ProcessCrash);
        REQUIRE_MESSAGE(receipt.status == RecordReceipt::Status::Committed, receipt.error_code);
        return receipt;
    }

    // 一轮完整回合:turn 起 -> input -> 模型往返 -> turn 收。
    void CompleteTurn(const std::string& turn) {
        Put(EventKind::TurnStarted, Scope(turn, std::nullopt, std::nullopt),
            nlohmann::json{{"trigger", "external_user"}});
        EventScope input_scope = Scope(turn, std::nullopt, std::nullopt);
        input_scope.actor = Actor::User;
        input_scope.origin = Origin::ExternalUser;
        Put(EventKind::InputReceived, input_scope,
            nlohmann::json{{"input_id", "input-0001"},
                           {"content", nlohmann::json::array({"text"})},
                           {"channel", "terminal"},
                           {"sender", nlohmann::json{{"kind", "local_user"}}}});
        EventScope prep = Scope(turn, "req-0001", std::nullopt);
        prep.actor = Actor::Model;
        prep.origin = Origin::ProviderModel;
        const RecordReceipt prepared =
            Put(EventKind::ModelRequestPrepared, prep,
                nlohmann::json{{"model", "demo-model"},
                           {"provider", "demo"},
                           {"wire", "responses"},
                           {"message_refs", nlohmann::json::array()}});
        Put(EventKind::ModelRequestSent, Scope(turn, "req-0001", std::nullopt),
            nlohmann::json{{"prepared_event_id", prepared.event_id}});
        Put(EventKind::ModelOutputCompleted, Scope(turn, "req-0001", std::nullopt),
            nlohmann::json{{"output_id", "output-0001"},
                           {"blocks", nlohmann::json::array()},
                           {"stop_reason", "end_turn"}});
        Put(EventKind::TurnCompleted, Scope(turn, std::nullopt, std::nullopt),
            nlohmann::json{{"outcome", "succeeded"}});
    }

    // 半截回合:只到 sent,不收口(给 final flush 留开着的 span)。
    void OpenTurn(const std::string& turn) {
        Put(EventKind::TurnStarted, Scope(turn, std::nullopt, std::nullopt),
            nlohmann::json{{"trigger", "external_user"}});
        EventScope input_scope = Scope(turn, std::nullopt, std::nullopt);
        input_scope.actor = Actor::User;
        input_scope.origin = Origin::ExternalUser;
        Put(EventKind::InputReceived, input_scope,
            nlohmann::json{{"input_id", "input-0002"},
                           {"content", nlohmann::json::array({"text"})},
                           {"channel", "terminal"},
                           {"sender", nlohmann::json{{"kind", "local_user"}}}});
        EventScope prep = Scope(turn, "req-0002", std::nullopt);
        prep.actor = Actor::Model;
        prep.origin = Origin::ProviderModel;
        const RecordReceipt prepared =
            Put(EventKind::ModelRequestPrepared, prep,
                nlohmann::json{{"model", "demo-model"},
                           {"provider", "demo"},
                           {"wire", "responses"},
                           {"message_refs", nlohmann::json::array()}});
        Put(EventKind::ModelRequestSent, Scope(turn, "req-0002", std::nullopt),
            nlohmann::json{{"prepared_event_id", prepared.event_id}});
    }

    std::string LastEventId() const { return recorder.has_value() ? "main-tel-1:evt-" + [&] {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08llu",
                      static_cast<unsigned long long>(recorder->next_seq() - 1));
        return std::string(buffer);
    }() : std::string(); }

    void CloseRun() {
        const RecordReceipt receipt = recorder->FinishRun(
            EventKind::RunCompleted, std::string(), Durability::ProcessCrash);
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }
};

TelemetryServiceOptions MakeOptions(const std::filesystem::path& root) {
    TelemetryServiceOptions options;
    options.telemetry_root = root;
    options.resource.service_version = "0.26.0-test";
    options.resource.workspace_key = "ws-000000000000";
    options.resource.frontend = "terminal";
    options.tick_ms = 40;            // 快 tick:测试不等真秒
    options.flush_interval_ms = 30;  // 快封口:cursor 推进及时
    return options;
}

// 有界等待:谓词真回 true,超时回 false。
bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

std::vector<SpoolBatchRecord> ReadAllSealedBatches(const std::filesystem::path& root) {
    std::vector<SpoolBatchRecord> batches;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root / "spool", ec)) {
        const std::string name = entry.path().filename().generic_string();
        if (name.size() < 9 || name.compare(name.size() - 9, std::string::npos, ".otlpjson") != 0 ||
            name.compare(0, 4, "seg-") != 0) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }
            const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
            if (auto record = SpoolBatchRecord::FromJson(json)) {
                batches.push_back(std::move(*record));
            }
        }
    }
    return batches;
}

bool CursorFileReaches(const std::filesystem::path& root, const std::string& event_id) {
    const auto cursor = LoadCursor(root / "cursors", "ws-000000000000",
                                   "20260901-000000-TEL01", "main.jsonl", nullptr);
    return cursor.has_value() && cursor->last_event_id == event_id;
}

// ---- v3 半场(T07 / V3-GAP-02):service 集成面的 v3 夹具与取数断言 ----
//
// 夹具直写 V3Writer 落真账(经 ReadV3Ledger 验卷的路),不经建场口;与
// projector_v3 册的分工:那边钉折叠合同,这边钉发现/游标/重启/去重的
// service 集成行为。

class V3FixedClock : public v3::V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct V3LedgerFixture {
    V3FixedClock clock;
    std::filesystem::path sessions_root;
    std::filesystem::path session_dir;
    std::string workspace_key = "ws-000000000000";
    std::string session_id;
    std::string run_id;
    std::string stream_name;                 // "<sessionId>.jsonl"
    std::optional<v3::V3Writer> writer;
    // cursor 推进目标:末行三件(两类行共用 seq,cursor 记到行粒度)。
    std::string last_row_id;
    std::string last_row_hash;
    std::uint64_t last_row_seq = 0;

    explicit V3LedgerFixture(const char* tag, std::string id)
        : session_id(std::move(id)), run_id("main-" + session_id),
          stream_name(session_id + ".jsonl") {
        sessions_root = std::filesystem::temp_directory_path() /
                        ("lubancode-tel-svc3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(sessions_root, ec);
        session_dir = sessions_root / session_id;
        std::filesystem::create_directories(session_dir, ec);
        v3::V3WriterOptions options;
        options.run_kind = "main_session";
        auto started = v3::V3Writer::Start(session_dir / stream_name, session_id, run_id,
                                           "你是 LubanCode。", nlohmann::json::object(),
                                           options, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
        // Start 落两行(首行 system + session.started):末行是 session.started。
        last_row_seq = 2;
    }

    void Track(const v3::WriteReceipt& receipt) {
        REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
        last_row_id = receipt.id;
        last_row_hash = receipt.line_hash;
        last_row_seq = receipt.seq;
    }

    // 一轮完整请求链:user 接纳 -> prepared -> sent -> completed -> assistant
    // usage owner 接纳(与 projector_v3 册 InstallRequest 同形;usage 传
    // json(nullptr) = 缺实报)。回 request_id。
    std::string InstallRequest(const std::string& turn_id, const nlohmann::json& usage) {
        v3::MessageDraft user;
        user.turn_id = turn_id;
        user.purpose = v3::MessagePurpose::Conversation;
        user.origin = v3::MessageOrigin::Human;
        user.message =
            nlohmann::json::object({{"role", "user"}, {"content", "v3 集成:" + turn_id}});
        v3::WriteReceipt user_receipt =
            writer->AppendMessage(std::move(user), Durability::PowerLoss);
        Track(user_receipt);
        Track(writer->AdmitMessages({user_receipt.id}, Durability::PowerLoss));
        std::vector<std::string> input_refs;
        const auto& chain = writer->context().chain;
        for (std::size_t i = 1; i < chain.size(); ++i) {
            input_refs.push_back(chain[i].message_ref);
        }
        const std::string request_id = writer->NewRequestId();
        const std::string step_id = writer->NewStepId();
        nlohmann::json snapshot = nlohmann::json::object(
            {{"provider", "stub"}, {"wire", "openai"}, {"model", "stub-mini"}});
        Track(writer->PrepareRequest(request_id, turn_id, step_id, "conversation",
                                     writer->context().system_message_ref, input_refs,
                                     std::move(snapshot), std::nullopt, Durability::PowerLoss));
        {
            v3::EventDraft sent;
            sent.kind = v3::EventKindV3::ModelRequestSent;
            sent.status = v3::OpStatus::Done;
            sent.request_id = request_id;
            sent.turn_id = turn_id;
            sent.step_id = step_id;
            sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
            Track(writer->AppendEvent(std::move(sent), Durability::PowerLoss));
        }
        {
            v3::EventDraft done;
            done.kind = v3::EventKindV3::ModelResponseCompleted;
            done.status = v3::OpStatus::Done;
            done.request_id = request_id;
            done.turn_id = turn_id;
            done.step_id = step_id;
            done.payload = nlohmann::json{{"finishReason", "stop"}};
            Track(writer->AppendEvent(std::move(done), Durability::PowerLoss));
        }
        v3::MessageDraft assistant;
        assistant.turn_id = turn_id;
        assistant.step_id = step_id;
        assistant.request_id = request_id;
        assistant.purpose = v3::MessagePurpose::Conversation;
        assistant.origin = v3::MessageOrigin::SessionRuntime;
        assistant.provider = "stub";
        assistant.wire = "openai";
        assistant.model = "stub-mini";
        assistant.response_model = nlohmann::json(nullptr);
        assistant.usage = usage;
        assistant.message =
            nlohmann::json::object({{"role", "assistant"}, {"content", "v3 答复"}});
        v3::WriteReceipt assistant_receipt =
            writer->AppendMessage(std::move(assistant), Durability::PowerLoss);
        Track(assistant_receipt);
        Track(writer->AdmitMessages({assistant_receipt.id}, Durability::PowerLoss));
        return request_id;
    }

    void EndSession(bool clean = true) {
        v3::EventDraft ended;
        ended.kind = v3::EventKindV3::SessionEnded;
        ended.payload = nlohmann::json{{"reason", clean ? "completed" : "crashed"},
                                       {"closeQuality", clean ? "clean" : "incomplete"}};
        Track(writer->AppendEvent(std::move(ended), Durability::PowerLoss));
    }
};

bool V3CursorFileReaches(const std::filesystem::path& root, const std::string& session_id,
                         const std::string& last_row_id) {
    const auto cursor = LoadCursor(root / "cursors", "ws-000000000000", session_id,
                                   session_id + ".jsonl", nullptr);
    return cursor.has_value() && cursor->last_event_id == last_row_id;
}

// OTLP traces 批里点名 span 的属性(整型);找不到回 -1。
std::int64_t SpanIntAttr(const SpoolBatchRecord& batch, const std::string& span_name,
                         const std::string& attr_key) {
    if (batch.signal != "traces" || !batch.payload.contains("resourceSpans")) {
        return -1;
    }
    for (const nlohmann::json& resource_spans : batch.payload.at("resourceSpans")) {
        if (!resource_spans.contains("scopeSpans")) {
            continue;
        }
        for (const nlohmann::json& scope_spans : resource_spans.at("scopeSpans")) {
            if (!scope_spans.contains("spans")) {
                continue;
            }
            for (const nlohmann::json& span : scope_spans.at("spans")) {
                if (!span.contains("name") || span.at("name") != span_name ||
                    !span.contains("attributes")) {
                    continue;
                }
                for (const nlohmann::json& attribute : span.at("attributes")) {
                    if (attribute.contains("key") && attribute.at("key") == attr_key &&
                        attribute.contains("value") &&
                        attribute.at("value").contains("intValue") &&
                        attribute.at("value").at("intValue").is_string()) {
                        return std::stoll(attribute.at("value").at("intValue")
                                              .get<std::string>());
                    }
                }
            }
        }
    }
    return -1;
}

// 批里点名名字的 span 枚数。
std::size_t CountSpans(const SpoolBatchRecord& batch, const std::string& span_name) {
    if (batch.signal != "traces" || !batch.payload.contains("resourceSpans")) {
        return 0;
    }
    std::size_t count = 0;
    for (const nlohmann::json& resource_spans : batch.payload.at("resourceSpans")) {
        if (!resource_spans.contains("scopeSpans")) {
            continue;
        }
        for (const nlohmann::json& scope_spans : resource_spans.at("scopeSpans")) {
            if (!scope_spans.contains("spans")) {
                continue;
            }
            for (const nlohmann::json& span : scope_spans.at("spans")) {
                if (span.contains("name") && span.at("name") == span_name) {
                    count += 1;
                }
            }
        }
    }
    return count;
}

// metrics 批里 lubancode.model.tokens 按 kind(input/output)取值;窗口末
// last_event_id 对上才读(累计快照只认最新窗);找不到回 -1。
std::int64_t TokenMetric(const std::vector<SpoolBatchRecord>& batches,
                         const std::string& window_last_id, const std::string& kind) {
    for (const SpoolBatchRecord& batch : batches) {
        if (batch.signal != "metrics" || batch.last_event_id != window_last_id ||
            !batch.payload.contains("resourceMetrics")) {
            continue;
        }
        for (const nlohmann::json& resource_metrics : batch.payload.at("resourceMetrics")) {
            if (!resource_metrics.contains("scopeMetrics")) {
                continue;
            }
            for (const nlohmann::json& scope_metrics : resource_metrics.at("scopeMetrics")) {
                if (!scope_metrics.contains("metrics")) {
                    continue;
                }
                for (const nlohmann::json& metric : scope_metrics.at("metrics")) {
                    if (!metric.contains("name") || metric.at("name") != "lubancode.model.tokens" ||
                        !metric.contains("sum") || !metric.at("sum").contains("dataPoints")) {
                        continue;
                    }
                    for (const nlohmann::json& point : metric.at("sum").at("dataPoints")) {
                        bool kind_match = false;
                        if (point.contains("attributes")) {
                            for (const nlohmann::json& attribute : point.at("attributes")) {
                                if (attribute.contains("key") && attribute.at("key") == "kind" &&
                                    attribute.contains("value") &&
                                    attribute.at("value").contains("stringValue") &&
                                    attribute.at("value").at("stringValue") == kind) {
                                    kind_match = true;
                                }
                            }
                        }
                        if (kind_match && point.contains("asInt") &&
                            point.at("asInt").is_string()) {
                            return std::stoll(point.at("asInt").get<std::string>());
                        }
                    }
                }
            }
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("装配门:flag 关零副作用;telemetry 开 trajectory 关明拒") {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-gate";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    TelemetryAssemblyInputs off;
    off.config_telemetry = false;
    off.config_trajectory = true;
    off.options = MakeOptions(root);
    std::string note;
    CHECK(TryAssembleTelemetryService(off, &note) == nullptr);
    CHECK(note == "telemetry.off");
    CHECK_FALSE(std::filesystem::exists(root, ec));  // §8.5:目录都不建

    TelemetryAssemblyInputs requires_trajectory;
    requires_trajectory.config_telemetry = true;
    requires_trajectory.config_trajectory = false;
    requires_trajectory.options = MakeOptions(root);
    CHECK(TryAssembleTelemetryService(requires_trajectory, &note) == nullptr);
    CHECK(note == "telemetry.requires_trajectory");
    CHECK_FALSE(std::filesystem::exists(root, ec));
}

TEST_CASE("生命周期:committed wake -> 增量投影 -> sealed 段 + cursor 推进") {
    JournalFixture fixture("lifecycle");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();
    const std::string last_id = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-lifecycle";
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});

    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, last_id); }));
    service.Stop();

    // durable 段:traces 批带完整 span 树,metrics 批带累计快照。
    const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
    REQUIRE_FALSE(batches.empty());
    bool saw_traces = false;
    bool saw_metrics = false;
    for (const SpoolBatchRecord& batch : batches) {
        if (batch.signal == "traces") {
            saw_traces = true;
            REQUIRE(batch.payload.contains("resourceSpans"));
            const std::string dump = batch.payload.dump();
            CHECK(dump.find("lubancode.agent.run") != std::string::npos);
            CHECK(dump.find("lubancode.agent.turn") != std::string::npos);
            CHECK(dump.find("gen_ai.request") != std::string::npos);
        } else if (batch.signal == "metrics") {
            saw_metrics = true;
            REQUIRE(batch.payload.contains("resourceMetrics"));
        }
    }
    CHECK(saw_traces);
    CHECK(saw_metrics);
    // 窗口对账字段齐(§18.2 meta 带 batch 范围):traces 批把整本 Journal
    // 投到末事件。
    bool saw_last_window = false;
    for (const SpoolBatchRecord& batch : batches) {
        if (batch.signal == "traces") {
            saw_last_window = saw_last_window || batch.last_event_id == last_id;
            CHECK(batch.projector_version == std::string(kProjectorVersion));
            CHECK(batch.projection_generation == 1);
        }
    }
    CHECK(saw_last_window);

    // state.json:projection_key 由 service 持有,不空、128 位十六进制(64 字节)。
    std::ifstream state_file(root / "state.json", std::ios::binary);
    std::stringstream state_buffer;
    state_buffer << state_file.rdbuf();
    const nlohmann::json state = nlohmann::json::parse(state_buffer.str(), nullptr, false);
    REQUIRE_FALSE(state.is_discarded());
    const std::string key = state.at("projection_key_hex").get<std::string>();
    CHECK(key.size() == 128);
    CHECK(state.at("projection_generation").get<int>() == 1);
    CHECK(state.at("device_instance_id").is_string());

    // 状态面:/telemetry 与 /doctor 的行都有货。
    const TelemetryServiceStatus status = service.Status();
    const std::vector<std::string> status_lines = FormatTelemetryStatusLines(status);
    REQUIRE_FALSE(status_lines.empty());
    const std::vector<std::string> doctor_lines = FormatTelemetryDoctorLines(status);
    REQUIRE(status.streams.size() == 1);
    CHECK(status.streams.front().stream_id == "main.jsonl");
    CHECK(status.streams.front().error_code.empty());
    CHECK(status.streams.front().lag_events == 0);
    CHECK(doctor_lines.size() > status_lines.size());
}

TEST_CASE("丢 wake:周期 tick 补账(§14.1)") {
    JournalFixture fixture("lostwake");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();
    const std::string last_id = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-lostwake";
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    // 不投任何 wake:只靠周期扫描发现新账。
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, last_id); }));
    service.Stop();
    REQUIRE_FALSE(ReadAllSealedBatches(root).empty());
}

TEST_CASE("增量:第二轮只投新窗口,旧 span 不重发") {
    JournalFixture fixture("incremental");
    fixture.CompleteTurn("turn-0001");
    const std::string first_last = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-incremental";
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, first_last); }));

    // 第二轮:新开一 turn,只该多出第二窗的批。
    const std::size_t batches_before = ReadAllSealedBatches(root).size();
    fixture.CompleteTurn("turn-0002");
    const std::string second_last = fixture.LastEventId();
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, second_last); }));
    service.Stop();

    const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
    CHECK(batches.size() > batches_before);
    // 第一窗的 run span 不因第二窗重投而翻倍:批 id 去重(同 generation 内
    // 确定窗末推导,窗口推进后旧 id 不再出现)。
    std::set<std::string> ids;
    for (const SpoolBatchRecord& batch : batches) {
        CHECK(ids.insert(batch.batch_id).second);
    }
}

TEST_CASE("崩溃修复:cursor 文件丢(seal 后没写成)→ 重开修前推,不重投") {
    JournalFixture fixture("crash");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();
    const std::string last_id = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-crash";
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
        service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
        REQUIRE(WaitUntil([&] {
            return !ReadAllSealedBatches(root).empty() && CursorFileReaches(root, last_id);
        }));
        // 模拟"seal 完、cursor 没落成"就崩:删 cursor 文件。
        std::error_code ec;
        std::filesystem::remove_all(root / "cursors", ec);
    }
    const std::size_t batches_after_crash = ReadAllSealedBatches(root).size();
    CHECK(batches_after_crash > 0);

    TelemetryService reopened(MakeOptions(root));
    REQUIRE(reopened.Start());
    reopened.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    // 覆盖对账把 cursor 修前推到 durable 端点;不再重投同窗(批数不涨)。
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, last_id); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(ReadAllSealedBatches(root).size() == batches_after_crash);
    const TelemetryServiceStatus status = reopened.Status();
    REQUIRE(status.streams.size() == 1);
    CHECK(status.streams.front().error_code.empty());
    reopened.Stop();
}

TEST_CASE("cursor 孤儿:超前无账 → 停投报错,服务不倒(§18.5)") {
    JournalFixture fixture("orphan");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();
    const std::string last_id = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-orphan";
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
        service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
        REQUIRE(WaitUntil([&] {
            return !ReadAllSealedBatches(root).empty() && CursorFileReaches(root, last_id);
        }));
        service.Stop();
    }
    // 模拟"spool 段全丢、又没留 ACK/清理水位":cursor 超前无账。
    std::error_code ec;
    std::filesystem::remove_all(root / "spool", ec);

    TelemetryService reopened(MakeOptions(root));
    REQUIRE(reopened.Start());
    reopened.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    REQUIRE(WaitUntil([&] {
        const TelemetryServiceStatus status = reopened.Status();
        return status.streams.size() == 1 && !status.streams.front().error_code.empty();
    }));
    const TelemetryServiceStatus status = reopened.Status();
    CHECK(status.streams.front().error_code == "telemetry.cursor_orphan");
    CHECK(status.running);  // 服务不倒:别的 stream 照跑,doctor 报红
    reopened.Stop();
}

TEST_CASE("IO 坏:spool 写不进 → degraded 停收,Agent 侧 Notify 照常") {
    JournalFixture fixture("iodegraded");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    // 注意 root 不能与 fixture 的 session_dir 同路径:JournalFixture 用
    // "lubancode-tel-svc-iodegraded" 当 session 目录,下面的 remove_all(root)
    // 在 POSIX 上会把刚落好的 Journal 一并删掉(Windows 上靠 recorder 攥着
    // 句柄删不动才侥幸活着)——投影没了源,degraded 永远等不来。
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-iodegraded-root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "spool", ec);
    // active.tmp 被占成非空目录:删不掉、写不进(满盘/只读盘的同型故障)。
    std::filesystem::create_directories(root / "spool" / "active.tmp", ec);
    { std::ofstream blocker(root / "spool" / "active.tmp" / "blocked", std::ios::trunc); }

    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    // Agent 侧只管投 wake:不阻塞、不抛(§17.2 不反压业务线程)。
    for (int i = 0; i < 20; ++i) {
        service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    }
    REQUIRE(WaitUntil([&] { return !service.Status().degraded_reason.empty(); }));
    const TelemetryServiceStatus status = service.Status();
    CHECK(status.degraded_reason == "telemetry.spool_rejected");
    CHECK(status.spool.degraded);
    CHECK(status.running);
    service.Stop();  // 有界收场,不吊死
    CHECK_FALSE(service.Status().running);
}

TEST_CASE("projection_key 归属:重启沿用;projector 版本换代 → generation+1 换钥匙") {
    JournalFixture fixture("generation");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    // 与 fixture 的 session_dir("lubancode-tel-svc-generation")分开:
    // 同路径时 remove_all(root) 会把 Journal 删掉(POSIX 上真删得动),
    // sealed batches 永远等不来。
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-generation-root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::string first_key;
    std::string first_device;
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
        service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
        REQUIRE(WaitUntil([&] { return !ReadAllSealedBatches(root).empty(); }));
        service.Stop();
        const auto state = nlohmann::json::parse(
            [](const std::filesystem::path& path) {
                std::ifstream file(path, std::ios::binary);
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }(root / "state.json"),
            nullptr, false);
        first_key = state.at("projection_key_hex").get<std::string>();
        first_device = state.at("device_instance_id").get<std::string>();
    }

    // 同代重开:钥匙与设备 id 不变(span id 稳定重建的地基)。
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.Stop();
        const auto state = nlohmann::json::parse(
            [](const std::filesystem::path& path) {
                std::ifstream file(path, std::ios::binary);
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }(root / "state.json"),
            nullptr, false);
        CHECK(state.at("projection_key_hex").get<std::string>() == first_key);
        CHECK(state.at("device_instance_id").get<std::string>() == first_device);
        CHECK(state.at("projection_generation").get<int>() == 1);
    }

    // 换 projector 版本(§27.2 不兼容):另开 generation + 换钥匙。
    {
        auto state = nlohmann::json::parse(
            [](const std::filesystem::path& path) {
                std::ifstream file(path, std::ios::binary);
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }(root / "state.json"),
            nullptr, false);
        state["projector_version"] = "telemetry-projector-v0-old";
        { std::ofstream file(root / "state.json", std::ios::binary | std::ios::trunc);
          file << state.dump(); }
    }
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
        REQUIRE(WaitUntil([&] {
            const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
            for (const SpoolBatchRecord& batch : batches) {
                if (batch.projection_generation == 2) {
                    return true;
                }
            }
            return false;
        }));
        service.Stop();
        const auto state = nlohmann::json::parse(
            [](const std::filesystem::path& path) {
                std::ifstream file(path, std::ios::binary);
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }(root / "state.json"),
            nullptr, false);
        CHECK(state.at("projection_generation").get<int>() == 2);
        CHECK(state.at("projection_key_hex").get<std::string>() != first_key);
    }
}

TEST_CASE("final flush:开着的 span 在 Stop 后按 missing 收口入账") {
    JournalFixture fixture("finalflush");
    fixture.OpenTurn("turn-0009");  // sent 之后不收口
    const std::string last_id = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-finalflush";
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    // 常规趟只推进 cursor;开着的 request span 不发(没终态不冒充)。
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, last_id); }));
    service.Stop();

    const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
    bool saw_missing_request = false;
    for (const SpoolBatchRecord& batch : batches) {
        if (batch.signal != "traces" || !batch.final_window) {
            continue;
        }
        const std::string dump = batch.payload.dump();
        if (dump.find("gen_ai.request") != std::string::npos &&
            dump.find("lubancode.span.terminal") != std::string::npos &&
            dump.find("missing") != std::string::npos) {
            saw_missing_request = true;
        }
    }
    CHECK(saw_missing_request);
}

#ifdef _WIN32
TEST_CASE("windows 短拒不丢账:cursor 落盘被独占句柄拦,松手后追平") {
    // 间歇超时的机理钉(windows 独有,POSIX rename 无共享违例):测试侧
    // WaitUntil 每 10ms LoadCursor 轮询,worker 的 cursor 原子替换撞上
    // 开着的句柄就短拒(MSVC fstream 不带 FILE_SHARE_DELETE);旧账在
    // AdvancePendingCursors 里落盘失败也销 pending 推进,最后一只窗口
    // 的推进被静默丢掉,cursor 文件从此停在旧位置——窗口不再长大,永久
    // 停摆。新账:落盘失败留 pending,tick 重投,松手即追平。独占句柄
    // (_SH_DENYRW)锁 1.5s,盖过 StoreCursor 的 10×10ms 重试档与数个
    // 40ms tick,期间第二窗的推进必然落盘失败。
    JournalFixture fixture("cursorhold");
    fixture.CompleteTurn("turn-0001");
    const std::string first_last = fixture.LastEventId();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc-cursorhold";
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, first_last); }));

    const std::filesystem::path cursor_path = root / "cursors" / fixture.workspace_key /
                                              fixture.session_id / "main.jsonl.json";
    std::FILE* exclusive = _wfsopen(cursor_path.c_str(), L"rb", _SH_DENYRW);
    REQUIRE(exclusive != nullptr);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::fclose(exclusive);
    });

    fixture.CompleteTurn("turn-0002");
    const std::string second_last = fixture.LastEventId();
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    // 锁窗内追不上;松手后有界追平(旧账:pending 已销,永久停摆,此断言红)。
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, second_last); }, 8000));
    releaser.join();
    service.Stop();

    // 第二窗的常规件 traces 批只此一份(推进重投靠批 id 去重,不重发)。
    // Stop 的 final flush 会按 missing 收口开着的 run span(run 未 CloseRun),
    // 另发一只锚在同窗口末的 final 件——那是设计行为(§26.3),分开数;
    // metrics 批与 traces 同窗口末,也不在此数。
    std::size_t second_window_batches = 0;
    for (const SpoolBatchRecord& batch : ReadAllSealedBatches(root)) {
        if (batch.signal == "traces" && batch.last_event_id == second_last &&
            !batch.final_window) {
            second_window_batches += 1;
        }
    }
    CHECK(second_window_batches == 1);
}
#endif

// ---------------------------------------------------------------------------
// v3 半场(V3-GAP-02 销项验):发现/投影/游标行粒度/两代混投/重启续投/去重
// ---------------------------------------------------------------------------

TEST_CASE("v3 主账:发现 <id>.jsonl → 投影折叠 → cursor 记到行粒度(seq/哈希)") {
    V3LedgerFixture fixture("main", "20260924-000000-TELV3A");
    const nlohmann::json usage = nlohmann::json{{"inputTokens", 1200}, {"outputTokens", 34}};
    fixture.InstallRequest("turn-0001", usage);
    fixture.EndSession();

    // telemetry root 与 session 目录分家(册内 IO 案的教训:同路径会互删)。
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc3-main-root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, fixture.stream_name});

    REQUIRE(WaitUntil([&] { return V3CursorFileReaches(root, fixture.session_id,
                                                       fixture.last_row_id); }));
    service.Stop();

    // 发现面:stream 即 <sessionId>.jsonl,健康无错。
    const TelemetryServiceStatus status = service.Status();
    REQUIRE(status.streams.size() == 1);
    CHECK(status.streams.front().stream_id == fixture.stream_name);
    CHECK(status.streams.front().error_code.empty());
    CHECK(status.streams.front().lag_events == 0);

    // cursor 三件:行粒度——末行是 session.ended 事件,seq/哈希逐项对上。
    const auto cursor = LoadCursor(root / "cursors", fixture.workspace_key, fixture.session_id,
                                   fixture.stream_name, nullptr);
    REQUIRE(cursor.has_value());
    CHECK(cursor->last_event_id == fixture.last_row_id);
    CHECK(cursor->last_event_hash == fixture.last_row_hash);
    CHECK(cursor->last_event_seq == fixture.last_row_seq);
    CHECK(cursor->last_event_seq > 0);
    // v3 两类行共用 seq:cursor 末行若停在 message 行上,seq 也照记(这里
    // 末行是事件;message 行粒度由重启续投案覆盖)。

    // 数据链:traces 批里 v3 事件真被折叠成 span——session/request 各一枚,
    // request span 的 usage 取自 assistant message 行(事件行开锚、消息行
    // 结算,两类行折进同一枚 span,不分裂)。
    const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
    std::size_t session_spans = 0;
    std::size_t request_spans = 0;
    std::int64_t input_tokens = -1;
    for (const SpoolBatchRecord& batch : batches) {
        session_spans += CountSpans(batch, "lubancode.session");
        request_spans += CountSpans(batch, "gen_ai.request");
        if (SpanIntAttr(batch, "gen_ai.request", "gen_ai.usage.input_tokens") >= 0) {
            input_tokens = SpanIntAttr(batch, "gen_ai.request", "gen_ai.usage.input_tokens");
        }
    }
    CHECK(session_spans == 1);
    CHECK(request_spans == 1);
    CHECK(input_tokens == 1200);

    // metrics 批:token 计数对表(assistant usage owner 唯一可累计)。
    CHECK(TokenMetric(batches, fixture.last_row_id, "input") == 1200);
    CHECK(TokenMetric(batches, fixture.last_row_id, "output") == 34);
}

TEST_CASE("两代混投:v2 场与 v3 场并存,各自发现各自投,同一逻辑活动不分裂") {
    JournalFixture v2_fixture("mixed");
    v2_fixture.CompleteTurn("turn-0001");
    v2_fixture.CloseRun();
    const std::string v2_last = v2_fixture.LastEventId();

    V3LedgerFixture v3_fixture("mixed", "20260924-000000-TELV3B");
    const nlohmann::json usage = nlohmann::json{{"inputTokens", 500}, {"outputTokens", 12}};
    v3_fixture.InstallRequest("turn-0001", usage);
    v3_fixture.EndSession();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc3-mixed-root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root));
    REQUIRE(service.Start());
    service.RegisterSession(v2_fixture.workspace_key, v2_fixture.session_id,
                            v2_fixture.session_dir);
    service.RegisterSession(v3_fixture.workspace_key, v3_fixture.session_id,
                            v3_fixture.session_dir);
    service.Notify(CommitWake{v2_fixture.workspace_key, v2_fixture.session_id, "main.jsonl"});
    service.Notify(
        CommitWake{v3_fixture.workspace_key, v3_fixture.session_id, v3_fixture.stream_name});

    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, v2_last); }));
    REQUIRE(WaitUntil([&] { return V3CursorFileReaches(root, v3_fixture.session_id,
                                                       v3_fixture.last_row_id); }));
    service.Stop();

    // 发现面:两代账都在册、都健康(两代账都投;互不遮蔽)。
    const TelemetryServiceStatus status = service.Status();
    REQUIRE(status.streams.size() == 2);
    bool saw_v2 = false;
    bool saw_v3 = false;
    for (const auto& stream : status.streams) {
        CHECK(stream.error_code.empty());
        saw_v2 = saw_v2 || stream.stream_id == "main.jsonl";
        saw_v3 = saw_v3 || stream.stream_id == v3_fixture.stream_name;
    }
    CHECK(saw_v2);
    CHECK(saw_v3);

    // 数据面:同一逻辑活动(user -> 请求 -> 应答 -> 收场)在两代格式里各投
    // 恰好一枚 request span——不因格式不同把一次活动裂成多枚;span 身份
    // 全体唯一(traceId+spanId 无重复)。
    const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
    std::size_t v2_requests = 0;
    std::size_t v3_requests = 0;
    std::set<std::string> span_ids;
    bool duplicate_span = false;
    for (const SpoolBatchRecord& batch : batches) {
        if (batch.signal != "traces" || !batch.payload.contains("resourceSpans")) {
            continue;
        }
        for (const nlohmann::json& resource_spans : batch.payload.at("resourceSpans")) {
            if (!resource_spans.contains("scopeSpans")) {
                continue;
            }
            for (const nlohmann::json& scope_spans : resource_spans.at("scopeSpans")) {
                if (!scope_spans.contains("spans")) {
                    continue;
                }
                for (const nlohmann::json& span : scope_spans.at("spans")) {
                    if (span.contains("name") && span.at("name") == "gen_ai.request") {
                        const std::string stream = batch.stream_id;
                        if (stream == "main.jsonl") {
                            v2_requests += 1;
                        } else {
                            v3_requests += 1;
                        }
                    }
                    if (span.contains("traceId") && span.contains("spanId")) {
                        duplicate_span = duplicate_span ||
                                         !span_ids
                                              .insert(span.at("traceId").get<std::string>() + "|" +
                                                      span.at("spanId").get<std::string>())
                                              .second;
                    }
                }
            }
        }
    }
    CHECK(v2_requests == 1);
    CHECK(v3_requests == 1);
    CHECK_FALSE(duplicate_span);
}

TEST_CASE("v3 重启续投:Stop 后账续长,重开从断点续投,已投不重投") {
    V3LedgerFixture fixture("restart", "20260924-000000-TELV3C");
    const std::int64_t first_in = 800;
    const std::int64_t second_in = 300;
    fixture.InstallRequest(
        "turn-0001", nlohmann::json{{"inputTokens", first_in}, {"outputTokens", 20}});
    // 第一窗末行 = assistant 接纳提交(context.input.applied,事件行)。
    const std::string first_last = fixture.last_row_id;

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc3-restart-root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
        service.Notify(
            CommitWake{fixture.workspace_key, fixture.session_id, fixture.stream_name});
        REQUIRE(WaitUntil([&] { return V3CursorFileReaches(root, fixture.session_id,
                                                           first_last); }));
        service.Stop();
    }

    // 断点验位:cursor 停在事件行上(eventId),seq 也是该行 seq。
    {
        const auto cursor = LoadCursor(root / "cursors", fixture.workspace_key,
                                       fixture.session_id, fixture.stream_name, nullptr);
        REQUIRE(cursor.has_value());
        CHECK(cursor->last_event_id == first_last);
        CHECK(cursor->last_event_seq == fixture.last_row_seq);
    }
    const std::size_t batches_before = ReadAllSealedBatches(root).size();
    CHECK(batches_before > 0);

    // 账续长:第二窗请求链 + 一枚已落账未接纳的排队 user 消息(第二窗末行
    // = message 行,session 不收场)。两类行的行粒度游标都钉到。
    fixture.InstallRequest(
        "turn-0002", nlohmann::json{{"inputTokens", second_in}, {"outputTokens", 7}});
    {
        v3::MessageDraft queued;
        queued.turn_id = "turn-0003";
        queued.purpose = v3::MessagePurpose::Conversation;
        queued.origin = v3::MessageOrigin::Human;
        queued.message =
            nlohmann::json::object({{"role", "user"}, {"content", "排队的下一问"}});
        fixture.Track(fixture.writer->AppendMessage(std::move(queued), Durability::PowerLoss));
    }
    const std::string second_last = fixture.last_row_id;  // messageId(message 行)

    TelemetryService reopened(MakeOptions(root));
    REQUIRE(reopened.Start());
    reopened.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    REQUIRE(WaitUntil([&] { return V3CursorFileReaches(root, fixture.session_id,
                                                       second_last); }));
    reopened.Stop();

    // 断点续投到位:cursor 末行 = messageId,seq = 该 message 行 seq(两类
    // 行共用发号,cursor 无视行型记到行粒度)。
    {
        const auto cursor = LoadCursor(root / "cursors", fixture.workspace_key,
                                       fixture.session_id, fixture.stream_name, nullptr);
        REQUIRE(cursor.has_value());
        CHECK(cursor->last_event_id == second_last);
        CHECK(cursor->last_event_seq == fixture.last_row_seq);
        CHECK(cursor->last_event_hash == fixture.last_row_hash);
    }

    // 已投不重投:批 id 全体唯一;第一窗的批不因重开再发一份。
    const std::vector<SpoolBatchRecord> batches = ReadAllSealedBatches(root);
    CHECK(batches.size() > batches_before);
    std::set<std::string> ids;
    for (const SpoolBatchRecord& batch : batches) {
        CHECK(ids.insert(batch.batch_id).second);
    }

    // span 归一:两枚逻辑请求恰好两枚 request span,且每枚(按 traceId+
    // spanId)全体只出现一次——重开不把第一窗的 request span 再投一遍。
    // 注:session/turn span 无 v3 终态事件,每个服务生命周期的 final flush
    // 各按 missing 收口一次(§26.3 设计行为),不在此数。
    std::size_t request_spans = 0;
    std::set<std::string> request_span_ids;
    bool request_span_duplicated = false;
    for (const SpoolBatchRecord& batch : batches) {
        if (batch.signal != "traces" || !batch.payload.contains("resourceSpans")) {
            continue;
        }
        for (const nlohmann::json& resource_spans : batch.payload.at("resourceSpans")) {
            if (!resource_spans.contains("scopeSpans")) {
                continue;
            }
            for (const nlohmann::json& scope_spans : resource_spans.at("scopeSpans")) {
                if (!scope_spans.contains("spans")) {
                    continue;
                }
                for (const nlohmann::json& span : scope_spans.at("spans")) {
                    if (!span.contains("name") || span.at("name") != "gen_ai.request" ||
                        !span.contains("traceId") || !span.contains("spanId")) {
                        continue;
                    }
                    request_spans += 1;
                    request_span_duplicated =
                        request_span_duplicated ||
                        !request_span_ids
                             .insert(span.at("traceId").get<std::string>() + "|" +
                                     span.at("spanId").get<std::string>())
                             .second;
                }
            }
        }
    }
    CHECK(request_spans == 2);
    CHECK_FALSE(request_span_duplicated);

    // 不重复累计:两窗的 metrics 都是全量累计快照——第一窗 == 800,第二窗
    // == 800+300(重开不把整卷再加一遍)。
    CHECK(TokenMetric(batches, first_last, "input") == first_in);
    CHECK(TokenMetric(batches, second_last, "input") == first_in + second_in);
}

TEST_CASE("v3 崩溃去重:cursor 文件丢 → 覆盖对账修前推(v3 seq 三件),不重投") {
    V3LedgerFixture fixture("dedup", "20260924-000000-TELV3D");
    fixture.InstallRequest("turn-0001",
                           nlohmann::json{{"inputTokens", 640}, {"outputTokens", 11}});
    fixture.EndSession();

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-svc3-dedup-root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    {
        TelemetryService service(MakeOptions(root));
        REQUIRE(service.Start());
        service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
        service.Notify(
            CommitWake{fixture.workspace_key, fixture.session_id, fixture.stream_name});
        REQUIRE(WaitUntil([&] {
            return !ReadAllSealedBatches(root).empty() &&
                   V3CursorFileReaches(root, fixture.session_id, fixture.last_row_id);
        }));
        // 模拟"seal 完、cursor 没落成"就崩:删 cursor 文件。
        std::filesystem::remove_all(root / "cursors", ec);
    }
    // 只数常规窗批:v3 turn span 无终态,重开后的 Stop 会按 missing 收口
    // 再发一只 final 批(§26.3 设计行为),不算重投。
    const auto count_regular_batches = [](const std::filesystem::path& telemetry_root) {
        std::size_t count = 0;
        for (const SpoolBatchRecord& batch : ReadAllSealedBatches(telemetry_root)) {
            if (!batch.final_window) {
                count += 1;
            }
        }
        return count;
    };
    const std::size_t batches_after_crash = count_regular_batches(root);
    CHECK(batches_after_crash > 0);

    TelemetryService reopened(MakeOptions(root));
    REQUIRE(reopened.Start());
    reopened.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    // 覆盖对账把 cursor 修前推到 durable 端点(v3 行三件含 seq);不重投。
    REQUIRE(WaitUntil([&] { return V3CursorFileReaches(root, fixture.session_id,
                                                       fixture.last_row_id); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(count_regular_batches(root) == batches_after_crash);
    // 修回来的 cursor 仍带 v3 行粒度三件。
    const auto cursor = LoadCursor(root / "cursors", fixture.workspace_key, fixture.session_id,
                                   fixture.stream_name, nullptr);
    REQUIRE(cursor.has_value());
    CHECK(cursor->last_event_seq == fixture.last_row_seq);
    CHECK(cursor->last_event_hash == fixture.last_row_hash);
    const TelemetryServiceStatus status = reopened.Status();
    REQUIRE(status.streams.size() == 1);
    CHECK(status.streams.front().error_code.empty());
    reopened.Stop();
}
