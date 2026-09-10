#include "api/responses/request.hpp"

#include <type_traits>
#include <variant>

namespace lubancode::api::responses {

namespace {

using nlohmann::json;

// Role -> wire 角色名,共用件(批六归一):responses 的另一角叫 "assistant"。
std::string WireRole(Role role) {
    return RoleToString(role, "assistant");
}

// 四角色换骨(v3 第二棒,差距清单 §8.2 第 4 条):文本部件的类型只认
// assistant 一角——assistant 的正文是 output_text,其余(user、折 user
// 侧的 tool)一律 input_text。System 不该到这层(BuildRequestJson 把它
// 顶置进顶层 instructions),防御到达也折 input_text,与 WireRole 的
// 折法一致,不造第三角。现行 User/Assistant 的输出一字不变。
const char* TextPartType(Role role) {
    return role == Role::Assistant ? "output_text" : "input_text";
}

// 没有图片的旧消息继续沿用逐块转 item 的写法，避免把既有请求形状悄悄
// 合并。含图片时另走下面的成组分支，input_text 和 input_image 才能同框。
json ContentBlockToItem(const ContentBlock& block, Role role) {
    return std::visit(
        [role](const auto& b) -> json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                return json{{"type", "message"},
                            {"role", WireRole(role)},
                            {"content", json::array({json{{"type", TextPartType(role)}, {"text", b.text}}})}};
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                return json{{"type", "message"},
                            {"role", WireRole(role)},
                            {"content", json::array({json{{"type", "input_image"},
                                                       {"image_url", "data:" + b.media_type + ";base64," + b.data}}})}};
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return json{{"type", "function_call"},
                            {"call_id", b.id},
                            {"name", b.name},
                            {"arguments", b.input.dump()}};
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                // responses wire 的 reasoning 是一次性的,不参与续会话重放。
                // 这里给一个 reasoning 占位,调用方(BuildRequestJson)会跳过。
                return json{{"type", "__thinking_skip__"}};
            } else if constexpr (std::is_same_v<T, RedactedThinkingBlock>) {
                // 加密思考块(差距清单 §8.2 第 6 条)同款一次性:不透明载荷
                // 在这副 wire 上没有可回传的形状,占位由调用方跳过。
                return json{{"type", "__thinking_skip__"}};
            } else if constexpr (std::is_same_v<T, ModelImageBlock>) {
                // 模型输出图片的替身:历史里只有 artifact 引用,重放翻成
                // 一句短文本标记(base64 绝不塞回请求,续聊不重放正文)。
                return json{{"type", "message"},
                            {"role", WireRole(role)},
                            {"content", json::array({json{{"type", TextPartType(role)},
                                                       {"text", ModelImageReplayText(b)}}})}};
            } else if constexpr (std::is_same_v<T, ServerToolUseBlock>) {
                // anthropic 原生工具搜索块(动态工具 P3)在 responses wire 的
                // 明降级:翻成一句事实文本,不悄悄丢块。
                return json{{"type", "message"},
                            {"role", WireRole(role)},
                            {"content", json::array({json{
                                {"type", TextPartType(role)},
                                {"text", "[服务端工具搜索(anthropic 原生): " + b.name + " 已由 provider 执行]"}}})}};
            } else if constexpr (std::is_same_v<T, ServerToolResultBlock>) {
                return json{{"type", "message"},
                            {"role", WireRole(role)},
                            {"content", json::array({json{
                                {"type", TextPartType(role)},
                                {"text", "[服务端工具搜索结果(anthropic 原生): " + b.content.dump() + "]"}}})}};
            } else {
                // 工具结果图片回喂(协议原生):function_call_output.output
                // 可为 "an array of image or file objects instead of a
                // string"——出处 platform.openai.com/docs/guides/
                // function-calling("Formatting results");数组元素与 user
                // 消息的输入部件同形(input_text 在前、input_image 的
                // image_url 吃 data: URL)。wire_base64 非空才上数组;
                // 空则照旧字符串投影,老钉子不红。
                json images = json::array();
                for (const auto& rich : b.blocks) {
                    if (const auto* image = std::get_if<tools::ImageContent>(&rich);
                        image != nullptr && !image->wire_base64.empty()) {
                        images.push_back(json{{"type", "input_image"},
                                              {"image_url", "data:" + image->mime_type + ";base64," +
                                                                image->wire_base64}});
                    }
                }
                json item{{"type", "function_call_output"}, {"call_id", b.tool_use_id}};
                if (images.empty()) {
                    item["output"] = b.content;
                } else {
                    json output = json::array();
                    output.push_back(json{{"type", "input_text"}, {"text", b.content}});
                    output.insert(output.end(), images.begin(), images.end());
                    item["output"] = std::move(output);
                }
                return item;
            }
        },
        block);
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, bool native_web_search, const json& extra_body,
                                WireMessageMap* wire_map) {
    json body;
    body["model"] = request.model;
    // max_output_tokens 可省略(responses 协议):unset 交服务端默认
    // (Request::max_tokens 注释,规格根因一)。显式声明了才落键。
    if (request.max_tokens.has_value()) {
        body["max_output_tokens"] = *request.max_tokens;
    }
    body["stream"] = true;
    body["store"] = false;  // 无状态:历史全靠自己带,跟 Anthropic 后端行为一致

    // 四角色换骨(v3 第二棒,差距清单 §8.2 第 4 条):System 角色消息是
    // 上下文根,顶置进顶层 instructions(§4.46 目标表),input 里一条不落。
    // Request::system(现行两角色路径的唯一入口)先行,System 消息按消息序
    // 接在其后("\n" 连接,空段不造)。System 消息只取 TextBlock——与
    // anthropic/chat 两家同一取舍,富块 system 后续棒次需要再扩。
    std::string instructions = request.system;
    for (const auto& message : request.messages) {
        if (message.role != Role::System) {
            continue;
        }
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<TextBlock>(&block);
                text != nullptr && !text->text.empty()) {
                if (!instructions.empty()) {
                    instructions += "\n";
                }
                instructions += text->text;
            }
        }
    }
    if (!instructions.empty()) {
        body["instructions"] = std::move(instructions);
    }

    // 推理参数(模型协议兼容实录矩阵单 P1 起按方言落线):
    //   - 方言声明 effort_path=reasoning.effort:档位按模型声明落 effort;
    //     手册明文 reasoning.effort 优先于旧开关 enable_thinking——effort
    //     落了线,旧开关就不发(退役键,少一个是一个);off 档走
    //     reasoning.effort="none"(手册:枚举里有 none=关闭思考)。
    //   - 方言声明 toggle=enable_thinking_bool 且模型声明了 toggle、又没
    //     有 effort 档可落:落顶层布尔(旧开关路径)。
    //   - 没方言:legacy(实测 MiniMax-M3 responses 端点四档全透传,
    //     HTTP 200、reasoning_tokens 随档位递增,不做档位限制/回退)。
    if (!request.reasoning_effort.empty()) {
        const auto& dialect = request.reasoning.dialect;
        const bool off = ReasoningEffortIsOff(request.reasoning_effort, request.reasoning);
        const bool effort_ok = request.reasoning.empty() || request.reasoning.supports_effort;

        bool effort_written = false;
        if (dialect.effort_path == "reasoning.effort" && effort_ok) {
            body["reasoning"] = json{{"effort", request.reasoning_effort}};
            effort_written = true;
        } else if (dialect.empty() && effort_ok) {
            body["reasoning"] = json{{"effort", request.reasoning_effort}};
            effort_written = true;
        }
        if (!effort_written && dialect.toggle == "enable_thinking_bool" &&
            request.reasoning.supports_toggle) {
            // 没有档位形状只有开关形状的端:旧开关顶层布尔。
            body["enable_thinking"] = !off;
        }
        if (dialect.empty() && request.reasoning.supports_toggle) {
            // legacy 兼容路径:改动前的行为不动(effort 与 thinking.type
            // 双键,哪怕 effort 档没被模型声明)。
            body["thinking"] = json{{"type", off ? "disabled" : "enabled"}};
        }
    }

    json input = json::array();
    // 拍平对照(差距清单 §8.2 第 7 条):responses 逐块成 item——一条内部
    // 消息可裂成多个 item(正文 message + 每枚工具调用/结果各一个),
    // 思考块(含加密思考)被跳过后整条没剩东西的就是空对照。
    if (wire_map != nullptr) {
        wire_map->container = "input";
        wire_map->message_to_wire.assign(request.messages.size(), {});
    }
    for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index) {
        const auto& message = request.messages[message_index];
        // System 角色已顶置进 instructions,input 里一条不落(不重复注入)。
        if (message.role == Role::System) {
            continue;
        }
        bool has_image = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<ImageBlock>(block)) {
                has_image = true;
                break;
            }
        }
        if (!has_image) {
            for (const auto& block : message.content) {
                if (std::holds_alternative<ThinkingBlock>(block) ||
                    std::holds_alternative<RedactedThinkingBlock>(block)) {
                    continue;  // 思考块不回传:responses wire 的 reasoning 是一次性的
                }
                if (wire_map != nullptr) {
                    wire_map->message_to_wire[message_index].push_back(input.size());
                }
                input.push_back(ContentBlockToItem(block, message.role));
            }
            continue;
        }

        // Responses 的 input_image 必须跟 input_text 一样塞进 message.content。
        // 遇到工具块才把已经攒着的普通内容吐成一条 message，守住块顺序。
        json content = json::array();
        const auto flush_content = [&] {
            if (content.empty()) {
                return;
            }
            if (wire_map != nullptr) {
                wire_map->message_to_wire[message_index].push_back(input.size());
            }
            input.push_back(json{{"type", "message"}, {"role", WireRole(message.role)}, {"content", content}});
            content = json::array();
        };
        for (const auto& block : message.content) {
            std::visit(
                [&](const auto& b) {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, TextBlock>) {
                        content.push_back(json{{"type", TextPartType(message.role)},
                                               {"text", b.text}});
                    } else if constexpr (std::is_same_v<T, ImageBlock>) {
                        content.push_back(json{{"type", "input_image"},
                                               {"image_url", "data:" + b.media_type + ";base64," + b.data}});
                    } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                        // 思考块不回传:responses wire 的 reasoning 是一次性的
                    } else if constexpr (std::is_same_v<T, RedactedThinkingBlock>) {
                        // 加密思考块(差距清单 §8.2 第 6 条)同款一次性,
                        // 不透明载荷没有可回传的形状。
                    } else if constexpr (std::is_same_v<T, ModelImageBlock>) {
                        // 图片引用翻短文本标记,与成组分支同规矩。
                        content.push_back(json{{"type", TextPartType(message.role)},
                                               {"text", ModelImageReplayText(b)}});
                    } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                        flush_content();
                        if (wire_map != nullptr) {
                            wire_map->message_to_wire[message_index].push_back(input.size());
                        }
                        input.push_back(ContentBlockToItem(block, message.role));
                    } else {
                        flush_content();
                        if (wire_map != nullptr) {
                            wire_map->message_to_wire[message_index].push_back(input.size());
                        }
                        input.push_back(ContentBlockToItem(block, message.role));
                    }
                },
                block);
        }
        flush_content();
    }
    if (wire_map != nullptr) {
        wire_map->wire_element_count = input.size();
    }
    body["input"] = input;

    // native_web_search 是服务端原生能力声明,跟 request.tools(本地函数
    // 工具)是两码事——就算本地工具表是空的,只要开关开着也要能声明,所以
    // 这里不能再用 "!request.tools.empty()" 当建不建 tools 字段的唯一门槛。
    if (!request.tools.empty() || native_web_search) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{
                {"type", "function"},
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", ToolSchemaForWire(tool.input_schema)},
            });
        }
        if (native_web_search) {
            tools.push_back(json{{"type", "web_search"}});
        }
        body["tools"] = tools;
    }

    // extra_body 永远在最后合并(见 client.hpp 里 BuildRequestJson 的注释):
    // 键冲突时整个覆盖前面算出来的值,只做顶层浅合并,不做深合并(共用件
    // api::MergeExtraBody,provider 级先、variant 级后)。
    MergeExtraBody(body, extra_body);
    MergeExtraBody(body, request.extra_body);

    return body;
}

// 拍平对照(差距清单 §8.2 第 7 条):与 BuildRequestJson 同一条拼装路
// 产出(第四参传指针共用)。消息拍平与 native_web_search/extra_body 无关
//(那只动顶层键)。
WireMessageMap BuildMessageWireMap(const Request& request) {
    WireMessageMap map;
    BuildRequestJson(request, /*native_web_search=*/false, json::object(), &map);
    return map;
}

}  // namespace lubancode::api::responses
