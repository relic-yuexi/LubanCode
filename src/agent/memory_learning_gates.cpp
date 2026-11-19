#include "agent/memory_learning_gates.hpp"
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace lubancode::agent::memory_learning_gates {
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

}  // namespace lubancode::agent::memory_learning_gates
