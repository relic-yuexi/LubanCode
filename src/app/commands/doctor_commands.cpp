// /doctor 的执行体:effort/cache 两个子命令的探针、指标读取与报告。纯函数
// (请求构造、指标解析、四态分类、前缀对账)全导出在 doctor_commands.hpp,
// 单测直接钉;这一头只留 IO(发请求、读 metrics、写回配置)与打印。

#include "app/commands/doctor_commands.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include <cpr/cpr.h>

#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/chat/request.hpp"
#include "api/responses/client.hpp"
#include "api/responses/request.hpp"
#include "app/backend_stack.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

// ---------------- 探针发送(IO) ----------------

// 一次探针请求的收账。ok=false 时 http_status/error 说清为什么;usage 只有
// provider 真回了才置 reported——诊断的第一戒律:没报告不是零。
struct ProbeOutcome {
    bool ok = false;
    int http_status = 0;
    std::string error;
    std::string stop_reason;
    api::Usage usage;
    bool usage_reported = false;
    std::int64_t text_chars = 0;
    std::int64_t thinking_chars = 0;

    bool usage_any() const {
        return usage.input_tokens > 0 || usage.output_tokens > 0 || usage.cache_read_tokens > 0 ||
               usage.cache_creation_tokens > 0 || usage.output_reasoning_tokens > 0;
    }
};

ProbeOutcome RunProbe(api::Backend& backend, const api::Request& request) {
    ProbeOutcome out;
    const auto send = backend.send_stream(
        request,
        [&out](const api::StreamEvent& event) {
            std::visit(
                [&out](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, api::TextDelta>) {
                        out.text_chars += static_cast<std::int64_t>(e.text.size());
                    } else if constexpr (std::is_same_v<T, api::ThinkingDelta>) {
                        out.thinking_chars += static_cast<std::int64_t>(e.text.size());
                    } else if constexpr (std::is_same_v<T, api::MessageDone>) {
                        out.stop_reason = e.stop_reason;
                        out.usage = e.usage;
                    } else if constexpr (std::is_same_v<T, api::StreamError>) {
                        out.error = e.message;
                    }
                },
                event);
        },
        /*cancel=*/nullptr);
    if (!send.has_value()) {
        out.http_status = send.error().http_status;
        out.error = send.error().message;
        return out;
    }
    out.usage_reported = out.usage_any();
    out.ok = out.error.empty();
    return out;
}

// GET 一份 metrics 文本。失败给可读错误,不抛。
std::expected<std::string, std::string> FetchMetricsText(const std::string& metrics_url,
                                                          const lubancode::config::Config& config) {
    cpr::Response response = cpr::Get(
        cpr::Url{metrics_url},
        cpr::ConnectTimeout{std::chrono::milliseconds(config.connect_timeout_ms)},
        cpr::Timeout{std::chrono::seconds(config.request_timeout_secs)});
    if (response.error) {
        return std::unexpected("读取 metrics 失败: " + response.error.message);
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected("metrics 端点返回 HTTP " + std::to_string(response.status_code));
    }
    return response.text;
}

// 一行 metrics 的数值尾巴("prefix_cache_queries_total 12.0" 的 12.0)。
std::optional<std::int64_t> MetricValueFromLine(const std::string& line, std::size_t name_end) {
    const std::size_t last = line.find_last_not_of(" \t");
    if (last == std::string::npos || last < name_end) {
        return std::nullopt;
    }
    std::size_t value_begin = line.find_last_of(" \t", last);
    value_begin = value_begin == std::string::npos ? 0 : value_begin + 1;
    if (value_begin < name_end) {
        return std::nullopt;
    }
    const std::string value_text = line.substr(value_begin, last - value_begin + 1);
    try {
        const double value = std::stod(value_text);
        return static_cast<std::int64_t>(std::llround(value));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

// ---------------- 纯函数 ----------------

api::Request BuildEffortProbeRequest(const std::string& model, const std::string& level) {
    api::Request probe;
    probe.model = model;
    probe.system = "You are a diagnostic probe. Answer with exactly: ok";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"Reply with exactly: ok"});
    probe.messages.push_back(std::move(user));
    probe.max_tokens = 64;
    probe.reasoning_effort = level;  // 空串 = 不发参数,字段整个缺席
    return probe;
}

std::string DescribeRequestEffort(lubancode::config::Wire wire, const api::Request& request,
                                  const nlohmann::json& extra_body, const std::string& think_param) {
    if (wire == lubancode::config::Wire::ChatCompletions) {
        lubancode::api::chat::ChatRequestOptions options;
        options.reasoning_param = think_param;
        const nlohmann::json body = lubancode::api::chat::BuildRequestJson(request, extra_body, options);
        const std::string param = think_param.empty() ? std::string("reasoning_effort") : think_param;
        if (auto it = body.find(param); it != body.end()) {
            // 值被 extra_body 压成什么就报什么(字符串/对象都如实 dump)。
            return param + " = " + it->dump();
        }
        return trf("doctor.effort.field_absent", param);
    }
    if (wire == lubancode::config::Wire::Responses) {
        const nlohmann::json body = lubancode::api::responses::BuildRequestJson(request);
        if (auto it = body.find("reasoning"); it != body.end() && it->is_object() && it->contains("effort")) {
            return "reasoning.effort = " + (*it)["effort"].dump();
        }
        return trf("doctor.effort.field_absent", std::string("reasoning.effort"));
    }
    const nlohmann::json body = lubancode::api::anthropic::BuildRequestJson(request, false, extra_body);
    if (auto it = body.find("thinking"); it != body.end() && it->is_object()) {
        const std::string type = it->value("type", std::string());
        if (type == "disabled") {
            return "thinking.type = disabled(none 档)";
        }
        if (it->contains("budget_tokens")) {
            return "thinking.budget_tokens = " + (*it)["budget_tokens"].dump() + "(" +
                   request.reasoning_effort + tr("doctor.effort.mapped_suffix");
        }
        return "thinking = " + it->dump();
    }
    return trf("doctor.effort.field_absent", std::string("thinking"));
}

PrefixCacheMetrics ParsePrefixCacheMetrics(const std::string& text) {
    PrefixCacheMetrics out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        std::size_t name_end = line.find_first_of("{ \t", first);
        if (name_end == std::string::npos) {
            name_end = line.size();
        }
        std::string name = line.substr(first, name_end - first);
        if (name.rfind("vllm:", 0) == 0) {
            name = name.substr(5);  // vLLM 的指标名带 vllm: 前缀,剥掉再认
        }
        // enable_prefix_caching 是个 label,挂在哪行 gauge 上都算数。
        if (!out.enabled.has_value()) {
            const std::size_t flag = line.find("enable_prefix_caching=\"");
            if (flag != std::string::npos) {
                const std::size_t value_pos = flag + std::string_view{"enable_prefix_caching=\""}.size();
                const std::size_t quote = line.find('"', value_pos);
                if (quote != std::string::npos) {
                    const std::string value = line.substr(value_pos, quote - value_pos);
                    if (value == "True") {
                        out.enabled = true;
                    } else if (value == "False") {
                        out.enabled = false;
                    }
                }
            }
        }
        const auto value = MetricValueFromLine(line, name_end);
        if (!value.has_value()) {
            continue;
        }
        if (name == "prefix_cache_queries_total") {
            out.queries_total = *value;
        } else if (name == "prefix_cache_hits_total") {
            out.hits_total = *value;
        } else if (name == "prompt_tokens_cached_total") {
            out.prompt_tokens_cached_total = *value;
        }
    }
    return out;
}

CacheObservation ClassifyCacheObservation(bool usage_reported, std::int64_t cache_read,
                                          std::optional<bool> server_enabled) {
    if (!usage_reported) {
        return CacheObservation::NotReported;
    }
    if (cache_read > 0) {
        return CacheObservation::Hit;
    }
    if (server_enabled.has_value() && !*server_enabled) {
        return CacheObservation::Disabled;
    }
    return CacheObservation::EnabledNoHit;
}

FixedPrefixProbePair BuildFixedPrefixProbePair(const std::string& model, std::size_t prefix_fill_bytes) {
    // 填充段是一段无信息量的固定文字:诊断信号在前缀字节稳不稳,不在内容。
    const std::string filler = "prefix-cache probe filler segment 0123456789. ";
    std::string fixed;
    fixed.reserve(prefix_fill_bytes + filler.size());
    while (fixed.size() < prefix_fill_bytes) {
        fixed += filler;
    }
    const std::string system = "You are a prefix-cache diagnostic probe. Always answer with exactly: ok";

    FixedPrefixProbePair out;
    for (int round = 1; round <= 2; ++round) {
        api::Request& request = round == 1 ? out.round1 : out.round2;
        request.model = model;
        request.system = system;
        request.max_tokens = 32;
        api::Message prefix;
        prefix.role = api::Role::User;
        prefix.content.push_back(api::TextBlock{fixed});
        api::Message tail;
        tail.role = api::Role::User;
        tail.content.push_back(api::TextBlock{round == 1 ? "First round. Reply with exactly: ok"
                                                          : "Second round. Reply with exactly: ok"});
        request.messages.push_back(std::move(prefix));
        request.messages.push_back(std::move(tail));
    }
    out.designed_prefix_bytes = system.size() + fixed.size();
    return out;
}

std::size_t CommonPrefixBytes(const std::string& a, const std::string& b) {
    const std::size_t n = (std::min)(a.size(), b.size());
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) {
        ++i;
    }
    return i;
}

bool IsLoopbackUrl(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t host_begin = scheme == std::string::npos ? 0 : scheme + 3;
    std::size_t host_end;
    if (host_begin < url.size() && url[host_begin] == '[') {
        // IPv6 字面量:[::1] 里的冒号不是端口分隔符,host 取到 ']' 为止。
        host_end = url.find(']', host_begin);
        if (host_end == std::string::npos) {
            host_end = url.size();
        } else {
            ++host_end;
        }
    } else {
        host_end = url.find_first_of("/:?#", host_begin);
        if (host_end == std::string::npos) {
            host_end = url.size();
        }
    }
    std::string host = url.substr(host_begin, host_end - host_begin);
    for (char& c : host) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (host == "localhost" || host == "::1" || host == "[::1]") {
        return true;
    }
    return host.rfind("127.", 0) == 0;
}

std::string SanitizeProbeError(const std::string& message, std::size_t max_chars) {
    std::string out;
    out.reserve((std::min)(message.size(), max_chars) + 16);
    for (const char c : message) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\n' || u == '\t') {
            out += ' ';
        } else if (u == '\r') {
            continue;  // 回车是换行符的尾巴,单独吞掉,免得 \r\n 变两个空格
        } else if (u < 0x20) {
            continue;  // 其余控制字符剥掉,不进终端
        } else {
            out += c;
        }
        if (out.size() >= max_chars) {
            out += tr("doctor.error.truncated");
            break;
        }
    }
    return out;
}

// ---------------- 执行 ----------------

namespace {

// /doctor effort:<level> 已经剥好(空串 = 探"不发参数")。
void RunEffortProbe(const std::string& level, const DoctorContext& context) {
    const lubancode::config::Config& config = context.config;
    const api::Request probe = BuildEffortProbeRequest(context.current_model, level);

    std::cout << trf("doctor.effort.probe_header", context.current_model,
                     level.empty() ? tr("doctor.level.unset") : level)
              << "\n";
    // 请求侧:先看"实际发送值"——按当前 wire 把请求体翻出来,字段在不在、
    // 值是什么,如实报告(不发送任何正文,只报参数名与档位值)。
    std::cout << tr("doctor.effort.request_field") << " "
              << DescribeRequestEffort(config.wire, probe, config.extra_body, config.think_param) << "\n";
    std::cout << tr("doctor.probe.sending") << "\n";
    std::cout.flush();

    auto backend = BuildBackend(config);
    const ProbeOutcome outcome = RunProbe(*backend, probe);

    if (!outcome.error.empty()) {
        std::cout << trf("doctor.effort.http_error",
                         outcome.http_status > 0 ? std::to_string(outcome.http_status) : std::string("-"))
                  << "\n"
                  << "  " << SanitizeProbeError(outcome.error) << "\n";
    } else {
        std::cout << tr("doctor.effort.http_ok") << "\n";
    }
    std::cout << tr("doctor.effort.finish") << " "
              << (outcome.stop_reason.empty() ? tr("doctor.value.absent") : outcome.stop_reason) << "\n";
    std::cout << trf("doctor.effort.body", outcome.text_chars, outcome.thinking_chars) << "\n";
    if (!outcome.usage_reported) {
        std::cout << tr("doctor.effort.usage_not_reported") << "\n";
    } else {
        std::cout << trf("doctor.effort.usage", lubancode::cli::FormatTokenCount(outcome.usage.input_tokens),
                         lubancode::cli::FormatTokenCount(outcome.usage.output_tokens),
                         lubancode::cli::FormatTokenCount(outcome.usage.cache_read_tokens))
                  << "\n";
        if (outcome.usage.output_tokens > 0) {
            std::cout << (outcome.usage.output_reasoning_tokens > 0
                              ? trf("doctor.effort.usage_reasoning",
                                    lubancode::cli::FormatTokenCount(outcome.usage.output_reasoning_tokens))
                              : tr("doctor.effort.usage_no_split"))
                      << "\n";
        }
    }
    std::cout.flush();
}

// metrics 读得到就回填会话缓存结论(状态栏/统计行跟着换措辞)。
std::optional<PrefixCacheMetrics> ReadAndReportMetrics(const DoctorContext& context) {
    const lubancode::config::Config& config = context.config;
    if (config.metrics_url.empty()) {
        std::cout << tr("doctor.cache.no_metrics") << "\n";
        return std::nullopt;
    }
    std::cout << trf("doctor.cache.metrics_header", config.metrics_url) << "\n";
    const auto text = FetchMetricsText(config.metrics_url, config);
    if (!text.has_value()) {
        std::cout << trf("doctor.cache.metrics_read_failed", text.error()) << "\n";
        return std::nullopt;
    }
    const PrefixCacheMetrics metrics = ParsePrefixCacheMetrics(*text);
    if (metrics.enabled.has_value()) {
        std::cout << (*metrics.enabled ? tr("doctor.cache.metrics_enabled") : tr("doctor.cache.metrics_disabled"))
                  << "\n";
        context.context_tracker.set_server_prefix_caching(metrics.enabled);
    } else {
        std::cout << tr("doctor.cache.metrics_enabled_unknown") << "\n";
    }
    std::cout << trf("doctor.cache.metrics_counters",
                     metrics.queries_total.has_value() ? std::to_string(*metrics.queries_total)
                                                       : std::string(tr("doctor.value.absent")),
                     metrics.hits_total.has_value() ? std::to_string(*metrics.hits_total)
                                                    : std::string(tr("doctor.value.absent")),
                     metrics.prompt_tokens_cached_total.has_value()
                         ? std::to_string(*metrics.prompt_tokens_cached_total)
                         : std::string(tr("doctor.value.absent")))
              << "\n";
    return metrics;
}

// /doctor cache probe:两轮固定前缀对账。
void RunCacheProbe(const DoctorContext& context) {
    const lubancode::config::Config& config = context.config;
    // 安全闸:公网 provider 不发探针。metrics_url 明配 = 用户声明这是自有端。
    if (!IsLoopbackUrl(config.base_url) && config.metrics_url.empty()) {
        std::cout << tr("doctor.cache.probe_gate") << "\n";
        return;
    }
    std::optional<PrefixCacheMetrics> before = ReadAndReportMetrics(context);

    const FixedPrefixProbePair pair = BuildFixedPrefixProbePair(context.current_model);
    auto backend = BuildBackend(config);

    const auto describe_round = [&](int round, const ProbeOutcome& outcome) {
        std::cout << trf("doctor.cache.probe_round", round) << "\n";
        if (!outcome.error.empty()) {
            std::cout << "  "
                      << trf("doctor.effort.http_error", outcome.http_status > 0
                                                                ? std::to_string(outcome.http_status)
                                                                : std::string("-"))
                      << "\n"
                      << "  " << SanitizeProbeError(outcome.error) << "\n";
            return;
        }
        std::cout << tr("doctor.effort.http_ok") << "\n";
        if (outcome.usage_reported) {
            std::cout << trf("doctor.cache.probe_usage",
                             lubancode::cli::FormatTokenCount(outcome.usage.input_tokens),
                             lubancode::cli::FormatTokenCount(outcome.usage.cache_read_tokens))
                      << "\n";
        } else {
            std::cout << tr("doctor.effort.usage_not_reported") << "\n";
        }
    };

    const ProbeOutcome round1 = RunProbe(*backend, pair.round1);
    describe_round(1, round1);
    const ProbeOutcome round2 = RunProbe(*backend, pair.round2);
    describe_round(2, round2);

    // 前缀字节稳定性:按当前 wire 把两轮请求体序列化出来,量公共前缀。
    {
        std::string dump1;
        std::string dump2;
        if (config.wire == lubancode::config::Wire::ChatCompletions) {
            lubancode::api::chat::ChatRequestOptions options;
            options.reasoning_param = config.think_param;
            dump1 = lubancode::api::chat::BuildRequestJson(pair.round1, config.extra_body, options).dump();
            dump2 = lubancode::api::chat::BuildRequestJson(pair.round2, config.extra_body, options).dump();
        } else if (config.wire == lubancode::config::Wire::Responses) {
            dump1 = lubancode::api::responses::BuildRequestJson(pair.round1).dump();
            dump2 = lubancode::api::responses::BuildRequestJson(pair.round2).dump();
        } else {
            dump1 = lubancode::api::anthropic::BuildRequestJson(pair.round1, false, config.extra_body).dump();
            dump2 = lubancode::api::anthropic::BuildRequestJson(pair.round2, false, config.extra_body).dump();
        }
        const std::size_t common = CommonPrefixBytes(dump1, dump2);
        std::cout << trf("doctor.cache.probe_prefix", common, pair.designed_prefix_bytes)
                  << (common >= pair.designed_prefix_bytes ? tr("doctor.cache.probe_prefix_stable")
                                                            : tr("doctor.cache.probe_prefix_broken"))
                  << "\n";
    }

    // 服务端 query/hit 增量:轮前轮后各读一次 metrics,差值才是一这轮探针
    // 自己造成的。没配 metrics_url 就只报 usage 侧的 cached tokens。
    if (before.has_value() && !config.metrics_url.empty()) {
        const auto after_text = FetchMetricsText(config.metrics_url, config);
        if (after_text.has_value()) {
            const PrefixCacheMetrics after = ParsePrefixCacheMetrics(*after_text);
            const auto delta = [](const std::optional<std::int64_t>& b, const std::optional<std::int64_t>& a)
                -> std::string {
                if (!b.has_value() || !a.has_value()) {
                    return std::string(tr("doctor.value.absent"));
                }
                return std::to_string(*a - *b);
            };
            std::cout << trf("doctor.cache.probe_delta", delta(before->queries_total, after.queries_total),
                             delta(before->hits_total, after.hits_total),
                             delta(before->prompt_tokens_cached_total, after.prompt_tokens_cached_total))
                      << "\n";
        } else {
            std::cout << trf("doctor.cache.metrics_read_failed", after_text.error()) << "\n";
        }
    }
    std::cout.flush();
}

// /doctor cache usage:stream_usage 能力探针,结论写回 provider 配置。
void RunStreamUsageProbe(const DoctorContext& context) {
    lubancode::config::Config& config = context.config;
    if (config.wire != lubancode::config::Wire::ChatCompletions) {
        std::cout << tr("doctor.usage.not_chat") << "\n";
        return;
    }
    if (context.active_provider.empty() || lubancode::config::FindProvider(context.providers,
                                                                            context.active_provider) == nullptr) {
        std::cout << tr("doctor.usage.no_provider") << "\n";
        return;
    }
    std::cout << tr("doctor.usage.probing") << "\n";
    std::cout.flush();

    // 探针 backend:与正常请求同一套地址/密钥/超时/extra_body,唯独
    // stream_usage 强制为 true——要试的就是这个能力。
    const auto headers =
        lubancode::config::ResolveProviderHeaderTemplates(config.extra_headers, config.auth_token);
    lubancode::api::chat::ChatRequestOptions options;
    options.reasoning_param = config.think_param;
    options.stream_usage = true;
    lubancode::api::chat::ChatCompletionsBackend backend(config.base_url, config.auth_token,
                                                         config.connect_timeout_ms,
                                                         config.stream_idle_timeout_secs, config.extra_body,
                                                         headers, std::move(options),
                                                         config.request_hard_timeout_secs);
    const api::Request probe = BuildEffortProbeRequest(context.current_model, context.current_think);
    const ProbeOutcome outcome = RunProbe(backend, probe);
    if (!outcome.error.empty()) {
        std::cout << trf("doctor.effort.http_error", outcome.http_status > 0
                                                         ? std::to_string(outcome.http_status)
                                                         : std::string("-"))
                  << "\n"
                  << "  " << SanitizeProbeError(outcome.error) << "\n";
        return;
    }
    const bool supported = outcome.usage_reported;
    std::cout << (supported ? tr("doctor.usage.supported") : tr("doctor.usage.unsupported")) << "\n";
    // 写回:内存里的 providers 列表先改,再落到 active_provider 所在的配置
    // 文件(项目级钉住写项目路径,否则写全局),当前生效的 config.stream_usage
    // 一并同步(backend 由调用方 rebuild)。
    lubancode::config::SetProviderStreamUsage(context.providers, context.active_provider, supported);
    const auto saved =
        context.provider_write_path.has_value()
            ? lubancode::config::UpdateProvidersInConfigFile(*context.provider_write_path, context.providers)
                  .transform([path = *context.provider_write_path]() { return path; })
            : lubancode::config::SetProviderStreamUsageInGlobalConfig(context.active_provider, supported);
    if (!saved.has_value()) {
        std::cout << trf("doctor.usage.write_failed", saved.error()) << "\n";
        return;
    }
    config.stream_usage = supported;
    config.stream_usage_declared = true;
    std::cout << trf("doctor.usage.written", context.active_provider, *saved) << "\n";
    std::cout.flush();
}

void PrintDoctorOverview(const DoctorContext& context) {
    const lubancode::config::Config& config = context.config;
    std::cout << tr("doctor.overview.header") << "\n";
    std::cout << tr("doctor.overview.effort")
              << (context.current_think.empty() ? tr("doctor.level.unset") : context.current_think) << "\n";
    std::cout << tr("doctor.overview.declared")
              << (config.provider_think_levels.empty()
                      ? tr("doctor.overview.unverified")
                      : trf("doctor.overview.levels", config.provider_think_levels.size()))
              << "\n";
    std::cout << tr("doctor.overview.cache")
              << [&] {
                    const auto observed = context.context_tracker.server_prefix_caching();
                    if (!observed.has_value()) {
                        return std::string(tr("doctor.cache.state.unverified"));
                    }
                    return *observed ? std::string(tr("doctor.cache.metrics_enabled"))
                                     : std::string(tr("doctor.cache.metrics_disabled"));
                }()
              << "\n";
    std::cout << tr("doctor.overview.usage_hint") << "\n";
    std::cout.flush();
}

// /doctor agents:main 与各 agent type 的差异矩阵(规格"架构落点":能力
// 差异要打印得出来,不靠散落的 Register/不 Register 暗示)。矩阵材料从
// DoctorContext 来,没接(单测/旧调用方)就整节省略。i18n 中英成对。
void PrintAgentsMatrix(const DoctorContext& context) {
    std::cout << tr("doctor.agents.header") << "\n";
    const auto tool_count = [](const lubancode::tools::ToolRegistry* registry) {
        return registry != nullptr ? registry->All().size() : std::size_t{0};
    };
    if (context.main_profile != nullptr) {
        std::cout << trf("doctor.agents.budget",
                         context.main_profile->max_output_tokens.value_or(0),
                         context.main_profile->max_steps_per_turn == 0
                             ? std::string(tr("config.steps.unlimited"))
                             : std::to_string(context.main_profile->max_steps_per_turn),
                         context.main_profile->length_continuations)
                  << "\n";
        std::cout << trf("doctor.agents.governance",
                         context.config.subagent.max_active.value_or(lubancode::config::kDefaultSubagentMaxActive),
                         context.config.subagent.max_depth.value_or(lubancode::config::kDefaultSubagentMaxDepth))
                  << "\n";
    }
    if (context.main_registry != nullptr) {
        std::cout << trf("doctor.agents.row_main", tool_count(context.main_registry)) << "\n";
    }
    if (context.sub_registry != nullptr) {
        std::cout << trf("doctor.agents.row_sub", tool_count(context.sub_registry)) << "\n";
    }
    if (context.explore_registry != nullptr) {
        std::cout << trf("doctor.agents.row_explore", tool_count(context.explore_registry)) << "\n";
    }
    std::cout << tr("doctor.agents.note") << "\n";
    // 子代理流诊断开关(规格"二、治'无法证明'"):这里提一句,下次用户问
    // "你怎么知道真入了账"就有现成的自查口。
    std::cout << tr("doctor.agents.subagent_debug_log") << "\n";
    std::cout.flush();
}

}  // namespace

void HandleDoctorCommand(const std::string& args, const DoctorContext& context) {
    std::istringstream input(args);
    std::string subcommand;
    input >> subcommand;
    for (char& c : subcommand) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string rest;
    std::getline(input, rest);
    const std::size_t rest_begin = rest.find_first_not_of(' ');
    rest = rest_begin == std::string::npos ? std::string() : rest.substr(rest_begin);

    if (subcommand == "effort") {
        std::string level = rest;
        for (char& c : level) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (level == "unset" || level == "none-param" || level == "off") {
            level.clear();  // 探"不发参数"那条路
        }
        RunEffortProbe(level, context);
        return;
    }
    if (subcommand == "cache") {
        std::string action;
        std::istringstream rest_input(rest);
        rest_input >> action;
        for (char& c : action) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (action == "probe") {
            RunCacheProbe(context);
            return;
        }
        if (action == "usage") {
            RunStreamUsageProbe(context);
            return;
        }
        ReadAndReportMetrics(context);
        return;
    }
    if (subcommand == "agents") {
        PrintAgentsMatrix(context);
        return;
    }
    if (subcommand.empty()) {
        PrintDoctorOverview(context);
        return;
    }
    std::cout << tr("doctor.usage.usage_line") << "\n";
    std::cout.flush();
}

}  // namespace lubancode::app
