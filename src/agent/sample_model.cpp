// SampleModel 原语的实现(骨架拆解单批一·病四)。头注释是行为对账的账本。

#include "agent/sample_model.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "tools/schema_check.hpp"  // output_schema 复检:与工具入参同一只校验器
namespace lubancode::agent {

SampleResult SampleModel(api::Backend& backend, const SampleRequest& request, const SampleOptions& options) {
    api::Request wire;
    wire.model = request.model;
    wire.system = request.system;
    wire.messages = request.messages;
    wire.max_tokens = request.max_tokens;
    wire.reasoning_effort = request.reasoning_effort;

    if (request.enforce_output_limit && request.max_tokens) {
        backend.ForceMaxOutputTokensOverride(wire, *request.max_tokens);
        const auto effective = backend.GetEffectiveOutputLimit(wire);
        if (!effective.tokens || *effective.tokens <= 0 || *effective.tokens > *request.max_tokens) {
            SampleResult blocked;
            blocked.error = api::Error{api::ErrorKind::Api,
                "auxiliary output limit could not be enforced", 0, "output_limit_unenforceable"};
            return blocked;
        }
    }

    const auto started = std::chrono::steady_clock::now();

    // Token 账本单 A1(公共 ModelRequestRecorder,§11.2):旁路采样与
    // AgentLoop 走同一套模型边界。prepared 记不住就停在边界不发模型
    // (§7.4 耐久栅栏;旁路小活各自的旧兜底照走——标题不起、压缩报错,
    // 与"账写不住"同一档)。usage owner 与 output 三态收口在采样终态处。
    std::string recorded_request_id;
    if (options.boundary_recorder != nullptr) {
        RequestPreparedContext prepared_ctx;
        prepared_ctx.purpose = options.purpose;
        prepared_ctx.timeout_budget_secs = options.timeout_secs;
        recorded_request_id = options.boundary_recorder->OnRequestPrepared(wire, prepared_ctx);
        if (recorded_request_id.empty()) {
            SampleResult blocked;
            blocked.ok = false;
            blocked.error = api::Error{api::ErrorKind::Api, "轨迹账写盘失败,采样停在请求边界,未发模型", 0};
            blocked.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
            return blocked;
        }
        if (!options.boundary_recorder->OnRequestSent(recorded_request_id)) {
            // 发送前写账硬闸(失败与恢复单 P1-C/FA-03):sent 记不住,采样停
            // 在边界不发模型——与 prepared 同一道耐久栅栏。
            SampleResult blocked;
            blocked.ok = false;
            blocked.error = api::Error{api::ErrorKind::Api, "轨迹账写盘失败,采样停在发送边界,未发模型", 0};
            blocked.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
            return blocked;
        }
    }

    // 看门狗(与旧六处同一形状:steady clock 差 + 100ms 轮询,到点拉本地
    // 旗)。取消误报 ESC 单把取消口收成三形:
    //   只外部链(无预算):effective = 外部链,零看门狗,旧行为一字不动;
    //   只预算(无外部链):effective = 本地旗,旧行为一字不动;
    //   两者同时在场:合并旗——看门狗两头盯(与 goal evaluator 组合旗同
    //   一形状),任一升起都掐流,并记下谁先升(fired:1=外部链,2=本地
    //   deadline)。旧"外部链在场时超时不抢断、本地旗无人读"的死档废除
    //   (§四-2:统一归因时不许漏这个组合)。
    // join 必须有:本地旗活在本栈,detach 出去的线程不许越栈引用。
    std::atomic<bool> local_cancel{false};
    std::atomic<bool> merged_cancel{false};
    std::atomic<int> cancel_fired{0};  // 合并形专用:1=外部链先升,2=deadline 先到
    std::atomic<bool> done{false};
    std::optional<std::thread> watchdog;
    const bool dual_cancel = options.cancel != nullptr && options.timeout_secs > 0;
    if (options.timeout_secs > 0 && !dual_cancel) {
        watchdog.emplace([&local_cancel, &done, timeout = options.timeout_secs]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
            while (!done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!done.load()) local_cancel = true;
        });
    } else if (dual_cancel) {
        watchdog.emplace([&merged_cancel, &cancel_fired, &done, external = options.cancel,
                          timeout = options.timeout_secs]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
            while (!done.load()) {
                if (external != nullptr && external->load()) {
                    cancel_fired.store(1);
                    merged_cancel.store(true);
                    return;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    cancel_fired.store(2);
                    merged_cancel.store(true);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    api::MessageAssembler assembler;
    bool stream_error = false;
    std::string stream_error_message;
    std::string stream_error_code;
    std::string assembler_response_id;
    const std::atomic<bool>* effective_cancel = dual_cancel ? &merged_cancel
        : (options.cancel != nullptr ? options.cancel
                                     : (options.timeout_secs > 0 ? &local_cancel : nullptr));
    const auto sent = backend.send_stream(
        wire,
        [&](const api::StreamEvent& event) {
            assembler.Feed(event);
            std::visit(
                [&](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, api::MessageStart>) {
                        // provider 回的外部号(§6.1.2):只作对账,不铸本地
                        // 身份。AgentLoop 侧同源同口径。
                        assembler_response_id = e.id;
                    } else if constexpr (std::is_same_v<T, api::StreamError>) {
                        stream_error = true;
                        stream_error_message = e.message;
                        stream_error_code = e.code;
                    }
                },
                event);
        },
        effective_cancel);
    done = true;
    if (watchdog.has_value()) {
        watchdog->join();
    }

    // ---- 取消归因(取消误报 ESC 单 Bug 1):四分,不臆断按键 ------------
    // 完成与 deadline 同场的裁决:MessageDone 已见过(stop_reason 非空)=
    // 完整成功响应——收尾才升的取消旗不改判,响应一字不少就不算取消。
    // 反过来:取消不能伪造成功,半截就是半截。
    const bool cancel_after_complete =
        !sent.has_value() && sent.error().kind == api::ErrorKind::Cancelled && !stream_error &&
        !assembler.stop_reason().empty();
    OutputCancelSource cancel_source = OutputCancelSource::StreamError;  // 谁的旗都没升=来源未知
    bool local_deadline_hit = false;
    if (dual_cancel) {
        if (cancel_fired.load() == 1) {
            cancel_source = options.cancel_source;  // 外部链先升:按升旗人申报
        } else if (cancel_fired.load() == 2) {
            cancel_source = OutputCancelSource::Internal;
            local_deadline_hit = true;
        }
    } else if (options.cancel != nullptr && options.cancel->load()) {
        cancel_source = options.cancel_source;
    } else if (options.cancel == nullptr && local_cancel.load()) {
        cancel_source = OutputCancelSource::Internal;
        local_deadline_hit = true;
    }
    // 返回错误的文案随归因修正:HTTP 层只知取消信号,给的是中性话;这里
    // 才是知道来源的一层。deadline 带预算与稳定码 local_deadline;用户取
    // 消只在升旗人申报过 UserInterrupt 时才说(证据 = 交互层按键监听);
    // 来源未知保持中性,不冒充按键。
    api::Error attributed_error;
    bool error_attributed = false;
    if (!sent.has_value() && sent.error().kind == api::ErrorKind::Cancelled && !cancel_after_complete) {
        attributed_error = sent.error();
        error_attributed = true;
        if (local_deadline_hit) {
            attributed_error.message =
                "采样超过 " + std::to_string(options.timeout_secs) + " 秒,被本地超时预算停止";
            attributed_error.api_code = "local_deadline";
        } else if (cancel_source == OutputCancelSource::UserInterrupt) {
            attributed_error.message = "用户取消了这次请求";
        } else if (cancel_source == OutputCancelSource::Internal) {
            attributed_error.message = "请求被宿主内部取消信号停止";
        }
    }

    SampleResult result;
    // usage 半截也出账(旧口径:六处都是先记账再判错)。
    const api::Usage& usage = assembler.usage();
    result.usage = usage;
    result.usage_reported = usage.input_tokens > 0 || usage.output_tokens > 0 || usage.cache_read_tokens > 0 ||
                           usage.cache_creation_tokens > 0 || usage.output_reasoning_tokens > 0;
    // 半截流(无 ContentBlockDone/MessageDone 收尾)先催收再取,文本不丢
    // ——llm 节点旧路按裸 TextDelta 累加,这里不许比它少一个字。已收尾时
    // 催收是空操作。
    assembler.FinalizeOpenBlock();
    for (const auto& block : assembler.BuildMessage().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            result.text += text->text;
        }
    }
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    result.provider_response_id = assembler_response_id;
    result.stop_reason = assembler.stop_reason();

    // 轨迹收口(Token 账本单 A1,§6.1.1/§7.3):usage owner 先落(没报也
    // 落 owner,token 字段不现),output 随后按三态自己收口,不复制
    // usage。半截流的账照样落——usage 半截也出账是本原语的旧口径,
    // 轨迹同账。
    if (options.boundary_recorder != nullptr && !recorded_request_id.empty()) {
        options.boundary_recorder->OnUsageRecorded(recorded_request_id, usage, result.usage_reported,
                                                   result.provider_response_id);
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content = assembler.BuildMessage().content;
        if (!sent.has_value() && !cancel_after_complete) {
            if (sent.error().kind == api::ErrorKind::Cancelled) {
                // 取消来源说真话(主会话输出预留占坑单 §4.2 + 取消误报 ESC
                // 单 Bug 1):四分归因与返回错误/终端共用同一个 cancel_source
                // ——用户取消只在升旗人申报时记 user_interrupt;本地 deadline
                // 与宿主内部停止记 internal;谁的旗都没升却回取消分型的,按
                // 流侧异常记账,不冒充按键。
                options.boundary_recorder->OnOutputCancelled(recorded_request_id, cancel_source);
            } else {
                options.boundary_recorder->OnOutputFailed(recorded_request_id, sent.error().message);
            }
        } else if (stream_error) {
            options.boundary_recorder->OnOutputFailed(recorded_request_id, stream_error_message);
        } else {
            // 含"完成与 deadline 同场"的裁决胜者:完整成功响应照走 completed。
            options.boundary_recorder->OnOutputCompleted(recorded_request_id, assistant,
                result.stop_reason.empty() ? "end_turn" : result.stop_reason,
                                                         result.provider_response_id);
        }
    }

    if (!sent.has_value() && !cancel_after_complete) {
        result.ok = false;
        result.error = error_attributed ? attributed_error : sent.error();
        return result;
    }
    if (stream_error) {
        result.ok = false;
        result.error = api::Error{api::ErrorKind::Api, stream_error_message, 0, stream_error_code};
        return result;
    }
    result.ok = true;

    // output_schema 复检(设了才跑):正文先解析成 JSON,再过与工具入参
    // 同一只校验器。复检不翻 ok——采样本身成了,正文合不合schema由调用方
    // 拿 schema_ok 自己兜底。
    if (!request.output_schema.empty()) {
        const nlohmann::json parsed = nlohmann::json::parse(result.text, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            // 解析失败在 allow_exceptions=false 下回的是 discarded 值,不是
            // null——字面 "null" 正文是合法 JSON,得交给 schema 按 type 去拒。
            result.schema_ok = false;
            result.schema_error = "采样正文不是合法 JSON";
        } else if (const auto schema_error = tools::ValidateInputAgainstSchema(parsed, request.output_schema);
                   schema_error.has_value()) {
            result.schema_ok = false;
            result.schema_error = *schema_error;
        }
    }
    return result;
}

void AddSampleAccounting(BackgroundCallAccounting* accounting, const SampleResult& result) {
    if (accounting == nullptr) {
        return;
    }
    accounting->usage.input_tokens += result.usage.input_tokens;
    accounting->usage.cache_read_tokens += result.usage.cache_read_tokens;
    accounting->usage.cache_creation_tokens += result.usage.cache_creation_tokens;
    accounting->usage.output_tokens += result.usage.output_tokens;
    accounting->usage.output_reasoning_tokens += result.usage.output_reasoning_tokens;
    accounting->usage_reported =
        accounting->usage_reported || result.usage_reported;
}

}  // namespace lubancode::agent
