#include "agent/loop.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include "api/assembler.hpp"
#include "platform/text_encoding.hpp"  // SanitizeUtf8:工具结果入历史前的编码兜底

namespace lubancode::agent {

namespace {

// 执行一个工具调用:先通知上层要开始了,M9 的 pre_tool 钩子紧接着检查一遍
// (拦截了就直接结束,连确认都不问),needs_confirm 的话再问一句,拒绝/
// 找不到工具/被钩子拦截/正常执行,最后都会走 on_tool_done 通知一遍,保证
// 上层能看到完整的生命周期。工具真执行完之后再跑一遍 post_tool 钩子
// (M9)——这是本次任务在 agent/ 里唯一的挂接点,函数本身不知道 hooks 具体
// 怎么解析执行,只在两个该介入的地方各调一次回调。
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const Callbacks& callbacks,
                                const std::function<bool(const tools::Tool&)>& tool_filter) {
    if (callbacks.on_tool_start) {
        callbacks.on_tool_start(call.name, call.input);
    }

    tools::Tool* tool = registry.Find(call.name);
    if (tool == nullptr) {
        tools::Tool::Result result{"未知工具: " + call.name, true};
        if (callbacks.on_tool_done) {
            callbacks.on_tool_done(call.name, result);
        }
        return result;
    }

    // tool_search(延迟挂载):注册表里查得到,但过滤谓词不放行——延迟工具
    // 还没挂载。不当"未知工具"糊弄,给一条指路的友好错误,模型下一步自然
    // 去调 tool_search。
    if (tool_filter && !tool_filter(*tool)) {
        tools::Tool::Result result{"工具 " + call.name + " 存在但尚未挂载:请先用 tool_search 检索挂载,再调用。",
                                    true};
        if (callbacks.on_tool_done) {
            callbacks.on_tool_done(call.name, result);
        }
        return result;
    }

    if (callbacks.on_pre_tool_hook) {
        const std::optional<std::string> blocked = callbacks.on_pre_tool_hook(call.name, call.input);
        if (blocked.has_value()) {
            tools::Tool::Result result{*blocked, true};
            if (callbacks.on_tool_done) {
                callbacks.on_tool_done(call.name, result);
            }
            return result;
        }
    }

    if (tool->needs_confirm()) {
        const bool allowed = callbacks.on_tool_confirm ? callbacks.on_tool_confirm(call.name, call.input) : true;
        if (!allowed) {
            tools::Tool::Result result{"用户拒绝执行该工具", true};
            if (callbacks.on_tool_done) {
                callbacks.on_tool_done(call.name, result);
            }
            return result;
        }
    }

    tools::Tool::Result result = tool->execute(call.input);
    if (callbacks.on_post_tool_hook) {
        callbacks.on_post_tool_hook(call.name, call.input, result);
    }
    if (callbacks.on_tool_done) {
        callbacks.on_tool_done(call.name, result);
    }
    return result;
}

}  // namespace

AgentLoop::AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
                      std::string system_prompt, int max_tokens, int max_turns, std::size_t max_context_chars)
    : backend_(backend),
      registry_(registry),
      model_(std::move(model)),
      system_prompt_(std::move(system_prompt)),
      max_tokens_(max_tokens),
      max_turns_(max_turns),
      max_context_chars_(max_context_chars) {}

std::vector<api::ToolDefinition> AgentLoop::BuildToolDefinitions() const {
    std::vector<api::ToolDefinition> defs;
    defs.reserve(registry_.All().size());
    for (const auto& tool : registry_.All()) {
        // tool_search(延迟挂载):谓词不放行的工具(延迟且未挂载)不进
        // tools 数组。没设谓词就是全量,跟从前一样。
        if (tool_filter_ && !tool_filter_(*tool)) {
            continue;
        }
        defs.push_back(api::ToolDefinition{tool->name(), tool->description(), tool->input_schema()});
    }
    return defs;
}

std::expected<RunOutcome, std::string> AgentLoop::Run(const std::string& user_input, const Callbacks& callbacks,
                                                        const std::atomic<bool>* cancel) {
    api::Message user_message;
    user_message.role = api::Role::User;
    user_message.content.push_back(api::TextBlock{user_input});
    return Run(std::move(user_message), callbacks, cancel);
}

std::expected<RunOutcome, std::string> AgentLoop::Run(api::Message user_message, const Callbacks& callbacks,
                                                        const std::atomic<bool>* cancel) {
    if (user_message.role != api::Role::User || user_message.content.empty()) {
        return std::unexpected("用户消息为空，无法发送。");
    }
    history_.push_back(std::move(user_message));

    for (int turn = 0; turn < max_turns_; ++turn) {
        api::Request request;
        request.model = model_;
        request.system = system_prompt_;
        request.messages = TrimHistory(history_, max_context_chars_);
        request.max_tokens = max_tokens_;
        // 每轮现拼,不在 Run() 开头拼一次复用:tool_search 命中会在一次
        // Run() 中途把工具加进 loaded 集合(谓词的判定依据),下一轮请求
        // 就得带上新挂载工具的完整定义。没设谓词时,重拼出来的内容每轮
        // 一样,行为不变,只多花一点拼 JSON 的工夫。
        request.tools = BuildToolDefinitions();

        // 硬上限:轮级裁剪 + 工具结果截断都做完还是装不下(比如单条用户输入
        // 就超大),明确报错,不把一份注定被拒的超大请求发出去。
        if (EstimateChars(request.messages) > max_context_chars_) {
            return std::unexpected("上下文超过上限(" + std::to_string(max_context_chars_) +
                                    " 字符),裁剪与截断后仍装不下,无法发送。请用 /compact 压缩历史,或开新会话。");
        }

        api::MessageAssembler assembler;
        bool stream_error = false;
        std::string stream_error_message;

        const auto send_result = backend_.send_stream(
            request,
            [&](const api::StreamEvent& event) {
                assembler.Feed(event);
                std::visit(
                    [&](const auto& e) {
                        using T = std::decay_t<decltype(e)>;
                        if constexpr (std::is_same_v<T, api::TextDelta>) {
                            if (callbacks.on_text_delta) {
                                callbacks.on_text_delta(e.text);
                            }
                        } else if constexpr (std::is_same_v<T, api::StreamError>) {
                            stream_error = true;
                            stream_error_message = e.message;
                        }
                    },
                    event);
            },
            cancel);

        if (!send_result.has_value()) {
            const api::Error& err = send_result.error();
            if (err.kind == api::ErrorKind::Cancelled) {
                // ESC 打断:流被从中间掐断,ContentBlockDone/MessageDone 永远
                // 不会来了,手动把还开着的块(文本或 tool_use)收个尾,半截话
                // 也要照常攒进历史,不能悄悄丢掉。
                assembler.FinalizeOpenBlock();
                api::Message assistant_message = assembler.BuildMessage();
                assistant_message.content.push_back(api::TextBlock{"[用户按 ESC 打断了这条回答]"});
                history_.push_back(assistant_message);

                // 半截流里如果混进了没走完的 tool_use 块(硬收尾出来的,
                // input 多半是空对象或者解析失败),必须给每一个都配一条
                // tool_result——不然这条 assistant 消息下一轮重放给模型时,
                // tool_use/tool_result 配对关系就破了,API 会直接拒绝整个
                // 请求。
                std::vector<api::ContentBlock> orphan_results;
                for (const auto& block : assistant_message.content) {
                    if (std::holds_alternative<api::ToolUseBlock>(block)) {
                        const auto& call = std::get<api::ToolUseBlock>(block);
                        orphan_results.push_back(api::ToolResultBlock{call.id, "用户按 ESC 打断,该工具未执行", true});
                    }
                }
                if (!orphan_results.empty()) {
                    api::Message orphan_message;
                    orphan_message.role = api::Role::User;
                    orphan_message.content = std::move(orphan_results);
                    history_.push_back(std::move(orphan_message));
                }

                return RunOutcome{true};
            }
            return std::unexpected("请求失败: " + err.message);
        }
        if (stream_error) {
            return std::unexpected("模型返回错误: " + stream_error_message);
        }

        api::Message assistant_message = assembler.BuildMessage();
        const std::string stop_reason = assembler.stop_reason();
        history_.push_back(assistant_message);

        if (callbacks.on_usage) {
            callbacks.on_usage(assembler.usage());
        }

        // 防御:stop_reason 说的是 end_turn(或者干脆是空的——终止帧丢了),
        // 消息里却攒出了 tool_use 块。信块不信帧:照 tool_use 处理,把工具跑了、
        // 结果成对喂回去。不然历史里就留下一条没有 tool_result 配对的 tool_use,
        // 下一轮请求直接被 API 以 400 拒掉。
        bool has_tool_use = false;
        for (const auto& block : assistant_message.content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                has_tool_use = true;
                break;
            }
        }

        if (stop_reason != "tool_use" && !has_tool_use) {
            return RunOutcome{};
        }

        // 工具循环:逐个执行模型要的工具调用。cancel 中途被置位("工具已
        // 执行、结果还没发回"那个当口——正在跑的这个工具照常等它跑完、结果
        // 照常入历史;还没轮到的后续工具不再真的执行,补一条"未执行"的合成
        // 结果)保住 tool_use/tool_result 的成对约束,再从 Run() 正常返回。
        bool interrupted = false;
        std::vector<api::ContentBlock> tool_results;
        for (const auto& block : assistant_message.content) {
            if (!std::holds_alternative<api::ToolUseBlock>(block)) {
                continue;
            }
            const auto& call = std::get<api::ToolUseBlock>(block);
            if (interrupted || (cancel != nullptr && cancel->load())) {
                interrupted = true;
                tool_results.push_back(api::ToolResultBlock{call.id, "用户按 ESC 打断,该工具未执行", true});
                continue;
            }
            const tools::Tool::Result result = RunOneTool(registry_, call, callbacks, tool_filter_);
            // 边界兜底:不管工具(含 MCP、hooks 加工过的结果、读到二进制的
            // 文件工具……)自己有没有做好编码,进历史前一律过一遍
            // SanitizeUtf8——一坨坏字节不该把整轮对话崩掉(下次
            // nlohmann::json 序列化历史时,非法 UTF-8 会直接抛
            // type_error.316,异常穿透到顶层)。run_command.cpp 已经在捕获侧
            // 治过一次本,这里是最后一道拦网,双保险不重复不冲突。
            tool_results.push_back(
                api::ToolResultBlock{call.id, platform::SanitizeUtf8(result.content), result.is_error});
            if (cancel != nullptr && cancel->load()) {
                interrupted = true;
            }
        }

        api::Message tool_result_message;
        tool_result_message.role = api::Role::User;
        tool_result_message.content = std::move(tool_results);
        history_.push_back(std::move(tool_result_message));

        if (interrupted) {
            return RunOutcome{true};
        }
    }

    return std::unexpected("超过最大轮数(" + std::to_string(max_turns_) + "),已停止,避免死循环。");
}

}  // namespace lubancode::agent
