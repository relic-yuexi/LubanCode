// 任意 provider 的真端点抽验模板(模型怪癖矩阵单,opt-in):
//
//   MODEL_PROBE_PROVIDER 没设就直接跳过(exit 0),不进离线 ctest。要在真机
//   上抽验任何怪模型,指环境变量即可跑一轮"发-流-工具-收":
//
//     MODEL_PROBE_PROVIDER=zai            catalog 里的 provider id(必填)
//     MODEL_PROBE_MODEL=glm-5.2           可选,默认该 provider 的 default_model
//     MODEL_PROBE_KEY=sk-...              可选,默认读该 provider 的 key_env
//     MODEL_PROBE_BASE_URL=https://...    可选,默认目录里的 base_url
//     MODEL_PROBE_EFFORT=high             可选,默认模型 default_think/目录档
//     MODEL_PROBE_STUB=1                  离线 stub 自测(不走网络,验证模板自身)
//
// 例(Windows): lubancode_test_probe.exe 之前先
//   set MODEL_PROBE_PROVIDER=zai
//   set ZAI_API_KEY=sk-...
//   set MODEL_PROBE_MODEL=glm-5.2
//   set MODEL_PROBE_EFFORT=maximal   ← 顺手试怪档位,看请求体怎么翻
//
// 验什么(与 deepseek_e2e 同一付骨架,provider 换成任指):
//   1. 发:目录 preset(provider caps:stream_usage/reasoning_replay/think_param
//      /extra_body/extra_headers)按 wire 装配后端,发真请求;
//   2. 流:MessageStart/Thinking/Text 事件与逐请求 usage(各家摊账口径);
//   3. 工具:一轮 probe_file 强制往返;
//   4. 收:终止原因、终份请求按该 wire 序列化出来的 reasoning 形状
//      (打印给人看:哪家翻出什么形状一眼定案)。
//
// 防假绿:stub 模式(MODEL_PROBE_STUB=1)用进程内假后端把同一付 harness
// 跑一遍——模板自身的账(事件序/工具往返/usage 落账)离线可验,没钥匙
// 也能自测模板没坏。

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/chat/request.hpp"
#include "api/gemini/client.hpp"
#include "api/gemini/request.hpp"
#include "api/responses/client.hpp"
#include "api/responses/request.hpp"
#include "api/types.hpp"
#include "config/provider_catalog.hpp"
#include "embedded_provider_catalog.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

std::string EnvOr(const char* name, const std::string& fallback = "") {
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

// 捕获层:记下每份 api::Request(末尾按 wire 序列化给人看)。
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

// 可控探针工具:固定长度正文,工具往返的"结果"不抖。
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

// stub 模式的进程内假后端:第一次调用走"思考+工具",后续走"思考+正文",
// 第二次起 usage 里带缓存命中。不发一个网络包。
class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        on_event(api::MessageStart{"stub-1", request.model});
        on_event(api::ThinkingDelta{"想一想", ""});
        if (calls_++ == 0) {
            on_event(api::ToolUseStart{0, "stub_tool_0", "probe_file"});
            on_event(api::ToolUseInputDelta{0, R"({"ignore":"x"})"});
            on_event(api::ContentBlockDone{0});
            on_event(api::MessageDone{"tool_use", {}});
            return {};
        }
        on_event(api::TextDelta{"探针正文收讫。"});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"end_turn",
                                  api::Usage{1000, 60, calls_ > 2 ? 49000 : 0, 0, 20}});
        return {};
    }

private:
    int calls_ = 0;
};

// 按 wire 装配后端(镜像 src/app/backend_stack.cpp 的拼法;模板自持一份,
// 不依赖 app 装配层,链 lubancode_core 即可)。
std::unique_ptr<api::Backend> BuildForPreset(const config::ProviderPreset& preset,
                                             const std::string& base_url,
                                             const std::string& key) {
    constexpr int kConnectTimeoutMs = 20000;
    constexpr int kStreamIdleTimeoutSecs = 180;
    constexpr int kHardTimeoutSecs = 300;
    const auto headers = config::ResolveProviderHeaderTemplates(preset.extra_headers, key);
    switch (preset.wire) {
    case config::Wire::Responses:
        return std::make_unique<api::responses::ResponsesBackend>(
            base_url, key, kConnectTimeoutMs, kStreamIdleTimeoutSecs, preset.native_web_search,
            preset.extra_body, headers, kHardTimeoutSecs);
    case config::Wire::GoogleGenerateContent:
        return std::make_unique<api::gemini::GeminiBackend>(
            base_url, key, kConnectTimeoutMs, kStreamIdleTimeoutSecs, preset.extra_body, headers,
            kHardTimeoutSecs);
    case config::Wire::ChatCompletions: {
        api::chat::ChatRequestOptions options;
        options.stream_usage = preset.stream_usage;
        // 档位参数名默认 reasoning_effort;目录 preset 不改这名,用户真要换名
        // 走本地配置 think_param(见 backend_stack.cpp 同一处)。
        options.reasoning_delta_field = preset.reasoning_delta_field;
        options.reasoning_replay_field = preset.reasoning_replay_field;
        options.reasoning_replay = preset.reasoning_replay == "tool_episode"
                                       ? api::chat::ReasoningReplayPolicy::ToolEpisode
                                       : api::chat::ReasoningReplayPolicy::Never;
        return std::make_unique<api::chat::ChatCompletionsBackend>(
            base_url, key, kConnectTimeoutMs, kStreamIdleTimeoutSecs, preset.extra_body, headers,
            std::move(options), kHardTimeoutSecs);
    }
    default:
        return std::make_unique<api::anthropic::AnthropicBackend>(
            base_url, key, kConnectTimeoutMs, kStreamIdleTimeoutSecs, preset.native_web_search,
            preset.extra_body, headers, kHardTimeoutSecs);
    }
}

// 终份请求按该 wire 序列化,把 reasoning 相关形状打给人看。
void DumpReasoningShape(const config::ProviderPreset& preset, const api::Request& request) {
    nlohmann::json body;
    switch (preset.wire) {
    case config::Wire::ChatCompletions:
        body = api::chat::BuildRequestJson(request, preset.extra_body);
        break;
    case config::Wire::Responses:
        body = api::responses::BuildRequestJson(request, preset.native_web_search, preset.extra_body);
        break;
    case config::Wire::GoogleGenerateContent:
        body = api::gemini::BuildRequestJson(request, preset.extra_body);
        break;
    default:
        body = api::anthropic::BuildRequestJson(request, preset.native_web_search, preset.extra_body);
        break;
    }
    std::cout << "[model-probe] 终份请求 reasoning 形状:\n";
    for (const char* key : {"reasoning_effort", "reasoning", "thinking", "output_config",
                            "thinking_budget", "enable_thinking"}) {
        if (body.contains(key)) {
            std::cout << "  " << key << " = " << body.at(key).dump() << "\n";
        }
    }
    if (const auto config_it = body.find("generationConfig");
        config_it != body.end() && config_it->contains("thinkingConfig")) {
        std::cout << "  generationConfig.thinkingConfig = "
                  << config_it->at("thinkingConfig").dump() << "\n";
    }
    bool saw_replay = false;
    if (body.contains("messages")) {
        for (const auto& message : body.at("messages")) {
            if (message.contains("reasoning_content") || message.contains("reasoning")) {
                saw_replay = true;
            }
        }
    }
    std::cout << "  reasoning 回传: " << (saw_replay ? "带(工具段思考回传可见)" : "无") << "\n";
}

}  // namespace

int main() {
    const bool stub = EnvOr("MODEL_PROBE_STUB") == "1";
    const std::string provider_id = EnvOr("MODEL_PROBE_PROVIDER");
    if (provider_id.empty()) {
        std::cout << "[model-probe] MODEL_PROBE_PROVIDER 未设置,跳过(不进离线 ctest)。\n"
                  << "[model-probe] 用法:MODEL_PROBE_PROVIDER=<catalog id> [MODEL_PROBE_MODEL=...] "
                     "[MODEL_PROBE_KEY=...|<key_env> 环境变量] [MODEL_PROBE_EFFORT=...] "
                     "[MODEL_PROBE_BASE_URL=...] [MODEL_PROBE_STUB=1]\n";
        return 0;
    }

    const auto catalog = config::ParseProviderCatalogJson(config::embedded::kProviderCatalogJson,
                                                          "<embedded>");
    if (!catalog.has_value()) {
        std::cout << "[model-probe] [失败] 内嵌 catalog 解析不动:" << catalog.error() << "\n";
        return 1;
    }
    const config::ProviderPreset* preset = catalog->FindProvider(provider_id);
    if (preset == nullptr) {
        std::cout << "[model-probe] [失败] catalog 里没有 provider id \"" << provider_id
                  << "\"(66 家里挑一个,见 catalog/providers.json)\n";
        return 1;
    }

    const std::string model = EnvOr("MODEL_PROBE_MODEL", preset->default_model);
    const std::string base_url = EnvOr("MODEL_PROBE_BASE_URL", preset->base_url);
    const config::ProviderCatalogModel* model_entry = preset->FindModel(model);

    std::string effort = EnvOr("MODEL_PROBE_EFFORT");
    if (effort.empty() && model_entry != nullptr) {
        effort = model_entry->default_think;
    }
    if (effort.empty()) {
        effort = preset->model_reasoning_effort;
    }
    const std::string key = stub ? std::string("stub-key") : EnvOr("MODEL_PROBE_KEY");
    const std::string effective_key =
        !key.empty() ? key : (stub ? key : EnvOr(preset->key_env.c_str()));
    if (!stub && effective_key.empty()) {
        std::cout << "[model-probe] 钥匙缺席:" << preset->key_env
                  << " 与 MODEL_PROBE_KEY 都没设,跳过(不进离线 ctest)。\n";
        return 0;
    }

    std::cout << "[model-probe] provider=" << provider_id << " wire=" << static_cast<int>(preset->wire)
              << " model=" << model << " effort=\"" << effort << "\" base=" << base_url
              << (stub ? " (STUB 离线自测)" : "") << "\n";

    CapturingBackend backend(
        stub ? std::make_unique<StubBackend>() : BuildForPreset(*preset, base_url, effective_key));

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ProbeTool>());

    agent::AgentProfile profile;
    profile.provider = provider_id;
    profile.request.model = model;
    profile.request.reasoning_effort = effort;
    if (model_entry != nullptr) {
        profile.request.reasoning = model_entry->reasoning;
    }
    profile.runtime.max_output_tokens = 512;
    profile.runtime.max_steps_per_turn = 0;
    profile.system_prompt =
        "你是抽验会话里的被测模型。回答保持极短,让出工具调用的空间。";
    agent::Agent loop(backend, registry, std::move(profile));

    struct StepLog {
        std::int64_t input = 0;
        std::int64_t output = 0;
        std::int64_t cache_read = 0;
        std::int64_t reasoning = 0;
        std::string request_id;
    };
    std::vector<StepLog> steps;
    runtime::IdAuthority event_ids;
    runtime::TurnEventAdapter events("model-probe", event_ids);
    events.Attach([&steps](const runtime::ServerEvent& event) {
        if (event.kind != runtime::ServerEventKind::UsageUpdated) {
            return;
        }
        StepLog log;
        log.input = event.payload.value("input_tokens", std::int64_t{0});
        log.output = event.payload.value("output_tokens", std::int64_t{0});
        log.cache_read = event.payload.value("cache_read_tokens", std::int64_t{0});
        log.reasoning = event.payload.value("reasoning_tokens", std::int64_t{0});
        log.request_id = event.payload.value("request_id", std::string());
        steps.push_back(std::move(log));
    });
    events.Start();
    agent::TurnWiring callbacks;
    callbacks.events = &events;

    const auto run_turn = [&](const std::string& user_text) -> bool {
        const auto result = loop.Run(user_text, callbacks);
        if (!result.has_value()) {
            std::cout << "[model-probe] [失败] Run 失败: " << result.error() << "\n";
            return false;
        }
        return true;
    };

    std::cout << "[model-probe] 第 1 轮:强制工具往返...\n";
    if (!run_turn("请调用 probe_file 工具一次(入参随意),然后用一句话告诉我你读到了多长的正文。不要做别的。")) {
        return 1;
    }
    std::cout << "[model-probe] 第 2 轮:追问...\n";
    if (!run_turn("用一句话概括上一轮你做了什么。")) {
        return 1;
    }

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

    std::cout << "\n[model-probe] 逐步细账(usage 摊账口径:input/cache_read/reasoning):\n";
    int failures = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const StepLog& step = steps[i];
        std::cout << "  step " << i << " / request_id=" << (step.request_id.empty() ? "(无)" : step.request_id)
                  << " / input=" << step.input << " cache_read=" << step.cache_read
                  << " output=" << step.output << " reasoning=" << step.reasoning << "\n";
        if (step.input == 0 && step.output == 0 && step.cache_read == 0) {
            std::cout << "    [警告] 服务端未回报 usage(全零),按 unknown 记。\n";
        }
    }
    if (steps.empty()) {
        std::cout << "  [失败] 一步 usage 都没落账——事件流或解析有问题。\n";
        ++failures;
    }
    if (!saw_tool) {
        std::cout << "[model-probe] 工具往返: 未发生(模型没听话,"
                  << (stub ? "stub 后端坏了" : "本轮结果不判定") << ")\n";
        if (stub) {
            ++failures;
        }
    } else {
        std::cout << "[model-probe] 工具往返: 发生(probe_file)\n";
    }

    if (!backend.captured.empty()) {
        DumpReasoningShape(*preset, backend.captured.back());
    }

    if (failures > 0) {
        std::cout << "\n[model-probe] 结论:失败(" << failures << " 处硬伤)。\n";
        return 1;
    }
    std::cout << "\n[model-probe] 结论:通过——发-流-工具-收四步走通,usage 与 reasoning 形状见上"
              << (stub ? "(stub 数据,真端点结论以带钥匙的一跑为准)" : "") << "。\n";
    return 0;
}
