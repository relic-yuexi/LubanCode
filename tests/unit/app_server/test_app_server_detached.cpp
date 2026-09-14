// 助理模式断线合同册(常驻助理 Web 主界面单 W0,冻结合同的回归钉):
//   1. 能力声明区分:Detached 的 initialize 带 capabilities.mode=assistant、
//      workLifetime=detached 与助理方法名;基线 MakeInitializeResult(独立
//      app-server 的 initialize 面)一个都不带——不全局改旧客户端语义;
//   2. SessionBound(旧语义,缺省):连接收线即打断在跑回合(后端收
//      cancel,终态 interrupted,零正文);
//   3. Detached(助理模式):连接收线只撤订阅,已受理回合照跑到终态
//      (final)并落账——关标签/刷新不打断工作;停止任务是 turn/interrupt,
//      停止助理是 shutdown,断线不是其中任何一个。
// 真监听回环 + 扣住应答的假后端(release 前:SessionBound 吃 cancel 旗,
// Detached 不吃;release 后吐一幕纯文本)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/protocol.hpp"
#include "app_server/server.hpp"
#include "app_server/ws_sockets.hpp"
#include "app_server/ws_transport.hpp"
#include "tools/path_utils.hpp"

using namespace lubancode;

namespace {

// 设置/还原环境变量(v3 册口径:thread 走 v3 新账)。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

// 共享闸:同一服务里每场 thread 一只新后端,但"放行/取消/正文"三笔账
// 全服务共享(断言口径:模型到底跑没跑、吐没吐话)。
struct HoldGate {
    std::atomic<bool> released{false};
    std::atomic<int> cancelled{0};
    std::atomic<int> emitted{0};
};

class HoldBackend : public api::Backend {
public:
    explicit HoldBackend(std::shared_ptr<HoldGate> gate) : gate_(std::move(gate)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        while (!gate_->released.load()) {
            if (cancel != nullptr && cancel->load()) {
                gate_->cancelled.fetch_add(1);
                return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        on_event(api::MessageStart{"msg", "fake-model"});
        on_event(api::TextDelta{"关页之后才回的话"});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}});
        gate_->emitted.fetch_add(1);
        return {};
    }

private:
    std::shared_ptr<HoldGate> gate_;
};

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return tools::PathToUtf8(dir);
}

// ---- 测试用 WS 客户端(裸 socket + 手搓升级与帧;ws 册同款裁剪) ----

std::string MaskedTextFrame(const std::string& payload) {
    static const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>(mask[i]));
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return frame;
}

class TestWsClient {
public:
    explicit TestWsClient(int port) {
        std::string error;
        socket_ = app_server::net::ConnectTcp("127.0.0.1", port, error);
        REQUIRE_MESSAGE(socket_.valid(), ("connect 失败: " + error).c_str());
        socket_.SetRecvTimeoutMs(300);
    }

    bool Upgrade() {
        const std::string request =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        if (!socket_.SendAll(request)) {
            return false;
        }
        std::string header;
        while (header.find("\r\n\r\n") == std::string::npos) {
            char buffer[1024];
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                return false;
            }
            header.append(buffer, buffer + got);
        }
        return header.find("HTTP/1.1 101") != std::string::npos;
    }

    void SendText(const std::string& message) { socket_.SendAll(MaskedTextFrame(message)); }

    void HardClose() { socket_.Close(); }

    std::optional<std::string> ReadText() {
        while (inbox_.empty()) {
            char buffer[65536];
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                return std::nullopt;
            }
            Feed(std::string_view(buffer, buffer + got));
            if (failed_) {
                return std::nullopt;
            }
        }
        std::string message = std::move(inbox_.front());
        inbox_.pop_front();
        return message;
    }

private:
    void Feed(std::string_view chunk) {
        buffer_.append(chunk);
        while (true) {
            if (buffer_.size() < 2) {
                return;
            }
            const std::uint8_t first = static_cast<std::uint8_t>(buffer_[0]);
            const std::uint8_t second = static_cast<std::uint8_t>(buffer_[1]);
            const bool fin = (first & 0x80) != 0;
            const std::uint8_t opcode = first & 0x0F;
            std::uint64_t length = second & 0x7F;
            std::size_t header_size = 2;
            if (length == 126) {
                if (buffer_.size() < 4) {
                    return;
                }
                length = (static_cast<std::uint8_t>(buffer_[2]) << 8) | static_cast<std::uint8_t>(buffer_[3]);
                header_size = 4;
            } else if (length == 127) {
                if (buffer_.size() < 10) {
                    return;
                }
                length = 0;
                for (int i = 0; i < 8; ++i) {
                    length = (length << 8) | static_cast<std::uint8_t>(buffer_[2 + i]);
                }
                header_size = 10;
            }
            if (buffer_.size() < header_size + length) {
                return;
            }
            const std::string payload = buffer_.substr(header_size, static_cast<std::size_t>(length));
            buffer_.erase(0, header_size + static_cast<std::size_t>(length));
            if (opcode >= 0x8) {
                if (opcode == 0x8 && fin) {
                    failed_ = true;
                }
                continue;
            }
            if (!fin || opcode != 0x1) {
                failed_ = true;
                return;
            }
            inbox_.push_back(payload);
        }
    }

    app_server::net::Socket socket_;
    std::string buffer_;
    std::deque<std::string> inbox_;
    bool failed_ = false;
};

std::optional<nlohmann::json> WaitForMessage(TestWsClient& client,
                                             const std::function<bool(const nlohmann::json&)>& match,
                                             int tries = 200) {
    for (int i = 0; i < tries; ++i) {
        while (const std::optional<std::string> message = client.ReadText()) {
            const nlohmann::json parsed = nlohmann::json::parse(*message, nullptr, false);
            if (!parsed.is_discarded() && match(parsed)) {
                return parsed;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// 能力声明区分(W0:不全局改旧客户端语义)
// ---------------------------------------------------------------------------

TEST_CASE("assistant 能力声明:基线不带 mode,Docked 扩展口 additive 生效") {
    // 基线(独立 app-server 的 initialize 面):无 mode/workLifetime/助理方法。
    const nlohmann::json base = app_server::MakeInitializeResult("test", "test-platform");
    REQUIRE(base.contains("capabilities"));
    CHECK_FALSE(base["capabilities"].contains("mode"));
    CHECK_FALSE(base["capabilities"].contains("workLifetime"));
    CHECK(std::find(base["capabilities"]["methods"].begin(),
                    base["capabilities"]["methods"].end(),
                    "assistant/status") == base["capabilities"]["methods"].end());

    // 助理模式装配:initialize_result_extender 走线(WS dispatcher 铸的
    // initialize 真吃过扩展口),字段 additive、旧字段一字未动。
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    app_server::ServerOptions options;
    options.cwd = "/test/assistant";
    options.workspaces_dir = MakeTempDir("lubancode_test_detached_caps");
    options.work_lifetime = app_server::ServerOptions::WorkLifetime::Detached;
    options.initialize_result_extender = [](nlohmann::json result) {
        result["capabilities"]["mode"] = "assistant";
        result["capabilities"]["workLifetime"] = "detached";
        result["capabilities"]["methods"].push_back("assistant/status");
        result["capabilities"]["methods"].push_back("config/status");
        result["capabilities"]["methods"].push_back("config/model/set");
        result["capabilities"]["methods"].push_back("config/test");
        return result;
    };
    app_server::Server server(std::move(options),
                              []() -> std::unique_ptr<api::Backend> { return nullptr; }, nullptr);
    app_server::WsOptions ws;
    ws.bind_host = "127.0.0.1";
    ws.port = 0;
    app_server::WsTransport transport(ws);
    REQUIRE(transport.Start());
    std::thread serve([&] { server.ServeWsConnection(transport); });
    {
        TestWsClient client(transport.actual_port());
        REQUIRE(client.Upgrade());
        client.SendText(R"({"id":1,"method":"initialize","params":{"clientName":"t"}})");
        const auto init = WaitForMessage(client, [](const nlohmann::json& m) { return m.contains("id"); });
        REQUIRE(init.has_value());
        REQUIRE((*init)["result"].contains("capabilities"));
        CHECK((*init)["result"]["capabilities"]["mode"] == "assistant");
        CHECK((*init)["result"]["capabilities"]["workLifetime"] == "detached");
        const auto& methods = (*init)["result"]["capabilities"]["methods"];
        CHECK(std::find(methods.begin(), methods.end(), "assistant/status") != methods.end());
        CHECK(std::find(methods.begin(), methods.end(), "config/model/set") != methods.end());
        CHECK((*init)["result"]["protocolVersion"].get<std::string>() ==
              std::string(app_server::kProtocolVersion));
        CHECK((*init)["result"].contains("lubancodeVersion"));
        client.HardClose();
    }
    transport.Stop();
    serve.join();
}

// ---------------------------------------------------------------------------
// 断线合同:两个模式同一剧本(发话 → 后端扣着 → 断线 → 分岔断言)
// ---------------------------------------------------------------------------

TEST_CASE("断线合同:SessionBound 断线即打断;Detached 断线任务照跑到终态") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");

    for (int mode = 0; mode < 2; ++mode) {
        const bool detached = mode == 1;
        CAPTURE(detached);
        const std::string workspaces =
            MakeTempDir(detached ? "lubancode_test_detached_on" : "lubancode_test_detached_off");
        auto gate = std::make_shared<HoldGate>();

        app_server::WsOptions ws;
        ws.bind_host = "127.0.0.1";
        ws.port = 0;
        app_server::WsTransport transport(ws);
        REQUIRE(transport.Start());

        app_server::ServerOptions options;
        const std::string cwd = (tools::Utf8ToPath(workspaces) / "ws").generic_string();
        std::error_code ec;
        std::filesystem::create_directories(tools::Utf8ToPath(cwd), ec);
        options.cwd = cwd;
        options.workspaces_dir = workspaces;
        options.outbox_capacity = 256;
        options.interrupt_hard_deadline_ms = 500;  // 硬时限收短,SessionBound 不等 15s
        options.work_lifetime =
            detached ? app_server::ServerOptions::WorkLifetime::Detached
                     : app_server::ServerOptions::WorkLifetime::SessionBound;
        app_server::Server server(
            std::move(options),
            [&gate]() -> std::unique_ptr<api::Backend> { return std::make_unique<HoldBackend>(gate); },
            nullptr);

        // 服务线程:一条连完接一条(宿主同款串行语义;纯断线继续等)。
        std::thread serve([&] {
            while (server.ServeWsConnection(transport) ==
                   app_server::Server::WsServeOutcome::Disconnected) {
            }
        });

        std::string thread_id;
        {
            TestWsClient client(transport.actual_port());
            REQUIRE(client.Upgrade());
            client.SendText(R"({"id":1,"method":"initialize","params":{}})");
            REQUIRE(WaitForMessage(client, [](const nlohmann::json& m) { return m.contains("id"); }));
            client.SendText(R"({"method":"initialized"})");
            const std::string thread_params = R"({"id":2,"method":"thread/start","params":{"cwd":")" +
                                             cwd + R"("}})";
            client.SendText(thread_params);
            const auto started = WaitForMessage(
                client, [](const nlohmann::json& m) { return m.value("method", "") == "thread/started"; });
            REQUIRE(started.has_value());
            thread_id = (*started)["params"]["threadId"].get<std::string>();
            client.SendText(R"({"id":3,"method":"turn/start","params":{"threadId":")" + thread_id +
                            R"(","text":"关页还跑吗","clientOperationId":"op-detach-1"}})");
            const auto accepted = WaitForMessage(
                client, [](const nlohmann::json& m) { return m.contains("id") && m["id"] == 3; });
            REQUIRE(accepted.has_value());

            // 关标签/刷新的分身:硬断。后端此刻仍扣着。
            client.HardClose();
        }

        if (!detached) {
            // 旧语义:断线即打断。硬时限(500ms)内收口:cancel 计一票、
            // 零正文;之后放行也不再有话(回合早死)。
            for (int i = 0; i < 200 && gate->cancelled.load() == 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            REQUIRE(gate->cancelled.load() == 1);
            CHECK(gate->emitted.load() == 0);
            gate->Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CHECK(gate->emitted.load() == 0);  // 没有第二次跑
        } else {
            // 助理模式:断线只撤订阅。200ms 内不许 cancel、不许终态;
            // 放行后跑到 final 并落账(领域账核对,不靠事件)。
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CHECK(gate->cancelled.load() == 0);
            gate->Release();
            bool final_seen = false;
            nlohmann::json operation;
            for (int i = 0; i < 400 && !final_seen; ++i) {
                std::string read_error;
                operation = server.HandleOperationRead(thread_id, "op-detach-1", "", read_error);
                if (read_error.empty() && operation.contains("status") &&
                    operation["status"].is_string() &&
                    operation["status"].get<std::string>() == "final") {
                    final_seen = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            REQUIRE(final_seen);
            CHECK(gate->emitted.load() == 1);  // 模型只跑过一次(同一输入不双跑)
            CHECK(gate->cancelled.load() == 0);
        }
        transport.Stop();
        serve.join();
    }
}
