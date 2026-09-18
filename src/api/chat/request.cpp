#include "api/chat/request.hpp"

#include <type_traits>
#include <variant>

namespace lubancode::api::chat {

namespace {

using nlohmann::json;

json TextAndImages(const Message& message) {
    json parts = json::array();
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) {
            parts.push_back(json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* image = std::get_if<ImageBlock>(&block)) {
            parts.push_back(json{{"type", "image_url"},
                                 {"image_url", json{{"url", "data:" + image->media_type + ";base64," + image->data}}}});
        }
    }
    return parts;
}

bool HasImage(const Message& message) {
    for (const auto& block : message.content) {
        if (std::holds_alternative<ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

std::string JoinedText(const Message& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* part = std::get_if<TextBlock>(&block)) {
            text += part->text;
        } else if (const auto* image = std::get_if<ModelImageBlock>(&block)) {
            // 模型输出图片的替身:引用翻短文本标记(base64 不回传)。assistant
            // 消息的 content 只有这一处出口,不接的话续聊会把图片痕迹整个丢掉。
            if (!text.empty()) {
                text += "\n";
            }
            text += ModelImageReplayText(*image);
        } else if (const auto* server_use = std::get_if<ServerToolUseBlock>(&block)) {
            // anthropic 原生工具搜索块(动态工具 P3)在 chat wire 的明降级:
            // 这条 wire 没有 server tool 的形状,翻成一句事实文本,不悄悄丢块
            // ——历史里发生过什么,下一轮还看得见。
            if (!text.empty()) {
                text += "\n";
            }
            text += "[服务端工具搜索(anthropic 原生): " + server_use->name + " 已由 provider 执行]";
        } else if (const auto* server_result = std::get_if<ServerToolResultBlock>(&block)) {
            if (!text.empty()) {
                text += "\n";
            }
            text += "[服务端工具搜索结果(anthropic 原生): " + server_result->content.dump() + "]";
        }
    }
    return text;
}

// 消息里全部思考正文的原字节拼接(块序不动,不加标签不摘要)。
std::string JoinedThinking(const Message& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* part = std::get_if<ThinkingBlock>(&block)) {
            text += part->text;
        }
    }
    return text;
}

// 有思考就回传;方言只指定字段与服务端额外参数。
bool WantsKeepAll(const Request& request) {
    return ShouldReplayThinking(request) &&
           request.reasoning.dialect.history_control == "thinking_keep" &&
           !ReasoningEffortIsOff(request.reasoning_effort, request.reasoning);
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, const nlohmann::json& extra_body,
                                const ChatRequestOptions& options, WireMessageMap* wire_map) {
    json body{{"model", request.model}, {"stream", true}};
    // max_tokens 可省略(chat 协议):unset 就整个不带字段,交服务端/模型
    // 默认——vLLM 这类端的默认上限远大于旧版写死的 4096,reasoning 模型
    // 思考不至于一步撞墙(规格根因一)。显式声明了才落键。
    if (request.max_tokens.has_value()) {
        body["max_tokens"] = *request.max_tokens;
    }

    json messages = json::array();
    // 拍平对照(差距清单 §8.2 第 7 条)先立骨架:第 7 条要的是"内部消息
    // 序 -> wire 消息序",chat 的 system 是多源拼一条(Request::system +
    // 多条 System 消息),正文与工具结果可一裂二,对照表按实际落点记。
    if (wire_map != nullptr) {
        wire_map->container = "messages";
        wire_map->message_to_wire.assign(request.messages.size(), {});
    }
    // 四角色换骨(v3 第二棒,差距清单 §8.2 第 2 条):System 角色消息是
    // 上下文根,按 Chat 协议落 system role 消息——这家的 system 是消息流
    // 里的一员,没有顶层参数(与 anthropic 的顶层 system、responses 的
    // instructions 分家,§4.46 目标表)。Request::system(现行两角色路径
    // 的唯一入口)先行,System 消息按消息序接在其后("\n" 连接,空段
    // 不造),多源拼成一条落 messages 首位。System 消息只取 TextBlock
    // ——v3 §1.2 的 system 是 soul/规则文本,富块后续棒次需要再扩,这里
    // 不悄悄丢也不硬造。
    std::string system_text = request.system;
    for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index) {
        const auto& message = request.messages[message_index];
        if (message.role != Role::System) {
            continue;
        }
        bool contributed = false;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<TextBlock>(&block);
                text != nullptr && !text->text.empty()) {
                if (!system_text.empty()) {
                    system_text += "\n";
                }
                system_text += text->text;
                contributed = true;
            }
        }
        // 多源拼进同一条 system 消息(差距清单 §8.2 第 7 条的"两个内部
        // messageRef 对应同一 wire message"):出过字的 System 消息都指
        // 向 wire[0];没出过字的空壳如实记空。
        if (wire_map != nullptr && contributed) {
            wire_map->message_to_wire[message_index].push_back(0);
        }
    }
    if (!system_text.empty()) {
        messages.push_back(json{{"role", "system"}, {"content", std::move(system_text)}});
    }

    const auto& dialect = request.reasoning.dialect;
    const std::string replay_field = dialect.replay_field.empty()
                                         ? options.reasoning_replay_field
                                         : dialect.replay_field;

    for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index) {
        const auto& message = request.messages[message_index];
        // System 角色已折进首条 system 消息,对话流里一条不落(不重复注入)。
        if (message.role == Role::System) {
            continue;
        }
        // User 与 Tool 同路(四角色换骨):Tool 是独立的工具结果消息,不再
        // 伪装 user——而 Chat wire 的拍平本就把 User 容器里的
        // ToolResultBlock 拆成独立 role=tool 消息,两角殊途同归:正文/图片
        // 落 user 消息,每枚 ToolResultBlock 各落一条 tool 消息
        //(tool_call_id 配对)。只装工具结果的消息(JoinedText 空、无图)
        // 不产 user 消息,空正文不造——与旧路逐字节同形。
        if (message.role == Role::User || message.role == Role::Tool) {
            std::string text = JoinedText(message);
            const bool has_image = HasImage(message);
            if (!text.empty() || has_image) {
                if (wire_map != nullptr) {
                    wire_map->message_to_wire[message_index].push_back(messages.size());
                }
                messages.push_back(json{{"role", "user"},
                                        {"content", has_image ? TextAndImages(message) : json(text)}});
            }
            for (const auto& block : message.content) {
                if (const auto* result = std::get_if<ToolResultBlock>(&block)) {
                    // 工具结果图片:chat completions 的 tool 消息 content 只有
                    // 字符串一档(image_url 部件只在 user 消息有文档背书,
                    // 见 developers.openai.com/api/reference/resources/chat/
                    // subresources/completions/methods/create 的 role:tool 条
                    // 目;社区同口径 community.openai.com/t/gpt4-o-support-
                    // for-image-urls-as-tool-responses/907546)。不硬造数组
                    // 协议——图片字节不出门,追加一行明降级附注指路落盘
                    // 路径;没有图片块的结果一个字节不加,老钉子不红。
                    std::string content = result->content + ToolResultImageDegradedNote(*result);
                    if (wire_map != nullptr) {
                        wire_map->message_to_wire[message_index].push_back(messages.size());
                    }
                    messages.push_back(json{{"role", "tool"},
                                            {"tool_call_id", result->tool_use_id},
                                            {"content", std::move(content)}});
                }
            }
            continue;
        }

        json assistant{{"role", "assistant"}};
        const std::string text = JoinedText(message);
        assistant["content"] = text.empty() ? json(nullptr) : json(text);
        // 不裁掉历史思考,也不从正文里的 <think> 伪造思考块。
        if (ShouldReplayThinking(request)) {
            const std::string reasoning = JoinedThinking(message);
            if (!reasoning.empty()) {
                const std::string field =
                    replay_field.empty() ? std::string("reasoning_content") : replay_field;
                assistant[field] = reasoning;
            }
        }
        json tool_calls = json::array();
        for (const auto& block : message.content) {
            if (const auto* call = std::get_if<ToolUseBlock>(&block)) {
                tool_calls.push_back(json{{"id", call->id},
                                          {"type", "function"},
                                          {"function", json{{"name", call->name},
                                                            {"arguments", call->input.dump()}}}});
            }
        }
        if (!tool_calls.empty()) {
            assistant["tool_calls"] = std::move(tool_calls);
        }
        if (wire_map != nullptr) {
            wire_map->message_to_wire[message_index].push_back(messages.size());
        }
        messages.push_back(std::move(assistant));
    }
    if (wire_map != nullptr) {
        wire_map->wire_element_count = messages.size();
    }
    body["messages"] = std::move(messages);

    // provider 声明了 stream_usage capability 才带 stream_options(有些兼容端
    // 不认这个字段,乱发会被拒);extra_body 在最后浅合并,用户显式写的
    // stream_options 整个压过这里的默认值。
    if (options.stream_usage) {
        body["stream_options"] = json{{"include_usage", true}};
    }

    if (!request.reasoning_effort.empty()) {
        const bool off = ReasoningEffortIsOff(request.reasoning_effort, request.reasoning);
        const auto& dialect = request.reasoning.dialect;
        const bool dialect_toggle = dialect.toggle == "enable_thinking_bool" ||
                                    dialect.toggle == "thinking_type" ||
                                    dialect.toggle == "chat_template_kwargs_enable_thinking";

        // 档位:方言声明了 effort_path 才按形状落,落不落仍看模型声明没声明
        // effort 档;没方言走 legacy(参数名按 provider 本地声明)。
        if (dialect.effort_path == "reasoning_effort") {
            if (request.reasoning.empty() || request.reasoning.supports_effort) {
                const std::string param = dialect.effort_param.empty()
                                              ? (options.reasoning_param.empty()
                                                     ? std::string("reasoning_effort")
                                                     : options.reasoning_param)
                                              : dialect.effort_param;
                body[param] = request.reasoning_effort;
            }
        } else if (dialect.empty() &&
                   (request.reasoning.empty() || request.reasoning.supports_effort)) {
            // 参数名按 provider 声明走(默认 reasoning_effort);空档位仍然整个
            // 缺席字段——"不填"就是真的不发,不偷偷塞默认档。
            const std::string param = options.reasoning_param.empty() ? std::string("reasoning_effort")
                                                                      : options.reasoning_param;
            body[param] = request.reasoning_effort;
        }

        // 开关:方言给了形状按形状;没方言维持 generic thinking.type(兼容
        // 旧目录,视为 unverified)。GLM 这类"档位+开关两键并开"由手册背书
        // (zai 实测),两键各自独立。
        if (dialect_toggle && request.reasoning.supports_toggle) {
            if (dialect.toggle == "enable_thinking_bool") {
                body["enable_thinking"] = !off;
            } else if (dialect.toggle == "chat_template_kwargs_enable_thinking") {
                // vLLM/qwen 模板开关:嵌套键。extra_body 的浅合并规矩照旧——
                // 用户显式写了顶层 chat_template_kwargs 就整个压过这里(想带
                // 别的模板参数得整份写,同 stream_options 的待遇)。
                body["chat_template_kwargs"] = json{{"enable_thinking", !off}};
            } else {
                body["thinking"] = json{{"type", off ? dialect.toggle_off : dialect.toggle_on}};
            }
        } else if (dialect.empty() && request.reasoning.supports_toggle) {
            body["thinking"] = json{{"type", off ? "disabled" : "enabled"}};
        }

        // 预算:方言声明了 thinking_budget 且模型声明了 budget 区间才落。
        // auto 档不发(手册:默认值为模型最大思维链长度,不填即默认)。
        if (!off && dialect.budget_path == "thinking_budget" &&
            (request.reasoning.budget_min.has_value() || request.reasoning.budget_max.has_value()) &&
            LowerReasoningEffort(request.reasoning_effort) != "auto") {
            body["thinking_budget"] =
                ReasoningBudgetForEffort(request.reasoning, request.reasoning_effort,
                                          request.max_tokens.value_or(0));
        }
    }

    // K2.6 默认同发 keep 与 type;用户关闭后不再发送 keep。
    if (WantsKeepAll(request)) {
        const auto& dialect = request.reasoning.dialect;
        if (!body.contains("thinking")) {
            body["thinking"] = json::object();
        }
        // type 保持 enabled:K2.6 的保留建立在思考开启之上;档位块刚写过
        // type 就不重写(同值),没写过(空档位)按方言补上。
        if (!body["thinking"].contains("type")) {
            body["thinking"]["type"] =
                dialect.toggle_on.empty() ? std::string("enabled") : dialect.toggle_on;
        }
        body["thinking"]["keep"] =
            dialect.history_all_value.empty() ? std::string("all") : dialect.history_all_value;
    }

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{{"type", "function"},
                                 {"function", json{{"name", tool.name},
                                                   {"description", tool.description},
                                                   {"parameters", ToolSchemaForWire(tool.input_schema)}}}});
        }
        body["tools"] = std::move(tools);
    }

    // extra_body 在最后浅合并:provider 级先、Request::extra_body(模型
    // variant)后,同名顶层键后者压前者(共用件 api::MergeExtraBody)。
    MergeExtraBody(body, extra_body);
    MergeExtraBody(body, request.extra_body);
    return body;
}

// 拍平对照(差距清单 §8.2 第 7 条):与 BuildRequestJson 同一条拼装路
// 产出(第四参传指针共用)。消息拍平与 options/extra_body 无关——
// options 只动 assistant 消息内部字段,extra_body 只动顶层键,都动不了
// messages 的条数与次序。
WireMessageMap BuildMessageWireMap(const Request& request) {
    WireMessageMap map;
    BuildRequestJson(request, json::object(), ChatRequestOptions{}, &map);
    return map;
}

}  // namespace lubancode::api::chat
