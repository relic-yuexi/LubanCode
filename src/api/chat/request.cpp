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

bool HasToolUse(const Message& message) {
    for (const auto& block : message.content) {
        if (std::holds_alternative<ToolUseBlock>(block)) {
            return true;
        }
    }
    return false;
}

// 一条"真正的用户输入"消息:role 是 User 且内容里有 Text/Image——区别于
// 同样顶着 User 角色、全是 ToolResultBlock 的"把工具结果喂回去"中间消息。
// (agent/context.cpp 等处的同名语义,这里独立一份,语义钉死在注释里。)
bool IsUserTurnStart(const Message& message) {
    if (message.role != Role::User) {
        return false;
    }
    for (const auto& block : message.content) {
        if (std::holds_alternative<TextBlock>(block) || std::holds_alternative<ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

// user-to-user 交互段划分 + 工具段标记:segment_has_tool_use[i] 为真表示
// 第 i 条消息所在的交互段(从一条真 user 输入到下一条真 user 输入之前)
// 里有任何 assistant 消息发起过工具调用。reasoning 回传(tool_episode
// 策略)只认这个标记——单条消息有没有 ToolUseBlock 不够,得看整段交互
// 是否启用了工具(DeepSeek 规矩:走过工具的交互段,后续请求须完整回传
// 相关 reasoning_content,到下一条 user 轮仍保留)。
std::vector<bool> SegmentToolUseFlags(const std::vector<Message>& messages) {
    std::vector<bool> flags(messages.size(), false);
    bool segment_has_tool = false;
    std::size_t segment_begin = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i > segment_begin && IsUserTurnStart(messages[i])) {
            // 新一段开始:给刚结束的那段落标记。
            for (std::size_t j = segment_begin; j < i; ++j) {
                flags[j] = segment_has_tool;
            }
            segment_begin = i;
            segment_has_tool = false;
        }
        if (messages[i].role == Role::Assistant && HasToolUse(messages[i])) {
            segment_has_tool = true;
        }
    }
    for (std::size_t j = segment_begin; j < messages.size(); ++j) {
        flags[j] = segment_has_tool;
    }
    return flags;
}

// 回传策略的最终裁决(Kimi 保留式思考单 P0,决策次序见该单 §5.3):
//   1. request.reasoning.dialect 有正式声明 -> 用 model-resolved
//      replay/replay_field(目录可识别模型的唯一真相);
//   2. 方言为空(手写旧 provider)-> 回落 ChatRequestOptions;
//   3. 两边都没写 -> Never。
// 不按模型字符串猜:同一模型可能经直连、聚合端、本地 vLLM 出站,协议
// 责任随实际绑定 provider 声明的方言走。字段名同裁决:方言 replay_field
// 压过 legacy reasoning_replay_field,空 = reasoning_content。
struct ResolvedReplay {
    ReasoningReplayPolicy policy = ReasoningReplayPolicy::Never;
    std::string field;
};

ResolvedReplay ResolveReplay(const Request& request, const ChatRequestOptions& options) {
    ResolvedReplay out;
    if (!request.reasoning.dialect.empty()) {
        const std::string& replay = request.reasoning.dialect.replay;
        out.policy = replay == "always" ? ReasoningReplayPolicy::Always
                      : replay == "tool_episode" ? ReasoningReplayPolicy::ToolEpisode
                                                 : ReasoningReplayPolicy::Never;
        out.field = request.reasoning.dialect.replay_field;
        // K2.6 开 history all(P1):replay 同步升 Always。客户端带历史
        // reasoning 与服务端 thinking.keep 是同一份契约的两半——只发 keep
        // 不带历史,或只带历史不发 keep,都不算跨轮 Preserved Thinking。
        // 只认方言声明了请求控制(thinking_keep)的模型;K3/K2.7 的固定
        // always 本来就是全量回传,K2.5 与无方言的旧 provider 一概不升。
        if (request.reasoning_history == ReasoningHistoryMode::All &&
            request.reasoning.dialect.history_control == "thinking_keep") {
            out.policy = ReasoningReplayPolicy::Always;
        }
        return out;
    }
    out.policy = options.reasoning_replay;
    out.field = options.reasoning_replay_field;
    return out;
}

// history all 的落线判定(P1):模型方言声明了 thinking_keep 形状、用户
// 显式选了 All、且思考没被关掉(关了思考还要保留,是配置入口已明报的
// 冲突;序列化器没有报错通道,到这里只剩"开思考+要保留"一种合法组合,
// 防御性的关闭态不发 keep——发了也是自相矛盾的请求)。
bool WantsKeepAll(const Request& request) {
    return request.reasoning_history == ReasoningHistoryMode::All &&
           request.reasoning.dialect.history_control == "thinking_keep" &&
           !ReasoningEffortIsOff(request.reasoning_effort, request.reasoning);
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, const nlohmann::json& extra_body,
                                const ChatRequestOptions& options) {
    json body{{"model", request.model}, {"stream", true}};
    // max_tokens 可省略(chat 协议):unset 就整个不带字段,交服务端/模型
    // 默认——vLLM 这类端的默认上限远大于旧版写死的 4096,reasoning 模型
    // 思考不至于一步撞墙(规格根因一)。显式声明了才落键。
    if (request.max_tokens.has_value()) {
        body["max_tokens"] = *request.max_tokens;
    }

    json messages = json::array();
    if (!request.system.empty()) {
        messages.push_back(json{{"role", "system"}, {"content", request.system}});
    }

    // 回传策略裁决(方言优先,legacy 回落),tool_episode 才要算段标记。
    const ResolvedReplay replay = ResolveReplay(request, options);
    const std::vector<bool> segment_tool_use =
        replay.policy == ReasoningReplayPolicy::ToolEpisode
            ? SegmentToolUseFlags(request.messages)
            : std::vector<bool>{};

    for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index) {
        const auto& message = request.messages[message_index];
        if (message.role == Role::User) {
            std::string text = JoinedText(message);
            const bool has_image = HasImage(message);
            if (!text.empty() || has_image) {
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
        // reasoning 回传:策略裁决(方言优先,legacy 回落)说了算。
        //   tool_episode:这段交互走了工具,段内 assistant 的思考按原字节
        //   回传——字段名按 provider 声明走(默认 reasoning_content,
        //   DeepSeek 协议;vLLM/Qwen 这类端声明成 reasoning),一条消息只写
        //   一份(多枚 tool call 也不拆不重),不混进 content。纯对话段照旧
        //   略过。
        //   always:工作视图里每条原始 assistant 只要带 ThinkingBlock 就回传
        //   (Kimi K3/K2.7 Preserved Thinking:完整 assistant message 原样
        //   送回,消息在,配套 reasoning 就得在),纯对话/工具/总结一视同仁;
        //   多枚思考块按块序原字节拼接,只落一枚字段;没思考不造空串,也不
        //   凭正文 <think> 猜——只有 provider 正式 reasoning 字段攒出的块
        //   才算数。
        const bool replay_reasoning =
            replay.policy == ReasoningReplayPolicy::Always ||
            (replay.policy == ReasoningReplayPolicy::ToolEpisode &&
             message_index < segment_tool_use.size() && segment_tool_use[message_index]);
        if (replay_reasoning) {
            const std::string reasoning = JoinedThinking(message);
            if (!reasoning.empty()) {
                const std::string field =
                    replay.field.empty() ? std::string("reasoning_content") : replay.field;
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
        messages.push_back(std::move(assistant));
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

    // K2.6 history all 的请求字段(P1):thinking.keep 与 type 同发。档位
    // 空着(未设档 = 服务端默认开思考)也要发——跨轮保留不依赖档位声明,
    // 只有"方言声明了 thinking_keep + 用户显式选 All + 思考没关"才落线。
    // 不走 extra_body 硬塞:那是 provider 级浅覆盖,切到不该发 thinking 的
    // 模型仍会带着,恰是本单要堵的漏。
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

}  // namespace lubancode::api::chat
