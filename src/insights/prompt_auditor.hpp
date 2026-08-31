// Prompt Audit 三层审法(Token 账本单 §八/A3)。
//
// static  —— 审眼前配置:Prompt 模块五层来源/override 链、同文重复、
//            n-gram 重合、token 占比、工具描述与 schema 形状、动态段
//            位置、用户模块漂移。不碰 Journal,不冒充历史事实。
// runtime —— 审真实请求:Journal 里 model.request.prepared 的
//            request_snapshot_ref(manifest+toolset+形状)逐请求对账,
//            看哪些层在连续请求间变、稳定前缀断在哪、system 增长与
//            cache 命中的同向观察。
// outcome —— 审结果信号:工具终态/verification/outcome 摩擦(A4 的
//            FrictionClassifier 出证据,这里只挑与 prompt 相关的抬成
//            finding,措辞守 §8.3 的"不能越界说什么")。
//
// 口径铁律:
//   - 只摆事实不说教,主人裁决;语义类检查(§8.1 第 8/9 条)本地规则
//     只抓明显模式,confidence 一律 low、措辞带"疑似";
//   - prompt 正文绝不进 finding/报告——证据只落 hash、token、计数、
//     段名、事件引用;绝对路径不落,只落相对路径/段名;
//   - 每条 finding 能回到 source(段 id/hash)或 event(prepared 事件 id)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/prompt_assembler.hpp"  // PromptModuleSource(用户模块漂移账)
#include "agent/prompt_manifest.hpp"
#include "insights/finding.hpp"
#include "trajectory/event.hpp"

namespace lubancode::insights {

inline constexpr const char* kPromptAuditRuleVersion = "prompt-audit-v1";
inline constexpr const char* kPromptAuditReportSchema = "lubancode.prompt.audit";
inline constexpr int kPromptAuditReportSchemaVersion = 1;

// 一枚工具定义的审计视图(中性拷贝:insights 不 include tools 层)。
struct AuditToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;   // 空 json = 没给 schema
    std::string source_kind;       // builtin/mcp/lsp/plugin_lua/plugin_native/agent/deferred
    std::string source_instance;   // MCP server 名 / plugin id;可空
};

// static 的输入:全部来自当前进程内的真实拼装现场,不读 Journal。
struct StaticAuditInput {
    agent::PromptManifest manifest;  // ResolveFinalPrompt 产的那份
    // segment_id -> 渲染正文(内存账,只用于重复/n-gram/明显冲突测量,
    // 不进任何输出)。
    std::map<std::string, std::string> segment_texts;
    // 已剥注释的魂正文与模型指令正文(同上,只测量不输出)。
    std::string soul_text;
    std::string model_instructions_text;
    // 用户模块 vs 嵌入版的来源账(/prompt audit static 第 12 条)。
    std::vector<agent::PromptModuleSource> module_sources;
    std::vector<AuditToolDefinition> tools;
    std::int64_t context_budget_tokens = 0;  // 0 = 预算未知,占比类规则不判
};

// static 的段级事实账(报告正文,metadata only)。
struct PromptAuditFacts {
    std::int64_t system_tokens = 0;
    std::int64_t soul_tokens = 0;
    std::int64_t model_instructions_tokens = 0;
    std::int64_t tool_definition_tokens = 0;
    std::int64_t tool_count = 0;
    std::int64_t total_context_tokens = 0;   // 上面四项合计
    std::int64_t budget_tokens = 0;          // 0 = 未知
    struct SegmentFact {
        std::string segment_id;
        std::string role;
        std::string source_kind;
        std::int64_t tokens = 0;
        int order = 0;
        bool volatile_segment = false;
    };
    std::vector<SegmentFact> segments;  // 按 order 升序(拼装序,稳定)

    nlohmann::json ToJson() const;
};

// runtime 的一次请求视图:prepared 事件 + 可关联的 usage。
struct RuntimeRequestView {
    std::string run_id;
    std::string request_id;
    std::string purpose;  // 线上名;prepared 缺 = "unknown"
    std::string event_id; // prepared 事件 id(finding 回引)
    std::optional<agent::RequestSnapshotMetadata> snapshot;  // 解不开/缺席 = nullopt
    bool usage_reported = false;
    std::int64_t total_input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t output_tokens = 0;
    std::optional<int> cache_epoch;
};

// runtime 的输入:一场 session 的请求序列(按 stream 字典序、stream 内
// 出现序——读侧装配见 CollectRuntimeRequests)。
struct RuntimeAuditInput {
    std::string session_id;
    std::vector<RuntimeRequestView> requests;
};

// static 规则集。返回 finding(确定性次序);facts 非空时填段级事实账。
std::vector<Finding> AuditPromptStatic(const StaticAuditInput& input, PromptAuditFacts* facts);

// runtime 规则集(§8.2:只说发生了什么)。
std::vector<Finding> AuditPromptRuntime(const RuntimeAuditInput& input);

// 相邻请求间的层变化账(§8.2)。AuditPromptRuntime 内部同款;A4 的分析器
// 与功能信号也要吃,外露成纯函数。
struct RuntimeChangeSummary {
    std::int64_t comparable = 0;                  // 有 snapshot 的相邻对
    std::int64_t toolset_changes = 0;             // toolset_hash 变化次数
    std::int64_t prefix_changes = 0;              // stable_prefix_hash 变化(不限 epoch)
    std::int64_t prefix_breaks_same_epoch = 0;    // 同 epoch 内前缀变(本应追加)
    std::map<std::string, std::int64_t> segment_changes;  // 稳定段变化计数
};
RuntimeChangeSummary SummarizeRuntimeChanges(const std::vector<RuntimeRequestView>& requests);

// ---- 读侧装配 ----
// 把各 stream 的 prepared 事件折成 RuntimeRequestView,并按
// run_id+request_id+attempt 与 ProjectUsage 的 sample 对账(usage 缺席
// 如实标 usage_reported=false)。streams 形状与 IntegrityGate 的产出一致
// (run_id 字典序,事件按 seq 升序);此版无 IO,读盘壳在下面。
struct RuntimeRequestsRead {
    bool ok = false;
    std::string error_code;
    std::string message;
    std::string session_id;
    std::vector<RuntimeRequestView> requests;
    std::vector<std::string> warnings;
};
RuntimeRequestsRead CollectRuntimeRequestsFromStreams(
    const std::vector<std::pair<std::string, std::vector<trajectory::EventEnvelope>>>& streams);

// 认领一场 session 目录(stream 清单与 session_usage_reader 同一套),
// 逐条解析后委托 CollectRuntimeRequestsFromStreams。stream 坏/混版本 →
// 该 stream 点名排除(warnings),不出残账。目录不存在 → ok=false。
RuntimeRequestsRead CollectRuntimeRequests(const std::filesystem::path& session_dir);

}  // namespace lubancode::insights
