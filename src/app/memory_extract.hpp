// 回合收尾的记忆抽取(0.30.x 候审箱第一期):外层回合结束后,把本轮
// 增量(用户消息、最终回答、结构化工具摘要)交给主模型做一次总结,顺手
// 产出去重候选与下一轮检索扩展词。抽取借当前主模型、严格 JSON、失败降级
// 不影响主会话——这套纯逻辑与请求拼装都住在这,交互会话只管接线。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <vector>

#include "agent/model_router.hpp"  // BackgroundCallAccounting(usage 出账)
#include "agent/sample_model.hpp"  // SampleResult(抽取侧收口的入参)
#include "api/backend.hpp"
#include "api/types.hpp"
#include "memory/project_memory.hpp"  // MemoryWriteReceiptSink(P0 写路回执)

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

// 抽取结果(回合总结 + 候选 + 检索扩展词)。
struct ProposedCandidate {
    std::string kind;         // fact | preference | feedback
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string confidence;   // user-stated | verified | inferred
    std::string occurred_at;  // 事件发生时间:材料里明确给出才填,否则空(不造假)
};

struct MemoryExtraction {
    std::string task_type;    // code | research | config | docs | other
    std::string summary;
    std::vector<std::string> retrieval_terms;
    std::vector<ProposedCandidate> candidates;
};

// 任务类型判定(用户基调 1:先推测目的再选总结提示词)。纯词法启发,不
// 打请求;user_text 是本轮用户消息,tool_names 是本轮调用过的工具名。
std::string ClassifyTaskType(const std::string& user_text, const std::vector<std::string>& tool_names);

// 把一个回合的消息增量压成给模型看的转写:用户/助手正文收全(各截
// 4 KiB),工具调用只留名字与紧凑入参,工具结果只留开头一小段;大段日志、
// 网页/MCP 原文不整包送抽取。max_bytes 是整段转写的字节上限。
std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes);

// 抽取提示词:基础契约(features/memory-summary-base.md)+ 分型侧重
// (features/memory-summary-<type>.md),用户目录可覆盖。task_type 认不出
// 时用 other。
std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type);

// 解析模型输出(容错:剥代码围栏、取首个 { 到末个 })。候选最多 3 条,
// 字段缺错的整条丢弃,不整份报错。
std::expected<MemoryExtraction, std::string> ParseExtractionJson(const std::string& text);

// 发一次抽取请求(同步,带看门狗取消)。失败只返回错误,调用方降级。
// reasoning_effort 非空时随请求带上(cheap 路由的档位);accounting 非空时
// 把这次调用的 usage/时长记进去(分角色记账,不混普通 turn 的账)。
// 采样走 agent::SampleModel 原语(批一·病四)。
std::expected<MemoryExtraction, std::string> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs,
                                                                 const std::string& reasoning_effort = std::string(),
                                                                 agent::BackgroundCallAccounting* accounting = nullptr);

// 采样结果的抽取侧收口:RunMemoryExtraction 与走 ModelRouterService::Sample
// 一站的调用方共用——失败回 message、空文回"抽取输出为空"、成功交解析。
std::expected<MemoryExtraction, std::string> FinishMemoryExtraction(const agent::SampleResult& sampled);

// ---------------------------------------------------------------------------
// 记忆写入调度单 P0(§六/§10):调度账。纯 instrumentation——以下每个
// 枚举、计数、事件都不改现行 every-turn 路一个字节的控制流;门控判定
// (§7)是 P1 的活,P0 只把枚举表立对。
// ---------------------------------------------------------------------------

// 用户正文的有效成分统计(§3.2 MeaningfulTextStats 的首折)。中文没有
// 天然空格,不能照抄 split(' ')>=3 的英文口径;P0 先立三项易得计数,
// code_token_count/only_acknowledgement/only_slash_command 留给 P1 的
// 门控判定。纯函数,UTF-8 感知。
struct MeaningfulTextStats {
    std::uint64_t unicode_scalar_count = 0;  // UTF-8 码点数(不含续字节)
    std::uint64_t cjk_char_count = 0;        // CJK 统一表意(含 Ext-A/兼容区)
    std::uint64_t latin_word_count = 0;      // ASCII 字母/数字连成的词数
    // P1 门控判定接线后才填(P0 恒缺省,typed event 不落键)。
    std::uint64_t code_token_count = 0;
    bool only_acknowledgement = false;
    bool only_slash_command = false;
};
MeaningfulTextStats ComputeMeaningfulTextStats(const std::string& text);

// 抽取触发器(§5.1 extract.mode 的影子账)。P0 现行路只有 every_turn;
// gated/batch/idle/compact/session_end 是 P3 的档位,名字先冻结。
enum class ExtractionTrigger {
    EveryTurn,  // 现行路:每个有新增 history 的合格回合同步跑一次
    // P3 起:gated 攒批路的水位/空闲/压缩/收口触发。
    BatchWatermark,
    IdleTimeout,
    BeforeCompact,
    SessionEnd,
};
const char* ExtractionTriggerName(ExtractionTrigger trigger);

// 门控决策(§6.1 extraction_gate_decision)。P0 两态:现行前置门过了就
// called、拦下就 skipped;P1 的 shadow/gated 判定沿用同一枚表。
enum class ExtractionDecision { Skipped, Called };
const char* ExtractionDecisionName(ExtractionDecision decision);

// 跳过原因(§7 的稳定 reason,§15 跨平台一致)。P0 在线的四条是现行
// ExtractTurnMemory 的既有前置门;P1/P3 的枚举名先冻结、判定后接。
enum class ExtractionSkipReason {
    // P0 在线(现行前置门,§2.1):
    Disabled,         // project_memory 空 / generate_enabled=false(§10.1 skipped_disabled)
    NoNewHistory,     // 本轮 history 没增长(§10.1 history_grew 的补集)
    EmptyTranscript,  // 增量转写去协议壳后为空
    PromptMissing,    // 抽取系统提示词拼不出(prompts 目录缺模块)
    // P1 起(§7 门控;P0 只立名,不产值):
    ExtractModeOff,
    AlreadyMutated,
    ShortText,
    AcknowledgementOnly,
    SlashCommandOnly,
    NoDurableSignal,
};
const char* ExtractionSkipReasonName(ExtractionSkipReason reason);

// 抽取失败的稳定码(§10.3 时延/失败账的 reason 枚举)。
std::string StableExtractErrorCode(const std::string& error);

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
    // 回合收尾账落袋:foreground_tail_ms = 回合收尾到抽取返回的墙钟
    //(§10.3);trajectory 在场时落 memory.extraction.assessed。
    void FinishTurn(std::int64_t foreground_tail_ms);

    // ---- 抽取观测点(ExtractTurnMemory 的前置门与收口;纯记账) ----
    void NoteExtractionSkipped(ExtractionSkipReason reason);
    void NoteHistoryGrew();  // 过了 history 前置门(§10.1 history_grew_turns)
    void NoteExtractionCalled();
    struct ExtractOutcome {
        bool ok = false;
        bool usage_reported = false;  // provider 没报 = token 三项不算数
        std::int64_t input_tokens = 0;
        std::int64_t output_tokens = 0;
        std::int64_t cached_tokens = 0;  // cache_read + cache_creation
        std::int64_t extract_wall_ms = 0;
        std::size_t review_candidates = 0;  // 进待审区的候选数
        std::size_t auto_written = 0;       // auto 档直写排队数
        std::string error_code;             // ok=false 时的稳定码
    };
    void NoteExtractionOutcome(const ExtractOutcome& outcome);

    // ---- memory::MemoryWriteReceiptSink(四路写路投递) ----
    void OnMemoryWriteReceipt(const memory::MemoryWriteReceipt& receipt) override;

    // 只读快照(诊断/后续接线;P0 不上 UI)。
    const ExtractionFunnel& funnel() const { return funnel_; }

private:
    void RecordAssessedLocked(std::int64_t foreground_tail_ms);
    void RecordReceiptLocked(const memory::MemoryWriteReceipt& receipt,
                             const std::string& turn_id);

    runtime::TrajectorySessionLedger* trajectory_ = nullptr;  // 空 = 不落盘
    mutable std::mutex mutex_;
    MemoryTurnState state_;
    bool turn_open_ = false;
    // called 之后的收口材料(FinishTurn 落袋)。
    bool extraction_called_ = false;
    ExtractOutcome pending_outcome_;
    ExtractionFunnel funnel_;
};

}  // namespace lubancode::app
