// /doctor:本地兼容端 Effort 与前缀缓存诊断(2026-08 单)。两个子命令:
//   /doctor effort [档位|unset]   发一只极小探针,报告 HTTP 状态、服务端
//                                  错误(清洗后)、请求里实际发送的档位、
//                                  usage 的 reasoning/output 拆账。
//   /doctor cache [probe|usage]   metrics_url 明配后读服务端 /metrics,报
//                                  前缀缓存四态;probe 再发两轮固定前缀
//                                  请求对账;usage 做 stream_usage 能力探针
//                                  并写回 provider 配置。
// 裸敲 /doctor 列两个子命令的现状摘要,不发任何请求。
//
// 诊断只对本地兼容端动手:probe/usage 两个要发模型请求的动作都挡在
// IsLoopbackUrl 或"metrics_url 已明配"这道闸后面,绝不擅自探公网 provider。
// 密钥与正文照旧打码:报告只摆参数名、档位值、token 数与错误摘要,不打
// Authorization、不打 system/messages 正文。

#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "agent/runtime_profile.hpp"
#include "cli/context_tracker.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {

// ---------------- 纯函数(单测钉住) ----------------

// Effort 探针请求:极小 max_tokens、固定一句 system、一句要"ok"的 user,
// reasoning_effort = level(空串 = 探"不发参数"那条路,字段整个缺席)。
api::Request BuildEffortProbeRequest(const std::string& model, const std::string& level);

// 按当前 wire 把请求体翻出来,报告"实际发送值"一行。chat wire 的参数名听
// provider 声明(think_param,空 = reasoning_effort);responses 报
// reasoning.effort;anthropic 报映射后的 thinking(budget_tokens)。返回人话,
// 如 "reasoning_effort = xhigh" / "未发送参数(请求体无此字段)"。
// extra_body 可能压过内置字段——如实报告被压后的值。
std::string DescribeRequestEffort(lubancode::config::Wire wire, const api::Request& request,
                                  const nlohmann::json& extra_body, const std::string& think_param);

// vLLM /metrics(Prometheus 文本)里的前缀缓存指标。哪个名字没出现就留
// nullopt,不拿 0 冒充"确实为零"——诊断端最忌把"没报告"与"报告为零"糊成
// 同一个 0。
struct PrefixCacheMetrics {
    std::optional<bool> enabled;                      // enable_prefix_caching label
    std::optional<std::int64_t> queries_total;        // prefix_cache_queries_total
    std::optional<std::int64_t> hits_total;           // prefix_cache_hits_total
    std::optional<std::int64_t> prompt_tokens_cached_total;  // prompt_tokens_cached_total
};
PrefixCacheMetrics ParsePrefixCacheMetrics(const std::string& text);

// usage 账的四态(缓存诊断单第 1 条):not_reported / disabled /
// enabled_no_hit / hit。同一个 0 不糊——disabled 只有服务端指标(或探针)
// 明说没启用才判,否则 0 命中如实报"未命中"。
enum class CacheObservation { NotReported, Disabled, EnabledNoHit, Hit };
CacheObservation ClassifyCacheObservation(bool usage_reported, std::int64_t cache_read,
                                          std::optional<bool> server_enabled);

// 两轮固定前缀探针:同 system、同一条固定前缀消息,只换最后一句。两轮的
// 公共前缀(system + 固定消息)在各自的 wire 里序列化后字节应当一致——
// prefix_fill_bytes 控制固定段的填充量(默认 2 KiB,够触发 vLLM 的前缀块)。
struct FixedPrefixProbePair {
    api::Request round1;
    api::Request round2;
    std::size_t designed_prefix_bytes = 0;  // 设计公共前缀(system+固定消息)字节数
};
FixedPrefixProbePair BuildFixedPrefixProbePair(const std::string& model,
                                               std::size_t prefix_fill_bytes = 2048);

// 两段文本的最长公共前缀字节数(报"前缀字节是否稳定"用)。
std::size_t CommonPrefixBytes(const std::string& a, const std::string& b);

// URL 是否指向本机(loopback host)。probe/usage 两个发请求的动作的安全闸:
// 公网 provider 不发,除非用户明配了 metrics_url(声明"这是我自己的端")。
bool IsLoopbackUrl(const std::string& url);

// 探针错误的清洗:压成单行、剥控制字符、超长截断(默认 2000)。服务端
// error body 原文保留在截断窗内,密钥不该出现在服务端响应里,请求侧的
// 正文本来就不进报告。
std::string SanitizeProbeError(const std::string& message, std::size_t max_chars = 2000);

// ---------------- 执行(IO) ----------------

// /doctor 的会话侧依赖打包。providers 传当前配置里的整份列表(stream_usage
// 探针写回要用);current_think/current_model 是会话状态;context_tracker
// 在 /doctor cache 拿到服务端结论后回写,状态栏与统计行跟着换措辞。
// provider_write_path:当前 active_provider 所在配置文件(项目级钉住时是
// 项目路径,否则 nullopt = 写全局);stream_usage 探针写回跟着这条走,别把
// 项目级 provider 的结论错写进全局。
struct DoctorContext {
    lubancode::config::Config& config;
    std::vector<lubancode::config::ProviderConfig>& providers;
    const std::string& active_provider;
    const std::string& current_model;
    const std::string& current_think;
    const lubancode::cli::Theme& theme;
    lubancode::cli::ContextTracker& context_tracker;
    std::optional<std::string> provider_write_path;
    // /doctor agents 的差异矩阵材料(规格"架构落点":能力差异要打印得出来,
    // 不靠散落的 Register 暗示)。可空:没接的调用方(单测)那节不打印。
    const lubancode::agent::AgentRuntimeProfile* main_profile = nullptr;
    const lubancode::tools::ToolRegistry* main_registry = nullptr;
    const lubancode::tools::ToolRegistry* sub_registry = nullptr;
    const lubancode::tools::ToolRegistry* explore_registry = nullptr;
};

void HandleDoctorCommand(const std::string& args, const DoctorContext& context);

// 命令分派注册制(会话终章):/doctor 的分派位(case 体原样搬自大 switch;
// 探针写回 config 后顺手重建会话 backend)。
struct SlashDispatchContext;
CommandFlow HandleSlashDoctor(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
