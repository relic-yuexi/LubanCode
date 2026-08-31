// /doctor 的执行体:effort/cache 两个子命令的探针、指标读取与报告。纯函数
// (请求构造、指标解析、四态分类、前缀对账)全导出在 doctor_commands.hpp,
// 单测直接钉;这一头只留 IO(发请求、读 metrics、写回配置)与打印。

#include "app/commands/doctor_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include <cpr/cpr.h>

#include "agent/context.hpp"  // EstimateUtf8Tokens:设计前缀 token 估算的统一口径
#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/chat/request.hpp"
#include "api/gemini/request.hpp"
#include "api/responses/client.hpp"
#include "api/responses/request.hpp"
#include "app/backend_stack.hpp"
#include "cli/console_input.hpp"  // ReadLine:公网探针的一次性确认门(问题 9)
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "tools/shell_info.hpp"

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

// 一行 metrics 的数值("prefix_cache_queries_total 12.0" 的 12.0)。取名字
//(或标签块)之后的第一枚数值记号——Prometheus 文本还允许跟第二枚时间戳
// 记号("name 12.0 1699999999"),取尾巴会把时间戳当读数。
std::optional<std::int64_t> MetricValueFromLine(const std::string& line, std::size_t name_end) {
    std::size_t pos = name_end;
    if (pos < line.size() && line[pos] == '{') {
        const std::size_t close = line.find('}', pos);
        if (close == std::string::npos) {
            return std::nullopt;  // 标签块没闭合,这行坏了
        }
        pos = close + 1;
    }
    pos = line.find_first_not_of(" \t", pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t token_end = line.find_first_of(" \t", pos);
    if (token_end == std::string::npos) {
        token_end = line.size();
    }
    const std::string value_text = line.substr(pos, token_end - pos);
    try {
        const double value = std::stod(value_text);
        return static_cast<std::int64_t>(std::llround(value));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

// ---------------- 纯函数 ----------------

api::Request BuildEffortProbeRequest(const std::string& model, const std::string& level,
                                     std::optional<int> max_tokens) {
    api::Request probe;
    probe.model = model;
    probe.system = "You are a diagnostic probe. Answer with exactly: ok";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"Reply with exactly: ok"});
    probe.messages.push_back(std::move(user));
    probe.max_tokens = max_tokens.value_or(64);
    probe.reasoning_effort = level;  // 空串 = 不发参数,字段整个缺席
    return probe;
}

std::string DescribeRequestEffort(lubancode::config::Wire wire, const api::Request& request,
                                  const nlohmann::json& extra_body, const std::string& think_param) {
    // 模型协议兼容实录矩阵单 P1:报告"最终 wire 形状"——请求体里所有
    // reasoning 相关键的键名与值一并列出(extra_body 压过的如实报),
    // 不再只挑一只键。方言状态跟在尾巴上:没声明/没验证不冒充。
    const auto collect = [](const nlohmann::json& body, const std::string& fallback_key,
                            const std::string& extra_key) {
        std::vector<std::string> parts;
        // chat 家的档位参数名听 provider 声明(think_param),先看它。
        if (!extra_key.empty()) {
            if (auto it = body.find(extra_key); it != body.end()) {
                parts.push_back(extra_key + " = " + it->dump());
            }
        }
        for (const char* key : {"enable_thinking", "reasoning", "thinking", "thinking_budget",
                                "output_config"}) {
            auto it = body.find(key);
            if (it == body.end()) continue;
            // 常见组合拆成人话路径,与其让用户读整只对象,不如直接给字段级
            // 形状(与手册段落名对得上)。
            if (std::string(key) == "reasoning" && it->is_object() && it->contains("effort") &&
                it->size() == 1) {
                parts.push_back("reasoning.effort = " + (*it)["effort"].dump());
            } else if (std::string(key) == "thinking" && it->is_object() &&
                       it->contains("budget_tokens")) {
                parts.push_back("thinking.type = " + it->value("type", std::string()) +
                                "  thinking.budget_tokens = " + (*it)["budget_tokens"].dump());
            } else {
                parts.push_back(std::string(key) + " = " + it->dump());
            }
        }
        if (auto it = body.find("generationConfig"); it != body.end() && it->is_object()) {
            if (auto thinking = it->find("thinkingConfig");
                thinking != it->end() && !thinking->is_null()) {
                parts.push_back("generationConfig.thinkingConfig = " + thinking->dump());
            }
        }
        if (parts.empty()) {
            return trf("doctor.effort.field_absent", fallback_key);
        }
        std::string out;
        for (const auto& part : parts) {
            if (!out.empty()) out += "  ";
            out += part;
        }
        return out;
    };
    std::string described;
    if (wire == lubancode::config::Wire::ChatCompletions) {
        lubancode::api::chat::ChatRequestOptions options;
        options.reasoning_param = think_param;
        const nlohmann::json body = lubancode::api::chat::BuildRequestJson(request, extra_body, options);
        const std::string param = think_param.empty() ? std::string("reasoning_effort") : think_param;
        described = collect(body, param, param);
    } else if (wire == lubancode::config::Wire::Responses) {
        const nlohmann::json body = lubancode::api::responses::BuildRequestJson(request);
        described = collect(body, "reasoning.effort", "");
    } else if (wire == lubancode::config::Wire::GoogleGenerateContent) {
        const nlohmann::json body = lubancode::api::gemini::BuildRequestJson(request, extra_body);
        described = collect(body, "generationConfig.thinkingConfig", "");
    } else {
        const nlohmann::json body = lubancode::api::anthropic::BuildRequestJson(request, false, extra_body);
        described = collect(body, "thinking", "");
        if (!request.reasoning_effort.empty()) {
            described += "(" + request.reasoning_effort + tr("doctor.effort.mapped_suffix");
        }
    }
    // 方言状态(请求里没带档案 = 本地自定义端,走兼容形状)。诊断行直接
    // 拼字(与上面的键值行同一风格)。
    if (request.reasoning.dialect.empty()) {
        return described + "  [无方言声明,走兼容形状]";
    }
    return described + "  " +
           (request.reasoning.dialect.verified ? "[方言已验证]" : "[方言未验证]");
}

PrefixCacheMetrics ParsePrefixCacheMetrics(const std::string& text) {
    PrefixCacheMetrics out;
    // v1 引擎把前缀缓存按层拆成 gpu_/cpu_ 两族(旧名缺席才用);各族内
    // 同名多行(带 label)取末行——vLLM 输出的末行是总计。
    std::optional<std::int64_t> gpu_queries, gpu_hits, cpu_queries, cpu_hits;
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
        } else if (name == "gpu_prefix_cache_queries_total") {
            gpu_queries = *value;
        } else if (name == "gpu_prefix_cache_hits_total") {
            gpu_hits = *value;
        } else if (name == "cpu_prefix_cache_queries_total") {
            cpu_queries = *value;
        } else if (name == "cpu_prefix_cache_hits_total") {
            cpu_hits = *value;
        } else if (name == "prompt_tokens_cached_total") {
            out.prompt_tokens_cached_total = *value;
        } else if (name == "num_requests_running") {
            out.num_requests_running = *value;
        } else if (name == "num_requests_waiting") {
            out.num_requests_waiting = *value;
        }
    }
    const auto merge = [](std::optional<std::int64_t>& total, std::optional<std::int64_t> gpu,
                          std::optional<std::int64_t> cpu) {
        if (total.has_value()) {
            return;  // v0 旧名在场就是总数,不与分层数叠加
        }
        if (gpu.has_value() || cpu.has_value()) {
            total = gpu.value_or(0) + cpu.value_or(0);
        }
    };
    merge(out.queries_total, gpu_queries, cpu_queries);
    merge(out.hits_total, gpu_hits, cpu_hits);
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

std::vector<std::string> SummarizeEffortProbeRounds(const std::vector<EffortProbeRoundResult>& rounds,
                                                    bool off_requested) {
    // 四账分开(MiniCPM5 真机巡检单 P1):HTTP 接受、thinking 产出、正文
    // 产出、终止原因分布——一眼能看出"2xx 收了但思考照发"这种方言不生效。
    // 判词只陈述观察,不拿 2xx 当行为支持;预算被思考耗尽的回只进
    // inconclusive 账,不拿 text=0 说事。
    std::vector<std::string> lines;
    if (rounds.empty()) {
        lines.push_back("未发出任何探针。");
        return lines;
    }
    const std::size_t total = rounds.size();
    const std::size_t http_ok = std::count_if(rounds.begin(), rounds.end(),
                                              [](const EffortProbeRoundResult& r) { return r.http_ok; });
    const std::size_t thinking_rounds =
        std::count_if(rounds.begin(), rounds.end(),
                      [](const EffortProbeRoundResult& r) { return r.thinking_chars > 0; });
    const std::size_t text_rounds = std::count_if(rounds.begin(), rounds.end(),
                                                  [](const EffortProbeRoundResult& r) { return r.text_chars > 0; });
    lines.push_back("HTTP 接受:" + std::to_string(http_ok) + "/" + std::to_string(total) + " 回收到 2xx");
    lines.push_back("thinking 产出:" + std::to_string(thinking_rounds) + "/" + std::to_string(total) +
                    " 回有思考增量");
    lines.push_back("正文产出:" + std::to_string(text_rounds) + "/" + std::to_string(total) +
                    " 回有正文增量");
    // 终止原因分布:同因聚合计数,空 stop_reason 单独记"未回"。
    std::map<std::string, std::size_t> stop_counts;
    for (const EffortProbeRoundResult& r : rounds) {
        const std::string key = r.stop_reason.empty() ? "(未回)" : r.stop_reason;
        ++stop_counts[key];
    }
    std::string distribution;
    for (const auto& [reason, count] : stop_counts) {
        if (!distribution.empty()) {
            distribution += " · ";
        }
        distribution += reason + " ×" + std::to_string(count);
    }
    lines.push_back("终止原因分布:" + distribution);

    // 判词:先看有没有"预算耗尽"(stop=max_tokens 且正文 0)的回——这类回
    // 对"正文是否产出"没有发言权。全部回都耗尽 = 只判 inconclusive;部分
    // 耗尽,判词按有效回说,但附一句提醒。
    const auto budget_exhausted = [](const EffortProbeRoundResult& r) {
        return r.http_ok && r.stop_reason == "max_tokens" && r.text_chars == 0;
    };
    const std::size_t exhausted =
        std::count_if(rounds.begin(), rounds.end(), budget_exhausted);
    const std::size_t effective = http_ok - exhausted;
    if (exhausted > 0) {
        lines.push_back("探针预算耗尽(思考吃满 max_tokens、正文 0):" + std::to_string(exhausted) +
                        "/" + std::to_string(total) + " 回,这些回不判支持或不支持。");
    }
    if (effective == 0) {
        lines.push_back("判词:inconclusive——有效回为 0(探针预算全被思考耗尽或全非 2xx),"
                        "换更大预算或先看 HTTP 账,别拿这轮回当档位证据。");
        return lines;
    }
    const std::size_t effective_thinking = std::count_if(
        rounds.begin(), rounds.end(), [&](const EffortProbeRoundResult& r) {
            return r.http_ok && !budget_exhausted(r) && r.thinking_chars > 0;
        });
    const std::size_t effective_text = std::count_if(
        rounds.begin(), rounds.end(), [&](const EffortProbeRoundResult& r) {
            return r.http_ok && !budget_exhausted(r) && r.text_chars > 0;
        });
    lines.push_back("判词:有效 " + std::to_string(effective) + " 回里 " + std::to_string(effective_thinking) +
                    " 回产出思考、" + std::to_string(effective_text) +
                    " 回产出正文。这是行为观察,不替服务端背书档位语义;"
                    "/think none 档若仍见思考产出,即关闭未被端点证实。");
    if (off_requested && effective_thinking > 0) {
        // 明报(勘察单 P1 补账):关闭档的请求被端 2xx 收下不等于生效——
        // 有效回里仍有思考,就是"当没看见"。判词点名,不藏在条件句里。
        lines.push_back("判词:本档是关闭档,请求端 2xx 收下,但 " + std::to_string(effective_thinking) +
                        "/" + std::to_string(effective) +
                        " 有效回仍产出思考——关闭未被端点证实,这端把关闭参数当没看见;"
                        "要真关思考,走实测生效的那条路(如 chat 面的 chat_template_kwargs)。");
    }
    return lines;
}

FixedPrefixProbeSet BuildFixedPrefixProbes(const std::string& model, int rounds,
                                            std::size_t prefix_fill_bytes) {
    // 填充段是一段无信息量的固定文字:诊断信号在前缀字节稳不稳,不在内容。
    const std::string filler = "prefix-cache probe filler segment 0123456789. ";
    std::string fixed;
    fixed.reserve(prefix_fill_bytes + filler.size());
    while (fixed.size() < prefix_fill_bytes) {
        fixed += filler;
    }
    const std::string system = "You are a prefix-cache diagnostic probe. Always answer with exactly: ok";

    if (rounds < 2) {
        rounds = 2;
    }
    if (rounds > kCacheProbeMaxRounds) {
        rounds = kCacheProbeMaxRounds;
    }
    FixedPrefixProbeSet out;
    out.requests.reserve(static_cast<std::size_t>(rounds));
    for (int round = 1; round <= rounds; ++round) {
        api::Request request;
        request.model = model;
        request.system = system;
        request.max_tokens = static_cast<int>(kCacheProbeOutputTokenCapPerRound);
        api::Message prefix;
        prefix.role = api::Role::User;
        prefix.content.push_back(api::TextBlock{fixed});
        api::Message tail;
        tail.role = api::Role::User;
        // 每轮尾巴各一句:公共前缀(system+固定消息)之外只差这几个字节,
        // 序列化后前缀是否逐字节稳定,CommonPrefixBytes 一量便知。
        tail.content.push_back(api::TextBlock{"Round " + std::to_string(round) +
                                              ". Reply with exactly: ok"});
        request.messages.push_back(std::move(prefix));
        request.messages.push_back(std::move(tail));
        out.requests.push_back(std::move(request));
    }
    out.designed_prefix_bytes = system.size() + fixed.size();
    // 设计前缀的 token 估算(分型比对用):与全库统一口径同一把尺
    //(agent::EstimateUtf8Tokens;填充段是 ASCII,约 4 字符/token)。
    out.designed_prefix_tokens =
        static_cast<std::int64_t>(lubancode::agent::EstimateUtf8Tokens(system + fixed));
    return out;
}

FixedPrefixProbePair BuildFixedPrefixProbePair(const std::string& model, std::size_t prefix_fill_bytes) {
    // 旧两口(单测钉着):N 轮组的前两轮,行为与从前一字不差。
    const FixedPrefixProbeSet set = BuildFixedPrefixProbes(model, 2, prefix_fill_bytes);
    FixedPrefixProbePair out;
    out.round1 = set.requests[0];
    out.round2 = set.requests[1];
    out.designed_prefix_bytes = set.designed_prefix_bytes;
    return out;
}

bool AllowCacheProbeRun(bool loopback, bool has_metrics_url, std::optional<bool> consent) {
    if (loopback || has_metrics_url) {
        return true;  // 本机端 / 明配 metrics 的自有端:旧安全闸的放行面不变
    }
    return consent.has_value() && *consent;  // 公网:没答、拒答都不发
}

std::optional<bool> ParseProbeConsentAnswer(const std::string& answer) {
    std::string text;
    text.reserve(answer.size());
    for (const char c : answer) {
        if (c != ' ' && c != '\t') {
            text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (text == "y" || text == "yes" || text == "是" || text == "好") {
        return true;
    }
    if (text == "n" || text == "no" || text == "否" || text == "不") {
        return false;
    }
    return std::nullopt;  // 空句/认不得:不算同意,调用方按不放行处理
}

std::string DescribeEndpointForDisclosure(const std::string& url) {
    // 只留 scheme://host[:port][/路径]:query 与 fragment 整段剥掉——
    // 确认门要明写发去哪,但不把可能带 key 的查询参数亮到屏上或日志里。
    const std::size_t cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}

CacheProbeVerdict ClassifyCacheProbeRounds(const std::vector<CacheProbeRoundResult>& rounds,
                                            std::int64_t designed_prefix_tokens,
                                            int coverage_threshold_percent) {
    // 只看 http_ok 且真报了 usage 的轮——HTTP 失败与缺测的轮不进分型,
    // 不拿 0 冒充证据。
    std::vector<const CacheProbeRoundResult*> reported;
    reported.reserve(rounds.size());
    for (const auto& round : rounds) {
        if (round.http_ok && round.usage_reported) {
            reported.push_back(&round);
        }
    }
    if (reported.empty()) {
        return CacheProbeVerdict::NotReported;
    }
    // 全零:完全未见命中。
    bool any_hit = false;
    for (const auto* round : reported) {
        if (round->cache_read > 0) {
            any_hit = true;
            break;
        }
    }
    if (!any_hit) {
        return CacheProbeVerdict::NoHit;
    }
    // 首轮按惯例是写缓存(冷启动 miss 不算上游的错),判命中形状看后续
    // 轮;只有首轮可用时(两轮探针、第二轮 HTTP 失败)退回首轮自己。
    std::size_t tail_begin = reported.size() >= 2 ? 1 : 0;
    bool tail_has_miss = false;
    bool tail_all_covered = true;
    bool tail_constant = true;
    const std::int64_t threshold =
        designed_prefix_tokens > 0
            ? designed_prefix_tokens * coverage_threshold_percent / 100
            : (std::numeric_limits<std::int64_t>::max)();  // 括号防 windows.h 的 max 宏
    const std::int64_t reference = reported[tail_begin]->cache_read;
    for (std::size_t i = tail_begin; i < reported.size(); ++i) {
        const std::int64_t cache_read = reported[i]->cache_read;
        if (cache_read == 0) {
            tail_has_miss = true;
        }
        if (cache_read < threshold) {
            tail_all_covered = false;
        }
        if (cache_read != reference) {
            tail_constant = false;
        }
    }
    if (tail_has_miss) {
        return CacheProbeVerdict::IntermittentMiss;
    }
    if (tail_all_covered) {
        return CacheProbeVerdict::StableHit;
    }
    if (tail_constant) {
        // 恒定且低于设计前缀:上游只肯缓存一只固定的块(如恒 1024)。
        return CacheProbeVerdict::FixedQuantumHit;
    }
    // 全命中、没吃满、也不恒定:命中量在抖——归间歇 miss,比硬塞进
    // "稳定"诚实。
    return CacheProbeVerdict::IntermittentMiss;
}

std::string CacheProbeVerdictLabel(CacheProbeVerdict verdict) {
    switch (verdict) {
        case CacheProbeVerdict::StableHit:
            return tr("doctor.cache.verdict.stable_hit");
        case CacheProbeVerdict::FixedQuantumHit:
            return tr("doctor.cache.verdict.fixed_quantum");
        case CacheProbeVerdict::IntermittentMiss:
            return tr("doctor.cache.verdict.intermittent");
        case CacheProbeVerdict::NoHit:
            return tr("doctor.cache.verdict.no_hit");
        case CacheProbeVerdict::NotReported:
            return tr("doctor.cache.verdict.not_reported");
    }
    return std::string();
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
    // 探针预算给推理优先模型留足(MiniCPM5 真机巡检单 P1):64 token 全耗在
    // thinking 的现场,正文压根儿没机会产出——预算耗尽的回只判 inconclusive。
    api::Request probe = BuildEffortProbeRequest(context.current_model, level, kEffortProbeBudgetTokens);
    // 模型档案(目录里有就带上,模型协议兼容实录矩阵单 P1):方言决定请求
    // 落线形状。目录查不到 = 本地自定义端,走兼容形状,诊断行自己会标。
    const auto catalog = lubancode::config::LoadProviderCatalog();
    if (const auto* preset = catalog.FindProvider(context.active_provider); preset != nullptr) {
        if (const auto* model = preset->FindModel(context.current_model); model != nullptr) {
            probe.reasoning = model->reasoning;
        }
    }
    // 目录侧声明(模型目录条目,含用户 models.json):思考关不掉的模型在
    // 这儿明说,none 档的行为预期先立住,再让三回对照去验证。
    const lubancode::config::ModelCatalog model_catalog = lubancode::config::LoadModelCatalog();
    const lubancode::config::ModelCatalogEntry* entry =
        model_catalog.FindByProviderAndSlug(context.active_provider, context.current_model);

    TermOut() << trf("doctor.effort.probe_header", context.current_model,
                     level.empty() ? tr("doctor.level.unset") : level)
              << "\n";
    if (lubancode::config::ClassifyThinkOffDeclaration(entry) ==
        lubancode::config::ThinkOffDeclaration::DeclaredUnsupported) {
        TermOut() << "目录声明:此模型思考关不掉(always_think/off_unsupported)。none 档发出关闭请求后,"
                     "仍以三回对照的 thinking 产出账为准。\n";
    }
    // 请求侧:先看"实际发送值"——按当前 wire 把请求体翻出来,字段在不在、
    // 值是什么,如实报告(不发送任何正文,只报参数名与档位值)。
    TermOut() << tr("doctor.effort.request_field") << " "
              << DescribeRequestEffort(config.wire, probe, config.extra_body, config.think_param) << "\n";
    TermOut() << "每档重复 " << kEffortProbeRepeats
              << " 回对照(单回 2xx 不当行为支持;探针预算 " << kEffortProbeBudgetTokens
              << " token,被思考耗尽的回只判 inconclusive)。\n";
    TermOut() << tr("doctor.probe.sending") << "\n";
    TermOut().flush();

    auto backend = BuildBackend(config);
    std::vector<EffortProbeRoundResult> rounds;
    for (int round = 1; round <= kEffortProbeRepeats; ++round) {
        const ProbeOutcome outcome = RunProbe(*backend, probe);
        EffortProbeRoundResult result;
        result.http_ok = outcome.error.empty();
        result.thinking_chars = outcome.thinking_chars;
        result.text_chars = outcome.text_chars;
        result.stop_reason = outcome.stop_reason;
        rounds.push_back(result);
        TermOut() << "  第 " << round << "/" << kEffortProbeRepeats << " 回: ";
        if (!outcome.error.empty()) {
            TermOut() << trf("doctor.effort.http_error",
                             outcome.http_status > 0 ? std::to_string(outcome.http_status) : std::string("-"))
                      << "  " << SanitizeProbeError(outcome.error) << "\n";
            continue;
        }
        TermOut() << tr("doctor.effort.http_ok") << "  " << tr("doctor.effort.finish") << " "
                  << (outcome.stop_reason.empty() ? std::string(tr("doctor.value.absent")) : outcome.stop_reason)
                  << "  " << trf("doctor.effort.body", outcome.text_chars, outcome.thinking_chars) << "\n";
        TermOut().flush();
    }

    // 关闭档探针的判词要明报"收下但无效"(SummarizeEffortProbeRounds 注释):
    // 档位按模型方言折算(none/off 类都算关闭档)。
    const bool off_requested =
        !level.empty() && ReasoningEffortIsOff(level, probe.reasoning);
    for (const std::string& line : SummarizeEffortProbeRounds(rounds, off_requested)) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

// metrics 读得到就回填会话缓存结论(状态栏/统计行跟着换措辞)。
std::optional<PrefixCacheMetrics> ReadAndReportMetrics(const DoctorContext& context) {
    const lubancode::config::Config& config = context.config;
    if (config.metrics_url.empty()) {
        TermOut() << tr("doctor.cache.no_metrics") << "\n";
        return std::nullopt;
    }
    TermOut() << trf("doctor.cache.metrics_header", config.metrics_url) << "\n";
    const auto text = FetchMetricsText(config.metrics_url, config);
    if (!text.has_value()) {
        TermOut() << trf("doctor.cache.metrics_read_failed", text.error()) << "\n";
        return std::nullopt;
    }
    const PrefixCacheMetrics metrics = ParsePrefixCacheMetrics(*text);
    if (metrics.enabled.has_value()) {
        TermOut() << (*metrics.enabled ? tr("doctor.cache.metrics_enabled") : tr("doctor.cache.metrics_disabled"))
                  << "\n";
        context.context_tracker.set_server_prefix_caching(metrics.enabled);
    } else {
        TermOut() << tr("doctor.cache.metrics_enabled_unknown") << "\n";
    }
    TermOut() << trf("doctor.cache.metrics_counters",
                     metrics.queries_total.has_value() ? std::to_string(*metrics.queries_total)
                                                       : std::string(tr("doctor.value.absent")),
                     metrics.hits_total.has_value() ? std::to_string(*metrics.hits_total)
                                                    : std::string(tr("doctor.value.absent")),
                     metrics.prompt_tokens_cached_total.has_value()
                         ? std::to_string(*metrics.prompt_tokens_cached_total)
                         : std::string(tr("doctor.value.absent")))
              << "\n";
    // vLLM 常见负载 gauge(现场语境,不是缓存指标):任一在场才报这一行。
    if (metrics.num_requests_running.has_value() || metrics.num_requests_waiting.has_value()) {
        TermOut() << trf("doctor.cache.metrics_load",
                         metrics.num_requests_running.has_value()
                             ? std::to_string(*metrics.num_requests_running)
                             : std::string(tr("doctor.value.absent")),
                         metrics.num_requests_waiting.has_value()
                             ? std::to_string(*metrics.num_requests_waiting)
                             : std::string(tr("doctor.value.absent")))
                  << "\n";
    }
    return metrics;
}

// /doctor cache probe:N 轮固定前缀对账(默认 2,上限 kCacheProbeMaxRounds)。
// 公网 provider 走一次性确认门(问题 9):先披露轮数、token 上限与端点,
// 答应才发;回环端与明配 metrics_url 的端照旧直发。
void RunCacheProbe(const DoctorContext& context, int rounds) {
    const lubancode::config::Config& config = context.config;
    if (rounds < 2) {
        rounds = 2;
    }
    if (rounds > kCacheProbeMaxRounds) {
        TermOut() << trf("doctor.cache.probe_rounds_capped", kCacheProbeMaxRounds) << "\n";
        rounds = kCacheProbeMaxRounds;
    }
    const bool loopback = IsLoopbackUrl(config.base_url);
    // 确认门:回环/明配 metrics 直发;公网先披露再问,未确认不发。
    if (!AllowCacheProbeRun(loopback, !config.metrics_url.empty(), std::nullopt)) {
        TermOut() << tr("doctor.cache.probe_disclosure_header") << "\n";
        TermOut() << trf("doctor.cache.probe_disclosure_body",
                         DescribeEndpointForDisclosure(config.base_url), rounds,
                         lubancode::cli::FormatTokenCount(kCacheProbeInputTokenCapPerRound),
                         lubancode::cli::FormatTokenCount(kCacheProbeOutputTokenCapPerRound))
                  << "\n";
        const auto answer =
            lubancode::cli::ReadLine(tr("doctor.cache.probe_confirm_prompt"), context.theme);
        if (!answer.has_value() ||
            !AllowCacheProbeRun(loopback, !config.metrics_url.empty(), ParseProbeConsentAnswer(*answer))) {
            TermOut() << tr("doctor.cache.probe_declined") << "\n";
            TermOut().flush();
            return;
        }
    }
    std::optional<PrefixCacheMetrics> before = ReadAndReportMetrics(context);

    const FixedPrefixProbeSet probes = BuildFixedPrefixProbes(context.current_model, rounds);
    auto backend = BuildBackend(config);
    std::vector<CacheProbeRoundResult> round_results;
    round_results.reserve(probes.requests.size());

    const auto describe_round = [&](int round, const ProbeOutcome& outcome) {
        TermOut() << trf("doctor.cache.probe_round", round) << "\n";
        if (!outcome.error.empty()) {
            TermOut() << "  "
                      << trf("doctor.effort.http_error", outcome.http_status > 0
                                                                ? std::to_string(outcome.http_status)
                                                                : std::string("-"))
                      << "\n"
                      << "  " << SanitizeProbeError(outcome.error) << "\n";
            return;
        }
        TermOut() << tr("doctor.effort.http_ok") << "\n";
        if (outcome.usage_reported) {
            TermOut() << trf("doctor.cache.probe_usage",
                             lubancode::cli::FormatTokenCount(api::TotalInputTokens(outcome.usage)),
                             lubancode::cli::FormatTokenCount(outcome.usage.cache_read_tokens))
                      << "\n";
        } else {
            TermOut() << tr("doctor.effort.usage_not_reported") << "\n";
        }
    };

    for (std::size_t i = 0; i < probes.requests.size(); ++i) {
        const ProbeOutcome outcome = RunProbe(*backend, probes.requests[i]);
        describe_round(static_cast<int>(i) + 1, outcome);
        CacheProbeRoundResult result;
        result.http_ok = outcome.error.empty();
        result.usage_reported = outcome.usage_reported;
        if (outcome.usage_reported) {
            result.cache_read = outcome.usage.cache_read_tokens;
            result.total_input = api::TotalInputTokens(outcome.usage);
        }
        round_results.push_back(result);
    }

    // 前缀字节稳定性:按当前 wire 把各轮请求体序列化出来,量相邻两轮的
    // 公共前缀,取最小值——任意相邻一对不稳,前缀就是不稳。
    {
        const auto dump_request = [&](const api::Request& request) {
            if (config.wire == lubancode::config::Wire::ChatCompletions) {
                lubancode::api::chat::ChatRequestOptions options;
                options.reasoning_param = config.think_param;
                return lubancode::api::chat::BuildRequestJson(request, config.extra_body, options).dump();
            }
            if (config.wire == lubancode::config::Wire::Responses) {
                return lubancode::api::responses::BuildRequestJson(request).dump();
            }
            if (config.wire == lubancode::config::Wire::GoogleGenerateContent) {
                return lubancode::api::gemini::BuildRequestJson(request, config.extra_body).dump();
            }
            return lubancode::api::anthropic::BuildRequestJson(request, false, config.extra_body).dump();
        };
        std::size_t common = probes.designed_prefix_bytes;  // 上限:设计前缀
        std::string previous = dump_request(probes.requests[0]);
        for (std::size_t i = 1; i < probes.requests.size(); ++i) {
            const std::string current = dump_request(probes.requests[i]);
            const std::size_t pair_common = CommonPrefixBytes(previous, current);
            if (pair_common < common) {
                common = pair_common;
            }
            previous = current;
        }
        TermOut() << trf("doctor.cache.probe_prefix", common, probes.designed_prefix_bytes)
                  << (common >= probes.designed_prefix_bytes ? tr("doctor.cache.probe_prefix_stable")
                                                              : tr("doctor.cache.probe_prefix_broken"))
                  << "\n";
    }

    // 分型(问题 9):多轮固定前缀的命中形状——稳定命中/固定阈值命中/
    // 间歇 miss/完全未见命中/无法判定,判词后紧跟证据边界,不越权背书。
    {
        const CacheProbeVerdict verdict = ClassifyCacheProbeRounds(round_results, probes.designed_prefix_tokens);
        TermOut() << trf("doctor.cache.probe_verdict", CacheProbeVerdictLabel(verdict)) << "\n";
        if (!before.has_value() || !before->enabled.has_value()) {
            TermOut() << tr("doctor.cache.probe_evidence_bound") << "\n";
        }
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
            TermOut() << trf("doctor.cache.probe_delta", delta(before->queries_total, after.queries_total),
                             delta(before->hits_total, after.hits_total),
                             delta(before->prompt_tokens_cached_total, after.prompt_tokens_cached_total))
                      << "\n";
        } else {
            TermOut() << trf("doctor.cache.metrics_read_failed", after_text.error()) << "\n";
        }
    }
    TermOut().flush();
}

// /doctor cache usage:stream_usage 能力探针,结论写回 provider 配置。
void RunStreamUsageProbe(const DoctorContext& context) {
    lubancode::config::Config& config = context.config;
    if (config.wire != lubancode::config::Wire::ChatCompletions) {
        TermOut() << tr("doctor.usage.not_chat") << "\n";
        return;
    }
    if (context.active_provider.empty() || lubancode::config::FindProvider(context.providers,
                                                                            context.active_provider) == nullptr) {
        TermOut() << tr("doctor.usage.no_provider") << "\n";
        return;
    }
    TermOut() << tr("doctor.usage.probing") << "\n";
    TermOut().flush();

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
        TermOut() << trf("doctor.effort.http_error", outcome.http_status > 0
                                                         ? std::to_string(outcome.http_status)
                                                         : std::string("-"))
                  << "\n"
                  << "  " << SanitizeProbeError(outcome.error) << "\n";
        return;
    }
    const bool supported = outcome.usage_reported;
    TermOut() << (supported ? tr("doctor.usage.supported") : tr("doctor.usage.unsupported")) << "\n";
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
        TermOut() << trf("doctor.usage.write_failed", saved.error()) << "\n";
        return;
    }
    config.stream_usage = supported;
    config.stream_usage_declared = true;
    TermOut() << trf("doctor.usage.written", context.active_provider, *saved) << "\n";
    TermOut().flush();
}

void PrintDoctorOverview(const DoctorContext& context) {
    const lubancode::config::Config& config = context.config;
    TermOut() << tr("doctor.overview.header") << "\n";
    TermOut() << tr("doctor.overview.effort")
              << (context.current_think.empty() ? tr("doctor.level.unset") : context.current_think) << "\n";
    TermOut() << tr("doctor.overview.declared")
              << (config.provider_think_levels.empty()
                      ? tr("doctor.overview.unverified")
                      : trf("doctor.overview.levels", config.provider_think_levels.size()))
              << "\n";
    TermOut() << tr("doctor.overview.cache")
              << [&] {
                    const auto observed = context.context_tracker.server_prefix_caching();
                    if (!observed.has_value()) {
                        return std::string(tr("doctor.cache.state.unverified"));
                    }
                    return *observed ? std::string(tr("doctor.cache.metrics_enabled"))
                                     : std::string(tr("doctor.cache.metrics_disabled"));
                }()
              << "\n";
    TermOut() << tr("doctor.overview.usage_hint") << "\n";
    TermOut().flush();
}

// /doctor agents:main 与各 agent type 的差异矩阵(规格"架构落点":能力
// 差异要打印得出来,不靠散落的 Register/不 Register 暗示)。矩阵材料从
// DoctorContext 来,没接(单测/旧调用方)就整节省略。i18n 中英成对。
void PrintAgentsMatrix(const DoctorContext& context) {
    TermOut() << tr("doctor.agents.header") << "\n";
    const auto tool_count = [](const lubancode::tools::ToolRegistry* registry) {
        return registry != nullptr ? registry->All().size() : std::size_t{0};
    };
    if (context.main_profile != nullptr) {
        TermOut() << trf("doctor.agents.budget",
                         context.main_profile->max_output_tokens.value_or(0),
                         context.main_profile->max_steps_per_turn == 0
                             ? std::string(tr("config.steps.unlimited"))
                             : std::to_string(context.main_profile->max_steps_per_turn),
                         context.main_profile->length_continuations)
                  << "\n";
        TermOut() << trf("doctor.agents.governance",
                         context.config.subagent.max_active.value_or(lubancode::config::kDefaultSubagentMaxActive),
                         context.config.subagent.max_depth.value_or(lubancode::config::kDefaultSubagentMaxDepth))
                  << "\n";
    }
    if (context.main_registry != nullptr) {
        TermOut() << trf("doctor.agents.row_main", tool_count(context.main_registry)) << "\n";
    }
    if (context.sub_registry != nullptr) {
        TermOut() << trf("doctor.agents.row_sub", tool_count(context.sub_registry)) << "\n";
    }
    if (context.explore_registry != nullptr) {
        TermOut() << trf("doctor.agents.row_explore", tool_count(context.explore_registry)) << "\n";
    }
    TermOut() << tr("doctor.agents.note") << "\n";
    // 子代理流诊断开关(规格"二、治'无法证明'"):这里提一句,下次用户问
    // "你怎么知道真入了账"就有现成的自查口。
    TermOut() << tr("doctor.agents.subagent_debug_log") << "\n";
    TermOut().flush();
}

// /doctor instructions(AGENTS.md 作用域单 P1-1):cwd 基线的完整诊断——
// 逐 source 账 + 分型诊断 + 字节帽的真实计费口径。Resolver 用会话那只
// (与写前闸同一份账);没接(单测/旧装配)按 SessionResolverOptions
// 现起一只,fallback 名单从当前配置来。
void PrintInstructionsDoctor(const DoctorContext& context) {
    std::unique_ptr<const lubancode::config::ProjectInstructionResolver> local_resolver;
    const lubancode::config::ProjectInstructionResolver* resolver = context.instruction_resolver;
    if (resolver == nullptr) {
        local_resolver = std::make_unique<const lubancode::config::ProjectInstructionResolver>(
            lubancode::config::SessionResolverOptions(context.config.project_doc_fallback_filenames));
        resolver = local_resolver.get();
    }
    const lubancode::config::InstructionChain chain =
        resolver->ResolveForPath(std::filesystem::current_path());

    TermOut() << context.theme.stats
              << "AGENTS.md 项目指令诊断(cwd 基线;/instructions path <路径> 看目标链)"
              << context.theme.reset << "\n";
    for (const std::string& line :
         lubancode::config::FormatInstructionChainLines(chain, resolver->max_bytes())) {
        TermOut() << line << "\n";
    }
    TermOut() << lubancode::config::InstructionBudgetAccountingNote(resolver->max_bytes()) << "\n";
    if (!context.config.project_doc_fallback_filenames.empty()) {
        std::string names;
        for (const std::string& name : context.config.project_doc_fallback_filenames) {
            names += (names.empty() ? "" : ", ") + name;
        }
        TermOut() << "fallback 名单(显式配置): " << names << "\n";
    } else {
        TermOut() << "fallback 名单: 未配置(只认 AGENTS.override.md / AGENTS.md)\n";
    }
    TermOut() << "缓存: 文档正文按 path + size + mtime 快筛,内容摘要落锤;"
                 "AGENTS.md 被外部编辑后,下一次解析(含写前闸)即重读,无需重启\n";
    const std::vector<std::string> diagnostics = lubancode::config::FormatInstructionDiagnosticLines(chain);
    if (diagnostics.empty()) {
        TermOut() << "诊断: 无\n";
    } else {
        TermOut() << "诊断:\n";
        for (const std::string& line : diagnostics) {
            TermOut() << line << "\n";
        }
    }
    TermOut().flush();
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
            // 可带轮数:/doctor cache probe 5(2-8,默认 2;多轮才分得出
            // 稳定/固定阈值/间歇)。认不得的按默认走,不当错误。
            std::string rounds_text;
            rest_input >> rounds_text;
            int rounds = 2;
            if (!rounds_text.empty()) {
                try {
                    rounds = std::stoi(rounds_text);
                } catch (...) {
                    rounds = 2;
                }
            }
            RunCacheProbe(context, rounds);
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
    if (subcommand == "instructions") {
        PrintInstructionsDoctor(context);
        return;
    }
    if (subcommand == "shell") {
        // 进程生命线单 P2:shell 方言、版本、profile、TTY 语义明牌。
        // 这些是产品边界,不是 bug——用户拿交互终端(Bash/Zsh/Fish、pwsh)
        // 的经验套 run_command,落差在这里摊开说清。
        TermOut() << context.theme.stats << "run_command 的 shell 环境:" << context.theme.reset << "\n";
        for (const auto& shell : lubancode::tools::ProbeShells()) {
            TermOut() << "  [" << shell.id << "] " << shell.executable;
            if (!shell.version.empty()) {
                TermOut() << "  版本: " << shell.version;
            }
            TermOut() << "\n";
            TermOut() << "    login shell: " << (shell.login_shell ? "是" : "否")
                      << "  加载 profile: " << (shell.profile_loaded ? "是" : "否")
                      << "  stdin TTY: " << (shell.stdin_is_tty ? "是" : "否")
                      << "  stdout TTY: " << (shell.stdout_is_tty ? "是" : "否") << "\n";
            if (!shell.notes.empty()) {
                TermOut() << "    " << shell.notes << "\n";
            }
        }
        TermOut() << "  shell 由操作系统提供,LubanCode 只拉进程,不随包附送 Bash;"
                     "也不把 sh 偷换成 Bash、不把 powershell 偷换成 pwsh。\n";
        TermOut().flush();
        return;
    }
    if (subcommand.empty()) {
        PrintDoctorOverview(context);
        return;
    }
    TermOut() << tr("doctor.usage.usage_line") << "\n";
    TermOut().flush();
}

// 命令分派注册制(会话终章):/doctor 的分派位。本地兼容端 Effort/前缀
// 缓存诊断——探针自己建临时 backend,与会话 backend 无关;stream_usage
// 探针写回 config 后这里顺手重建 real_backend,新能力下一次请求就带上。
CommandFlow HandleSlashDoctor(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    lubancode::app::DoctorContext doctor_context{*ctx.config,
                                                 ctx.config->providers,
                                                 *ctx.active_provider,
                                                 *ctx.current_model,
                                                 *ctx.current_think,
                                                 *ctx.theme,
                                                 *ctx.context_tracker,
                                                 *ctx.active_provider_write_path,
                                                 ctx.main_agent != nullptr ? &ctx.main_agent->runtime_profile() : nullptr,
                                                 ctx.registry,
                                                 ctx.sub_registry,
                                                 ctx.tool_runtime != nullptr ? ctx.tool_runtime->explore_registry()
                                                                             : nullptr,
                                                 ctx.instruction_resolver};
    HandleDoctorCommand(parsed.args, doctor_context);
    ctx.real_backend->Rebuild(*ctx.config);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
