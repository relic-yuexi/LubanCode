// memory_extract.hpp 的实现。任务分型、转写压缩与 JSON 解析全是纯函数,
// 好单测;只有 RunMemoryExtraction 碰网络。

#include "app/memory_extract.hpp"

#include <initializer_list>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "agent/prompt_assembler.hpp"
#include "agent/sample_model.hpp"  // SampleModel 原语:采样的公共路(批一·病四)
#include "api/backend.hpp"
#include "platform/text_encoding.hpp"

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

std::expected<MemoryExtraction, std::string> ParseExtractionJson(const std::string& text) {
    // 容错:模型有时仍裹 ```json 围栏,剥掉;取首个 { 到末个 }。
    std::size_t begin = text.find('{');
    std::size_t end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return std::unexpected("抽取输出里找不到 JSON object");
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.substr(begin, end - begin + 1));
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(std::string("抽取输出不是合法 JSON: ") + e.what());
    }
    if (!root.is_object()) return std::unexpected("抽取输出不是 JSON object");

    MemoryExtraction extraction;
    extraction.task_type = root.value("task_type", std::string("other"));
    if (extraction.task_type != "code" && extraction.task_type != "research" &&
        extraction.task_type != "config" && extraction.task_type != "docs") {
        extraction.task_type = "other";
    }
    extraction.summary = root.value("summary", std::string());
    if (root.contains("retrieval_terms") && root["retrieval_terms"].is_array()) {
        for (const auto& item : root["retrieval_terms"]) {
            if (item.is_string() && extraction.retrieval_terms.size() < 8) {
                extraction.retrieval_terms.push_back(item.get<std::string>());
            }
        }
    }
    if (root.contains("candidates") && root["candidates"].is_array()) {
        for (const auto& item : root["candidates"]) {
            if (!item.is_object() || extraction.candidates.size() >= 3) continue;
            ProposedCandidate candidate;
            candidate.kind = item.value("kind", std::string());
            if (candidate.kind != "fact" && candidate.kind != "preference" &&
                candidate.kind != "feedback") {
                continue;
            }
            candidate.title = item.value("title", std::string());
            candidate.summary = item.value("summary", std::string());
            candidate.content = item.value("content", std::string());
            candidate.confidence = item.value("confidence", std::string("inferred"));
            if (candidate.title.empty() || candidate.content.empty()) continue;
            if (item.contains("keywords") && item["keywords"].is_array()) {
                for (const auto& keyword : item["keywords"]) {
                    if (keyword.is_string()) candidate.keywords.push_back(keyword.get<std::string>());
                }
            }
            if (item.contains("paths") && item["paths"].is_array()) {
                for (const auto& path : item["paths"]) {
                    if (path.is_string()) candidate.paths.push_back(path.get<std::string>());
                }
            }
            extraction.candidates.push_back(std::move(candidate));
        }
    }
    return extraction;
}

std::expected<MemoryExtraction, std::string> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs,
                                                                 const std::string& reasoning_effort,
                                                                 agent::BackgroundCallAccounting* accounting) {
    // 采样走 SampleModel 原语(批一·病四):攒流/usage/兜错/看门狗的路只有
    // 一份,这里只剩提示拼装与解析;错误只回 message(旧口径)。
    agent::SampleRequest sample;
    sample.model = model;
    sample.system = system_prompt;
    sample.reasoning_effort = reasoning_effort;
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

std::expected<MemoryExtraction, std::string> FinishMemoryExtraction(const agent::SampleResult& sampled) {
    if (!sampled.ok) return std::unexpected(sampled.error.message);
    if (sampled.text.empty()) return std::unexpected("抽取输出为空");
    return ParseExtractionJson(sampled.text);
}

}  // namespace lubancode::app
