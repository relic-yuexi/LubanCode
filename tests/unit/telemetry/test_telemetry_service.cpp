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

using namespace lubancode::telemetry;
using namespace lubancode::trajectory;

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
