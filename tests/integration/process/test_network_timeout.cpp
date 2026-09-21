// M11(网络超时):验证两个 API 客户端在"连不上"“半路断流"两种场景下,
// 真的会主动断开、报清楚的中文错误,不会干等。
//
// 本地起一个最简陋的原始 socket 假服务器(不是真 HTTP 服务器,手写状态行 +
// 几行 SSE 就够骗过 cpr/libcurl 的响应解析):
//   - 半路断流:accept 之后先吐一帧合法的 SSE(message_start),然后既不再
//     发数据也不关连接——模拟"连上了、收到了一半、后面没反应了"。
//     AnthropicBackend/ResponsesBackend 配的是 cpr::LowSpeed,预期在
//     stream_idle_timeout_secs 秒量级内就主动掐断,不会傻等到服务器那边
//     的挂起结束。
//   - 连不上:绑一个端口立刻关掉,拿这个"刚刚还开着、现在没人听"的端口去连,
//     操作系统直接拒绝连接(RST),验证 ErrorKind::Network 的报错文案走的是
//     i18n 里包过的"连接失败: ..."模板,不是 libcurl 原始英文错误串直通。
//   - 回归:完整走完一遍正常的 SSE(短消息),确认新增的超时参数没有破坏
//     "服务器规规矩矩応答"这条主路径。
//
// 硬墙钟(cpr 并发挂死单)另有一组:accept 之后**一个字节都不发**——连接
// 收下了、响应头都不给。真机现场(本机代理/TUN 截胡 127.0.0.1 回环)的挂死
// 就是这副样子:请求进了 cpr::Post 再不返。这组用例把 stream_idle 配得很大,
// 只留 request_hard_timeout_secs 一道闸,证明落锤的是硬墙钟、不是 LowSpeed,
// 而且收场文案分型到"请求硬超时"那一档。
//
// 只在 Windows 下编译(项目当前只在 WIN32 下过测试;POSIX 分支的 socket API
// 写法留了条件编译,但没有 CI 覆盖,谨慎起见别在非 WIN32 平台上悄悄跑一份
// 没验证过的路径)。
// [FD-09 修正]上面这段是老黄历:CI 的 macos-clang 腿实际一直在编译并运行
// 本文件(POSIX 分支有 CI 覆盖)。写侧有一样 POSIX 特有的事得防:对端掐流
// 后继续 send 默认递 SIGPIPE 杀整个测试进程——Linux 走 MSG_NOSIGNAL,
// macOS 靠 accept 后的 SO_NOSIGPIPE(见 SendIfAlive/StartFakeServer)。

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/http_stream_transport.hpp"
#include "api/responses/client.hpp"
#include "api/types.hpp"
#include "cli/i18n.hpp"
#include "config/config.hpp"

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

// WSAStartup 全进程只用做一次;非 Windows 平台是空操作。静态局部变量的
// "首次调用才构造"语义天然保证幂等,doctest 单进程跑一堆 TEST_CASE 也不会
// 重复初始化/提前 WSACleanup。
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

// 绑 127.0.0.1:0(系统分配一个当前空闲的端口),返回监听 socket 和端口号。
struct BoundListener {
    socket_t fd = kInvalidSocket;
    int port = 0;
};

BoundListener BindLoopbackListener() {
    EnsureSocketsReady();

    BoundListener out;
    out.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(out.fd != kInvalidSocket);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(out.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    REQUIRE(::getsockname(out.fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0);
    out.port = ntohs(bound.sin_port);
    return out;
}

// 起一个只服务一次连接的本地假服务器:listen 之后另起一个 detach 的线程
// 等 accept,拿到连接就把原始字节喂给 handler(handler 自己决定怎么回:
// 完整走完的 SSE、还是吐半截就挂起、或者干脆什么也不干)。返回端口,调用方
// 拼 "http://127.0.0.1:<port>" 当 base_url 用。
// 线程 detach 而不 join:挂起类场景的 handler 可能睡得比整个测试跑完还久,
// join 会拖累测试耗时;detach 出去的线程在进程退出时由系统直接收掉,
// 不碰任何跨线程共享状态(只操作自己的 socket 描述符),安全。
int StartFakeServer(std::function<void(socket_t)> handler) {
    const BoundListener listener = BindLoopbackListener();
    REQUIRE(::listen(listener.fd, 1) == 0);

    std::thread([fd = listener.fd, handler]() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const socket_t client_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd != kInvalidSocket) {
#ifdef __APPLE__
            // macOS 没有 MSG_NOSIGNAL:客户端掐流后再往这个 socket 写,默认
            // 递 SIGPIPE 杀进程。socket 级关掉,让 send 走 EPIPE 错误返回
            // (与 src/app_server/ws_sockets.cpp、channel/transport/tcp_socket.cpp
            // 同一套章法)。
            int nosigpipe = 1;
            ::setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
            handler(client_fd);
            CloseSocket(client_fd);
        }
        CloseSocket(fd);
    }).detach();

    return listener.port;
}

void SendAll(socket_t s, const std::string& data) {
    ::send(s, data.data(), static_cast<int>(data.size()), 0);
}

// 往可能已被对端掐断的 socket 写一段,活着才继续(FD-09 用例:客户端掐流
// 后假服务器还要接着发)。Linux 走 MSG_NOSIGNAL;macOS 靠 accept 后的
// SO_NOSIGPIPE;Windows 没有这个信号,send 只回错误码。
bool SendIfAlive(socket_t s, const char* data, int len) {
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
    return ::send(s, data, len, MSG_NOSIGNAL) > 0;
#else
    return ::send(s, data, len, 0) > 0;
#endif
}

// 发送侧干净收尾(发 FIN):不带 Content-Length 的连接式响应体靠这个让
// 客户端读到 EOF、流"正常结束"——区别于掐流和挂死两种非正常收场。
void ShutdownSend(socket_t s) {
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

// 把客户端发来的请求排干(不关心内容)。不排干也不会死锁(TCP 收发缓冲区
// 独立),但排干一下能避免"服务器这边还有没读完的数据就关连接"在个别
// 网络栈上触发 RST 而不是干净的 FIN,让"正常走完"这条回归测试更稳。
void DrainRequest(socket_t s) {
    char buf[4096];
    ::recv(s, buf, sizeof(buf), 0);
}

lubancode::api::Request MakeMinimalRequest() {
    lubancode::api::Request request;
    request.model = "test-model";
    request.max_tokens = 16;
    lubancode::api::Message user_message;
    user_message.role = lubancode::api::Role::User;
    user_message.content.push_back(lubancode::api::TextBlock{"hi"});
    request.messages.push_back(std::move(user_message));
    return request;
}

}  // namespace

TEST_CASE("anthropic: SSE 半路断流(收到部分数据后服务器挂起不再吭声)触发空闲读超时") {
    lubancode::cli::SetLanguage("zh-CN");
    constexpr int kIdleTimeoutSecs = 2;

    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "\r\n"
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"test\"}}\n"
                "\n");
        // 发完这一帧就装死:不再写数据,也不关闭连接,模拟"连上了、收到了
        // 一部分、后面突然没反应了"这种半路断流。
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    lubancode::api::anthropic::AnthropicBackend backend("http://127.0.0.1:" + std::to_string(port), "test-token",
                                                          /*connect_timeout_ms=*/3000,
                                                          /*stream_idle_timeout_secs=*/kIdleTimeoutSecs);

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Network);
    CHECK(result.error().message == lubancode::cli::trf("error.network.stream_idle_timeout", kIdleTimeoutSecs));
    // 真的是空闲超时提前掐断的,不是傻等了服务器那 30 秒挂起。
    CHECK(elapsed < std::chrono::seconds(kIdleTimeoutSecs + 8));
}

TEST_CASE("anthropic: SSE 半路停住时 cancel 在 2s 内掐断,不等下一枚响应字节") {
    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "\r\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"test\"}}\n"
                "\n");
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    lubancode::api::anthropic::AnthropicBackend backend(
        "http://127.0.0.1:" + std::to_string(port), "test-token",
        /*connect_timeout_ms=*/3000,
        /*stream_idle_timeout_secs=*/25,
        /*native_web_search=*/false, /*extra_body=*/{},
        /*extra_headers=*/{},
        /*request_hard_timeout_secs=*/0);
    std::atomic<bool> cancel{false};
    std::thread cancel_thread([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel.store(true);
    });

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {}, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    cancel_thread.join();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Cancelled);
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("anthropic: 服务端连响应头也不回时 cancel 在 2s 内掐断") {
    const int port = StartFakeServer([](socket_t) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });
    lubancode::api::anthropic::AnthropicBackend backend(
        "http://127.0.0.1:" + std::to_string(port), "test-token",
        /*connect_timeout_ms=*/3000,
        /*stream_idle_timeout_secs=*/25,
        /*native_web_search=*/false, /*extra_body=*/{},
        /*extra_headers=*/{},
        /*request_hard_timeout_secs=*/0);
    std::atomic<bool> cancel{false};
    std::thread cancel_thread([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel.store(true);
    });

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {}, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    cancel_thread.join();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Cancelled);
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("responses: SSE 半路断流(收到部分数据后服务器挂起不再吭声)触发空闲读超时") {
    lubancode::cli::SetLanguage("zh-CN");
    constexpr int kIdleTimeoutSecs = 2;

    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "\r\n"
                "event: response.created\n"
                "data: {\"type\":\"response.created\"}\n"
                "\n");
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    lubancode::api::responses::ResponsesBackend backend("http://127.0.0.1:" + std::to_string(port), "test-token",
                                                          /*connect_timeout_ms=*/3000,
                                                          /*stream_idle_timeout_secs=*/kIdleTimeoutSecs);

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Network);
    CHECK(result.error().message == lubancode::cli::trf("error.network.stream_idle_timeout", kIdleTimeoutSecs));
    CHECK(elapsed < std::chrono::seconds(kIdleTimeoutSecs + 8));
}

TEST_CASE("anthropic: 连不上服务器(端口没人监听)报连接失败,消息走 i18n 文案不是 curl 原始英文串") {
    lubancode::cli::SetLanguage("zh-CN");

    // 绑一个端口立刻关掉:那个端口号短时间内大概率还没被别的进程占用,
    // 连过去会被操作系统直接拒绝(RST/ECONNREFUSED),不是"服务器存在但
    // 慢"那种要等超时的场景,是"根本没人听"的快速失败。
    const BoundListener probe = BindLoopbackListener();
    const int port = probe.port;
    CloseSocket(probe.fd);

    lubancode::api::anthropic::AnthropicBackend backend("http://127.0.0.1:" + std::to_string(port), "test-token",
                                                          /*connect_timeout_ms=*/3000,
                                                          /*stream_idle_timeout_secs=*/5);

    const auto result = backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Network);
    CHECK(result.error().message.rfind("连接失败: ", 0) == 0);
}

TEST_CASE("anthropic: 配了新超时参数,服务器正常应答完整 SSE 流仍然成功(回归)") {
    lubancode::cli::SetLanguage("zh-CN");

    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "\r\n"
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"test\"}}\n"
                "\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,"
                "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n"
                "\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
                "\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}\n"
                "\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n"
                "\n");
        // 完整答完就正常关连接(而不是像上面两个用例那样挂起)。
    });

    lubancode::api::anthropic::AnthropicBackend backend("http://127.0.0.1:" + std::to_string(port), "test-token",
                                                          /*connect_timeout_ms=*/3000,
                                                          /*stream_idle_timeout_secs=*/5);

    std::string collected_text;
    const auto result = backend.send_stream(MakeMinimalRequest(), [&](const lubancode::api::StreamEvent& event) {
        if (const auto* delta = std::get_if<lubancode::api::TextDelta>(&event)) {
            collected_text += delta->text;
        }
    });

    REQUIRE(result.has_value());
    CHECK(collected_text == "hi");
}

// ---------------------------------------------------------------------------
// 硬墙钟(cpr 并发挂死单):裸 socket 收下连接后一个字节都不回——连接阶段
// 完成了、响应头都不给,connect 超时管不着(已连上),LowSpeed 空闲超时也
// 配得很大,唯一能落锤的就是 request_hard_timeout_secs 这面墙。这正是真机
// 现场"子代理长轮询请求进 cpr::Post 再不返"的形状。
// ---------------------------------------------------------------------------

TEST_CASE("anthropic: 收下连接后彻底装死,硬墙钟在 2s 内落锤,分型文案点明硬超时") {
    lubancode::cli::SetLanguage("zh-CN");
    constexpr int kHardTimeoutSecs = 2;

    // 装死姿势:连请求都懒得读,收下连接就睡——一个字节不发、不关。
    const int port = StartFakeServer([](socket_t) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    // idle 给 25s(远大于硬墙钟):证明落锤的不是 LowSpeed 那道闸。
    lubancode::api::anthropic::AnthropicBackend backend(
        "http://127.0.0.1:" + std::to_string(port), "test-token",
        /*connect_timeout_ms=*/3000,
        /*stream_idle_timeout_secs=*/25,
        /*native_web_search=*/false, /*extra_body=*/{},
        /*extra_headers=*/{},
        /*request_hard_timeout_secs=*/kHardTimeoutSecs);

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Network);
    CHECK(result.error().message.find(
              lubancode::cli::trf("error.network.hard_timeout", kHardTimeoutSecs)) == 0);
    // 真的是硬墙钟提前掐断的,不是干等了 25s 的空闲超时,更不是 30s 的装死。
    CHECK(elapsed < std::chrono::seconds(kHardTimeoutSecs + 8));
}

TEST_CASE("responses: 收下连接后彻底装死,硬墙钟落锤(与 anthropic 同一副机理)") {
    lubancode::cli::SetLanguage("zh-CN");
    constexpr int kHardTimeoutSecs = 2;

    const int port = StartFakeServer([](socket_t) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    lubancode::api::responses::ResponsesBackend backend(
        "http://127.0.0.1:" + std::to_string(port), "test-token",
        /*connect_timeout_ms=*/3000,
        /*stream_idle_timeout_secs=*/25,
        /*native_web_search=*/false, /*extra_body=*/{},
        /*extra_headers=*/{},
        /*request_hard_timeout_secs=*/kHardTimeoutSecs);

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Network);
    CHECK(result.error().message.find(
              lubancode::cli::trf("error.network.hard_timeout", kHardTimeoutSecs)) == 0);
    CHECK(elapsed < std::chrono::seconds(kHardTimeoutSecs + 8));
}

TEST_CASE("chat: 收下连接后彻底装死,硬墙钟落锤(chat wire 同一副机理)") {
    lubancode::cli::SetLanguage("zh-CN");
    constexpr int kHardTimeoutSecs = 2;

    const int port = StartFakeServer([](socket_t) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    lubancode::api::chat::ChatCompletionsBackend backend(
        "http://127.0.0.1:" + std::to_string(port), "test-token",
        /*connect_timeout_ms=*/3000,
        /*stream_idle_timeout_secs=*/25,
        /*extra_body=*/{}, /*extra_headers=*/{},
        /*options=*/{},
        /*request_hard_timeout_secs=*/kHardTimeoutSecs);

    const auto start = std::chrono::steady_clock::now();
    const auto result =
        backend.send_stream(MakeMinimalRequest(), [](const lubancode::api::StreamEvent&) {});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Network);
    CHECK(result.error().message.find(
              lubancode::cli::trf("error.network.hard_timeout", kHardTimeoutSecs)) == 0);
    CHECK(elapsed < std::chrono::seconds(kHardTimeoutSecs + 8));
}

TEST_CASE("硬墙钟关掉(0 = 不设),行为交还 idle/connect 两道闸,不误伤") {
    // 这条不测"挂多久"(那要真等 25s),只证 0 值本身被尊重:配置解析层
    // 收非负整数、缺省留 nullopt——构造侧的"0 = 不设"语义在这里钉一下,
    // 省得将来把 0 当成正数误用。合并/默认值的账在 test_config.cpp 里另钉。
    const auto parsed =
        lubancode::config::ParseFileConfigJson(R"({"request_hard_timeout_secs": 0})", "x.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->request_hard_timeout_secs.has_value());
    CHECK(*parsed->request_hard_timeout_secs == 0);

    const auto missing = lubancode::config::ParseFileConfigJson(R"({})", "x.json");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->request_hard_timeout_secs.has_value());
}

// ---------------------------------------------------------------------------
// FD-09(错误响应体接收帽):非 2xx 分支原先 `error_body.append(data)` 无
// 上限——故障/恶意服务器回一份无 Content-Length 的连接式大错误体时,内存
// 会一直涨到流结束/硬超时为止。时间闸(硬墙钟/空闲超时)不等于字节闸。
// 这组用例直调 PostSseStream(绕开 backend,好给 HttpStreamCall.
// max_error_body_bytes 注小帽钉边界值),fake 服务器一律不带
// Content-Length——就是"连接式错误体"这个病灶形状本身。
// ---------------------------------------------------------------------------

TEST_CASE("PostSseStream: 连接式错误体超过接收帽,就地掐流并保留有界摘要(分型不归 Network)") {
    constexpr std::size_t kCap = 16 * 1024;
    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\n");
        const std::string block(4 * 1024, 'x');
        for (int i = 0; i < 1024; ++i) {  // 最多 4 MiB,远超 16 KiB 的帽
            if (!SendIfAlive(client, block.data(), static_cast<int>(block.size()))) {
                break;  // 客户端已掐流:连接这头多半也断了,别死等
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));  // 不关连接:装作"还会一直发"
    });

    lubancode::api::HttpStreamCall call;
    call.url = "http://127.0.0.1:" + std::to_string(port);
    call.body = "{}";
    call.connect_timeout_ms = 3000;
    call.stream_idle_timeout_secs = 25;
    call.max_error_body_bytes = kCap;

    const auto result =
        lubancode::api::PostSseStream(call, [](std::string_view) { return true; });

    REQUIRE_FALSE(result.has_value());
    // 分型必须是 HttpStatus 而不是 Network:重试环(IsRetryableError)对
    // Network 一律重试——超帽的大错误体不能被当成网络抖动再收六遍;
    // 429/5xx 的既有重试语义在 HttpStatus 分型下原样保留。
    CHECK(result.error().kind == lubancode::api::ErrorKind::HttpStatus);
    CHECK(result.error().http_status == 500);
    // 稳定截断说明在头部:一眼能看出"这不是完整错误体",截断原因是确定的。
    // 拼头部是给下游留路——SummarizeErrorBodyForUser 和日志首行只看开头。
    const std::string notice =
        "[错误响应体超过 " + std::to_string(kCap) + " 字节上限,接收已中止,以下为截断摘要]";
    CHECK(result.error().message.rfind(notice, 0) == 0);
    // 内存保留量有界:帽内前缀 + 一行说明,总量不越过帽加说明文字。
    CHECK(result.error().message.size() < kCap + 200);
}

TEST_CASE("PostSseStream: 错误体正好到帽不掐,完整保留且不带截断说明(边界)") {
    constexpr std::size_t kCap = 4096;
    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\n");
        SendAll(client, std::string(kCap, 'a'));  // 恰好 kCap 字节:到帽不超
        ShutdownSend(client);                     // 干净收尾:流正常结束,不是掐的
    });

    lubancode::api::HttpStreamCall call;
    call.url = "http://127.0.0.1:" + std::to_string(port);
    call.body = "{}";
    call.connect_timeout_ms = 3000;
    call.stream_idle_timeout_secs = 25;
    call.max_error_body_bytes = kCap;

    const auto result =
        lubancode::api::PostSseStream(call, [](std::string_view) { return true; });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::HttpStatus);
    CHECK(result.error().http_status == 400);
    // 原样完整:不多一个字节的截断说明,也不少一个 'a'。
    CHECK(result.error().message == std::string(kCap, 'a'));
}

TEST_CASE("PostSseStream: 错误体超帽一字节也掐,超限段整体不进摘要(边界)") {
    constexpr std::size_t kCap = 100;
    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n\r\n");
        SendAll(client, std::string(kCap, 'a'));
        SendAll(client, std::string(50, 'b'));  // 帽后 50 字节:这一段放进去就超
        ShutdownSend(client);
    });

    lubancode::api::HttpStreamCall call;
    call.url = "http://127.0.0.1:" + std::to_string(port);
    call.body = "{}";
    call.connect_timeout_ms = 3000;
    call.stream_idle_timeout_secs = 25;
    call.max_error_body_bytes = kCap;

    const auto result =
        lubancode::api::PostSseStream(call, [](std::string_view) { return true; });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::HttpStatus);
    CHECK(result.error().http_status == 403);
    CHECK(result.error().message.find("[错误响应体超过 " + std::to_string(kCap) + " 字节上限") !=
          std::string::npos);
    // 超限的那一段整体不进("这一段放进去就超就不放"):帽后内容绝不混进摘要。
    CHECK(result.error().message.find('b') == std::string::npos);
    // 帽内前缀保留量有界:libcurl 回调分块粒度不定,混段时前缀可能短于
    // kCap(比如 kCap 个 'a' 和 'b' 同段到达就整段丢弃),上界是稳的。
    CHECK(std::count(result.error().message.begin(), result.error().message.end(), 'a') <=
          static_cast<std::ptrdiff_t>(kCap));
}

TEST_CASE("PostSseStream: 零长度错误体不踩帽逻辑,兜底文案照旧(回归)") {
    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\n");
        ShutdownSend(client);  // 一个字节的体都不给,干净关
    });

    lubancode::api::HttpStreamCall call;
    call.url = "http://127.0.0.1:" + std::to_string(port);
    call.body = "{}";
    call.connect_timeout_ms = 3000;
    call.stream_idle_timeout_secs = 25;
    call.max_error_body_bytes = 8;

    const auto result =
        lubancode::api::PostSseStream(call, [](std::string_view) { return true; });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::HttpStatus);
    CHECK(result.error().http_status == 503);
    CHECK(result.error().message == "服务端返回了非 200 状态码,但响应体是空的");
}

TEST_CASE("PostSseStream: 取消与超帽同拍,取消优先——收场报 Cancelled 不报错误体帽") {
    constexpr std::size_t kCap = 8;
    const int port = StartFakeServer([](socket_t client) {
        DrainRequest(client);
        SendAll(client, "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/plain\r\n\r\n");
        SendAll(client, std::string(kCap, 'a'));  // 先发到帽:这一段在帽内,不触发
        // 等 1s 再发超帽块:留足窗口让主线程的 cancel 先置位——同一次
        // WriteCallback 里"取消检查在前、帽检查在后"这个顺序就是被钉的
        // 合同(收场分型:取消 > 帧溢出 > 错误体帽 > 网络错 > 状态)。
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::string block(2048, 'c');
        for (int i = 0; i < 50; ++i) {
            if (!SendIfAlive(client, block.data(), static_cast<int>(block.size()))) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    lubancode::api::HttpStreamCall call;
    call.url = "http://127.0.0.1:" + std::to_string(port);
    call.body = "{}";
    call.connect_timeout_ms = 3000;
    call.stream_idle_timeout_secs = 25;
    call.max_error_body_bytes = kCap;

    std::atomic<bool> cancel{false};
    std::thread cancel_thread([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true);
    });
    const auto result =
        lubancode::api::PostSseStream(call, [](std::string_view) { return true; }, &cancel);
    cancel_thread.join();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == lubancode::api::ErrorKind::Cancelled);
}

TEST_CASE("PostSseStream: 错误体帽不碰 2xx 成功流——总量越过小帽的长 SSE 照常收完") {
    constexpr std::size_t kCap = 1024;  // 故意小:成功流必须越过它
    constexpr int kFrames = 24;
    const std::string frame = "data: " + std::string(480, 'x') + "\n\n";  // 488 字节/帧
    const int port = StartFakeServer([frame](socket_t client) {
        DrainRequest(client);
        SendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n");
        for (int i = 0; i < kFrames; ++i) {  // 总量 ~11.7 KiB,远超 1 KiB 的错误帽
            SendAll(client, frame);
        }
        ShutdownSend(client);
    });

    lubancode::api::HttpStreamCall call;
    call.url = "http://127.0.0.1:" + std::to_string(port);
    call.body = "{}";
    call.connect_timeout_ms = 3000;
    call.stream_idle_timeout_secs = 25;
    call.max_error_body_bytes = kCap;

    std::size_t sink_bytes = 0;
    const auto result = lubancode::api::PostSseStream(
        call, [&](std::string_view chunk) {
            sink_bytes += chunk.size();
            return true;
        });

    // 帽只管错误分支:成功流不存在"总量限制",一个字节都不少地过 sink
    // (单帧粒度的容量仍归 SseFramer 的 8MiB 单帧帽管,不在这条路上)。
    REQUIRE(result.has_value());
    CHECK(sink_bytes == static_cast<std::size_t>(kFrames) * frame.size());
}
