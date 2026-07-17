#include "agent/prompt_assembler.hpp"

#include <ctime>

#include "embedded_prompts.hpp"  // 构建期生成:<build>/generated/embedded_prompts.hpp

namespace lubancode::agent {

namespace {

// 本机今天的日期,YYYY-MM-DD。系统提示在会话构造时拼一次,跨天长会话里
// 这行会陈旧——跟 cwd 一个待遇,够用,不为它做逐轮刷新。
std::string TodayLocalDate() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}

constexpr const char* kOsLabel =
#ifdef _WIN32
    "Windows";
#elif defined(__APPLE__)
    "macOS";
#else
    "Linux";
#endif

}  // namespace

std::string AssembledDefaultPersona() {
    std::string out;
    for (const char* module : embedded::kCoreModules) {
        if (!out.empty()) {
            out += "\n\n";
        }
        out += module;
    }
    return out;
}

std::string BuildEnvironmentSegment(const std::string& cwd, const std::string& current_date) {
    return "# 运行环境\n\n- 工作目录: " + cwd + "\n- 今天日期: " +
           (current_date.empty() ? TodayLocalDate() : current_date) + "\n- 操作系统: " + kOsLabel +
           "\n\n凡是能动手做的事(读文件、跑命令、改文件……),优先调用工具去做,不要凭空猜测或编造结果——"
           "这条不受上面人格设定的影响,该用工具时就用。";
}

std::string AssembleSystemPrompt(const PromptOptions& options) {
    // 人格:法/CLI 非空整段替换 core;空串回退嵌入的 core 模块。
    std::string prompt = options.persona.empty() ? AssembledDefaultPersona() : options.persona;
    const auto append = [&prompt](const std::string& segment) {
        prompt += "\n\n";
        prompt += segment;
    };

    append(BuildEnvironmentSegment(options.cwd, options.current_date));

    // 恒在的四件套:基础工具的方针跟着工具走,不跟人格走。
    append(embedded::kFeature_files);
    append(embedded::kFeature_shell);
    append(embedded::kFeature_delegation);
    append(embedded::kFeature_todo);

    // 条件注入:没启用的能力一个字不占。
    if (!options.skills_segment.empty()) {
        append(embedded::kFeature_skills);
        append(options.skills_segment);  // 模块讲规矩,清单紧随其后
    }
    if (options.web) {
        append(embedded::kFeature_web);
    }
    if (options.mcp) {
        append(embedded::kFeature_mcp);
    }
    if (options.lsp) {
        append(embedded::kFeature_lsp);
    }

    // 平台段按 wire 注一个;认不出的 wire 不注,不瞎猜。
    if (options.wire == "anthropic") {
        append(embedded::kPlatform_anthropic);
    } else if (options.wire == "responses") {
        append(embedded::kPlatform_responses);
    }
    return prompt;
}

}  // namespace lubancode::agent
