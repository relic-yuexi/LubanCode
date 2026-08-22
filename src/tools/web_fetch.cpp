#include "tools/web_fetch.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

#include <cpr/cpr.h>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

constexpr std::size_t kDefaultMaxBytes = 100 * 1024;  // 100KB

// s[pos..] 是否大小写不敏感地以 prefix 开头。
bool StartsWithCi(const std::string& s, std::size_t pos, const char* prefix) {
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (pos + i >= s.size()) {
            return false;
        }
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// 大小写不敏感地找 needle,找不到返回 npos。
std::size_t FindCi(const std::string& s, std::size_t from, const char* needle) {
    for (std::size_t pos = from; pos < s.size(); ++pos) {
        if (StartsWithCi(s, pos, needle)) {
            return pos;
        }
    }
    return std::string::npos;
}

// 标签名(已转小写)是不是块级标签——替成换行,别的行内标签替成空。
bool IsBlockTag(const std::string& tag) {
    static const char* kBlockTags[] = {
        "p",  "div", "br", "hr", "li", "ul", "ol", "tr", "td", "th", "table", "thead", "tbody",
        "h1", "h2",  "h3", "h4", "h5", "h6", "blockquote", "pre", "section", "article",
        "header", "footer", "nav", "aside", "main", "form", "figure", "figcaption", "dl", "dt", "dd",
    };
    for (const char* block : kBlockTags) {
        if (tag == block) {
            return true;
        }
    }
    return false;
}

// 常见实体还原。只认任务清单点名的那几个(外加 &apos;/&#39,单引号太常见),
// 认不出的实体原样保留——宁可多看见一个 "&hellip;",也别瞎猜着替换。
// 返回消费掉的字节数(含 '&'),0 表示这不是一个认得的实体。
std::size_t DecodeEntityAt(const std::string& s, std::size_t pos, std::string& out) {
    struct Entity {
        const char* name;
        const char* replacement;
    };
    static const Entity kEntities[] = {
        {"&amp;", "&"}, {"&lt;", "<"},   {"&gt;", ">"},    {"&quot;", "\""},
        {"&nbsp;", " "}, {"&apos;", "'"}, {"&#39;", "'"},
    };
    for (const auto& entity : kEntities) {
        if (StartsWithCi(s, pos, entity.name)) {
            out += entity.replacement;
            std::size_t len = 0;
            while (entity.name[len] != '\0') {
                ++len;
            }
            return len;
        }
    }
    return 0;
}

}  // namespace

std::string StripHtml(const std::string& html) {
    // ---- 第一趟:剔整块(script/style/注释),换标签,还原实体 ----
    std::string text;
    text.reserve(html.size() / 2);

    std::size_t pos = 0;
    while (pos < html.size()) {
        const char c = html[pos];
        if (c == '&') {
            if (const std::size_t consumed = DecodeEntityAt(html, pos, text); consumed > 0) {
                pos += consumed;
                continue;
            }
            text += '&';
            ++pos;
            continue;
        }
        if (c != '<') {
            text += c;
            ++pos;
            continue;
        }

        // 注释整块跳过。
        if (StartsWithCi(html, pos, "<!--")) {
            const std::size_t end = html.find("-->", pos + 4);
            pos = (end == std::string::npos) ? html.size() : end + 3;
            continue;
        }
        // <script ...>...</script> / <style ...>...</style> 连内容一起剔。
        if (StartsWithCi(html, pos, "<script")) {
            const std::size_t end = FindCi(html, pos + 7, "</script");
            const std::size_t close = (end == std::string::npos) ? std::string::npos : html.find('>', end);
            pos = (close == std::string::npos) ? html.size() : close + 1;
            continue;
        }
        if (StartsWithCi(html, pos, "<style")) {
            const std::size_t end = FindCi(html, pos + 6, "</style");
            const std::size_t close = (end == std::string::npos) ? std::string::npos : html.find('>', end);
            pos = (close == std::string::npos) ? html.size() : close + 1;
            continue;
        }

        // 普通标签:抠出标签名,块级替换行,行内替空。没有闭合 '>' 的
        // 残破标签,当它一直烂到结尾。
        const std::size_t close = html.find('>', pos + 1);
        std::size_t name_start = pos + 1;
        if (name_start < html.size() && html[name_start] == '/') {
            ++name_start;
        }
        std::string tag_name;
        for (std::size_t i = name_start; i < html.size() && (close == std::string::npos || i < close); ++i) {
            const char tc = html[i];
            if (std::isalnum(static_cast<unsigned char>(tc)) == 0) {
                break;
            }
            tag_name += static_cast<char>(std::tolower(static_cast<unsigned char>(tc)));
        }
        if (IsBlockTag(tag_name)) {
            text += '\n';
        }
        pos = (close == std::string::npos) ? html.size() : close + 1;
    }

    // ---- 第二趟:空白折叠。行内空白(空格/tab/\r)折成一个空格,换行
    // 保留但三连以上折成两个;行首行尾的空格顺手去掉。 ----
    std::string out;
    out.reserve(text.size());
    int pending_newlines = 0;
    bool pending_space = false;
    bool line_has_content = false;
    for (const char c : text) {
        if (c == '\n') {
            ++pending_newlines;
            pending_space = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            pending_space = true;
            continue;
        }
        if (pending_newlines > 0) {
            if (line_has_content) {
                // 不写 std::min:cpr 间接拉进 windows.h,min/max 宏会撞名。
                out.append(pending_newlines > 2 ? 2 : pending_newlines, '\n');
            }
            pending_newlines = 0;
            pending_space = false;
            line_has_content = false;
        }
        if (pending_space) {
            if (line_has_content) {
                out += ' ';
            }
            pending_space = false;
        }
        out += c;
        line_has_content = true;
    }
    return out;
}

std::string TruncateUtf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    // 往回退过所有续字节(10xxxxxx),落在字符边界上。最多退 3 步——
    // 合法 UTF-8 的续字节连不到 4 个,连到了说明本来就是烂的,直接切。
    for (int i = 0; i < 3 && cut > 0; ++i) {
        if ((static_cast<unsigned char>(text[cut]) & 0xC0) != 0x80) {
            break;
        }
        --cut;
    }
    return text.substr(0, cut);
}

std::string SanitizeUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        unsigned int min_value = 0;
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            min_value = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            min_value = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            min_value = 0x10000;
        } else {
            out += '?';
            ++i;
            continue;
        }
        if (i + len > text.size()) {
            out += '?';
            ++i;
            continue;
        }
        unsigned int code = c & (0xFF >> (len + 1));
        bool valid = true;
        for (std::size_t j = 1; j < len; ++j) {
            const unsigned char cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (cc & 0x3F);
        }
        // 过长编码、代理区、超出 U+10FFFF 都算非法。
        if (valid && (code < min_value || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)) {
            valid = false;
        }
        if (valid) {
            out.append(text, i, len);
            i += len;
        } else {
            out += '?';
            ++i;
        }
    }
    return out;
}

WebFetchTool::WebFetchTool(std::string user_agent) : user_agent_(std::move(user_agent)) {}

std::string WebFetchTool::name() const {
    return "web_fetch";
}

std::string WebFetchTool::description() const {
    // 文案在 src/prompts/tools/<语言>/web_fetch.md,兜底是迁移前的原文。
    return ToolText("web_fetch", "description",
                    "抓取一个网页(HTTP GET,跟随重定向)。HTML 会剥掉标签只留正文,普通文本原样返回,"
                    "二进制内容不支持。返回内容开头带一行 URL/状态码/类型说明。适合看文档、查资料;"
                    "需要深读多个长网页再总结时,把活交给 agent 子代理去做,别把整篇长文堆进主对话。");
}

nlohmann::json WebFetchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json url_prop = nlohmann::json::object();
    url_prop["type"] = "string";
    url_prop["description"] =
        ToolText("web_fetch", "param.url", "要抓取的完整 URL,必须以 http:// 或 https:// 开头");
    properties["url"] = url_prop;

    nlohmann::json max_bytes_prop = nlohmann::json::object();
    max_bytes_prop["type"] = "integer";
    max_bytes_prop["description"] = ToolText("web_fetch", "param.max_bytes",
                                             "返回正文的字节数上限,超出截断并标注。不填默认 102400(100KB)");
    properties["max_bytes"] = max_bytes_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"url"});

    return schema;
}

Tool::Result WebFetchTool::execute(const nlohmann::json& input) {
    if (!input.contains("url") || !input.at("url").is_string()) {
        return {"缺少必填参数 url(字符串)", true};
    }
    const std::string url = input.at("url").get<std::string>();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return {"url 必须以 http:// 或 https:// 开头,拿到的是: " + url, true};
    }

    std::size_t max_bytes = kDefaultMaxBytes;
    if (auto it = input.find("max_bytes"); it != input.end() && !it->is_null()) {
        const long long raw = it->get<long long>();
        if (raw <= 0) {
            return {"max_bytes 必须是正整数,拿到的是: " + std::to_string(raw), true};
        }
        max_bytes = static_cast<std::size_t>(raw);
    }

    cpr::Response response = cpr::Get(cpr::Url{url},
                                       cpr::Header{{"User-Agent", user_agent_}},
                                       cpr::Timeout{30000},
                                       cpr::Redirect{});

    if (response.error) {
        return {"网络请求失败: " + response.error.message, true};
    }

    const int status = static_cast<int>(response.status_code);
    if (status < 200 || status >= 300) {
        return {"服务端返回 HTTP " + std::to_string(status) + ": " + url, true};
    }

    // Content-Type(cpr 的 header map 是大小写不敏感的)。
    std::string content_type;
    if (auto it = response.header.find("Content-Type"); it != response.header.end()) {
        content_type = it->second;
    }
    std::string content_type_lower = content_type;
    for (char& c : content_type_lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // 二进制拒收:正文里出现 NUL 字节,基本可断定不是文本(图片/压缩包/
    // 可执行文件……),剥不出正文,给模型也没用。
    if (response.text.find('\0') != std::string::npos) {
        return {"这个 URL 返回的是二进制内容(" + (content_type.empty() ? std::string("未知类型") : content_type) +
                    "),web_fetch 只支持文本",
                true};
    }

    const bool is_html = content_type_lower.find("text/html") != std::string::npos;
    std::string body = is_html ? StripHtml(response.text) : response.text;
    // 网页什么编码的都有(GBK/Latin-1……),先把非法 UTF-8 洗掉——
    // tool_result 最终要进 JSON 请求体,nlohmann dump() 遇到非法 UTF-8 会抛。
    body = SanitizeUtf8(body);

    const bool truncated = body.size() > max_bytes;
    if (truncated) {
        body = TruncateUtf8(body, max_bytes);
    }

    // 开头一行元信息。URL 用 response.url(跟随重定向后的最终地址)。
    const std::string final_url = response.url.str().empty() ? url : response.url.str();
    std::string head = "URL: " + final_url + " (HTTP " + std::to_string(status) + ", " +
                       (content_type.empty() ? std::string("未知类型") : content_type) +
                       (truncated ? ", 已截断至 " + std::to_string(max_bytes) + " 字节" : ", 未截断") + ")\n\n";

    return {head + body, false};
}

}  // namespace lubancode::tools
