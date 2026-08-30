// FakeHttpServer 的实现:可移植 socket(Winsock / POSIX 同一副写法)。
// 设计与线程收尾见 fake_http_server.hpp 文件头。
#include "fake_http_server.hpp"

#include <algorithm>
#include <thread>

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

namespace lubancode::test_support {
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

std::string ReasonPhrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        default:
            return "Status";
    }
}

bool SendAll(socket_t s, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int chunk = ::send(s, data + sent, static_cast<int>(size - sent), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool SendAll(socket_t s, const std::string& data) { return SendAll(s, data.data(), data.size()); }

// 收到 "\r\n\r\n" 为止;返回读到的全部字节(含可能多读进来的体前缀)。
bool ReadUntilHeaderEnd(socket_t s, std::string& buffer) {
    char chunk[4096];
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        const int got = ::recv(s, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(got));
    }
    return true;
}

bool ReadMore(socket_t s, std::string& buffer, std::size_t want) {
    char chunk[4096];
    while (buffer.size() < want) {
        const int got = ::recv(s, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(got));
    }
    return true;
}

}  // namespace

struct FakeHttpServer::State {
    socket_t listener = kInvalidSocket;
    std::atomic<bool> stopping{false};
    std::atomic<int> connections{0};
    mutable std::mutex mutex;
    std::vector<FakeHttpResponse> scripts;
    std::vector<FakeHttpRequest> log;

    FakeHttpResponse NextScript() {
        const std::lock_guard<std::mutex> lock(mutex);
        if (scripts.empty()) {
            FakeHttpResponse fallback;
            fallback.status = 500;
            fallback.body = "fake server: script exhausted";
            return fallback;
        }
        FakeHttpResponse next = std::move(scripts.front());
        scripts.erase(scripts.begin());
        return next;
    }

    void Record(FakeHttpRequest request) {
        const std::lock_guard<std::mutex> lock(mutex);
        log.push_back(std::move(request));
    }

    // 一条连接的完整服务:收请求 -> 回脚本。
    void ServeConnection(socket_t client) {
        // 10s 收超时:客户端半途跑路(取消/超帽掐流)不让线程挂到进程退出。
#ifdef _WIN32
        DWORD timeout_ms = 10'000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        struct timeval tv {};
        tv.tv_sec = 10;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        std::string buffer;
        if (!ReadUntilHeaderEnd(client, buffer)) {
            CloseSocket(client);
            return;
        }
        const std::size_t header_end = buffer.find("\r\n\r\n") + 4;

        // 解请求行 + 头。
        FakeHttpRequest request;
        const std::size_t line_end = buffer.find("\r\n");
        const std::string request_line = buffer.substr(0, line_end);
        const std::size_t method_end = request_line.find(' ');
        const std::size_t target_end = request_line.find(' ', method_end + 1);
        if (method_end != std::string::npos && target_end != std::string::npos) {
            request.method = request_line.substr(0, method_end);
            request.target = request_line.substr(method_end + 1, target_end - method_end - 1);
        }
        std::size_t cursor = line_end + 2;
        std::size_t content_length = 0;
        bool expect_continue = false;
        while (cursor < header_end - 2) {
            const std::size_t next = buffer.find("\r\n", cursor);
            const std::size_t end = next == std::string::npos ? header_end - 2 : next;
            const std::string line = buffer.substr(cursor, end - cursor);
            cursor = end + 2;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                continue;
            }
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            for (auto& c : name) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            if (name == "content-length") {
                try {
                    content_length = static_cast<std::size_t>(std::stoull(value));
                } catch (const std::exception&) {
                    content_length = 0;
                }
            } else if (name == "expect" && value.find("100-continue") != std::string::npos) {
                expect_continue = true;
            }
            request.headers.emplace_back(std::move(name), std::move(value));
        }

        // 大体 POST:curl 会先发 Expect: 100-continue 等确认,先回 100 再收体。
        if (expect_continue) {
            SendAll(client, "HTTP/1.1 100 Continue\r\n\r\n");
        }
        if (content_length > 0) {
            if (!ReadMore(client, buffer, header_end + content_length)) {
                CloseSocket(client);
                return;
            }
            request.body = buffer.substr(header_end, content_length);
        }
        Record(std::move(request));

        // 按脚本回响应。
        const FakeHttpResponse script = NextScript();
        if (script.delay_before_response.count() > 0) {
            std::this_thread::sleep_for(script.delay_before_response);
        }
        // (std::min):防 windows.h 的 min 宏。
        const std::size_t send_bytes = (std::min)(script.stall_after_body_bytes, script.body.size());
        std::string head = "HTTP/1.1 " + std::to_string(script.status) + " " + ReasonPhrase(script.status) + "\r\n";
        bool has_content_length = false;
        for (const auto& [name, value] : script.headers) {
            if (name == "Content-Length" || name == "content-length") {
                has_content_length = true;
            }
            head += name + ": " + value + "\r\n";
        }
        if (!has_content_length) {
            head += "Content-Length: " + std::to_string(script.body.size()) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";
        if (!SendAll(client, head)) {
            CloseSocket(client);
            return;
        }
        if (send_bytes > 0 && !SendAll(client, script.body.data(), send_bytes)) {
            CloseSocket(client);
            return;
        }
        if (send_bytes < script.body.size()) {
            // 发半截后挂死:客户端的取消/墙钟来收场。
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        CloseSocket(client);
    }
};

FakeHttpServer::FakeHttpServer() : state_(std::make_shared<State>()) {
    EnsureSocketsReady();

    const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        return;
    }
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocket(listener);
        return;
    }
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0 ||
        ::listen(listener, 16) != 0) {
        CloseSocket(listener);
        return;
    }
    port_ = ntohs(bound.sin_port);
    state_->listener = listener;

    std::thread([state = state_]() {
        while (!state->stopping.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            const socket_t client =
                ::accept(state->listener, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client == kInvalidSocket) {
                return;  // 监听 socket 关了,收工
            }
            state->connections.fetch_add(1);
            std::thread([state, client]() { state->ServeConnection(client); }).detach();
        }
    }).detach();
}

FakeHttpServer::~FakeHttpServer() {
    state_->stopping.store(true);
    if (state_->listener != kInvalidSocket) {
        CloseSocket(state_->listener);
        state_->listener = kInvalidSocket;
    }
}

void FakeHttpServer::Enqueue(FakeHttpResponse response) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->scripts.push_back(std::move(response));
}

std::vector<FakeHttpRequest> FakeHttpServer::requests() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->log;
}

int FakeHttpServer::connection_count() const { return state_->connections.load(); }

}  // namespace lubancode::test_support
