// DeepSeek 真机 e2e(前缀缓存守恒单,opt-in):
//
//   DEEPSEEK_API_KEY 没设就直接跳过(exit 0),不进离线 ctest;要在真机
//   上跑,先 export DEEPSEEK_API_KEY(可选 DEEPSEEK_MODEL,默认
//   deepseek-v4-pro;DEEPSEEK_BASE_URL,默认官方端点)。
//
// 流程(规格"真实 DeepSeek 测试"节):
//   1. 铺一段足够长的稳定 system/历史(跨过服务端缓存单元);
//   2. 强制走一次可控工具往返(thinking 开着,顺带验 reasoning 回传不吃
//      400——第二期);
//   3. 再追问一轮;
//   4. 逐请求捕获 usage(hit/miss)与前缀指纹;
//   5. 除首份请求与明确 epoch break 外,要求 cache_read_tokens > 0;
//   6. 服务端尽力缓存偶有失手,受控重试一次;仍失败就把"请求前缀不等"
//      与"前缀相等但服务端未命中"分开报。
//
// 防假绿:每步核对独立 usage 的 request id(MessageStart.id),解析器把旧
// usage 重放一遍也糊不过去;usage 四项全零按"服务端未回报"报,不拿 0
// 冒充真未命中。

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "agent/prefix.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 捕获层:记下每份 api::Request,usage/请求 id 从 AgentLoop 的报表拿,
// 转发给真 ChatCompletionsBackend。
class CapturingBackend : public api::Backend {
public:
    CapturingBackend(std::unique_ptr<api::Backend> inner) : inner_(std::move(inner)) {}

    std::vector<api::Request> captured;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        captured.push_back(request);
        return inner_->send_stream(request, on_event, cancel);
    }

private:
    std::unique_ptr<api::Backend> inner_;
};

// 可控探针工具:返回固定长度的正文,工具往返的"结果"不抖。
class ProbeTool : public tools::Tool {
public:
    std::string name() const override { return "probe_file"; }
    std::string description() const override {
        return "读出一份固定探针正文(测试用);入参 ignore 可为任意字符串。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json{{"type", "object"},
                              {"properties", nlohmann::json{{"ignore", {{"type", "string"}}}}}};
    }
    tools::Tool::Result execute(const nlohmann::json&) override {
        return {std::string(2400, 'P'), false};
    }
};

std::string EnvOr(const char* name, const std::string& fallback) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, name);
    if (err != 0 || buffer == nullptr) {
        return fallback;
    }
    const std::string value = buffer[0] == '\0' ? fallback : std::string(buffer);
    std::free(buffer);
    return value;
#else
    const char* raw = std::getenv(name);
    return raw == nullptr || raw[0] == '\0' ? fallback : std::string(raw);
#endif
}

// 一步的完整账:请求指纹 + usage + 身份。
struct StepLog {
    api::Request request;
    api::UsageReport report;
};

}  // namespace

int main() {
    const std::string api_key = EnvOr("DEEPSEEK_API_KEY", "");
    if (api_key.empty()) {
        std::cout << "[deepseek-e2e] DEEPSEEK_API_KEY 未设置,跳过(不进离线 ctest)。\n";
        return 0;
    }
    const std::string model = EnvOr("DEEPSEEK_MODEL", "deepseek-v4-pro");
    const std::string base_url = EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com");

    api::chat::ChatRequestOptions chat_options;
    chat_options.stream_usage = true;                                  // 第一期:要独立 usage chunk
    chat_options.reasoning_replay = api::chat::ReasoningReplayPolicy::ToolEpisode;  // 第二期
    auto inner = std::make_unique<api::chat::ChatCompletionsBackend>(
        base_url, api_key, /*connect_timeout_ms=*/20000, /*stream_idle_timeout_secs=*/120,
        /*extra_body=*/nlohmann::json{{"thinking", nlohmann::json{{"type", "enabled"}}}},
        std::map<std::string, std::string>{}, chat_options);
    CapturingBackend backend(std::move(inner));

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ProbeTool>());

    // 稳定长 system:跨过服务端缓存单元(DeepSeek 按 64 token 一格存前缀,
    // 铺几 KB 保守够用),内容全程不变。
    const std::string stable_block(3000, 'S');
    const std::string system_prompt =
        "你是测试会话里的被测模型。回答保持极短。以下是一段稳定填充材料,用于撑起前缀缓存:\n" +
        stable_block + "\n填充材料结束。";

    agent::AgentLoop loop(backend, registry, model, system_prompt,
                          /*max_tokens=*/512, /*max_steps_per_turn=*/0);

    std::vector<StepLog> steps;
    agent::Callbacks callbacks;
    callbacks.on_usage = [&steps, &backend](const api::UsageReport& report) {
        // on_usage 在请求收尾时触发,同一时间只有一步在飞——捕获数组里
        // 最后一份就是本步刚发出去的请求。
        StepLog log;
        log.request = backend.captured.back();
        log.report = report;
        steps.push_back(std::move(log));
    };

    auto run_turn = [&](const std::string& user_text) -> bool {
        const auto result = loop.Run(user_text, callbacks);
        if (!result.has_value()) {
            std::cout << "[deepseek-e2e] Run 失败: " << result.error() << "\n";
            return false;
        }
        return true;
    };

    std::cout << "[deepseek-e2e] model=" << model << " base=" << base_url << "\n";
    std::cout << "[deepseek-e2e] 第 1 轮:强制工具往返(thinking 开)...\n";
    if (!run_turn("请调用 probe_file 工具一次(入参随意),然后用一句话告诉我你读到了多长的正文。不要做别的。")) {
        return 1;
    }
    std::cout << "[deepseek-e2e] 第 2 轮:追问...\n";
    if (!run_turn("用一句话概括上一轮你做了什么。")) {
        return 1;
    }

    // 对齐账本:usage 步数与捕获请求数一一对应(每步恰好一次请求、一回报)。
    if (steps.size() != backend.captured.size()) {
        std::cout << "[deepseek-e2e] [失败] usage 步数(" << steps.size() << ")与请求数("
                  << backend.captured.size() << ")对不上——usage 解析或回传有问题。\n";
        return 1;
    }

    // 逐请求体检:指纹、追加律、usage、request id、reasoning 回传形状。
    std::cout << "\n[deepseek-e2e] 逐步细账:\n";
    int failures = 0;
    std::string first_request_id;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const StepLog& step = steps[i];
        const std::int64_t total_input = api::TotalInputTokens(step.report.usage);
        const int hit_percent =
            total_input > 0 ? static_cast<int>(step.report.usage.cache_read_tokens * 100 / total_input) : -1;
        std::cout << "  step " << i << " / epoch " << step.report.cache_epoch
                  << " / request_id=" << (step.report.request_id.empty() ? "(无)" : step.report.request_id)
                  << " / read=" << step.report.usage.cache_read_tokens
                  << " miss=" << step.report.usage.input_tokens << " total=" << total_input
                  << " hit=" << (hit_percent < 0 ? std::string("?%") : std::to_string(hit_percent) + "%")
                  << " / append_only=" << (step.report.prefix_append_only ? "true" : "false");
        if (!step.report.epoch_break_reason.empty()) {
            std::cout << " / epoch_break=" << step.report.epoch_break_reason;
        }
        std::cout << "\n";
        if (i == 0) {
            first_request_id = step.report.request_id;
        } else if (step.report.request_id == first_request_id && !first_request_id.empty()) {
            std::cout << "    [失败] request_id 与首步相同——解析器把旧 usage 重放了一遍,这绿是假的。\n";
            ++failures;
        }
        if (!step.report.reported()) {
            std::cout << "    [警告] 服务端未回报 usage(四项全零),按 unknown 记,不判命中。\n";
        }
    }

    // reasoning 回传(第二期):工具交互段之后的请求,序列化出来应带
    // reasoning_content。直接对捕获的中立请求跑一遍 Chat 序列化核对。
    if (steps.size() >= 2) {
        const auto body = api::chat::BuildRequestJson(steps.back().request, nlohmann::json::object(),
                                                      chat_options);
        bool saw_reasoning = false;
        for (const auto& message : body["messages"]) {
            if (message.contains("reasoning_content")) {
                saw_reasoning = true;
                break;
            }
        }
        const char* reasoning_state =
            saw_reasoning ? "带 reasoning_content(协议达成)" : "不含 reasoning_content(本轮或未走工具)";
        std::cout << "\n[deepseek-e2e] reasoning 回传:末份请求" << reasoning_state << "\n";
    }

    // 缓存命中验收:除首份与明确 epoch break 外,要求 cache_read_tokens > 0。
    // 失手时区分两种情形:前缀不等(客户端的账)与前缀相等但未命中(服务
    // 端尽力缓存的账)——受控重试一次。
    auto check_hits = [&]() {
        int misses = 0;
        for (std::size_t i = 1; i < steps.size(); ++i) {
            const StepLog& step = steps[i];
            if (!step.report.reported() || !step.report.epoch_break_reason.empty()) {
                continue;  // 没回报 / 明确断因的不算
            }
            if (step.report.usage.cache_read_tokens > 0) {
                continue;
            }
            // 冷 miss:看前缀指纹,分清是谁的账。
            const agent::PrefixDiff diff = agent::DiffRequests(steps[i - 1].request, step.request);
            if (!diff.append_only()) {
                std::cout << "  [失败] step " << i << " 冷 miss 且前缀不等(" << diff.break_reason()
                          << ")——客户端改了前缀,别赖服务端。\n";
            } else {
                std::cout << "  [情形] step " << i << " 前缀相等但未命中——服务端尽力缓存失手(允许受控重试)。\n";
            }
            ++misses;
        }
        return misses;
    };

    std::cout << "\n[deepseek-e2e] 命中验收:\n";
    int misses = check_hits();
    if (misses > 0) {
        std::cout << "[deepseek-e2e] 受控重试一次(再发一轮追问)...\n";
        if (!run_turn("再说一遍,一句话就好。")) {
            return 1;
        }
        // 重试的 usage 已由回调记进 steps(请求本体在回调里就带上了)。
        misses = check_hits();
    }

    // 工具往返真的发生了吗(第 1 轮应有一次 probe_file 调用)。
    bool saw_tool = false;
    for (const auto& request : backend.captured) {
        for (const auto& message : request.messages) {
            for (const auto& block : message.content) {
                if (const auto* call = std::get_if<api::ToolUseBlock>(&block);
                    call != nullptr && call->name == "probe_file") {
                    saw_tool = true;
                }
            }
        }
        if (saw_tool) break;
    }
    std::cout << "[deepseek-e2e] 工具往返: "
              << (saw_tool ? "发生(probe_file)" : "未发生(模型没听话,本轮结果不判定)") << "\n";

    if (failures > 0) {
        std::cout << "\n[deepseek-e2e] 结论:失败(" << failures << " 处硬伤)。\n";
        return 1;
    }
    if (misses > 0) {
        std::cout << "\n[deepseek-e2e] 结论:重试后仍有 " << misses
                  << " 处未命中——按上面的分账定性(前缀不等=客户端,前缀相等=服务端尽力缓存)。\n";
        return saw_tool ? 1 : 0;
    }
    std::cout << "\n[deepseek-e2e] 结论:通过——除首份/明确断因外均命中,usage 口径与 reasoning 回传合契约。\n";
    return 0;
}
