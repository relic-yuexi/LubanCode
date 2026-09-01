// FeatureSignalEngine(Token 账本单 §十二/A4)。
//
// 从一场 session 的事实账里找"可能少走弯路"的现成功能。只推荐仓库已经
// 支持的能力(/clear、project memory、skill、workflow、Prompt Profile、
// 延迟工具索引、子代理工具收窄);TODO 里的能力不推荐,不写成现成功能。
// 每条信号带证据与先决条件;先决不满足就不出(§12.2"先决条件"栏)。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "insights/finding.hpp"

namespace lubancode::insights {

inline constexpr const char* kFeatureSignalRuleVersion = "feature-signal-v1";

// 一场 session 的信号输入(分析器折好递进来;这里不读盘)。
struct FeatureSignalInput {
    std::string session_id;
    // 摩擦计数(类名 -> 次数;FrictionClassifier 的折叠账)。
    std::map<std::string, std::int64_t> friction_counts;
    // 验收种类计数(kind -> 次数):同类验证反复出现 = 步骤稳定。
    std::map<std::string, std::int64_t> verification_kinds;
    // usage 面。
    std::int64_t main_tokens = 0;      // 主会话 input+output
    std::int64_t subagent_tokens = 0;  // 子执行 input+output
    std::int64_t total_input_tokens = 0;
    std::int64_t tool_definition_tokens = 0;  // 有 manifest 的请求的工具定义估算均值
    std::int64_t prefix_breaks_same_epoch = 0;
    std::int64_t unexpected_miss_candidates = 0;
    std::int64_t cache_read_tokens = 0;
};

// 一条功能信号。
struct FeatureSignal {
    std::string signal_id;   // FS-01..
    std::string feature;     // 推荐的现成能力
    std::string summary;     // 一句话事实+动作
    std::string precondition;  // §12.2 先决条件(已满足的写法)
    std::vector<EvidenceItem> evidence;
};

// 检测(纯函数)。次序稳定:同输入同输出。id 按规则钉死(FS-01 验证固化、
// FS-02 工具面收窄、FS-03 cache 前缀、FS-04 子代理收窄)——不是场内
// 序号:同一条规则跨场同名,A5 聚合层才能按 id 汇总(单子 A4 行"映射落
// FS-01–FS-04"的钉法)。只开一条时也用本命 id,不从 1 重编。
std::vector<FeatureSignal> DetectFeatureSignals(const FeatureSignalInput& input);

// 信号静态目录(A5 汇总层用):规则钉死的 id -> 现成能力与先决条件,
// 文案与 DetectFeatureSignals 同源,不另立门户。
struct FeatureSignalCatalogEntry {
    std::string signal_id;
    std::string feature;
    std::string precondition;
    std::string action;  // 建议节的一句话动作(只指现成功能)
};
const std::vector<FeatureSignalCatalogEntry>& FeatureSignalCatalog();
std::optional<FeatureSignalCatalogEntry> FindFeatureSignal(const std::string& signal_id);

}  // namespace lubancode::insights
