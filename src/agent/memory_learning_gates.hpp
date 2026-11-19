#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::agent::memory_learning_gates {
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
// 纯确认/纯命令)；NoDurableSignal 在发请求前拦截；
// ExtractModeOff 留给独立配置轴。
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
    NoDurableSignal,  // 无耐久信号，不发抽取请求
};
const char* ExtractionSkipReasonName(ExtractionSkipReason reason);

// ---------------------------------------------------------------------------
// 记忆写入调度单 P1(§7):零成本门控。必跳层与最短正文门是真闸
//(拦下就不构造 prompt)；耐久信号也是真闸。全部纯函数，词法判定，不打请求。
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
// 词法判断。命中项进账本；EvaluateTurnDurableSignals 合并用户意图和
// 有工具证据的助手结论，供请求前门控使用。名字冻结名单:
//   preference_or_correction   跨回合偏好/禁忌/纠错(案一)
//   config_or_build_change     配置/依赖/构建/发布合同变更,须有工具证据(案二)
//   test_conclusion            测试/诊断的稳定结论,须有工具证据(案三)
//   module_boundary_or_entry   模块边界/命令入口/操作约束,须有工具证据(案四)
//   explicit_remember_unsaved  用户点名要记、主回合未存成(案五)
//   compact_pending_material   compact 未审材料(P3 有缓冲区才评,P1 恒不命中)
std::vector<std::string> EvaluateDurableSignals(const std::string& user_text,
                                                const MeaningfulTextStats& stats,
                                                bool has_tool_evidence, bool turn_mutated);

std::vector<std::string> EvaluateTurnDurableSignals(const std::string& user_text,
                                                   const std::string& assistant_text,
                                                   bool has_tool_evidence);
}  // namespace lubancode::agent::memory_learning_gates
