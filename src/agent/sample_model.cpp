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

    const auto started = std::chrono::steady_clock::now();

    // Token 账本单 A1(公共 ModelRequestRecorder,§11.2):旁路采样与
    // AgentLoop 走同一套模型边界。prepared 记不住就停在边界不发模型
    // (§7.4 耐久栅栏;旁路小活各自的旧兜底照走——标题不起、压缩报错,
    // 与"账写不住"同一档)。usage owner 与 output 三态收口在采样终态处。
    std::string recorded_request_id;
    if (options.boundary_recorder != nullptr) {
        RequestPreparedContext prepared_ctx;
        prepared_ctx.purpose = options.purpose;
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
        options.boundary_recorder->OnRequestSent(recorded_request_id);
    }

    // 看门狗(与旧六处同一形状:steady clock 差 + 100ms 轮询,到点拉本地
    // 旗)。外部取消链在场时 send_stream 只吃外部链——本地旗没人读,超时
    // 不抢断,goal evaluator 的旧口径如实保留。join 必须有:本地旗活在
    // 本栈,detach 出去的线程不许越栈引用。
    std::atomic<bool> local_cancel{false};
    std::atomic<bool> done{false};
    std::optional<std::thread> watchdog;
    if (options.timeout_secs > 0) {
        watchdog.emplace([&local_cancel, &done, timeout = options.timeout_secs]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
            while (!done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!done.load()) local_cancel = true;
        });
    }

    api::MessageAssembler assembler;
    bool stream_error = false;
    std::string stream_error_message;
    std::string stream_error_code;
    std::string assembler_response_id;
    const std::atomic<bool>* effective_cancel =
        options.cancel != nullptr ? options.cancel : (options.timeout_secs > 0 ? &local_cancel : nullptr);
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
        if (!sent.has_value()) {
            if (sent.error().kind == api::ErrorKind::Cancelled) {
                options.boundary_recorder->OnOutputCancelled(recorded_request_id);
            } else {
                options.boundary_recorder->OnOutputFailed(recorded_request_id, sent.error().message);
            }
        } else if (stream_error) {
            options.boundary_recorder->OnOutputFailed(recorded_request_id, stream_error_message);
        } else {
            options.boundary_recorder->OnOutputCompleted(recorded_request_id, assistant, "end_turn",
                                                         result.provider_response_id);
        }
    }

    if (!sent.has_value()) {
        result.ok = false;
        result.error = sent.error();
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
        if (parsed.is_null()) {
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
