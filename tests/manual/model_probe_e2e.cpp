// 任意 provider 的真端点抽验模板(模型怪癖矩阵单,opt-in) +
// 真机矩阵 manifest 驱动模式(模型协议兼容实录矩阵单 P3):
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
// manifest 模式(一条命令跑一组哨兵,生成四栏判定报告):
//
//     MODEL_PROBE_MANIFEST=<path>         哨兵清单(仓库带 tests/manual/
//                                          model_probe_manifest.json)
//     MODEL_PROBE_STUB=1                  离线自检:全部哨兵按 unavailable/
//                                          unverified 记,验证报告链路,不算 PASS
//     MODEL_PROBE_REPORT_DIR=<dir>        报告落盘目录,默认
//                                          build/test-evidence/model-probe/<date>
//
// 四栏判定(工单真机判定表,不许揉成一个"支持"):
//   accepted          请求成功且没有流内 error(单看 HTTP 状态不算)
//   emitted_thinking  收到该 wire 认可的非空思考事件(正文自称思考不算)
//   effective_control on/off 或浅/深多次对照,观察值稳定分开(回显参数不算)
//   accounted         usage 明列思考 token 且守恒(reasoning_tokens=0 不算没思考)
// 结果枚举:pass / fail / ignored / unsupported / unavailable / unverified。
// 没钥匙一律 SKIP(unavailable),不算 PASS——工单明文。
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
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

// ---------------------------------------------------------------------------
// manifest 驱动真机矩阵(P3 骨架):哨兵清单 -> 四栏判定 -> 脱敏报告。
// ---------------------------------------------------------------------------

struct SentinelSpec {
    std::string id;
    std::string provider;
    std::string model;
    std::string category;
    std::string control;  // toggle | effort | always | none
    std::string off;      // toggle: off 档;effort: low 档
    std::string on;       // on / high 档
    std::string note;
    int repeats = 3;
};

struct ProbeSample {
    bool ok = false;             // send_stream 成功且无流内 error
    std::int64_t thinking_chars = 0;
    std::int64_t text_chars = 0;
    std::int64_t reasoning_tokens = 0;
    bool usage_reported = false;
    std::string error;
};

// 单发一次极小探测(不发工具,一句话要 "ok"):四栏的观察值全从这里来。
ProbeSample ProbeOnce(api::Backend& backend, const api::Request& request) {
    ProbeSample sample;
    auto result = backend.send_stream(request, [&](const api::StreamEvent& event) {
        if (const auto* thinking = std::get_if<api::ThinkingDelta>(&event)) {
            sample.thinking_chars += static_cast<std::int64_t>(thinking->text.size());
        } else if (const auto* text = std::get_if<api::TextDelta>(&event)) {
            sample.text_chars += static_cast<std::int64_t>(text->text.size());
        } else if (const auto* done = std::get_if<api::MessageDone>(&event)) {
            sample.usage_reported = done->usage.input_tokens > 0 || done->usage.output_tokens > 0;
            sample.reasoning_tokens = done->usage.output_reasoning_tokens;
        } else if (const auto* error = std::get_if<api::StreamError>(&event)) {
            sample.error = error->message;
        }
    });
    sample.ok = result.has_value() && sample.error.empty();
    if (!result.has_value()) sample.error = result.error().message;
    return sample;
}

struct SentinelOutcome {
    std::string id;
    std::string verdict;  // pass / fail / ignored / unsupported / unavailable / unverified
    bool accepted = false;
    bool emitted_thinking = false;
    bool effective_control = false;
    bool accounted = false;
    int runs = 0;
    std::vector<std::string> notes;
};

// 四栏里的 effective_control:两档观察值多次对照,分组稳定且分开。
bool ObservationsSeparate(const std::vector<ProbeSample>& low_group,
                          const std::vector<ProbeSample>& high_group) {
    if (low_group.empty() || high_group.empty()) return false;
    const auto signature = [](const ProbeSample& s) {
        return s.thinking_chars > 0 || s.reasoning_tokens > 0;
    };
    bool low_stable = true;
    bool high_stable = true;
    for (const auto& sample : low_group) low_stable = low_stable && (signature(sample) == signature(low_group[0]));
    for (const auto& sample : high_group) {
        high_stable = high_stable && (signature(sample) == signature(high_group[0]));
    }
    return low_stable && high_stable && signature(low_group[0]) != signature(high_group[0]);
}

std::vector<SentinelSpec> LoadSentinels(const std::string& manifest_path, std::string* error) {
    std::vector<SentinelSpec> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fs::path(manifest_path), ec) || ec) {
        *error = "manifest 不存在: " + manifest_path;
        return out;
    }
    std::ifstream file(fs::path(manifest_path), std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(buffer.str());
    } catch (const nlohmann::json::exception& e) {
        *error = std::string("manifest 不是合法 JSON: ") + e.what();
        return out;
    }
    const int repeats = root.value("defaults", nlohmann::json::object()).value("repeats", 3);
    for (const auto& item : root.value("sentinels", nlohmann::json::array())) {
        SentinelSpec spec;
        spec.id = item.value("id", "");
        spec.provider = item.value("provider", "");
        spec.model = item.value("model", "");
        spec.category = item.value("category", "");
        spec.control = item.value("control", "none");
        spec.off = item.value("off", item.value("low", ""));
        spec.on = item.value("on", item.value("high", ""));
        spec.note = item.value("note", "");
        spec.repeats = item.value("repeats", repeats);
        if (spec.id.empty() || spec.provider.empty()) {
            *error = "manifest 哨兵缺 id/provider: " + item.dump();
            return {};
        }
        out.push_back(std::move(spec));
    }
    return out;
}

std::string TodayStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday);
    return stamp;
}

int RunManifestMatrix(const std::string& manifest_path) {
    const bool stub = EnvOr("MODEL_PROBE_STUB") == "1";
    std::string load_error;
    const std::vector<SentinelSpec> sentinels = LoadSentinels(manifest_path, &load_error);
    if (!load_error.empty()) {
        std::cout << "[matrix] [失败] " << load_error << "\n";
        return 1;
    }
    auto catalog = config::ParseProviderCatalogJson(config::embedded::kProviderCatalogJson,
                                                    "<embedded>");
    if (!catalog.has_value()) {
        std::cout << "[matrix] [失败] 内嵌 catalog 解析不动:" << catalog.error() << "\n";
        return 1;
    }

    std::vector<SentinelOutcome> outcomes;
    for (const auto& spec : sentinels) {
        SentinelOutcome outcome;
        outcome.id = spec.id;
        std::cout << "[matrix] 哨兵 " << spec.id << " (" << spec.provider << "/"
                  << spec.model << ", control=" << spec.control << ")\n";

        // vLLM 部署参数一类没有 HTTP 端点的哨兵:unsupported 占位记案。
        if (spec.control == "none") {
            outcome.verdict = "unsupported";
            outcome.notes.push_back(spec.note.empty() ? "control=none,占位记案" : spec.note);
            outcomes.push_back(std::move(outcome));
            continue;
        }
        const config::ProviderPreset* preset = catalog->FindProvider(spec.provider);
        if (preset == nullptr) {
            outcome.verdict = "unavailable";
            outcome.notes.push_back("catalog 里没有 provider \"" + spec.provider +
                                    "\",SKIP 不算 PASS");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        // 钥匙三态:stub 自检 / 真钥匙 / 没钥匙(没钥匙不算 PASS,工单明文)。
        std::string key = stub ? std::string("stub-key") : EnvOr("MODEL_PROBE_KEY");
        if (key.empty() && !stub) key = EnvOr(preset->key_env.c_str());
        if (key.empty()) {
            outcome.verdict = "unavailable";
            outcome.notes.push_back("钥匙缺席(" + preset->key_env +
                                    " 与 MODEL_PROBE_KEY 都没设),SKIP 不算 PASS");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        const config::ProviderCatalogModel* model_entry = preset->FindModel(spec.model);
        if (model_entry == nullptr) {
            outcome.verdict = "unavailable";
            outcome.notes.push_back("provider 里有目录但模型 \"" + spec.model +
                                    "\" 不在(退役/改名),SKIP");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        // 档位集:toggle=off/on,effort=low/high,always=单档。
        std::vector<std::pair<std::string, bool>> levels;  // (档位, 是否"开"侧)
        if (spec.control == "toggle") {
            levels = {{spec.off, false}, {spec.on, true}};
        } else if (spec.control == "effort") {
            levels = {{spec.off, false}, {spec.on, true}};
        } else {
            levels = {{spec.on, true}};
        }

        std::map<std::string, std::vector<ProbeSample>> groups;
        std::unique_ptr<api::Backend> backend =
            stub ? std::make_unique<StubBackend>() : BuildForPreset(*preset, preset->base_url, key);
        for (const auto& [level, is_on] : levels) {
            for (int i = 0; i < spec.repeats; ++i) {
                api::Request request;
                request.model = spec.model;
                request.reasoning = model_entry->reasoning;
                request.reasoning_effort = level;
                request.max_tokens = 512;
                request.system = "你是真机矩阵哨兵探测。回答保持极短。";
                api::Message user;
                user.role = api::Role::User;
                user.content.push_back(api::TextBlock{"Reply with exactly: ok"});
                request.messages.push_back(std::move(user));
                groups[level].push_back(ProbeOnce(*backend, request));
                ++outcome.runs;
            }
        }

        if (stub) {
            // stub 自检:模板链路走通即可,四栏一律不算数。
            outcome.verdict = "unverified";
            outcome.notes.push_back("stub 自检:harness 与报告链路走通,四栏判定以真机为准");
            outcomes.push_back(std::move(outcome));
            continue;
        }

        // ---- 四栏(真机) ----
        const auto all_ok = [&] {
            for (const auto& [level, group] : groups) {
                for (const auto& sample : group) {
                    if (!sample.ok) return false;
                }
            }
            return true;
        };
        outcome.accepted = all_ok();
        if (!outcome.accepted) {
            for (const auto& [level, group] : groups) {
                for (const auto& sample : group) {
                    if (!sample.ok) {
                        outcome.notes.push_back("档位 " + level + " 有失败请求: " + sample.error);
                    }
                }
            }
        }
        const std::vector<ProbeSample>& on_group = groups[spec.on];
        for (const auto& sample : on_group) {
            if (sample.thinking_chars > 0) outcome.emitted_thinking = true;
        }
        outcome.accounted = false;
        for (const auto& sample : on_group) {
            if (sample.usage_reported && sample.reasoning_tokens > 0) outcome.accounted = true;
        }
        if (spec.control == "always") {
            outcome.notes.push_back("只思考哨兵无对照组,effective_control 栏不判(unverified)");
        } else {
            outcome.effective_control = ObservationsSeparate(groups[spec.off], on_group);
        }

        if (!outcome.accepted) {
            outcome.verdict = "fail";
        } else if (spec.control == "always") {
            outcome.verdict = (outcome.emitted_thinking && outcome.accounted) ? "pass" : "unverified";
        } else {
            outcome.verdict = (outcome.accepted && outcome.emitted_thinking &&
                               outcome.effective_control && outcome.accounted)
                                  ? "pass"
                                  : "unverified";
        }
        outcomes.push_back(std::move(outcome));
    }

    // ---- 报告:文本 + JSON,脱敏(无钥匙无正文),落 build/test-evidence/。----
    std::string report_dir = EnvOr("MODEL_PROBE_REPORT_DIR");
    if (report_dir.empty()) {
        report_dir = "build/test-evidence/model-probe/" + TodayStamp();
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(report_dir), ec);

    nlohmann::json report_json;
    report_json["generated_at"] = TodayStamp();
    report_json["manifest"] = manifest_path;
    report_json["mode"] = stub ? "stub-self-check" : "live";
    report_json["note"] = "没钥匙/没跑/只收 2xx 一概不算 PASS;四栏分开,不许揉成一个支持";
    nlohmann::json sentinel_json = nlohmann::json::array();
    std::ostringstream table;
    table << "id                               verdict       accepted emitted effective accounted runs\n";
    int pass_count = 0;
    int fail_count = 0;
    for (const auto& outcome : outcomes) {
        table << outcome.id
              << std::string(32 > outcome.id.size() ? 32 - outcome.id.size() : 0, ' ')
              << outcome.verdict
              << std::string(14 > outcome.verdict.size() ? 14 - outcome.verdict.size() : 0, ' ')
              << (outcome.accepted ? "yes" : "-") << "      "
              << (outcome.emitted_thinking ? "yes" : "-") << "      "
              << (outcome.effective_control ? "yes" : "-") << "        "
              << (outcome.accounted ? "yes" : "-") << "       " << outcome.runs << "\n";
        if (outcome.verdict == "pass") ++pass_count;
        if (outcome.verdict == "fail") ++fail_count;
        nlohmann::json entry;
        entry["id"] = outcome.id;
        entry["verdict"] = outcome.verdict;
        entry["accepted"] = outcome.accepted;
        entry["emitted_thinking"] = outcome.emitted_thinking;
        entry["effective_control"] = outcome.effective_control;
        entry["accounted"] = outcome.accounted;
        entry["runs"] = outcome.runs;
        entry["notes"] = outcome.notes;
        sentinel_json.push_back(std::move(entry));
    }
    report_json["sentinels"] = std::move(sentinel_json);
    report_json["summary"] = {{"pass", pass_count}, {"fail", fail_count},
                              {"total", outcomes.size()}};

    const std::filesystem::path dir(report_dir);
    {
        std::ofstream out(dir / "report.txt", std::ios::binary);
        out << table.str();
    }
    {
        std::ofstream out(dir / "report.json", std::ios::binary);
        out << report_json.dump(2) << "\n";
    }
    std::cout << "\n[matrix] 四栏判定表:\n" << table.str();
    std::cout << "[matrix] pass=" << pass_count << " fail=" << fail_count
              << " total=" << outcomes.size() << "\n";
    std::cout << "[matrix] 报告落盘: " << report_dir << "/report.{txt,json}(不进源码仓)\n";
    // 没钥匙/ stub 的 SKIP 不算失败;真机 fail 才红。
    return (fail_count > 0 && !stub) ? 1 : 0;
}

}  // namespace

int main() {
    // manifest 驱动矩阵模式优先:一条命令跑一组哨兵,四栏判定报告。
    const std::string manifest_path = EnvOr("MODEL_PROBE_MANIFEST");
    if (!manifest_path.empty()) {
        return RunManifestMatrix(manifest_path);
    }

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
