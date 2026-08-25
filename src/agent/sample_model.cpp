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
    const std::atomic<bool>* effective_cancel =
        options.cancel != nullptr ? options.cancel : (options.timeout_secs > 0 ? &local_cancel : nullptr);
    const auto sent = backend.send_stream(
        wire,
        [&](const api::StreamEvent& event) {
            assembler.Feed(event);
            if (const auto* error = std::get_if<api::StreamError>(&event)) {
                stream_error = true;
                stream_error_message = error->message;
            }
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

    if (!sent.has_value()) {
        result.ok = false;
        result.error = sent.error();
        return result;
    }
    if (stream_error) {
        result.ok = false;
        result.error = api::Error{api::ErrorKind::Api, stream_error_message, 0};
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
