// T2 OTLP/HTTP 出口的回环验收(端云协同可观测单 §19,验收线"断网、429、
// 5xx、Collector 重启后补送;重复 batch 可识别;业务线程不等网络"):
//   - 回环假 collector(127.0.0.1 原始 socket,不连任何真外部端点)收货
//     验合同:POST /v1/traces|/v1/metrics、Content-Type、批次识别头、body
//     过 T0 的 ValidateOtlpTracesJson;
//   - 重试/退避/ACK/tombstone 全链:429/5xx 可重试、404 停代、ACK 丢
//     (收货不回)重发同 batch id、双限弃置;
//   - 服务全链:Journal -> 投影 -> spool -> 出口 -> ACK 删段 + tombstone;
//   - 断网/Collector 重启后补送;出口挂死时投影照跑(业务线程不等网络);
//   - consent 门:公网 endpoint 无授权不发业务数据,授权后才真发;
//   - spool clear 两步删除后 cursor 对账不误报。
//
// 端口策:绑 127.0.0.1:0 拿空闲端口;“断网”场景先绑后关,collector 重启
// 再绑回同端口(SO_REUSEADDR)。
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <doctest/doctest.h>

// winsock/windows 头把 LoadCursor 定义成宏,会把 telemetry::LoadCursor 的
// 调用改写成 LoadCursorA(winuser.h 的 ANSI 宏)。这里undef掉,本册用的
// 是 telemetry 的 cursor 读回,不是 Win32 光标。
#ifdef LoadCursor
#undef LoadCursor
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "telemetry/exporter.hpp"
#include "telemetry/otlp_json.hpp"
#include "telemetry/service.hpp"
#include "telemetry/spool.hpp"
#include "trajectory/recorder.hpp"

using namespace lubancode::telemetry;
using namespace lubancode::trajectory;

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
void CloseSocket(socket_t s) { ::close(s); }
#endif

void EnsureSocketsReady() {
#ifdef _WIN32
    struct WinsockInit {
        WinsockInit() {
            WSADATA wsa;
            ::WSAStartup(MAKEWORD(2, 2), &wsa);
        }
    };
    static WinsockInit init;
#endif
}

// ---------------------------------------------------------------------------
// 回环假 collector:一条线程顺序 accept;每连接读一请求(带 Content-Length
// 完整收体)回一响应。响应脚本(FakeResponse 队列)空了以后默认 200 "{}"。
// ---------------------------------------------------------------------------
struct CollectedRequest {
    std::string method;
    std::string path;                      // /v1/traces | /v1/metrics
    std::string body;
    std::map<std::string, std::string> headers;  // 键小写
};

struct FakeResponse {
    int status = 200;
    std::string body = "{}";
    std::map<std::string, std::string> headers;  // 额外响应头(Retry-After)
    bool drop = false;                           // 收了不回(ACK 丢)
};

class FakeCollector {
public:
    explicit FakeCollector(int fixed_port = 0) {
        EnsureSocketsReady();
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener_ != kInvalidSocket);
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(fixed_port));
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        sockaddr_in bound{};
#ifdef _WIN32
        int bound_len = sizeof(bound);
#else
        socklen_t bound_len = sizeof(bound);
#endif
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0);
        port_ = ntohs(bound.sin_port);
        REQUIRE(::listen(listener_, 16) == 0);
        thread_ = std::thread([this] { ServeLoop(); });
    }

    ~FakeCollector() {
        stop_.store(true);
        if (listener_ != kInvalidSocket) {
            ::shutdown(listener_, 2);
            CloseSocket(listener_);
            listener_ = kInvalidSocket;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(port_); }
    int port() const { return port_; }

    void Queue(FakeResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        script_.push_back(std::move(response));
    }

    std::vector<CollectedRequest> Requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

    std::size_t RequestCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_.size();
    }

private:
    void ServeLoop() {
        while (!stop_.load()) {
            sockaddr_in client{};
#ifdef _WIN32
            int len = sizeof(client);
#else
            socklen_t len = sizeof(client);
#endif
            const socket_t client_fd =
                ::accept(listener_, reinterpret_cast<sockaddr*>(&client), &len);
            if (client_fd == kInvalidSocket) {
                return;  // listener 关了(析构)
            }
            ServeOne(client_fd);
            CloseSocket(client_fd);
        }
    }

    bool SendAll(socket_t fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int n = ::send(fd, data.data() + sent,
                                 static_cast<int>(data.size() - sent), 0);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    void ServeOne(socket_t client_fd) {
        // 收头(带超时,防坏连接吊死测试)。
#ifdef _WIN32
        DWORD timeout = 4000;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{4, 0};
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#endif
        std::string data;
        char buffer[4096];
        while (data.find("\r\n\r\n") == std::string::npos && data.size() < 64 * 1024) {
            const int n = ::recv(client_fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return;
            }
            data.append(buffer, static_cast<std::size_t>(n));
        }
        const std::size_t header_end = data.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return;
        }
        CollectedRequest request;
        std::istringstream head(data.substr(0, header_end));
        std::string request_line;
        std::getline(head, request_line);
        {
            std::istringstream line(request_line);
            line >> request.method >> request.path;
        }
        std::string header_line;
        std::size_t content_length = 0;
        while (std::getline(head, header_line)) {
            if (!header_line.empty() && header_line.back() == '\r') {
                header_line.pop_back();
            }
            if (header_line.empty()) {
                continue;
            }
            const std::size_t colon = header_line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = header_line.substr(0, colon);
            for (char& ch : name) {
                ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
            }
            std::string value = header_line.substr(colon + 1);
            const std::size_t first = value.find_first_not_of(' ');
            value = first == std::string::npos ? std::string() : value.substr(first);
            request.headers[name] = value;
            if (name == "content-length") {
                content_length = static_cast<std::size_t>(std::stoull(value));
            }
        }
        request.body = data.substr(header_end + 4);
        while (request.body.size() < content_length) {
            const int n = ::recv(client_fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return;
            }
            request.body.append(buffer, static_cast<std::size_t>(n));
        }
        request.body.resize(content_length);

        FakeResponse response;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!script_.empty()) {
                response = script_.front();
                script_.pop_front();
            }
            received_.push_back(std::move(request));
        }
        if (response.drop) {
            return;  // ACK 丢:收货不回
        }
        std::string out = "HTTP/1.1 " + std::to_string(response.status) + " x\r\n";
        out += "Content-Type: application/json\r\n";
        out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        out += "Connection: close\r\n";
        for (const auto& [name, value] : response.headers) {
            out += name + ": " + value + "\r\n";
        }
        out += "\r\n" + response.body;
        (void)SendAll(client_fd, out);
    }

    socket_t listener_ = kInvalidSocket;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::deque<FakeResponse> script_;
    std::vector<CollectedRequest> received_;
};

// 绑一个端口拿号再放掉("断网"用:连上去就是拒绝)。
int ReserveThenReleasePort() {
    EnsureSocketsReady();
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd != kInvalidSocket);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    sockaddr_in bound{};
#ifdef _WIN32
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
    const int port = ntohs(bound.sin_port);
    CloseSocket(fd);
    return port;
}

bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Journal 桩(照 service 单测的 Harness 裁剪:一场 session 一轮完整回合)。
// ---------------------------------------------------------------------------
class FixedClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

struct JournalFixture {
    FixedClock clock;
    std::filesystem::path session_dir;
    std::optional<TrajectoryRecorder> recorder;
    std::string workspace_key = "ws-000000000000";
    std::string session_id = "20260901-000000-TELEXP";

    explicit JournalFixture(const char* tag) {
        session_dir = std::filesystem::temp_directory_path() /
                      ("lubancode-tel-exp-e2e-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(session_dir, ec);
        std::filesystem::create_directories(session_dir / "artifacts", ec);
        EventScope scope;
        scope.workspace_key = workspace_key;
        scope.session_id = session_id;
        scope.run_id = "main-tel-1";
        scope.run_kind = RunKind::MainSession;
        scope.visibility = {Visibility::HostOnly};
        auto started = TrajectoryRecorder::Start(session_dir / "main.jsonl",
                                                 session_dir / "artifacts", scope,
                                                 RecorderOptions{}, &clock);
        REQUIRE(started.has_value());
        recorder = std::move(*started);
        // RunStarted 走专用写口(generic Record 的 schema 校验对它另有整表,
        // T1 测试同款)。
        const RecordReceipt run_started = recorder->WriteRunStarted(
            nlohmann::json{{"start_reason", "process_launch"}}, Durability::ProcessCrash);
        REQUIRE(run_started.status == RecordReceipt::Status::Committed);
    }

    EventScope Scope(std::optional<std::string> turn, std::optional<std::string> request,
                     std::optional<std::string> call) {
        EventScope scope;
        scope.workspace_key = workspace_key;
        scope.session_id = session_id;
        scope.run_id = "main-tel-1";
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

    void CompleteTurn(const std::string& turn) {
        Put(EventKind::TurnStarted, Scope(turn, {}, {}),
            nlohmann::json{{"trigger", "external_user"}});
        EventScope input = Scope(turn, {}, {});
        input.actor = Actor::User;
        input.origin = Origin::ExternalUser;
        Put(EventKind::InputReceived, input,
            nlohmann::json{{"input_id", "input-0001"},
                           {"content", nlohmann::json::array({"text"})},
                           {"channel", "terminal"},
                           {"sender", nlohmann::json{{"kind", "local_user"}}}});
        EventScope prep = Scope(turn, "req-0001", {});
        prep.actor = Actor::Model;
        prep.origin = Origin::ProviderModel;
        const RecordReceipt prepared =
            Put(EventKind::ModelRequestPrepared, prep,
                nlohmann::json{{"model", "demo-model"},
                               {"provider", "demo"},
                               {"wire", "responses"},
                               {"message_refs", nlohmann::json::array()}});
        Put(EventKind::ModelRequestSent, Scope(turn, "req-0001", {}),
            nlohmann::json{{"prepared_event_id", prepared.event_id}});
        Put(EventKind::ModelOutputCompleted, Scope(turn, "req-0001", {}),
            nlohmann::json{{"output_id", "output-0001"},
                           {"blocks", nlohmann::json::array()},
                           {"stop_reason", "end_turn"}});
        Put(EventKind::TurnCompleted, Scope(turn, {}, {}), nlohmann::json{{"outcome", "succeeded"}});
    }

    std::string LastEventId() const {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08llu",
                      static_cast<unsigned long long>(recorder->next_seq() - 1));
        return "main-tel-1:evt-" + std::string(buffer);
    }

    void CloseRun() {
        const RecordReceipt receipt = recorder->FinishRun(
            EventKind::RunCompleted, std::string(), Durability::ProcessCrash);
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    }
};

TelemetryServiceOptions MakeOptions(const std::filesystem::path& root,
                                    const std::string& endpoint) {
    TelemetryServiceOptions options;
    options.telemetry_root = root;
    options.resource.service_version = "0.26.0-test";
    options.resource.workspace_key = "ws-000000000000";
    options.resource.frontend = "terminal";
    options.tick_ms = 40;
    options.flush_interval_ms = 30;
    options.exporter.endpoint = endpoint;
    options.exporter.timeout_ms = 4000;
    options.exporter.retry.base_ms = 60;   // 测试退避要快
    options.exporter.retry.max_ms = 200;
    return options;
}

// 一只可直接出口的 traces 批(payload 走 T0 编码器)。
SpoolBatchRecord MakeTracesRecord(const char* batch_id) {
    TraceSpan span;
    span.trace_id = "10000000000000000000000000000001";
    span.span_id = "2000000000000001";
    span.name = "lubancode.agent.run";
    span.start_unix_nano = 1759000000000000000LL;
    span.end_unix_nano = 1759000001000000000LL;
    span.source_event_id = "main-tel-1:evt-00000001";
    span.source_terminal_event_id = "main-tel-1:evt-00000008";
    SpoolBatchRecord record;
    record.batch_id = batch_id;
    record.signal = "traces";
    record.workspace_key = "ws-000000000000";
    record.session_id = "s1";
    record.stream_id = "main.jsonl";
    record.first_event_id = "evt-1";
    record.last_event_id = "evt-8";
    record.last_event_hash = "h";
    record.payload = EncodeTracesRequest(BuildResourceAttributes(ResourceInputs{}), {span});
    return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// exporter 直发:合同与分型
// ---------------------------------------------------------------------------

TEST_CASE("合同:traces 批 POST /v1/traces,body 过 T0 校验,带批次识别头") {
    FakeCollector collector;
    OtlpExporterOptions options;
    options.endpoint = collector.endpoint();
    options.timeout_ms = 3000;
    OtlpHttpExporter exporter(options);

    const SpoolBatchRecord record = MakeTracesRecord("batch-000001");
    const ExportAttempt attempt = exporter.Export(record, nullptr);
    REQUIRE(attempt.kind == ExportOutcomeKind::Accepted);
    CHECK(attempt.http_status == 200);
    CHECK(attempt.body_bytes > 0);

    REQUIRE(collector.RequestCount() == 1);
    // 拷一份再断言:Requests() 回的是临时 vector,直接绑 front() 是悬垂引用。
    const CollectedRequest seen = collector.Requests().front();
    CHECK(seen.method == "POST");
    CHECK(seen.path == "/v1/traces");
    CHECK(seen.headers.at("content-type") == "application/json");
    CHECK(seen.headers.at("x-lubancode-batch-id") == "batch-000001");
    const nlohmann::json body = nlohmann::json::parse(seen.body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    CHECK_FALSE(ValidateOtlpTracesJson(body).has_value());  // T0 合同闸
    CHECK(body.at("resourceSpans").size() == 1);
}

TEST_CASE("合同:metrics 批走 /v1/metrics") {
    FakeCollector collector;
    OtlpExporterOptions options;
    options.endpoint = collector.endpoint();
    options.timeout_ms = 3000;
    OtlpHttpExporter exporter(options);

    SpoolBatchRecord record;
    record.batch_id = "batch-m1";
    record.signal = "metrics";
    record.workspace_key = "ws";
    record.session_id = "s";
    record.stream_id = "main.jsonl";
    record.first_event_id = "e1";
    record.last_event_id = "e2";
    record.payload = EncodeMetricsRequest(
        BuildResourceAttributes(ResourceInputs{}),
        {MetricSample{.name = "lubancode.turn.completed_total", .value = 1}});
    const ExportAttempt attempt = exporter.Export(record, nullptr);
    REQUIRE(attempt.kind == ExportOutcomeKind::Accepted);
    REQUIRE(collector.RequestCount() == 1);
    CHECK(collector.Requests().front().path == "/v1/metrics");
    const nlohmann::json body =
        nlohmann::json::parse(collector.Requests().front().body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    CHECK(body.contains("resourceMetrics"));
}

TEST_CASE("凭证:token 走 Authorization 头,不进 URL(§15.4/§19.4)") {
    FakeCollector collector;
    OtlpExporterOptions options;
    options.endpoint = collector.endpoint();
    options.timeout_ms = 3000;
    options.token_source = []() -> std::optional<std::string> { return std::string("tok-123"); };
    OtlpHttpExporter exporter(options);

    const ExportAttempt attempt = exporter.Export(MakeTracesRecord("batch-token"), nullptr);
    REQUIRE(attempt.kind == ExportOutcomeKind::Accepted);
    const CollectedRequest seen = collector.Requests().front();  // 拷贝,防悬垂
    CHECK(seen.headers.at("authorization") == "Bearer tok-123");
    CHECK(seen.path.find("tok-123") == std::string::npos);  // token 不进 URL
}

TEST_CASE("分型:429 带 Retry-After / 503 / 404 / partial") {
    FakeCollector collector;
    OtlpExporterOptions options;
    options.endpoint = collector.endpoint();
    options.timeout_ms = 3000;
    OtlpHttpExporter exporter(options);
    const SpoolBatchRecord record = MakeTracesRecord("batch-classify");

    collector.Queue(FakeResponse{.status = 429, .body = "{}",
                                 .headers = {{"Retry-After", "2"}}});
    ExportAttempt attempt = exporter.Export(record, nullptr);
    CHECK(attempt.kind == ExportOutcomeKind::Retryable);
    CHECK(attempt.http_status == 429);
    CHECK(attempt.retry_after_ms == 2000);

    collector.Queue(FakeResponse{.status = 503, .body = "busy"});
    attempt = exporter.Export(record, nullptr);
    CHECK(attempt.kind == ExportOutcomeKind::Retryable);
    CHECK(attempt.error_code == "telemetry.export.http_retryable");

    collector.Queue(FakeResponse{.status = 404, .body = "no route"});
    attempt = exporter.Export(record, nullptr);
    CHECK(attempt.kind == ExportOutcomeKind::Permanent);
    CHECK(attempt.error_code == "telemetry.export.http_permanent");

    // partial success(§19.3):收了 4 拒了 1,errorMessage 截帽进 detail。
    collector.Queue(FakeResponse{
        .status = 200,
        .body = R"({"partialSuccess":{"rejectedSpans":"1","acceptedSpans":"4",)"
                R"("errorMessage":"queue full for one span"}})"});
    attempt = exporter.Export(record, nullptr);
    CHECK(attempt.kind == ExportOutcomeKind::Partial);
    CHECK(attempt.rejected_points == 1);
    CHECK(attempt.accepted_points == 4);
    CHECK(attempt.detail.find("queue full") != std::string::npos);

    // partial 全拒:按可重试走(双限兜底,不盲发)。
    collector.Queue(FakeResponse{
        .status = 200,
        .body = R"({"partialSuccess":{"rejectedSpans":"5","acceptedSpans":"0"}})"});
    attempt = exporter.Export(record, nullptr);
    CHECK(attempt.kind == ExportOutcomeKind::Retryable);
    CHECK(attempt.error_code == "telemetry.export.partial_all_rejected");
}

TEST_CASE("ACK 丢:collector 收货不回 -> 传输错可重试;重发同 batch id(重复可识别)") {
    FakeCollector collector;
    collector.Queue(FakeResponse{.drop = true});  // 第一发:收了不回
    OtlpExporterOptions options;
    options.endpoint = collector.endpoint();
    options.timeout_ms = 3000;
    OtlpHttpExporter exporter(options);
    const SpoolBatchRecord record = MakeTracesRecord("batch-dedup");

    const ExportAttempt first = exporter.Export(record, nullptr);
    CHECK(first.kind == ExportOutcomeKind::Retryable);
    CHECK(first.http_status == 0);  // 没到 HTTP 层
    CHECK(first.error_code == "telemetry.export.transport");

    const ExportAttempt second = exporter.Export(record, nullptr);  // 默认 200
    REQUIRE(second.kind == ExportOutcomeKind::Accepted);

    REQUIRE(collector.RequestCount() == 2);
    const auto requests = collector.Requests();
    CHECK(requests[0].headers.at("x-lubancode-batch-id") == "batch-dedup");
    CHECK(requests[1].headers.at("x-lubancode-batch-id") == "batch-dedup");
    CHECK(requests[0].body == requests[1].body);  // 对端凭 id+body 幂等去重
}

TEST_CASE("断网:连不上按传输临时错(§19.2)") {
    const int dead_port = ReserveThenReleasePort();
    OtlpExporterOptions options;
    options.endpoint = "http://127.0.0.1:" + std::to_string(dead_port);
    options.timeout_ms = 2000;
    OtlpHttpExporter exporter(options);
    const ExportAttempt attempt = exporter.Export(MakeTracesRecord("batch-dead"), nullptr);
    CHECK(attempt.kind == ExportOutcomeKind::Retryable);
    CHECK(attempt.error_code == "telemetry.export.transport");
}

TEST_CASE("取消:关停时在传输的请求发取消(§26.4)") {
    FakeCollector collector;
    collector.Queue(FakeResponse{.drop = true});  // 收了永不回
    OtlpExporterOptions options;
    options.endpoint = collector.endpoint();
    options.timeout_ms = 3000;
    OtlpHttpExporter exporter(options);
    std::atomic<bool> cancel{true};
    const ExportAttempt attempt = exporter.Export(MakeTracesRecord("batch-cancel"), &cancel);
    CHECK(attempt.kind == ExportOutcomeKind::Cancelled);
}

bool CursorFileReaches(const std::filesystem::path& root, const std::string& event_id) {
    // 加括号防 windows.h 的 LoadCursor 宏(本册为了假 collector 带 winsock)。
    const auto cursor = (LoadCursor)(root / "cursors", "ws-000000000000",
                                     "20260901-000000-TELEXP", "main.jsonl", nullptr);
    return cursor.has_value() && cursor->last_event_id == event_id;
}

// ---------------------------------------------------------------------------
// 服务全链:spool 出队 -> POST -> ACK 删段 + tombstone;故障注入账不乱
// ---------------------------------------------------------------------------

TEST_CASE("全链:Journal -> 投影 -> spool -> 出口 -> ACK 删段,tombstone 记账") {
    JournalFixture fixture("fullchain");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    FakeCollector collector;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-fullchain";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root, collector.endpoint()));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});

    // 出口收货:traces 批 body 过 T0 合同闸。
    REQUIRE(WaitUntil([&] { return collector.RequestCount() >= 1; }));
    bool saw_valid_traces = false;
    for (const CollectedRequest& seen : collector.Requests()) {
        if (seen.path != "/v1/traces") {
            continue;
        }
        const nlohmann::json body = nlohmann::json::parse(seen.body, nullptr, false);
        if (!body.is_discarded() && !ValidateOtlpTracesJson(body).has_value()) {
            saw_valid_traces = true;
        }
    }
    CHECK(saw_valid_traces);

    // ACK 全链:sealed 段删净,active 清空,cursor 推到底。
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.segments == 0 && status.spool.active_batches == 0;
    }));
    REQUIRE(WaitUntil([&] {
        return CursorFileReaches(root, fixture.LastEventId());
    }));
    service.Stop();

    // tombstone 簿:ACK 的批 id 进 state.json(§18.2 删除失败防重复无限发)。
    std::ifstream state_file(root / "state.json", std::ios::binary);
    std::stringstream buffer;
    buffer << state_file.rdbuf();
    const nlohmann::json state = nlohmann::json::parse(buffer.str(), nullptr, false);
    REQUIRE_FALSE(state.is_discarded());
    CHECK(state.contains("tombstones"));
    CHECK(state.at("tombstones").size() >= 1);
    const auto status = service.Status();
    CHECK(status.exporter.exported_batches_total >= 1);
    CHECK(status.exporter.last_success_at_ms > 0);
}

TEST_CASE("断网->补送:collector 挂了 spool 留账,重启后全数补送") {
    JournalFixture fixture("recover");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    const int dead_port = ReserveThenReleasePort();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-recover";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(
        MakeOptions(root, "http://127.0.0.1:" + std::to_string(dead_port)));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});

    // 断网期:投影照跑,cursor 推进,spool 留账,出口有失败账。
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.sealed_batches >= 1 &&
               status.exporter.last_error_code == "telemetry.export.transport";
    }));
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, fixture.LastEventId()); }));

    // collector 重启(同端口):spool 存量全数补送,段删净。
    {
        FakeCollector collector(dead_port);
        REQUIRE(WaitUntil([&] {
            const auto status = service.Status();
            return status.spool.segments == 0 && status.spool.active_batches == 0;
        }, 15000));
        REQUIRE(collector.RequestCount() >= 1);
        bool saw_valid = false;
        for (const CollectedRequest& seen : collector.Requests()) {
            if (seen.path == "/v1/traces") {
                const nlohmann::json body = nlohmann::json::parse(seen.body, nullptr, false);
                if (!body.is_discarded() && !ValidateOtlpTracesJson(body).has_value()) {
                    saw_valid = true;
                }
            }
        }
        CHECK(saw_valid);
    }
    service.Stop();
}

TEST_CASE("5xx 后退避补送:第一发 503,退避后补送成功") {
    JournalFixture fixture("retry5xx");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    FakeCollector collector;
    collector.Queue(FakeResponse{.status = 503, .body = "busy"});
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-retry5xx";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root, collector.endpoint()));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});

    // 先钉住"503 真的挨过一发"(不然后面的空仓判定会空转过门):
    // 第一发吃到脚本里的 503,退避,重发吃默认 200,补送出清。
    REQUIRE(WaitUntil([&] { return collector.RequestCount() >= 1; }));
    REQUIRE(collector.Requests().front().path == "/v1/traces");
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.segments == 0 && status.spool.active_batches == 0;
    }, 15000));
    service.Stop();

    REQUIRE(collector.RequestCount() >= 2);  // 503 一发 + 退避后补发
    const auto status = service.Status();
    CHECK(status.exporter.retried_batches_total >= 1);
    CHECK(status.exporter.exported_batches_total >= 1);
}

TEST_CASE("永久错:404 停该 endpoint 代,后续批不再发") {
    JournalFixture fixture("permanent");
    fixture.CompleteTurn("turn-0001");
    fixture.CompleteTurn("turn-0002");
    fixture.CloseRun();

    FakeCollector collector;
    collector.Queue(FakeResponse{.status = 404, .body = "no route"});
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-permanent";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root, collector.endpoint()));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});

    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.exporter.gate_reason == "telemetry.export.http_permanent";
    }));
    // 停代后不再发请求;spool 留账不丢;投影照跑。
    const std::size_t requests_after_stop = collector.RequestCount();
    CHECK(requests_after_stop == 1);
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, fixture.LastEventId()); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(collector.RequestCount() == requests_after_stop);
    CHECK(service.Status().spool.sealed_batches >= 1);
    service.Stop();
}

TEST_CASE("业务线程不等网络:出口挂死(收货不回)时 Notify 快、投影照跑") {
    JournalFixture fixture("backpressure");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    FakeCollector collector;
    collector.Queue(FakeResponse{.drop = true});  // 黑洞:收货永不回
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-backpressure";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root, collector.endpoint()));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);

    // 出口挂死期间,业务侧 Notify 必须立刻回(µs 级纪律,这里钉 < 200ms)。
    const auto begin = std::chrono::steady_clock::now();
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    CHECK(elapsed.count() < 200);

    // 投影不受网络拖累:cursor 推到底,spool 有账,collector 一颗业务数据
    // 都没收到合法 OTLP(body 全在,但它永不回——那正是故障注入点)。
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, fixture.LastEventId()); }));
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.sealed_batches >= 1;
    }));
    CHECK(service.Status().queue.size_items == 0);  // 队不积压(已落 spool)
    service.Stop();
}

TEST_CASE("flush:黑洞出口有界返回;出口恢复后 flush 出清") {
    JournalFixture fixture("flush");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    const int dead_port = ReserveThenReleasePort();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-flush";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(
        MakeOptions(root, "http://127.0.0.1:" + std::to_string(dead_port)));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.sealed_batches >= 1;
    }));

    const auto begin = std::chrono::steady_clock::now();
    CHECK_FALSE(service.Flush(300));  // 有界:不吊死在公网上
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    CHECK(elapsed.count() < 3000);

    {
        FakeCollector collector(dead_port);
        CHECK(service.Flush(8000));
        CHECK(service.Status().spool.segments == 0);
    }
    service.Stop();
}

TEST_CASE("consent 门:公网 https 无授权不发;授权后才会真发起(§8.4)") {
    JournalFixture fixture("consent");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    // 非回环 endpoint 用 RFC 2606 保留域(不连任何真外部端点):授权前门关、
    // 一发不出;授权后门开,出口真的会去发起(连不上按传输错记账——
    // "有没有发"以出口账为准,不依赖连得通)。
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-consent";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryServiceOptions options = MakeOptions(root, "https://collector.invalid");
    options.exporter.timeout_ms = 1500;  // 连不上也要快退
    options.exporter.retry.base_ms = 50;
    options.exporter.retry.max_ms = 150;
    TelemetryService service(options);
    REQUIRE(service.Start());
    CHECK(service.ConsentState() == "required");
    {
        const auto status = service.Status();
        CHECK(status.exporter.gate_reason == "telemetry.consent_required");
    }
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, fixture.LastEventId()); }));
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.sealed_batches >= 1;
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(service.Status().exporter.last_error_at_ms == 0);  // 没授权:零发起、零错误账

    REQUIRE(service.GrantConsent());
    CHECK(service.ConsentState() == "granted");
    CHECK(service.Status().exporter.gate_reason.empty());
    // 门开了:真发起过(传输错也是"发过"的账)。
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.exporter.last_error_code == "telemetry.export.transport";
    }, 10000));

    service.RevokeConsent();
    CHECK(service.ConsentState() == "required");
    CHECK(service.Status().exporter.gate_reason == "telemetry.consent_required");
    service.Stop();
}

TEST_CASE("pause:停出口,投影与 spool 照常落(§24.2)") {
    JournalFixture fixture("pause");
    fixture.CompleteTurn("turn-0001");
    fixture.CloseRun();

    FakeCollector collector;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-pause";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(MakeOptions(root, collector.endpoint()));
    REQUIRE(service.Start());
    service.SetExportPaused(true);
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, fixture.LastEventId()); }));
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.sealed_batches >= 1;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(collector.RequestCount() == 0);  // 暂停:零请求
    CHECK(service.Status().exporter.paused);

    service.SetExportPaused(false);
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.segments == 0;
    }, 10000));
    CHECK(collector.RequestCount() >= 1);
    service.Stop();
}

TEST_CASE("spool clear:两步删除后批账落 tombstone,cursor 对账不报孤儿(§24.2/§18.5)") {
    JournalFixture fixture("clear");
    fixture.CompleteTurn("turn-0001");
    fixture.CompleteTurn("turn-0002");
    // 注意:run 先不关——清账之后还要追加 turn-0003 验"新账照投不误报孤儿",
    // 关了 run 再写事件会被账本拒(state.run_closed)。

    const int dead_port = ReserveThenReleasePort();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       "lubancode-tel-e2e-clear";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TelemetryService service(
        MakeOptions(root, "http://127.0.0.1:" + std::to_string(dead_port)));
    REQUIRE(service.Start());
    service.RegisterSession(fixture.workspace_key, fixture.session_id, fixture.session_dir);
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] {
        const auto status = service.Status();
        return status.spool.sealed_batches >= 1;
    }));

    const auto [segments, batches] = service.ClearSpool();
    CHECK(segments >= 1);
    CHECK(batches >= 1);
    {
        const auto status = service.Status();
        CHECK(status.spool.segments == 0);
        CHECK(status.spool.sealed_batches == 0);
    }

    // 清账之后投影继续跑:新回合照投,不因退场水位误报 cursor 孤儿。
    fixture.CompleteTurn("turn-0003");
    service.Notify(CommitWake{fixture.workspace_key, fixture.session_id, "main.jsonl"});
    REQUIRE(WaitUntil([&] { return CursorFileReaches(root, fixture.LastEventId()); }));
    const auto status = service.Status();
    for (const auto& stream : status.streams) {
        CHECK(stream.error_code.empty());
    }
    CHECK(service.Status().spool.sealed_batches >= 1);  // 新账照落
    service.Stop();
    fixture.CloseRun();
}
