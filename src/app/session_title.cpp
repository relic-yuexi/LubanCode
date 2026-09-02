#include "app/session_title.hpp"

#include <string_view>
#include <utility>
#include <variant>

#include "accounting/purpose.hpp"  // RequestPurpose(Token 账本单 A1)
#include "agent/sample_model.hpp"  // SampleModel 原语:采样的公共路(批一·病四)
#include "platform/text_encoding.hpp"

namespace lubancode::app {
namespace {

// 首尾整串比对:中文引号(“「”」)在 UTF-8 里三个字节,拿 front()/
// back() 的单字节比字面量,MSVC 静默、gcc 警告、clang 报错,还全比不中。
bool StartsWithBytes(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWithBytes(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string LeadingPathStem(const std::string& line) {
    const std::size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};

    std::size_t path_begin = begin;
    char quote = '\0';
    if (line[path_begin] == '"' || line[path_begin] == '\'') quote = line[path_begin++];
    const bool windows_path = path_begin + 2 < line.size() &&
                              ((line[path_begin] >= 'A' && line[path_begin] <= 'Z') ||
                               (line[path_begin] >= 'a' && line[path_begin] <= 'z')) &&
                              line[path_begin + 1] == ':' &&
                              (line[path_begin + 2] == '\\' || line[path_begin + 2] == '/');
    const bool rooted_path = path_begin < line.size() &&
                             (line[path_begin] == '\\' || line[path_begin] == '/');
    if (!windows_path && !rooted_path) return {};

    std::size_t path_end = path_begin;
    while (path_end < line.size()) {
        const char c = line[path_end];
        if ((quote != '\0' && c == quote) ||
            (quote == '\0' && (c == ' ' || c == '\t' || c == '\r'))) {
            break;
        }
        ++path_end;
    }
    const std::string path = line.substr(path_begin, path_end - path_begin);
    const std::size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

}  // namespace

std::string SanitizeTitle(const std::string& raw, std::size_t max_chars) {
    std::string text = raw;
    // 剥代码围栏:模型偶尔会把标题包进 ``` ```。
    if (text.size() > 6 && text.rfind("```", 0) == 0) {
        const std::size_t close = text.rfind("```");
        if (close >= 3) {
            text = text.substr(3, close - 3);
        }
    }
    // 压空白:标题单行,换行/制表折成空格,连续空格并成一个。
    std::string squeezed;
    bool pending_space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = !squeezed.empty();
            continue;
        }
        if (pending_space) {
            squeezed += ' ';
            pending_space = false;
        }
        squeezed += c;
    }
    // 剥首尾引号(模型爱加的书名号/引号一并剥掉一层)。
    while (!squeezed.empty()) {
        bool stripped = false;
        for (const std::string_view q : {"\"", "'", "“", "「"}) {
            if (StartsWithBytes(squeezed, q)) {
                squeezed.erase(0, q.size());
                stripped = true;
                break;
            }
        }
        if (!stripped) {
            break;
        }
    }
    while (!squeezed.empty()) {
        bool stripped = false;
        for (const std::string_view q : {"\"", "'", "”", "」"}) {
            if (EndsWithBytes(squeezed, q)) {
                squeezed.resize(squeezed.size() - q.size());
                stripped = true;
                break;
            }
        }
        if (!stripped) {
            break;
        }
    }
    // 限长:按 UTF-8 码点截,不从码点中腰劈开。
    std::size_t count = 0;
    std::size_t cut = squeezed.size();
    for (std::size_t i = 0; i < squeezed.size();) {
        if (count == max_chars) {
            cut = i;
            break;
        }
        std::size_t width = 1;
        const unsigned char lead = static_cast<unsigned char>(squeezed[i]);
        if ((lead & 0x80) == 0x00) {
            width = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            width = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            width = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            width = 4;
        }
        i += width;
        ++count;
    }
    squeezed.resize(cut);
    // 剥两端空白(压空白后再来一道,防截断后留空尾巴)。
    while (!squeezed.empty() && (squeezed.back() == ' ')) {
        squeezed.pop_back();
    }
    return squeezed;
}

std::string LocalSessionTitle(const std::string& first_query, std::size_t max_chars) {
    // 只取首行:多行粘贴的首行就是用户的题眼,后面几行截掉。
    const std::size_t newline = first_query.find('\n');
    const std::string first_line = newline == std::string::npos
                                       ? first_query
                                       : first_query.substr(0, newline);
    // 首问若以路径开头,题眼在文件名,不在盘符与层层目录。后面的
    // "commit 一下"之类只是处置动作,也不该挤掉真正主题。
    const std::string path_stem = LeadingPathStem(first_line);
    return SanitizeTitle(path_stem.empty() ? first_line : path_stem, max_chars);
}

std::expected<std::string, std::string> RefineSessionTitle(lubancode::api::Backend& backend,
                                                           const std::string& model,
                                                           const std::string& reasoning_effort,
                                                           const std::string& first_query,
                                                           int timeout_secs, const std::atomic<bool>* cancel,
                                                           lubancode::agent::BackgroundCallAccounting* accounting,
                                                           lubancode::agent::LoopBoundaryRecorder* boundary_recorder) {
    // 只喂首问(实测问题 7):输入不随首轮工具数量增长,也不等首轮回复。
    // 刀口必须在原串上算(600 字节刀口不留下半个汉字的老规矩)。
    constexpr std::size_t kQueryMaxBytes = 600;
    const std::size_t cut = lubancode::platform::Utf8PrefixBoundary(first_query, kQueryMaxBytes);
    const std::string query_excerpt = first_query.substr(0, cut);
    if (query_excerpt.empty()) {
        return std::unexpected("首问没有可起标题的文本");
    }

    // 采样走 SampleModel 原语(批一·病四):攒流/usage/兜错/看门狗的路只有
    // 一份,这里只剩提示拼装与标题清洗;错误只回 message(旧口径)。
    lubancode::agent::SampleRequest sample;
    sample.model = model;
    // reasoning 关或最低(单子预算):路由带档位就按配的来,没带就压到 low,
    // 不为十几个 token 烧思考。
    sample.reasoning_effort = reasoning_effort.empty() ? std::string("low") : reasoning_effort;
    sample.system = "给下面这条用户请求起一个会话标题。要求:中文不超过 16 个字(英文不超过 8 个词),"
                    "写任务的实际主题,不要写处置动作。若请求含文件路径,优先从文件名判断主题;"
                    "忽略提交、commit、继续、帮我处理、看看图片这类尾随指令。"
                    "例如应写\"修终端光标跳动\",不要写\"提交终端修复待办\";"
                    "直接输出标题本身,不要引号、不要标点结尾、不要解释。";
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{
        "本地主题线索: " + LocalSessionTitle(first_query, 48) + "\n原请求: " + query_excerpt});
    sample.messages.push_back(std::move(message));
    sample.max_tokens = kTitleRefineMaxTokens;

    lubancode::agent::SampleOptions sample_options;
    sample_options.timeout_secs = timeout_secs;
    sample_options.cancel = cancel;
    // Token 账本单 A1(旁路落账):递了桥就把这次精炼请求落成 Journal 里
    // 的 prepared/sent/usage/output(purpose=title_refine)。
    sample_options.boundary_recorder = boundary_recorder;
    sample_options.purpose = lubancode::accounting::RequestPurpose::TitleRefine;
    const lubancode::agent::SampleResult sampled =
        lubancode::agent::SampleModel(backend, sample, sample_options);

    if (accounting != nullptr) {
        lubancode::agent::AddSampleAccounting(accounting, sampled);
        accounting->duration_ms = sampled.duration_ms;
    }

    if (!sampled.ok) return std::unexpected(sampled.error.message);

    const std::string title = SanitizeTitle(sampled.text);
    if (title.empty()) {
        return std::unexpected("标题为空");
    }
    return title;
}

}  // namespace lubancode::app
