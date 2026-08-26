// AgentLoop 与 RunOneTool 的实现。Agent 本体在 agent/agent.cpp,上下文账
// 在 agent/context_manager.cpp——这里只剩轮次推进:拼请求(皮上的叠层就
// 地生效)、发流、工具循环、收口。

#include "agent/loop.hpp"

#include <algorithm>
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
#include "api/assembler.hpp"
#include "hooks/hash.hpp"  // Sha256Hex:trace 的入参/结果摘要锚
#include "platform/text_encoding.hpp"  // SanitizeExternalText:工具结果的第一道编码关口
#include "platform/wall_clock.hpp"     // 统一墙钟(批五):trace/计划动作的时间戳同源
#include "runtime/plan_mode.hpp"       // kErrModeDenied:Plan 硬闸的稳定码
#include "tools/schema_check.hpp"      // updatedInput 改写后的 schema 复检
#include "platform/log_sink.hpp"

namespace lubancode::agent {

namespace {
// Unix epoch 毫秒(chrono 跨 clock 铁律:两枚 now 差只在这类帮手里出现,
// 全文件统一走它)。批五乙收进统一墙钟:五套台账的真钟只 platform 一枚
// 落点,口径不变。
std::int64_t NowMsEpoch() {
    return platform::WallClockNowMs();
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
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const Callbacks& callbacks,
                                const std::function<bool(const tools::Tool&)>& tool_filter,
                                const std::string& filter_denial,
                                const ToolTraceContext* trace) {
    // 每条收尾路共用的分发口:先清洗,再 on_tool_done,清洗版随返回值交给
    // 调用方(进 history / 下一轮请求)。日志只记字段名、长度、坏字节位置
    // 与前后几个十六进制字节,不倒正文。
    const auto dispatch_done = [&callbacks](const std::string& tool_use_id, const std::string& name,
                                             tools::Tool::Result result) {
        if (!platform::IsValidUtf8(result.content)) {
            platform::LogSink::Instance().Warn(
                "loop", platform::DescribeUtf8Issue("tool_result:" + name, result.content));
            result.content = platform::SanitizeExternalText(result.content);
        }
        if (callbacks.on_tool_done) {
            callbacks.on_tool_done(tool_use_id, name, result);
        }
        return result;
    };

    // 工具状态机相位通报(没设回调 = 没配 hooks,行为与从前逐字节一致)。
    const auto phase = [&callbacks, &call](runtime::ToolPhase p) {
        if (callbacks.on_tool_phase) {
            callbacks.on_tool_phase(call.id, call.name, p);
        }
    };

    // ---- 逐枚追踪:栅栏发射器(trace 缺席 = 没装配,全部空操作) ---------
    // 领域事件只从这一个口出(单子"一份事件,两路消费"):Runtime 投影、
    // 持久账、录制件、Hook 关联各取所需,RunOneTool 不再多路手写。
    const auto started_at = std::chrono::steady_clock::now();
    ToolTraceEvent fired_started;  // 已越过 started 的载荷,finished 时复用
    bool crossed_start = false;
    const auto emit = [trace, &callbacks, &call, &fired_started, &crossed_start](ToolTraceEvent event) {
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
        // 批五乙留账收尾:trace 钟原先直读 system_clock,收进统一墙钟
        //(与全文件其余账面同一只手)。
        event.timestamp_ms = NowMsEpoch();
        if (event.kind == ToolTraceEventKind::ExecutionStarted) {
            fired_started = event;
            crossed_start = true;
        }
        callbacks.on_tool_trace(event);
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
        if (event.compensates.empty() && callbacks.on_tool_compensates && trace != nullptr) {
            event.compensates = callbacks.on_tool_compensates(trace->execution_id, call.name);
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

    if (callbacks.on_tool_start) {
        callbacks.on_tool_start(call.id, call.name, call.input);
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
    if (tool_filter && !tool_filter(*tool)) {
        const std::string denial =
            filter_denial.empty()
                ? "工具 " + call.name + " 存在但尚未挂载:请先用 tool_search 检索挂载,再调用。"
                : "工具 " + call.name + ": " + filter_denial;
        tools::Tool::Result unavailable{denial, true};
        unavailable.outcome = ToString(ToolOutcome::Unavailable);
        unavailable.error_code = kErrRegistryNotMounted;
        finish(unavailable, source_kind, source_instance, effect_class);
        return dispatch_done(call.id, call.name, std::move(unavailable));
    }

    // ---- Plan 模式(只读研究硬闸单):ModePolicy 在 PreToolUse Hook 之前。
    // 拒绝时 Hook 不跑、确认不问、工具不执行——Plan 拒绝压过 Hook 与
    // Yolo(单子:先过 Plan capability gate,再过 Hook/schema,再过
    // permission)。终态 ModeDenied,错误码 mode.denied*(装配层给的稳定
    // 细码),不冒充"没挂载"也不冒充"用户拒绝"。
    if (callbacks.on_mode_policy) {
        const std::string mode_denial = callbacks.on_mode_policy(call.name, call.input);
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

    // ---- PreToolUse:在 UI 标记"真执行"之前、权限确认之前。deny -> 拦;
    // ask -> 即使确认档放行也要问用户;allow -> 跳过用户确认(deny 规则
    // 与权限策略仍在确认回调里,钩子越不了权);updatedInput 只与 allow
    // 同返,先过一遍工具 schema,改写打回即拦。
    phase(runtime::ToolPhase::CheckingHook);
    runtime::ToolHookDecision pre;
    if (callbacks.on_pre_tool_use_hook) {
        pre = callbacks.on_pre_tool_use_hook(call.id, call.name, call.input);
    } else if (callbacks.on_pre_tool_hook) {
        // 旧回调兼容:非空 = deny。
        const std::optional<std::string> legacy_blocked = callbacks.on_pre_tool_hook(call.id, call.name, call.input);
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
        std::string content = denied.content;
        for (const auto& ctx : pre.additional_context) {
            content += "\n[钩子附注] " + ctx;
        }
        denied.content = std::move(content);
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
            std::string content = rejected.content;
            for (const auto& ctx : pre.additional_context) {
                content += "\n[钩子附注] " + ctx;
            }
            rejected.content = std::move(content);
            return dispatch_done(call.id, call.name, std::move(rejected));
        }
        effective_input = *pre.updated_input;
    }

    if (tool->needs_confirm()) {
        phase(runtime::ToolPhase::WaitingPermission);
        // P2(显示系统剥离单):异步审批通道优先——发 runtime::ApprovalRequest 拿
        // future,原地 Wait。终端前端的 future 实现是"当场问完再给结果"
        // (Wait 立即返回,与今日同步 on_tool_confirm 一字不差);远端前端
        // 的实现悬起 request_id,这里阻塞等,事件泵/连接线程不跟堵。
        // async 缺位回落到旧同步回调(子代理转发、单测、后台"没人可问"
        // 的短路都还在旧路上,不许变慢、不许多线程化)。
        bool allowed = true;
        if (callbacks.on_tool_confirm_async) {
            const std::shared_ptr<runtime::InteractionFuture> future =
                callbacks.on_tool_confirm_async(
                    runtime::ApprovalRequest{call.id, call.name, effective_input, std::string()});
            const std::optional<runtime::ApprovalResponse> response =
                future != nullptr ? future->WaitApproval() : std::nullopt;
            if (!response.has_value()) {
                // 悬空收口(cancel):等价拒绝,拒绝文案照"没人可答"写,
                // 不冒充用户拒绝。
                const std::string denial =
                    callbacks.on_tool_denial_text ? callbacks.on_tool_denial_text(call.id, call.name)
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
                callbacks.on_tool_confirm ? callbacks.on_tool_confirm(call.id, call.name, effective_input) : true;
        }
        if (!allowed) {
            // 拒绝文案可由回调层给(后台子代理的拒绝是"无法弹确认、未预放
            // 行",不是用户拒绝——缺省文案会把子代理的最终报告带偏成"均被
            // 用户拒绝")。
            const std::string denial = callbacks.on_tool_denial_text
                                           ? callbacks.on_tool_denial_text(call.id, call.name)
                                           : std::string("用户拒绝执行该工具");
            tools::Tool::Result declined{denial, true};
            declined.outcome = ToString(ToolOutcome::PermissionDeclined);
            declined.error_code = kErrPermissionDeclined;
            finish(declined, source_kind, source_instance, effect_class);
            return dispatch_done(call.id, call.name, std::move(declined));
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
        emit(std::move(started));
    }
    // 副作用闸:started 落不住的副作用工具,这里拦(单子:写不成时,
    // 副作用工具不得继续执行)。拦下的以 result_store_failed 收尾——不
    // 冒充工具失败,也不冒充成功;模型与恢复账都看得出是宿主拦的。
    if (trace != nullptr && callbacks.on_tool_trace_blocked &&
        callbacks.on_tool_trace_blocked(trace->execution_id)) {
        tools::Tool::Result blocked_by_trace{"追踪账写盘失败,该工具未执行(副作用档默认拦截)", true};
        blocked_by_trace.outcome = ToString(ToolOutcome::ResultStoreFailed);
        blocked_by_trace.error_code = kErrSessionTraceAppendFailed;
        // finished 栅栏已在 hub 侧落过(拦截时补的 terminal 行),这里只
        // 走展示与返回,不再发第二枚。
        return dispatch_done(call.id, call.name, std::move(blocked_by_trace));
    }
    tools::Tool::Result result = tool->execute(effective_input);
    // PostToolUse(新):结果先清洗成合法 UTF-8 再给钩子;钩子的反馈追加进
    // 模型所见 tool_result,原始结果照旧进审计(副作用已发生,不能撤销,
    // 也不冒充撤销)。旧回调照旧吃它一贯拿到的结果。
    if (!platform::IsValidUtf8(result.content)) {
        result.content = platform::SanitizeExternalText(result.content);
    }
    // finished 栅栏在 PostToolUse 之前:原始 outcome 与结果正文先落账,
    // Hook 崩溃抹不掉"工具已完成"的事实;钩子追加的文本只进模型所见,
    // trace 里的 result_ref 记的是追加前的原始结果(两份 digest 分得开,
    // 恢复时能判断 Hook 到底改了什么)。
    finish(result, source_kind, source_instance, effect_class);
    if (callbacks.on_post_tool_use_hook) {
        const std::vector<std::string> feedback =
            callbacks.on_post_tool_use_hook(call.id, call.name, effective_input, result);
        for (const auto& line : feedback) {
            result.content += "\n[post-tool-use hook 追加] " + line;
        }
    }
    if (callbacks.on_post_tool_hook) {
        callbacks.on_post_tool_hook(call.id, call.name, effective_input, result);
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

std::expected<RunOutcome, std::string> AgentLoop::Run(Agent& agent, api::Message user_message,
                                                       const Callbacks& callbacks,
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
    // 回合视觉收束(终端回合视觉收束单):本 Run() 已发出的批次序号
    // (on_tool_batch_started 的 batch_index 用;每个含工具的 step 消耗
    // 一枚,跨 step 不重号)。
    std::size_t batches_emitted = 0;
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
        // 回合视觉收束:step 边界。请求还没发,先报"这一拍开始了"——
        // 界面(工具批次分组)凭它知道上一批已换拍。没设回调零影响。
        if (callbacks.on_model_step_started) {
            callbacks.on_model_step_started(step_index);
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

        api::Request request;
        request.model = model_;
        request.system = system_prompt_;
        // 皮上的会话级叠层就地生效(批四·病十一其三:五层请求改写后端
        // 退役):延迟索引段 -> 模型目录指令 -> 魂,拼装次序与从前传输层
        // 包装的次序一字不差(索引在前、指令居中、魂压轴)。从前这些改动
        // 发生在指纹算完之后,前缀账看不见;现在进了指纹,model/system 一
        // 动 epoch 如实断,账不再瞎。
        if (agent.profile_.deferred_index_provider) {
            request.system =
                WithDeferredToolsIndex(request.system, agent.profile_.deferred_index_provider());
        }
        request.system = WithModelInstructions(request.system, agent.profile_.model_instructions);
        request.system = WithSoul(request.system, agent.profile_.soul);
        api::ApplyRequestProfile(request, agent.profile_.request);
        // 步数将尽提醒(第五期):固定文案,在"剩余步数第一次降到阈值"那一步
        // 追加进尚未发出的尾部消息(user 输入或刚攒完的 tool result),随
        // history 留住——不改 system,不撤旧提醒,追加律不破。ShouldNudge-
        // StepLimit 每次 Run 只真一次,天然只注一遍。
        if (ShouldNudgeStepLimit(step_index, profile_.max_steps_per_turn) &&
            !context_.durable_history().empty()) {
            const api::TextBlock nudge{BuildStepLimitNudgeText()};
            context_.AppendToLast(nudge);
        }

        // mid-turn 安全点:拼请求前先估 projected——system + 工具定义 + 全份
        // history + 输出预留,过参考线就把压力通报出去。上层回调里可以同步
        // 做一次语义压缩(ReplaceHistory),返回后下面 BuildWorkingView 拿到
        // 的就是(可能已换短的)请求视图。窗口未知(0)或没设回调时跳过,行为
        // 与从前一致——这一步不发出任何请求,估错了也不会误伤。
        if (profile_.context_window_tokens > 0 && wiring_.on_context_pressure) {
            // 输出上限纳入 projected 计算(规格根因一):声明了用声明值,
            // unset 用保守估计(kUnsetOutputReserveEstimateTokens)——服务端
            // 默认上限拿不到准数,宁可早压不撞墙。
            const OutputBudget output_budget{profile_.max_output_tokens, profile_.max_output_tokens_source};
            std::size_t projected = EstimateUtf8Tokens(request.system) +
                                    EstimateHistoryTokens(context_.request_history()) +
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
            wiring_.on_context_pressure(pressure);
        }

        // 工作视图(压缩 + 字符安全网 + sticky)全在 ContextManager;真丢了
        // 东西(丢轮/截结果)的因它自己记进前缀账。
        const ContextWorkingView working_view = context_.BuildWorkingView({profile_.max_context_chars});
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
            const ContextManager::PrefixAccount account = context_.AccountRequest(request);
            step_prefix_append_only = account.append_only;
            step_epoch_break_reason = account.break_reason;
            if (account.had_previous && PrefixDebugEnabled()) {
                // 诊断行:只带断因/位置/条数,不带任何正文与 hash 以外的东西。
                std::string prefix_diag = "[prefix] epoch " + std::to_string(context_.cache_epoch()) +
                                          " step " + std::to_string(step_index) + " append_only=" +
                                          (step_prefix_append_only ? "true" : "false");
                if (!step_prefix_append_only) {
                    prefix_diag += " reason=" + step_epoch_break_reason +
                                   " old_message_changed_at=" + std::to_string(account.old_message_changed_at);
                }
                prefix_diag += " appended_messages=" + std::to_string(account.appended_messages) +
                               " system_hash=" + account.system_hash.substr(0, 8) +
                               " tools_hash=" + account.tools_hash.substr(0, 8);
                platform::LogSink::Instance().Debug("loop", prefix_diag);
            }
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
                                callbacks.on_builtin_tool_start(e.id, e.name, e.input);
                            }
                        } else if constexpr (std::is_same_v<T, api::BuiltinToolDone>) {
                            if (callbacks.on_builtin_tool_done) {
                                callbacks.on_builtin_tool_done(e.id, e.name, e.input, e.summary, e.is_error);
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
        // 逐枚追踪:assistant 消息一入 history 就交装配层 append+flush 进
        // session(单子:provider assistant message 在执行工具前落盘)。
        // 老路(收口后 PersistNewMessages)照旧兜底——没装 trace 的会话
        // 一字不变;装了的,PersistNew 的只增不减账不会重复落(persisted
        // 基线此刻还没推进)。
        if (callbacks.on_assistant_message_ready) {
            callbacks.on_assistant_message_ready(assistant_message);
        }
        context_.PushMessage(std::move(assistant_message));
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
            report.cache_epoch = context_.cache_epoch();
            report.epoch_break_reason = step_epoch_break_reason;
            report.prefix_append_only = step_prefix_append_only;
            callbacks.on_usage(report);
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
        // 执行、结果还没发回"那个当口——正在跑的这个工具照常等它跑完、结果
        // 照常入历史;还没轮到的后续工具不再真的执行,补一条"未执行"的合成
        // 结果)保住 tool_use/tool_result 的成对约束,再从 Run() 正常返回。
        //
        // 逐枚追踪(单子"消息落盘次序要改"):assistant message 入 history
        // 后先发批次头(装配层此刻把 assistant 消息 append+flush 进
        // session),再为每枚 tool use 写 scheduled,然后逐枚执行
        // (started -> finished),五枚结果收齐、合并的 user 消息入 history
        // 后再发批次尾(装配层补落 user 消息 + 各枚 result_committed)。
        // 审计按枚及时落,崩溃窗口从"整轮"缩到"当前这枚";wire 语义不变,
        // 五枚结果仍同一条 user message。
        const bool trace_armed = callbacks.on_tool_trace != nullptr;
        std::string batch_id;
        int sequence_in_batch = 0;
        if (trace_armed) {
            batch_id = "batch-" + std::to_string(++batch_counter_);
        }
        std::vector<std::string> scheduled_ids;  // 本批各枚 execution_id(装 trace 时才有)
        std::vector<std::string> scheduled_tool_use_ids;
        std::vector<std::string> scheduled_names;
        if (trace_armed && callbacks.on_tool_trace) {
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
                callbacks.on_tool_trace(scheduled);
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
            if (!batch_ids.empty() && callbacks.on_tool_batch_started) {
                callbacks.on_tool_batch_started(step_index, static_cast<int>(batches_emitted), batch_ids);
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
                    callbacks.on_tool_trace(cancelled);
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
            const tools::Tool::Result result =
                RunOneTool(registry_, call, callbacks, tool_filter_, tool_filter_denial_,
                           trace_armed ? &trace_ctx : nullptr);
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
        // 批次尾回调要在消息 move 进双账之前拿:回调里读的是五枚结果齐的
        // user message(装配层此刻 append+flush 它)。
        const bool results_callback_armed = callbacks.on_tool_results_committed != nullptr;
        api::Message message_for_callback = results_callback_armed ? tool_result_message : api::Message{};
        context_.PushMessage(std::move(tool_result_message));

        if (batch_index_for_this_step >= 0 && callbacks.on_tool_batch_finished) {
            callbacks.on_tool_batch_finished(batch_index_for_this_step, interrupted);
        }

        if (trace_armed) {
            // 批次尾:结果消息本体已入 history。先交装配层 append+flush user
            // 消息(设了回调的会话),再为每枚发 result_committed 栅栏——
            // 栅栏是 canonical 事件流的一部分,不依赖装配层是否监听消息
            // 落盘;若这里之前崩溃,resume 由 trace 重建:finished 的从
            // result ref 恢复,started 无 finished 的标 unknown,只有
            // scheduled 的标未执行(单子"消息落盘次序")。
            if (results_callback_armed) {
                callbacks.on_tool_results_committed(batch_id, message_for_callback);
            }
            for (const std::string& execution_id : scheduled_ids) {
                ToolTraceEvent committed;
                committed.kind = ToolTraceEventKind::ResultCommitted;
                committed.batch_id = batch_id;
                committed.execution_id = execution_id;
                committed.timestamp_ms = NowMsEpoch();
                callbacks.on_tool_trace(committed);
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
