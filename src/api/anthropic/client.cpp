#include "api/anthropic/client.hpp"

#include <charconv>
#include <type_traits>
#include <utility>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "api/anthropic/events.hpp"
#include "api/sse_framing.hpp"

namespace lubancode::api::anthropic {

namespace {

using nlohmann::json;

std::string RoleToString(Role role) {
    return role == Role::User ? "user" : "assistant";
}

json ContentBlockToJson(const ContentBlock& block) {
    return std::visit(
        [](const auto& b) -> json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                return json{{"type", "text"}, {"text", b.text}};
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return json{{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                json j{{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}, {"content", b.content}};
                if (b.is_error) {
                    j["is_error"] = true;
                }
                return j;
            }
        },
        block);
}

// 拼出 Anthropic Messages API 的请求体(stream: true 恒开,M1 只走流式)。
json BuildRequestJson(const Request& request) {
    json body;
    body["model"] = request.model;
    body["max_tokens"] = request.max_tokens;
    body["stream"] = true;

    if (!request.system.empty()) {
        body["system"] = request.system;
    }

    json messages = json::array();
    for (const auto& message : request.messages) {
        json content = json::array();
        for (const auto& block : message.content) {
            content.push_back(ContentBlockToJson(block));
        }
        messages.push_back(json{{"role", RoleToString(message.role)}, {"content", content}});
    }
    body["messages"] = messages;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{
                {"name", tool.name},
                {"description", tool.description},
                {"input_schema", tool.input_schema},
            });
        }
        body["tools"] = tools;
    }

    return body;
}

// 从 HTTP 状态行(形如 "HTTP/1.1 200 OK")里抠出状态码。抠不出来返回 0。
int ExtractStatusCode(std::string_view header_line) {
    if (header_line.rfind("HTTP/", 0) != 0) {
        return 0;
    }
    const std::size_t space1 = header_line.find(' ');
    if (space1 == std::string_view::npos) {
        return 0;
    }
    const std::size_t space2 = header_line.find(' ', space1 + 1);
    const std::string_view code_sv = header_line.substr(space1 + 1, space2 == std::string_view::npos ? std::string_view::npos : space2 - space1 - 1);
    int code = 0;
    const auto result = std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), code);
    if (result.ec != std::errc{}) {
        return 0;
    }
    return code;
}

}  // namespace

AnthropicBackend::AnthropicBackend(std::string base_url, std::string auth_token)
    : base_url_(std::move(base_url)), auth_token_(std::move(auth_token)) {}

std::expected<void, Error> AnthropicBackend::send_stream(
    const Request& request,
    const std::function<void(const StreamEvent&)>& on_event) {
    const json body = BuildRequestJson(request);
    const std::string body_str = body.dump();

    SseFramer framer;
    std::string error_body;
    int status_code = 0;
    bool status_known = false;

    cpr::HeaderCallback header_cb(
        [&](const std::string_view& header, intptr_t) -> bool {
            if (const int code = ExtractStatusCode(header); code != 0) {
                status_code = code;
                status_known = true;
            }
            return true;
        });

    cpr::WriteCallback write_cb(
        [&](const std::string_view& data, intptr_t) -> bool {
            const bool is_success = status_known && status_code >= 200 && status_code < 300;
            if (!is_success) {
                // 非 2xx:这不是 SSE 流,是普通的错误响应体,原样攒起来
                // 好塞进 Error 里,不要喂给分帧器瞎解析。
                error_body.append(data);
                return true;
            }
            for (const SseFrame& frame : framer.feed(data)) {
                if (auto event = parse_event(frame); event.has_value()) {
                    on_event(*event);
                }
            }
            return true;
        });

    const std::string url = base_url_ + "/v1/messages";

    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + auth_token_},
        },
        cpr::Body{body_str},
        header_cb,
        write_cb);

    if (response.error) {
        return std::unexpected(Error{ErrorKind::Network, response.error.message, 0});
    }

    const int final_status = static_cast<int>(response.status_code);
    if (final_status < 200 || final_status >= 300) {
        std::string message = !error_body.empty() ? error_body : response.text;
        if (message.empty()) {
            message = "服务端返回了非 200 状态码,但响应体是空的";
        }
        return std::unexpected(Error{ErrorKind::HttpStatus, std::move(message), final_status});
    }

    return {};
}

}  // namespace lubancode::api::anthropic
