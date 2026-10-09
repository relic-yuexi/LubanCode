// memory_extract.hpp 的实现。任务分型、转写压缩与 JSON 解析全是纯函数,
// 好单测;只有 RunMemoryExtraction 碰网络。

#include "app/memory_extract.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "agent/prompt_assembler.hpp"
#include "agent/sample_model.hpp"  // SampleModel 原语:采样的公共路(批一·病四)
#include "api/backend.hpp"
#include "memory/project_memory.hpp"  // LooksLikeMemoryDate:occurred_at 的清洗
#include "platform/text_encoding.hpp"
#include "runtime/trajectory_session.hpp"  // MemoryTurnLedger 的落账口(P0 调度账)
#include "trajectory/recorder.hpp"

namespace lubancode::app {
namespace extraction = agent::memory_extraction;
const char* ExtractionErrorCodeName(ExtractionErrorCode code) {
    return extraction::ExtractionErrorCodeName(code);
}
std::string ClassifyTaskType(const std::string& text, const std::vector<std::string>& tools) {
    return extraction::ClassifyTaskType(text, tools);
}
std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes) {
    return extraction::BuildTurnTranscript(messages, max_bytes);
}
std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type) {
    return extraction::BuildExtractionSystemPrompt(prompts_dir, task_type);
}
std::expected<MemoryExtraction, ExtractionError> ParseExtractionJson(const std::string& text) {
    return extraction::ParseExtractionJson(text);
}
std::expected<MemoryExtraction, ExtractionError> RunMemoryExtraction(
    api::Backend& backend, const std::string& model, const std::string& system_prompt,
    const std::string& transcript, int timeout_secs, const std::string& reasoning_effort,
    agent::BackgroundCallAccounting* accounting, agent::LoopBoundaryRecorder* recorder,
    const std::atomic<bool>* cancel) {
    return extraction::RunMemoryExtraction(backend, model, system_prompt, transcript,
                                         timeout_secs, reasoning_effort, accounting, recorder, cancel);
}
std::expected<MemoryExtraction, ExtractionError> FinishMemoryExtraction(const agent::SampleResult& sampled) {
    return extraction::FinishMemoryExtraction(sampled);
}
const nlohmann::json& MemoryExtractionOutputSchema() {
    return extraction::MemoryExtractionOutputSchema();
}

// ---- 记忆写入调度单 P0(§六/§10)账本 + P1(§7)门控实现 -------------------

namespace {

// §7.1 案六的确认/否定/继续短语表。整段文本按空白与顿逗分节,每节剥去
// 尾助词与标点后须整词命中表内,全节都命中才算"纯确认"。宁可漏判
//(带正事的句子落 short_text),不把"好的,顺便改一下"认成确认。
bool IsAcknowledgementWord(std::string word) {
    static constexpr std::string_view kWords[] = {
        // 确认:好/嗯/行/对/是 一族(含口语变体)
        "好", "好的", "嗯", "嗯嗯", "嗯呢", "行", "行了", "行吧", "可以", "中",
        "对", "对的", "是", "是的", "没错", "哦", "噢", "好嘞", "好滴",
        // 否定:短拒绝一族
        "不", "不用", "没了", "没有", "没", "别", "算了", "不必", "不需要", "不用了",
        // 继续指令
        "继续", "接着", "接着来", "往下", "往下说", "说下去", "下一步", "再来", "再来一次",
        // 英文(小写比对)
        "ok", "okay", "okk", "yes", "yeah", "yep", "yup", "sure", "fine", "got it",
        "no", "nope", "nah", "go", "go on", "continue", "next", "proceed", "keep going",
        "go ahead",
    };
    for (const std::string_view candidate : kWords) {
        if (word == candidate) return true;
    }
    return false;
}

// 剥节首尾的空白与通用标点(ASCII + 全角)。尾助词不在这剥——先按原文
// 整词比对("算了"在表内),没中再剥助词试第二回("好的呀"→"好的")。
std::string StripPunctAndSpace(std::string word) {
    const auto is_noise = [](unsigned char c) {
        return std::isspace(c) != 0 || c == ',' || c == '.' || c == '!' || c == '?' || c == ';'
               || c == '~' || c == '"' || c == '\'' || c == ')' || c == '(';
    };
    static constexpr std::string_view kFullWidthPunct[] = {"，", "。", "！", "？", "；", "、", "～",
                                                           "”",  "“",  "）", "（", "…"};
    bool changed = true;
    while (changed && !word.empty()) {
        changed = false;
        while (!word.empty() && is_noise(static_cast<unsigned char>(word.front()))) {
            word.erase(word.begin());
            changed = true;
        }
        while (!word.empty() && is_noise(static_cast<unsigned char>(word.back()))) {
            word.pop_back();
            changed = true;
        }
        for (const std::string_view punct : kFullWidthPunct) {
            if (word.starts_with(punct)) {
                word.erase(0, punct.size());
                changed = true;
                break;
            }
            if (word.ends_with(punct)) {
                word.erase(word.size() - punct.size());
                changed = true;
                break;
            }
        }
    }
    return word;
}

// 剥尾助词(吧呀啊呢嘞哦噢了呗咯哈嘛),剥到不再是助词为止。
std::string StripTailParticles(std::string word) {
    static constexpr std::string_view kTailParticles[] = {"吧", "呀", "啊", "呢", "嘞", "哦",
                                                          "噢", "了", "呗", "咯", "哈", "嘛"};
    bool changed = true;
    while (changed && !word.empty()) {
        changed = false;
        for (const std::string_view particle : kTailParticles) {
            if (word.ends_with(particle)) {
                word.erase(word.size() - particle.size());
                changed = true;
                break;
            }
        }
    }
    return word;
}

// 一节是不是确认/否定/继续:剥首尾标点、压空白、小写化后整词命中;没中
// 再剥尾助词试一回,剥完空了不算("吧呀"剥成空串)。
bool IsAcknowledgementSegment(std::string word) {
    word = StripPunctAndSpace(std::move(word));
    if (word.empty()) return false;
    // 连续空白压成单空格(英文短语"go  on"与"go on"同一把尺)。
    std::string collapsed;
    bool in_space = false;
    for (const char byte : word) {
        const unsigned char c = static_cast<unsigned char>(byte);
        if (std::isspace(c) != 0) {
            if (!collapsed.empty() && !in_space) collapsed.push_back(' ');
            in_space = true;
            continue;
        }
        in_space = false;
        collapsed.push_back(byte);
    }
    if (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    word = std::move(collapsed);
    if (word.empty()) return false;
    // 英文短语统一小写比对。
    std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (IsAcknowledgementWord(word)) return true;
    const std::string stripped = StripTailParticles(word);
    return !stripped.empty() && IsAcknowledgementWord(stripped);
}

// 分节扫描:break_on_space=true 是细分的尺(空白也断节,中文友好),
// false 是粗分的尺(只按顿逗句问等标点断节,英文短语"go on"保形)。
// 全部节都命中才算;一节落空整把尺就倒。
bool SegmentsAllAcknowledgements(const std::string& text, bool break_on_space) {
    std::string current;
    bool any_match = false;
    bool all_match = true;
    const auto flush_segment = [&any_match, &all_match](std::string& segment) {
        if (segment.empty()) return;
        const bool matched = IsAcknowledgementSegment(std::move(segment));
        segment.clear();
        if (matched) {
            any_match = true;
        } else {
            all_match = false;
        }
    };
    static constexpr std::string_view kSegmentBreaks[] = {"，", "、", "。", "！", "？", "；"};
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            const bool is_break = c == ',' || c == ';' || (break_on_space && std::isspace(c) != 0);
            if (is_break) flush_segment(current);
            else current.push_back(text[i]);
            ++i;
            continue;
        }
        bool broke = false;
        for (const std::string_view marker : kSegmentBreaks) {
            if (text.compare(i, marker.size(), marker) == 0) {
                flush_segment(current);
                i += marker.size();
                broke = true;
                break;
            }
        }
        if (!broke) {
            current.push_back(text[i]);
            ++i;
        }
    }
    flush_segment(current);
    return any_match && all_match;
}

// 细分、粗分两把尺子各试一遍:任一把全节命中即纯确认。"ok, go on"细
// 分拆出孤零零的"on"对不上,粗分保住"go on"整词;"好的 继续"两把都
// 中;"好的 boss"两把都不中。
bool IsAcknowledgementText(const std::string& text) {
    return SegmentsAllAcknowledgements(text, true) || SegmentsAllAcknowledgements(text, false);
}

bool IsSlashCommandText(const std::string& text) {
    for (const char byte : text) {
        if (std::isspace(static_cast<unsigned char>(byte)) == 0) {
            return byte == '/';
        }
    }
    return false;
}

// 代码记号(§3.2 code_token_count):反引号围起的非空段(一段一记),
// 或同时含字母数字与代码记号字符(_ . / = : # @ $)的裸词。连字符与
// 尖括号归入词内但不作记号("well-known"不算,"<image attached>"这类
// UI 合成标记也不算,"build.sh"、"std::string"算)。
bool IsCodeWordChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '/' || c == '=' || c == ':' || c == '<' || c == '>' || c == '#' ||
           c == '@' || c == '$' || c == '-';
}

bool IsCodeMarkerChar(unsigned char c) {
    return c == '_' || c == '.' || c == '/' || c == '=' || c == ':' || c == '#' || c == '@' ||
           c == '$';
}

std::uint64_t CountCodeTokens(const std::string& text) {
    std::uint64_t count = 0;
    std::size_t i = 0;
    std::string word;
    const auto flush_word = [&count](const std::string& w) {
        if (w.empty()) return;
        bool has_alnum = false;
        bool has_marker = false;
        for (const char byte : w) {
            const unsigned char c = static_cast<unsigned char>(byte);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                has_alnum = true;
            } else if (IsCodeMarkerChar(c)) {
                has_marker = true;
            }
        }
        if (has_alnum && has_marker) ++count;
    };
    while (i < text.size()) {
        if (text[i] == '`') {
            // 反引号段:到下一枚反引号为止,非空即一记;没有配对就当裸字。
            const std::size_t close = text.find('`', i + 1);
            if (close != std::string::npos) {
                if (close > i + 1) ++count;
                i = close + 1;
                continue;
            }
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (IsCodeWordChar(c)) {
            word.push_back(text[i]);
        } else {
            flush_word(word);
            word.clear();
        }
        ++i;
    }
    flush_word(word);
    return count;
}

}  // namespace

MeaningfulTextStats ComputeMeaningfulTextStats(const std::string& text) {
    MeaningfulTextStats stats;
    // UTF-8 逐码点走:首字节定宽,续字节(0x80..0xBF)不重复计数。坏序列
    // 按单字节摊开,统计不炸即可——门控判定的正反例单测钉着。
    std::uint32_t code_point = 0;
    int pending = 0;  // 还差几个续字节
    auto flush = [&]() {
        if (pending > 0) return;  // 半截序列,丢弃
        if ((code_point >= 0x4E00 && code_point <= 0x9FFF) ||
            (code_point >= 0x3400 && code_point <= 0x4DBF) ||
            (code_point >= 0xF900 && code_point <= 0xFAFF)) {
            ++stats.cjk_char_count;
        }
    };
    bool in_latin_word = false;
    const auto is_latin = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    for (const char byte : text) {
        const unsigned char c = static_cast<unsigned char>(byte);
        if (pending > 0) {
            if ((c & 0xC0) == 0x80) {
                code_point = (code_point << 6) | (c & 0x3F);
                if (--pending == 0) {
                    ++stats.unicode_scalar_count;
                    flush();
                }
                continue;
            }
            pending = 0;  // 坏续字节:落回按首字节重解
        }
        if (c < 0x80) {
            ++stats.unicode_scalar_count;
            code_point = c;
            flush();
            if (is_latin(c)) {
                if (!in_latin_word) {
                    in_latin_word = true;
                    ++stats.latin_word_count;
                }
            } else {
                in_latin_word = false;
            }
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            code_point = c & 0x1F;
            pending = 1;
        } else if ((c & 0xF0) == 0xE0) {
            code_point = c & 0x0F;
            pending = 2;
        } else if ((c & 0xF8) == 0xF0) {
            code_point = c & 0x07;
            pending = 3;
        } else {
            // 坏首字节(0x80..0xBF 孤续字节/0xF8+):按一个标量摊开。
            ++stats.unicode_scalar_count;
        }
    }
    // P1 补全三项(§3.2):代码记号、纯确认、纯命令。
    stats.code_token_count = CountCodeTokens(text);
    stats.only_acknowledgement = IsAcknowledgementText(text);
    stats.only_slash_command = IsSlashCommandText(text);
    return stats;
}

// ---- P1(§7)门控 ------------------------------------------------------------

bool PassesMinimumTextGate(const MeaningfulTextStats& stats) {
    if (stats.cjk_char_count >= 8) return true;
    if (stats.latin_word_count >= 3) return true;
    // 代码记号要伴随自然语言:至少一个 CJK 字或一个拉丁词。纯符号堆
    //(反引号空段、孤立标点)不算自然语言。
    const bool has_natural_language = stats.cjk_char_count >= 1 || stats.latin_word_count >= 1;
    return stats.code_token_count >= 2 && has_natural_language;
}

std::optional<ExtractionSkipReason> EvaluateMustSkipTextGate(const MeaningfulTextStats& stats,
                                                             bool has_tool_evidence) {
    if (stats.only_slash_command) return ExtractionSkipReason::SlashCommandOnly;
    if (stats.only_acknowledgement && !has_tool_evidence) {
        return ExtractionSkipReason::AcknowledgementOnly;
    }
    if (!PassesMinimumTextGate(stats)) return ExtractionSkipReason::ShortText;
    return std::nullopt;
}

std::vector<std::string> EvaluateDurableSignals(const std::string& user_text,
                                                const MeaningfulTextStats& /*stats*/,
                                                bool has_tool_evidence, bool turn_mutated) {
    // 英文关键词按小写比对:文本先折一份小写(ASCII 段),中文不受影响。
    std::string lowered;
    lowered.reserve(user_text.size());
    for (const char byte : user_text) {
        const unsigned char c = static_cast<unsigned char>(byte);
        lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : byte);
    }
    const auto hit = [&lowered](std::initializer_list<const char*> needles) {
        for (const char* needle : needles) {
            if (lowered.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    std::vector<std::string> signals;
    // 案一:跨回合偏好/禁忌/纠错。
    if (hit({"以后", "下次", "别再", "再也不", "统一用", "改用", "换成", "永远", "记住要",
             "from now on", "always", "never", "instead", "prefer"})) {
        signals.push_back("preference_or_correction");
    }
    // 案二:配置/依赖/构建/发布合同变更,须有工具证据。
    if (has_tool_evidence &&
        hit({"配置", "依赖", "构建", "编译", "安装", "发布", "升级", "版本", "cmake", "cmakelists",
             "package.json", "pyproject", "requirements", "lockfile", ".lock", "build", "install",
             "release", "upgrade", "version", "config"})) {
        signals.push_back("config_or_build_change");
    }
    // 案三:测试/诊断的稳定结论,须有工具证据。
    if (has_tool_evidence &&
        hit({"全绿", "跑通", "通过了", "测试通过", "失败", "复现", "根因", "定位到", "回归",
             "all green", "all tests", "tests passed", "passed", "failed", "repro"})) {
        signals.push_back("test_conclusion");
    }
    // 案四:模块边界/命令入口/操作约束,须有工具证据。
    if (has_tool_evidence &&
        hit({"入口", "边界", "新命令", "命令", "接口", "注册", "拆出", "改名", "cli", "api",
             "entry", "command", "module", "rename"})) {
        signals.push_back("module_boundary_or_entry");
    }
    // 案五:用户点名要记、主回合未存成(存成了早在案二门就被同轮去重收手,
    // 走不到这;这里仍显式带 turn_mutated,判定的语义自足)。
    if (!turn_mutated && hit({"记住", "别忘了", "remember", "remember to"})) {
        signals.push_back("explicit_remember_unsaved");
    }
    // 案六:compact 未审材料——P3 有 extraction buffer 才评,名字冻结,
    // P1 恒不命中。
    return signals;
}

std::vector<std::string> EvaluateTurnDurableSignals(const std::string& user_text,
                                                   const std::string& assistant_text,
                                                   bool has_tool_evidence) {
    auto signals = EvaluateDurableSignals(user_text, ComputeMeaningfulTextStats(user_text),
                                         has_tool_evidence, false);
    if (has_tool_evidence) {
        const auto conclusions = EvaluateDurableSignals(assistant_text, {}, true, false);
        for (const auto& signal : conclusions) {
            if (signal == "preference_or_correction" || signal == "explicit_remember_unsaved") continue;
            if (std::find(signals.begin(), signals.end(), signal) == signals.end()) signals.push_back(signal);
        }
    }
    return signals;
}

bool MemoryGateShadowEnabled() {
    const char* raw = std::getenv("LUBANCODE_MEMORY_GATE_SHADOW");
    if (raw == nullptr) return false;
    const std::string value = raw;
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

const char* ExtractionTriggerName(ExtractionTrigger trigger) {
    switch (trigger) {
        case ExtractionTrigger::EveryTurn: return "every_turn";
        case ExtractionTrigger::BatchWatermark: return "batch_watermark";
        case ExtractionTrigger::IdleTimeout: return "idle_timeout";
        case ExtractionTrigger::BeforeCompact: return "before_compact";
        case ExtractionTrigger::SessionEnd: return "session_end";
    }
    return "every_turn";
}

const char* ExtractionDecisionName(ExtractionDecision decision) {
    switch (decision) {
        case ExtractionDecision::Skipped: return "skipped";
        case ExtractionDecision::Called: return "called";
    }
    return "skipped";
}

const char* ExtractionSkipReasonName(ExtractionSkipReason reason) {
    switch (reason) {
        case ExtractionSkipReason::Disabled: return "disabled";
        case ExtractionSkipReason::NoNewHistory: return "no_new_history";
        case ExtractionSkipReason::EmptyTranscript: return "empty_transcript";
        case ExtractionSkipReason::PromptMissing: return "prompt_missing";
        case ExtractionSkipReason::ExtractModeOff: return "extract_mode_off";
        case ExtractionSkipReason::AlreadyMutated: return "already_mutated";
        case ExtractionSkipReason::ShortText: return "short_text";
        case ExtractionSkipReason::AcknowledgementOnly: return "acknowledgement_only";
        case ExtractionSkipReason::SlashCommandOnly: return "slash_command_only";
        case ExtractionSkipReason::NoDurableSignal: return "no_durable_signal";
    }
    return "disabled";
}

std::string StableExtractErrorCode(const ExtractionError& error) {
    // 结构化版(P0-A 起):六类新码 + route_miss。旧账里 syntax_invalid/
    // utf8_invalid/schema_invalid 统称 parse_failed,离线对账按此折算。
    return app::ExtractionErrorCodeName(error.code);
}

std::string StableExtractErrorCode(const std::string& error) {
    // 旧文案版(保留):ExtractTurnMemory 失败路的固定文案(编译期字面量);
    // 认不出落 other。新文案以"抽取输出"开头,这把尺子继续量得准。
    if (error.starts_with("cheap 路由找不到 provider")) return "route_miss";
    if (error.starts_with("抽取输出为空")) return "empty_output";
    if (error.starts_with("抽取输出")) return "parse_failed";  // 不是合法 JSON / 找不到 object
    return "other";
}

std::uint64_t AutoQueuedFromAssessedPayload(const nlohmann::json& payload) {
    // 修复单 §五 D 的读侧统一口:新键优先,旧键是同一计数的历史名(排队
    // 数,不是落盘数)。nlohmann 缺键经 value() 安全取默认,不碰 UB。
    if (!payload.is_object()) return 0;
    for (const char* key : {"autoQueued", "auto_queued"}) {
        const auto found = payload.find(key);
        if (found != payload.end() && found->is_number_unsigned()) {
            return found->get<std::uint64_t>();
        }
    }
    for (const char* key : {"autoWritten", "auto_written"}) {
        const auto found = payload.find(key);
        if (found != payload.end() && found->is_number_unsigned()) {
            return found->get<std::uint64_t>();
        }
    }
    return 0;
}

namespace {

// §10.1 漏斗的 skip 计数器与 reason 的对账(一处收口,漏斗不散架)。
void CountSkip(ExtractionFunnel& funnel, ExtractionSkipReason reason) {
    switch (reason) {
        case ExtractionSkipReason::Disabled: ++funnel.skipped_disabled; break;
        case ExtractionSkipReason::NoNewHistory: ++funnel.skipped_no_new_history; break;
        case ExtractionSkipReason::EmptyTranscript: ++funnel.skipped_empty_transcript; break;
        case ExtractionSkipReason::PromptMissing: ++funnel.skipped_prompt_missing; break;
        // P1/P3 接线后才轮到这五枚。
        case ExtractionSkipReason::ShortText: ++funnel.skipped_short; break;
        case ExtractionSkipReason::AcknowledgementOnly: ++funnel.skipped_ack; break;
        case ExtractionSkipReason::SlashCommandOnly: ++funnel.skipped_command; break;
        case ExtractionSkipReason::AlreadyMutated: ++funnel.skipped_already_mutated; break;
        case ExtractionSkipReason::NoDurableSignal: ++funnel.skipped_no_durable_signal; break;
        case ExtractionSkipReason::ExtractModeOff: break;  // 档位账归配置,P0 不数
    }
}

}  // namespace

MemoryTurnLedger::MemoryTurnLedger(runtime::TrajectorySessionLedger* trajectory)
    : trajectory_(trajectory) {}
MemoryTurnLedger::~MemoryTurnLedger() = default;

void MemoryTurnLedger::BeginTurn(std::string session_id, std::string turn_id,
                                 const std::string& user_text) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // 悬账冲账(回合总结异步化单):上一轮门过起飞、收账还没赶上新轮
    // 开张——悬账以 aborted 口径落袋(decision=Called 而 outcome 缺席,
    // RecordAssessedLocked 的既有兜底),不编数字,也不阻塞新轮。
    if (!suspended_turn_id_.empty()) {
        RecordAssessedLocked(0);
        suspended_turn_id_.clear();
    }
    state_ = MemoryTurnState{};
    state_.session_id = std::move(session_id);
    state_.turn_id = std::move(turn_id);
    state_.user_text_stats = ComputeMeaningfulTextStats(user_text);
    state_.extraction_gate_decision = ExtractionDecision::Skipped;
    state_.extraction_gate_reason = ExtractionSkipReason::Disabled;
    turn_open_ = true;
    extraction_called_ = false;
    pending_outcome_ = ExtractOutcome{};
    gate_context_noted_ = false;
    turn_has_tool_evidence_ = false;
    shadow_evaluated_ = false;
    ++funnel_.outer_user_turns;
}

void MemoryTurnLedger::NoteExtractionSkipped(ExtractionSkipReason reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.extraction_gate_decision = ExtractionDecision::Skipped;
    state_.extraction_gate_reason = reason;
    CountSkip(funnel_, reason);
}

void MemoryTurnLedger::NoteHistoryGrew() {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++funnel_.history_grew_turns;
}

bool MemoryTurnLedger::turn_mutated() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return turn_open_ && !(state_.successful_save_ids.empty() && state_.successful_forget_ids.empty()
                           && state_.accepted_candidate_ids.empty());
}

void MemoryTurnLedger::NoteGateContext(bool has_tool_evidence) {
    const std::lock_guard<std::mutex> lock(mutex_);
    gate_context_noted_ = true;
    turn_has_tool_evidence_ = has_tool_evidence;
}

void MemoryTurnLedger::NoteDurableSignals(const std::vector<std::string>& reasons) {
    const std::lock_guard<std::mutex> lock(mutex_);
    shadow_evaluated_ = true;
    state_.durable_signal_reasons = reasons;
}

void MemoryTurnLedger::NoteExtractionCalled() {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.extraction_gate_decision = ExtractionDecision::Called;
    extraction_called_ = true;
    ++funnel_.extract_batches;
    ++funnel_.eligible_turns;
}

void MemoryTurnLedger::NoteExtractionOutcome(const ExtractOutcome& outcome) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_outcome_ = outcome;
    if (!outcome.ok) ++funnel_.extract_failures;
}

void MemoryTurnLedger::OnMemoryWriteReceipt(const memory::MemoryWriteReceipt& receipt) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string turn_id = turn_open_ ? state_.turn_id : std::string();
    // 本轮写入账(§6.1):排队成功按路分账;被拒记稳定码。job_id 是排进
    // pending 的文件名——排队≠落盘,P0 不冒充 committed。
    if (receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued) {
        if (receipt.operation == "forget") {
            state_.successful_forget_ids.push_back(receipt.job_id);
        } else {
            state_.successful_save_ids.push_back(receipt.job_id);
            if (receipt.source == memory::MemoryWriteSource::CandidateAccept) {
                // accept 的凭证即 job:候选文件当场删了,job 名是留得住的号。
                state_.accepted_candidate_ids.push_back(receipt.job_id);
            }
        }
    } else {
        state_.rejected_write_codes.push_back(receipt.error_code);
    }
    RecordReceiptLocked(receipt, turn_id);
}

void MemoryTurnLedger::FinishTurn(std::int64_t foreground_tail_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn_open_) {
        RecordAssessedLocked(foreground_tail_ms);
    }
    turn_open_ = false;
    suspended_turn_id_.clear();  // FinishTurn 落过袋的回合没有悬账(纪律:二选一)
    state_.turn_id.clear();  // 回合间的写路回执(slash 命令)不带回合号
}

void MemoryTurnLedger::SuspendTurn() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!turn_open_) {
        return;
    }
    // 悬账:state_(门决策/漏斗材料)原样留着等迟到收账补 outcome;轮先
    // 关(回合间口径:写路回执不挂轮号)。悬账期间 slash 写路回执还会写
    // state_ 的写入账——那是回合间的事,RecordAssessedLocked 不读它,串
    // 不进这轮的 assessed。
    suspended_turn_id_ = state_.turn_id;
    turn_open_ = false;
    state_.turn_id.clear();
}

bool MemoryTurnLedger::SettleSuspendedTurn(const std::string& turn_id, std::int64_t settle_wall_ms,
                                           const ExtractOutcome* outcome) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (suspended_turn_id_.empty() || suspended_turn_id_ != turn_id) {
        // 对不上档:悬账已被冲(BeginTurn 记了 aborted)或弃过(换代)。
        // 真失败仍数进漏斗——漏斗是"一场会话"的聚合,不因回合翻篇丢数。
        if (outcome != nullptr && !outcome->ok) {
            ++funnel_.extract_failures;
        }
        return false;
    }
    if (outcome != nullptr) {
        pending_outcome_ = *outcome;
        if (!outcome->ok) {
            ++funnel_.extract_failures;
        }
    }
    RecordAssessedLocked(settle_wall_ms);
    suspended_turn_id_.clear();
    state_ = MemoryTurnState{};  // 悬账期间攒下的回合间回执不串进下一笔
    pending_outcome_ = ExtractOutcome{};
    return true;
}

void MemoryTurnLedger::AbandonSuspendedTurn() {
    const std::lock_guard<std::mutex> lock(mutex_);
    // 换代弃账:不落盘、不记漏斗(这一轮连"评估完成"都到不了新场的账
    // 上)。state_ 留给下一轮 BeginTurn 重置。
    suspended_turn_id_.clear();
}

void MemoryTurnLedger::RecordAssessedLocked(std::int64_t foreground_tail_ms) {
    if (trajectory_ == nullptr) return;
    // v3 场走 v3 写口(取消误报 ESC 单 Bug 2):typed 事件 + camelCase 载荷,
    // 不往 v3 卷塞 v2 行;v2 老路一字不动。
    if (auto* v3_writer = trajectory_->v3_main_writer()) {
        RecordAssessedV3Locked(*v3_writer, foreground_tail_ms);
        return;
    }
    auto* recorder = trajectory_->main();
    if (recorder == nullptr) return;

    nlohmann::json payload{
        {"trigger", ExtractionTriggerName(ExtractionTrigger::EveryTurn)},
        {"turn_id", state_.turn_id},
        {"decision", ExtractionDecisionName(state_.extraction_gate_decision)},
        {"user_text_stats",
         nlohmann::json{{"unicode_scalar_count", state_.user_text_stats.unicode_scalar_count},
                        {"cjk_char_count", state_.user_text_stats.cjk_char_count},
                        {"latin_word_count", state_.user_text_stats.latin_word_count},
                        // P1(§3.2)补全的三项:六项一并算齐、一并落账,shadow
                        // 报告与门槛判定离线可复算。
                        {"code_token_count", state_.user_text_stats.code_token_count},
                        {"only_acknowledgement", state_.user_text_stats.only_acknowledgement},
                        {"only_slash_command", state_.user_text_stats.only_slash_command}}},
        {"foreground_tail_ms", foreground_tail_ms},
    };
    // P1(§7.1):工具证据在场否——ack 门与耐久信号的"须有工具证据"靠它
    // 离线复算。走不到转写扫描的回合(disabled/no_new_history/同轮去重)
    // 不落键。
    if (gate_context_noted_) {
        payload["has_tool_evidence"] = turn_has_tool_evidence_;
    }
    // P1(§7.2 shadow):耐久信号逐回合落账,离线重放可复算漏判。开着才
    // 落(空表 = 评过、一条没中);关着事件与 P0 同形。
    if (shadow_evaluated_) {
        payload["shadow_gate"] = nlohmann::json{
            {"durable_signal", state_.durable_signal_reasons.empty() ? "none" : "hit"},
            {"signals", state_.durable_signal_reasons}};
    }
    if (state_.extraction_gate_decision == ExtractionDecision::Skipped) {
        payload["skip_reason"] = ExtractionSkipReasonName(state_.extraction_gate_reason);
    } else {
        // called:收口材料齐上报;outcome 没送到(异常路)按 aborted 报,
        // 不编数字。provider 没报 usage 时 token 三项整组缺席(§10.3)。
        ExtractOutcome outcome = pending_outcome_;
        if (!outcome.ok && outcome.error_code.empty()) {
            // ok=false 且无稳定码:收口没走到,记 aborted。
            outcome.error_code = "aborted";
        }
        payload["extract_outcome"] = outcome.ok ? "completed" : "failed";
        if (!outcome.ok) payload["error_code"] = outcome.error_code;
        payload["extract_wall_ms"] = outcome.extract_wall_ms;
        payload["review_candidates"] = outcome.review_candidates;
        payload["auto_queued"] = outcome.auto_queued;
        if (outcome.usage_reported) {
            payload["usage_reported"] = true;
            payload["input_tokens"] = outcome.input_tokens;
            payload["output_tokens"] = outcome.output_tokens;
            payload["cached_tokens"] = outcome.cached_tokens;
        }
    }

    trajectory::EventScope scope = recorder->base_scope();
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.actor = trajectory::Actor::Host;
    scope.origin = trajectory::Origin::ScheduledHost;
    scope.visibility = {trajectory::Visibility::HostOnly};
    scope.training_policy = trajectory::TrainingPolicy::Exclude;

    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::MemoryExtractionAssessed;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    // 落不稳只吞(诊断口径同 MemoryLedgerBridge):调度账不许反过来
    // 拖垮回合收尾。
    (void)recorder->Record(request, trajectory::Durability::ProcessCrash);
}

void MemoryTurnLedger::RecordAssessedV3Locked(trajectory::v3::V3Writer& writer,
                                              std::int64_t foreground_tail_ms) {
    // v3 的 assessed 事实行(取消误报 ESC 单 Bug 2):字段与 v2 同一套账
    //(跳过原因/决策/收口材料/墙钟/失败码),键名随 v3 合同走 camelCase;
    // turnId 挂触发它的主回合,重开会话单凭事件答得出"哪次抽取、预算
    // 多久、实际多久、谁叫停"。
    nlohmann::json payload{
        {"trigger", ExtractionTriggerName(ExtractionTrigger::EveryTurn)},
        {"turnId", state_.turn_id},
        {"decision", ExtractionDecisionName(state_.extraction_gate_decision)},
        {"userTextStats",
         nlohmann::json{{"unicodeScalarCount", state_.user_text_stats.unicode_scalar_count},
                        {"cjkCharCount", state_.user_text_stats.cjk_char_count},
                        {"latinWordCount", state_.user_text_stats.latin_word_count},
                        {"codeTokenCount", state_.user_text_stats.code_token_count},
                        {"onlyAcknowledgement", state_.user_text_stats.only_acknowledgement},
                        {"onlySlashCommand", state_.user_text_stats.only_slash_command}}},
        {"foregroundTailMs", foreground_tail_ms},
    };
    if (gate_context_noted_) {
        payload["hasToolEvidence"] = turn_has_tool_evidence_;
    }
    if (shadow_evaluated_) {
        payload["shadowGate"] = nlohmann::json{
            {"durableSignal", state_.durable_signal_reasons.empty() ? "none" : "hit"},
            {"signals", state_.durable_signal_reasons}};
    }
    if (state_.extraction_gate_decision == ExtractionDecision::Skipped) {
        payload["skipReason"] = ExtractionSkipReasonName(state_.extraction_gate_reason);
    } else {
        ExtractOutcome outcome = pending_outcome_;
        if (!outcome.ok && outcome.error_code.empty()) {
            outcome.error_code = "aborted";  // 收口没走到,不编数字
        }
        payload["extractOutcome"] = outcome.ok ? "completed" : "failed";
        if (!outcome.ok) payload["errorCode"] = outcome.error_code;
        payload["extractWallMs"] = outcome.extract_wall_ms;
        payload["reviewCandidates"] = outcome.review_candidates;
        payload["autoQueued"] = outcome.auto_queued;
        if (outcome.usage_reported) {
            payload["usageReported"] = true;
            payload["inputTokens"] = outcome.input_tokens;
            payload["outputTokens"] = outcome.output_tokens;
            payload["cachedTokens"] = outcome.cached_tokens;
        }
    }
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::MemoryExtractionAssessed;
    if (!state_.turn_id.empty()) {
        draft.turn_id = state_.turn_id;
    }
    draft.payload = std::move(payload);
    (void)writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
}

void MemoryTurnLedger::RecordReceiptV3Locked(trajectory::v3::V3Writer& writer,
                                             const memory::MemoryWriteReceipt& receipt,
                                             const std::string& turn_id) {
    nlohmann::json payload{
        {"source", memory::MemoryWriteSourceName(receipt.source)},
        {"operation", receipt.operation},
        {"outcome", memory::MemoryWriteReceiptOutcomeName(receipt.outcome)},
        {"layer", receipt.layer},
    };
    if (!receipt.kind.empty()) payload["kind"] = receipt.kind;
    if (!turn_id.empty()) payload["turnId"] = turn_id;
    if (receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued) {
        payload["jobId"] = receipt.job_id;
    } else {
        payload["errorCode"] = receipt.error_code;
    }
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::MemoryWriteReceipted;
    if (!turn_id.empty()) {
        draft.turn_id = turn_id;
    }
    draft.payload = std::move(payload);
    (void)writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
}

void MemoryTurnLedger::RecordReceiptLocked(const memory::MemoryWriteReceipt& receipt,
                                           const std::string& turn_id) {
    if (trajectory_ == nullptr) return;
    // v3 场走 v3 写口(Bug 2 同门):receipted 事实行,载荷 camelCase。
    if (auto* v3_writer = trajectory_->v3_main_writer()) {
        RecordReceiptV3Locked(*v3_writer, receipt, turn_id);
        return;
    }
    auto* recorder = trajectory_->main();
    if (recorder == nullptr) return;

    nlohmann::json payload{
        {"source", memory::MemoryWriteSourceName(receipt.source)},
        {"operation", receipt.operation},
        {"outcome", memory::MemoryWriteReceiptOutcomeName(receipt.outcome)},
        {"layer", receipt.layer},
    };
    if (!receipt.kind.empty()) payload["kind"] = receipt.kind;
    if (!turn_id.empty()) payload["turn_id"] = turn_id;
    if (receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued) {
        payload["job_id"] = receipt.job_id;
    } else {
        payload["error_code"] = receipt.error_code;
    }

    trajectory::EventScope scope = recorder->base_scope();
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    // 谁发起的写:显式命令与候选接受归 user,模型工具归 tool,宿主抽取
    // 归 host(与 memory.save.requested 的 actor 口径同款)。
    switch (receipt.source) {
        case memory::MemoryWriteSource::ExplicitCommandSave:
        case memory::MemoryWriteSource::ExplicitForget:
        case memory::MemoryWriteSource::CandidateAccept:
            scope.actor = trajectory::Actor::User;
            scope.origin = trajectory::Origin::ExternalUser;
            break;
        case memory::MemoryWriteSource::ModelToolSave:
            scope.actor = trajectory::Actor::Tool;
            scope.origin = trajectory::Origin::BuiltinTool;
            break;
        case memory::MemoryWriteSource::AutoExtraction:
            scope.actor = trajectory::Actor::Host;
            scope.origin = trajectory::Origin::ScheduledHost;
            break;
    }
    scope.visibility = {trajectory::Visibility::HostOnly};
    scope.training_policy = trajectory::TrainingPolicy::Exclude;

    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::MemoryWriteReceipted;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    (void)recorder->Record(request, trajectory::Durability::ProcessCrash);
}

}  // namespace lubancode::app
