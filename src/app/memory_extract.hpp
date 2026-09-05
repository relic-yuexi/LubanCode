// 回合收尾的记忆抽取(0.30.x 候审箱第一期):外层回合结束后,把本轮
// 增量(用户消息、最终回答、结构化工具摘要)交给主模型做一次总结,顺手
// 产出去重候选与下一轮检索扩展词。抽取借当前主模型、严格 JSON、失败降级
// 不影响主会话——这套纯逻辑与请求拼装都住在这,交互会话只管接线。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
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
// 记忆写入调度单 P0(§六/§10):调度账。P0 批纯 instrumentation——
// 枚举、计数、事件不改现行 every-turn 路一个字节的控制流;P1 批起
// §7 门控上线(必跳层 + 同轮去重是真闸,§7.2 耐久信号走 shadow)。
// ---------------------------------------------------------------------------

// 用户正文的有效成分统计(§3.2 MeaningfulTextStats)。中文没有天然
// 空格,不能照抄 split(' ')>=3 的英文口径。P0 立三项计数,P1 补全后三
// 项(代码记号/纯确认/纯命令),六项一并算齐、一并落账。纯函数,
// UTF-8 感知,同一冻结输入跨平台一致(§15)。
struct MeaningfulTextStats {
    std::uint64_t unicode_scalar_count = 0;  // UTF-8 码点数(不含续字节)
    std::uint64_t cjk_char_count = 0;        // CJK 统一表意(含 Ext-A/兼容区)
    std::uint64_t latin_word_count = 0;      // ASCII 字母/数字连成的词数
    std::uint64_t code_token_count = 0;      // 反引号段 + 带代码记号的裸词
    bool only_acknowledgement = false;       // 整段只是确认/否定/继续短语
    bool only_slash_command = false;         // 整段以 '/' 起头(宿主命令)
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
// ExtractTurnMemory 的既有前置门;P1 起又接了四条(同轮去重/短文本/
// 纯确认/纯命令);NoDurableSignal 的判定在 shadow 里记账,P3 的 gated
// 模式才拿它拦调用;ExtractModeOff 留给 P2 的 extract.mode 轴。
enum class ExtractionSkipReason {
    // P0 在线(现行前置门,§2.1):
    Disabled,         // project_memory 空 / generate_enabled=false(§10.1 skipped_disabled)
    NoNewHistory,     // 本轮 history 没增长(§10.1 history_grew 的补集)
    EmptyTranscript,  // 增量转写去协议壳后为空
    PromptMissing,    // 抽取系统提示词拼不出(prompts 目录缺模块)
    // P1 在线(§7.1 必跳层 + §7.3 门槛):
    AlreadyMutated,   // 本轮已有成功 save/forget/accept(§7.1 案二·同轮去重)
    ShortText,        // 无正文/空白标点/门槛不过(§7.1 案三案四 + §7.3)
    AcknowledgementOnly,  // 纯确认/否定/继续且无工具证据(§7.1 案六)
    SlashCommandOnly,     // 纯宿主命令(§7.1 案五)
    // 冻结待接:
    ExtractModeOff,   // P2 的 extract.mode=off(现行配置口径落 Disabled)
    NoDurableSignal,  // §7.2 判空;P1 只在 shadow 账里记,P3 gated 才拦
};
const char* ExtractionSkipReasonName(ExtractionSkipReason reason);

// ---------------------------------------------------------------------------
// 记忆写入调度单 P1(§7):零成本门控。必跳层与最短正文门是真闸
//(拦下就不构造 prompt);"耐久信号"层是 shadow——只记判断不拦调用,
// 量漏判用。全部纯函数,词法判定,不打请求。
// ---------------------------------------------------------------------------

// §7.3 最短正文门:cjk>=8 OR 拉丁词>=3 OR(代码记号>=2 且伴随自然语言)。
// "伴随自然语言" = 至少一个 CJK 字或一个拉丁词——纯符号堆不算。
bool PassesMinimumTextGate(const MeaningfulTextStats& stats);

// §7.1 必跳层的文本侧判定(案三至案六)。上下文侧的案一(write 关/
// extract 关,现行配置口径即 Disabled)、NoNewHistory 与案七(转写去协议
// 壳为空)由调用点按现场判,不在纯函数里。判定次序照 §7.1:
//   案五 纯宿主命令 → slash_command_only
//   案六 纯确认/否定/继续且无工具证据 → acknowledgement_only
//   案三/案四 无正文、纯空白标点、UI 合成、门槛不过 → short_text
// 纯确认但带工具证据的,案六不拦(§7.1 的"且没有新工具证据"),落到
// 门槛上按 short_text 拦——确认短语天然过不了最短正文门。
// 返回被拦的稳定 reason;空 = 过门,可构造 prompt。
std::optional<ExtractionSkipReason> EvaluateMustSkipTextGate(const MeaningfulTextStats& stats,
                                                             bool has_tool_evidence);

// §7.2 耐久信号(P1 shadow 首折,宁可保守):过门后"值不值得送审"的
// 词法判断。命中项的名字进账本(shadow 报告逐回合可复算);P1 不用它
// 拦调用,gated 模式(P3 起)才接进控制流。名字冻结名单:
//   preference_or_correction   跨回合偏好/禁忌/纠错(案一)
//   config_or_build_change     配置/依赖/构建/发布合同变更,须有工具证据(案二)
//   test_conclusion            测试/诊断的稳定结论,须有工具证据(案三)
//   module_boundary_or_entry   模块边界/命令入口/操作约束,须有工具证据(案四)
//   explicit_remember_unsaved  用户点名要记、主回合未存成(案五)
//   compact_pending_material   compact 未审材料(P3 有缓冲区才评,P1 恒不命中)
std::vector<std::string> EvaluateDurableSignals(const std::string& user_text,
                                                const MeaningfulTextStats& stats,
                                                bool has_tool_evidence, bool turn_mutated);

// shadow 开关(§7.2):环境变量 LUBANCODE_MEMORY_GATE_SHADOW 置
// 1/true/on 才评耐久信号,默认关——typed event 与 P0 同形,要量漏判再
// 开。配置文件轴是 P2 的活,这里只认环境变量。
bool MemoryGateShadowEnabled();

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
    // P1 门控观测:工具证据在场否(进 assessed 事件,复算用)。
    bool gate_context_noted_ = false;
    bool turn_has_tool_evidence_ = false;
    // P1 shadow:耐久信号评过了没(评过才落 shadow_gate 键;关着不落,
    // 事件与 P0 同形)。
    bool shadow_evaluated_ = false;
    ExtractionFunnel funnel_;
};

}  // namespace lubancode::app
