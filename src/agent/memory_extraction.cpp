// memory_extract.hpp 的实现。任务分型、转写压缩与 JSON 解析全是纯函数,
// 好单测;只有 RunMemoryExtraction 碰网络。

#include "agent/memory_extraction.hpp"

#include <algorithm>
#include <cctype>
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

namespace lubancode::agent::memory_extraction {

namespace {

// 转写里各部件的截断阈值。
constexpr std::size_t kMaxUserBytes = 2 * 1024;
constexpr std::size_t kMaxFinalAnswerBytes = 3 * 1024;
constexpr std::size_t kMaxToolsBytes = 2 * 1024;
constexpr std::size_t kMaxRecentTools = 6;
constexpr std::size_t kMaxToolInputBytes = 160;
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
    const auto append = [](std::string& out, const std::string& text, std::size_t cap) {
        if (text.empty() || out.size() >= cap) return;
        if (!out.empty()) out += "\n";
        const auto room = cap - out.size();
        const std::string marker = "...[截断]";
        const bool marked = text.size() > room && room >= marker.size();
        const auto limit = marked ? room - marker.size() : std::min(room, text.size());
        out.append(text, 0, platform::Utf8PrefixBoundary(text, limit));
        if (marked) out += marker;
    };
    std::string user;
    std::string final_answer;
    struct ToolExcerpt { std::string id; std::string text; };
    std::vector<ToolExcerpt> tools;
    std::size_t tool_count = 0;
    for (const auto& message : messages) {
        std::string assistant;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                if (message.role == api::Role::User) {
                    append(user, text->text, kMaxUserBytes);
                } else if (message.role == api::Role::Assistant) {
                    append(assistant, text->text, kMaxFinalAnswerBytes);
                }
            } else if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                // Never serialize content/patch/body or arbitrary nested inputs.
                nlohmann::json locator = nlohmann::json::object();
                for (const auto* key : {"path", "file_path", "command", "cmd", "query", "pattern", "url", "symbol"}) {
                    if (use->input.is_object() && use->input.contains(key) && use->input[key].is_string()) {
                        locator[key] = ClipBytes(use->input[key].get<std::string>(), kMaxToolInputBytes);
                    }
                }
                tools.push_back({use->id, "[工具调用] " + ClipBytes(use->name, 80) + "(" +
                    ClipBytes(locator.dump(), kMaxToolInputBytes) + ")"});
                ++tool_count;
                if (tools.size() > kMaxRecentTools) tools.erase(tools.begin());
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                const auto found = std::find_if(tools.rbegin(), tools.rend(), [&](const auto& tool) {
                    return tool.id == result->tool_use_id;
                });
                if (found != tools.rend()) {
                    // One bounded result per call, including failure status. No images or structured payloads.
                    const auto end = found->text.find("\n[工具结果");
                    if (end != std::string::npos) found->text.resize(end);
                    found->text += std::string("\n[工具结果") + (result->is_error ? ",失败" : "") +
                                   "] " + ClipBytes(result->content, kMaxToolResultBytes);
                }
            }
        }
        if (!assistant.empty()) final_answer = std::move(assistant);
    }
    std::string out;
    if (!user.empty()) append(out, "[用户] " + user, max_bytes);
    if (!final_answer.empty()) append(out, "[助手] " + final_answer, max_bytes);
    // Reserve user intent and the final conclusion before any tool excerpts.
    std::string tool_text;
    for (auto it = tools.rbegin(); it != tools.rend(); ++it) {
        append(tool_text, it->text, kMaxToolsBytes);
    }
    if (!tool_text.empty()) {
        append(out, "[工具摘录，最近在前；仅供核对，省略内容不得推断]", max_bytes);
        append(out, tool_text, max_bytes);
    }
    if (tool_count > kMaxRecentTools) append(out, "[更早工具记录已省略]", max_bytes);
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
                                                                 agent::BackgroundCallAccounting* accounting,
                                                                 agent::LoopBoundaryRecorder* boundary_recorder,
                                                                 const std::atomic<bool>* cancel) {
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
    sample.max_tokens = kMemoryExtractMaxTokens;

    agent::SampleOptions sample_options;
    sample_options.timeout_secs = timeout_secs;
    // 回合总结异步化单:外部取消链与旁路桥原先只在同步前台路由调用方拼
    // (memory_commands 的旧路),后台执行器同一条路——cancel 在场时升旗
    // 人是宿主自己(会话拆除/换代的 RequestCancel),如实申报 Internal,
    // 不冤枉用户按键;boundary_recorder 只借不持,桥随调用方栈生灭。
    sample_options.cancel = cancel;
    if (cancel != nullptr) {
        sample_options.cancel_source = agent::OutputCancelSource::Internal;
    }
    sample_options.boundary_recorder = boundary_recorder;
    if (boundary_recorder != nullptr) {
        sample_options.purpose = lubancode::accounting::RequestPurpose::MemoryExtract;
    }
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
        error.message = "抽取输出被截断(结束原因: " + sampled.stop_reason +
                        ");请求已执行，输出未入库，不自动重试";
        if (sampled.usage_reported) {
            error.message += "; usage: input=" + std::to_string(sampled.usage.input_tokens) +
                             ", output=" + std::to_string(sampled.usage.output_tokens);
        } else {
            error.message += "; 服务端未报告 usage";
        }
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

}  // namespace lubancode::agent::memory_extraction
