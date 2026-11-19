// 回合收尾的记忆抽取(0.30.x 候审箱第一期):外层回合结束后,把本轮
// 增量(用户消息、最终回答、结构化工具摘要)交给主模型做一次总结,顺手
// 产出去重候选与下一轮检索扩展词。抽取借当前主模型、严格 JSON、失败降级
// 不影响主会话——这套纯逻辑与请求拼装都住在这,交互会话只管接线。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/memory_extraction.hpp"
#include "agent/memory_learning_gates.hpp"
#include "agent/model_router.hpp"  // BackgroundCallAccounting(usage 出账)
#include "agent/sample_model.hpp"  // SampleResult(抽取侧收口的入参)
#include "api/backend.hpp"
#include "api/types.hpp"
#include "memory/project_memory.hpp"  // MemoryWriteReceiptSink(P0 写路回执)

namespace lubancode::runtime {
class TrajectorySessionLedger;
}
namespace lubancode::trajectory::v3 {
class V3Writer;
}

namespace lubancode::app {

// Source-compatible host facade; values and parser belong to the shared engine.
using agent::memory_extraction::kMemoryExtractMaxTokens;
using agent::memory_extraction::kMemoryExtractTimeoutSecs;
using agent::memory_extraction::kExtractionNoOffset;
using agent::memory_extraction::kMaxCandidateContentBytes;
using ProposedCandidate = agent::memory_extraction::ProposedCandidate;
using MemoryExtraction = agent::memory_extraction::MemoryExtraction;
using ExtractionErrorCode = agent::memory_extraction::ExtractionErrorCode;
using ExtractionError = agent::memory_extraction::ExtractionError;
const char* ExtractionErrorCodeName(ExtractionErrorCode code);

// 任务类型判定(用户基调 1:先推测目的再选总结提示词)。纯词法启发,不
// 打请求;user_text 是本轮用户消息,tool_names 是本轮调用过的工具名。
std::string ClassifyTaskType(const std::string& user_text, const std::vector<std::string>& tool_names);

// 本地压缩本轮材料：用户正文 2 KiB、最终助手答复 3 KiB、最近六次工具
// 摘录共 2 KiB。工具参数只留定位字段，不发补丁/文件正文。忽略中间答复、
// 思考和图片；max_bytes 是整段转写的严格字节上限，不含系统提示词。
std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes);

// 抽取提示词:基础契约(features/memory-summary-base.md)+ 分型侧重
// (features/memory-summary-<type>.md),用户目录可覆盖。task_type 认不出
// 时用 other。
std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type);

// 解析模型输出。先验整段 UTF-8,再按明确规则收 JSON:纯 JSON、单层代码
// 围栏、无歧义的前后说明(首个 { 之前不含 {,配对 } 之后无剩余内容);多
// 对象、字符串外悬空花括号、半截对象一律拒绝,不选一个碰运气。字段合同
// (P0-B):顶层必须 object;task_type/summary 必填 string(summary 非空);
// 已声明字段类型错(null/数字/数组/对象)拒绝整次并在错误里带字段路径;
// 无效业务候选(kind 不在枚举、title/content 空、正文超预算)沿既有规则
// 跳过该条,禁止静默类型转换。候选最多 3 条。
std::expected<MemoryExtraction, ExtractionError> ParseExtractionJson(const std::string& text);

// 发一次抽取请求(同步,带看门狗取消)。失败只返回错误,调用方降级。
// reasoning_effort 非空时随请求带上(cheap 路由的档位);accounting 非空时
// 把这次调用的 usage/时长记进去(分角色记账,不混普通 turn 的账)。
// cancel/boundary_recorder 是回合总结异步化单添的口:外部取消链(会话
// 拆除/换代的 RequestCancel)与旁路桥(轨迹 Journal 的
// purpose=memory_extract 落账)原先只在同步前台路拼,后台执行器同一条
// 路也要走——默认空,旧行为不变。
// 采样走 agent::SampleModel 原语(批一·病四)。
std::expected<MemoryExtraction, ExtractionError> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs,
                                                                 const std::string& reasoning_effort = std::string(),
                                                                 agent::BackgroundCallAccounting* accounting = nullptr,
                                                                 agent::LoopBoundaryRecorder* boundary_recorder = nullptr,
                                                                 const std::atomic<bool>* cancel = nullptr);

// 采样结果的抽取侧收口:RunMemoryExtraction 与走 ModelRouterService::Sample
// 一站的调用方共用——失败回 transport_failed、空文回 empty_output、已知
// 截断结束原因(max_tokens/length 一族)回 output_truncated,成功交解析。
// 结束原因未知/缺失不据此判死:照走解析,诊断里单列 stop_reason 原值。
std::expected<MemoryExtraction, ExtractionError> FinishMemoryExtraction(const agent::SampleResult& sampled);

// 抽取输出的字段合同(SampleModel.output_schema 本地复检用,与
// ParseExtractionJson 的判型同一份合同;候选内部字段的合同在解析函数里
// 显式判型——公共校验器不递归嵌套)。设这份不等于 provider 结构化输出
// 接通:api::Request 没有 output_schema 字段,wire 侧约束待批六再议。
const nlohmann::json& MemoryExtractionOutputSchema();

// ---------------------------------------------------------------------------
// 记忆写入调度单 P0(§六/§10):调度账。P0 批纯 instrumentation——
// 枚举、计数、事件不改现行 every-turn 路一个字节的控制流;P1 批起
// 门控上线：必跳层、同轮去重、耐久信号都在请求前执行。
// ---------------------------------------------------------------------------

// 用户正文的有效成分统计(§3.2 MeaningfulTextStats)。中文没有天然
// 空格,不能照抄 split(' ')>=3 的英文口径。P0 立三项计数,P1 补全后三
// 项(代码记号/纯确认/纯命令),六项一并算齐、一并落账。纯函数,
// UTF-8 感知,同一冻结输入跨平台一致(§15)。
// Host facade names refer to the same pure engine values and functions.
using agent::memory_learning_gates::MeaningfulTextStats;
using agent::memory_learning_gates::ComputeMeaningfulTextStats;
using agent::memory_learning_gates::ExtractionTrigger;
using agent::memory_learning_gates::ExtractionTriggerName;
using agent::memory_learning_gates::ExtractionDecision;
using agent::memory_learning_gates::ExtractionDecisionName;
using agent::memory_learning_gates::ExtractionSkipReason;
using agent::memory_learning_gates::ExtractionSkipReasonName;
using agent::memory_learning_gates::PassesMinimumTextGate;
using agent::memory_learning_gates::EvaluateMustSkipTextGate;
using agent::memory_learning_gates::EvaluateDurableSignals;
using agent::memory_learning_gates::EvaluateTurnDurableSignals;

// shadow 开关(§7.2):环境变量 LUBANCODE_MEMORY_GATE_SHADOW 置
// 1/true/on 才评耐久信号,默认关——typed event 与 P0 同形,要量漏判再
// 开。配置文件轴是 P2 的活,这里只认环境变量。
bool MemoryGateShadowEnabled();

// User intent plus tool-backed assistant conclusions; assistant prose alone
// cannot invent an explicit user preference or a request to remember.
// EvaluateTurnDurableSignals is imported from the shared engine above.

// 抽取失败的稳定码(§10.3 时延/失败账的 reason 枚举)。
// 结构化版(P0-A 起):六类新码 + route_miss;旧文案版保留——旧账与旧
// 调用方(字符串前缀路)继续可用,parse_failed 是 syntax_invalid/
// utf8_invalid/schema_invalid 三类的旧统称。
std::string StableExtractErrorCode(const ExtractionError& error);
std::string StableExtractErrorCode(const std::string& error);

// 修复单 §五 D:assessed 事件里的"自动直写排队数"读侧统一口。新版账写
// autoQueued(v3)/auto_queued(v2);旧账的 autoWritten/auto_written 是同一
// 个数的旧名(排队计数,不是落盘计数),读侧兼容解释,不回改历史账。
// 两个键都在时认新键;都没有给 0。
std::uint64_t AutoQueuedFromAssessedPayload(const nlohmann::json& payload);

// 一场会话的调度漏斗(§10.1"每场至少聚合")。P0 在线的计数器填得出;
// P1/P3 的计数器先立在表里恒 0,接线那批才动。
struct ExtractionFunnel {
    std::uint64_t outer_user_turns = 0;
    std::uint64_t history_grew_turns = 0;
    std::uint64_t eligible_turns = 0;      // = extract_batches(P0 一轮一发)
    std::uint64_t extract_batches = 0;
    std::uint64_t extract_failures = 0;
    std::uint64_t skipped_disabled = 0;
    std::uint64_t skipped_no_new_history = 0;
    std::uint64_t skipped_empty_transcript = 0;
    std::uint64_t skipped_prompt_missing = 0;
    // P1 接线:短文本/纯确认/纯命令/同轮已写/无耐久信号。
    std::uint64_t skipped_short = 0;
    std::uint64_t skipped_ack = 0;
    std::uint64_t skipped_command = 0;
    std::uint64_t skipped_already_mutated = 0;
    std::uint64_t skipped_no_durable_signal = 0;
    // P3 接线:攒批缓冲。
    std::uint64_t buffered_turns = 0;
};

// 本轮写入账(§6.1 MemoryTurnState)。只活在运行时与 typed event 里,
// 不进用户 prompt。successful_*_ids 在 P0 记的是"成功排队的 job"
//(outcome=queued);落盘与否是 worker 的 lifecycle 账,这里不冒充。
struct MemoryTurnState {
    std::string session_id;
    std::string turn_id;
    MeaningfulTextStats user_text_stats;
    std::vector<std::string> successful_save_ids;      // 排队成功的 save job
    std::vector<std::string> successful_forget_ids;    // 排队成功的 forget job
    std::vector<std::string> accepted_candidate_ids;   // accept 成功的候选 id(经 job 名)
    std::vector<std::string> rejected_write_codes;     // 被拒写路的稳定码
    std::vector<std::string> durable_signal_reasons;   // P1(§7.2)起填
    ExtractionDecision extraction_gate_decision = ExtractionDecision::Skipped;
    ExtractionSkipReason extraction_gate_reason = ExtractionSkipReason::Disabled;
};

// 回合级调度账本 + 写路回执收件口。会话控制器持一只,活一场会话:
//   - BeginTurn/NoteXxx/FinishTurn 由回合收尾路调用(观测点);
//   - OnMemoryWriteReceipt 由 ProjectMemory 的四路写路投递(可能落在
//     回合内的工具执行里,内部一把小锁保账不撕);
//   - trajectory 在场时落两枚 typed event(memory.extraction.assessed /
//     memory.write.receipted),不在场(账本 flag 关/单测)只记内存账,
//     一笔不落盘,行为与从前一致。
// 落账失败只吞稳定码(诊断口径同 MemoryLedgerBridge),不影响主流程。
class MemoryTurnLedger final : public memory::MemoryWriteReceiptSink {
public:
    explicit MemoryTurnLedger(runtime::TrajectorySessionLedger* trajectory);
    ~MemoryTurnLedger() override;

    MemoryTurnLedger(const MemoryTurnLedger&) = delete;
    MemoryTurnLedger& operator=(const MemoryTurnLedger&) = delete;

    // ---- 回合生命周期(回合收尾路调;主线程) ----
    // session_id 可空(flag 关的会话没有轨迹场号)。
    void BeginTurn(std::string session_id, std::string turn_id, const std::string& user_text);
    // 回合收尾账落袋:foreground_tail_ms = 回合收尾到抽取终态的墙钟
    //(§10.3;异步化后口径并档:门拦回合 = 前台门的耗时,门过回合见
    // SettleSuspendedTurn——都是"抽取路径的墙钟尾巴",离线一张表可比,
    // 不混入等待用户输入的时间)。trajectory 在场时落
    // memory.extraction.assessed。
    void FinishTurn(std::int64_t foreground_tail_ms);
    // ---- 悬账(回合总结异步化单:门过起飞的回合,收口不等网络) ----
    struct ExtractOutcome;  // 嵌套类型后文才完整定义,悬账口先用(clang 严)
    // 收口悬账:回合账(state_ 与门决策)留着,轮号记进悬账槽;turn 关
    // (写路回执不再挂轮号,对齐 FinishTurn 的回合间口径)。迟到收账
    //(SettleSuspendedTurn)补 outcome 落袋;下一轮 BeginTurn 先到,悬账
    // 以 aborted 口径落袋(decision=Called 而 outcome 缺席——真账,不编
    // 数字),不阻塞新轮。
    void SuspendTurn();
    // 迟到收账:对上悬账轮号才补 outcome 并落 assessed;对不上(回合已
    // 翻篇/换代弃过)不动,只把真失败数进漏斗。settle_wall_ms = 回合
    // 收口到收账完成的墙钟(并档口径见 FinishTurn)。返回是否真落了袋。
    bool SettleSuspendedTurn(const std::string& turn_id, std::int64_t settle_wall_ms,
                             const ExtractOutcome* outcome);
    // 换代弃账(/clear、/resume):悬账清掉不落盘——旧场的回合账不写进
    // 新场的卷里,宁缺毋滥。漏斗计数在场,漏斗不受影响。
    void AbandonSuspendedTurn();

    // ---- 抽取观测点(ExtractTurnMemory 的前置门与收口;纯记账) ----
    void NoteExtractionSkipped(ExtractionSkipReason reason);
    void NoteHistoryGrew();  // 过了 history 前置门(§10.1 history_grew_turns)
    // P1(§7.1 案二):本轮是否已有成功的 save/forget/accept(§6.2 回执
    // 账)。ExtractTurnMemory 照它收手——同轮去重是 P1 的实时行为变更,
    // 这只读口是判定的唯一依据。
    bool turn_mutated() const;
    // P1(§7.1):本轮增量里有没有工具证据(工具调用或工具结果)。随
    // 转写扫描顺手记,进 assessed 事件——ack 门与耐久信号的"须有工具
    // 证据"靠它离线复算。
    void NoteGateContext(bool has_tool_evidence);
    // P1(§7.2 shadow):耐久信号判断落账。空表也标记"评过了"(shadow
    // 开着、一条没命中);只记账,不改任何控制流。
    void NoteDurableSignals(const std::vector<std::string>& reasons);
    void NoteExtractionCalled();
    struct ExtractOutcome {
        bool ok = false;
        bool usage_reported = false;  // provider 没报 = token 三项不算数
        std::int64_t input_tokens = 0;
        std::int64_t output_tokens = 0;
        std::int64_t cached_tokens = 0;  // cache_read + cache_creation
        std::int64_t extract_wall_ms = 0;
        std::size_t review_candidates = 0;  // 进待审区的候选数
        std::size_t auto_queued = 0;        // auto 档直写排队数(修复单 §五 D:只数排队,不冒充入库)
        std::string error_code;             // ok=false 时的稳定码
    };
    void NoteExtractionOutcome(const ExtractOutcome& outcome);

    // ---- memory::MemoryWriteReceiptSink(四路写路投递) ----
    void OnMemoryWriteReceipt(const memory::MemoryWriteReceipt& receipt) override;

    // 只读快照(诊断/后续接线;P0 不上 UI)。
    const ExtractionFunnel& funnel() const { return funnel_; }

private:
    // Assessment identity is frozen separately from inter-turn receipt scope.
    void RecordAssessedLocked(std::int64_t foreground_tail_ms, const std::string& trigger_turn_id);
    void RecordReceiptLocked(const memory::MemoryWriteReceipt& receipt,
                             const std::string& turn_id);
    // v3 场的写口(取消误报 ESC 单 Bug 2):typed 事件 memory.extraction.
    // assessed / memory.write.receipted,载荷 camelCase;落不稳只吞(诊断
    // 口径与 v2 同一条:调度账不许拖垮回合收尾)。
    void RecordAssessedV3Locked(trajectory::v3::V3Writer& writer, std::int64_t foreground_tail_ms,
                                const std::string& trigger_turn_id);
    void RecordReceiptV3Locked(trajectory::v3::V3Writer& writer, const memory::MemoryWriteReceipt& receipt,
                               const std::string& turn_id);

    runtime::TrajectorySessionLedger* trajectory_ = nullptr;  // 空 = 不落盘
    mutable std::mutex mutex_;
    MemoryTurnState state_;
    bool turn_open_ = false;
    // 悬账轮号(回合总结异步化单):SuspendTurn 记下,Settle/Abandon 或
    // 下一轮 BeginTurn 清掉。空 = 没有悬着的回合账。
    std::string suspended_turn_id_;
    // called 之后的收口材料(FinishTurn 落袋)。
    bool extraction_called_ = false;
    ExtractOutcome pending_outcome_;
    // P1 门控观测:工具证据在场否(进 assessed 事件,复算用)。
    bool gate_context_noted_ = false;
    bool turn_has_tool_evidence_ = false;
    // P1 shadow:耐久信号评过了没(评过才落 shadow_gate 键;关着不落,
    // 事件与 P0 同形)。
    bool shadow_evaluated_ = false;
    ExtractionFunnel funnel_;
};

}  // namespace lubancode::app
