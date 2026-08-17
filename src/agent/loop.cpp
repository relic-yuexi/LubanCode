#include "agent/loop.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>
#include <variant>

#include "agent/prefix.hpp"
#include "api/assembler.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:工具结果的第一道编码关口
#include "tools/schema_check.hpp"      // updatedInput 改写后的 schema 复检

namespace lubancode::agent {

// 执行一个工具调用:先通知上层要开始了,M9 的 pre_tool 钩子紧接着检查一遍
// (拦截了就直接结束,连确认都不问),needs_confirm 的话再问一句,拒绝/
// 找不到工具/被钩子拦截/正常执行,最后都会走 on_tool_done 通知一遍,保证
// 上层能看到完整的生命周期。工具真执行完之后再跑一遍 post_tool 钩子
// (M9)。
//
// PTC(P1)起此函数从匿名命名空间转正导出:programmatic_tool_calling 脚本
// 里的每一枚 stub 调用都要走这条完整链(schema 校验在钩子改写复检里、
// PreToolUse/权限/执行/PostToolUse/编码信任边界一个不少),不许另开一条
// 绕过 hooks 的暗门。JSON 与 PTC 两个后端共用同一份执行代码。
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const Callbacks& callbacks,
                                const std::function<bool(const tools::Tool&)>& tool_filter,
                                const std::string& filter_denial) {
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

    // 工具状态机相位通报(没设回调 = 没配 hooks,行为与从前逐字节一致)。
    const auto phase = [&callbacks, &call](ToolPhase p) {
        if (callbacks.on_tool_phase) {
            callbacks.on_tool_phase(call.name, p);
        }
    };
    // 钩子拦截的统一收尾:停在 Blocked 相位(不冒充"运行过又失败"),
    // additionalContext 一并塞进 tool_result 给模型看。
    const auto blocked = [&phase, &dispatch_done, &call](const std::string& reason,
                                                          const std::vector<std::string>& extra_context) {
        phase(ToolPhase::Blocked);
        std::string content = reason;
        for (const auto& ctx : extra_context) {
            content += "\n[钩子附注] " + ctx;
        }
        return dispatch_done(call.name, tools::Tool::Result{content, true});
    };

    if (callbacks.on_tool_start) {
        callbacks.on_tool_start(call.name, call.input);
    }

    tools::Tool* tool = registry.Find(call.name);
    if (tool == nullptr) {
        return dispatch_done(call.name, tools::Tool::Result{"未知工具: " + call.name, true});
    }

    // tool_search(延迟挂载):注册表里查得到,但过滤谓词不放行——延迟工具
    // 还没挂载,或者这个角色用不上它(Explore 只读那类)。不当"未知工具"
    // 糊弄,给一条指路的友好错误:默认说"尚未挂载,先 tool_search";调用
    // 方另给了 filter_denial(角色限制)就照说——限制来自哪里,得看得见。
    if (tool_filter && !tool_filter(*tool)) {
        const std::string denial =
            filter_denial.empty()
                ? "工具 " + call.name + " 存在但尚未挂载:请先用 tool_search 检索挂载,再调用。"
                : "工具 " + call.name + ": " + filter_denial;
        return dispatch_done(call.name, tools::Tool::Result{denial, true});
    }

    // ---- PreToolUse:在 UI 标记"真执行"之前、权限确认之前。deny -> 拦;
    // ask -> 即使确认档放行也要问用户;allow -> 跳过用户确认(deny 规则
    // 与权限策略仍在确认回调里,钩子越不了权);updatedInput 只与 allow
    // 同返,先过一遍工具 schema,改写打回即拦。
    phase(ToolPhase::CheckingHook);
    ToolHookDecision pre;
    if (callbacks.on_pre_tool_use_hook) {
        pre = callbacks.on_pre_tool_use_hook(call.name, call.input);
    } else if (callbacks.on_pre_tool_hook) {
        // 旧回调兼容:非空 = deny。
        const std::optional<std::string> legacy_blocked = callbacks.on_pre_tool_hook(call.name, call.input);
        if (legacy_blocked.has_value()) {
            pre.decision = ToolHookDecision::Decision::Deny;
            pre.reason = *legacy_blocked;
        }
    }

    if (pre.decision == ToolHookDecision::Decision::Deny) {
        return blocked(pre.reason.empty() ? "被 PreToolUse 钩子拦截" : pre.reason, pre.additional_context);
    }

    nlohmann::json effective_input = call.input;
    if (pre.updated_input.has_value()) {
        const auto schema_error = tools::ValidateInputAgainstSchema(*pre.updated_input, tool->input_schema());
        if (schema_error.has_value()) {
            // 钩子明确想改参,改出来的形状这工具不认——按拦截处理,不悄悄
            // 拿原参数跑出去(那是绕 schema 的路)。
            return blocked("PreToolUse 钩子改写入参未通过工具 schema,已拦截: " + *schema_error,
                           pre.additional_context);
        }
        effective_input = *pre.updated_input;
    }

    if (tool->needs_confirm()) {
        phase(ToolPhase::WaitingPermission);
        const bool allowed =
            callbacks.on_tool_confirm ? callbacks.on_tool_confirm(call.name, effective_input) : true;
        if (!allowed) {
            // 拒绝文案可由回调层给(后台子代理的拒绝是"无法弹确认、未预放
            // 行",不是用户拒绝——缺省文案会把子代理的最终报告带偏成"均被
            // 用户拒绝")。
            const std::string denial = callbacks.on_tool_denial_text ? callbacks.on_tool_denial_text(call.name)
                                                                     : std::string("用户拒绝执行该工具");
            return dispatch_done(call.name, tools::Tool::Result{denial, true});
        }
    }

    phase(ToolPhase::Running);
    tools::Tool::Result result = tool->execute(effective_input);
    // PostToolUse(新):结果先清洗成合法 UTF-8 再给钩子;钩子的反馈追加进
    // 模型所见 tool_result,原始结果照旧进审计(副作用已发生,不能撤销,
    // 也不冒充撤销)。旧回调照旧吃它一贯拿到的结果。
    if (!platform::IsValidUtf8(result.content)) {
        result.content = platform::SanitizeExternalText(result.content);
    }
    if (callbacks.on_post_tool_use_hook) {
        const std::vector<std::string> feedback = callbacks.on_post_tool_use_hook(call.name, effective_input, result);
        for (const auto& line : feedback) {
            result.content += "\n[post-tool-use hook 追加] " + line;
        }
    }
    if (callbacks.on_post_tool_hook) {
        callbacks.on_post_tool_hook(call.name, effective_input, result);
    }
    return dispatch_done(call.name, std::move(result));
}

namespace {

// 步数将尽提醒的正文:固定文案,不带倒计时数字。前缀缓存守恒单第五期起
// 不再改 system——提醒在"剩余步数第一次降到阈值"那一步,追加进当时尚未
// 发出的尾部 user/tool-result 消息,随 history 留住:后头不改数字、不撤
// 旧提醒,追加律不破。硬上限仍由循环计数执行,提示无需承担精确计数。
std::string BuildStepLimitNudgeText() {
    return "\n\n[系统提醒] 已进入轮数上限前的收尾区,请尽快收束:不要再开新的调查方向;"
           "把已经查到的事实、关键证据位置、排除掉的分支写成一个检查点,"
           "并给出部分结论与下一步建议。到限后检查点就是交回主会话的全部,别把它带进坟墓。";
}

// 输出预算耗尽时的续跑标记(规格根因四):宿主注入,要求模型收束思考、
// 先调工具或交检查点。明写"非用户输入"——不伪装成人话,也不原样重发
// 用户 prompt(那只是把同样的思考再烧一遍)。
std::string BuildLengthContinuationText() {
    return "[系统标记,非用户输入] 上一轮输出在预算上限处被截断(finish_reason=length),"
           "整轮只有思考、没有正文。请立即停止展开思考:要么调用下一枚该调的工具,"
           "要么用不超过五行给出检查点(已确认的事实、下一步)。不要再继续推理。";
}

// 前缀 debug 开关(环境变量 LUBANCODE_DEBUG_PREFIX 任意非空值打开):
// 每次请求跟上一份比,打一行断因与追加律。只打断因、位置与 hash,不打
// system 正文、工具参数、记忆正文(前缀缓存守恒单"不做"节)。
bool PrefixDebugEnabled() {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, "LUBANCODE_DEBUG_PREFIX");
    if (err != 0 || buffer == nullptr) {
        return false;
    }
    const bool enabled = buffer[0] != '\0';
    std::free(buffer);
    return enabled;
#else
    const char* raw = std::getenv("LUBANCODE_DEBUG_PREFIX");
    return raw != nullptr && raw[0] != '\0';
#endif
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

AgentLoop::AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, AgentRuntimeProfile profile,
                     std::string system_prompt)
    : backend_(backend),
      registry_(registry),
      model_(std::move(profile.model)),
      system_prompt_(std::move(system_prompt)),
      profile_(std::move(profile)) {}

AgentLoop::AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
                      std::string system_prompt, std::optional<int> max_tokens, int max_steps_per_turn,
                      std::size_t max_context_chars)
    : AgentLoop(backend, registry,
                [&]() {
                    AgentRuntimeProfile profile;
                    profile.model = std::move(model);
                    profile.max_output_tokens = std::move(max_tokens);
                    profile.max_steps_per_turn = max_steps_per_turn;
                    profile.max_context_chars = max_context_chars;
                    return profile;
                }(),
                std::move(system_prompt)) {}

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
    run_active_ = true;
    active_turn_context_ = turn_context_;
    struct RunStateGuard {
        bool& active;
        std::string& context;
        ~RunStateGuard() {
            active = false;
            context.clear();
        }
    } run_state_guard{run_active_, active_turn_context_};

    history_.push_back(user_message);
    // 动态上下文(记忆召回/任务名册)随本轮 user 进请求视图:发过即钉住,
    // 不再每回合改 system 制造分叉点。持久 history_ 只留真输入;动态块
    // 只落 request_history_,故而 session/export/compact 都不会把它带走。
    if (!active_turn_context_.empty()) {
        user_message.content.push_back(api::TextBlock{active_turn_context_});
    }
    request_history_.push_back(std::move(user_message));

    // 步数与 stop reason 的活账:每次模型请求(每个 step)各记一笔,收场时随
    // RunOutcome 交出去——上层(子代理)按它分型 budget_exhausted/no_final_text
    // 等,不再靠解析错误文案猜。
    int steps_used = 0;
    std::string last_stop_reason;
    // 前缀记账(agent/prefix.hpp):本步请求与上一份的追加律判定结果,
    // 发请求前算好,报 usage 时随 UsageReport 交出去。
    bool step_prefix_append_only = true;
    std::string step_epoch_break_reason;
    // 输出预算活账(规格根因四):撞墙、续跑、usage 是否报告、思考检查点
    // 都在这一份上滚,收场时随 RunOutcome.output_budget 交出去。
    OutputBudgetReport budget_report;
    budget_report.limit_tokens = profile_.max_output_tokens.value_or(0);
    budget_report.continuation_limit = profile_.length_continuations;

    // profile_.max_steps_per_turn <= 0 = 无上限:循环条件里第一个子句恒真,第二个
    // 子句(步数比较)压根不会被求值,step_index 就一直往上涨,靠 end_turn
    // 或者用户 ESC/Ctrl+C(cancel)收场,不靠这里的硬闸。profile_.max_steps_per_turn > 0
    // 时才是"到点就停"的老行为。
    for (int step_index = 0; profile_.max_steps_per_turn <= 0 || step_index < profile_.max_steps_per_turn; ++step_index) {
        // 跨会话传话的安全收件点:工具结果已攒完、下一次请求尚未发出——
        // 正是"不打断工具、步边界收信"的那个缝。step_index==0 不收:这一步
        // 的用户消息刚落下,空闲路径(main.cpp)在起 Run 之前已经收过一趟,
        // 纯文本轮的信该"先排住,本 turn 收口后再开一轮"(规格),不抢跑。
        // 回调只在主线程这里被调,流式/确认当口绝不会碰它,来信自然
        // 不可能替用户答确认。
        if (inbox_ && step_index > 0) {
            while (auto incoming = inbox_()) {
                api::Message durable = *incoming;
                InjectIncomingMessage(history_, std::move(durable));
                InjectIncomingMessage(request_history_, std::move(*incoming));
            }
        }

        api::Request request;
        request.model = model_;
        request.system = system_prompt_;
        // 步数将尽提醒(第五期):固定文案,在"剩余步数第一次降到阈值"那一步
        // 追加进尚未发出的尾部消息(user 输入或刚攒完的 tool result),随
        // history 留住——不改 system,不撤旧提醒,追加律不破。ShouldNudge-
        // StepLimit 每次 Run 只真一次,天然只注一遍。
        if (ShouldNudgeStepLimit(step_index, profile_.max_steps_per_turn) && !history_.empty()) {
            const api::TextBlock nudge{BuildStepLimitNudgeText()};
            history_.back().content.push_back(nudge);
            request_history_.back().content.push_back(nudge);
        }

        // mid-turn 安全点:拼请求前先估 projected——system + 工具定义 + 全份
        // history + 输出预留,过参考线就把压力通报出去。上层回调里可以同步
        // 做一次语义压缩(ReplaceHistory),返回后下面 TrimHistory 拿到的就
        // 是(可能已换短的)request_history_。窗口未知(0)或没设回调时跳过,行为
        // 与从前一致——这一步不发出任何请求,估错了也不会误伤。
        if (profile_.context_window_tokens > 0 && on_context_pressure_) {
            // 输出上限纳入 projected 计算(规格根因一):声明了用声明值,
            // unset 用保守估计(kUnsetOutputReserveEstimateTokens)——服务端
            // 默认上限拿不到准数,宁可早压不撞墙。
            const OutputBudget output_budget{profile_.max_output_tokens, profile_.max_output_tokens_source};
            std::size_t projected = EstimateUtf8Tokens(request.system) + EstimateHistoryTokens(request_history_) +
                                    static_cast<std::size_t>(output_budget.reserve_for_estimate());
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
            pressure.window_tokens = profile_.context_window_tokens;
            pressure.projected_overflow =
                projected >= profile_.context_window_tokens * static_cast<std::size_t>(kProjectedOverflowPercent) / 100;
            on_context_pressure_(pressure);
        }

        // 无损结构压缩(第六期"首次定形"):只改发给模型的视图——每枚
        // tool result 第一次进请求视图时定形(短则全文、超长首次即 artifact
        // 预览、重复自述指回、新版本自述替代),决策台账 epoch 内钉死,绝不
        // 追改已经发过的表示。活历史与 session JSONL 一字不动,tool use/
        // result 配对天然不破。压完的视图更小,后面字符安全网也更少真开刀。
        // 第二期:带仓时 Artifact 决策先落盘,视图带稳定 artifact_id。
        std::vector<api::Message> view_source;
        if (structural_compression_enabled_) {
            view_source = CompressWorkingView(request_history_, structural_options_, structural_stats_,
                                              result_view_memo_, artifact_store_);
        } else {
            view_source = request_history_;
        }

        // 字符安全网 + sticky 视图:还没动手裁过,就按老规矩裁;真动手裁了
        // (丢轮/截结果),把裁过的视图钉住,后续只往尾部追加新消息——不
        // 再每请求重算"第一轮 + 最近 N 轮"让窗口一路滑(窗口滑就是追改
        // 已发前缀)。全量 JSONL 照旧保留,sticky 只是模型眼下那本账。
        TrimReport trim_report;
        if (sticky_view_.has_value() && view_source.size() >= sticky_base_history_size_) {
            std::vector<api::Message> pinned = *sticky_view_;
            pinned.insert(pinned.end(), view_source.begin() + static_cast<std::ptrdiff_t>(sticky_base_history_size_),
                          view_source.end());
            if (EstimateHistoryBytes(pinned) > profile_.max_context_chars) {
                // 钉住的视图也装不下了(长会话总会到这一步):重裁一次,
                // 换一副新形状并重新钉住——这是一次明确的 epoch break,
                // 下面 trim_report 会把它记上(hard_trim)。
                pinned = TrimHistory(std::move(pinned), profile_.max_context_chars, kDefaultKeepRecentTurns, &trim_report);
                sticky_view_ = pinned;
                sticky_base_history_size_ = view_source.size();
            }
            request.messages = std::move(pinned);
        } else {
            std::vector<api::Message> trimmed =
                TrimHistory(view_source, profile_.max_context_chars, kDefaultKeepRecentTurns, &trim_report);
            if (trim_report.trimmed_turns || trim_report.truncated_results) {
                // 第一次真动手裁:钉住,开新 epoch 的账由下面的 hard_trim 记。
                sticky_view_ = trimmed;
                sticky_base_history_size_ = view_source.size();
            }
            request.messages = std::move(trimmed);
        }
        request.max_tokens = profile_.max_output_tokens;  // nullopt = unset,交服务端默认
        // 有损硬裁发生了(丢轮/截结果),显式通报——静默降级会让用户以为语
        // 义压缩已成功,模型其实已经看不到那段原文。
        if ((trim_report.trimmed_turns || trim_report.truncated_results) && on_context_pressure_) {
            ContextPressure pressure;
            pressure.phase = ContextPressure::Phase::AfterHardTrim;
            pressure.hard_trimmed_turns = trim_report.trimmed_turns;
            pressure.hard_dropped_messages = trim_report.dropped_messages;
            pressure.hard_truncated_results = trim_report.truncated_results;
            pressure.window_tokens = profile_.context_window_tokens;
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
        if (EstimateHistoryBytes(request.messages) > profile_.max_context_chars) {
            return std::unexpected("上下文超过上限(" + std::to_string(profile_.max_context_chars) +
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
                if (PrefixDebugEnabled()) {
                    // 诊断行:只带断因/位置/条数,不带任何正文与 hash 以外的东西。
                    std::cerr << "[prefix] epoch " << cache_epoch_ << " step " << step_index
                              << " append_only=" << (step_prefix_append_only ? "true" : "false");
                    if (!step_prefix_append_only) {
                        std::cerr << " reason=" << step_epoch_break_reason << " old_message_changed_at="
                                  << diff.old_message_changed_at;
                    }
                    std::cerr << " appended_messages=" << diff.appended_messages
                              << " system_hash=" << fingerprint.system_hash.substr(0, 8)
                              << " tools_hash=" << fingerprint.tools_hash.substr(0, 8) << "\n";
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
        // wire 边界闸门(宽窄转换异常单):中转把多字节序列劈在 delta 边界
        // 时,半截尾巴扣在闸内、下一块拼齐再放行——显示层永远只见完整合法
        // 的 UTF-8。history 侧 assembler 攒的是原始拼接(劈半自愈),不走
        // 闸;流收口后统一 Flush,残尾按 U+FFFD 放完。
        platform::Utf8DeltaGate text_delta_gate;
        platform::Utf8DeltaGate thinking_delta_gate;

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
                                const std::string gated = text_delta_gate.Feed(e.text);
                                if (!gated.empty()) {
                                    callbacks.on_text_delta(gated);
                                }
                            }
                        } else if constexpr (std::is_same_v<T, api::ThinkingDelta>) {
                            // 输出预算活账:思考字节与末段摘要(检查点证据,
                            // 截尾保留,不整段攒)。与回调分发同一处,流到即记。
                            budget_report.thinking_bytes += e.text.size();
                            budget_report.thinking_tail += e.text;
                            if (budget_report.thinking_tail.size() > 512) {
                                std::size_t start = budget_report.thinking_tail.size() - 512;
                                // 截尾不劈半个字:起点落在 UTF-8 续字节上就
                                // 推到下一个字符边界。
                                while (start < budget_report.thinking_tail.size() &&
                                       (static_cast<unsigned char>(budget_report.thinking_tail[start]) & 0xC0) ==
                                           0x80) {
                                    ++start;
                                }
                                budget_report.thinking_tail.erase(0, start);
                            }
                            if (callbacks.on_thinking_delta) {
                                const std::string gated = thinking_delta_gate.Feed(e.text);
                                if (!gated.empty()) {
                                    callbacks.on_thinking_delta(gated);
                                }
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

        // 流收口:闸里扣着的尾巴拼不齐就是坏字节,按 U+FFFD 放完——错误/
        // 打断路径也要放,显示层与 history 的账对得上。
        if (callbacks.on_text_delta) {
            const std::string text_tail = text_delta_gate.Flush();
            if (!text_tail.empty()) {
                callbacks.on_text_delta(text_tail);
            }
        }
        if (callbacks.on_thinking_delta) {
            const std::string thinking_tail = thinking_delta_gate.Flush();
            if (!thinking_tail.empty()) {
                callbacks.on_thinking_delta(thinking_tail);
            }
        }

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
                request_history_.push_back(assistant_message);

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
                    history_.push_back(orphan_message);
                    request_history_.push_back(std::move(orphan_message));
                }

                return RunOutcome{true, false, false, last_stop_reason, steps_used};
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
        request_history_.push_back(assistant_message);
        // usage 是否报告(规格根因四):任一请求带回过非零 usage 就算报告过,
        // 之后失败页说"token 数未报告"只看这一位,不拿 0 糊。
        {
            const api::Usage& usage = assembler.usage();
            budget_report.usage_reported = budget_report.usage_reported || usage.input_tokens > 0 ||
                                            usage.output_tokens > 0 || usage.cache_read_tokens > 0 ||
                                            usage.cache_creation_tokens > 0 || usage.output_reasoning_tokens > 0;
        }

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
            // 输出预算耗尽且正文为空(本地兼容端实测过的现场:reasoning 吃光
            // max_tokens,finish_reason=length,一个正文字都没有)。规格根因四:
            // max_tokens 是独立状态,先给一次受控续跑——只有思考、没有正文和
            // 工具时,注入一条宿主标记(要求收束思考,先调工具或交检查点),
            // 再走一步。标记不是用户输入,文本里写明来历;绝不原样重发 prompt
            // (那只是再烧一遍同样的思考)。续跑次数有显式账
            // (profile_.length_continuations,默认 1),烧完仍空才收场。
            bool has_text = false;
            for (const auto& block : assistant_message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr && !text->text.empty()) {
                    has_text = true;
                    break;
                }
            }
            if (stop_reason == "max_tokens" && !has_text) {
                const bool can_continue = budget_report.continuations_used < profile_.length_continuations &&
                                          (cancel == nullptr || !cancel->load());
                if (can_continue) {
                    api::Message marker;
                    marker.role = api::Role::User;
                    marker.content.push_back(api::TextBlock{BuildLengthContinuationText()});
                    history_.push_back(marker);
                    request_history_.push_back(std::move(marker));
                    ++budget_report.continuations_used;
                    continue;  // 下一步循环:不重发 prompt,只带标记续跑
                }
                budget_report.exhausted = true;
                RunOutcome outcome{false, false, true, stop_reason, steps_used};
                outcome.output_budget = budget_report;
                return outcome;
            }
            RunOutcome outcome{false, false, false, stop_reason, steps_used};
            outcome.output_budget = budget_report;  // 未撞墙也带走账(续跑过的轮次要可查)
            return outcome;
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
            const tools::Tool::Result result = RunOneTool(registry_, call, callbacks, tool_filter_,
                                                          tool_filter_denial_);
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
        history_.push_back(tool_result_message);
        request_history_.push_back(std::move(tool_result_message));

        if (interrupted) {
            return RunOutcome{true, false, false, last_stop_reason, steps_used};
        }
    }

    // 只有 profile_.max_steps_per_turn > 0(用户显式设了硬上限)才可能走到这里——无上限时
    // for 循环条件恒真,永远不会正常退出到这一行。预算耗尽不是错误:history
    // 里留着到限为止的全部来回,部分结果由调用方(子代理按 budget_exhausted
    // 收账)带走;主循环按老口径打一行"已达上限"。
    return RunOutcome{false, true, false, last_stop_reason, steps_used};
}

}  // namespace lubancode::agent
