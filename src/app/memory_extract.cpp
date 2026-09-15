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

namespace {

// 转写里各部件的截断阈值。
constexpr std::size_t kMaxTextBytes = 4 * 1024;
constexpr std::size_t kMaxToolInputBytes = 300;
constexpr std::size_t kMaxToolResultBytes = 240;

std::string ClipBytes(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    text.resize(lubancode::platform::Utf8PrefixBoundary(text, max_bytes));
    return text + "...(截断)";
}

int TaskTypeScore(const std::string& haystack, std::initializer_list<const char*> needles) {
    int score = 0;
    for (const char* needle : needles) {
        if (haystack.find(needle) != std::string::npos) ++score;
    }
    return score;
}

}  // namespace

std::string ClassifyTaskType(const std::string& user_text, const std::vector<std::string>& tool_names) {
    // 工具名拼进判词:调过 read_file/search 偏调研,调过 write/edit/run 偏
    // 修代码。纯词法,不打请求,错了顶多总结侧重偏一点,不伤主链。
    std::string joined_tools;
    for (const std::string& name : tool_names) joined_tools += name + " ";
    const std::string haystack = user_text + "\n" + joined_tools;

    struct Scored {
        const char* name;
        int score;
    };
    const Scored scored[] = {
        {"config", TaskTypeScore(haystack, {"安装", "依赖", "环境", "install", "pip ", "npm ", "uv ", "uv\n",
                                            "conda", "venv", "版本", "编译", "构建", "build", "cmake",
                                            "package.json", "pyproject", "portable", "virtualenv"})},
        {"docs", TaskTypeScore(haystack, {"文档", "README", "readme", "注释", "说明书写", "document", "doc ",
                                          "文档化", "写份", "写一份"})},
        {"research", TaskTypeScore(haystack, {"看看", "在哪", "读一", "分析", "调研", "为什么", "是怎么回事",
                                              "梳理", "找一找", "搜一", "read_file", "search", "web_fetch",
                                              "web_search"})},
        {"code", TaskTypeScore(haystack, {"修复", "实现", "重构", "改一", "改掉", "加个", "加一", "删掉", "bug",
                                          "fix", "报错", "崩了", "write_file", "edit_file", "run_command",
                                          "lsp"})},
    };
    const Scored* best = nullptr;
    for (const Scored& item : scored) {
        if (item.score > 0 && (best == nullptr || item.score > best->score)) {
            best = &item;
        }
    }
    if (best == nullptr) return "other";
    return best->name;
}

std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes) {
    std::string out;
    const auto append = [&out, max_bytes](std::string line) {
        if (out.size() >= max_bytes) return;
        if (out.size() + line.size() + 1 > max_bytes) {
            const std::size_t room = max_bytes - out.size() - 1;
            if (room > 20) line.resize(lubancode::platform::Utf8PrefixBoundary(line, room));
            else line.clear();
        }
        if (!line.empty()) {
            if (!out.empty()) out += "\n";
            out += line;
        }
    };

    for (const api::Message& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                if (message.role == api::Role::User) {
                    append("[用户] " + ClipBytes(text->text, kMaxTextBytes));
                } else {
                    append("[助手] " + ClipBytes(text->text, kMaxTextBytes));
                }
            } else if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                append("[工具调用] " + use->name + "(" + ClipBytes(use->input.dump(), kMaxToolInputBytes) + ")");
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                append(std::string("[工具结果") + (result->is_error ? ",失败" : "") + "] " +
                       ClipBytes(result->content, kMaxToolResultBytes));
            }
            // ThinkingBlock/ImageBlock 不进转写。
        }
    }
    return out;
}

std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type) {
    const std::string base = agent::ModuleTextByPath(prompts_dir, "features/memory-summary-base.md");
    std::string typed = agent::ModuleTextByPath(prompts_dir, "features/memory-summary-" + task_type + ".md");
    if (typed.empty()) {
        typed = agent::ModuleTextByPath(prompts_dir, "features/memory-summary-other.md");
    }
    if (base.empty()) return typed;
    if (typed.empty()) return base;
    return base + "\n\n" + typed;
}

// ---- 解析收口(P0-A/P0-B):模型文本按不可信输入 --------------------------
//
// 旧法"取首个 { 到末个 } 再严格解析"有三处不定:字符串里的花括号会截错
// 片段、多对象会被拼成一段语法怪胎、外层截断但内层已有 } 时误收半截。
// 新法分三步,每步都有确定结果:
//   1. 先验整段 UTF-8(合法中文落在 JSON 字符串外 ≠ 输入字节坏了,分开报);
//   2. 按明确规则收 JSON 候选段(纯 JSON / 单层围栏 / 无歧义前后说明);
//   3. 候选段过 nlohmann 严格解析 + 显式判型的字段合同。

namespace {

// 首尾空白剥掉(ASCII 空白与全角空格 U+3000),返回剥后子串,并把原文字节
// 偏移写回 begin_out。按完整序列剥,不劈半个字;遇坏序列停手——那是
// UTF-8 预检的事,这里不抢着报。
std::string_view TrimView(const std::string& text, std::size_t* begin_out) {
    constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80";
    const auto blank_width = [&text, &kIdeographicSpace](std::size_t i) -> std::size_t {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) return std::isspace(c) != 0 ? 1 : 0;
        if (text.compare(i, kIdeographicSpace.size(), kIdeographicSpace) == 0) {
            return kIdeographicSpace.size();
        }
        return 0;
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end) {
        const std::size_t width = blank_width(begin);
        if (width == 0) break;
        begin += width;
    }
    while (end > begin) {
        if (end >= kIdeographicSpace.size() &&
            text.compare(end - kIdeographicSpace.size(), kIdeographicSpace.size(), kIdeographicSpace) == 0) {
            end -= kIdeographicSpace.size();
            continue;
        }
        const auto c = static_cast<unsigned char>(text[end - 1]);
        if (c < 0x80 && std::isspace(c) != 0) {
            --end;
            continue;
        }
        break;
    }
    *begin_out = begin;
    return std::string_view(text).substr(begin, end - begin);
}

// 单层代码围栏:view 形如 "```json\n...\n```" 或 "```\n...\n```"。剥出的
// 内层(原文字节系)不含围栏行;嵌套/多围栏/围栏外还有话一律不剥,交上层
// 按前后说明的歧义规则拒绝。
bool StripSingleFence(const std::string& text, std::string_view view, std::size_t* begin_out,
                      std::size_t* len_out) {
    if (!view.starts_with("```")) return false;
    const std::size_t head_newline = view.find('\n');
    if (head_newline == std::string_view::npos) return false;
    const std::string_view lang = view.substr(3, head_newline - 3);
    for (const char c : lang) {
        const auto letter = static_cast<unsigned char>(c);
        const bool word_char = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
                               (letter >= '0' && letter <= '9') || letter == '-' || letter == '_';
        if (!word_char) return false;  // 围栏头行不是语言标记,不当围栏剥
    }
    const std::string_view body = view.substr(head_newline + 1);
    const std::size_t closing = body.rfind("```");
    if (closing == std::string_view::npos) return false;
    if (body.find("```") != closing) return false;  // 嵌套/多围栏:歧义,不剥
    const std::string_view after = body.substr(closing + 3);  // 围栏之后
    for (const char c : after) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) return false;  // 围栏外还有话
    }
    const std::size_t inner_offset = (view.data() - text.data()) + head_newline + 1;
    const std::string inner_owned(body.substr(0, closing));
    std::size_t inner_begin = 0;
    const std::string_view trimmed = TrimView(inner_owned, &inner_begin);
    *begin_out = inner_offset + inner_begin;
    *len_out = trimmed.size();
    return true;
}

// 字符串感知配对扫描:从 text[begin](须是 '{')起,返回配对 '}' 的原文
// 偏移;不闭合返回 npos。字符串内的 { } " 与 \" 转义全部跳过——正文里
// 提到花括号不会把扫描带沟里。
std::size_t MatchBrace(const std::string& text, std::size_t begin) {
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// 已知"长度截断"结束原因(小写比对):openai 的 length、anthropic 的
// max_tokens、若干中转的 max_output_tokens。未知/缺失不在这判——交解析,
// 诊断单列。
bool IsTruncationStopReason(const std::string& stop_reason) {
    std::string lowered;
    lowered.reserve(stop_reason.size());
    for (const char c : stop_reason) {
        const auto letter = static_cast<unsigned char>(c);
        lowered.push_back(letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : c);
    }
    return lowered == "max_tokens" || lowered == "length" || lowered == "max_output_tokens";
}

// schema_invalid 的构造捷径(带字段路径)。
ExtractionError SchemaError(const std::string& path, const std::string& detail) {
    ExtractionError error;
    error.code = ExtractionErrorCode::SchemaInvalid;
    error.field_path = path;
    error.message = "抽取输出字段不符合同: " + path + " " + detail;
    return error;
}

// 字符串字段:存在则必须 is_string,否则按合同拒整次;不存在返回 nullopt。
// 禁止静默类型转换——value(key, default) 那类兜底不许再回来。
std::optional<std::string> ContractString(const nlohmann::json& parent, const char* key,
                                          const std::string& path, ExtractionError* error) {
    if (!parent.contains(key)) return std::nullopt;
    const nlohmann::json& field = parent.at(key);
    if (!field.is_string()) {
        *error = SchemaError(path, "必须是 string,实际是 " + std::string(field.type_name()));
        return std::nullopt;
    }
    return field.get<std::string>();
}

}  // namespace

const char* ExtractionErrorCodeName(ExtractionErrorCode code) {
    switch (code) {
        case ExtractionErrorCode::SyntaxInvalid: return "syntax_invalid";
        case ExtractionErrorCode::Utf8Invalid: return "utf8_invalid";
        case ExtractionErrorCode::SchemaInvalid: return "schema_invalid";
        case ExtractionErrorCode::OutputTruncated: return "output_truncated";
        case ExtractionErrorCode::EmptyOutput: return "empty_output";
        case ExtractionErrorCode::TransportFailed: return "transport_failed";
        case ExtractionErrorCode::DeadlineTimeout: return "deadline_timeout";
        case ExtractionErrorCode::RouteMiss: return "route_miss";
    }
    return "other";
}

const nlohmann::json& MemoryExtractionOutputSchema() {
    static const nlohmann::json schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"task_type", {{"type", "string"}}},
          {"summary", {{"type", "string"}}},
          {"retrieval_terms", {{"type", "array"}}},
          {"candidates", {{"type", "array"}}}}},
        {"required", nlohmann::json::array({"task_type", "summary"})},
    };
    return schema;
}

std::expected<MemoryExtraction, ExtractionError> ParseExtractionJson(const std::string& text) {
    // 1) 整段 UTF-8 预检:先分清"输入字节坏了"与"合法中文落在 JSON 字符串
    //    外"。nlohmann 的 parse 也会拒非法 UTF-8,但那混在语法错里分不出类。
    const std::size_t invalid_offset = lubancode::platform::FirstInvalidUtf8Offset(text);
    if (invalid_offset != std::string::npos) {
        ExtractionError error;
        error.code = ExtractionErrorCode::Utf8Invalid;
        error.utf8_valid = false;
        error.error_offset = invalid_offset;
        error.message = "抽取输出不是合法 UTF-8(首个坏字节偏移 " + std::to_string(invalid_offset) +
                        ",共 " + std::to_string(text.size()) + " 字节)";
        return std::unexpected(error);
    }

    // 2) 收 JSON 候选段(原文字节系 [begin, begin+length)):
    //    a. 剥空白后整段以 '{' 开头 → 候选 = 剥后整段(纯 JSON 形态);
    //    a'. 剥空白后整段以 '[' 开头且以 ']' 结尾 → 顶层数组,parse 整段后
    //        按"顶层必须 object"的合同拒(不是语法错,是字段合同错);
    //    b. 整段被单个代码围栏包住 → 剥围栏,内层按 a 判(内层必须是纯 JSON);
    //    c. 其余:首个 '{' 起做字符串感知配对扫描;前导或尾巴带结构字符
    //       ('{'/'['——多对象、数组、嵌套围栏都是歧义)、扫描不闭合(半截)
    //       一律拒;首尾只是纯文字说明(无歧义)的兼容放行,规则钉死。
    std::size_t begin = 0;
    std::size_t length = 0;
    std::string_view view = TrimView(text, &begin);
    bool is_array_shape = !view.empty() && view.front() == '[' && view.back() == ']';
    if (!view.empty() && view.front() == '{') {
        length = view.size();  // 纯 JSON 形态
    } else if (is_array_shape) {
        length = view.size();  // 顶层数组:整段 parse,交给顶层合同去拒
    } else {
        std::size_t fenced_begin = 0;
        std::size_t fenced_len = 0;
        if (!view.empty() && StripSingleFence(text, view, &fenced_begin, &fenced_len) &&
            fenced_len > 0 && text[fenced_begin] == '{') {
            begin = fenced_begin;  // 围栏形态:内层是纯 JSON
            length = fenced_len;
        } else {
            // 前后说明形态。
            const std::size_t first_brace = text.find('{');
            ExtractionError error;
            error.code = ExtractionErrorCode::SyntaxInvalid;
            if (first_brace == std::string::npos) {
                error.message = "抽取输出里找不到 JSON object";
                return std::unexpected(error);
            }
            std::size_t lead_offset = 0;
            const std::string lead = text.substr(0, first_brace);
            if (TrimView(lead, &lead_offset).find('[') != std::string_view::npos) {
                error.message = "抽取输出带结构化前导(数组/列表),拒绝选取(首个 '{' 偏移 " +
                                std::to_string(first_brace) + ")";
                error.error_offset = first_brace;
                return std::unexpected(error);
            }
            const std::size_t matched = MatchBrace(text, first_brace);
            if (matched == std::string::npos) {
                error.message = "抽取输出的 JSON object 未闭合(半截输出,首个 '{' 偏移 " +
                                std::to_string(first_brace) + ")";
                error.error_offset = first_brace;
                return std::unexpected(error);
            }
            std::size_t tail_offset = 0;
            const std::string tail = text.substr(matched + 1);
            const std::string_view tail_view = TrimView(tail, &tail_offset);
            if (tail_view.find('{') != std::string_view::npos ||
                tail_view.find('[') != std::string_view::npos) {
                // 尾巴带结构字符:多对象/数组/续写,歧义,拒。
                error.message = "抽取输出带多对象或结构化尾随内容,拒绝选取(首个 '{' 偏移 " +
                                std::to_string(first_brace) + ",配对 '}' 偏移 " +
                                std::to_string(matched) + ")";
                error.error_offset = matched + 1;
                return std::unexpected(error);
            }
            // 尾巴只是纯文字说明(如"以上。"):无歧义,兼容放行。
            begin = first_brace;
            length = matched - first_brace + 1;
        }
    }

    // 3) 严格解析候选段;异常只取类别与字节位,不透传 last read 片段
    //    (它可能带半个多字节字符,渲染成 '\xEF' 替换符)。
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.substr(begin, length));
    } catch (const nlohmann::json::parse_error& e) {
        ExtractionError error;
        error.code = ExtractionErrorCode::SyntaxInvalid;
        error.error_offset = begin + (e.byte < text.size() ? e.byte : text.size());
        error.message = "抽取输出不是合法 JSON(错误字节偏移 " + std::to_string(error.error_offset) +
                        ",候选段 " + std::to_string(length) + " 字节,全文 " +
                        std::to_string(text.size()) + " 字节)";
        return std::unexpected(error);
    } catch (const nlohmann::json::exception& e) {
        // parse 之外的 nlohmann 异常(理论到不了,防御收口):不把 e.what() 带
        // 出去,只记 id 供对照。
        ExtractionError error;
        error.code = ExtractionErrorCode::SyntaxInvalid;
        error.message = std::string("抽取输出解析异常(json exception id ") + std::to_string(e.id) + ")";
        return std::unexpected(error);
    }
    if (!root.is_object()) {
        ExtractionError error = SchemaError("", "顶层必须是 object,实际是 " +
                                                    std::string(root.type_name()));
        return std::unexpected(error);
    }

    // 4) 字段合同:显式判型,全程先 contains 再取;合同外的类型错拒整次,
    //    业务无效(枚举外值、空 title/content、超预算正文)跳过该条。
    MemoryExtraction extraction;
    try {
        ExtractionError contract_error;
        const auto task_type = ContractString(root, "task_type", "task_type", &contract_error);
        if (task_type.has_value()) {
            extraction.task_type = *task_type;
            if (extraction.task_type != "code" && extraction.task_type != "research" &&
                extraction.task_type != "config" && extraction.task_type != "docs") {
                extraction.task_type = "other";  // 枚举外值:业务宽容,不拒整次
            }
        } else if (root.contains("task_type")) {
            return std::unexpected(contract_error);
        } else {
            return std::unexpected(SchemaError("task_type", "必填字段缺失"));
        }
        const auto summary = ContractString(root, "summary", "summary", &contract_error);
        if (summary.has_value()) {
            if (summary->empty()) {
                return std::unexpected(SchemaError("summary", "必填字段为空串"));
            }
            extraction.summary = *summary;
        } else if (root.contains("summary")) {
            return std::unexpected(contract_error);
        } else {
            return std::unexpected(SchemaError("summary", "必填字段缺失"));
        }

        if (root.contains("retrieval_terms")) {
            const nlohmann::json& terms = root.at("retrieval_terms");
            if (!terms.is_array()) {
                return std::unexpected(
                    SchemaError("retrieval_terms", "必须是 array,实际是 " + std::string(terms.type_name())));
            }
            for (const auto& item : terms) {
                if (item.is_string() && extraction.retrieval_terms.size() < 8) {
                    extraction.retrieval_terms.push_back(item.get<std::string>());
                }
            }
        }

        if (root.contains("candidates")) {
            const nlohmann::json& candidates = root.at("candidates");
            if (!candidates.is_array()) {
                return std::unexpected(SchemaError(
                    "candidates", "必须是 array,实际是 " + std::string(candidates.type_name())));
            }
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                if (extraction.candidates.size() >= 3) break;
                const nlohmann::json& item = candidates.at(index);
                const std::string base = "candidates[" + std::to_string(index) + "]";
                if (!item.is_object()) {
                    return std::unexpected(
                        SchemaError(base, "必须是 object,实际是 " + std::string(item.type_name())));
                }
                ProposedCandidate candidate;
                const auto kind = ContractString(item, "kind", base + ".kind", &contract_error);
                if (kind.has_value()) {
                    candidate.kind = *kind;
                } else if (item.contains("kind")) {
                    return std::unexpected(contract_error);
                }
                if (candidate.kind != "fact" && candidate.kind != "preference" &&
                    candidate.kind != "feedback") {
                    continue;  // kind 缺失或枚举外值:无效业务候选,沿既有规则跳过
                }
                const auto title = ContractString(item, "title", base + ".title", &contract_error);
                if (title.has_value()) {
                    candidate.title = *title;
                } else if (item.contains("title")) {
                    return std::unexpected(contract_error);
                }
                const auto candidate_summary =
                    ContractString(item, "summary", base + ".summary", &contract_error);
                if (candidate_summary.has_value()) {
                    candidate.summary = *candidate_summary;
                } else if (item.contains("summary")) {
                    return std::unexpected(contract_error);
                }
                const auto content = ContractString(item, "content", base + ".content", &contract_error);
                if (content.has_value()) {
                    candidate.content = *content;
                } else if (item.contains("content")) {
                    return std::unexpected(contract_error);
                }
                const auto confidence =
                    ContractString(item, "confidence", base + ".confidence", &contract_error);
                if (confidence.has_value()) {
                    candidate.confidence = *confidence;
                } else if (item.contains("confidence")) {
                    return std::unexpected(contract_error);
                }
                if (!candidate.confidence.empty() && candidate.confidence != "user-stated" &&
                    candidate.confidence != "verified" && candidate.confidence != "inferred") {
                    candidate.confidence.clear();  // 枚举外值清洗,下面补默认
                }
                if (candidate.confidence.empty()) candidate.confidence = "inferred";
                // 时间线锚点:材料里明确给出的日期才留;形状不像日期(模型编的
                // 相对时间、口语时间)一律落空,不造假也不拦整条候选。
                const auto occurred =
                    ContractString(item, "occurred_at", base + ".occurred_at", &contract_error);
                if (occurred.has_value()) {
                    if (memory::LooksLikeMemoryDate(*occurred)) candidate.occurred_at = *occurred;
                } else if (item.contains("occurred_at")) {
                    return std::unexpected(contract_error);
                }
                // 候选正文预算(P1-A):与写路上限(8 KiB)对齐;超长不截断不
                // 静默改写,整条跳过——先减冗长输出,不动请求的 max_tokens。
                if (candidate.title.empty() || candidate.content.empty()) continue;
                if (candidate.content.size() > kMaxCandidateContentBytes) continue;
                if (item.contains("keywords")) {
                    const nlohmann::json& keywords = item.at("keywords");
                    if (!keywords.is_array()) {
                        return std::unexpected(SchemaError(
                            base + ".keywords",
                            "必须是 array,实际是 " + std::string(keywords.type_name())));
                    }
                    for (const auto& keyword : keywords) {
                        if (keyword.is_string()) candidate.keywords.push_back(keyword.get<std::string>());
                    }
                }
                if (item.contains("paths")) {
                    const nlohmann::json& paths = item.at("paths");
                    if (!paths.is_array()) {
                        return std::unexpected(SchemaError(
                            base + ".paths", "必须是 array,实际是 " + std::string(paths.type_name())));
                    }
                    for (const auto& path : paths) {
                        if (path.is_string()) candidate.paths.push_back(path.get<std::string>());
                    }
                }
                extraction.candidates.push_back(std::move(candidate));
            }
        }
    } catch (const nlohmann::json::exception& e) {
        // 显式判型后理论到不了;收口保险——type_error 不许越过抽取层抛出。
        ExtractionError error;
        error.code = ExtractionErrorCode::SchemaInvalid;
        error.message = std::string("抽取输出字段提取异常(json exception id ") + std::to_string(e.id) + ")";
        return std::unexpected(error);
    }
    return extraction;
}

std::expected<MemoryExtraction, ExtractionError> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs,
                                                                 const std::string& reasoning_effort,
                                                                 agent::BackgroundCallAccounting* accounting) {
    // 采样走 SampleModel 原语(批一·病四):攒流/usage/兜错/看门狗的路只有
    // 一份,这里只剩提示拼装与解析。两条入口(RunMemoryExtraction 与
    // ModelRouterService::Sample 链)共用 FinishMemoryExtraction 收口——
    // 同一文本同一结果。
    agent::SampleRequest sample;
    sample.model = model;
    sample.system = system_prompt;
    sample.reasoning_effort = reasoning_effort;
    sample.output_schema = MemoryExtractionOutputSchema();
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{transcript});
    sample.messages.push_back(std::move(message));
    sample.max_tokens = 1500;

    agent::SampleOptions sample_options;
    sample_options.timeout_secs = timeout_secs;
    const agent::SampleResult sampled = agent::SampleModel(backend, sample, sample_options);

    // usage 出账(分角色记账):抽取这轮采样不混普通 turn 的账。
    if (accounting != nullptr) {
        accounting->usage.input_tokens += sampled.usage.input_tokens;
        accounting->usage.cache_read_tokens += sampled.usage.cache_read_tokens;
        accounting->usage.cache_creation_tokens += sampled.usage.cache_creation_tokens;
        accounting->usage.output_tokens += sampled.usage.output_tokens;
        accounting->usage.output_reasoning_tokens += sampled.usage.output_reasoning_tokens;
        accounting->usage_reported = sampled.usage_reported;
        accounting->duration_ms = sampled.duration_ms;
    }

    return FinishMemoryExtraction(sampled);
}

std::expected<MemoryExtraction, ExtractionError> FinishMemoryExtraction(const agent::SampleResult& sampled) {
    // 模型文本按不可信输入:每种失败都带稳定分类与定位诊断,不透传库异常。
    if (!sampled.ok) {
        ExtractionError error;
        if (sampled.error.kind == api::ErrorKind::Cancelled && sampled.error.api_code == "local_deadline") {
            // 本地超时预算到点(取消误报 ESC 单 Bug 1):单独分类,不与网络
            // 失败混账;终端按预算另起一行提示,这里只给不带按键指控的短文案。
            error.code = ExtractionErrorCode::DeadlineTimeout;
            error.message = "记忆抽取超过本地超时预算,本轮跳过";
        } else {
            error.code = ExtractionErrorCode::TransportFailed;
            // 错误消息来自 wire,可能夹任意字节——出口先消毒,保证终端文案是
            // 合法 UTF-8。
            error.message = "抽取请求失败: " + lubancode::platform::SanitizeExternalText(sampled.error.message);
        }
        error.request_id = sampled.provider_response_id;
        error.body_bytes = sampled.text.size();
        error.stop_reason = sampled.stop_reason;
        return std::unexpected(error);
    }
    if (sampled.text.empty()) {
        ExtractionError error;
        error.code = ExtractionErrorCode::EmptyOutput;
        error.message = "抽取输出为空";
        error.request_id = sampled.provider_response_id;
        error.stop_reason = sampled.stop_reason;
        return std::unexpected(error);
    }
    if (IsTruncationStopReason(sampled.stop_reason)) {
        // 已知长度截断:即便剩余文本碰巧拼得出合法 JSON 也不采——半截总结
        // 半截候选,宁缺毋滥。未知/缺失结束原因不在这判死,交解析,诊断单列。
        ExtractionError error;
        error.code = ExtractionErrorCode::OutputTruncated;
        error.message = "抽取输出被截断(结束原因: " + sampled.stop_reason + ")";
        error.request_id = sampled.provider_response_id;
        error.body_bytes = sampled.text.size();
        error.stop_reason = sampled.stop_reason;
        return std::unexpected(error);
    }
    auto parsed = ParseExtractionJson(sampled.text);
    if (!parsed.has_value()) {
        ExtractionError error = parsed.error();
        error.request_id = sampled.provider_response_id;
        error.body_bytes = sampled.text.size();
        error.stop_reason = sampled.stop_reason;
        // SampleModel 的 output_schema 复检账并进诊断(P1-A 消费 schema_ok):
        // 复检与字段合同同一份 schema,复检挂了而解析给出权威分类,两账并报。
        if (!sampled.schema_ok) error.schema_check_error = sampled.schema_error;
        return std::unexpected(error);
    }
    return parsed;
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
    return ExtractionErrorCodeName(error.code);
}

std::string StableExtractErrorCode(const std::string& error) {
    // 旧文案版(保留):ExtractTurnMemory 失败路的固定文案(编译期字面量);
    // 认不出落 other。新文案以"抽取输出"开头,这把尺子继续量得准。
    if (error.starts_with("cheap 路由找不到 provider")) return "route_miss";
    if (error.starts_with("抽取输出为空")) return "empty_output";
    if (error.starts_with("抽取输出")) return "parse_failed";  // 不是合法 JSON / 找不到 object
    return "other";
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
    state_.turn_id.clear();  // 回合间的写路回执(slash 命令)不带回合号
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
        payload["auto_written"] = outcome.auto_written;
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
        payload["autoWritten"] = outcome.auto_written;
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
