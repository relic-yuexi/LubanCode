#include "api/anthropic/client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <optional>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "api/anthropic/events.hpp"
#include "api/http_stream_transport.hpp"  // 批六:PostSseStream/DumpRequestBody,四家共用的传输骨架
#include "api/sse_framing.hpp"
#include "platform/log_sink.hpp"  // BuildThinkingJson 的档位警告仍在这边打

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
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                return json{{"type", "image"},
                            {"source", json{{"type", "base64"}, {"media_type", b.media_type}, {"data", b.data}}}};
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                json j{{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
                // PTC 的调用方标识(动态工具 P3·§7.2):wire 给过的照原样带回,
                // 没给不造字段。
                if (!b.caller.empty()) {
                    j["caller"] = b.caller;
                }
                return j;
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                // 工具结果图片回喂(协议原生):tool_result 的 content 可以是
                // 块数组,内嵌 image 块——出处 platform.claude.com/docs/en/
                // agents-and-tools/tool-use/implement-tool-use("content ...
                // a list of nested content blocks ... can use the text, image,
                // or document types");computer use 的截图回喂就是走这条路。
                // 上限(vision 文档):单图 10MB base64、8000x8000、四 MIME,
                // 重灌侧的帽(5MiB/8000px)比它更紧,这里不再复查。
                // wire_base64 非空(重灌成功)才上数组,文本投影放第一块;
                // 空则走旧字符串投影——与未重灌的从前逐字节一致,老钉子不红。
                json images = json::array();
                for (const auto& rich : b.blocks) {
                    if (const auto* image = std::get_if<tools::ImageContent>(&rich);
                        image != nullptr && !image->wire_base64.empty()) {
                        images.push_back(json{
                            {"type", "image"},
                            {"source", json{{"type", "base64"},
                                             {"media_type", image->mime_type},
                                             {"data", image->wire_base64}}}});
                    }
                }
                json j{{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}};
                if (images.empty()) {
                    j["content"] = b.content;
                } else {
                    json content = json::array();
                    content.push_back(json{{"type", "text"}, {"text", b.content}});
                    content.insert(content.end(), images.begin(), images.end());
                    j["content"] = std::move(content);
                }
                if (b.is_error) {
                    j["is_error"] = true;
                }
                return j;
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                // 续会话重放历史时 thinking 块必须带 signature,否则第二轮
                // 会被服务端以 400 拒掉。
                return json{{"type", "thinking"}, {"thinking", b.text}, {"signature", b.signature}};
            } else if constexpr (std::is_same_v<T, ModelImageBlock>) {
                // 模型输出图片的替身:引用翻短文本标记,base64 不回传。
                return json{{"type", "text"}, {"text", ModelImageReplayText(b)}};
            } else if constexpr (std::is_same_v<T, ServerToolUseBlock>) {
                // 服务端工具搜索(动态工具 P3·§7.2):provider 执行过的调用事实,
                // 下一轮原样回传(官方要求 assistant content 不改,含这对块)。
                // 绝不给它的 srvtoolu_ id 配 tool_result——服务端会 400。
                return json{{"type", "server_tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
            } else if constexpr (std::is_same_v<T, ServerToolResultBlock>) {
                // 搜索结果块:嵌套 content(含 tool_reference / error)原样回传,
                // 不压文本(单子红线 9:不把原生块压成普通文字假称支持)。
                return json{{"type", "tool_search_tool_result"},
                            {"tool_use_id", b.tool_use_id},
                            {"content", b.content}};
            }
        },
        block);
}

// M6.6/M10:think 档位 -> Anthropic 风格 thinking 参数。实测(MiniMax-M3 真实
// anthropic 兼容端点 /anthropic/v1/messages)确认支持
// {"type":"enabled","budget_tokens":N} / {"type":"disabled"},HTTP 200,
// 且 enabled 时真的返回 thinking 内容块——所以这里走"接受并映射"这条路,
// 不是"协议不支持,打警告跳过"那条路。
// M10:config/命令行层不再限死档位取值(responses wire 原样透传任意字符串,
// 由服务商自己判断合不合法);但 anthropic wire 走的是自家的 thinking 参数,
// 只认 budget_tokens 这个数字,没法"原样透传"一个任意字符串,所以内置一张
// 映射表:none/low/medium/high/xhigh/max。档位 -> budget_tokens 的具体数字
// 是任务明确交给这里自己定的一个设计选择(原话"档位→budget 映射你定"),
// 选的是 1k/4k/16k/32k/48k 这组常见量级,越往上思考预算越宽裕;唯一的硬
// 约束是 Anthropic 要求 budget_tokens 必须小于 max_tokens(不然思考预算比
// 整个回复上限还大,没意义、也可能被端点拒绝),所以这里按
// request.max_tokens 兜底夹一下,budget 超过 max_tokens 时退化成
// "max_tokens 留 256 给正文,剩下全给思考",绝不出现 budget >= max_tokens
// 的组合。映射表之外的档位名字(比如用户在 responses wire 下用惯了、后来
// 切到 anthropic wire 的自定义字符串):不报错、不崩,只打一行警告到
// stderr,当没设置处理(不发 thinking 字段),留给上层继续跑完这一轮。
// 新式 Claude Messages 用 adaptive thinking + output_config.effort(P1 起
// 由方言声明;旧目录的 wireDialect=="effort" 兼容折算)。
bool UsesAdaptiveEffort(const Request& request) {
    return request.reasoning.dialect.effort_path == "output_config.effort" ||
           request.reasoning.wire_dialect == "effort";
}

std::optional<json> BuildThinkingJson(const Request& request) {
    if (request.reasoning_effort.empty()) {
        return std::nullopt;
    }
    // 巡检单 P2:目录明说不吃推理的模型(纯生成类)不发 thinking——
    // legacy 路径(档案空)照旧,显式 declined 的整个字段缺席。
    if (request.reasoning.declined) {
        return std::nullopt;
    }
    const std::string lower = LowerReasoningEffort(request.reasoning_effort);
    if (ReasoningEffortIsOff(lower, request.reasoning)) {
        return json{{"type", "disabled"}};
    }

    // output_config 在 BuildRequestJson 里写；这里先落 thinking 开关。
    if (UsesAdaptiveEffort(request)) {
        return json{{"type", "adaptive"}};
    }

    const bool known_legacy = lower == "low" || lower == "medium" || lower == "high" ||
                              lower == "xhigh" || lower == "extra" || lower == "max" ||
                              lower == "auto";
    if (request.reasoning.empty() && !known_legacy) {
        platform::LogSink::Instance().Warn(
            "anthropic", "协议下无此档映射,已忽略: " + request.reasoning_effort);
        return std::nullopt;
    }

    // max_tokens 必填(anthropic):unset 时落公开兜底(见
    // kRequiredMaxOutputTokensFallback 注释),预算夹逼拿兜底后的有效值算。
    const int effective_max_tokens = request.max_tokens.value_or(kRequiredMaxOutputTokensFallback);

    const int budget = ReasoningBudgetForEffort(request.reasoning, lower, effective_max_tokens);

    return json{{"type", "enabled"}, {"budget_tokens", budget}};
}

}  // namespace

// 拼出 Anthropic Messages API 的请求体(stream: true 恒开,M1 只走流式)。
// 声明在 client.hpp 里,单测用;线上代码路径(send_stream)也是调这个函数。
json BuildRequestJson(const Request& request, bool native_web_search, const json& extra_body) {
    json body;
    body["model"] = request.model;
    // max_tokens 是 anthropic 协议的必填字段:unset 时落公开兜底
    // (kRequiredMaxOutputTokensFallback,三级声明都缺席才轮到它),不藏
    // 魔数;想改上限走配置 agent.max_output_tokens 或目录声明。
    body["max_tokens"] = request.max_tokens.value_or(kRequiredMaxOutputTokensFallback);
    body["stream"] = true;

    if (!request.system.empty()) {
        body["system"] = request.system;
    }

    if (const auto thinking = BuildThinkingJson(request); thinking.has_value()) {
        body["thinking"] = *thinking;
    }
    if (!request.reasoning_effort.empty() && UsesAdaptiveEffort(request) &&
        !ReasoningEffortIsOff(request.reasoning_effort, request.reasoning)) {
        std::string effort = LowerReasoningEffort(request.reasoning_effort);
        if (effort == "extra") effort = "xhigh";
        body["output_config"] = json{{"effort", effort}};
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

    // native_web_search 是服务端原生能力声明,跟 request.tools(本地函数
    // 工具)是两码事——就算本地工具表是空的,只要开关开着也要能声明,所以
    // 这里不能再用 "!request.tools.empty()" 当建不建 tools 字段的唯一门槛
    // (跟 Responses 那边 M12 的改法同一个道理)。server_tool_search(动态
    // 工具 P3)同一条道理:它是请求级的模型能力声明,空串 = 不声明。
    if (!request.tools.empty() || native_web_search || !request.server_tool_search.empty()) {
        json tools = json::array();
        std::size_t eager_tools = 0;
        for (const auto& tool : request.tools) {
            json item{
                {"name", tool.name},
                {"description", tool.description},
                {"input_schema", ToolSchemaForWire(tool.input_schema)},
            };
            // 动态工具 P3(§7.1):Deferred 定义照发全文(provider 要拿它跑搜索、
            // 展开 tool_reference),只标 defer_loading:true——服务端把这批定义
            // 排除在缓存前缀外,发现/调用都不破前缀。Eager 定义不带这个字段,
            // 与从前逐字节一致。官方约束:至少一枚非 deferred 工具(核心工具
            // 恒 eager,天然满足);deferred 定义不得再挂 cache_control——本仓
            // 从不在工具上发 cache_control,无冲突。
            if (tool.load_mode == ToolLoadMode::Deferred) {
                item["defer_loading"] = true;
            } else {
                ++eager_tools;
            }
            tools.push_back(std::move(item));
        }
        if (native_web_search) {
            // Anthropic 的 server tool 声明形状跟本地函数工具不一样:只有
            // type + name 两个字段,没有 description/input_schema。type 里
            // 那串数字是日期版本号——web_search_20260209 是 2026-02-09
            // 随 Claude 4.6 发布的当前 GA 版本(支持 dynamic filtering),
            // 官方文档确认基础用法(不启用 code execution 动态过滤)不需要
            // 额外 beta header,直接可用;旧版本 web_search_20250305 仍受
            // 支持,但新请求没有理由不用当前版本。
            tools.push_back(json{{"type", "web_search_20260209"}, {"name", "web_search"}});
            ++eager_tools;
        }
        if (!request.server_tool_search.empty()) {
            // 服务端工具搜索(动态工具 P3):regex 与 bm25 是同日发布的两个
            // 算法变体,不是新旧版(官方 Tool reference:Variant, not
            // version)。变体由模型目录声明(deferred_tools.server_tool_search),
            // 不在这头猜;声明形状与 web_search 同款(type + name,无
            // schema),搜索工具自身绝不 defer_loading(官方明令)。
            if (request.server_tool_search == "bm25") {
                tools.push_back(
                    json{{"type", "tool_search_tool_bm25_20251119"}, {"name", "tool_search_tool_bm25"}});
            } else {
                tools.push_back(
                    json{{"type", "tool_search_tool_regex_20251119"}, {"name", "tool_search_tool_regex"}});
            }
            ++eager_tools;
        }
        // 防御(官方 400 合同 "At least one tool must have defer_loading
        // =false. All tools cannot be deferred."):全表 deferred 时把首枚降回
        // eager——正常装配不会走到这(核心工具恒 eager),纯工具表被外部
        // 掏空的场合也别发一份必被拒的请求。
        if (eager_tools == 0 && !tools.empty()) {
            tools.front().erase("defer_loading");
        }
        body["tools"] = tools;
    }

    // extra_body 永远在最后合并:所有内置逻辑(thinking/native_web_search/
    // messages/tools)都拼完了,extra_body 里的值才浅合并进来、键冲突时
    // 整个覆盖掉前面算出来的值——这样用户才能靠它压过内置的 thinking 字段
    // (比如 GLM 那种既要 thinking.type 又要自定义 reasoning_effort 档位的
    // 写法)。只做顶层浅合并,不做深合并(嵌套 object 整个替换,不逐键钻
    // 进去比),保持行为简单、可预期。
    if (extra_body.is_object()) {
        for (auto it = extra_body.begin(); it != extra_body.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    if (request.extra_body.is_object()) {
        for (auto it = request.extra_body.begin(); it != request.extra_body.end(); ++it) {
            body[it.key()] = it.value();
        }
    }

    return body;
}

bool ShouldRecoverTaggedThinking(const Request& request) {
    if (request.messages.size() < 2) {
        return false;
    }
    const Message& tool_result_message = request.messages.back();
    if (tool_result_message.role != Role::User ||
        std::none_of(tool_result_message.content.begin(), tool_result_message.content.end(),
                     [](const ContentBlock& block) { return std::holds_alternative<ToolResultBlock>(block); })) {
        return false;
    }

    const Message& assistant = request.messages[request.messages.size() - 2];
    if (assistant.role != Role::Assistant) {
        return false;
    }
    const bool has_thinking =
        std::any_of(assistant.content.begin(), assistant.content.end(),
                    [](const ContentBlock& block) { return std::holds_alternative<ThinkingBlock>(block); });
    const bool has_tool_use =
        std::any_of(assistant.content.begin(), assistant.content.end(),
                    [](const ContentBlock& block) { return std::holds_alternative<ToolUseBlock>(block); });
    return has_thinking && has_tool_use;
}

std::map<std::string, std::string> ApplyExtraHeaders(std::map<std::string, std::string> base,
                                                        const std::map<std::string, std::string>& extra_headers) {
    for (const auto& [name, value] : extra_headers) {
        if (value.empty()) base.erase(name);
        else base[name] = value;
    }
    return base;
}

AnthropicBackend::AnthropicBackend(std::string base_url, std::string auth_token, int connect_timeout_ms,
                                    int stream_idle_timeout_secs, bool native_web_search,
                                    nlohmann::json extra_body, std::map<std::string, std::string> extra_headers,
                                    int request_hard_timeout_secs)
    : base_url_(std::move(base_url)),
      auth_token_(std::move(auth_token)),
      connect_timeout_ms_(connect_timeout_ms),
      stream_idle_timeout_secs_(stream_idle_timeout_secs),
      native_web_search_(native_web_search),
      extra_body_(std::move(extra_body)),
      extra_headers_(std::move(extra_headers)),
      request_hard_timeout_secs_(request_hard_timeout_secs) {}

std::expected<void, Error> AnthropicBackend::send_stream(
    const Request& request,
    const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    const json body = BuildRequestJson(sanitized_request, native_web_search_, extra_body_);
    const std::string body_str = DumpRequestBody("anthropic", body);
    // 动态工具 P3:本请求声明了 server_tool_search 才解析服务端搜索的原生块
    // (server_tool_use / tool_search_tool_result)——没声明的流照旧行为。
    const bool parse_server_tool_search = !sanitized_request.server_tool_search.empty();

    // 2xx 响应体 -> 分帧 -> 事件。终止事件/流错误两枚标志给收尾那段检查:
    // 流走完却一枚没见着,按协议错误报(见函数尾注释)。
    SseFramer framer;
    EventParser parser(ShouldRecoverTaggedThinking(sanitized_request), parse_server_tool_search);
    bool saw_message_done = false;
    bool saw_stream_error = false;
    bool tagged_thinking_warning_logged = false;
    const StreamDataSink sink = [&](std::string_view data) -> bool {
        for (const SseFrame& frame : framer.feed(data)) {
            for (auto& event : parser.Consume(frame)) {
                if (std::holds_alternative<MessageDone>(event)) {
                    saw_message_done = true;
                } else if (std::holds_alternative<StreamError>(event)) {
                    saw_stream_error = true;
                }
                on_event(event);
            }
            if (parser.recovered_tagged_thinking() && !tagged_thinking_warning_logged) {
                tagged_thinking_warning_logged = true;
                platform::LogSink::Instance().Warn(
                    "anthropic",
                    "兼容端把工具续轮思考塞进了 text_delta；已隔离原始 <think> 段，只保留正文");
            }
        }
        // 单帧超过上限,协议已不可信:返回 false 让传输层掐断。
        return !framer.overflowed();
    };

    // extra_headers 覆盖/追加到基础头上(ApplyExtraHeaders 是纯函数,单测
    // 直接调);cpr::Header 本身大小写不敏感,同名 key 再赋值一次就是覆盖,
    // 包括 Authorization——用户自己对后果负责。鉴权三态:auth_token 空
    // (无鉴权)时基础头里压根没有 Authorization,不发空 Bearer。
    HttpStreamCall call;
    call.url = base_url_ + "/v1/messages";
    call.headers = ApplyExtraHeaders(RequestBaseHeaders(auth_token_), extra_headers_);
    call.body = body_str;
    call.connect_timeout_ms = connect_timeout_ms_;
    call.stream_idle_timeout_secs = stream_idle_timeout_secs_;
    call.request_hard_timeout_secs = request_hard_timeout_secs_;

    auto streamed = PostSseStream(call, sink, cancel);
    if (!streamed.has_value()) {
        // 动态工具 P3(§7.3 原生失败便明退):目录声明了能力、端点却不认
        // defer_loading / server tool search 时,错误信息里点名这两枚词——
        // 报清是端点能力缺失,不是请求拼坏;指路:换模型/端点,或把
        // deferred_tool_mode 退回 proxy_reference(下一轮生效,本轮不自动改
        // 发,免得一轮被执行两遍)。
        if (parse_server_tool_search) {
            api::Error err = std::move(streamed.error());
            const std::string& message = err.message;
            const bool mentions_capability =
                message.find("defer_loading") != std::string::npos ||
                message.find("tool_search_tool") != std::string::npos ||
                message.find("tool_reference") != std::string::npos ||
                message.find("server_tool_use") != std::string::npos;
            if (mentions_capability) {
                err.message = message + "\n[原生工具搜索] 端点(" + request.model +
                              ")拒绝 defer_loading/服务端工具搜索声明:目录声明与端点实际能力不符。"
                              "本次请求不自动改发;可换端点/模型,或将配置 deferred_tool_mode 改为 "
                              "proxy_reference(下一轮生效,切换会开新 cache epoch)。";
            }
            return std::unexpected(std::move(err));
        }
        return std::unexpected(std::move(streamed.error()));
    }

    for (auto& event : parser.Finish()) {
        if (std::holds_alternative<StreamError>(event)) {
            saw_stream_error = true;
        }
        on_event(event);
    }

    // 流"正常"走完却没等到 MessageDone(message_delta):终止帧丢了或被当坏帧
    // 跳过了。这时 assembler 攒出来的消息不完整(stop_reason 是空的),当成功
    // 返回的话,上层会把半截消息当 end_turn 入历史——里头若有 tool_use,下一轮
    // 请求就 400。宁可明确报错。saw_stream_error 时不报:错误事件本身已经把
    // "这条流失败了"传给上层了。
    if (!saw_message_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到消息终止事件,响应不完整", 0});
    }

    return {};
}

// 诊断模式的 wire 序列化(问题 9):与 send_stream 同一条拼装路(清洗 +
// 同参数 BuildRequestJson + 同一只 dump)。只在 LUBANCODE_DEBUG_PREFIX
// 打开时被调用。
std::string AnthropicBackend::SerializeForDiagnostics(const Request& request) const {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    return DumpRequestBody("anthropic", BuildRequestJson(sanitized_request, native_web_search_, extra_body_));
}

}  // namespace lubancode::api::anthropic
