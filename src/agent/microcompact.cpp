#include "agent/microcompact.hpp"

#include <atomic>
#include <memory>
#include <sstream>
#include <utility>
#include <variant>

#include "accounting/purpose.hpp"  // RequestPurpose(Token 账本单 A1)
#include "agent/context.hpp"  // EstimateUtf8Tokens(输入估算用)
#include "agent/sample_model.hpp"  // SampleModel 原语:采样的公共路(批一·病四)

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

std::optional<MicrocompactSummary> ParseMicrocompactSummary(const std::string& text,
                                                            const std::string& artifact_id) {
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
        return std::nullopt;  // 残次摘要拒收,旧消息不动
    }
    if (root.contains("key_facts") && root["key_facts"].is_array()) {
        for (const auto& item : root["key_facts"]) {
            if (item.is_string() && summary.key_facts.size() < 16) {
                summary.key_facts.push_back(item.get<std::string>());
            }
        }
    }
    summary.source_artifact_id = artifact_id;
    return summary;
}

std::expected<MicrocompactSummary, std::string> RunMicrocompact(
    api::Backend& backend, const std::string& model, const std::string& reasoning_effort,
    const ContextArtifactStore& store, const ArtifactRef& ref, const MicrocompactOptions& options,
    BackgroundCallAccounting* accounting,
    const std::atomic<bool>* external_cancel) {
    if (external_cancel != nullptr && external_cancel->load()) {
        return std::unexpected("按需摘要已取消");
    }
    // 输入永远从 blob 原文来:hash 门在 ReadBlobVerified,不合/读不到就跳过
    // 这枚,绝不拿旧摘要再摘要。
    const auto original = store.ReadBlobVerified(ref);
    if (!original.has_value()) {
        return std::unexpected("原文 blob 读不到/hash 不合,无法生成按需摘要");
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

    // 采样走 SampleModel 原语(批一·病四):外部取消链 + 超时看门狗都交
    // 给原语(旧口径:到点拉旗;外部链取消的传播由原语直连,不再多绕
    // 100ms 轮询一道手)。
    SampleRequest sample;
    sample.model = model;
    sample.reasoning_effort = reasoning_effort;
    sample.system =
        "以下是一次工具调用的完整输出(可能中段省略)。请把它收成一份局部摘要,严格输出一个 JSON 对象"
        "(不要围栏、不要解释),键名逐字照写:\n"
        "{\"summary\": \"三五句话:这次调用做了什么、结论是什么\", "
        "\"key_facts\": [\"必须保住的决定、错误码、路径、符号、命令、退出码、未完成项、用户纠正,一条一句\"]}\n"
        "key_facts 只收这段输出里确证过的内容,不许猜补;没有就给空数组。"
        "错误行、失败原因、退出码、文件路径一个都不能丢——摘要给后续工作接力用。";
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(
        api::TextBlock{"工具 " + ref.tool_name + "(调用 " + ref.tool_use_id + ")的输出:\n\n" + input});
    sample.messages.push_back(std::move(message));
    sample.max_tokens = 1024;

    SampleOptions sample_options;
    sample_options.cancel = external_cancel;
    sample_options.timeout_secs = options.timeout_secs;
    // Token 账本单 A1:旁路桥在场就把这次 L2 请求落成 Journal 里的
    // prepared/sent/usage/output(purpose=compact_map)。
    std::unique_ptr<LoopBoundaryRecorder> recorder;
    if (options.bypass_recorder != nullptr) {
        recorder = options.bypass_recorder();
        sample_options.boundary_recorder = recorder.get();
        sample_options.purpose = accounting::RequestPurpose::CompactMap;
    }
    const SampleResult sampled = SampleModel(backend, sample, sample_options);

    if (accounting != nullptr) {
        accounting->usage.input_tokens += sampled.usage.input_tokens;
        accounting->usage.cache_read_tokens += sampled.usage.cache_read_tokens;
        accounting->usage.cache_creation_tokens += sampled.usage.cache_creation_tokens;
        accounting->usage.output_tokens += sampled.usage.output_tokens;
        accounting->usage.output_reasoning_tokens += sampled.usage.output_reasoning_tokens;
        accounting->usage_reported = sampled.usage_reported;
        accounting->duration_ms = sampled.duration_ms;
    }

    if (!sampled.ok) {
        return std::unexpected(sampled.error.message);
    }
    auto summary = ParseMicrocompactSummary(sampled.text, ref.artifact_id);
    if (!summary.has_value()) {
        return std::unexpected("cheap 输出不是合法摘要 JSON;旧 artifact 与原文未改");
    }
    summary->model = model;
    summary->derived_from_summary = false;  // 输入永远来自 blob 原文
    return *summary;
}

}  // namespace lubancode::agent
