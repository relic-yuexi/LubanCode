// /doctor:本地兼容端 Effort 与前缀缓存诊断(2026-08 单 + 真实实测问题单
// 问题 9 的公网确认门)。两个子命令:
//   /doctor effort [档位|unset]   发一只极小探针,报告 HTTP 状态、服务端
//                                  错误(清洗后)、请求里实际发送的档位、
//                                  usage 的 reasoning/output 拆账。
//   /doctor cache [probe [N]|usage]
//                                  metrics_url 明配后读服务端 /metrics,报
//                                  前缀缓存四态;probe 发 N 轮固定前缀请求
//                                  对账(默认 2,上限 8),出 usage、wire 公共
//                                  前缀字节与分型;usage 做 stream_usage
//                                  能力探针并写回 provider 配置。
// 裸敲 /doctor 列两个子命令的现状摘要,不发任何请求。
//
// 探针的发送面:本地回环端与明配 metrics_url 的端照旧直发;公网 provider
// 走一次性确认门(AllowCacheProbeRun)——先披露轮数、token 上限与端点,
// 用户答应才发,不偷偷烧 token。密钥与正文照旧打码:报告只摆参数名、档位
// 值、token 数与错误摘要,不打 Authorization、不打 system/messages 正文、
// 不打带查询参数的完整 URL。

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
#include "config/project_instructions.hpp"  // ProjectInstructionResolver:/doctor instructions
#include "runtime/trajectory_session.hpp"    // /doctor trajectory 的账本口(P0-4)
#include "telemetry/service.hpp"  // /doctor telemetry 的状态面(T1)
#include "tools/registry.hpp"

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

// ---------------- 纯函数(单测钉住) ----------------

// Effort 探针请求:极小 max_tokens、固定一句 system、一句要"ok"的 user,
// reasoning_effort = level(空串 = 探"不发参数"那条路,字段整个缺席)。
// max_tokens 可覆写:effort 诊断给推理优先模型留足正文预算
//(kEffortProbeBudgetTokens),64 那档只留给不在乎思考的极小探针。
api::Request BuildEffortProbeRequest(const std::string& model, const std::string& level,
                                     std::optional<int> max_tokens = std::nullopt);

// effort 诊断的三回对照(MiniCPM5 真机巡检单 P1):单回 HTTP 2xx 压根儿
// 不能说档位有效——unset/none/high 三档范围交叠的现场就是明证。每档至少
// 重复 kEffortProbeRepeats 回,四账分开:HTTP 接受、thinking 是否产出、
// 正文是否产出、终止原因与分布。探针预算被思考耗尽(stop = max_tokens
// 且正文 0)只判 inconclusive,不把 text=0 当支持或不支持。
inline constexpr int kEffortProbeRepeats = 3;
inline constexpr int kEffortProbeBudgetTokens = 1024;

// 一回探针的收账(纯函数的输入侧):http_ok = HTTP 2xx 且无传输错误。
struct EffortProbeRoundResult {
    bool http_ok = false;
    std::int64_t thinking_chars = 0;
    std::int64_t text_chars = 0;
    std::string stop_reason;
};

// 三回对照的聚合报告(纯函数,单测直接钉):返回若干行人话,依次是
// HTTP 接受、thinking 产出、正文产出、终止原因分布,末尾跟判词
//(inconclusive / none 档仍产出思考 / 观察到的事实陈述)。空表 = 调用方
// 没发探针,返回一行"未发出"。off_requested = 探针发的是关闭档(none/
// off 类):有效回里仍见思考时,判词明说"关闭请求被 2xx 收下但未被端点
// 证实"——vLLM anthropic 面 thinking.type=disabled 被无视就是这形状
//(勘察单 P1 补账),别让"收下了"糊成"生效了"。
std::vector<std::string> SummarizeEffortProbeRounds(const std::vector<EffortProbeRoundResult>& rounds,
                                                    bool off_requested = false);

// 按当前 wire 把请求体翻出来,报告"实际发送值"一行。chat wire 的参数名听
// provider 声明(think_param,空 = reasoning_effort);responses 报
// reasoning.effort;anthropic 报映射后的 thinking(budget_tokens)。返回人话,
// 如 "reasoning_effort = xhigh" / "未发送参数(请求体无此字段)"。
// extra_body 可能压过内置字段——如实报告被压后的值。
std::string DescribeRequestEffort(lubancode::config::Wire wire, const api::Request& request,
                                  const nlohmann::json& extra_body, const std::string& think_param);

// vLLM /metrics(Prometheus 文本)里的前缀缓存指标。哪个名字没出现就留
// nullopt,不拿 0 冒充"确实为零"——诊断端最忌把"没报告"与"报告为零"糊成
// 同一个 0。queries/hits 两项认两代名字:v0 引擎的 prefix_cache_* 与
// v1 引擎的 gpu_prefix_cache_* / cpu_prefix_cache_*(v1 把缓存按层拆开,
// 旧名缺席时按 gpu+cpu 合并读;两代同名在场以 v0 总数为准)。
// num_requests_running/waiting 是 vLLM 常见负载 gauge,不是缓存指标——
// 读数行带一句现场语境,缺席照旧 nullopt。
struct PrefixCacheMetrics {
    std::optional<bool> enabled;                      // enable_prefix_caching label
    std::optional<std::int64_t> queries_total;        // prefix_cache_queries_total(或 gpu_+cpu_)
    std::optional<std::int64_t> hits_total;           // prefix_cache_hits_total(或 gpu_+cpu_)
    std::optional<std::int64_t> prompt_tokens_cached_total;  // prompt_tokens_cached_total
    std::optional<std::int64_t> num_requests_running;  // vllm:num_requests_running
    std::optional<std::int64_t> num_requests_waiting;  // vllm:num_requests_waiting
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

// ---- cache probe 多轮与分型(真实实测问题单问题 9)-----------------------

// N 轮固定前缀探针组(问题 9):每轮同 system、同固定前缀消息,只换最后
// 一句("Round i. Reply with exactly: ok")。rounds 钳在 [2, kCacheProbeMaxRounds]。
// designed_prefix_tokens 按探针填充段 ASCII 4 字符/token 估,供分型比对。
struct FixedPrefixProbeSet {
    std::vector<api::Request> requests;
    std::size_t designed_prefix_bytes = 0;
    std::int64_t designed_prefix_tokens = 0;
};
FixedPrefixProbeSet BuildFixedPrefixProbes(const std::string& model, int rounds,
                                            std::size_t prefix_fill_bytes = 2048);

// 探针的 token 上限(保守写死,不许现场加码):输入按"填充段全量折 token
// 再放一倍余量"给 4096/轮,输出 32/轮(探针请求 max_tokens 就用这个数)。
// 轮数上限 8——确认门给用户看的预计上限全从这里算。
inline constexpr std::int64_t kCacheProbeInputTokenCapPerRound = 4096;
inline constexpr std::int64_t kCacheProbeOutputTokenCapPerRound = 32;
inline constexpr int kCacheProbeMaxRounds = 8;

// 一轮探针的收账(分型的输入侧):http_ok = HTTP 2xx 且无传输错;
// usage_reported = provider 真回了 usage(没回不冒充 0);cache_read 是
// cached_tokens 命中量,total_input 是完整输入(非缓存+缓存读)。
struct CacheProbeRoundResult {
    bool http_ok = false;
    bool usage_reported = false;
    std::int64_t cache_read = 0;
    std::int64_t total_input = 0;
};

// 多轮探针分型(问题 9):连跑多组固定前缀,区分——
//   StableHit        后续轮都命中,且命中量吃满设计前缀(稳定命中);
//   FixedQuantumHit  后续轮都命中,但命中量恒定且明显低于设计前缀
//                    (固定阈值命中,如上游恒只缓存 1024 token 的块);
//   IntermittentMiss 后续轮有的命中有的零,或命中量在抖(间歇 miss——同
//                    epoch 命中率抖动的上游侧形状);
//   NoHit            报了 usage 的轮全零命中(完全未见命中);
//   NotReported      一轮都没回 usage(无法判定,不猜)。
// 首轮按惯例是写缓存(冷启动 miss 不算上游的错):判命中形状看后续轮,
// 只有首轮可用时退回首轮自己。只统计 http_ok 且真报了 usage 的轮;
// coverage_threshold_percent 是"算吃满前缀"的覆盖比(默认 90)。
enum class CacheProbeVerdict { NotReported, NoHit, FixedQuantumHit, StableHit, IntermittentMiss };
CacheProbeVerdict ClassifyCacheProbeRounds(const std::vector<CacheProbeRoundResult>& rounds,
                                            std::int64_t designed_prefix_tokens,
                                            int coverage_threshold_percent = 90);
std::string CacheProbeVerdictLabel(CacheProbeVerdict verdict);

// 确认门(问题 9):公网 provider 不再一律拒发,改成一次性确认——先向
// 用户披露要发几枚请求、预计 token 上限与目标端点,答应才发。
//   loopback 或明配 metrics_url:直接跑(旧安全闸的放行面不变);
//   否则须 consent = true;nullopt(还没问/没答)与 false 一律不放行。
// 纯函数,单测钉死"未确认不发"。
bool AllowCacheProbeRun(bool loopback, bool has_metrics_url, std::optional<bool> consent);

// 确认答句的判定:y/yes/是/好 -> true;n/no/否/不 -> false;空与认不得的
// 一律 nullopt(不算同意,也不算拒绝——调用方按不放行处理)。
std::optional<bool> ParseProbeConsentAnswer(const std::string& answer);

// 端点描述(日志纪律):只留 scheme://host[:port][/路径],剥掉 query 与
// fragment——确认门明写发去哪,但不把带 key 的查询参数亮到屏上/日志里。
std::string DescribeEndpointForDisclosure(const std::string& url);

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
    // /doctor instructions(AGENTS.md 作用域单 P1-1):与写前闸同一只
    // Resolver。可空:没接的调用方按 SessionResolverOptions 现起一只。
    const lubancode::config::ProjectInstructionResolver* instruction_resolver = nullptr;
    // Token 账本单 A1(旁路落账):flag 开的会话递账本,effort/cache 探针
    // 各铸旁路桥落 Journal(purpose=doctor_probe)。空 = 没接轨迹/单测,
    // 一个探针请求一笔不落,行为与从前一致。wire 是桥 identity 的渠道名
    //(与主 turn 桥同源)。
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
    std::string trajectory_wire;
    // /doctor trajectory(P0-4 §13.1):flag 开的会话的轨迹账本。可空 =
    // 轨迹没开,那节明说"轨迹未开"。(与上 trajectory 同一对象,只读视角。)
    const lubancode::runtime::TrajectorySessionLedger* trajectory_ledger = nullptr;
    // /doctor telemetry(端云协同可观测单 T1):本地遥测服务的状态面。
    // 可空 = 遥测未装配(默认关),那节只说未开,不发任何请求。
    const lubancode::telemetry::TelemetryService* telemetry_service = nullptr;
    // /doctor insights(Token 账本单 A5):报告仓主目录与当前 workspace 的
    // sessions 根。空 = 没接(单测/旧装配),那节明说。
    std::optional<std::string> home_lubancode;
    std::optional<std::filesystem::path> insights_sessions_root;
};

void HandleDoctorCommand(const std::string& args, const DoctorContext& context);

// 命令分派注册制(会话终章):/doctor 的分派位(case 体原样搬自大 switch;
// 探针写回 config 后顺手重建会话 backend)。
struct SlashDispatchContext;
CommandFlow HandleSlashDoctor(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
