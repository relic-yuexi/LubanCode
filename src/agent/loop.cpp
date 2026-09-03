// AgentLoop 与 RunOneTool 的实现。Agent 本体在 agent/agent.cpp,上下文账
// 在 agent/context_manager.cpp——这里只剩轮次推进:拼请求(皮上的叠层就
// 地生效)、发流、工具循环、收口。

#include "agent/loop.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "agent/agent.hpp"
#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "agent/prefix.hpp"
#include "agent/prompts.hpp"
#include "agent/resolved_prompt_builder.hpp"  // Token 账本单 A1:三层后叠 + manifest 同一次解析
#include "agent/tool_result_images.hpp"  // 工具结果图片回喂:请求出门前的 base64 重灌
#include "api/assembler.hpp"
#include "cli/i18n.hpp"
#include "hooks/hash.hpp"  // Sha256Hex:trace 的入参/结果摘要锚
#include "platform/text_encoding.hpp"  // SanitizeExternalText:工具结果的第一道编码关口
#include "platform/wall_clock.hpp"     // trace 与计划动作须共用一枚墙钟
#include "runtime/plan_mode.hpp"       // kErrModeDenied:Plan 硬闸的稳定码
#include "tools/instruction_scope.hpp"  // 闸文案两档前缀:握手/超预算的分账
#include "tools/schema_check.hpp"      // updatedInput 改写后的 schema 复检
#include "platform/log_sink.hpp"

namespace lubancode::agent {

namespace {
constexpr std::size_t kContextPreflightHeadroomTokens = 512;

std::string FormatRecoveryElapsed(std::chrono::milliseconds elapsed) {
    const auto total_seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    if (total_seconds < 60) {
        return std::to_string(total_seconds) + "s";
    }
    const auto minutes = total_seconds / 60;
    const auto seconds = total_seconds % 60;
    return std::to_string(minutes) + "m" + (seconds < 10 ? "0" : "") + std::to_string(seconds) + "s";
}

// 预检放不下时的应急输出预留(派工单 §四):常规预留(声明值/保守估)装
// 不下、当前消息自己装得下时启用——按窗口取一个小而正的值,让任务能在
// 同一上下文里收尾交出短交接,而不是当场死掉叫用户开新会话。
// clamp(window/16, 2k, 8k);窗口未知给 4096。
std::size_t EmergencyOutputReserveTokens(std::size_t window_tokens) {
    std::size_t reserve = window_tokens == 0 ? 4096 : window_tokens / 16;
    if (reserve < 2048) {
        reserve = 2048;
    }
    if (reserve > 8192) {
        reserve = 8192;
    }
    return reserve;
}

// 应急收窄时注入的一次性收尾交代(派工单 §4.2/§4.4):模型当轮立即收尾,
// 交出已跑命令、关键结果、未提交改动与后续建议——durable handoff 的正文
// 由模型按这段指引产出,现场(工具结果/检查点/隔离房)由宿主保住。
std::string BuildContextWrapupNudgeText(std::size_t emergency_reserve) {
    return "[宿主] 上下文将尽:本请求的输出预留已临时收窄至约 " + std::to_string(emergency_reserve) +
           " token。请立即收尾,不再发起新的工具调用;用一段简短交接写明:已执行的命令与其结果、关键结论、"
           "未提交的改动、建议的后续步骤。";
}

// Unix epoch 毫秒。chrono 不同 clock 不能混算;所有台账都从 platform
// 取墙钟,免得各自换算后留下不同口径。
std::int64_t NowMsEpoch() {
    return platform::WallClockNowMs();
}

// 发送前的保守尺。日常预算仍用全库统一的 EstimateUtf8Tokens；临出门再
// 给“空白隔开的短词”逐枚托底。`a a a ...` 在常见 tokenizer 里几乎一词
// 一 token，旧尺只按 4 ASCII/ token 会少算一半，正是 MiniCPM 真机越窗
// 的那副形状。取 max，不把普通长英文按一字一 token 粗暴拦掉。
std::size_t EstimateTextTokensForPreflight(const std::string& text) {
    std::size_t whitespace_terms = 0;
    bool in_term = false;
    for (const unsigned char ch : text) {
        const bool separator = std::isspace(ch) != 0;
        if (!separator && !in_term) {
            ++whitespace_terms;
        }
        in_term = !separator;
    }
    return std::max(EstimateUtf8Tokens(text), whitespace_terms);
}

std::size_t EstimateMessageTokensForPreflight(const api::Message& message) {
    std::size_t total = 0;
    for (const auto& block : message.content) {
        total += std::visit(
            [](const auto& b) -> std::size_t {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, api::TextBlock>) {
                    return EstimateTextTokensForPreflight(b.text);
                } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                    // 图片按像素折 token(宽×高/750,anthropic 口径、各家
                    // 居中):块上宽高优先,缺了从 base64 读头,真读不出才
                    // 退字节口径——公共尺在 EstimateImageTokensForPreflight
                    // (工具结果图片回喂单),与工具图同一条。
                    return EstimateImageTokensForPreflight(b.width, b.height, b.data);
                } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                    return EstimateTextTokensForPreflight(b.name) + EstimateTextTokensForPreflight(b.id) +
                           EstimateTextTokensForPreflight(b.input.dump());
                } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                    // MCP 富结果单 P0.3:富块的 base64 从不进 durable history
                    // ——content 是 TextProjection,图片/音频只剩 artifact
                    // 短句,按短文本估。工具结果图片回喂单添的后账:请求副本
                    // 上重灌过的 wire_base64 会真上 wire,token 与用户贴图
                    // ImageBlock 同走像素口径公共尺(宽×高/750,读不出宽高
                    // 退字节口径)——老字节口径把 3MB 的 3072x1918 截图记成
                    // 65 万 token,真上 wire 只值八千,整轮被误报越窗。
                    std::size_t image_tokens = 0;
                    for (const auto& rich : b.blocks) {
                        if (const auto* image = std::get_if<tools::ImageContent>(&rich);
                            image != nullptr) {
                            image_tokens += EstimateImageTokensForPreflight(image->width, image->height,
                                                                           image->wire_base64);
                        }
                    }
                    return EstimateTextTokensForPreflight(b.tool_use_id) +
                           EstimateTextTokensForPreflight(b.content) + image_tokens;
                } else if constexpr (std::is_same_v<T, api::ThinkingBlock>) {
                    return EstimateTextTokensForPreflight(b.text) + EstimateTextTokensForPreflight(b.signature);
                } else {
                    return 0;
                }
            },
            block);
    }
    return total;
}

std::size_t EstimateRequestInputTokensForPreflight(const api::Request& request) {
    std::size_t total = EstimateTextTokensForPreflight(request.system);
    for (const auto& message : request.messages) total += EstimateMessageTokensForPreflight(message);
    for (const auto& tool : request.tools) {
        total += EstimateTextTokensForPreflight(tool.name) + EstimateTextTokensForPreflight(tool.description) +
                 EstimateTextTokensForPreflight(tool.input_schema.dump());
    }
    return total;
}

std::size_t EstimateHistoryTokensForPreflight(const std::vector<api::Message>& messages) {
    std::size_t total = 0;
    for (const auto& message : messages) total += EstimateMessageTokensForPreflight(message);
    return total;
}

bool ExceedsContextWindow(std::size_t input_tokens, std::size_t output_tokens, std::size_t window_tokens) {
    return input_tokens >= window_tokens || output_tokens >= window_tokens - input_tokens ||
           kContextPreflightHeadroomTokens >= window_tokens - input_tokens - output_tokens;
}
}  // namespace

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
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const TurnWiring& wiring,
                                const std::function<bool(const tools::Tool&)>& tool_filter,
                                const std::string& filter_denial,
                                const ToolTraceContext* trace,
                                const std::atomic<bool>* cancel,
                                const tools::ProxyCallContext* proxy,
                                const std::function<bool(const tools::Tool&)>& turn_gate,
                                const std::string& turn_gate_denial) {
    // 每条收尾路共用的分发口:先清洗,再 on_tool_done,清洗版随返回值交给
    // 调用方(进 history / 下一轮请求)。日志只记字段名、长度、坏字节位置
    // 与前后几个十六进制字节,不倒正文。
    const auto dispatch_done = [&wiring](const std::string& tool_use_id, const std::string& name,
                                             tools::Tool::Result result) {
        if (!platform::IsValidUtf8(result.content)) {
            platform::LogSink::Instance().Warn(
                "loop", platform::DescribeUtf8Issue("tool_result:" + name, result.content));
            // 富结果单:payload 的全部文本字段(含 structured JSON)一起洗,
            // content 投影随之重算——不洗块里的坏串,块进 history 后 dump()
            // 照样 316。
            result.SanitizeInPlace();
        }
        if (wiring.events != nullptr) {
            wiring.events->OnToolDone(tool_use_id, name, result, wiring.subordinate_stream);
        }
        return result;
    };

    // 工具状态机相位通报(没设回调 = 没配 hooks,行为与从前逐字节一致)。
    const auto phase = [&wiring, &call](runtime::ToolPhase p) {
        if (wiring.on_tool_phase) {
            wiring.on_tool_phase(call.id, call.name, p);
        }
    };

    // ---- 逐枚追踪:栅栏发射器(trace 缺席 = 没装配,全部空操作) ---------
    // 领域事件只从这一个口出(单子"一份事件,两路消费"):Runtime 投影、
    // 持久账、录制件、Hook 关联各取所需,RunOneTool 不再多路手写。
    const auto started_at = std::chrono::steady_clock::now();
    ToolTraceEvent fired_started;  // 已越过 started 的载荷,finished 时复用
    bool crossed_start = false;
    const auto emit = [trace, &wiring, &call, &fired_started, &crossed_start](ToolTraceEvent event) {
        if (trace == nullptr) {
            return;
        }
        // 身份三件从 trace 上下文带(execution_id 宿主发号,batch/序号/
        // parent 由调用方钉);事件自己只管相位与载荷。
        event.execution_id = trace->execution_id;
        event.item_id = trace->execution_id;  // Runtime item id 同源(单子:不自造第二只计数器)
        event.tool_use_id = call.id;
        event.tool_name = call.name;
        event.batch_id = trace->batch_id;
        event.sequence_in_batch = trace->sequence_in_batch;
        event.turn_id = trace->turn_id;
        event.thread_id = trace->thread_id;
        event.provider_request_id = trace->provider_request_id;
        event.parent_execution_id = trace->parent_execution_id;
        event.retry_of = trace->retry_of;
        event.blocked_by = trace->blocked_by;
        // compensates 只补空:finish 侧问过装配层(undo 工具 execute 后
        // 报的动态关系)就不再被这里的静态空值抹掉。
        if (event.compensates.empty()) {
            event.compensates = trace->compensates;
        }
        // trace 与其余台账须同用一枚墙钟,否则跨账对时会生偏差。
        event.timestamp_ms = NowMsEpoch();
        // 动态工具 P1:经 tool_invoke 代理调用的两层事实(单子 §6.2)。事件
        // 的一等字段 tool_name 画的是真实目标(协议证据的另一半),这里补
        // 的是 transport 那一层——两层都留,不拿 tool_invoke 糊账,也不丢
        // 引用与摘要的凭据。
        if (!trace->transport_tool.empty()) {
            event.details["transport_tool"] = trace->transport_tool;
            event.details["resolved_tool"] = call.name;
            event.details["tool_ref"] = trace->tool_ref;
            event.details["schema_digest"] = trace->schema_digest;
        }
        if (event.kind == ToolTraceEventKind::ExecutionStarted) {
            fired_started = event;
            crossed_start = true;
        }
        wiring.on_tool_trace(event);
    };
    // 结果引用:小结果内联(过 kInlineResultCap),大结果标 Unavailable
    // (artifact 卸载是装配层/artifact store 的活,RunOneTool 不重复落一份;
    // 恢复侧拿不到正文就如实标,不冒充可恢复)。
    const auto make_result_ref = [](const std::string& content) {
        ToolResultRef ref;
        ref.sha256 = hooks::Sha256Hex(content);
        ref.bytes = content.size();
        if (content.size() <= kInlineResultCap) {
            ref.kind = ToolResultRef::Kind::Inline;
            ref.content = content;
        } else {
            ref.kind = ToolResultRef::Kind::Unavailable;
        }
        ref.preview = BuildTracePreview(content, 160, 160);
        return ref;
    };
    // 终态栅栏:拿到原始结果(UTF-8 规范化之后、PostToolUse 之前——原样
    // outcome 先落账,免得 Hook 崩溃抹掉工具已完成的事实)。
    const auto finish = [&](const tools::Tool::Result& result, ToolSourceKind source_kind,
                            const std::string& source_instance, EffectClass effect_class) {
        if (trace == nullptr) {
            return;
        }
        ToolTraceEvent event;
        event.kind = ToolTraceEventKind::ExecutionFinished;
        // outcome:工具自报的稳定字符串优先;没报的按失败形态投影
        //(succeeded 只在工具明确自报时才算,不拿 is_error=false 冒充)。
        if (!result.outcome.empty()) {
            if (!ParseToolOutcome(result.outcome, event.outcome)) {
                event.outcome = result.is_error ? ToolOutcome::ToolError : ToolOutcome::Succeeded;
            }
        } else {
            event.outcome = result.is_error ? ToolOutcome::ToolError : ToolOutcome::Succeeded;
        }
        event.error_code = result.error_code;
        event.fallback_message =
            result.content.size() <= 200
                ? result.content
                : result.content.substr(0, platform::Utf8PrefixBoundary(result.content, 200));
        event.details = result.details;
        // MCP 内层账(逐枚追踪单"MCP 外层 execution 要挂内层"):jsonrpc id
        // 与 transport generation 从 details 提升成一等字段,迟到响应事件
        // 凭这对关联原 execution,不投给新调用。
        if (result.details.contains("jsonrpc_request_id") && result.details["jsonrpc_request_id"].is_number_integer()) {
            event.jsonrpc_request_id = result.details["jsonrpc_request_id"].get<std::int64_t>();
        }
        event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
        event.result_ref = make_result_ref(result.content);
        event.source_kind = source_kind;
        event.source_instance = source_instance;
        // 本地文件条件式撤销(单子第四期):write/edit 带回的 undo token
        // 随 finished 栅栏落账;恢复侧凭它(加上当前文件内容的复检)给
        // "可撤销"建议,宿主不自动执行。
        if (!result.undo_path.empty()) {
            event.undo.path = result.undo_path;
            event.undo.preimage_sha256 = result.undo_preimage_sha256;
            event.undo.postimage_sha256 = result.undo_postimage_sha256;
            event.undo.created_new_file = result.undo_created_new_file;
            event.undo.preimage = result.undo_preimage;
        }
        if (crossed_start) {
            event.effect_class = fired_started.effect_class;
            event.effective_input_sha256 = fired_started.effective_input_sha256;
        } else {
            event.effect_class = effect_class;
        }
        // 补偿关系边(单子第四期):静态(context.compensates)优先;
        // 没有时问装配层(undo/补偿工具 execute 后才报得出自己补偿谁)。
        // 只补空不覆盖——原调用与已钉的边不动。
        if (event.compensates.empty() && wiring.on_tool_compensates && trace != nullptr) {
            event.compensates = wiring.on_tool_compensates(trace->execution_id, call.name);
        }
        emit(std::move(event));
    };

    // 钩子拦截的统一收尾:停在 Blocked 相位(不冒充"运行过又失败"),
    // additionalContext 一并塞进 tool_result 给模型看。
    const auto blocked = [&phase, &dispatch_done, &call](const std::string& reason,
                                                          const std::vector<std::string>& extra_context) {
        phase(runtime::ToolPhase::Blocked);
        std::string content = reason;
        for (const auto& ctx : extra_context) {
            content += "\n[钩子附注] " + ctx;
        }
        return dispatch_done(call.id, call.name, tools::Tool::Result{content, true});
    };

    if (wiring.events != nullptr) {
        wiring.events->OnToolStart(call.id, call.name, call.input, wiring.subordinate_stream);
    }

    tools::Tool* tool = registry.Find(call.name);
    // 来源/副作用档从注册元数据拿(逐枚追踪单:不靠 RTTI 猜);没带元数据
    // 的注册按 builtin 记,副作用档取工具自己的保守声明。
    const tools::ToolRegistration* registration = registry.RegistrationOf(call.name);
    ToolSourceKind source_kind = ToolSourceKind::Builtin;
    std::string source_instance;
    EffectClass effect_class = EffectClass::InProcessUnknown;
    if (registration != nullptr) {
        switch (registration->source_kind) {
            case tools::ToolSourceKind::Builtin: source_kind = ToolSourceKind::Builtin; break;
            case tools::ToolSourceKind::Mcp: source_kind = ToolSourceKind::Mcp; break;
            case tools::ToolSourceKind::Lsp: source_kind = ToolSourceKind::Lsp; break;
            case tools::ToolSourceKind::PluginLua: source_kind = ToolSourceKind::PluginLua; break;
            case tools::ToolSourceKind::PluginNative: source_kind = ToolSourceKind::PluginNative; break;
            case tools::ToolSourceKind::Agent: source_kind = ToolSourceKind::Agent; break;
            case tools::ToolSourceKind::Ptc: source_kind = ToolSourceKind::Ptc; break;
            case tools::ToolSourceKind::Deferred: source_kind = ToolSourceKind::Deferred; break;
        }
        source_instance = registration->source_instance;
        // 两侧 EffectClass 枚举各自独立(tools 不牵 agent),语义一一对应,
        // 这里显式映射(与下面的 tool->effect_class() 同一张表)。
        switch (registration->effect_class) {
            case tools::EffectClass::ReadOnlyLocal: effect_class = EffectClass::ReadOnlyLocal; break;
            case tools::EffectClass::ReadOnlyRemote: effect_class = EffectClass::ReadOnlyRemote; break;
            case tools::EffectClass::LocalReversible: effect_class = EffectClass::LocalReversible; break;
            case tools::EffectClass::LocalProcessUnknown: effect_class = EffectClass::LocalProcessUnknown; break;
            case tools::EffectClass::RemoteIdempotent: effect_class = EffectClass::RemoteIdempotent; break;
            case tools::EffectClass::RemoteCompensatable: effect_class = EffectClass::RemoteCompensatable; break;
            case tools::EffectClass::RemoteIrreversible: effect_class = EffectClass::RemoteIrreversible; break;
            case tools::EffectClass::InProcessUnknown: effect_class = EffectClass::InProcessUnknown; break;
        }
    } else if (tool != nullptr) {
        switch (tool->effect_class()) {
            case tools::EffectClass::ReadOnlyLocal: effect_class = EffectClass::ReadOnlyLocal; break;
            case tools::EffectClass::ReadOnlyRemote: effect_class = EffectClass::ReadOnlyRemote; break;
            case tools::EffectClass::LocalReversible: effect_class = EffectClass::LocalReversible; break;
            case tools::EffectClass::LocalProcessUnknown: effect_class = EffectClass::LocalProcessUnknown; break;
            case tools::EffectClass::RemoteIdempotent: effect_class = EffectClass::RemoteIdempotent; break;
            case tools::EffectClass::RemoteCompensatable: effect_class = EffectClass::RemoteCompensatable; break;
            case tools::EffectClass::RemoteIrreversible: effect_class = EffectClass::RemoteIrreversible; break;
            case tools::EffectClass::InProcessUnknown: effect_class = EffectClass::InProcessUnknown; break;
        }
    }

    if (tool == nullptr) {
        tools::Tool::Result unknown{"未知工具: " + call.name, true};
        unknown.outcome = ToString(ToolOutcome::UnknownTool);
        unknown.error_code = kErrRegistryUnknownTool;
        finish(unknown, source_kind, source_instance, effect_class);
        return dispatch_done(call.id, call.name, std::move(unknown));
    }

    // tool_search(延迟挂载):注册表里查得到,但过滤谓词不放行——延迟工具
    // 还没挂载,或者这个角色用不上它(Explore 只读那类)。不当"未知工具"
    // 糊弄,给一条指路的友好错误:默认说"尚未挂载,先 tool_search";调用
    // 方另给了 filter_denial(角色限制)就照说——限制来自哪里,得看得见。
    // denial 支持 "稳定码|人话" 两截(动态工具 P1 起,proxy 模式用它报
    // tool_not_allowed 一类稳定码;老文案没有 '|' 走老口径,一字不差)。
    if (tool_filter && !tool_filter(*tool)) {
        std::string code = kErrRegistryNotMounted;
        std::string reason = filter_denial;
        const std::size_t code_split = filter_denial.find('|');
        if (code_split != std::string::npos && !filter_denial.substr(0, code_split).empty()) {
            code = filter_denial.substr(0, code_split);
            reason = filter_denial.substr(code_split + 1);
        }
        const std::string denial =
            reason.empty()
                ? "工具 " + call.name + " 存在但尚未挂载:请先用 tool_search 检索挂载,再调用。"
                : "工具 " + call.name + ": " + reason;
        tools::Tool::Result unavailable{denial, true};
        unavailable.outcome = ToString(ToolOutcome::Unavailable);
        unavailable.error_code = code;
        finish(unavailable, source_kind, source_instance, effect_class);
        return dispatch_done(call.id, call.name, std::move(unavailable));
    }

    // ---- 条件工具的 turn 级执行闸(动态工具 P2·§8.2):定义常驻 tools
    // 数组,不随轮次进出(tools hash 恒定);"这一轮可不可用"在此调用当口
    // 现判真实生命周期(goal iteration 活着?loop tick 在拍上?)。直名
    // 调用与经 tool_invoke 解引用来的调用同一道闸——工具不在暴露面之外
    // 不等于调用它会被放过。拒绝 = turn.tool_not_active 稳定码、终态
    // TurnGateDenied(单子 §十:等相应轮次或换路径,不得重试同一调用),
    // 不冒充"没挂载"、不冒充"用户拒绝"。空谓词 = 没有 turn 级条件工具
    //(子代理/单测/workflow/PTC/旧装配),行为与从前一字不差。
    if (turn_gate && !turn_gate(*tool)) {
        phase(runtime::ToolPhase::Blocked);
        std::string code = kErrTurnToolNotActive;
        std::string reason = turn_gate_denial;
        const std::size_t gate_split = turn_gate_denial.find('|');
        if (gate_split != std::string::npos && !turn_gate_denial.substr(0, gate_split).empty()) {
            code = turn_gate_denial.substr(0, gate_split);
            reason = turn_gate_denial.substr(gate_split + 1);
        }
        if (reason.empty()) {
            reason = "工具 " + call.name + " 的定义常驻,但只在对应的执行轮次里可用;当前轮不是。";
        }
        tools::Tool::Result inactive{reason, true};
        inactive.outcome = ToString(ToolOutcome::TurnGateDenied);
        inactive.error_code = code;
        finish(inactive, source_kind, source_instance, effect_class);
        return dispatch_done(call.id, call.name, std::move(inactive));
    }

    // ---- Plan 模式(只读研究硬闸单):ModePolicy 在 PreToolUse Hook 之前。
    // 拒绝时 Hook 不跑、确认不问、工具不执行——Plan 拒绝压过 Hook 与
    // Yolo(单子:先过 Plan capability gate,再过 Hook/schema,再过
    // permission)。终态 ModeDenied,错误码 mode.denied*(装配层给的稳定
    // 细码),不冒充"没挂载"也不冒充"用户拒绝"。
    if (wiring.on_mode_policy) {
        const std::string mode_denial = wiring.on_mode_policy(call.name, call.input);
        if (!mode_denial.empty()) {
            phase(runtime::ToolPhase::Blocked);
            // 回调交回的是"细码|人话"两截(细码给账,人话给模型与用户);
            // 只有一截就整段当人话,码退回通用 mode.denied。
            const std::size_t split = mode_denial.find('|');
            std::string code = runtime::kErrModeDenied;
            std::string reason = mode_denial;
            if (split != std::string::npos && !mode_denial.substr(0, split).empty()) {
                code = mode_denial.substr(0, split);
                reason = mode_denial.substr(split + 1);
            }
            tools::Tool::Result denied{reason, true};
            denied.outcome = ToString(ToolOutcome::ModeDenied);
            denied.error_code = code;
            denied.details = nlohmann::json{{"mode", "plan"}};
            finish(denied, source_kind, source_instance, effect_class);
            return dispatch_done(call.id, call.name, std::move(denied));
        }
    }

    // ---- 写前作用域闸(AGENTS.md 作用域单 P0/P1):ModePolicy 之后、
    // PreToolUse Hook 与用户确认之前——比打开写句柄、建目录、写临时文件
    // 都早(单子 §7.3 的次序硬规)。返回非空 = 拦截文案,按前缀分两档:
    //   握手([instructions_required]):该目标的 instruction chain 本 Agent
    //     尚未确认,规则全文已随文案注入,tool_result 落进 history,下一份
    //     请求模型读后原样重试即放行。第一次拦住是协议握手,不是错误,
    //     终态 ScopeGatePending + 稳定码 scope.instructions_required——
    //     不冒充"用户拒绝"、不冒充工具失败。
    //   fail closed([instructions_over_budget],P1):链整份装不进预算,
    //     拒收明说——终态 ScopeGateOverBudget + scope.instructions_over_
    //     budget,重试不放行,须拆规则或调大预算。
    // 不设回调 = 没装闸(单测/旧装配),行为与从前一字不差。
    if (wiring.on_scope_gate) {
        const std::optional<std::string> scope_denial = wiring.on_scope_gate(call.name, call.input);
        if (scope_denial.has_value() && !scope_denial->empty()) {
            phase(runtime::ToolPhase::Blocked);
            tools::Tool::Result gated{*scope_denial, true};
            const bool over_budget =
                scope_denial->rfind(tools::kScopeGateOverBudgetPrefix, 0) == 0;
            gated.outcome = ToString(over_budget ? ToolOutcome::ScopeGateOverBudget
                                                 : ToolOutcome::ScopeGatePending);
            gated.error_code =
                over_budget ? kErrScopeInstructionsOverBudget : kErrScopeInstructionsRequired;
            gated.details = nlohmann::json{
                {"gate", over_budget ? "instructions_over_budget" : "instructions_required"}};
            finish(gated, source_kind, source_instance, effect_class);
            return dispatch_done(call.id, call.name, std::move(gated));
        }
    }

    // ---- 动态工具 P1(proxy 调用的参数先验,单子 §5.5):tool_invoke 的
    // 顶层 schema 只声明 arguments 是个宽对象,细校验在这里做——拿目标工
    // 具当下那份真 schema 验一遍。不过 = invalid_target_arguments 稳定拒绝,
    // 不执行目标;模型该按发现结果里的 schema 修参数。普通调用不走这段
    //(工具自验的老合同不动),PreToolUse 改写后的复检也照旧在下面。
    if (proxy != nullptr) {
        if (const auto schema_error = tools::ValidateInputAgainstSchema(call.input, tool->input_schema());
            schema_error.has_value()) {
            phase(runtime::ToolPhase::Blocked);
            tools::Tool::Result rejected{"tool_invoke 的 arguments 未通过目标工具的真实 schema,已拒绝执行: " +
                                             *schema_error + "\n请按 tool_search 结果里的 input_schema 修正参数后重试。",
                                         true};
            rejected.outcome = ToString(ToolOutcome::SchemaRejected);
            rejected.error_code = tools::kErrToolRefInvalidArguments;
            rejected.details["transport_tool"] = proxy->transport_name;
            rejected.details["resolved_tool"] = call.name;
            rejected.details["tool_ref"] = proxy->tool_ref;
            finish(rejected, source_kind, source_instance, effect_class);
            return dispatch_done(call.id, call.name, std::move(rejected));
        }
    }

    // ---- PreToolUse:在 UI 标记"真执行"之前、权限确认之前。deny -> 拦;
    // ask -> 即使确认档放行也要问用户;allow -> 跳过用户确认(deny 规则
    // 与权限策略仍在确认回调里,钩子越不了权);updatedInput 只与 allow
    // 同返,先过一遍工具 schema,改写打回即拦。
    phase(runtime::ToolPhase::CheckingHook);
    runtime::ToolHookDecision pre;
    if (wiring.on_pre_tool_use_hook) {
        pre = wiring.on_pre_tool_use_hook(call.id, call.name, call.input);
    } else if (wiring.on_pre_tool_hook) {
        // 旧回调兼容:非空 = deny。
        const std::optional<std::string> legacy_blocked = wiring.on_pre_tool_hook(call.id, call.name, call.input);
        if (legacy_blocked.has_value()) {
            pre.decision = runtime::ToolHookDecision::Decision::Deny;
            pre.reason = *legacy_blocked;
        }
    }

    if (pre.decision == runtime::ToolHookDecision::Decision::Deny) {
        phase(runtime::ToolPhase::Blocked);  // 停在 blocked,不冒充"运行过又失败"
        tools::Tool::Result denied{pre.reason.empty() ? std::string("被 PreToolUse 钩子拦截") : pre.reason, true};
        denied.outcome = ToString(ToolOutcome::HookDenied);
        denied.error_code = kErrHookPreDenied;
        finish(denied, source_kind, source_instance, effect_class);
        for (const auto& ctx : pre.additional_context) {
            denied.AppendText("\n[钩子附注] " + ctx);
        }
        return dispatch_done(call.id, call.name, std::move(denied));
    }

    nlohmann::json effective_input = call.input;
    if (pre.updated_input.has_value()) {
        const auto schema_error = tools::ValidateInputAgainstSchema(*pre.updated_input, tool->input_schema());
        if (schema_error.has_value()) {
            // 钩子明确想改参,改出来的形状这工具不认——按拦截处理,不悄悄
            // 拿原参数跑出去(那是绕 schema 的路)。
            phase(runtime::ToolPhase::Blocked);  // 改写打回也是拦,同样停在 blocked
            tools::Tool::Result rejected{"PreToolUse 钩子改写入参未通过工具 schema,已拦截: " + *schema_error, true};
            rejected.outcome = ToString(ToolOutcome::SchemaRejected);
            rejected.error_code = kErrHookUpdatedInputInvalid;
            finish(rejected, source_kind, source_instance, effect_class);
            for (const auto& ctx : pre.additional_context) {
                rejected.AppendText("\n[钩子附注] " + ctx);
            }
            return dispatch_done(call.id, call.name, std::move(rejected));
        }
        effective_input = *pre.updated_input;
    }

    if (tool->needs_confirm()) {
        runtime::PermissionVerdict permission;
        if (wiring.on_permission_evaluate) {
            permission = wiring.on_permission_evaluate(call.id, call.name, tool->approval_class(),
                                                       effective_input, pre);
        }
        if (permission.action == runtime::PermissionVerdict::Action::Deny) {
            phase(runtime::ToolPhase::Blocked);
            const bool command_denied = permission.reason == runtime::PermissionVerdict::Reason::CommandDenied;
            // 不询问档/策略黑名单在预裁定阶段直接拒绝时，也让装配层提供
            // 场景化文案。后台子代理借此如实说明“没有审批口、未预放行”，
            // 而不是退回通用的不询问档文案；结构化 outcome/error_code 不变。
            const std::string denial =
                wiring.on_tool_denial_text
                    ? wiring.on_tool_denial_text(call.id, call.name)
                    : (command_denied
                           ? (call.name + " 命中 deny_commands，已被权限策略直接拒绝，本次未执行。")
                           : ("当前为“不询问”档；" + call.name +
                              " 需要授权，本次未执行。请切回可询问档，或先在权限配置中明确放行。"));
            tools::Tool::Result declined{denial, true};
            declined.outcome = ToString(ToolOutcome::PermissionDeclined);
            declined.error_code = kErrPermissionNoPromptDenied;
            declined.details["gate"] = "permission";
            declined.details["reason"] = command_denied ? "deny_commands" : "no_prompt_denied";
            declined.details["mode"] = "dont_ask";
            declined.details["tool"] = call.name;
            declined.details["deny_hit"] = permission.deny_hit;
            finish(declined, source_kind, source_instance, effect_class);
            return dispatch_done(call.id, call.name, std::move(declined));
        }
        if (permission.action == runtime::PermissionVerdict::Action::Allow) {
            // 显式预授权或档位自动放行，绝不进入 PermissionRequest/前端确认。
        } else {
        phase(runtime::ToolPhase::WaitingPermission);
        // P2(显示系统剥离单):异步审批通道优先——发 runtime::ApprovalRequest 拿
        // future,原地 Wait。终端前端的 future 实现是"当场问完再给结果"
        // (Wait 立即返回,与今日同步 on_tool_confirm 一字不差);远端前端
        // 的实现悬起 request_id,这里阻塞等,事件泵/连接线程不跟堵。
        // async 缺位回落到旧同步回调(子代理转发、单测、后台"没人可问"
        // 的短路都还在旧路上,不许变慢、不许多线程化)。
        bool allowed = true;
        if (wiring.on_tool_confirm_async) {
            const std::shared_ptr<runtime::InteractionFuture> future =
                wiring.on_tool_confirm_async(
                    runtime::ApprovalRequest{call.id, call.name, effective_input, std::string()});
            const std::optional<runtime::ApprovalResponse> response =
                future != nullptr ? future->WaitApproval() : std::nullopt;
            if (!response.has_value()) {
                // 悬空收口(cancel):等价拒绝,拒绝文案照"没人可答"写,
                // 不冒充用户拒绝。
                const std::string denial =
                    wiring.on_tool_denial_text ? wiring.on_tool_denial_text(call.id, call.name)
                                               : std::string("审批请求悬空收口,未执行该工具");
                tools::Tool::Result cancelled{denial, true};
                cancelled.outcome = ToString(ToolOutcome::PermissionDeclined);
                cancelled.error_code = kErrPermissionDeclined;
                finish(cancelled, source_kind, source_instance, effect_class);
                return dispatch_done(call.id, call.name, std::move(cancelled));
            }
            switch (response->decision) {
                case runtime::InteractionDecision::Accept:
                case runtime::InteractionDecision::AcceptForSession:
                    allowed = true;
                    break;
                case runtime::InteractionDecision::Decline:
                case runtime::InteractionDecision::Cancel:
                    allowed = false;
                    break;
            }
        } else {
            allowed =
                wiring.on_tool_confirm ? wiring.on_tool_confirm(call.id, call.name, effective_input) : true;
        }
        if (!allowed) {
            // 拒绝文案可由回调层给(后台子代理的拒绝是"无法弹确认、未预放
            // 行",不是用户拒绝——缺省文案会把子代理的最终报告带偏成"均被
            // 用户拒绝")。
            const std::string denial = wiring.on_tool_denial_text
                                           ? wiring.on_tool_denial_text(call.id, call.name)
                                           : std::string("用户拒绝执行该工具");
            tools::Tool::Result declined{denial, true};
            declined.outcome = ToString(ToolOutcome::PermissionDeclined);
            declined.error_code = kErrPermissionDeclined;
            finish(declined, source_kind, source_instance, effect_class);
            return dispatch_done(call.id, call.name, std::move(declined));
        }
        }
    }

    phase(runtime::ToolPhase::Running);
    // 逐枚追踪:durable started 在 Tool::execute 前发射(副作用边界之前)。
    // 落不落得住由 sink 决定(持久 sink append+flush;装配层对副作用工具
    // 会把"写不成就拦执行"的闸装在 sink 里);这里只保证事件先于 execute。
    {
        ToolTraceEvent started;
        started.kind = ToolTraceEventKind::ExecutionStarted;
        started.effective_input_sha256 = hooks::Sha256Hex(effective_input.dump());
        started.effect_class = effect_class;
        started.source_kind = source_kind;
        started.source_instance = source_instance;
        // 实际执行的入参原文(轨迹接线:tool.input.effective 需要正文,
        // 老路只吃摘要;is_object 守门——null(没改写的空入参)不落)。
        if (effective_input.is_object()) {
            started.effective_arguments = effective_input;
        }
        emit(std::move(started));
    }
    // 副作用闸:started 落不住的副作用工具,这里拦(单子:写不成时,
    // 副作用工具不得继续执行)。拦下的以 result_store_failed 收尾——不
    // 冒充工具失败,也不冒充成功;模型与恢复账都看得出是宿主拦的。
    if (trace != nullptr && wiring.on_tool_trace_blocked &&
        wiring.on_tool_trace_blocked(trace->execution_id)) {
        tools::Tool::Result blocked_by_trace{"追踪账写盘失败,该工具未执行(副作用档默认拦截)", true};
        blocked_by_trace.outcome = ToString(ToolOutcome::ResultStoreFailed);
        blocked_by_trace.error_code = kErrSessionTraceAppendFailed;
        // finished 栅栏已在 hub 侧落过(拦截时补的 terminal 行),这里只
        // 走展示与返回,不再发第二枚。
        return dispatch_done(call.id, call.name, std::move(blocked_by_trace));
    }
    // 取消旗随调用递进(子代理 x 停止失效单):共享工具实例上没有"这一
    // 次"的取消源——SetCancel 灌的是装配层那根(主回合 ESC),子代理的
    // CancelChain 合并旗到不了那里。不肯合作取消的工具无视 context、行为
    // 不变;肯合作的(run_command/Lua/插件)置位即收,不再等到超时。
    tools::Tool::Result result = tool->execute(effective_input, tools::ToolExecutionContext{cancel, wiring.tool_artifact_dir});
    // PostToolUse(新):结果先清洗成合法 UTF-8 再给钩子;钩子的反馈追加进
    // 模型所见 tool_result,原始结果照旧进审计(副作用已发生,不能撤销,
    // 也不冒充撤销)。旧回调照旧吃它一贯拿到的结果。
    if (!platform::IsValidUtf8(result.content)) {
        result.SanitizeInPlace();
    }
    // finished 栅栏在 PostToolUse 之前:原始 outcome 与结果正文先落账,
    // Hook 崩溃抹不掉"工具已完成"的事实;钩子追加的文本只进模型所见,
    // trace 里的 result_ref 记的是追加前的原始结果(两份 digest 分得开,
    // 恢复时能判断 Hook 到底改了什么)。
    finish(result, source_kind, source_instance, effect_class);
    if (wiring.on_post_tool_use_hook) {
        const std::vector<std::string> feedback =
            wiring.on_post_tool_use_hook(call.id, call.name, effective_input, result);
        for (const auto& line : feedback) {
            result.AppendText("\n[post-tool-use hook 追加] " + line);
        }
    }
    if (wiring.on_post_tool_hook) {
        wiring.on_post_tool_hook(call.id, call.name, effective_input, result);
    }
    return dispatch_done(call.id, call.name, std::move(result));
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

// ---------------------------------------------------------------------------
// 服务端工具搜索(动态工具 P3):结果块内容 -> 显示卡的摘要与错误位。
// content 是 wire 的嵌套原文:tool_search_tool_search_result(tool_references
// 名单)或 tool_search_tool_result_error(error_code/error_message)。这里只折
// 给人看的一句,历史块本身一行不动(单子 §7.2 无损保存)。
// ---------------------------------------------------------------------------
std::string ServerToolSearchSummary(const nlohmann::json& content) {
    if (!content.is_object()) {
        return "服务端工具搜索完成";
    }
    const std::string type = content.value("type", std::string());
    if (type == "tool_search_tool_result_error") {
        return "服务端工具搜索失败: " + content.value("error_code", std::string("unknown")) + " " +
               content.value("error_message", std::string());
    }
    if (content.contains("tool_references") && content["tool_references"].is_array()) {
        std::string names;
        for (const auto& reference : content["tool_references"]) {
            if (reference.is_object() && reference.contains("tool_name") && reference["tool_name"].is_string()) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += reference["tool_name"].get<std::string>();
            }
        }
        if (names.empty()) {
            return "服务端工具搜索:没有命中的工具";
        }
        return "服务端工具搜索发现 " + std::to_string(content["tool_references"].size()) + " 枚工具: " + names;
    }
    return "服务端工具搜索完成";
}

bool ServerToolSearchFailed(const nlohmann::json& content) {
    return content.is_object() && content.value("type", std::string()) == "tool_search_tool_result_error";
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

// 预算软线催办(真机实测 P2-1:只读审查 34 次工具调用仍停在"等首字节"):
// 任一成本硬线跨过软线时注入一次,要求模型基于现有证据收尾。与步数将尽
// 提醒同一注入点、同一份追加律;两条共用一面"已催"旗,一次 Run 至多一条。
std::string BuildBudgetSoftNudgeText() {
    return "\n\n[系统提醒] 本任务的预算已过大半,请基于现有证据收尾:不要再开新的调查方向;"
           "把已经查到的事实与关键证据位置整理成结论交回;确有未尽事项,作为后续建议列出,"
           "不要继续展开。";
}

// 三根硬线任一跨过软线?(纯函数,单测钉)各硬线 0 = 未设该线,不参与。
bool CrossesBudgetSoftLine(int steps_used, int max_steps_per_turn, std::int64_t tokens_seen,
                           std::int64_t max_total_tokens, std::int64_t elapsed_ms, std::int64_t max_wall_ms,
                           int soft_percent) {
    if (soft_percent <= 0) {
        return false;  // 不催:只留硬闸
    }
    if (max_steps_per_turn > 0 &&
        static_cast<std::int64_t>(steps_used) >= BudgetSoftLine(max_steps_per_turn, soft_percent)) {
        return true;
    }
    if (max_total_tokens > 0 && tokens_seen >= BudgetSoftLine(max_total_tokens, soft_percent)) {
        return true;
    }
    if (max_wall_ms > 0 && elapsed_ms >= BudgetSoftLine(max_wall_ms, soft_percent)) {
        return true;
    }
    return false;
}

std::expected<RunOutcome, std::string> AgentLoop::Run(Agent& agent, api::Message user_message,
                                                       const TurnWiring& wiring,
                                                       const std::atomic<bool>* cancel) {
    api::Backend& backend_ = agent.backend_;
    tools::ToolRegistry& registry_ = agent.registry_;
    const std::string& model_ = agent.profile_.request.model;
    std::string& system_prompt_ = agent.system_prompt_;
    std::string& active_turn_context_ = agent.active_turn_context_;
    bool& run_active_ = agent.run_active_;
    const AgentRuntimeProfile& profile_ = agent.profile_.runtime;
    ContextManager& context_ = agent.context_;
    const std::function<bool(const tools::Tool&)>& tool_filter_ = agent.profile_.tool_filter;
    const std::string& tool_filter_denial_ = agent.profile_.tool_filter_denial;
    // 动态工具 P1(通用 ProxyReference):代理引用的解引用器与执行资格。
    // resolver 为空 = 本 Agent 没开 proxy 模式,tool_invoke 调用照"未知/
    // 注册表里的壳工具"处理,行为与从前一字不差。
    const std::shared_ptr<tools::DeferredToolResolver>& tool_ref_resolver_ = agent.profile_.tool_ref_resolver;
    const std::function<bool(const tools::Tool&)>& tool_execution_policy_ = agent.profile_.tool_execution_policy;
    // 动态工具 P2(条件工具也守恒):turn 级执行闸。直名调用与代理解引用
    // 调用同一道;空 = 本 Agent 没有 turn 级条件工具,两处调用照旧。
    const std::function<bool(const tools::Tool&)>& tool_turn_gate_ = agent.profile_.tool_turn_gate;
    const std::string& tool_turn_gate_denial_ = agent.profile_.tool_turn_gate_denial;
    const AgentWiring& wiring_ = agent.wiring_;
    int& batch_counter_ = agent.batch_counter_;
    const auto BuildToolDefinitions = [&agent]() { return agent.BuildToolDefinitions(); };
    const auto issue_execution_id = [&agent]() { return agent.issue_execution_id(); };

    if (user_message.role != api::Role::User || user_message.content.empty()) {
        return std::unexpected("用户消息为空，无法发送。");
    }
    run_active_ = true;
    active_turn_context_ = agent.turn_context_;
    struct RunStateGuard {
        bool& active;
        std::string& context;
        ~RunStateGuard() {
            active = false;
            context.clear();
        }
    } run_state_guard{run_active_, active_turn_context_};

    // 一轮用户输入落双账:durable 是真输入;动态上下文(记忆召回/任务
    // 名册)只随本轮 user 进请求视图——发过即钉住,不再每回合改 system
    // 制造分叉点。持久 history_ 不收这块,session/export/compact/记忆抽取
    // 都只见用户真输入。
    api::Message durable_user_message = user_message;
    if (!active_turn_context_.empty()) {
        user_message.content.push_back(api::TextBlock{active_turn_context_});
    }
    context_.PushUserTurn(std::move(durable_user_message), std::move(user_message));

    // 步数与 stop reason 的活账:每次模型请求(每个 step)各记一笔,收场时随
    // RunOutcome 交出去——上层(子代理)按它分型 budget_exhausted/no_final_text
    // 等,不再靠解析错误文案猜。
    int steps_used = 0;
    std::string last_stop_reason;
    // 成本刹车活账(真机实测 P2-6):本 Run 的墙钟起算点与累计 token(完整
    // 输入 + 输出,与台账/面板同一口径)。三根硬线(步数在上面 profile_ 里)
    // 每个步顶查一次,软线催办也吃这两笔账。
    const auto run_started = std::chrono::steady_clock::now();
    std::int64_t tokens_seen = 0;
    const std::int64_t max_wall_ms =
        profile_.max_wall_secs > 0 ? static_cast<std::int64_t>(profile_.max_wall_secs) * 1000 : 0;
    // 催办只此一条:步数将尽提示与预算软线催办共用这面旗(规格"重复念叨
    // 只会把剩余步数也烧掉"同一笔账),一次 Run 至多注入一次。
    bool budget_nudged = false;
    // 应急收窄的收尾交代(派工单 §四)也只注入一次:预留收窄会持续生效到
    // Run 收口,念叨一遍够了。
    bool context_wrapup_nudged = false;
    // 回合视觉收束(终端回合视觉收束单):本 Run() 已发出的批次序号
    // (on_tool_batch_started 的 batch_index 用;每个含工具的 step 消耗
    // 一枚,跨 step 不重号)。
    std::size_t batches_emitted = 0;
    // 前缀记账(agent/prefix.hpp):本步请求与上一份的追加律判定结果,
    // 发请求前算好,报 usage 时随 UsageReport 交出去。诊断账(问题 9)
    // 在同一处攒:epoch/稳定前缀/指纹 hash,发 usage 时一并交出去。
    bool step_prefix_append_only = true;
    std::string step_epoch_break_reason;
    ContextManager::PrefixAccount step_prefix_account;
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
        // 回合视觉收束:step 边界。请求还没发,先报"这一拍开始了"——
        // 界面(工具批次分组)凭它知道上一批已换拍。没设回调零影响。
        if (wiring.events != nullptr) {
            wiring.events->OnModelStepStarted(step_index);
        }
        // 跨会话传话的安全收件点:工具结果已攒完、下一次请求尚未发出——
        // 正是"不打断工具、步边界收信"的那个缝。step_index==0 不收:这一步
        // 的用户消息刚落下,空闲路径(main.cpp)在起 Run 之前已经收过一趟,
        // 纯文本轮的信该"先排住,本 turn 收口后再开一轮"(规格),不抢跑。
        // 回调只在主线程这里被调,流式/确认当口绝不会碰它,来信自然
        // 不可能替用户答确认。
        if (wiring_.inbox && step_index > 0) {
            while (auto incoming = wiring_.inbox()) {
                context_.InjectIncoming(std::move(*incoming));
            }
        }

        // 成本硬线(真机实测 P2-6):时间/token 两根在步顶查——步数那根由
        // 循环条件执法。断线即收场:不是错误,history 里留着到限为止的全部
        // 来回,部分结果由调用方按 budget_exhausted 带走,缘由写明哪根线断。
        if (profile_.max_wall_secs > 0 || profile_.max_total_tokens > 0) {
            const std::int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - run_started)
                                                .count();
            const bool over_wall = max_wall_ms > 0 && elapsed_ms >= max_wall_ms;
            const bool over_tokens = profile_.max_total_tokens > 0 && tokens_seen >= profile_.max_total_tokens;
            if (over_wall || over_tokens) {
                RunOutcome outcome{false, false, false, last_stop_reason, steps_used};
                outcome.hit_time_budget = over_wall;
                outcome.hit_token_budget = over_tokens;
                outcome.output_budget = budget_report;
                return outcome;
            }
        }

        api::Request request;
        request.model = model_;
        request.system = system_prompt_;
        // 皮上的会话级叠层就地生效(批四·病十一其三:五层请求改写后端
        // 退役):延迟索引段 -> 模型目录指令 -> 魂,拼装次序与从前传输层
        // 包装的次序一字不差(索引在前、指令居中、魂压轴)。从前这些改动
        // 发生在指纹算完之后,前缀账看不见;现在进了指纹,model/system 一
        // 动 epoch 如实断,账不再瞎。
        // Token 账本单 A1(ResolvedPromptBuilder):接了底账(construction/
        // /clear 重拼时一并算,见 agent/agent.hpp 的 AgentProfile.
        // resolved_prompt_base)且底账文本仍与 system_prompt_ 同步(没被
        // SetSystemPrompt 之类的活口越过 ResolvedPromptBuilder 直改),就
        // 走完整解析产 manifest;否则走原三行——不接线的路径、或底账已
        // 与当前系统提示错位的少数口(如 /worktree 重拼),文本与从前
        // 逐字节一致,只是没有 manifest。
        std::optional<PromptManifest> request_prompt_manifest;
        const bool resolved_prompt_in_sync = agent.profile_.resolved_prompt_base.has_value() &&
                                             agent.profile_.resolved_prompt_base->text == system_prompt_;
        if (resolved_prompt_in_sync) {
            const std::string deferred_index_segment =
                agent.profile_.deferred_index_provider ? agent.profile_.deferred_index_provider() : std::string();
            AssembledPrompt assembled = ResolveFinalPrompt(
                *agent.profile_.resolved_prompt_base, deferred_index_segment, agent.profile_.model_instructions,
                agent.profile_.soul, agent.profile_.soul.empty() ? std::string() : std::string("custom"));
            request.system = std::move(assembled.text);
            request_prompt_manifest = std::move(assembled.manifest);
        } else {
            if (agent.profile_.deferred_index_provider) {
                request.system =
                    WithDeferredToolsIndex(request.system, agent.profile_.deferred_index_provider());
            }
            request.system = WithModelInstructions(request.system, agent.profile_.model_instructions);
            request.system = WithSoul(request.system, agent.profile_.soul);
        }
        api::ApplyRequestProfile(request, agent.profile_.request);
        // 每轮现拼:tool_search 可能刚在上一拍挂载新工具。压力预估与最终
        // 发送必须吃同一份定义，免得先估旧表、后发新表。
        request.tools = BuildToolDefinitions();
        // 第一拍的新消息不可压。system、工具表与它自己已加输出预留越窗时，
        // 先报错，连自动 compact 回调都不叫；压旧历史救不了这笔固定账。
        if (step_index == 0 && profile_.context_window_tokens > 0 && !context_.request_history().empty()) {
            std::size_t fixed_input_tokens = EstimateTextTokensForPreflight(request.system) +
                                             EstimateMessageTokensForPreflight(context_.request_history().back());
            for (const auto& tool : request.tools) {
                fixed_input_tokens += EstimateTextTokensForPreflight(tool.name) +
                                      EstimateTextTokensForPreflight(tool.description) +
                                      EstimateTextTokensForPreflight(tool.input_schema.dump());
            }
            const OutputBudget output_budget{profile_.max_output_tokens, profile_.max_output_tokens_source};
            const std::size_t output_tokens = static_cast<std::size_t>(output_budget.reserve_for_estimate());
            if (ExceedsContextWindow(fixed_input_tokens, output_tokens, profile_.context_window_tokens)) {
                return std::unexpected(
                    "上下文预检未通过:当前消息与固定提示约 " + std::to_string(fixed_input_tokens) +
                    " token + 输出预留 " + std::to_string(output_tokens) + " + 协议余量 " +
                    std::to_string(kContextPreflightHeadroomTokens) + "，超过窗口 " +
                    std::to_string(profile_.context_window_tokens) +
                    "。当前消息本身已装不下，压缩旧历史也无济于事；请缩短输入或调低输出上限。");
            }
        }
        // 步数将尽提醒(第五期)+ 预算软线催办(P2-1/P2-6):固定文案,在触发
        // 条件第一次成立的那一步追加进尚未发出的尾部消息(user 输入或刚攒完
        // 的 tool result),随 history 留住——不改 system,不撤旧提醒,追加律
        // 不破。两条共用 budget_nudged:一次 Run 至多注入一条,先到先得。
        // turn 预算单 §9.2:装了任务级 turn 门的(子代理/workflow 节点),
        // 判定换成任务 remaining——认领口在预算账里,一任务恰一次,跨 Run
        // 不重发;没装门的(主会话/旧调用方)照旧按单轮 step 判,一字不动。
        if (!budget_nudged && !context_.durable_history().empty()) {
            const bool turn_gate_armed = wiring.turn_budget != nullptr && wiring.turn_budget->armed();
            const bool nudge_task_turns =
                turn_gate_armed && wiring.turn_budget->claim_turn_nudge && wiring.turn_budget->claim_turn_nudge();
            const bool nudge_run_steps =
                !turn_gate_armed && ShouldNudgeStepLimit(step_index, profile_.max_steps_per_turn);
            if (nudge_run_steps || nudge_task_turns) {
                const api::TextBlock nudge{BuildStepLimitNudgeText()};
                context_.AppendToLast(nudge);
                budget_nudged = true;
            } else if (CrossesBudgetSoftLine(steps_used, profile_.max_steps_per_turn, tokens_seen,
                                             profile_.max_total_tokens,
                                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - run_started)
                                                 .count(),
                                             max_wall_ms, profile_.budget_soft_percent)) {
                const api::TextBlock nudge{BuildBudgetSoftNudgeText()};
                context_.AppendToLast(nudge);
                budget_nudged = true;
            }
        }

        // mid-turn 安全点:先拼好真请求要发的那副工作视图(无损结构压缩 +
        // 字符安全网 + sticky,全在 ContextManager;真丢了东西的因它自己记
        // 进前缀账),projected 就拿这副视图估——口径归一(压缩触发失衡单
        // §二.A):估的和发的是同一副牌,不再走 BuildPressureDryRunView 单独
        // 虚算。旧路的两笔账分家:P1-1 之前拿未压缩全量估(真请求 47k 估出
        // 189k);P1-1 之后虽与工作视图同样去重,但不走字符安全网与 sticky,
        // 且量它的尺是"临出门"的保守托底尺——短词密集的工具输出(副作用
        // 工具的重复结果判重不开门,结构压缩收不走)逐词计数能虚出日常尺
        // 两倍,叠加按真实口径标定的 80% 参考线,触发线实际落在真实水位
        // ~25%:用户真机 61.5k/256k(24%)被喊溢出,压完"没有冷区榨不出
        // 收益"空跑收场。
        // 双闸(§二.B):projected 过 kProjectedOverflowPercent 参考线之外,
        // 真实水位(同一副工作视图按日常尺 EstimateHistoryTokens,与 /context
        // 显示同一把)也须过 kRealOverflowPercent——虚算单独不触发;真实
        // 水位真到线上,该压的仍压。上层回调里可以同步做一次语义压缩
        // (ReplaceHistory 开新 epoch),返回后 epoch 断了就按新史重拼——
        // 发出去的仍是(可能已换短的)那份请求视图。窗口未知(0)或没设
        // 回调时只拼视图不评估,行为与从前一致——这一步不发出任何请求。
        ContextWorkingView working_view = context_.BuildWorkingView({profile_.max_context_chars});
        if (profile_.context_window_tokens > 0 && wiring_.on_context_pressure) {
            // 输出上限纳入 projected 计算(规格根因一):声明了用声明值,
            // unset 用保守估计(kUnsetOutputReserveEstimateTokens)——服务端
            // 默认上限拿不到准数,宁可早压不撞墙。输出预留单独保留:那是
            // 真会占窗口的空间;防"下一轮工具结果再涨"也靠它,不再对整段
            // 历史做倍率虚算(重复结果的去重由工作视图的结构压缩负责)。
            const OutputBudget output_budget{profile_.max_output_tokens, profile_.max_output_tokens_source};
            std::size_t projected = EstimateTextTokensForPreflight(request.system) +
                                    EstimateHistoryTokensForPreflight(working_view.messages) +
                                    static_cast<std::size_t>(output_budget.reserve_for_estimate());
            for (const auto& tool : request.tools) {
                projected += EstimateTextTokensForPreflight(tool.name) +
                             EstimateTextTokensForPreflight(tool.description) +
                             EstimateTextTokensForPreflight(tool.input_schema.dump());
            }
            // B 闸的真实水位:同一副工作视图,换日常尺(全库统一的
            // EstimateUtf8Tokens 口径,无逐词托底)——projected 那把托底尺
            // 量"最坏情况放不放得下"是对的,量"现在真实用了多少"会把
            // 短词密集的历史虚抬两倍;水位与 /context 同尺,账才对得上。
            const std::size_t working_view_tokens = EstimateHistoryTokens(working_view.messages);
            const std::size_t projected_line =
                profile_.context_window_tokens * static_cast<std::size_t>(kProjectedOverflowPercent) / 100;
            const std::size_t real_line =
                profile_.context_window_tokens * static_cast<std::size_t>(kRealOverflowPercent) / 100;
            ContextPressure pressure;
            pressure.phase = ContextPressure::Phase::PreRequest;
            pressure.projected_tokens = projected;
            pressure.window_tokens = profile_.context_window_tokens;
            pressure.working_view_tokens = working_view_tokens;
            pressure.working_view_overflow = working_view_tokens >= real_line;
            pressure.projected_overflow = projected >= projected_line && pressure.working_view_overflow;
            const int epoch_before = context_.cache_epoch();
            wiring_.on_context_pressure(pressure);
            if (context_.cache_epoch() != epoch_before) {
                // 回调里真换了史(midturn compact 走 ReplaceHistory):按新史
                // 重拼工作视图——ReplaceHistory 已清决策台账与 sticky,重拼
                // 从头定形,与旧时序(压完再 BuildWorkingView)同一副牌。
                working_view = context_.BuildWorkingView({profile_.max_context_chars});
            }
        }
        request.messages = working_view.messages;
        const TrimReport& trim_report = working_view.trim;
        // 编码关口(兜底前的主动闸):消息内容上 wire 前统一过一遍清洗,
        // 合法内容零成本原样返回。旧会话档/管道输入/外部文本带进来的
        // 坏串到这里就该被洗掉——四个 wire client 的 dump() 316 兜底
        // 只洗发往网络的拷贝,内存里的坏串留驻会每轮重打日志,这里才是
        // 治本的一刀。
        for (auto& message : request.messages) {
            api::SanitizeMessage(message);
        }
        // 工具结果图片回喂(工具结果图片回喂单):请求副本上把工具图从
        // artifact 落盘重灌成 base64,四家 wire 据此上原生图块。位置有讲
        // 究——必须在 sanitize(洗投影文本)之后、前缀记账(算请求指纹)
        // 之前:同 artifact 灌出的 base64 逐轮一致,指纹不因重灌而断 epoch;
        // durable history 不沾 base64(SanitizeMessage/存档都在副本之外),
        // 会话档案不膨胀。超帽/文件丢了的图自动留在文本降级路,不报错。
        if (!wiring.tool_artifact_dir.empty()) {
            const std::size_t rehydrated_images =
                RehydrateToolResultImages(request, wiring.tool_artifact_dir);
            if (rehydrated_images > 0 && PrefixDebugEnabled()) {
                platform::LogSink::Instance().Debug(
                    "loop", "[tool-image] 重灌 " + std::to_string(rehydrated_images) +
                                " 张工具图随请求上 wire");
            }
        }
        request.max_tokens = profile_.max_output_tokens;  // nullopt = unset,交服务端默认
        // 有损硬裁发生了(丢轮/截结果),显式通报——静默降级会让用户以为语
        // 义压缩已成功,模型其实已经看不到那段原文了。
        if ((trim_report.trimmed_turns || trim_report.truncated_results) && wiring_.on_context_pressure) {
            ContextPressure pressure;
            pressure.phase = ContextPressure::Phase::AfterHardTrim;
            pressure.hard_trimmed_turns = trim_report.trimmed_turns;
            pressure.hard_dropped_messages = trim_report.dropped_messages;
            pressure.hard_truncated_results = trim_report.truncated_results;
            pressure.window_tokens = profile_.context_window_tokens;
            wiring_.on_context_pressure(pressure);
        }
        // 有损硬裁真出手了:下一份请求的工作视图换了裁剪形状,前缀记账给
        // 它点名(指纹 diff 只能报 old_message_changed,这里的因更准)。
        if (trim_report.trimmed_turns || trim_report.truncated_results) {
            context_.NotePendingEpochBreak("hard_trim");
        }
        // 硬上限:轮级裁剪 + 工具结果截断都做完还是装不下(比如单条用户输入
        // 就超大),明确报错,不把一份注定被拒的超大请求发出去。
        if (EstimateHistoryBytes(request.messages) > profile_.max_context_chars) {
            return std::unexpected("上下文超过上限(" + std::to_string(profile_.max_context_chars) +
                                    " 字符),裁剪与截断后仍装不下,无法发送。请用 /compact 压缩历史,或开新会话。");
        }

        // token 窗口的最后一道硬闸。上面的压力回调已经有机会压缩；回来后
        // 仍是“输入估算 + 输出预留 + 协议余量 > 窗口”时,先试应急预留
        //(派工单 §四):当前消息自己装得下、只是常规预留太肥的,把本请求
        // 的输出上限收窄到一个小而正的值放行——同任务继续跑,并注入一次
        // 收尾交代。应急也装不下(当前消息本身过大/历史真满了)才就地报错。
        // 尤其是当前单条用户消息本身过大时，摘要压多少遍也救不了，不能再
        // 把同一份请求发给 provider 撞 500。
        if (profile_.context_window_tokens > 0) {
            const OutputBudget output_budget{profile_.max_output_tokens, profile_.max_output_tokens_source};
            const std::size_t input_tokens = EstimateRequestInputTokensForPreflight(request);
            const std::size_t window_tokens = profile_.context_window_tokens;
            const std::size_t current_turn_tokens =
                request.messages.empty() ? 0 : EstimateMessageTokensForPreflight(request.messages.back());
            std::size_t output_tokens = static_cast<std::size_t>(output_budget.reserve_for_estimate());
            // 三项账进可观测事件(派工单 §4.4):estimated_input + reserved_
            // output + protocol_margin,判定处当场发,不等问题发生后再翻账。
            const auto emit_preflight = [&](std::size_t used_reserve, bool clamped) {
                platform::LogSink::Instance().Info(
                    "loop", "[context-preflight] estimated_input=" + std::to_string(input_tokens) +
                                " reserved_output=" + std::to_string(used_reserve) +
                                " protocol_margin=" + std::to_string(kContextPreflightHeadroomTokens) +
                                " window=" + std::to_string(window_tokens) +
                                (clamped ? " action=reserve_clamped" : " action=exceeded"));
                ContextPressure pressure;
                pressure.phase = ContextPressure::Phase::PreflightExceeded;
                pressure.estimated_input_tokens = input_tokens;
                pressure.reserved_output_tokens = used_reserve;
                pressure.protocol_headroom_tokens = kContextPreflightHeadroomTokens;
                pressure.window_tokens = window_tokens;
                pressure.reserve_clamped = clamped;
                // trajectory 是当轮/当 run 的边界账，不能依赖宿主是否另接了
                // 长生命周期的压力回调；拒绝分支不会走 model.request.prepared，
                // 因此必须在判定当场独立落。
                if (wiring.boundary_recorder != nullptr) {
                    wiring.boundary_recorder->OnContextPressure(pressure);
                }
                if (wiring_.on_context_pressure) {
                    wiring_.on_context_pressure(pressure);
                }
            };
            if (ExceedsContextWindow(input_tokens, output_tokens, window_tokens)) {
                const std::size_t emergency = EmergencyOutputReserveTokens(window_tokens);
                if (!ExceedsContextWindow(current_turn_tokens, emergency, window_tokens) &&
                    !ExceedsContextWindow(input_tokens, emergency, window_tokens)) {
                    // 应急放行:本请求按小预留发,注入一次收尾交代(已跑命令/
                    // 关键结果/未提交改动/后续建议)——durable handoff 的正文
                    // 由模型产出,同任务续跑而不是叫用户开新会话。
                    request.max_tokens = static_cast<int>(emergency);
                    output_tokens = emergency;
                    emit_preflight(output_tokens, /*clamped=*/true);
                    if (!context_wrapup_nudged) {
                        const api::TextBlock nudge{BuildContextWrapupNudgeText(emergency)};
                        context_.AppendToLast(nudge);
                        if (!request.messages.empty()) {
                            request.messages.back().content.push_back(nudge);
                        }
                        context_wrapup_nudged = true;
                    }
                } else {
                    emit_preflight(output_tokens, /*clamped=*/false);
                    const bool current_turn_alone_overflows =
                        ExceedsContextWindow(current_turn_tokens, emergency, window_tokens);
                    return std::unexpected(
                        "上下文预检未通过:输入约 " + std::to_string(input_tokens) + " token + 输出预留 " +
                        std::to_string(output_tokens) + " + 协议余量 " +
                        std::to_string(kContextPreflightHeadroomTokens) +
                        "，超过窗口 " + std::to_string(window_tokens) +
                        (current_turn_alone_overflows
                             ? "。当前消息本身已装不下，压缩旧历史也无济于事；请缩短输入或调低输出上限。"
                             : "。自动压缩后仍装不下；请开新会话、缩短输入，或调低输出上限。") +
                        " 现场不丢:已完成的工具结果与最后检查点已随任务保留,可据此续派同一任务。");
                }
            }
        }

        // 前缀记账:与上一份请求比追加律。断了就开新 cache epoch,并点名
        // 断因——loop 自己知道的因(compact/hard trim)比指纹反推的准,
        // 优先用它;没有显式因就按 diff 点名(model/system/tools/旧消息)。
        // epoch 断不是失败,无名无姓地断才是失败(agent/prefix.hpp)。
        // 诊断模式(LUBANCODE_DEBUG_PREFIX)才把请求按 wire 序列化一份
        // 递进去量公共前缀字节;常态传 nullptr,不做全序列化。
        {
            std::string wire_dump;
            const std::string* wire_dump_ptr = nullptr;
            if (PrefixDebugEnabled()) {
                wire_dump = backend_.SerializeForDiagnostics(request);
                if (!wire_dump.empty()) {
                    wire_dump_ptr = &wire_dump;
                }
            }
            step_prefix_account = context_.AccountRequest(request, wire_dump_ptr);
            step_prefix_append_only = step_prefix_account.append_only;
            step_epoch_break_reason = step_prefix_account.break_reason;
            if (step_prefix_account.had_previous && PrefixDebugEnabled()) {
                // 诊断行:只带断因/位置/条数,不带任何正文与 hash 以外的东西。
                std::string prefix_diag = "[prefix] epoch " + std::to_string(step_prefix_account.cache_epoch) +
                                          " step " + std::to_string(step_index) + " append_only=" +
                                          (step_prefix_append_only ? "true" : "false");
                if (!step_prefix_append_only) {
                    prefix_diag += " reason=" + step_epoch_break_reason +
                                   " old_message_changed_at=" +
                                   std::to_string(step_prefix_account.old_message_changed_at);
                }
                prefix_diag += " appended_messages=" + std::to_string(step_prefix_account.appended_messages) +
                               " stable_prefix=" + std::to_string(step_prefix_account.stable_prefix_messages) +
                               "/" + std::to_string(step_prefix_account.total_messages) +
                               " system_hash=" + step_prefix_account.system_hash.substr(0, 8) +
                               " tools_hash=" + step_prefix_account.tools_hash.substr(0, 8);
                if (step_prefix_account.wire_common_prefix_bytes >= 0) {
                    prefix_diag += " wire_common_prefix_bytes=" +
                                   std::to_string(step_prefix_account.wire_common_prefix_bytes);
                }
                platform::LogSink::Instance().Debug("loop", prefix_diag);
            }
        }

        // ---- 任务级 turn 预算门(turn 预算单 P0-1/§3.2):请求发出前的原子准入 ----
        // 预留、发出、完成三步分开:先占一枚 permit(拒绝即按任务 turn 预算
        // 耗尽收场,请求不发);durable 请求边界写失败且请求未发则归还名额;
        // 边界落稳才 commit(reserved->attempted,从此 API 错/流断/取消都
        // 保留这笔账,设计单 §6.4);完整 assistant 入 history 后另记 completed。
        // backend 肚里的连接重试不回到这层,不重复扣 turn。没装门的会话
        // 一处不调,行为与从前一字不差。
        std::optional<ModelTurnPermit> turn_permit;
        if (wiring.turn_budget != nullptr && wiring.turn_budget->armed()) {
            const auto reserved_permit = wiring.turn_budget->try_reserve();
            if (!reserved_permit.has_value()) {
                // denied:不发请求。history 里留着到限为止的全部来回(上一拍
                // 的工具结果已照常落账),部分结果由调用方按 TurnLimitExhausted
                // 收口带走——不得冒充已有最终结论(设计单 §7.2)。
                RunOutcome outcome{false, false, false, last_stop_reason, steps_used};
                outcome.output_budget = budget_report;
                outcome.hit_turn_limit = true;
                return outcome;
            }
            turn_permit = *reserved_permit;
        }

        // 轨迹边界(P0-2)与 Token 账本 A1 的 prepared/sent 记账已并入下方
        // 恢复环(监督器单 P0-1):每枚尝试各记一笔——本层不再重复落账,
        // 预算 permit 的归还/提交同样跟环内的各尝试走(见环内注释)。
        std::string trajectory_request_id;
        bool turn_committed = false;  // 首枚尝试已提交预算,重试趟跳过(不靠状态机拒绝判)
        int committed_turn_index = 0;  // 首枚尝试 commit 拿到的 task_turn_index(重试趟复用,§11.1)

        // ---- 请求级恢复(监督器单 P0-1):尝试环 --------------------------------
        // 半截流永不落地:每次尝试重置 assembler 与显示闸,只有 send_stream
        // 归队且流无错的那份才往下走 BuildMessage(单子 §8.1 原子提交)。可
        // 安全重发的错(Network/408/429/502/503/504)按阶梯退避后从同一
        // history 提交边界原样重发——幂等;已提交的 ToolResult 不在重发范围,
        // 绝不重跑工具。用户取消与不可重试错误立即收口,退避等待中也可打断。
        // 主路与子路共用这一环:两路都经 AgentLoop::Run,不在 AgentTool 里
        // 另抄私货(单子 §8.2)。
        api::MessageAssembler assembler;
        bool stream_error = false;
        std::string stream_error_message;
        std::string stream_error_code;
        // 模型输出图片(ccmoon 巡检单 P0):落盘成功的引用块按到达序攒着,
        // 流收口后并进 assistant 消息;同一 item id 只落一回(重复终帧——
        // output_item.done 与 response.completed 各到一次——只算头一回)。
        std::vector<api::ModelImageBlock> model_images;
        std::vector<std::string> landed_image_ids;
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
        // 轨迹请求 id 与恢复账:每枚尝试各自重置(尝试内记 prepared/sent)。
        // trajectory_request_id 复用外层(轨迹边界 P0-2/A1)那枚声明——
        // 恢复环每趟 clear() 后由各尝试的 prepared 重新赋值,不再重声明。
        const std::string history_commit_hash = api::HistoryCommitHashOf(request);
        bool trajectory_write_failed = false;
        int recovery_attempts_used = 0;
        std::chrono::milliseconds recovery_elapsed{0};
        api::RequestRecoveryHooks recovery_hooks;
        recovery_hooks.wait_backoff = wiring.wait_request_backoff;
        recovery_hooks.on_attempt = [&wiring, &recovery_attempts_used, &recovery_elapsed](
                                        const api::ModelRequestAttempt& recovery_attempt,
                                        api::RequestAttemptPhase phase) {
            recovery_elapsed = recovery_attempt.elapsed;
            if (phase == api::RequestAttemptPhase::Started) {
                ++recovery_attempts_used;
            }
            if (wiring.on_request_attempt) {
                wiring.on_request_attempt(recovery_attempt, phase);
            }
        };
        // 一次尝试:重置局部 -> 轨迹 prepared/sent -> 发流 -> 放闸尾巴。
        const auto run_one_attempt = [&](api::ModelRequestAttempt& recovery_attempt)
                                          -> std::expected<void, api::Error> {
            assembler = api::MessageAssembler{};
            stream_error = false;
            stream_error_message.clear();
            stream_error_code.clear();
            model_images.clear();
            landed_image_ids.clear();
            stream_request_id.clear();
            stream_model.clear();
            text_delta_gate = platform::Utf8DeltaGate{};
            thinking_delta_gate = platform::Utf8DeltaGate{};
            trajectory_request_id.clear();
            recovery_attempt.history_commit_hash = history_commit_hash;
            // 轨迹边界(P0-2):request prepared 记不住就不发模型(§7.4 耐久
            // 栅栏);落稳随即记 sent。没接轨迹的会话一字不加。写失败按 Api
            // 类错误退出尝试环(不可重发),环外按老文案收口。
            if (wiring.boundary_recorder != nullptr) {
                RequestPreparedContext prepared_ctx;
                prepared_ctx.purpose = agent.profile_.purpose;
                if (request_prompt_manifest.has_value()) {
                    prepared_ctx.has_prompt_manifest = true;
                    prepared_ctx.prompt_manifest = *request_prompt_manifest;
                }
                prepared_ctx.has_prefix_account = true;
                prepared_ctx.system_hash = step_prefix_account.system_hash;
                prepared_ctx.tools_hash = step_prefix_account.tools_hash;
                prepared_ctx.cache_epoch = step_prefix_account.cache_epoch;
                prepared_ctx.prefix_append_only = step_prefix_account.append_only;
                trajectory_request_id = wiring.boundary_recorder->OnRequestPrepared(request, prepared_ctx);
                if (trajectory_request_id.empty()) {
                    // 轨迹账写盘失败,本枚请求不出门:归还预算 permit 名额
                    //(attempted 不加,turn 预算单 §6.4),退出尝试环不重试。
                    if (turn_permit.has_value() && wiring.turn_budget->abort_before_send) {
                        wiring.turn_budget->abort_before_send(*turn_permit);
                        turn_permit.reset();
                    }
                    trajectory_write_failed = true;
                    return std::unexpected(api::Error{api::ErrorKind::Api, "trajectory write failed", 0});
                }
            }
            // permit 从"占额"翻"已发"(reserved-=1,attempted+=1,turn 预算单
            // §3.2)——与轨迹解耦:没接 boundary_recorder 的会话照样提交(纯预
            // 算门单测即此形状);只在本请求首枚尝试提交,恢复重试不重复扣
            // turn(与 backend 肚内重试同口径)。提交不上按本地账错退出尝试环。
            // P1-1:commit 提到 sent 之前——sent 是"真的发出去"的事实,提交
            // 失败(本地账错)时不再先记一笔 sent 糊账;task_turn_index 也随
            // commit 返回,正好随 sent 边界交轨迹(§11.1)。
            if (turn_permit.has_value() && !turn_committed && wiring.turn_budget->commit_sent) {
                auto committed = wiring.turn_budget->commit_sent(*turn_permit);
                if (!committed.has_value()) {
                    turn_permit.reset();
                    return std::unexpected(api::Error{api::ErrorKind::Api,
                                                      "turn budget commit failed", 0});
                }
                turn_committed = true;
                committed_turn_index = *committed;
                // 不 reset permit:留到 assistant 入 history 后 mark_completed
                // 用(Turn 单 §3.2 三步账);重试趟由上面的旗子跳过,不重复扣。
            }
            if (wiring.boundary_recorder != nullptr && !trajectory_request_id.empty()) {
                if (turn_committed) {
                    // 任务 turn 账随发随记(§11.1):started 边界带 index/limit/
                    // input round,收口三态(completed/failed/cancelled)由实现
                    // 侧按 request_id 对回这枚 turn。
                    wiring.boundary_recorder->OnRequestSentWithTurn(
                        trajectory_request_id, committed_turn_index,
                        turn_permit.has_value() ? turn_permit->limit : 0, wiring.input_round_index);
                } else {
                    wiring.boundary_recorder->OnRequestSent(trajectory_request_id);
                }
            }
            const std::size_t thinking_bytes_at_attempt_start = budget_report.thinking_bytes;
            std::string thinking_tail_at_attempt_start = budget_report.thinking_tail;
            const auto attempt_result = backend_.send_stream(
                request,
                [&](const api::StreamEvent& event) {
                if (!recovery_attempt.saw_stream_event) {
                    recovery_attempt.saw_stream_event = true;  // 首枚流事件旁证
                    recovery_attempt.saw_headers = true;
                }
                assembler.Feed(event);
                std::visit(
                    [&](const auto& e) {
                        using T = std::decay_t<decltype(e)>;
                        if constexpr (std::is_same_v<T, api::MessageStart>) {
                            stream_request_id = e.id;
                            stream_model = e.model;
                        } else if constexpr (std::is_same_v<T, api::TextDelta>) {
                            if (wiring.events != nullptr) {
                                const std::string gated = text_delta_gate.Feed(e.text);
                                if (!gated.empty()) {
                                    wiring.events->OnTextDelta(gated);
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
                            if (wiring.events != nullptr) {
                                const std::string gated = thinking_delta_gate.Feed(e.text);
                                if (!gated.empty()) {
                                    wiring.events->OnThinkingDelta(gated);
                                }
                            }
                        } else if constexpr (std::is_same_v<T, api::StreamError>) {
                            stream_error = true;
                            stream_error_message = e.message;
                            stream_error_code = e.code;
                        } else if constexpr (std::is_same_v<T, api::ImageOutput>) {
                            // 图片正文到站(Responses 的 image_generation_call。
                            // result):base64 只走到这里为止——落盘口还引用,
                            // 引用入历史,正文用完即弃,不进 assembler/session。
                            if (std::find(landed_image_ids.begin(), landed_image_ids.end(), e.id) !=
                                landed_image_ids.end()) {
                                return;  // 重复终帧:同 id 只落一回
                            }
                            if (stream_error) {
                                return;  // 已有错在身,别再落新盘
                            }
                            const auto close_image_card = [&](bool ok, const std::string& summary) {
                                if (wiring.events != nullptr) {
                                    wiring.events->OnBuiltinToolDone(e.id, "image_generation",
                                                                     nlohmann::json::object(), summary, !ok);
                                }
                            };
                            if (!wiring.on_model_image) {
                                stream_error = true;
                                stream_error_message = "服务端返回了图片,但本会话未接线图片落盘,结果未保存";
                                close_image_card(false, stream_error_message);
                                return;
                            }
                            const auto landing = wiring.on_model_image(e);
                            if (!landing.has_value()) {
                                stream_error = true;
                                stream_error_message = "图片未保存: " + landing.error();
                                close_image_card(false, stream_error_message);
                                return;
                            }
                            landed_image_ids.push_back(e.id);
                            model_images.push_back(std::move(landing->block));
                            {
                                const api::ModelImageBlock& block = model_images.back();
                                std::string meta;
                                if (block.width > 0 || block.height > 0) {
                                    meta += " " + std::to_string(block.width) + "x" + std::to_string(block.height);
                                }
                                close_image_card(true, "已保存 " + landing->display_path + meta);
                            }
                        } else if constexpr (std::is_same_v<T, api::BuiltinToolStart>) {
                            if (wiring.events != nullptr) {
                                wiring.events->OnBuiltinToolStart(e.id, e.name, e.input);
                            }
                        } else if constexpr (std::is_same_v<T, api::BuiltinToolDone>) {
                            if (wiring.events != nullptr) {
                                wiring.events->OnBuiltinToolDone(e.id, e.name, e.input, e.summary, e.is_error);
                            }
                        } else if constexpr (std::is_same_v<T, api::ServerToolUseStart>) {
                            // 服务端工具搜索(动态工具 P3):provider 执行的搜索,
                            // 复用服务端内置工具的显示卡——只画轨迹,绝不在本地
                            // 执行。入参还在后头的增量里,这里先起卡。
                            if (wiring.events != nullptr) {
                                wiring.events->OnBuiltinToolStart(e.id, e.name, nlohmann::json::object());
                            }
                        } else if constexpr (std::is_same_v<T, api::ServerToolResult>) {
                            if (wiring.events != nullptr) {
                                wiring.events->OnBuiltinToolDone(e.tool_use_id, "tool_search",
                                                                 nlohmann::json::object(),
                                                                 ServerToolSearchSummary(e.content),
                                                                 ServerToolSearchFailed(e.content));
                            }
                        }
                    },
                    event);
            },
                cancel);
            // 流收口:闸里扣着的尾巴拼不齐就是坏字节,按 U+FFFD 放完——错误/
            // 打断路径也要放,显示层与 history 的账对得上。
            if (wiring.events != nullptr) {
                const std::string text_tail = text_delta_gate.Flush();
                if (!text_tail.empty()) {
                    wiring.events->OnTextDelta(text_tail);
                }
            }
            if (wiring.events != nullptr) {
                const std::string thinking_tail = thinking_delta_gate.Flush();
                if (!thinking_tail.empty()) {
                    wiring.events->OnThinkingDelta(thinking_tail);
                }
            }
            if (attempt_result.has_value() && stream_error) {
                // 兼容端常回 HTTP 200 + error 事件。必须在尝试边界折成 Api
                // 错误,恢复环才能按 provider code 判瞬时错并重发。
                api::Error error{api::ErrorKind::Api, stream_error_message, 0, stream_error_code};
                if (api::IsRetryableError(error)) {
                    budget_report.thinking_bytes = thinking_bytes_at_attempt_start;
                    budget_report.thinking_tail = std::move(thinking_tail_at_attempt_start);
                }
                return std::unexpected(std::move(error));
            }
            if (!attempt_result.has_value() && api::IsRetryableError(attempt_result.error())) {
                // 本次尝试攒进活账的思考字节回滚:重试会整段重来,不双记。
                budget_report.thinking_bytes = thinking_bytes_at_attempt_start;
                budget_report.thinking_tail = std::move(thinking_tail_at_attempt_start);
            }
            return attempt_result;
        };
        const auto send_result = api::RunRequestWithRecovery(run_one_attempt, recovery_hooks, cancel);
        if (trajectory_write_failed) {
            return std::unexpected("轨迹账写盘失败,本轮停在请求边界,未发模型");
        }

        if (!send_result.has_value()) {
            const api::Error& err = send_result.error();
            if (err.kind == api::ErrorKind::Cancelled) {
                // ESC 打断:流被从中间掐断,ContentBlockDone/MessageDone 永远
                // 不会来了,手动把还开着的块(文本或 tool_use)收个尾,半截话
                // 也要照常攒进历史,不能悄悄丢掉。打断前已落盘的图片引用也
                // 一并带上——文件是真落了的,历史不留账就成了孤儿。
                assembler.FinalizeOpenBlock();
                api::Message assistant_message = assembler.BuildMessage();
                for (auto& image : model_images) {
                    assistant_message.content.push_back(std::move(image));
                }
                assistant_message.content.push_back(api::TextBlock{"[用户按 ESC 打断了这条回答]"});
                context_.PushMessage(std::move(assistant_message));

                // 半截流里如果混进了没走完的 tool_use 块(硬收尾出来的,
                // input 多半是空对象或者解析失败),必须给每一个都配一条
                // tool_result——不然这条 assistant 消息下一轮重放给模型时,
                // tool_use/tool_result 配对关系就破了,API 会直接拒绝整个
                // 请求。
                std::vector<api::ContentBlock> orphan_results;
                for (const auto& block : context_.durable_history().back().content) {
                    if (std::holds_alternative<api::ToolUseBlock>(block)) {
                        const auto& call = std::get<api::ToolUseBlock>(block);
                        orphan_results.push_back(
                            api::ToolResultBlock{call.id, "用户按 ESC 打断,该工具未执行", true});
                    }
                }
                if (!orphan_results.empty()) {
                    api::Message orphan_message;
                    orphan_message.role = api::Role::User;
                    orphan_message.content = std::move(orphan_results);
                    context_.PushMessage(std::move(orphan_message));
                }

                // 轨迹边界:打断也是一枚明确收口(output.cancelled;usage
                // 若报了先记 owner)。
                if (wiring.boundary_recorder != nullptr && !trajectory_request_id.empty()) {
                    wiring.boundary_recorder->OnUsageRecorded(
                        trajectory_request_id, assembler.usage(), assembler.usage_seen(), stream_request_id,
                        step_prefix_account.cache_epoch, step_prefix_account.append_only,
                        assembler.cache_seen());
                    wiring.boundary_recorder->OnOutputCancelled(trajectory_request_id);
                }
                return RunOutcome{true, false, false, last_stop_reason, steps_used};
            }
            // 错误的人话收口(ccmoon 巡检单 P1):HTTP 非 2xx 把状态码与
            // 摘要后的错误体带上(抽 message/type/code、打码密钥、截短),
            // 不再把整段 JSON 原样糊脸;网络类人话(连接超时一类)过一遍
            // 同一道打码截短,原文不丢。
            {
                // 轨迹边界:请求失败是明确收口(output.failed),记的是打码
                // 截短后的人话,不落原始错误体。
                if (wiring.boundary_recorder != nullptr && !trajectory_request_id.empty()) {
                    std::string fail_reason = err.message;
                    if (err.kind == api::ErrorKind::HttpStatus && err.http_status != 0) {
                        fail_reason = "HTTP " + std::to_string(err.http_status);
                    }
                    wiring.boundary_recorder->OnOutputFailed(trajectory_request_id, fail_reason);
                }
                std::string message = err.message;
                if (err.kind == api::ErrorKind::HttpStatus && err.http_status != 0) {
                    message = "HTTP " + std::to_string(err.http_status) + ": " +
                              api::SummarizeErrorBodyForUser(message);
                } else {
                    message = api::SummarizeErrorBodyForUser(message);
                }
                if (recovery_attempts_used > 1) {
                    message += cli::trf("error.request.recovery_exhausted", recovery_attempts_used - 1,
                                        FormatRecoveryElapsed(recovery_elapsed));
                }
                return std::unexpected(cli::trf("error.request.failed", message));
            }
        }
        if (stream_error) {
            if (wiring.boundary_recorder != nullptr && !trajectory_request_id.empty()) {
                wiring.boundary_recorder->OnOutputFailed(trajectory_request_id, stream_error_message);
            }
            return std::unexpected("模型返回错误: " + stream_error_message);
        }

        api::Message assistant_message = assembler.BuildMessage();
        // 模型输出的图片引用块并进消息(到达序在文本后):session/export/
        // resume 只见引用,base64 早已在落盘口换成文件。on_assistant_message_
        // ready 在下一行,落盘账(persist)拿到的就是这份带引用的消息。
        for (auto& image : model_images) {
            assistant_message.content.push_back(std::move(image));
        }
        const std::string stop_reason = assembler.stop_reason();
        ++steps_used;
        last_stop_reason = stop_reason;

        // 子代理空轨迹单 P0-F:provider 没给身份的 function call 已被
        // assembler 丢弃(终帧补 id 的合并也救不回的)。这份输出按畸形
        // 收口:已攒正文如实入史,落 model.output.failed,回合明败——
        // 不执行、不合成局部工具结果、不伪造 provider call id,也不带着
        // "声称 tool_use 却没有工具"的空转进入下一步。
        if (assembler.idless_tool_calls_dropped() > 0) {
            if (wiring.boundary_recorder != nullptr && !trajectory_request_id.empty()) {
                wiring.boundary_recorder->OnOutputFailed(
                    trajectory_request_id,
                    "malformed_tool_call_no_id: function call 缺 call_id(" +
                        std::to_string(assembler.idless_tool_calls_dropped()) + " 枚),已丢弃不执行");
            }
            context_.PushMessage(std::move(assistant_message));
            return std::unexpected("模型输出畸形: function call 缺 call_id(" +
                                   std::to_string(assembler.idless_tool_calls_dropped()) +
                                   " 枚),已丢弃不执行");
        }
        // 逐枚追踪:assistant 消息一入 history 就交装配层 append+flush 进
        // session(单子:provider assistant message 在执行工具前落盘)。
        // 老路(收口后 PersistNewMessages)照旧兜底——没装 trace 的会话
        // 一字不变;装了的,PersistNew 的只增不减账不会重复落(persisted
        // 基线此刻还没推进)。
        if (wiring.on_assistant_message_ready) {
            wiring.on_assistant_message_ready(assistant_message);
        }
        // 轨迹边界(P0-2):模型输出先记成事实才许跑工具(§15.1"模型输出
        // 必须先 Record 成功,才许跑工具");usage owner(v2)先行落账。输出
        // 记不住就明败,不执行工具(§7.4 耐久栅栏)。
        if (wiring.boundary_recorder != nullptr && !trajectory_request_id.empty()) {
            wiring.boundary_recorder->OnUsageRecorded(trajectory_request_id, assembler.usage(),
                                                      assembler.usage_seen(), stream_request_id,
                                                      step_prefix_account.cache_epoch,
                                                      step_prefix_account.append_only,
                                                      assembler.cache_seen());
            if (!wiring.boundary_recorder->OnOutputCompleted(trajectory_request_id, assistant_message,
                                                             stop_reason, stream_request_id)) {
                return std::unexpected("轨迹账写盘失败,模型输出未落账,不执行工具");
            }
        }
        context_.PushMessage(std::move(assistant_message));
        // 任务级 turn 账的完成确认(设计单 §6.4):完整 assistant message 入
        // history 后 completed += 1。幂等——同一 permit 重复回调不加二次。
        // API 错/流断/用户取消不走这里,attempted 保留,completed 不加。
        if (turn_permit.has_value() && wiring.turn_budget->mark_completed) {
            wiring.turn_budget->mark_completed(*turn_permit);
        }
        // usage 是否报告(规格根因四):任一请求带回过非零 usage 就算报告过,
        // 之后失败页说"token 数未报告"只看这一位,不拿 0 糊。
        {
            const api::Usage& usage = assembler.usage();
            budget_report.usage_reported =
                budget_report.usage_reported || assembler.usage_seen() || usage.input_tokens > 0 ||
                usage.output_tokens > 0 || usage.cache_read_tokens > 0 ||
                usage.cache_creation_tokens > 0 || usage.output_reasoning_tokens > 0;
            // 成本刹车(P2-6):token 硬线按"完整输入 + 输出"累计,与台账/
            // 面板同口径——provider 漏 usage 只会晚触发,不会把闸拆了。
            tokens_seen += api::TotalInputTokens(usage) + usage.output_tokens;
        }

        if (wiring.events != nullptr) {
            api::UsageReport report;
            report.usage = assembler.usage();
            report.step_index = step_index;
            report.provider_response_id = stream_request_id;
            report.model = stream_model;
            report.cache_epoch = context_.cache_epoch();
            report.epoch_break_reason = step_epoch_break_reason;
            report.prefix_append_only = step_prefix_append_only;
            // provider 明报位(Token 账本单 A0):wire 见过 usage 帧才算,
            // 明报全零也是真,没报不许拿 0 冒充。
            report.reported_by_provider = assembler.usage_seen();
            report.cache_reported_by_provider = assembler.cache_seen();
            // 每请求缓存诊断账(问题 9):本地前缀视角全量带出——epoch 首请
            // 求、system/tools/稳定前缀指纹与长度、wire 公共前缀字节(诊断
            // 模式才有,-1 = 不可得)。只留短 hash 与长度,不落正文。
            report.epoch_first_request = !step_prefix_account.had_previous;
            report.system_hash = step_prefix_account.system_hash;
            report.tools_hash = step_prefix_account.tools_hash;
            report.prefix_hash = step_prefix_account.prefix_hash;
            report.stable_prefix_messages = step_prefix_account.stable_prefix_messages;
            report.total_messages = step_prefix_account.total_messages;
            report.wire_common_prefix_bytes = step_prefix_account.wire_common_prefix_bytes;
            wiring.events->OnUsage(report);
        }

        // 防御:stop_reason 说的是 end_turn(或者干脆是空的——终止帧丢了),
        // 消息里却攒出了 tool_use 块。信块不信帧:照 tool_use 处理,把工具跑了、
        // 结果成对喂回去。不然历史里就留下一条没有 tool_result 配对的 tool_use,
        // 下一轮请求直接被 API 以 400 拒掉。
        const api::Message& last_assistant = context_.durable_history().back();
        bool has_tool_use = false;
        for (const auto& block : last_assistant.content) {
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
            for (const auto& block : last_assistant.content) {
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
                    context_.PushMessage(std::move(marker));
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
        // 执行、结果还没发回"那个当口——正在跑的这个工具拿到取消旗后按各自
        // 能力收口:run_command 收进程树、Lua 掐指令钩子,不肯合作的照旧等它
        // 跑完;结果照常入历史;还没轮到的后续工具不再真的执行,补一条
        // "未执行"的合成结果)保住 tool_use/tool_result 的成对约束,再从
        // Run() 正常返回。
        //
        // 逐枚追踪(单子"消息落盘次序要改"):assistant message 入 history
        // 后先发批次头(装配层此刻把 assistant 消息 append+flush 进
        // session),再为每枚 tool use 写 scheduled,然后逐枚执行
        // (started -> finished),五枚结果收齐、合并的 user 消息入 history
        // 后再发批次尾(装配层补落 user 消息 + 各枚 result_committed)。
        // 审计按枚及时落,崩溃窗口从"整轮"缩到"当前这枚";wire 语义不变,
        // 五枚结果仍同一条 user message。
        const bool trace_armed = wiring.on_tool_trace != nullptr;
        std::string batch_id;
        int sequence_in_batch = 0;
        if (trace_armed) {
            batch_id = "batch-" + std::to_string(++batch_counter_);
        }
        std::vector<std::string> scheduled_ids;  // 本批各枚 execution_id(装 trace 时才有)
        std::vector<std::string> scheduled_tool_use_ids;
        std::vector<std::string> scheduled_names;
        if (trace_armed && wiring.on_tool_trace) {
            for (const auto& block : last_assistant.content) {
                if (!std::holds_alternative<api::ToolUseBlock>(block)) {
                    continue;
                }
                const auto& call = std::get<api::ToolUseBlock>(block);
                ToolTraceEvent scheduled;
                scheduled.kind = ToolTraceEventKind::Scheduled;
                scheduled.batch_id = batch_id;
                scheduled.sequence_in_batch = sequence_in_batch;
                scheduled.execution_id = issue_execution_id();
                scheduled.tool_use_id = call.id;
                scheduled.tool_name = call.name;
                scheduled.timestamp_ms = NowMsEpoch();
                // 动态工具 P1:scheduled 是 wire 事实——经代理壳发起的调用
                // 此刻只知道 transport 那层,解出的真实目标等执行事件报
                //(resolved 那层由 RunOneTool 的 emit 补),两层不混写。
                if (tool_ref_resolver_ != nullptr && call.name == "tool_invoke") {
                    scheduled.details["transport_tool"] = call.name;
                }
                wiring.on_tool_trace(scheduled);
                scheduled_ids.push_back(scheduled.execution_id);
                scheduled_tool_use_ids.push_back(call.id);
                scheduled_names.push_back(call.name);
                ++sequence_in_batch;
            }
        }

        // 回合视觉收束:批次边界。遍历前把这一批的 tool_use id 按模型给
        // 的顺序交出去(界面先全登记 Pending,再逐枚推进);遍历后(含
        // 打断补账)报收。没设回调零影响;执行语义一字不动。
        int batch_index_for_this_step = -1;
        {
            std::vector<std::string> batch_ids;
            for (const auto& block : last_assistant.content) {
                if (std::holds_alternative<api::ToolUseBlock>(block)) {
                    batch_ids.push_back(std::get<api::ToolUseBlock>(block).id);
                }
            }
            if (!batch_ids.empty() && wiring.events != nullptr) {
                wiring.events->OnToolBatchStarted(step_index, static_cast<int>(batches_emitted), batch_ids);
            }
            if (!batch_ids.empty()) {
                batch_index_for_this_step = static_cast<int>(batches_emitted);
                ++batches_emitted;
            }
        }
        bool interrupted = false;
        int tool_index = -1;
        std::vector<api::ContentBlock> tool_results;
        for (const auto& block : last_assistant.content) {
            if (!std::holds_alternative<api::ToolUseBlock>(block)) {
                continue;
            }
            ++tool_index;
            const auto& call = std::get<api::ToolUseBlock>(block);
            if (interrupted || (cancel != nullptr && cancel->load())) {
                interrupted = true;
                // 未轮到便被 ESC 收掉:记 cancelled_before_start 终态栅栏
                // (单子生命周期规矩),不冒充执行过。
                if (trace_armed) {
                    ToolTraceEvent cancelled;
                    cancelled.kind = ToolTraceEventKind::ExecutionFinished;
                    cancelled.outcome = ToolOutcome::CancelledBeforeStart;
                    cancelled.batch_id = batch_id;
                    cancelled.sequence_in_batch = tool_index;
                    cancelled.execution_id = scheduled_ids[tool_index];
                    cancelled.tool_use_id = call.id;
                    cancelled.tool_name = call.name;
                    cancelled.timestamp_ms = NowMsEpoch();
                    wiring.on_tool_trace(cancelled);
                }
                tool_results.push_back(api::ToolResultBlock{call.id, "用户按 ESC 打断,该工具未执行", true});
                continue;
            }
            ToolTraceContext trace_ctx;
            if (trace_armed) {
                trace_ctx.execution_id = scheduled_ids[tool_index];
                trace_ctx.batch_id = batch_id;
                trace_ctx.sequence_in_batch = tool_index;
                trace_ctx.provider_request_id = stream_request_id;
            }
            // ---- 动态工具 P1(通用 ProxyReference):tool_invoke 的规范化
            //(单子 §6.1)。在进入 RunOneTool 之前把 wire 调用解引用成真实
            // 目标调用——只对 target_call 调一次 RunOneTool,tool_use_id 沿用
            // wire call 的 id,assistant tool call 与 user tool result 仍一一
            // 配对。解不开(伪拼/跨会话/stale/工具没了)就地落稳定错误码
            // 的 tool_result(仍用原 wire id),并补终态栅栏——不是无 id 的
            // assistant 文本。直接按名调用延迟工具不走这条:tool_filter 照拦
            //(发现不等于授权,模型不能凭名字穿代理)。
            if (tool_ref_resolver_ != nullptr && call.name == "tool_invoke") {
                const auto resolved = tool_ref_resolver_->Resolve(registry_, call);
                if (!resolved.has_value()) {
                    const tools::DeferredToolResolver::Refusal& refusal = resolved.error();
                    if (trace_armed) {
                        ToolTraceEvent refused;
                        refused.kind = ToolTraceEventKind::ExecutionFinished;
                        refused.outcome = ToolOutcome::ToolError;
                        refused.error_code = refusal.code;
                        refused.fallback_message =
                            refusal.message.size() <= 200
                                ? refusal.message
                                : refusal.message.substr(0, platform::Utf8PrefixBoundary(refusal.message, 200));
                        refused.batch_id = batch_id;
                        refused.sequence_in_batch = tool_index;
                        refused.execution_id = scheduled_ids[tool_index];
                        refused.tool_use_id = call.id;
                        refused.tool_name = call.name;  // 解引用失败:只有 wire 那层可报
                        refused.details = nlohmann::json{{"transport_tool", call.name}};
                        refused.timestamp_ms = NowMsEpoch();
                        wiring.on_tool_trace(refused);
                    }
                    tool_results.push_back(
                        api::ToolResultBlock{call.id, platform::SanitizeUtf8(refusal.message), true});
                    continue;
                }
                // 规范化后的真实目标调用:id 沿用 wire call 的,名字与入参
                // 换成解出来的那枚。执行资格经 tool_execution_policy(空 =
                // 装配层未给限制,各项闸照走);普通工具过滤不套在代理调用
                // 上——延迟工具本就不在顶层 tools,套了会把正路堵死。
                api::ToolUseBlock target_call = call;
                target_call.name = resolved->target_name;
                target_call.input = resolved->arguments;
                tools::ProxyCallContext proxy_ctx;
                proxy_ctx.transport_name = call.name;
                proxy_ctx.tool_ref = resolved->tool_ref;
                proxy_ctx.schema_digest = resolved->schema_digest;
                trace_ctx.transport_tool = call.name;
                trace_ctx.tool_ref = resolved->tool_ref;
                trace_ctx.schema_digest = resolved->schema_digest;
                const std::string execution_denial =
                    agent.profile_.tool_execution_denial.empty()
                        ? std::string(tools::kErrToolRefNotAllowed) + "|该工具不在当前会话的执行策略内,不得重试同一调用。"
                        : agent.profile_.tool_execution_denial;
                const tools::Tool::Result result =
                    RunOneTool(registry_, target_call, wiring, tool_execution_policy_, execution_denial,
                               trace_armed ? &trace_ctx : nullptr, cancel, &proxy_ctx, tool_turn_gate_,
                               tool_turn_gate_denial_);
                {
                    api::ToolResultBlock block;
                    block.tool_use_id = call.id;  // 配对的是 wire 那枚 tool_invoke 的 id
                    block.content = platform::SanitizeUtf8(result.content);
                    block.is_error = result.is_error;
                    if (!result.payload.empty()) {
                        block.blocks = result.payload.content;
                        block.structured_content = result.payload.structured_content;
                    }
                    tool_results.push_back(std::move(block));
                }
                if (cancel != nullptr && cancel->load()) {
                    interrupted = true;
                }
                continue;
            }
            const tools::Tool::Result result =
                RunOneTool(registry_, call, wiring, tool_filter_, tool_filter_denial_,
                           trace_armed ? &trace_ctx : nullptr, cancel, /*proxy=*/nullptr, tool_turn_gate_,
                           tool_turn_gate_denial_);
            // 断言式兜底:RunOneTool 出口已经规范化过(见它文件头的信任边界
            // 注释),这里再过一遍 SanitizeUtf8 只为防将来有人在 Run() 之外
            // 绕路改历史——已经合法的内容是原样穿透的空操作。
            // MCP 富结果单:payload 富块与 structuredContent 随投影一起入史
            // (文本结果 blocks 为空,行为与从前一字不差);投影里图片/音频
            // 是 artifact 短句,四家 wire 吃它作文本降级。
            {
                api::ToolResultBlock block;
                block.tool_use_id = call.id;
                block.content = platform::SanitizeUtf8(result.content);
                block.is_error = result.is_error;
                if (!result.payload.empty()) {
                    block.blocks = result.payload.content;
                    block.structured_content = result.payload.structured_content;
                }
                tool_results.push_back(std::move(block));
            }
            if (cancel != nullptr && cancel->load()) {
                interrupted = true;
            }
        }

        api::Message tool_result_message;
        tool_result_message.role = api::Role::User;
        tool_result_message.content = std::move(tool_results);
        // 批次尾回调要在消息 move 进双账之前拿:回调里读的是五枚结果齐的
        // user message(装配层此刻 append+flush 它)。
        const bool results_callback_armed = wiring.on_tool_results_committed != nullptr;
        api::Message message_for_callback = results_callback_armed ? tool_result_message : api::Message{};
        context_.PushMessage(std::move(tool_result_message));

        if (batch_index_for_this_step >= 0 && wiring.events != nullptr) {
            wiring.events->OnToolBatchFinished(batch_index_for_this_step, interrupted);
        }

        if (trace_armed) {
            // 批次尾:结果消息本体已入 history。先交装配层 append+flush user
            // 消息(设了回调的会话),再为每枚发 result_committed 栅栏——
            // 栅栏是 canonical 事件流的一部分,不依赖装配层是否监听消息
            // 落盘;若这里之前崩溃,resume 由 trace 重建:finished 的从
            // result ref 恢复,started 无 finished 的标 unknown,只有
            // scheduled 的标未执行(单子"消息落盘次序")。
            if (results_callback_armed) {
                wiring.on_tool_results_committed(batch_id, message_for_callback);
            }
            for (const std::string& execution_id : scheduled_ids) {
                ToolTraceEvent committed;
                committed.kind = ToolTraceEventKind::ResultCommitted;
                committed.batch_id = batch_id;
                committed.execution_id = execution_id;
                committed.timestamp_ms = NowMsEpoch();
                wiring.on_tool_trace(committed);
            }
        }

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
