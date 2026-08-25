#include "agent/microcompact.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "agent/context.hpp"  // EstimateUtf8Tokens(输入估算用)
#include "api/assembler.hpp"

namespace lubancode::agent {

namespace {

std::string TrimWhitespace(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

std::size_t ColdArtifactBytes(const std::vector<api::Message>& history, const ResultViewMemo& memo) {
    // 口径:事件账里"冷区 + Artifact/L2 视图"的结果原文累计字节。
    const std::size_t hot_start = HotZoneStartIndex(history);
    std::size_t total = 0;
    const auto ledger = BuildEventLedger(history);
    for (const auto& event : ledger) {
        if (event.message_index >= hot_start) {
            continue;  // 热区不碰
        }
        const auto it = memo.decisions.find(event.tool_use_id);
        if (it == memo.decisions.end() ||
            (it->second.kind != ResultViewKind::Artifact && it->second.kind != ResultViewKind::MicrocompactSummary)) {
            continue;
        }
        total += event.result_content.size();
    }
    return total;
}

std::vector<MicrocompactCandidate> PickMicrocompactCandidates(const std::vector<api::Message>& history,
                                                              const ResultViewMemo& memo,
                                                              const MicrocompactOptions& options,
                                                              const MicrocompactHysteresis& hysteresis) {
    std::vector<MicrocompactCandidate> candidates;
    const std::size_t cold_bytes = ColdArtifactBytes(history, memo);
    // 触发线 + 迟滞:没过线不压;上趟压过后,冷区要比上趟再涨 cooldown_growth_percent
    // 才准再压(免得刚压完又立刻重压,规格"用迟滞避免刚压完又立刻重压")。
    if (cold_bytes < options.cold_trigger_bytes) {
        return candidates;
    }
    if (hysteresis.pass_attempted) {
        const std::size_t threshold =
            hysteresis.last_pass_cold_bytes +
            hysteresis.last_pass_cold_bytes * static_cast<std::size_t>(options.cooldown_growth_percent) / 100;
        if (cold_bytes < threshold) {
            return candidates;
        }
    }

    const std::size_t hot_start = HotZoneStartIndex(history);
    const auto ledger = BuildEventLedger(history);
    for (const auto& event : ledger) {
        if (event.message_index >= hot_start) {
            continue;  // 热区绝不碰
        }
        const auto it = memo.decisions.find(event.tool_use_id);
        if (it == memo.decisions.end() || it->second.kind != ResultViewKind::Artifact ||
            it->second.artifact_id.empty()) {
            continue;  // 只收拾"已落盘、还是 L1 预览"的;已是 L2 的不重做
        }
        MicrocompactCandidate candidate;
        candidate.tool_use_id = event.tool_use_id;
        candidate.artifact_id = it->second.artifact_id;
        candidate.event_id = event.id;
        candidate.tool_name = event.tool_name;
        candidate.content_bytes = event.result_content.size();
        candidates.push_back(std::move(candidate));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const MicrocompactCandidate& a, const MicrocompactCandidate& b) {
                  return a.content_bytes > b.content_bytes;  // 大头先收拾
              });
    if (candidates.size() > static_cast<std::size_t>(options.max_per_pass)) {
        candidates.resize(static_cast<std::size_t>(options.max_per_pass));
    }
    return candidates;
}

std::optional<MicrocompactSummary> ParseMicrocompactSummary(const std::string& text,
                                                            const std::string& artifact_id,
                                                            const std::string& event_id) {
    // 剥 ```json 围栏;没有围栏就取首 { 到末 }。
    std::size_t begin = text.find('{');
    std::size_t end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return std::nullopt;
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.substr(begin, end - begin + 1));
    } catch (...) {
        return std::nullopt;
    }
    if (!root.is_object() || !root.contains("summary") || !root["summary"].is_string()) {
        return std::nullopt;
    }
    MicrocompactSummary summary;
    summary.summary = TrimWhitespace(root["summary"].get<std::string>());
    if (summary.summary.empty() || summary.summary.size() < 20) {
        return std::nullopt;  // 残次摘要拒收,退 L1
    }
    if (root.contains("key_facts") && root["key_facts"].is_array()) {
        for (const auto& item : root["key_facts"]) {
            if (item.is_string() && summary.key_facts.size() < 16) {
                summary.key_facts.push_back(item.get<std::string>());
            }
        }
    }
    summary.source_artifact_id = artifact_id;
    summary.source_event_id = event_id;
    return summary;
}

std::expected<MicrocompactSummary, std::string> RunMicrocompact(
    api::Backend& backend, const std::string& model, const std::string& reasoning_effort,
    const ContextArtifactStore& store, const ArtifactRef& ref, const std::string& event_id,
    const MicrocompactOptions& options, BackgroundCallAccounting* accounting,
    const std::atomic<bool>* external_cancel) {
    if (external_cancel != nullptr && external_cancel->load()) {
        return std::unexpected("后台微压缩已取消");
    }
    // 输入永远从 blob 原文来:hash 门在 ReadBlobVerified,不合/读不到就跳过
    // 这枚(退 L1),绝不拿旧摘要再摘要。
    const auto original = store.ReadBlobVerified(ref);
    if (!original.has_value()) {
        return std::unexpected("原文 blob 读不到/hash 不合,跳过这枚(保持 L1 预览)");
    }
    // 头尾各半截到 input_cap_bytes:行边界截,不劈码点。
    std::string input = *original;
    if (input.size() > options.input_cap_bytes) {
        const std::size_t half = options.input_cap_bytes / 2;
        std::size_t head_end = half;
        while (head_end > 0 && head_end < input.size() &&
               (static_cast<unsigned char>(input[head_end]) & 0xC0) == 0x80) {
            ++head_end;  // 推到码点边界
        }
        while (head_end > 0 && input[head_end - 1] != '\n' && head_end > half - 2048) {
            --head_end;  // 退到行边界(最多让 2KB)
        }
        std::size_t tail_begin = input.size() > half ? input.size() - half : 0;
        while (tail_begin < input.size() && (static_cast<unsigned char>(input[tail_begin]) & 0xC0) == 0x80) {
            ++tail_begin;
        }
        while (tail_begin < input.size() && input[tail_begin] != '\n' && tail_begin < input.size() - half + 2048) {
            ++tail_begin;
        }
        input = input.substr(0, head_end) + "\n…[中段省略 " +
                std::to_string((tail_begin - head_end) / 1024) + " KiB,可用 context_read 按行窗读取]…\n" +
                input.substr(tail_begin);
    }

    api::Request request;
    request.model = model;
    request.reasoning_effort = reasoning_effort;
    request.system =
        "以下是一次工具调用的完整输出(可能中段省略)。请把它收成一份局部摘要,严格输出一个 JSON 对象"
        "(不要围栏、不要解释),键名逐字照写:\n"
        "{\"summary\": \"三五句话:这次调用做了什么、结论是什么\", "
        "\"key_facts\": [\"必须保住的决定、错误码、路径、符号、命令、退出码、未完成项、用户纠正,一条一句\"]}\n"
        "key_facts 只收这段输出里确证过的内容,不许猜补;没有就给空数组。"
        "错误行、失败原因、退出码、文件路径一个都不能丢——摘要给后续工作接力用。";
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"工具 " + ref.tool_name + "(事件 " + event_id + ")的输出:\n\n" + input});
    request.messages.push_back(std::move(message));
    request.max_tokens = 1024;
    const auto started = std::chrono::steady_clock::now();

    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::thread watchdog([&cancel, &done, external_cancel, timeout = options.timeout_secs]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            if (external_cancel != nullptr && external_cancel->load()) {
                cancel = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load()) cancel = true;
    });

    api::MessageAssembler assembler;
    bool stream_error = false;
    std::string stream_error_message;
    auto send_result = backend.send_stream(
        request,
        [&](const api::StreamEvent& event) {
            assembler.Feed(event);
            if (const auto* error = std::get_if<api::StreamError>(&event)) {
                stream_error = true;
                stream_error_message = error->message;
            }
        },
        &cancel);
    done = true;
    watchdog.join();

    if (accounting != nullptr) {
        const api::Usage& usage = assembler.usage();
        accounting->usage.input_tokens += usage.input_tokens;
        accounting->usage.cache_read_tokens += usage.cache_read_tokens;
        accounting->usage.cache_creation_tokens += usage.cache_creation_tokens;
        accounting->usage.output_tokens += usage.output_tokens;
        accounting->usage.output_reasoning_tokens += usage.output_reasoning_tokens;
        accounting->usage_reported = usage.input_tokens > 0 || usage.output_tokens > 0 ||
                                     usage.cache_read_tokens > 0 || usage.cache_creation_tokens > 0;
        accounting->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
    }

    if (!send_result.has_value()) {
        return std::unexpected(send_result.error().message);
    }
    if (stream_error) {
        return std::unexpected(stream_error_message);
    }
    std::string reply;
    for (const auto& block : assembler.BuildMessage().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            reply += text->text;
        }
    }
    auto summary = ParseMicrocompactSummary(reply, ref.artifact_id, event_id);
    if (!summary.has_value()) {
        return std::unexpected("cheap 输出不是合法摘要 JSON,退回 L1");
    }
    summary->model = model;
    summary->derived_from_summary = false;  // 输入永远来自 blob 原文
    return *summary;
}

int ApplyMicrocompactSummaries(ResultViewMemo& memo,
                               const std::map<std::string, MicrocompactSummary>& summaries) {
    int applied = 0;
    for (const auto& [tool_use_id, summary] : summaries) {
        auto it = memo.decisions.find(tool_use_id);
        if (it == memo.decisions.end() || it->second.kind != ResultViewKind::Artifact) {
            continue;  // 决策不在/已不是 L1 形态:不动(绝不改 Full/重复引用)
        }
        it->second.kind = ResultViewKind::MicrocompactSummary;
        it->second.summary_text = summary.summary;
        it->second.summary_model = summary.model;
        // key_facts 并进正文(一行一条),摘要与事实单都在视图里。
        if (!summary.key_facts.empty()) {
            it->second.summary_text += "\n关键事实:";
            for (const auto& fact : summary.key_facts) {
                it->second.summary_text += "\n- " + fact;
            }
        }
        ++applied;
    }
    return applied;
}

}  // namespace lubancode::agent
