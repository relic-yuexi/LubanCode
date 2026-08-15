#include "agent/loop.hpp"

#include <algorithm>
#include <iostream>
#include <type_traits>
#include <utility>
#include <variant>

#include "agent/prefix.hpp"
#include "api/assembler.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:工具结果的第一道编码关口

namespace lubancode::agent {

namespace {

// 执行一个工具调用:先通知上层要开始了,M9 的 pre_tool 钩子紧接着检查一遍
// (拦截了就直接结束,连确认都不问),needs_confirm 的话再问一句,拒绝/
// 找不到工具/被钩子拦截/正常执行,最后都会走 on_tool_done 通知一遍,保证
// 上层能看到完整的生命周期。工具真执行完之后再跑一遍 post_tool 钩子
// (M9)——这是本次任务在 agent/ 里唯一的挂接点,函数本身不知道 hooks 具体
// 怎么解析执行,只在两个该介入的地方各调一次回调。
//
// 编码信任边界:所有工具结果(内置工具、MCP、插件、post hook 加工过的)
// 在交给任何消费者之前,先在这里过一遍 SanitizeExternalText——此前只有入
// history 前清洗一次,on_tool_done 背后的 recorder/转录/Ctrl+E 拿到的还是
// 原文,坏字节照样能把它们的 JSON 序列化打崩(见 todos/工具输出非法UTF8
// 导致会话退出.todo)。规范化之后,recorder、转录、history、请求/会话
// JSON 拿到的都是同一份内容。
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const Callbacks& callbacks,
                                const std::function<bool(const tools::Tool&)>& tool_filter) {
    // 每条收尾路共用的分发口:先清洗,再 on_tool_done,清洗版随返回值交给
    // 调用方(进 history / 下一轮请求)。日志只记字段名、长度、坏字节位置
    // 与前后几个十六进制字节,不倒正文。
    const auto dispatch_done = [&callbacks](const std::string& name, tools::Tool::Result result) {
        if (!platform::IsValidUtf8(result.content)) {
            std::cerr << "[utf8] " << platform::DescribeUtf8Issue("tool_result:" + name, result.content) << "\n";
            result.content = platform::SanitizeExternalText(result.content);
        }
        if (callbacks.on_tool_done) {
            callbacks.on_tool_done(name, result);
        }
        return result;
    };

    if (callbacks.on_tool_start) {
        callbacks.on_tool_start(call.name, call.input);
    }

    tools::Tool* tool = registry.Find(call.name);
    if (tool == nullptr) {
        return dispatch_done(call.name, tools::Tool::Result{"未知工具: " + call.name, true});
    }

    // tool_search(延迟挂载):注册表里查得到,但过滤谓词不放行——延迟工具
    // 还没挂载。不当"未知工具"糊弄,给一条指路的友好错误,模型下一步自然
    // 去调 tool_search。
    if (tool_filter && !tool_filter(*tool)) {
        return dispatch_done(call.name, tools::Tool::Result{
                                            "工具 " + call.name + " 存在但尚未挂载:请先用 tool_search 检索挂载,再调用。",
                                            true});
    }

    if (callbacks.on_pre_tool_hook) {
        const std::optional<std::string> blocked = callbacks.on_pre_tool_hook(call.name, call.input);
        if (blocked.has_value()) {
            return dispatch_done(call.name, tools::Tool::Result{*blocked, true});
        }
    }

    if (tool->needs_confirm()) {
        const bool allowed = callbacks.on_tool_confirm ? callbacks.on_tool_confirm(call.name, call.input) : true;
        if (!allowed) {
            return dispatch_done(call.name, tools::Tool::Result{"用户拒绝执行该工具", true});
        }
    }

    tools::Tool::Result result = tool->execute(call.input);
    if (callbacks.on_post_tool_hook) {
        callbacks.on_post_tool_hook(call.name, call.input, result);
    }
    return dispatch_done(call.name, std::move(result));
}

// 步数将尽提醒的正文。remaining_steps 是"从当步(含)到硬上限还能走几步"
// (ShouldNudgeStepLimit 判断为真时才会调这个,所以 remaining_steps 必然
// <= kStepLimitNudgeThreshold)。附在 system 尾部而不是塞进 history——只影响
// 当步请求怎么发,不污染对话历史(下一步 remaining_steps 变了,提示文本也
// 该跟着变,history 里留一份旧提示没有意义)。
std::string BuildStepLimitNudgeText(int remaining_steps) {
    return "\n\n[系统提醒] 步数预算将尽,含本步在内最多还能再走 " + std::to_string(remaining_steps) +
           " 步就会被强制停止(这是预算硬上限,不是真的不让你干了)。从现在起停止漫游式探索:"
           "不要再开新的调查方向;把已经查到的事实、关键证据位置、排除掉的分支写成一个检查点,"
           "并给出部分结论与下一步建议。到限后检查点就是交回主会话的全部,别把它带进坟墓。";
}

}  // namespace

void InjectIncomingMessage(std::vector<api::Message>& history, api::Message incoming) {
    if (incoming.role != api::Role::User || incoming.content.empty()) {
        return;
    }
    if (!history.empty() && history.back().role == api::Role::User) {
        // 末条是 user(最常见:刚攒完的 tool_result 消息)——文本块追加进
        // 去即可,不起第二条连排的 user 消息,三种 wire 协议都安全。
        for (auto& block : incoming.content) {
            history.back().content.push_back(std::move(block));
        }
        return;
    }
    history.push_back(std::move(incoming));
}

bool ShouldNudgeStepLimit(int step_index, int max_steps_per_turn) {
    // max_steps_per_turn <= 0 = 无上限,压根没有"步数将尽"这回事,永不触发。
    if (max_steps_per_turn <= 0) {
        return false;
    }
    // step_index 是 for 循环里"即将发起的这一步"的 0-based 下标,
    // max_steps_per_turn - step_index 就是含当步在内还能走几步。收口提示只
    // 注入一次:落在"剩余步数第一次降到阈值(含)以下"的那一步——即剩余数
    // 恰等于 min(max_steps_per_turn, 阈值)。预算本来就小于阈值时第一步就
    // 提醒,之后各步不再重复。
    return (max_steps_per_turn - step_index) == (std::min)(max_steps_per_turn, kStepLimitNudgeThreshold);
}

AgentLoop::AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
                      std::string system_prompt, int max_tokens, int max_steps_per_turn,
                      std::size_t max_context_chars)
    : backend_(backend),
      registry_(registry),
      model_(std::move(model)),
      system_prompt_(std::move(system_prompt)),
      max_tokens_(max_tokens),
      max_steps_per_turn_(max_steps_per_turn),
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

    // 步数与 stop reason 的活账:每次模型请求(每个 step)各记一笔,收场时随
    // RunOutcome 交出去——上层(子代理)按它分型 budget_exhausted/no_final_text
    // 等,不再靠解析错误文案猜。
    int steps_used = 0;
    std::string last_stop_reason;
    // 前缀记账(agent/prefix.hpp):本步请求与上一份的追加律判定结果,
    // 发请求前算好,报 usage 时随 UsageReport 交出去。
    bool step_prefix_append_only = true;
    std::string step_epoch_break_reason;

    // max_steps_per_turn_ <= 0 = 无上限:循环条件里第一个子句恒真,第二个
    // 子句(步数比较)压根不会被求值,step_index 就一直往上涨,靠 end_turn
    // 或者用户 ESC/Ctrl+C(cancel)收场,不靠这里的硬闸。max_steps_per_turn_ > 0
    // 时才是"到点就停"的老行为。
    for (int step_index = 0; max_steps_per_turn_ <= 0 || step_index < max_steps_per_turn_; ++step_index) {
        // 跨会话传话的安全收件点:工具结果已攒完、下一次请求尚未发出——
        // 正是"不打断工具、步边界收信"的那个缝。step_index==0 不收:这一步
        // 的用户消息刚落下,空闲路径(main.cpp)在起 Run 之前已经收过一趟,
        // 纯文本轮的信该"先排住,本 turn 收口后再开一轮"(规格),不抢跑。
        // 回调只在主线程这里被调,流式/确认当口绝不会碰它,来信自然
        // 不可能替用户答确认。
        if (inbox_ && step_index > 0) {
            while (auto incoming = inbox_()) {
                InjectIncomingMessage(history_, std::move(*incoming));
            }
        }

        api::Request request;
        request.model = model_;
        request.system = system_prompt_;
        if (!turn_system_suffix_.empty()) {
            request.system += "\n\n" + turn_system_suffix_;
        }
        // 步数将尽提醒:只追加进这一步实际发出去的 system,不改 system_prompt_
        // 本身、也不进 history_——下一步 step_index 变了,剩余步数跟着变,提示
        // 该有就有、该消失就消失,没有"提示搭便车永久赖在历史里"的问题。
        if (ShouldNudgeStepLimit(step_index, max_steps_per_turn_)) {
            request.system += BuildStepLimitNudgeText(max_steps_per_turn_ - step_index);
        }

        // mid-turn 安全点:拼请求前先估 projected——system + 工具定义 + 全份
        // history + 输出预留,过参考线就把压力通报出去。上层回调里可以同步
        // 做一次语义压缩(ReplaceHistory),返回后下面 TrimHistory 拿到的就
        // 是(可能已换短的)history_。窗口未知(0)或没设回调时跳过,行为
        // 与从前一致——这一步不发出任何请求,估错了也不会误伤。
        if (context_window_tokens_ > 0 && on_context_pressure_) {
            std::size_t projected = EstimateUtf8Tokens(request.system) + EstimateHistoryTokens(history_) +
                                    static_cast<std::size_t>(max_tokens_);
            for (const auto& tool : registry_.All()) {
                if (tool_filter_ && !tool_filter_(*tool)) {
                    continue;
                }
                projected += EstimateUtf8Tokens(tool->name()) + EstimateUtf8Tokens(tool->description()) +
                             EstimateUtf8Tokens(tool->input_schema().dump());
            }
            ContextPressure pressure;
            pressure.phase = ContextPressure::Phase::PreRequest;
            pressure.projected_tokens = projected;
            pressure.window_tokens = context_window_tokens_;
            pressure.projected_overflow =
                projected >= context_window_tokens_ * static_cast<std::size_t>(kProjectedOverflowPercent) / 100;
            on_context_pressure_(pressure);
        }

        // 无损结构压缩(第二期):只改发给模型的视图——冷区里重复的只读
        // 工具结果换引用、被新版本覆盖的旧读取标 superseded、超长结果换
        // artifact 引用(头尾预览)。活历史与 session JSONL 一字不动,
        // tool use/result 配对天然不破(只重写 result 的 content 字符串)。
        // 压完的视图更小,后面 TrimHistory 的字符安全网也更少真开刀。
        const std::vector<api::Message>& view_source =
            structural_compression_enabled_ ? CompressWorkingView(history_, structural_options_, structural_stats_)
                                            : history_;
        TrimReport trim_report;
        request.messages =
            TrimHistory(view_source, max_context_chars_, kDefaultKeepRecentTurns, &trim_report);
        request.max_tokens = max_tokens_;
        // 有损硬裁发生了(丢轮/截结果),显式通报——静默降级会让用户以为语
        // 义压缩已成功,模型其实已经看不到那段原文。
        if ((trim_report.trimmed_turns || trim_report.truncated_results) && on_context_pressure_) {
            ContextPressure pressure;
            pressure.phase = ContextPressure::Phase::AfterHardTrim;
            pressure.hard_trimmed_turns = trim_report.trimmed_turns;
            pressure.hard_dropped_messages = trim_report.dropped_messages;
            pressure.hard_truncated_results = trim_report.truncated_results;
            pressure.window_tokens = context_window_tokens_;
            on_context_pressure_(pressure);
        }
        // 有损硬裁真出手了:下一份请求的工作视图换了裁剪形状,前缀记账给
        // 它点名(指纹 diff 只能报 old_message_changed,这里的因更准)。
        if (trim_report.trimmed_turns || trim_report.truncated_results) {
            if (pending_epoch_break_reason_.empty()) {
                pending_epoch_break_reason_ = "hard_trim";
            }
        }
        // 每轮现拼,不在 Run() 开头拼一次复用:tool_search 命中会在一次
        // Run() 中途把工具加进 loaded 集合(谓词的判定依据),下一轮请求
        // 就得带上新挂载工具的完整定义。没设谓词时,重拼出来的内容每轮
        // 一样,行为不变,只多花一点拼 JSON 的工夫。
        request.tools = BuildToolDefinitions();

        // 硬上限:轮级裁剪 + 工具结果截断都做完还是装不下(比如单条用户输入
        // 就超大),明确报错,不把一份注定被拒的超大请求发出去。
        if (EstimateHistoryBytes(request.messages) > max_context_chars_) {
            return std::unexpected("上下文超过上限(" + std::to_string(max_context_chars_) +
                                    " 字符),裁剪与截断后仍装不下,无法发送。请用 /compact 压缩历史,或开新会话。");
        }

        // 前缀记账:与上一份请求比追加律。断了就开新 cache epoch,并点名
        // 断因——loop 自己知道的因(compact/hard trim)比指纹反推的准,
        // 优先用它;没有显式因就按 diff 点名(model/system/tools/旧消息)。
        // epoch 断不是失败,无名无姓地断才是失败(agent/prefix.hpp)。
        {
            const PrefixFingerprint fingerprint = FingerprintRequest(request);
            step_prefix_append_only = true;
            step_epoch_break_reason.clear();
            if (last_prefix_.has_value()) {
                const PrefixDiff diff = DiffFingerprints(*last_prefix_, fingerprint);
                step_prefix_append_only = diff.append_only();
                if (!step_prefix_append_only) {
                    step_epoch_break_reason =
                        pending_epoch_break_reason_.empty() ? diff.break_reason() : pending_epoch_break_reason_;
                    ++cache_epoch_;
                }
            }
            pending_epoch_break_reason_.clear();
            last_prefix_ = std::move(fingerprint);
        }

        api::MessageAssembler assembler;
        bool stream_error = false;
        std::string stream_error_message;
        // MessageStart 的身份(request id/model)单记一笔:usage 报告要带
        // 它,哪一步是哪个请求才有账可查(前缀缓存守恒单第一期)。
        std::string stream_request_id;
        std::string stream_model;

        const auto send_result = backend_.send_stream(
            request,
            [&](const api::StreamEvent& event) {
                assembler.Feed(event);
                std::visit(
                    [&](const auto& e) {
                        using T = std::decay_t<decltype(e)>;
                        if constexpr (std::is_same_v<T, api::MessageStart>) {
                            stream_request_id = e.id;
                            stream_model = e.model;
                        } else if constexpr (std::is_same_v<T, api::TextDelta>) {
                            if (callbacks.on_text_delta) {
                                callbacks.on_text_delta(e.text);
                            }
                        } else if constexpr (std::is_same_v<T, api::ThinkingDelta>) {
                            if (callbacks.on_thinking_delta) {
                                callbacks.on_thinking_delta(e.text);
                            }
                        } else if constexpr (std::is_same_v<T, api::StreamError>) {
                            stream_error = true;
                            stream_error_message = e.message;
                        } else if constexpr (std::is_same_v<T, api::BuiltinToolStart>) {
                            if (callbacks.on_builtin_tool_start) {
                                callbacks.on_builtin_tool_start(e.name, e.input);
                            }
                        } else if constexpr (std::is_same_v<T, api::BuiltinToolDone>) {
                            if (callbacks.on_builtin_tool_done) {
                                callbacks.on_builtin_tool_done(e.name, e.input, e.summary, e.is_error);
                            }
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

                return RunOutcome{true, false, last_stop_reason, steps_used};
            }
            return std::unexpected("请求失败: " + err.message);
        }
        if (stream_error) {
            return std::unexpected("模型返回错误: " + stream_error_message);
        }

        api::Message assistant_message = assembler.BuildMessage();
        const std::string stop_reason = assembler.stop_reason();
        ++steps_used;
        last_stop_reason = stop_reason;
        history_.push_back(assistant_message);

        if (callbacks.on_usage) {
            api::UsageReport report;
            report.usage = assembler.usage();
            report.step_index = step_index;
            report.request_id = stream_request_id;
            report.model = stream_model;
            report.cache_epoch = cache_epoch_;
            report.epoch_break_reason = step_epoch_break_reason;
            report.prefix_append_only = step_prefix_append_only;
            callbacks.on_usage(report);
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
            return RunOutcome{false, false, stop_reason, steps_used};
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
            // 断言式兜底:RunOneTool 出口已经规范化过(见它文件头的信任边界
            // 注释),这里再过一遍 SanitizeUtf8 只为防将来有人在 Run() 之外
            // 绕路改历史——已经合法的内容是原样穿透的空操作。
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
            return RunOutcome{true, false, last_stop_reason, steps_used};
        }
    }

    // 只有 max_steps_per_turn_ > 0(用户显式设了硬上限)才可能走到这里——无上限时
    // for 循环条件恒真,永远不会正常退出到这一行。预算耗尽不是错误:history
    // 里留着到限为止的全部来回,部分结果由调用方(子代理按 budget_exhausted
    // 收账)带走;主循环按老口径打一行"已达上限"。
    return RunOutcome{false, true, last_stop_reason, steps_used};
}

}  // namespace lubancode::agent
