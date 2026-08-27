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
struct ReplayPlan {
    std::vector<std::string> sse_rounds;  // 每连接一份完整 SSE 字节
    std::uint32_t chunk_seed = 20260827;  // 0 = 整块发
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
                    "HTTP/1.1 200 OK\r\n"
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
