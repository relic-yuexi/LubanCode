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

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
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
