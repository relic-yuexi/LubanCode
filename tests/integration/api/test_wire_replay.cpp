// L4 回环 HTTP 集成(模型协议兼容实录矩阵单 P2):本地 loopback 假服务把
// 整条路真跑穿——
//
//   Provider profile -> Request shaping -> HTTP path/headers/body
//     -> 任意大小的网络 chunk -> SSE framer -> wire parser
//     -> StreamEvent / assembler -> tool call + tool result
//     -> 第二轮 reasoning/signature replay -> usage 与 stop reason
//
// 每家 wire 一册 CASE:第一轮按 fixture 回"思考 + 工具调用",核请求侧的
// 落线形状(方言说了什么,HTTP 里就得是什么);assembler 攒出 assistant
// 消息后把工具结果拼回历史发第二轮,核"工具后第二轮"的回传形状——
// Chat 的 reasoning_content 原字节、Anthropic 的 thinking 块带 signature、
// Responses 的 function_call/function_call_output、Gemini 的
// functionCall/functionResponse。第二轮流按随机小块(seed 固定)切着发,
// 跨 chunk 解析不坏;另有一册半路断流的负路径(2xx 不是绿,断流必须报错)。
//
// 不靠公网、不花 token,Windows/Linux 都编(socket 层同 network_timeout
// 单的双分支),进常规 CTest。

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { ::closesocket(s); }
void EnsureSocketsReady() {
    struct WinsockInit {
        WinsockInit() {
            WSADATA wsa;
            ::WSAStartup(MAKEWORD(2, 2), &wsa);
        }
    };
    static WinsockInit init;
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
void CloseSocket(socket_t s) { ::close(s); }
void EnsureSocketsReady() {}
#endif

#include "api/anthropic/client.hpp"
#include "api/assembler.hpp"
#include "api/chat/client.hpp"
#include "api/chat/request.hpp"
#include "api/gemini/client.hpp"
#include "api/responses/client.hpp"
#include "api/types.hpp"
#include "config/provider_catalog.hpp"
#include "embedded_provider_catalog.hpp"

#include "api_fixture.hpp"

using namespace lubancode;

namespace {

// 绑 127.0.0.1:0,系统分配空闲端口。
socket_t BindLoopbackListener(int* out_port) {
    EnsureSocketsReady();
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd != kInvalidSocket);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0);
    *out_port = ntohs(bound.sin_port);
    return fd;
}

// 一只收到的 HTTP 请求(已按 Content-Length 收满)。
struct HttpRequest {
    std::string request_line;  // "POST /v1/messages HTTP/1.1"
    std::string path;          // 不含 query
    std::string query;         // "?alt=sse"(可空)
    std::map<std::string, std::string> headers;  // 键转小写
    std::string body;
};

std::string LowerKey(std::string key) {
    for (char& c : key) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return key;
}

// 请求解析:跑在服务端 detach 线程上,不能用 doctest 宏(非测试线程),
// 坏请求返回 nullopt,调用方关连接走人。
std::optional<HttpRequest> TryReadHttpRequest(socket_t s) {
    std::string raw;
    char buf[4096];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int got = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (got <= 0) return std::nullopt;
        raw.append(buf, static_cast<std::size_t>(got));
        header_end = raw.find("\r\n\r\n");
    }

    HttpRequest out;
    const std::string head = raw.substr(0, header_end);
    std::size_t line_end = head.find("\r\n");
    out.request_line = head.substr(0, line_end);
    const std::size_t first_space = out.request_line.find(' ');
    const std::size_t second_space = out.request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        return std::nullopt;
    }
    const std::string target =
        out.request_line.substr(first_space + 1, second_space - first_space - 1);
    const std::size_t query_pos = target.find('?');
    out.path = query_pos == std::string::npos ? target : target.substr(0, query_pos);
    out.query = query_pos == std::string::npos ? std::string() : target.substr(query_pos);

    std::size_t cursor = line_end + 2;
    while (cursor < head.size()) {
        const std::size_t eol = head.find("\r\n", cursor);
        const std::string line =
            head.substr(cursor, eol == std::string::npos ? std::string::npos : eol - cursor);
        // 最后一行头紧贴 \r\n\r\n 空行,head 剥掉分隔符后它没有行尾——
        // 一样要解析(否则恰好把 Content-Length 漏掉)。
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            out.headers[LowerKey(line.substr(0, colon))] = line.substr(colon + 2);
        }
        if (eol == std::string::npos) break;
        cursor = eol + 2;
    }

    // 请求体两种来路:Content-Length 定长(libcurl 小 body)或
    // Transfer-Encoding: chunked(流式 body)。都要解出原文,否则请求侧
    // 断言白搭。
    const bool chunked =
        out.headers.count("transfer-encoding") != 0 &&
        out.headers.at("transfer-encoding").find("chunked") != std::string::npos;
    std::size_t content_length = 0;
    if (const auto it = out.headers.find("content-length"); it != out.headers.end()) {
        content_length = static_cast<std::size_t>(std::stoull(it->second));
    }
    std::string buffer = raw.substr(header_end + 4);
    const auto ensure = [&](std::size_t need) {
        while (buffer.size() < need) {
            const int got = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
            if (got <= 0) return false;
            buffer.append(buf, static_cast<std::size_t>(got));
        }
        return true;
    };
    if (chunked) {
        std::string body;
        std::size_t pos = 0;
        for (;;) {
            std::size_t eol = buffer.find("\r\n", pos);
            while (eol == std::string::npos) {
                if (!ensure(buffer.size() + 1)) return std::nullopt;
                eol = buffer.find("\r\n", pos);
            }
            const unsigned long size =
                std::strtoul(buffer.substr(pos, eol - pos).c_str(), nullptr, 16);
            pos = eol + 2;
            if (size == 0) break;  // 终止 chunk,后面是 trailer(不等了)
            if (!ensure(pos + size + 2)) return std::nullopt;
            body.append(buffer, pos, size);
            pos += size + 2;  // 数据 + CRLF
        }
        out.body = std::move(body);
    } else {
        if (!ensure(content_length)) return std::nullopt;
        out.body = buffer.substr(0, content_length);
    }
    return out;
}

void SendAll(socket_t s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = static_cast<int>(
            ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0));
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

// SSE 字节按随机小块切着发(与 L3 同一付刀口,seed 固定可复现)。
void SendChunked(socket_t s, const std::string& bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> size(3, 11);
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(size(rng)),
                                                       bytes.size() - i);
        SendAll(s, bytes.substr(i, take));
        i += take;
    }
}

// 多轮回放服务器:第 i 个连接读满请求 -> 回第 i 份 SSE 流(末份之后再来
// 的连接直接关,防 detach 线程挂着)。切法二选一:整块 / 随机小块。
// json_body = true 时回 application/json 裸体(兼容端把 stream 请求当
// 非流式答的形状,vLLM 勘察单 P2 的非流式回退案用)。
struct ReplayPlan {
    std::vector<std::string> sse_rounds;  // 每连接一份完整 SSE 字节(或 JSON 体)
    std::uint32_t chunk_seed = 20260827;  // 0 = 整块发
    bool json_body = false;
};

int StartReplayServer(const ReplayPlan& plan, std::vector<HttpRequest>* received) {
    int port = 0;
    const socket_t listener = BindLoopbackListener(&port);
    REQUIRE(::listen(listener, 4) == 0);
    std::thread([listener, plan, received]() {
        for (const std::string& sse : plan.sse_rounds) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            const socket_t client =
                ::accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client == kInvalidSocket) break;
            auto request = TryReadHttpRequest(client);
            if (!request.has_value()) {
                CloseSocket(client);
                continue;
            }
            received->push_back(std::move(*request));
            SendAll(client,
                    plan.json_body
                        ? "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                        : "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Connection: close\r\n"
                          "\r\n");
            if (plan.chunk_seed == 0) SendAll(client, sse);
            else SendChunked(client, sse, plan.chunk_seed);
            CloseSocket(client);
        }
        CloseSocket(listener);
    }).detach();
    return port;
}

// 半路断流服务器:回响应头 + 半截 SSE 就硬关连接。
int StartCutoffServer(std::vector<HttpRequest>* received) {
    int port = 0;
    const socket_t listener = BindLoopbackListener(&port);
    REQUIRE(::listen(listener, 4) == 0);
    std::thread([listener, received]() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const socket_t client =
            ::accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client != kInvalidSocket) {
            auto request = TryReadHttpRequest(client);
            if (request.has_value()) received->push_back(std::move(*request));
            SendAll(client,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "\r\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"x\"}}\n"
                    "\n");
            // 不等客户端,直接硬断(RST 或 FIN 都行,反正流没走完)。
            CloseSocket(client);
        }
        CloseSocket(listener);
    }).detach();
    return port;
}

const config::ProviderCatalog& EmbeddedCatalog() {
    static const auto parsed = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(parsed.has_value());
    static const config::ProviderCatalog catalog = std::move(*parsed);
    return catalog;
}

api::ReasoningConfig CatalogReasoning(const char* provider_id, const char* model_id) {
    const auto* provider = EmbeddedCatalog().FindProvider(provider_id);
    REQUIRE(provider != nullptr);
    const auto* model = provider->FindModel(model_id);
    REQUIRE(model != nullptr);
    return model->reasoning;
}

// 一轮 send_stream:事件喂 assembler,返回攒出的 assistant 消息。
api::Message RunRound(api::Backend& backend, const api::Request& request,
                      std::string* out_stop_reason = nullptr) {
    api::MessageAssembler assembler;
    std::optional<api::StreamError> stream_error;
    const auto result = backend.send_stream(request, [&](const api::StreamEvent& event) {
        if (const auto* error = std::get_if<api::StreamError>(&event)) {
            stream_error = *error;
            return;
        }
        if (const auto* done = std::get_if<api::MessageDone>(&event)) {
            if (out_stop_reason != nullptr) *out_stop_reason = done->stop_reason;
        }
        assembler.Feed(event);
    });
    REQUIRE(result.has_value());
    REQUIRE_FALSE(stream_error.has_value());
    return assembler.BuildMessage();
}

std::string FixtureStream(const char* wire_dir, const char* id) {
    const auto fixture = lubancode_test::LoadApiFixture(wire_dir, id);
    REQUIRE(fixture.has_value());
    return fixture->stream;
}

api::Message UserText(const std::string& text) {
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{text});
    return user;
}

}  // namespace

// ---------------------------------------------------------------------------
// anthropic-messages:思考 -> 工具 -> 签名回传 -> 终答
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 anthropic: 思考+工具往返,第二轮 thinking 块带 signature,终答随机切片不坏") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("anthropic_messages", "manual_thinking_then_tool_use"),
        FixtureStream("anthropic_messages", "manual_thinking_enabled_stream"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::anthropic::AnthropicBackend backend(base, "test-token", 3000, 30);
    api::Request first;
    first.model = "qwen3.7-plus";
    first.reasoning = CatalogReasoning("dashscope-anthropic", "qwen3.7-plus");
    first.reasoning_effort = "high";
    first.max_tokens = 2048;
    first.messages.push_back(UserText("杭州天气怎么样"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    // 请求侧:方言说了 thinking.type + budget_tokens,HTTP 里就得是。
    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/v1/messages");
    CHECK(received[0].headers.at("authorization") == "Bearer test-token");
    CHECK(received[0].headers.at("content-type") == "application/json");
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("model") == "qwen3.7-plus");
    REQUIRE(body1.contains("thinking"));
    CHECK(body1["thinking"].at("type") == "enabled");
    CHECK(body1["thinking"].contains("budget_tokens"));

    // 攒出的 assistant:thinking 块 + tool_use 块。
    REQUIRE(assistant.content.size() == 2);
    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content[0]);
    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content[1]);
    REQUIRE(thinking != nullptr);
    REQUIRE(call != nullptr);
    CHECK_FALSE(thinking->text.empty());
    CHECK(call->name == "get_weather");

    // 第二轮:thinking 原字节 + signature 回传,tool_result 进 user。
    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.max_tokens = 2048;
    second.messages.push_back(UserText("杭州天气怎么样"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(
        api::ToolResultBlock{call->id, R"({"temp":"31C"})", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    const api::Message final_message = RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    for (const auto& [name, value] : received[1].headers) {
    }
    const auto body2 = nlohmann::json::parse(received[1].body);
    // 回传的 assistant content:thinking 块必须带 signature 键(空串也是键),
    // tool_use id 原样。
    const auto& replayed = body2.at("messages")[1].at("content");
    REQUIRE(replayed.size() == 2);
    CHECK(replayed[0].at("type") == "thinking");
    CHECK(replayed[0].contains("signature"));
    CHECK(replayed[0].at("thinking") == thinking->text);  // 原字节
    CHECK(replayed[1].at("type") == "tool_use");
    CHECK(replayed[1].at("id") == call->id);
    CHECK(replayed[1].at("name") == "get_weather");
    // tool_result 在下一条 user 消息里。
    const auto& tool_result = body2.at("messages")[2].at("content");
    CHECK(tool_result[0].at("type") == "tool_result");
    CHECK(tool_result[0].at("tool_use_id") == call->id);

    // 终答:思考块 + 正文块都在(随机小块切着发也没丢)。
    REQUIRE(final_message.content.size() == 2);
    CHECK(std::holds_alternative<api::ThinkingBlock>(final_message.content[0]));
    CHECK_FALSE(std::get<api::TextBlock>(final_message.content[1]).text.empty());
}

// ---------------------------------------------------------------------------
// openai-chat-completions:思考 -> 工具 -> reasoning_content 回传 -> 终答
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 chat: DeepSeek tool_episode 第二轮 reasoning_content 原字节回传") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_chat", "deepseek_tool_episode_stream"),
        FixtureStream("openai_chat", "manual_enable_thinking_reasoning_content"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::chat::ChatRequestOptions options;
    options.reasoning_replay = api::chat::ReasoningReplayPolicy::ToolEpisode;
    options.reasoning_replay_field = "reasoning_content";
    api::chat::ChatCompletionsBackend backend(base, "test-token", 3000, 30,
                                              nlohmann::json::object(), {}, options);

    api::Request first;
    first.model = "deepseek-v4-pro";
    first.reasoning = CatalogReasoning("deepseek", "deepseek-v4-pro");
    first.reasoning_effort = "high";
    first.messages.push_back(UserText("读一下探针文件"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/chat/completions");
    CHECK(received[0].headers.at("authorization") == "Bearer test-token");
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("model") == "deepseek-v4-pro");
    // deepseek 家方言:thinking_type 开关 + reasoning_effort 档(GLM/DeepSeek 档)。
    CHECK(body1.at("thinking").at("type") == "enabled");
    CHECK(body1.at("reasoning_effort") == "high");

    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content.back());
    REQUIRE(call != nullptr);
    CHECK(call->name == "probe_file");

    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.messages.push_back(UserText("读一下探针文件"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{call->id, "2400 字探针正文", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    const api::Message final_message = RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    // 工具段的 assistant:reasoning_content 原字节回传(DeepSeek 规矩),
    // tool_calls 连 id 带参原样。
    const auto& replayed = body2.at("messages")[1];
    CHECK(replayed.at("reasoning_content") == "先想路径");
    REQUIRE(replayed.contains("tool_calls"));
    CHECK(replayed.at("tool_calls")[0].at("id") == call->id);
    CHECK(replayed.at("tool_calls")[0].at("function").at("name") == "probe_file");
    // 工具结果走 role=tool + tool_call_id。
    const auto& tool_message = body2.at("messages")[2];
    CHECK(tool_message.at("role") == "tool");
    CHECK(tool_message.at("tool_call_id") == call->id);

    CHECK_FALSE(std::get<api::TextBlock>(final_message.content.back()).text.empty());
}

// ---------------------------------------------------------------------------
// Kimi 保留式思考单 P0:Moonshot 四枚模型方言驱动(backend options 全默认,
// 不靠 legacy reasoning_replay——目录方言说了算)。
//   K3   always:跨轮纯对话,第二轮请求原样带回 reasoning,不发 thinking
//   K2.6 tool_episode:本 Turn 工具循环回传,不发 reasoning_effort
//   K2.5 never:思考照收,历史不回传,thinking.keep 一概不发
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 chat: Kimi K3 跨轮 always——第二轮请求带回第一轮 reasoning_content,不发 thinking") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_chat", "kimi_k3_reasoning_text_stream"),
        FixtureStream("openai_chat", "kimi_k3_reasoning_text_stream"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    // options 一件不配:replay 全由 request.reasoning.dialect 裁决。
    api::chat::ChatCompletionsBackend backend(base, "test-token", 3000, 30,
                                              nlohmann::json::object(), {}, api::chat::ChatRequestOptions{});

    api::Request first;
    first.model = "kimi-k3";
    first.reasoning = CatalogReasoning("moonshot", "kimi-k3");
    first.reasoning_effort = "high";
    first.messages.push_back(UserText("你好"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "end_turn");

    REQUIRE(received.size() == 1);
    const auto body1 = nlohmann::json::parse(received[0].body);
    // K3 契约:顶层 reasoning_effort(low/high/max 档),thinking 整个不发。
    CHECK(body1.at("reasoning_effort") == "high");
    CHECK_FALSE(body1.contains("thinking"));
    CHECK_FALSE(body1.contains("thinking_budget"));

    // 攒出的 assistant:thinking 块 + 正文块。
    REQUIRE(assistant.content.size() == 2);
    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content[0]);
    const auto* text = std::get_if<api::TextBlock>(&assistant.content[1]);
    REQUIRE(thinking != nullptr);
    REQUIRE(text != nullptr);
    CHECK(thinking->text == "先想一句再答");

    // 第二轮:U1 + assistant(完整保留) + U2。Preserved Thinking 是客户端
    // 责任——assistant 在,reasoning_content 就得在。
    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.messages.push_back(UserText("你好"));
    second.messages.push_back(assistant);
    second.messages.push_back(UserText("再问一句"));

    std::string final_stop;
    RunRound(backend, second, &final_stop);
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    const auto& replayed = body2.at("messages")[1];
    CHECK(replayed.at("role") == "assistant");
    CHECK(replayed.at("reasoning_content") == "先想一句再答");  // 原字节
    CHECK(replayed.at("content") == "答:你好");
    CHECK_FALSE(body2.contains("thinking"));
}

TEST_CASE("wire 回环 chat: Kimi K2.6 工具循环——本 Turn reasoning 回传,不发 reasoning_effort") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_chat", "kimi_k26_tool_episode_stream"),
        FixtureStream("openai_chat", "manual_enable_thinking_reasoning_content"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::chat::ChatCompletionsBackend backend(base, "test-token", 3000, 30,
                                              nlohmann::json::object(), {}, api::chat::ChatRequestOptions{});

    api::Request first;
    first.model = "kimi-k2.6";
    first.reasoning = CatalogReasoning("moonshot", "kimi-k2.6");
    first.reasoning_effort = "high";  // 档位只当开关用:官方不认 effort 参数
    first.tools.push_back({"probe_file", "探针", nlohmann::json{{"type", "object"}}});
    first.messages.push_back(UserText("读一下探针文件"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    const auto body1 = nlohmann::json::parse(received[0].body);
    // K2.6 契约:thinking.type 可开关;reasoning_effort/budget 官方不认。
    CHECK(body1.at("thinking").at("type") == "enabled");
    CHECK_FALSE(body1.contains("reasoning_effort"));
    CHECK_FALSE(body1.contains("thinking_budget"));

    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content.front());
    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content.back());
    REQUIRE(thinking != nullptr);
    REQUIRE(call != nullptr);
    CHECK(thinking->text == "先想路径");
    CHECK(call->name == "probe_file");

    // 第二轮:工具结果拼回历史。同一工具循环的 reasoning 须原字节随行。
    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.tools = first.tools;
    second.messages.push_back(UserText("读一下探针文件"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{call->id, "2400 字探针正文", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    RunRound(backend, second, &final_stop);
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    const auto& replayed = body2.at("messages")[1];
    CHECK(replayed.at("reasoning_content") == "先想路径");
    REQUIRE(replayed.contains("tool_calls"));
    CHECK(replayed.at("tool_calls")[0].at("id") == call->id);
    CHECK(replayed.at("tool_calls")[0].at("function").at("name") == "probe_file");
    const auto& tool_message = body2.at("messages")[2];
    CHECK(tool_message.at("role") == "tool");
    CHECK(tool_message.at("tool_call_id") == call->id);
    // 跨 Turn 未开 keep(P1):不宣称 Preserved Thinking,thinking 里没有 keep。
    CHECK_FALSE(body2.at("thinking").contains("keep"));
    CHECK_FALSE(body2.contains("reasoning_effort"));
}

TEST_CASE("wire 回环 chat: Kimi K2.5 不回传历史思考,也不误发 thinking.keep") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_chat", "kimi_k25_reasoning_text_stream"),
        FixtureStream("openai_chat", "kimi_k25_reasoning_text_stream"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::chat::ChatCompletionsBackend backend(base, "test-token", 3000, 30,
                                              nlohmann::json::object(), {}, api::chat::ChatRequestOptions{});

    api::Request first;
    first.model = "kimi-k2.5";
    first.reasoning = CatalogReasoning("moonshot", "kimi-k2.5");
    first.reasoning_effort = "high";
    first.messages.push_back(UserText("你好"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "end_turn");

    REQUIRE(received.size() == 1);
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("thinking").at("type") == "enabled");
    CHECK_FALSE(body1.at("thinking").contains("keep"));

    // 思考照收不丢(本地历史与 session 都在),只是不出站。
    REQUIRE(assistant.content.size() == 2);
    CHECK(std::get_if<api::ThinkingBlock>(&assistant.content[0]) != nullptr);

    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.messages.push_back(UserText("你好"));
    second.messages.push_back(assistant);
    second.messages.push_back(UserText("再问一句"));

    std::string final_stop;
    RunRound(backend, second, &final_stop);
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    // K2.5 不支持 Preserved Thinking:assistant 消息在,reasoning 不回传。
    const auto& replayed = body2.at("messages")[1];
    CHECK(replayed.at("role") == "assistant");
    CHECK_FALSE(replayed.contains("reasoning_content"));
    CHECK(replayed.at("content") == "答:你好");
    CHECK_FALSE(body2.at("thinking").contains("keep"));
}

// ---------------------------------------------------------------------------
// vLLM 本地模型四 wire 支持勘察单 P0/P1:目录 vllm 预设驱动(backend options
// 只配 stream_usage,回传策略全由方言裁决)。qwen3 模板开关唯一生效路是
// chat_template_kwargs.enable_thinking(顶层布尔被端无视);工具段 reasoning
// 回传走 vLLM 0.27 新名字 reasoning,tool_call id 是 chatcmpl-tool- 前缀。
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 chat: vLLM qwen3.8 工具循环——chat_template_kwargs 开关落线,reasoning 字段回传") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_chat", "live_vllm_qwen38_tool_episode_stream"),
        FixtureStream("openai_chat", "vllm_qwen_reasoning_delta"),
    };
    plan.chunk_seed = 20260831;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::chat::ChatRequestOptions options;
    options.stream_usage = true;  // 目录镜像进配置的路,这里手动带上
    api::chat::ChatCompletionsBackend backend(base, "test-token", 3000, 30,
                                              nlohmann::json::object(), {}, options);

    api::Request first;
    first.model = "qwen3.8-27b";
    first.reasoning = CatalogReasoning("vllm", "qwen3.8-27b");
    first.reasoning_effort = "high";  // 档位只当开关用:qwen3 不吃 effort 参数
    first.tools.push_back({"get_weather", "查天气", nlohmann::json{{"type", "object"}}});
    first.messages.push_back(UserText("北京天气怎么样"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/chat/completions");
    const auto body1 = nlohmann::json::parse(received[0].body);
    // vLLM/qwen3 契约:开关唯一生效路是嵌套键;顶层 enable_thinking、
    // thinking、reasoning_effort 都不是这家的形状,一个不发。
    CHECK(body1.at("chat_template_kwargs").at("enable_thinking") == true);
    CHECK_FALSE(body1.contains("enable_thinking"));
    CHECK_FALSE(body1.contains("thinking"));
    CHECK_FALSE(body1.contains("reasoning_effort"));
    CHECK(body1.at("stream_options").at("include_usage") == true);
    // max_tokens 未声明就不发,交服务端默认(vLLM 上限 262144,思考不撞墙)。
    CHECK_FALSE(body1.contains("max_tokens"));

    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content.front());
    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content.back());
    REQUIRE(thinking != nullptr);
    REQUIRE(call != nullptr);
    CHECK(thinking->text == "用户问北京气温，需要调用工具查询。");
    CHECK(call->name == "get_weather");
    CHECK(call->id == "chatcmpl-tool-955ccbe40430534b");  // vLLM 前缀,非 call_
    CHECK(call->input.dump() == "{\"city\":\"北京\"}");

    // 第二轮:工具结果拼回历史。工具段 reasoning 按方言声明的字段名回传。
    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.tools = first.tools;
    second.messages.push_back(UserText("北京天气怎么样"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(
        api::ToolResultBlock{call->id, R"({"temp":"31C"})", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    const api::Message final_message = RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    const auto& replayed = body2.at("messages")[1];
    // vLLM 0.27 新名 reasoning(不是 DeepSeek 的 reasoning_content),原字节。
    CHECK(replayed.at("reasoning") == "用户问北京气温，需要调用工具查询。");
    CHECK_FALSE(replayed.contains("reasoning_content"));
    REQUIRE(replayed.contains("tool_calls"));
    CHECK(replayed.at("tool_calls")[0].at("id") == call->id);
    CHECK(replayed.at("tool_calls")[0].at("function").at("name") == "get_weather");
    CHECK(replayed.at("tool_calls")[0].at("function").at("arguments") == "{\"city\":\"北京\"}");
    const auto& tool_message = body2.at("messages")[2];
    CHECK(tool_message.at("role") == "tool");
    CHECK(tool_message.at("tool_call_id") == call->id);
    // 工具循环里开关照旧落嵌套键,形状不因轮次变脸。
    CHECK(body2.at("chat_template_kwargs").at("enable_thinking") == true);

    // 终答(既有 vLLM 思考流夹具):思考块与正文块都在,usage 尾帧记账。
    REQUIRE(final_message.content.size() == 2);
    CHECK(std::holds_alternative<api::ThinkingBlock>(final_message.content[0]));
    CHECK_FALSE(std::get<api::TextBlock>(final_message.content[1]).text.empty());
}

// ---------------------------------------------------------------------------
// vLLM 本地模型勘察单 P2(§七测试矩阵补齐):responses 面与 messages 面
// 两轮回环。responses:流式 function_call 项的 call_id 是 call_ 前缀
//(非流式项带 fc_/chatcmpl-tool- 双 id,客户端认 call_id),第二轮 input
// 里 function_call/function_call_output 成对、思考项一次性不回传。
// messages:第一轮 thinking 块 + 32 位 hex 假签 + tool_use(chatcmpl-tool-
// 前缀),第二轮 thinking 块带 signature 原字节回传——假签不验格式。
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 responses: vLLM qwen3.8 工具循环——call_ 前缀 id,思考项一次性不回传") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_responses", "live_vllm_qwen38_function_call_stream"),
        FixtureStream("openai_responses", "live_vllm_qwen38_reasoning_text_stream"),
    };
    plan.chunk_seed = 20260831;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::responses::ResponsesBackend backend(base, "test-token", 3000, 30);

    api::Request first;
    first.model = "qwen3.8-27b";
    // 本地自定义端没有目录方言:不配 reasoning/档位,请求侧一个思考键不发
    //(vLLM responses 面的关思考路未实测,不冒充)。
    first.tools.push_back({"get_weather", "查天气", nlohmann::json{{"type", "object"}}});
    first.messages.push_back(UserText("北京天气怎么样"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/responses");
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("model") == "qwen3.8-27b");
    CHECK(body1.at("store") == false);
    CHECK(body1.at("stream") == true);
    CHECK_FALSE(body1.contains("reasoning"));
    CHECK_FALSE(body1.contains("enable_thinking"));
    CHECK_FALSE(body1.contains("max_output_tokens"));

    // 攒出的 assistant:思考块(reasoning_text.delta)+ 工具块;流式
    // call_id 是 call_ 前缀。
    REQUIRE(assistant.content.size() == 2);
    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content[0]);
    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content[1]);
    REQUIRE(thinking != nullptr);
    REQUIRE(call != nullptr);
    CHECK(thinking->text == "The user asks Beijing weather, call get_weather.");
    CHECK(call->id == "call_b6db57c845012a73");
    CHECK(call->name == "get_weather");
    CHECK(call->input.dump() == "{\"city\":\"北京\"}");

    // 第二轮:工具结果拼回历史,思考块不出站。
    api::Request second;
    second.model = first.model;
    second.tools = first.tools;
    second.messages.push_back(UserText("北京天气怎么样"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{call->id, R"({"temp":"31C"})", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    const api::Message final_message = RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    // input 里:function_call(call_id 原样,name/arguments 齐)+ function_
    // call_output(同 call_id);reasoning 一次性,一个条目都不回传。
    const auto& input = body2.at("input");
    bool saw_function_call = false;
    bool saw_output = false;
    bool saw_reasoning_replay = false;
    for (const auto& item : input) {
        const std::string type = item.value("type", "");
        if (type == "function_call" && item.value("call_id", "") == call->id) {
            saw_function_call = true;
            CHECK(item.at("name") == "get_weather");
            CHECK(item.at("arguments") == "{\"city\":\"北京\"}");
        }
        if (type == "function_call_output" && item.value("call_id", "") == call->id) {
            saw_output = true;
            CHECK(item.at("output") == R"({"temp":"31C"})");
        }
        if (type == "reasoning") {
            saw_reasoning_replay = true;
        }
    }
    CHECK(saw_function_call);
    CHECK(saw_output);
    CHECK_FALSE(saw_reasoning_replay);

    // 终答(reasoning_text 思考流夹具):思考 + 正文都在,usage 记账。
    REQUIRE(final_message.content.size() == 2);
    CHECK(std::holds_alternative<api::ThinkingBlock>(final_message.content[0]));
    CHECK_FALSE(std::get<api::TextBlock>(final_message.content[1]).text.empty());
}

TEST_CASE("wire 回环 responses: 2xx 收到非 SSE 的 JSON 体走非流式展开,不报流意外结束") {
    // vLLM 勘察单 P2:兼容端把 stream 请求当非流式答——响应体是整只
    // JSON 对象(output 数组逐项),一行 data: 都没有,分帧器一帧解不出。
    // 传输层 2xx 正常收完,流式路到收尾触发非流式回退(ExpandNonStream
    // -Response),思考/正文/usage 一样不缺;展不开(真断流)才报错。
    const std::string body =
        R"({"id":"resp_ns","object":"response","status":"completed","model":"qwen3.8-27b",)"
        R"("output":[{"id":"rs_1","type":"reasoning","summary":[],)"
        R"("content":[{"text":"We need answer user: 2+2=4.","type":"reasoning_text"}]},)"
        R"({"id":"msg_1","type":"message","role":"assistant","status":"completed",)"
        R"("content":[{"type":"output_text","text":"\n\n2"}]}],)"
        R"("usage":{"input_tokens":59,"output_tokens":38}})";
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {body};
    plan.json_body = true;
    plan.chunk_seed = 20260831;  // JSON 体也按随机小块切着发
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::responses::ResponsesBackend backend(base, "test-token", 3000, 30);
    api::Request request;
    request.model = "qwen3.8-27b";
    request.messages.push_back(UserText("2+2=?"));

    api::MessageAssembler assembler;
    std::string stop;
    const auto result = backend.send_stream(request, [&](const api::StreamEvent& event) {
        if (const auto* done = std::get_if<api::MessageDone>(&event)) {
            stop = done->stop_reason;
        }
        assembler.Feed(event);
    });
    REQUIRE(result.has_value());  // 没走"流意外结束"的报错路
    CHECK(stop == "end_turn");
    const api::Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    const auto* thinking = std::get_if<api::ThinkingBlock>(&message.content[0]);
    const auto* text = std::get_if<api::TextBlock>(&message.content[1]);
    REQUIRE(thinking != nullptr);
    REQUIRE(text != nullptr);
    CHECK(thinking->text == "We need answer user: 2+2=4.");
    CHECK(text->text == "\n\n2");
}

TEST_CASE("wire 回环 messages: vLLM qwen3.8 假签回传——hex signature 原字节,chatcmpl-tool- id") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("anthropic_messages", "live_vllm_qwen38_thinking_then_tool_use_stream"),
        FixtureStream("anthropic_messages", "manual_thinking_enabled_stream"),
    };
    plan.chunk_seed = 20260831;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::anthropic::AnthropicBackend backend(base, "test-token", 3000, 30);

    api::Request first;
    first.model = "qwen3.8-27b";
    first.reasoning = CatalogReasoning("vllm-anthropic", "qwen3.8-27b");
    first.reasoning_effort = "high";
    first.max_tokens = 2048;
    first.tools.push_back({"get_weather", "查天气", nlohmann::json{{"type", "object"}}});
    first.messages.push_back(UserText("北京天气怎么样"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/v1/messages");
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("model") == "qwen3.8-27b");
    // vllm-anthropic 方言:请求形状与真 Anthropic 同(thinking enabled +
    // budget);这端 disabled 无效是响应侧行为,请求侧照发不误。
    CHECK(body1.at("thinking").at("type") == "enabled");
    CHECK(body1.at("thinking").contains("budget_tokens"));

    // 攒出的 assistant:thinking 块带 32 位 hex 假签 + tool_use
    //(chatcmpl-tool- 前缀,非 toolu_ 前缀)。
    REQUIRE(assistant.content.size() == 2);
    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content[0]);
    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content[1]);
    REQUIRE(thinking != nullptr);
    REQUIRE(call != nullptr);
    CHECK(thinking->text == "用户问北京气温,需要调用工具查询。");
    CHECK(thinking->signature == "4bbde37d9a1f60c2e8d5a3b41f07c6d2");
    CHECK(call->id == "chatcmpl-tool-a31f7da795ee7a5b");
    CHECK(call->name == "get_weather");
    CHECK(call->input.dump() == "{\"city\":\"北京\"}");

    // 第二轮:thinking 块带 signature 原字节回传(假签不验格式,原样走),
    // tool_result 进 user。
    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.max_tokens = 2048;
    second.tools = first.tools;
    second.messages.push_back(UserText("北京天气怎么样"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{call->id, R"({"temp":"31C"})", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    const api::Message final_message = RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    const auto& replayed = body2.at("messages")[1].at("content");
    REQUIRE(replayed.size() == 2);
    CHECK(replayed[0].at("type") == "thinking");
    CHECK(replayed[0].at("signature") == "4bbde37d9a1f60c2e8d5a3b41f07c6d2");  // 原字节
    CHECK(replayed[0].at("thinking") == thinking->text);
    CHECK(replayed[1].at("type") == "tool_use");
    CHECK(replayed[1].at("id") == call->id);
    CHECK(replayed[1].at("name") == "get_weather");
    const auto& tool_result = body2.at("messages")[2].at("content");
    CHECK(tool_result[0].at("type") == "tool_result");
    CHECK(tool_result[0].at("tool_use_id") == call->id);

    // 终答(真签思考流夹具):思考 + 正文都在。
    REQUIRE(final_message.content.size() == 2);
    CHECK(std::holds_alternative<api::ThinkingBlock>(final_message.content[0]));
    CHECK_FALSE(std::get<api::TextBlock>(final_message.content[1]).text.empty());
}

// ---------------------------------------------------------------------------
// Kimi 保留式思考单 P1:K2.6 开 history all 的两轮跨 Turn 回环。
//   Request 1: U1(thinking.keep=all 已在场——第一份请求就得告诉服务端
//              这场要保留)
//   Response 1: reasoning_content + content
//   Request 2: U1 + assistant(reasoning_content 原字节) + U2
//              (keep 与 type 仍同发)
// 随后同一场子把模型切成 K3(模拟 /model 切换):keep 不误带,reasoning
// 照回(always),thinking 整个不发——K2.6 的 keep 状态不硬带。
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 chat: Kimi K2.6 history all 两轮跨 Turn——keep 与历史 reasoning 同发,切 K3 不误带") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_chat", "kimi_k26_reasoning_text_stream"),
        FixtureStream("openai_chat", "kimi_k26_reasoning_text_stream"),
        FixtureStream("openai_chat", "kimi_k3_reasoning_text_stream"),
    };
    plan.chunk_seed = 20260830;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::chat::ChatCompletionsBackend backend(base, "test-token", 3000, 30,
                                              nlohmann::json::object(), {}, api::chat::ChatRequestOptions{});

    // Turn 1:开了 history all 的第一份请求——keep 从第一份就在场。
    api::Request first;
    first.model = "kimi-k2.6";
    first.reasoning = CatalogReasoning("moonshot", "kimi-k2.6");
    first.reasoning_effort = "high";  // 档位只当开关用:官方不认 effort 参数
    first.reasoning_history = api::ReasoningHistoryMode::All;
    first.messages.push_back(UserText("你好"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "end_turn");

    REQUIRE(received.size() == 1);
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("thinking").at("type") == "enabled");
    CHECK(body1.at("thinking").at("keep") == "all");  // 第一份请求就声明保留
    CHECK_FALSE(body1.contains("reasoning_effort"));

    REQUIRE(assistant.content.size() == 2);
    const auto* thinking = std::get_if<api::ThinkingBlock>(&assistant.content[0]);
    const auto* text = std::get_if<api::TextBlock>(&assistant.content[1]);
    REQUIRE(thinking != nullptr);
    REQUIRE(text != nullptr);
    CHECK(thinking->text == "这轮想的要跨轮保留");

    // Turn 2:纯对话历史跨轮回传——keep 在场,历史 reasoning 原字节随行
    //(方言缺省 tool_episode 不回传纯对话段,history all 把 replay 升 always)。
    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.reasoning_history = api::ReasoningHistoryMode::All;
    second.messages.push_back(UserText("你好"));
    second.messages.push_back(assistant);
    second.messages.push_back(UserText("再问一句"));

    std::string final_stop;
    RunRound(backend, second, &final_stop);
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    CHECK(body2.at("thinking").at("type") == "enabled");
    CHECK(body2.at("thinking").at("keep") == "all");
    const auto& replayed = body2.at("messages")[1];
    CHECK(replayed.at("role") == "assistant");
    CHECK(replayed.at("reasoning_content") == "这轮想的要跨轮保留");  // 原字节
    CHECK(replayed.at("content") == "答:你好");
    CHECK_FALSE(body2.contains("reasoning_effort"));

    // 同一场子切到 K3(模型方言换了,history 选择仍在):thinking 整个不发
    // ——keep 一个字节不误带;always 回传照旧(固定契约与选择无关)。
    api::Request third;
    third.model = "kimi-k3";
    third.reasoning = CatalogReasoning("moonshot", "kimi-k3");
    third.reasoning_effort = "high";
    third.reasoning_history = api::ReasoningHistoryMode::All;  // 选择没清,守门靠方言
    third.messages.push_back(UserText("你好"));
    third.messages.push_back(assistant);
    third.messages.push_back(UserText("切了模型再问"));

    std::string k3_stop;
    RunRound(backend, third, &k3_stop);
    REQUIRE(received.size() == 3);
    const auto body3 = nlohmann::json::parse(received[2].body);
    CHECK(body3.at("model") == "kimi-k3");
    CHECK(body3.at("reasoning_effort") == "high");
    CHECK_FALSE(body3.contains("thinking"));  // keep 不硬带,thinking 整个不发
    const auto& k3_replayed = body3.at("messages")[1];
    CHECK(k3_replayed.at("reasoning_content") == "这轮想的要跨轮保留");  // always 照回
}

// ---------------------------------------------------------------------------
// openai-responses:reasoning -> function_call -> 回传 -> 终答
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 responses: reasoning 一次性不回传,function_call/output 原样走 input") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("openai_responses", "manual_function_call_after_reasoning"),
        FixtureStream("openai_responses", "manual_basic_message_stream"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::responses::ResponsesBackend backend(base, "test-token", 3000, 30);

    api::Request first;
    first.model = "qwen3.7-max";
    first.reasoning = CatalogReasoning("dashscope-responses", "qwen3.7-max");
    first.reasoning_effort = "high";
    first.messages.push_back(UserText("杭州天气怎么样"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/responses");
    const auto body1 = nlohmann::json::parse(received[0].body);
    CHECK(body1.at("model") == "qwen3.7-max");
    // qwen3.7-max 在 responses 家没声明 effort 档(只有 none/auto + toggle):
    // 方言走旧开关顶层 enable_thinking(手册:reasoning.effort 优先,没有
    // 档位可落时才用 enable_thinking),不夹带 thinking 键。
    CHECK(body1.at("enable_thinking") == true);
    CHECK_FALSE(body1.contains("reasoning"));
    CHECK_FALSE(body1.contains("thinking"));

    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content.back());
    REQUIRE(call != nullptr);

    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.messages.push_back(UserText("杭州天气怎么样"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{call->id, R"({"temp":"31C"})", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    // input 里:function_call(call_id 原样)+ function_call_output;reasoning
    // 一次性,思考块不回传。
    const auto& input = body2.at("input");
    bool saw_function_call = false;
    bool saw_output = false;
    bool saw_reasoning_replay = false;
    for (const auto& item : input) {
        const std::string type = item.value("type", "");
        if (type == "function_call" && item.value("call_id", "") == call->id) {
            saw_function_call = true;
        }
        if (type == "function_call_output" && item.value("call_id", "") == call->id) {
            saw_output = true;
        }
        if (type == "reasoning") saw_reasoning_replay = true;
    }
    CHECK(saw_function_call);
    CHECK(saw_output);
    CHECK_FALSE(saw_reasoning_replay);  // responses 思考不回传
}

// ---------------------------------------------------------------------------
// google-generate-content:thought -> functionCall -> 回传 -> 终答
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 gemini: thought 一次性不重放,functionCall/functionResponse 配对回传") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("google_generate_content", "internal_thought_function_call_stream"),
        FixtureStream("google_generate_content", "internal_thought_part_stream"),
    };
    plan.chunk_seed = 20260827;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::gemini::GeminiBackend backend(base, "test-token", 3000, 30);

    api::Request first;
    first.model = "gemini-3.1-flash-lite";
    first.reasoning = CatalogReasoning("gemini", "gemini-3.1-flash-lite");
    first.reasoning_effort = "high";
    first.messages.push_back(UserText("杭州天气怎么样"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    REQUIRE(received.size() == 1);
    CHECK(received[0].path == "/v1beta/models/gemini-3.1-flash-lite:streamGenerateContent");
    CHECK(received[0].query == "?alt=sse");
    CHECK(received[0].headers.at("x-goog-api-key") == "test-token");
    const auto body1 = nlohmann::json::parse(received[0].body);
    // gemini 家方言:thinkingConfig(level 与 budget 不同发,选 level)。
    const auto& thinking1 = body1.at("generationConfig").at("thinkingConfig");
    CHECK(thinking1.at("includeThoughts") == true);
    CHECK(thinking1.contains("thinkingLevel"));

    const auto* call = std::get_if<api::ToolUseBlock>(&assistant.content.back());
    REQUIRE(call != nullptr);
    CHECK(call->name == "get_weather");

    api::Request second;
    second.model = first.model;
    second.reasoning = first.reasoning;
    second.reasoning_effort = "high";
    second.messages.push_back(UserText("杭州天气怎么样"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{call->id, R"({"temp":"31C"})", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    // contents 里:functionCall(模型发起)与 functionResponse(按函数名对)
    // 各自在条;thought part 不重放。
    bool saw_call = false;
    bool saw_response = false;
    for (const auto& content : body2.at("contents")) {
        for (const auto& part : content.at("parts")) {
            if (part.contains("functionCall") &&
                part.at("functionCall").at("name") == "get_weather") {
                saw_call = true;
            }
            if (part.contains("functionResponse") &&
                part.at("functionResponse").at("name") == "get_weather") {
                saw_response = true;
            }
        }
    }
    CHECK(saw_call);
    CHECK(saw_response);
}

// ---------------------------------------------------------------------------
// 负路径:半路断流不是绿
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环: 半路断流必须报错,2xx 不是绿") {
    std::vector<HttpRequest> received;
    const int port = StartCutoffServer(&received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::anthropic::AnthropicBackend backend(base, "test-token", 3000, 10);
    api::Request request;
    request.model = "qwen3.7-plus";
    request.max_tokens = 16;
    request.messages.push_back(UserText("hi"));

    bool saw_done = false;
    const auto result = backend.send_stream(request, [&](const api::StreamEvent& event) {
        if (std::holds_alternative<api::MessageDone>(event)) saw_done = true;
    });
    // 服务端只回了半截流:要么 send_stream 报错,要么事件流里根本没有
    // MessageDone——绝不允许"HTTP 200 = 一切正常"糊过去。
    CHECK((!result.has_value() || !saw_done));
}


// ---------------------------------------------------------------------------
// anthropic-messages:动态工具 P3(原生工具搜索)。服务端搜索 -> 结果块 ->
// 模型直调发现的工具;第二轮原生对块原样回传(不配 tool_result),发现的
// 工具照常配 tool_result。夹具手工转写自官方 Streaming 样例(真机核对未
// 做,本机无钥匙——对照脚本在 tests/manual/native_tool_search_cache_compare.py)。
// ---------------------------------------------------------------------------

TEST_CASE("wire 回环 anthropic: 原生工具搜索无损解析与回传,server 块不配 tool_result(P3)") {
    std::vector<HttpRequest> received;
    ReplayPlan plan;
    plan.sse_rounds = {
        FixtureStream("anthropic_messages", "manual_native_tool_search_stream"),
        FixtureStream("anthropic_messages", "manual_thinking_enabled_stream"),
    };
    plan.chunk_seed = 20260831;
    const int port = StartReplayServer(plan, &received);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    api::anthropic::AnthropicBackend backend(base, "test-token", 3000, 30);
    // 生产同款 native 请求:server_tool_search 声明 + 延迟定义标 defer_loading。
    api::Request first;
    first.model = "claude-opus-5";
    first.max_tokens = 2048;
    first.server_tool_search = "regex";
    api::ToolDefinition deferred;
    deferred.name = "mcp__github__search_issues";
    deferred.description = "Search issues in a GitHub repository.";
    deferred.input_schema = nlohmann::json{{"type", "object"}};
    deferred.load_mode = api::ToolLoadMode::Deferred;
    first.tools.push_back(deferred);
    first.messages.push_back(UserText("帮我查 LubanCode 的 issue"));

    std::string stop;
    const api::Message assistant = RunRound(backend, first, &stop);
    CHECK(stop == "tool_use");

    // 请求侧:本地延迟定义在前(带 defer_loading),服务端搜索声明追加在后
    //(server tool 声明永远排在本地函数工具之后)。
    REQUIRE(received.size() == 1);
    const auto body1 = nlohmann::json::parse(received[0].body);
    REQUIRE(body1.at("tools").size() == 2);
    CHECK(body1["tools"][0].at("name") == "mcp__github__search_issues");
    CHECK(body1["tools"][0].at("defer_loading") == true);
    CHECK(body1["tools"][1].at("type") == "tool_search_tool_regex_20251119");
    CHECK(body1["tools"][1].at("name") == "tool_search_tool_regex");

    // 攒出的 assistant:正文 + server_tool_use + tool_search_tool_result + 真实 tool_use。
    REQUIRE(assistant.content.size() == 4);
    CHECK(std::holds_alternative<api::TextBlock>(assistant.content[0]));
    const auto* server_use = std::get_if<api::ServerToolUseBlock>(&assistant.content[1]);
    const auto* server_result = std::get_if<api::ServerToolResultBlock>(&assistant.content[2]);
    const auto* real_call = std::get_if<api::ToolUseBlock>(&assistant.content[3]);
    REQUIRE(server_use != nullptr);
    REQUIRE(server_result != nullptr);
    REQUIRE(real_call != nullptr);
    CHECK(server_use->id == "srvtoolu_01ABC123");
    CHECK(server_use->name == "tool_search_tool_regex");
    CHECK(server_use->input.at("pattern") == "github issue");
    CHECK(server_result->tool_use_id == "srvtoolu_01ABC123");
    CHECK(server_result->content.at("tool_references").at(0).at("tool_name") ==
          "mcp__github__search_issues");
    CHECK(real_call->name == "mcp__github__search_issues");
    CHECK(real_call->input.at("query") == "repo:relic-yuexi/LubanCode cache");

    // 第二轮:原生对块原样回传(server 的 srvtoolu id 绝不配 tool_result),
    // 真实工具照常配 tool_result;server_tool_search 与 defer_loading 再发。
    api::Request second;
    second.model = first.model;
    second.max_tokens = 2048;
    second.server_tool_search = "regex";
    second.tools = first.tools;
    second.messages.push_back(UserText("帮我查 LubanCode 的 issue"));
    second.messages.push_back(assistant);
    api::Message tool_back;
    tool_back.role = api::Role::User;
    tool_back.content.push_back(api::ToolResultBlock{real_call->id, "issue #42", false});
    second.messages.push_back(std::move(tool_back));

    std::string final_stop;
    const api::Message final_message = RunRound(backend, second, &final_stop);
    CHECK(final_stop == "end_turn");
    CHECK_FALSE(final_message.content.empty());
    REQUIRE(received.size() == 2);
    const auto body2 = nlohmann::json::parse(received[1].body);
    REQUIRE(body2.at("tools").size() == 2);
    CHECK(body2["tools"][0].at("defer_loading") == true);

    const auto& replayed = body2.at("messages")[1].at("content");
    REQUIRE(replayed.size() == 4);
    CHECK(replayed[1].at("type") == "server_tool_use");
    CHECK(replayed[1].at("id") == "srvtoolu_01ABC123");
    CHECK(replayed[1].at("input").at("pattern") == "github issue");
    CHECK(replayed[2].at("type") == "tool_search_tool_result");
    CHECK(replayed[2].at("tool_use_id") == "srvtoolu_01ABC123");
    CHECK(replayed[2].at("content").at("tool_references").at(0).at("tool_name") ==
          "mcp__github__search_issues");
    CHECK(replayed[3].at("type") == "tool_use");
    CHECK(replayed[3].at("id") == real_call->id);
    // tool_result 只配真实工具那枚 id;整份回传体里没有给 srvtoolu 的
    // tool_result(官方:给服务端 id 配 tool_result 会被拒)。
    const auto& tool_result = body2.at("messages")[2].at("content");
    REQUIRE(tool_result.size() == 1);
    CHECK(tool_result[0].at("type") == "tool_result");
    CHECK(tool_result[0].at("tool_use_id") == real_call->id);
    for (const auto& message : body2.at("messages")) {
        for (const auto& block : message.at("content")) {
            if (block.value("type", std::string()) == "tool_result") {
                CHECK(block.at("tool_use_id").get<std::string>().rfind("srvtoolu", 0) != 0);
            }
        }
    }
}
