// prompt_auditor.hpp 的实现:static/runtime 两层规则 + runtime 读侧装配。
// 纯规则部分零 IO;CollectRuntimeRequests 只读 Journal。
#include "insights/prompt_auditor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "accounting/session_usage_reader.hpp"
#include "accounting/usage_projector.hpp"
#include "agent/context.hpp"  // EstimateUtf8Tokens
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"

namespace lubancode::insights {
namespace {

// ---------------- 通用小件 ----------------

// 四舍五入整数百分比;分母 <= 0 给 0(调用方先判 unknown)。
int SharePercent(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) {
        return 0;
    }
    return static_cast<int>((part * 200 + whole) / (whole * 2));
}

EvidenceItem Ev(std::string metric, nlohmann::json value) {
    EvidenceItem item;
    item.metric = std::move(metric);
    item.value = std::move(value);
    return item;
}

Finding MakeFinding(std::string category, FindingSeverity severity, FindingConfidence confidence,
                    std::string summary, std::string recommendation, std::string rule_code) {
    Finding finding;
    finding.category = std::move(category);
    finding.severity = severity;
    finding.confidence = confidence;
    finding.origin = FindingOrigin::DeterministicRule;
    finding.summary = std::move(summary);
    finding.recommendation = std::move(recommendation);
    finding.rule_version = std::string(kPromptAuditRuleVersion) + ":" + std::move(rule_code);
    return finding;
}

// ---------------- 文本测量(只测量,不外传正文) ----------------

// 把文本折成 token 集合:ASCII 词(小写化,≥2 字)+ CJK 字符二元组。
// 用于 n-gram 重合的 Jaccard;正文本身不出这个编译单元。
std::set<std::string> TextTokens(const std::string& text) {
    std::set<std::string> tokens;
    std::string word;
    std::string prev_cjk;
    const auto flush_word = [&]() {
        if (word.size() >= 2) {
            tokens.insert(word);
        }
        word.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (std::isalnum(c) != 0) {
                word.push_back(static_cast<char>(std::tolower(c)));
            } else {
                flush_word();
            }
            prev_cjk.clear();
            ++i;
            continue;
        }
        flush_word();
        // 多字节序列:判 CJK 统一表意(U+4E00-U+9FFF,UTF-8 三字节
        // 0xE4-0xE9 起)。其余文种按整段跳过(不猜)。
        const std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        if (i + len <= text.size() && c >= 0xE4 && c <= 0xE9) {
            const std::string ch = text.substr(i, 3);
            if (!prev_cjk.empty()) {
                tokens.insert(prev_cjk + ch);
            }
            prev_cjk = ch;
        } else {
            prev_cjk.clear();
        }
        i += len;
    }
    flush_word();
    return tokens;
}

double Jaccard(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    std::size_t shared = 0;
    for (const auto& token : a) {
        if (b.contains(token)) {
            ++shared;
        }
    }
    const std::size_t merged = a.size() + b.size() - shared;
    return merged == 0 ? 0.0 : static_cast<double>(shared) / static_cast<double>(merged);
}

bool ContainsAny(const std::string& text, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------- static 规则 ----------------

constexpr std::int64_t kToolDescriptionTokenLimit = 900;  // 单枚工具描述的"过长"线
constexpr int kSchemaDepthLimit = 6;                      // schema 嵌套深度线

// schema 嵌套深度(object/array 往下数)。
int JsonDepth(const nlohmann::json& json) {
    if (!json.is_object() && !json.is_array()) {
        return 0;
    }
    int depth = 0;
    if (json.is_object()) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            depth = std::max(depth, JsonDepth(it.value()));
        }
    } else {
        for (const auto& item : json) {
            depth = std::max(depth, JsonDepth(item));
        }
    }
    return depth + 1;
}

void AuditOverrideChains(const agent::PromptManifest& manifest, std::vector<Finding>& out) {
    std::vector<std::string> overridden;
    for (const auto& segment : manifest.segments) {
        if (!segment.overrides.empty()) {
            overridden.push_back(segment.segment_id + "(压掉 " +
                                 std::to_string(segment.overrides.size()) + " 层)");
        }
    }
    if (overridden.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "prompt.override_chain", FindingSeverity::Info, FindingConfidence::High,
        "有 " + std::to_string(overridden.size()) + " 个模块被上层覆盖,最终来源已入账(账本可查,无来源不明项)",
        "覆盖是分层设计的本意;若非本意,查对应模块的五层解析账", "S01");
    finding.scope = "config";
    finding.evidence.push_back(Ev("overridden_segments", overridden));
    out.push_back(std::move(finding));
}

void AuditDuplicateContent(const agent::PromptManifest& manifest, std::vector<Finding>& out) {
    std::map<std::string, std::vector<std::string>> by_hash;
    for (const auto& segment : manifest.segments) {
        if (!segment.rendered_hash.empty()) {
            by_hash[segment.rendered_hash].push_back(segment.segment_id);
        }
    }
    std::vector<std::string> duplicates;
    std::string sample_hash;
    for (const auto& [hash, ids] : by_hash) {
        if (ids.size() >= 2) {
            duplicates.push_back(ids.front() + " 与 " +
                                 std::to_string(ids.size() - 1) + " 个别的段同文");
            if (sample_hash.empty()) {
                sample_hash = hash;
            }
        }
    }
    if (duplicates.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "prompt.duplicate_content", FindingSeverity::Warning, FindingConfidence::High,
        "有 " + std::to_string(duplicates.size()) + " 组段正文 hash 完全相同(同文重复,白占 token)",
        "重复段留一份;另一份改为引用或删除", "S02");
    finding.scope = "config";
    finding.evidence.push_back(Ev("duplicate_groups", duplicates));
    finding.evidence.push_back(Ev("sample_rendered_hash", sample_hash.substr(0, 16) + "…"));
    out.push_back(std::move(finding));
}

void AuditNgramOverlap(const StaticAuditInput& input, std::vector<Finding>& out) {
    struct PairHit {
        std::string a;
        std::string b;
        int percent = 0;
    };
    std::vector<PairHit> hits;
    std::vector<const std::pair<const std::string, std::string>*> texts;
    for (const auto& entry : input.segment_texts) {
        if (entry.second.size() >= 160) {
            texts.push_back(&entry);
        }
    }
    // 魄与模型指令也进测量(它们与模块撞车同样费 token)。
    std::vector<std::pair<std::string, std::set<std::string>>> tokenized;
    for (const auto* entry : texts) {
        tokenized.emplace_back(entry->first, TextTokens(entry->second));
    }
    if (input.soul_text.size() >= 160) {
        tokenized.emplace_back("(soul)", TextTokens(input.soul_text));
    }
    if (input.model_instructions_text.size() >= 160) {
        tokenized.emplace_back("(model_instructions)", TextTokens(input.model_instructions_text));
    }
    for (std::size_t i = 0; i < tokenized.size(); ++i) {
        for (std::size_t j = i + 1; j < tokenized.size(); ++j) {
            const double overlap = Jaccard(tokenized[i].second, tokenized[j].second);
            if (overlap >= 0.55) {
                hits.push_back(PairHit{tokenized[i].first, tokenized[j].first,
                                       static_cast<int>(overlap * 100.0)});
            }
        }
    }
    if (hits.empty()) {
        return;
    }
    std::sort(hits.begin(), hits.end(), [](const PairHit& a, const PairHit& b) {
        if (a.percent != b.percent) {
            return a.percent > b.percent;
        }
        return a.a + a.b < b.a + b.b;
    });
    Finding finding = MakeFinding(
        "prompt.ngram_overlap", FindingSeverity::Warning, FindingConfidence::Medium,
        "有 " + std::to_string(hits.size()) + " 对文本 token 重合度 ≥55%(高重合;是否冗余由主人裁决)",
        "重合高的两段并排看一眼,能合并的合并", "S03");
    finding.scope = "config";
    nlohmann::json pairs = nlohmann::json::array();
    for (const auto& hit : hits) {
        pairs.push_back(nlohmann::json{{"a", hit.a}, {"b", hit.b}, {"overlap_percent", hit.percent}});
    }
    finding.evidence.push_back(Ev("overlap_pairs", pairs));
    out.push_back(std::move(finding));
}

void AuditTokenShare(const PromptAuditFacts& facts, std::vector<Finding>& out) {
    if (facts.budget_tokens <= 0 || facts.total_context_tokens <= 0) {
        return;  // 预算未知,占比规则不判(§8.1 第 4 条的分母要先在)
    }
    const int total_percent =
        SharePercent(facts.total_context_tokens, facts.budget_tokens);
    if (total_percent >= 50) {
        Finding finding = MakeFinding(
            "prompt.token_share", FindingSeverity::Warning, FindingConfidence::High,
            "system+工具定义合计约占上下文预算 " + std::to_string(total_percent) +
                "%(超过 50% 线)",
            "裁工具面(延迟工具索引/裁 MCP)或精简模块;占比可定位见段级表", "S04");
        finding.scope = "config";
        finding.evidence.push_back(Ev("context_tokens", facts.total_context_tokens));
        finding.evidence.push_back(Ev("budget_tokens", facts.budget_tokens));
        finding.evidence.push_back(Ev("share_percent", total_percent));
        out.push_back(std::move(finding));
        return;
    }
    // 单段超预算 15%。
    std::int64_t biggest = 0;
    std::string biggest_id;
    for (const auto& segment : facts.segments) {
        if (segment.tokens > biggest) {
            biggest = segment.tokens;
            biggest_id = segment.segment_id;
        }
    }
    if (biggest > 0 && SharePercent(biggest, facts.budget_tokens) >= 15) {
        Finding finding = MakeFinding(
            "prompt.token_share", FindingSeverity::Info, FindingConfidence::High,
            "单段 " + biggest_id + " 约占上下文预算 " +
                std::to_string(SharePercent(biggest, facts.budget_tokens)) + "%(≥15%)",
            "大段拆薄或按能力开关分档;是否要紧由主人裁决", "S04");
        finding.scope = "config";
        finding.evidence.push_back(Ev("segment_id", biggest_id));
        finding.evidence.push_back(Ev("segment_tokens", biggest));
        finding.evidence.push_back(Ev("budget_tokens", facts.budget_tokens));
        out.push_back(std::move(finding));
    }
}

void AuditToolDescriptions(const std::vector<AuditToolDefinition>& tools,
                           std::vector<Finding>& out) {
    std::vector<std::string> long_tools;
    std::int64_t worst = 0;
    for (const auto& tool : tools) {
        const std::int64_t tokens =
            static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.description));
        worst = std::max(worst, tokens);
        if (tokens > kToolDescriptionTokenLimit) {
            long_tools.push_back(tool.name + "(" + std::to_string(tokens) + " tok)");
        }
    }
    if (!long_tools.empty()) {
        Finding finding = MakeFinding(
            "tool.description_bloat", FindingSeverity::Info, FindingConfidence::High,
            "有 " + std::to_string(long_tools.size()) + " 枚工具描述超过 " +
                std::to_string(kToolDescriptionTokenLimit) + " token",
            "长描述收短;动作边界(何时用/何时停)保留", "S05");
        finding.scope = "config";
        finding.evidence.push_back(Ev("long_descriptions", long_tools));
        out.push_back(std::move(finding));
    }
    // 描述彼此重复(同文):模型分不清两枚工具就该合或该改名。
    std::map<std::size_t, std::vector<std::string>> by_desc;
    for (const auto& tool : tools) {
        if (!tool.description.empty()) {
            by_desc[std::hash<std::string>{}(tool.description)].push_back(tool.name);
        }
    }
    std::vector<std::string> dup_groups;
    for (const auto& entry : by_desc) {
        if (entry.second.size() >= 2) {
            dup_groups.push_back(entry.second.front() + " 与 " +
                                 std::to_string(entry.second.size() - 1) + " 枚描述同文");
        }
    }
    if (!dup_groups.empty()) {
        Finding finding = MakeFinding(
            "tool.description_duplicate", FindingSeverity::Warning, FindingConfidence::High,
            "有 " + std::to_string(dup_groups.size()) + " 组工具描述完全相同",
            "同名同义工具合并;不同义则描述写清差别", "S06");
        finding.scope = "config";
        finding.evidence.push_back(Ev("duplicate_groups", dup_groups));
        out.push_back(std::move(finding));
    }
}

void AuditToolSchemas(const std::vector<AuditToolDefinition>& tools, std::vector<Finding>& out) {
    std::vector<std::string> shapeless;   // 缺 type=object / properties
    std::vector<std::string> no_required;
    std::vector<std::string> too_deep;
    std::vector<std::string> name_collisions;
    std::map<std::string, std::vector<std::string>> by_name;
    for (const auto& tool : tools) {
        by_name[tool.name].push_back(tool.source_kind.empty() ? "builtin" : tool.source_kind);
        if (tool.input_schema.is_null() || tool.input_schema.empty()) {
            shapeless.push_back(tool.name + "(没给 schema)");
            continue;
        }
        const std::string type = tool.input_schema.value("type", "");
        if (type != "object" || !tool.input_schema.contains("properties")) {
            shapeless.push_back(tool.name + "(type=" + (type.empty() ? "缺" : type) + ")");
            continue;
        }
        if (!tool.input_schema.contains("required")) {
            no_required.push_back(tool.name);
        }
        if (JsonDepth(tool.input_schema) > kSchemaDepthLimit) {
            too_deep.push_back(tool.name + "(" + std::to_string(JsonDepth(tool.input_schema)) + " 层)");
        }
    }
    for (const auto& entry : by_name) {
        if (entry.second.size() >= 2) {
            name_collisions.push_back(entry.first + "(" + std::to_string(entry.second.size()) + " 路注册)");
        }
    }
    if (!shapeless.empty()) {
        Finding finding = MakeFinding(
            "tool.schema_shape", FindingSeverity::Warning, FindingConfidence::High,
            "有 " + std::to_string(shapeless.size()) + " 枚工具 schema 缺 type=object 或 properties",
            "补齐 schema 形状;缺形状的参数模型只能瞎猜", "S07");
        finding.scope = "config";
        finding.evidence.push_back(Ev("shapeless", shapeless));
        out.push_back(std::move(finding));
    }
    if (!no_required.empty()) {
        Finding finding = MakeFinding(
            "tool.schema_required", FindingSeverity::Info, FindingConfidence::High,
            "有 " + std::to_string(no_required.size()) + " 枚工具 schema 没写 required",
            "必填字段写进 required;全可选也是一种口径,写明更好", "S07");
        finding.scope = "config";
        finding.evidence.push_back(Ev("no_required", no_required));
        out.push_back(std::move(finding));
    }
    if (!too_deep.empty()) {
        Finding finding = MakeFinding(
            "tool.schema_depth", FindingSeverity::Info, FindingConfidence::High,
            "有 " + std::to_string(too_deep.size()) + " 枚工具 schema 嵌套超过 " +
                std::to_string(kSchemaDepthLimit) + " 层",
            "深嵌拍平;深形状模型容易填错", "S07");
        finding.scope = "config";
        finding.evidence.push_back(Ev("too_deep", too_deep));
        out.push_back(std::move(finding));
    }
    if (!name_collisions.empty()) {
        Finding finding = MakeFinding(
            "tool.name_collision", FindingSeverity::Warning, FindingConfidence::High,
            "有 " + std::to_string(name_collisions.size()) + " 个工具名被多路注册撞车",
            "撞名的注册分家或改名;同名不同义,模型必错", "S08");
        finding.scope = "config";
        finding.evidence.push_back(Ev("collisions", name_collisions));
        out.push_back(std::move(finding));
    }
}

// §8.1 第 8 条:绝对禁令互撞。本地只抓明显模式(固定对仗词表),置信 low。
void AuditSuspectedConflicts(const StaticAuditInput& input, std::vector<Finding>& out) {
    struct ConflictPair {
        const char* category;
        std::vector<std::string> side_a;
        std::vector<std::string> side_b;
    };
    static const std::vector<ConflictPair> kPairs = {
        {"approval", {"必须先问", "先征得同意", "always ask first", "ask before"},
         {"直接执行", "无需确认", "不必问", "never ask", "直接做"}},
        {"command_run", {"禁止运行命令", "never run commands", "不许执行命令"},
         {"总是运行命令", "always run commands", "自动执行命令"}},
    };
    std::string corpus;
    for (const auto& [id, text] : input.segment_texts) {
        corpus += text;
        corpus += '\n';
    }
    corpus += input.soul_text;
    corpus += input.model_instructions_text;
    std::vector<std::string> fired;
    for (const auto& pair : kPairs) {
        if (ContainsAny(corpus, pair.side_a) && ContainsAny(corpus, pair.side_b)) {
            fired.push_back(std::string("对仗词两侧都出现:") + pair.category);
        }
    }
    if (fired.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "instruction.absolute_conflict", FindingSeverity::Warning, FindingConfidence::Low,
        "疑似绝对指令互撞(" + std::to_string(fired.size()) +
            " 组对仗词两侧都有;词表命中,语义须主人复核)",
        "两段并排看;真冲突则给其中一侧加触发条件", "S09");
    finding.scope = "config";
    finding.evidence.push_back(Ev("suspected_pairs", fired));
    finding.evidence.push_back(Ev("note", "本地规则只抓明显模式,confidence=low;复核后才算数"));
    out.push_back(std::move(finding));
}

// §8.1 第 9 条:口号密度。长文全是祈使句、无触发条件/停止线/验收词。
void AuditSloganHeavy(const StaticAuditInput& input, std::vector<Finding>& out) {
    static const std::vector<std::string> kImperatives = {"必须", "务必", "一定要", "不得",
                                                          "always", "never", "must", "should"};
    static const std::vector<std::string> kBoundaries = {"如果", "若",   "当",     "when", "if",
                                                         "直到", "验收", "停止",   "边界", "触发",
                                                         "条件", "until", "verify", "stop"};
    std::vector<std::string> heavy;
    for (const auto& [id, text] : input.segment_texts) {
        if (text.size() < 400) {
            continue;
        }
        if (ContainsAny(text, kBoundaries)) {
            continue;
        }
        int imperative = 0;
        for (const auto& marker : kImperatives) {
            std::size_t pos = 0;
            while ((pos = text.find(marker, pos)) != std::string::npos) {
                ++imperative;
                pos += marker.size();
            }
        }
        if (imperative >= 5) {
            heavy.push_back(id);
        }
    }
    if (heavy.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "instruction.slogan_heavy", FindingSeverity::Info, FindingConfidence::Low,
        "疑似口号段:" + std::to_string(heavy.size()) +
            " 个长段全是祈使句,没写到触发条件/停止线/验收(词法判定,语义须主人复核)",
        "给口号补触发条件与验收;没有边界的指令落不了地", "S10");
    finding.scope = "config";
    finding.evidence.push_back(Ev("segments", heavy));
    out.push_back(std::move(finding));
}

void AuditVolatilePlacement(const agent::PromptManifest& manifest, std::vector<Finding>& out) {
    // 非volatile段的最大 order,与 volatile 段的 order 比:volatile 后还有
    // 稳定段 = 动态内容插在稳定前缀中段(§8.1 第 10 条)。
    int max_stable_order = -1;
    for (const auto& segment : manifest.segments) {
        if (!segment.volatile_segment) {
            max_stable_order = std::max(max_stable_order, segment.order);
        }
    }
    std::vector<std::string> mid;
    for (const auto& segment : manifest.segments) {
        if (segment.volatile_segment && segment.order < max_stable_order) {
            mid.push_back(segment.segment_id);
        }
    }
    if (mid.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "prompt.volatile_mid_prefix", FindingSeverity::Warning, FindingConfidence::High,
        "有 " + std::to_string(mid.size()) + " 个动态段后面还有稳定段(动态内容插在稳定前缀中段,易断 cache)",
        "动态段(时间/cwd/工具清单)挪到前缀尾部;稳定段前置", "S11");
    finding.scope = "config";
    finding.evidence.push_back(Ev("volatile_segments_before_stable_tail", mid));
    finding.evidence.push_back(Ev("last_stable_order", max_stable_order));
    out.push_back(std::move(finding));
}

void AuditUserModuleDrift(const std::vector<agent::PromptModuleSource>& sources,
                          std::vector<Finding>& out) {
    std::vector<std::string> drifted;
    for (const auto& source : sources) {
        if (source.from_user_file && source.differs_from_embedded) {
            drifted.push_back(source.rel_path);
        }
    }
    if (drifted.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "prompt.user_module_drift", FindingSeverity::Info, FindingConfidence::High,
        "有 " + std::to_string(drifted.size()) + " 个用户模块已偏离内置版(差异内容本批不展开)",
        "升级后并排核对一遍; drift 是有意改动就留,是忘了跟就同步", "S12");
    finding.scope = "config";
    finding.evidence.push_back(Ev("drifted_modules", drifted));
    finding.counter_evidence.push_back(Ev("note", "differs_from_embedded 只说明不同,不说明谁对"));
    out.push_back(std::move(finding));
}

void AuditMcpWeight(const std::vector<AuditToolDefinition>& tools,
                    const PromptAuditFacts& facts, std::vector<Finding>& out) {
    std::int64_t mcp_tokens = 0;
    std::int64_t mcp_count = 0;
    for (const auto& tool : tools) {
        if (tool.source_kind == "mcp") {
            ++mcp_count;
            mcp_tokens += static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.name)) +
                          static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.description)) +
                          static_cast<std::int64_t>(agent::EstimateUtf8Tokens(
                              tool.input_schema.is_null() ? std::string()
                                                           : tool.input_schema.dump()));
        }
    }
    if (mcp_count == 0 || facts.total_context_tokens <= 0) {
        return;
    }
    const int share = SharePercent(mcp_tokens, facts.total_context_tokens);
    if (share < 10) {
        return;
    }
    Finding finding = MakeFinding(
        "tool.mcp_weight", FindingSeverity::Info, FindingConfidence::High,
        "MCP 工具 " + std::to_string(mcp_count) + " 枚常驻,估算 " +
            std::to_string(mcp_tokens) + " token,约占上下文 " + std::to_string(share) +
            "%(是否曾被调用,看 /prompt audit runtime)",
        "不常用的 MCP server 走按需挂载或裁掉", "S13");
    finding.scope = "config";
    finding.evidence.push_back(Ev("mcp_tool_count", mcp_count));
    finding.evidence.push_back(Ev("mcp_tool_tokens_estimated", mcp_tokens));
    finding.evidence.push_back(Ev("context_tokens_total", facts.total_context_tokens));
    out.push_back(std::move(finding));
}

// ---------------- runtime 规则 ----------------

// SummarizeRuntimeChanges 的定义在文件尾(公有,出匿名命名空间)。


void AuditSnapshotCoverage(const std::vector<RuntimeRequestView>& requests,
                           std::vector<Finding>& out) {
    std::vector<std::string> missing;
    for (const auto& view : requests) {
        if (!view.snapshot.has_value()) {
            missing.push_back(view.request_id);
        }
    }
    if (missing.empty()) {
        return;
    }
    Finding finding = MakeFinding(
        "prompt.snapshot_missing", FindingSeverity::Info, FindingConfidence::High,
        "有 " + std::to_string(missing.size()) + "/" + std::to_string(requests.size()) +
            " 笔请求没有可解析的 request snapshot(A1 前的旧账或缺 manifest;这些请求不进层变化分析)",
        "新请求都带 manifest;旧账不补造", "R01");
    finding.scope = "session";
    finding.evidence.push_back(Ev("requests_without_snapshot", missing));
    out.push_back(std::move(finding));
}

void AuditToolsetChurn(const std::vector<RuntimeRequestView>& requests,
                       const RuntimeChangeSummary& account, std::vector<Finding>& out) {
    if (account.comparable < 2 || account.toolset_changes < 2) {
        return;
    }
    std::vector<std::string> event_refs;
    const RuntimeRequestView* prev = nullptr;
    for (const auto& view : requests) {
        if (prev != nullptr && prev->snapshot.has_value() && view.snapshot.has_value() &&
            !prev->snapshot->request_shape.toolset_hash.empty() &&
            !view.snapshot->request_shape.toolset_hash.empty() &&
            prev->snapshot->request_shape.toolset_hash !=
                view.snapshot->request_shape.toolset_hash) {
            event_refs.push_back(view.event_id);
        }
        prev = &view;
    }
    Finding finding = MakeFinding(
        "cache.toolset_churn", FindingSeverity::Warning, FindingConfidence::High,
        "连续 " + std::to_string(account.comparable) + " 对可比较请求中,toolset_hash 变了 " +
            std::to_string(account.toolset_changes) + " 次(工具表在抖)",
        "固定工具注册与序列化次序;动态索引放前缀尾部", "R02");
    finding.scope = "session";
    finding.evidence.push_back(Ev("comparable_pairs", account.comparable));
    finding.evidence.push_back(Ev("tools_hash_changes", account.toolset_changes));
    for (const auto& ref : event_refs) {
        EvidenceItem item = Ev("prepared_event_id", ref);
        item.event_id = ref;
        finding.evidence.push_back(std::move(item));
    }
    finding.counter_evidence.push_back(
        Ev("note", "provider 未明报 cache 能力时,抖动是否真的花钱要看 usage 的 cache_read"));
    out.push_back(std::move(finding));
}

void AuditPrefixChurn(const RuntimeChangeSummary& account, std::vector<Finding>& out) {
    if (account.prefix_breaks_same_epoch < 2) {
        return;
    }
    Finding finding = MakeFinding(
        "cache.prefix_churn", FindingSeverity::Warning, FindingConfidence::Medium,
        "同一 cache_epoch 内稳定前缀变了 " +
            std::to_string(account.prefix_breaks_same_epoch) +
            " 次(本应追加-only;前缀改写会让 cache 整段作废)",
        "查稳定段的 rendered_hash 变化来源;改写历史的段挪出稳定前缀", "R03");
    finding.scope = "session";
    finding.evidence.push_back(Ev("prefix_breaks_same_epoch", account.prefix_breaks_same_epoch));
    finding.evidence.push_back(Ev("prefix_changes_total", account.prefix_changes));
    finding.counter_evidence.push_back(Ev(
        "note", "compact/显式清理/toolset 改变引起的前缀变属预期重建(A2 的 epoch 账另计)"));
    out.push_back(std::move(finding));
}

void AuditSegmentChurn(const RuntimeChangeSummary& account, std::vector<Finding>& out) {
    std::vector<std::pair<std::int64_t, std::string>> churners;
    for (const auto& [segment_id, count] : account.segment_changes) {
        if (count >= 2) {
            churners.emplace_back(count, segment_id);
        }
    }
    if (churners.empty()) {
        return;
    }
    std::sort(churners.begin(), churners.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) {
                      return a.first > b.first;
                  }
                  return a.second < b.second;
              });
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& [count, segment_id] : churners) {
        rows.push_back(nlohmann::json{{"segment_id", segment_id}, {"changes", count}});
    }
    Finding finding = MakeFinding(
        "prompt.segment_churn", FindingSeverity::Info, FindingConfidence::High,
        "有 " + std::to_string(churners.size()) +
            " 个自称稳定的段在连续请求间变了 ≥2 次(稳定段不该逐请求变)",
        "该段要么标 volatile(承认它动态),要么把动态内容抽出去", "R04");
    finding.scope = "session";
    finding.evidence.push_back(Ev("stable_segments_changed", rows));
    out.push_back(std::move(finding));
}

void AuditGrowthVersusCache(const std::vector<RuntimeRequestView>& requests,
                            std::vector<Finding>& out) {
    // 首末两笔有实测的请求:system token 涨、cache 命中率跌,同向才报,
    // 措辞只说"同向观察",不写因果(§8.2 只说发生了什么)。
    const RuntimeRequestView* first = nullptr;
    const RuntimeRequestView* last = nullptr;
    for (const auto& view : requests) {
        if (!view.snapshot.has_value() || !view.usage_reported ||
            view.total_input_tokens <= 0) {
            continue;
        }
        if (first == nullptr) {
            first = &view;
        }
        last = &view;
    }
    if (first == nullptr || last == nullptr || first == last) {
        return;
    }
    const std::int64_t first_system = first->snapshot->prompt_manifest.resolved_prompt_tokens_estimated;
    const std::int64_t last_system = last->snapshot->prompt_manifest.resolved_prompt_tokens_estimated;
    if (first_system <= 0 || last_system <= 0) {
        return;
    }
    const double growth =
        static_cast<double>(last_system - first_system) / static_cast<double>(first_system);
    const int first_ratio = SharePercent(first->cache_read_tokens, first->total_input_tokens);
    const int last_ratio = SharePercent(last->cache_read_tokens, last->total_input_tokens);
    if (growth < 0.3 || (first_ratio - last_ratio) < 25) {
        return;
    }
    Finding finding = MakeFinding(
        "prompt.growth_cache_miss", FindingSeverity::Warning, FindingConfidence::Medium,
        "system prompt 估算涨了 " + std::to_string(static_cast<int>(growth * 100)) +
            "%,同期 cache 命中率从 " + std::to_string(first_ratio) + "% 落到 " +
            std::to_string(last_ratio) + "%(同向观察,不是因果结论)",
        "查这段增长来自哪几段(段级表);把膨胀段按需化", "R05");
    finding.scope = "session";
    finding.evidence.push_back(Ev("first_system_tokens", first_system));
    finding.evidence.push_back(Ev("last_system_tokens", last_system));
    finding.evidence.push_back(Ev("first_cache_read_percent", first_ratio));
    finding.evidence.push_back(Ev("last_cache_read_percent", last_ratio));
    finding.evidence.push_back(Ev("first_event", first->event_id));
    finding.evidence.push_back(Ev("last_event", last->event_id));
    finding.counter_evidence.push_back(
        Ev("note", "TTL 过期、provider 波动也长这模样;不能凭这一条断 prompt 的罪"));
    out.push_back(std::move(finding));
}

}  // namespace

// 相邻请求的层变化账(公有:A4 分析器与功能信号复用)。
RuntimeChangeSummary SummarizeRuntimeChanges(const std::vector<RuntimeRequestView>& requests) {
    RuntimeChangeSummary account;
    const RuntimeRequestView* prev = nullptr;
    for (const auto& view : requests) {
        if (!view.snapshot.has_value()) {
            continue;  // 旧账没有 manifest,不比(另由 R01 点名)
        }
        if (prev != nullptr && prev->snapshot.has_value()) {
            account.comparable += 1;
            const agent::RequestSnapshotMetadata& a = *prev->snapshot;
            const agent::RequestSnapshotMetadata& b = *view.snapshot;
            const bool same_run = prev->run_id == view.run_id;
            if (!a.request_shape.toolset_hash.empty() && !b.request_shape.toolset_hash.empty() &&
                a.request_shape.toolset_hash != b.request_shape.toolset_hash) {
                account.toolset_changes += 1;
            }
            const bool same_epoch =
                prev->cache_epoch.has_value() && prev->cache_epoch == view.cache_epoch;
            if (!a.prompt_manifest.stable_prefix_hash.empty() &&
                !b.prompt_manifest.stable_prefix_hash.empty() &&
                a.prompt_manifest.stable_prefix_hash != b.prompt_manifest.stable_prefix_hash) {
                account.prefix_changes += 1;
                if (same_run && same_epoch) {
                    account.prefix_breaks_same_epoch += 1;
                }
            }
            if (same_run) {
                // 稳定段(非 volatile)的 rendered_hash 变化计数。
                std::map<std::string, std::string> prev_hashes;
                for (const auto& segment : a.prompt_manifest.segments) {
                    if (!segment.volatile_segment) {
                        prev_hashes[segment.segment_id] = segment.rendered_hash;
                    }
                }
                for (const auto& segment : b.prompt_manifest.segments) {
                    if (segment.volatile_segment) {
                        continue;
                    }
                    const auto it = prev_hashes.find(segment.segment_id);
                    if (it != prev_hashes.end() && it->second != segment.rendered_hash) {
                        account.segment_changes[segment.segment_id] += 1;
                    }
                }
            }
        }
        prev = &view;
    }
    return account;
}

nlohmann::json PromptAuditFacts::ToJson() const {
    nlohmann::json segments = nlohmann::json::array();
    for (const auto& segment : this->segments) {
        segments.push_back(nlohmann::json{{"segment_id", segment.segment_id},
                                          {"role", segment.role},
                                          {"source_kind", segment.source_kind},
                                          {"tokens_estimated", segment.tokens},
                                          {"order", segment.order},
                                          {"volatile", segment.volatile_segment}});
    }
    return nlohmann::json{
        {"system_tokens_estimated", system_tokens},
        {"soul_tokens_estimated", soul_tokens},
        {"model_instructions_tokens_estimated", model_instructions_tokens},
        {"tool_definition_tokens_estimated", tool_definition_tokens},
        {"tool_count", tool_count},
        {"total_context_tokens_estimated", total_context_tokens},
        {"context_budget_tokens", budget_tokens},
        {"segments", segments}};
}

std::vector<Finding> AuditPromptStatic(const StaticAuditInput& input, PromptAuditFacts* facts) {
    std::vector<Finding> out;
    PromptAuditFacts local;
    PromptAuditFacts& account = facts != nullptr ? *facts : local;
    account = PromptAuditFacts{};
    account.system_tokens = input.manifest.resolved_prompt_tokens_estimated;
    account.soul_tokens = input.manifest.soul.tokens_estimated;
    account.model_instructions_tokens = input.manifest.model_instructions.tokens_estimated;
    for (const auto& tool : input.tools) {
        account.tool_definition_tokens +=
            static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.name)) +
            static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.description)) +
            static_cast<std::int64_t>(agent::EstimateUtf8Tokens(
                tool.input_schema.is_null() ? std::string() : tool.input_schema.dump()));
    }
    account.tool_count = static_cast<std::int64_t>(input.tools.size());
    account.total_context_tokens = account.system_tokens + account.soul_tokens +
                                   account.model_instructions_tokens +
                                   account.tool_definition_tokens;
    account.budget_tokens = input.context_budget_tokens;
    for (const auto& segment : input.manifest.segments) {
        account.segments.push_back(PromptAuditFacts::SegmentFact{
            segment.segment_id, segment.role, segment.source_kind,
            segment.rendered_tokens_estimated, segment.order, segment.volatile_segment});
    }
    std::sort(account.segments.begin(), account.segments.end(),
              [](const PromptAuditFacts::SegmentFact& a, const PromptAuditFacts::SegmentFact& b) {
                  return a.order < b.order;
              });

    AuditOverrideChains(input.manifest, out);
    AuditDuplicateContent(input.manifest, out);
    AuditNgramOverlap(input, out);
    AuditTokenShare(account, out);
    AuditToolDescriptions(input.tools, out);
    AuditToolSchemas(input.tools, out);
    AuditSuspectedConflicts(input, out);
    AuditSloganHeavy(input, out);
    AuditVolatilePlacement(input.manifest, out);
    AuditUserModuleDrift(input.module_sources, out);
    AuditMcpWeight(input.tools, account, out);

    // finding_id 按规则码钉死(P-AUD-S02 这般):换 mode 重跑、单层看还是
    // 全看,id 不挪窝,explain 才对得上号。
    for (auto& finding : out) {
        const std::size_t colon = finding.rule_version.rfind(':');
        finding.finding_id =
            "P-AUD-" + (colon == std::string::npos
                            ? finding.rule_version
                            : finding.rule_version.substr(colon + 1));
    }
    return out;
}

std::vector<Finding> AuditPromptRuntime(const RuntimeAuditInput& input) {
    std::vector<Finding> out;
    if (input.requests.empty()) {
        return out;
    }
    const RuntimeChangeSummary account = SummarizeRuntimeChanges(input.requests);
    AuditSnapshotCoverage(input.requests, out);
    AuditToolsetChurn(input.requests, account, out);
    AuditPrefixChurn(account, out);
    AuditSegmentChurn(account, out);
    AuditGrowthVersusCache(input.requests, out);
    for (auto& finding : out) {
        const std::size_t colon = finding.rule_version.rfind(':');
        finding.finding_id =
            "P-AUD-" + (colon == std::string::npos
                            ? finding.rule_version
                            : finding.rule_version.substr(colon + 1));
    }
    return out;
}

RuntimeRequestsRead CollectRuntimeRequestsFromStreams(
    const std::vector<std::pair<std::string, std::vector<trajectory::EventEnvelope>>>& streams) {
    RuntimeRequestsRead read;
    bool any = false;
    for (const auto& [stream_name, envelopes] : streams) {
        // usage 对账:同一 stream 的 ProjectUsage samples,按 request+attempt 排。
        std::map<std::string, std::vector<const accounting::UsageSample*>> samples_by_request;
        const accounting::UsageProjection projection = accounting::ProjectUsage(envelopes);
        if (!projection.ok) {
            read.warnings.push_back("prompt.stream_usage_rejected: " + stream_name + ": " +
                                    projection.error_code);
            // usage 对不上不拦 prepared 侧的层变化分析;只是这些请求标没账。
        } else {
            for (const auto& sample : projection.samples) {
                samples_by_request[sample.request_id].push_back(&sample);
            }
        }
        for (auto& [request_id, list] : samples_by_request) {
            std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
                return a->attempt < b->attempt;
            });
        }
        const std::string run_id =
            envelopes.empty() ? stream_name : envelopes.front().run_id;
        std::map<std::string, int> occurrence_of;
        for (const auto& envelope : envelopes) {
            if (envelope.kind != trajectory::EventKind::ModelRequestPrepared) {
                continue;
            }
            any = true;
            RuntimeRequestView view;
            view.run_id = run_id;
            view.request_id = envelope.request_id.value_or("");
            view.event_id = envelope.event_id;
            view.purpose = envelope.payload.value("purpose", "unknown");
            const auto snapshot_json = envelope.payload.find("request_snapshot_ref");
            if (snapshot_json != envelope.payload.end() && snapshot_json->is_object()) {
                std::string error;
                auto snapshot =
                    agent::RequestSnapshotMetadata::FromJsonStrict(*snapshot_json, &error);
                if (snapshot.has_value()) {
                    view.snapshot = std::move(snapshot);
                } else {
                    read.warnings.push_back("prompt.snapshot_parse_failed: " + envelope.event_id +
                                            ": " + error);
                }
            }
            // usage 关联:prepared 是第 N 次出现 → attempt N 的 sample。
            const int occurrence = ++occurrence_of[view.request_id];
            const auto it = samples_by_request.find(view.request_id);
            if (it != samples_by_request.end() &&
                static_cast<std::size_t>(occurrence) <= it->second.size()) {
                const accounting::UsageSample& sample = *it->second[occurrence - 1];
                view.usage_reported = sample.usage.has_value();
                if (sample.usage.has_value()) {
                    view.total_input_tokens = sample.total_input_tokens;
                    view.cache_read_tokens = sample.usage->cache_read_tokens;
                    view.output_tokens = sample.usage->output_tokens;
                }
                view.cache_epoch = sample.cache_epoch;
            }
            read.requests.push_back(std::move(view));
        }
        if (read.session_id.empty() && !envelopes.empty()) {
            read.session_id = envelopes.front().session_id;
        }
    }
    if (read.session_id.empty() && !streams.empty()) {
        read.session_id = streams.front().first;
    }
    read.ok = true;
    if (!any) {
        read.warnings.push_back("prompt.no_prepared_events: 这场 session 没有模型请求账");
    }
    return read;
}

RuntimeRequestsRead CollectRuntimeRequests(const std::filesystem::path& session_dir) {
    RuntimeRequestsRead read;
    const auto stream_files = accounting::ListSessionStreams(session_dir);
    if (!stream_files.has_value()) {
        read.error_code = "prompt.session_not_found";
        read.message = "没有这场 session:" + session_dir.string();
        return read;
    }
    std::vector<std::pair<std::string, std::vector<trajectory::EventEnvelope>>> streams;
    for (const auto& path : *stream_files) {
        const auto lines = trajectory::ReadJournalLines(path);
        if (!lines.has_value()) {
            read.warnings.push_back("prompt.stream_unreadable: " + path.filename().string());
            continue;
        }
        std::vector<trajectory::EventEnvelope> envelopes;
        bool stream_ok = true;
        for (const auto& line : *lines) {
            const auto parsed = nlohmann::json::parse(line, nullptr, false);
            if (parsed.is_discarded()) {
                stream_ok = false;
                break;
            }
            trajectory::EventEnvelope envelope;
            if (trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value()) {
                stream_ok = false;
                break;
            }
            envelopes.push_back(std::move(envelope));
        }
        if (!stream_ok) {
            read.warnings.push_back("prompt.stream_rejected: " + path.filename().string());
            continue;
        }
        streams.emplace_back(path.stem().string(), std::move(envelopes));
    }
    RuntimeRequestsRead collected = CollectRuntimeRequestsFromStreams(streams);
    collected.warnings.insert(collected.warnings.begin(), read.warnings.begin(),
                              read.warnings.end());
    return collected;
}

}  // namespace lubancode::insights
