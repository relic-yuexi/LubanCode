// LubanBench-Tool(规格"基准"节):JSON 与 PTC 同模型、同题、temperature 0
// 各跑至少五轮的基准骨架。八类场景 + 每题指标 + JSON 报告。
//
// 两层跑法,如实交账:
//   离线层(P0/P1,进 ctest):手写"仿真脚本"当模型产出,过 PtcRunner +
//     假工具判卷——验证判卷尺、指标口径与 harness 本身,不冒充模型成绩;
//   在线层(手测清单):同模型 JSON/PTC 各五轮,须真 API key,本模块提供
//     题库与判卷,跑法见 docs/ptc.md。
//
// BFCL 子集(论文式):SingleToolShortParams/ChainDependency/Fanout 三类
// 的题面即 BFCL v4 的 simple/multi/chained/mirror 形状,离线层覆盖。

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "ptc/runner.hpp"

namespace lubancode::ptc {

// 八类场景(规格"基准"节编号一一对应)。
enum class BenchKind {
    SingleToolShortParams,  // 1. 单工具、短参数
    ChainDependency,        // 2. 2/5/10/20 步真依赖链
    FanoutIndependent,      // 3. 2/8/16/32/64/100 路独立 fan-out
    NeedleInHaystack,       // 4. 128 份无关工具 schema 下找 1..3 个目标
    LargeResultSummary,     // 5. 大结果落盘与摘要
    HalfFailures,           // 6. 一半调用失败、超时、拒权
    MixedReadOnlySideEffects,  // 7. 混合只读与副作用(P3 前:副作用占位记账)
    ExtensionCross,         // 8. hooks/Lua/MCP/LSP/技能/多智能体交叉
};

std::string ToString(BenchKind kind);

// 一道题:题面 + 假工具 schema + 期望账(判卷依据)。
struct BenchScenario {
    std::string id;
    BenchKind kind = BenchKind::SingleToolShortParams;
    std::string prompt;                       // 给模型的题面
    std::vector<api::ToolDefinition> tools;   // 假工具 schema(在线层给模型看)
    // 期望账:{"calls": [{"tool": "...", "input": {...}}...], "emit_probe": "..."}
    // calls 按序匹配(链题)/按多重匹配(fan-out 题);emit_probe 是 emit
    // 摘要里必须出现的子串。
    nlohmann::json expected;
    // 题面对应的"仿真脚本"(离线层用;空 = 这道题没有离线仿真)。
    std::string reference_script;
};

// 每题指标(规格"每题记"清单逐项)。
struct BenchMetrics {
    std::string scenario_id;
    std::string backend;         // "json" | "ptc"
    bool task_success = false;   // 任务成功率
    bool missed_calls = false;   // 漏调用
    double param_accuracy = 0.0; // 参数正确率(期望入参的命中占比)
    int model_rounds = 0;        // 模型请求轮数(离线层 = 1,在线层实记)
    int first_result_ms = 0;     // 首结果耗时
    int total_ms = 0;            // 总耗时
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    int duplicate_calls = 0;     // 工具重复调用数
    bool sandbox_covered = false;   // sandbox 覆盖率(跑在 OS 沙箱里)
    bool permission_covered = false;// 权限链覆盖(过完整链)
    bool hooks_covered = false;     // hooks 覆盖
    int side_effect_replays = 0;    // 失败后的副作用重复数(本版只读恒 0)
    int call_count = 0;

    nlohmann::json ToJson() const;
};

// 八类场景的题库骨架(P0/P1 各类至少一道;在线层可继续加密)。
std::vector<BenchScenario> BuildBenchScenarios();

// 判卷:拿 PtcRunResult 的调用账对照 expected,算指标。纯函数。
BenchMetrics JudgeRun(const BenchScenario& scenario, const PtcRunResult& run, bool sandbox_covered,
                      bool permission_covered, bool hooks_covered);

// 离线跑一道题:reference_script 过 PtcRunner + 按题工具造的假工具表
// (executor 由调用方给,通常接 JudgeRun 同一套假工具)。
BenchMetrics RunScenarioOffline(const BenchScenario& scenario, const PtcRunner::Options& options);

// 全部指标落 JSON 报告(给 /tmp 落盘比对用)。
std::string BenchReportJson(const std::vector<BenchMetrics>& runs);

}  // namespace lubancode::ptc
