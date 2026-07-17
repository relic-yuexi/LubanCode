#include "api/anthropic/client.hpp"

#include <charconv>
#include <optional>
#include <type_traits>
#include <utility>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "api/anthropic/events.hpp"
#include "api/sse_framing.hpp"

namespace lubancode::api::anthropic {

using nlohmann::json;

namespace {

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

// M6.6:think 档位 -> Anthropic 风格 thinking 参数。实测(MiniMax-M3 真实
// anthropic 兼容端点 /anthropic/v1/messages)确认支持
// {"type":"enabled","budget_tokens":N} / {"type":"disabled"},HTTP 200,
// 且 enabled 时真的返回 thinking 内容块——所以这里走"接受并映射"这条路,
// 不是"协议不支持,打警告跳过"那条路。
// 档位 -> budget_tokens 的具体数字是任务明确交给这里自己定的一个设计选择
// (原话"档位→budget 映射你定"),选的是 low=1024/medium=4096/high=16384
// 这组常见量级;唯一的硬约束是 Anthropic 要求 budget_tokens 必须小于
// max_tokens(不然思考预算比整个回复上限还大,没意义、也可能被端点拒绝),
// 所以这里按 request.max_tokens 兜底夹一下,budget 超过 max_tokens 时退化成
// "max_tokens 留 256 给正文,剩下全给思考",绝不出现 budget >= max_tokens
// 的组合。
std::optional<json> BuildThinkingJson(const Request& request) {
    if (request.reasoning_effort.empty()) {
        return std::nullopt;
    }
    if (request.reasoning_effort == "none") {
        return json{{"type", "disabled"}};
    }

    int budget = 1024;
    if (request.reasoning_effort == "low") {
        budget = 1024;
    } else if (request.reasoning_effort == "medium") {
        budget = 4096;
    } else if (request.reasoning_effort == "high") {
        budget = 16384;
    } else {
        // 不认得的档位(配置层已经拦过合法值,理论上到不了这里):当没设置处理。
        return std::nullopt;
    }

    if (budget >= request.max_tokens) {
        budget = request.max_tokens > 256 ? request.max_tokens - 256 : request.max_tokens / 2;
        if (budget < 1) {
            budget = 1;
        }
    }

    return json{{"type", "enabled"}, {"budget_tokens", budget}};
}

}  // namespace

// 拼出 Anthropic Messages API 的请求体(stream: true 恒开,M1 只走流式)。
// 声明在 client.hpp 里,单测用;线上代码路径(send_stream)也是调这个函数。
json BuildRequestJson(const Request& request) {
    json body;
    body["model"] = request.model;
    body["max_tokens"] = request.max_tokens;
    body["stream"] = true;

    if (!request.system.empty()) {
        body["system"] = request.system;
    }

    if (const auto thinking = BuildThinkingJson(request); thinking.has_value()) {
        body["thinking"] = *thinking;
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

namespace {

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
