#include "tools/web_fetch.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <cpr/cpr.h>
#include <curl/curl.h>

#include "platform/text_encoding.hpp"  // SanitizeExternalText/Utf8PrefixBoundary:外来文本公共关口
#include "tools/tool_text.hpp"         // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

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
    return text.substr(0, platform::Utf8PrefixBoundary(text, max_bytes));
}

PreparedBody PrepareFetchedBody(const std::string& content_type, const std::string& raw_body,
                                std::size_t max_bytes) {
    // Content-Type 大小写不敏感地认 text/html(cpr 的 header 已小写化过
    // 一道,这里把入参也折小写,纯函数口不赖调用方)。
    std::string content_type_lower = content_type;
    for (char& c : content_type_lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool is_html = content_type_lower.find("text/html") != std::string::npos;
    std::string body = is_html ? StripHtml(raw_body) : raw_body;
    // 网页什么编码的都有(GBK/Latin-1……),统一过外来文本清洗——
    // tool_result 最终要进 JSON 请求体,nlohmann dump() 遇到非法 UTF-8 会抛;
    // 与 MCP rich result、Search 同一份替换合同(platform::SanitizeExternalText)。
    body = platform::SanitizeExternalText(body);

    PreparedBody prepared;
    prepared.truncated = body.size() > max_bytes;
    if (prepared.truncated) {
        body = TruncateUtf8(body, max_bytes);
    }
    prepared.text = std::move(body);
    return prepared;
}

namespace {
bool HeaderText(const std::string& value) {
    if (value.empty() || value.size() > 256 || !platform::IsValidUtf8(value)) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool UrlText(const std::string& value) {
    if (value.empty() || value.size() > 8192 || !platform::IsValidUtf8(value)) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}
struct HttpUrl { std::string text, scheme; };
std::expected<HttpUrl, std::string> ResolveHttpUrl(const std::string& value, const std::string& base = {}) {
    if (!UrlText(value)) return std::unexpected("invalid_url");
    // CPR owns libcurl global initialization. Keep its local handle alive until
    // the pure URL handle has retired; this opens no connection or worker.
    cpr::Session initialized;
    if (!initialized.GetCurlHolder() || !initialized.GetCurlHolder()->handle)
        throw std::runtime_error("web_fetch.transport_initialization_failed");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url) return std::unexpected("invalid_url");
    if ((!base.empty() && curl_url_set(url.get(), CURLUPART_URL, base.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) ||
        curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        return std::unexpected("invalid_url");
    const auto part = [&](CURLUPart kind) -> std::string {
        char* raw = nullptr;
        const auto status = curl_url_get(url.get(), kind, &raw, 0);
        std::unique_ptr<char, decltype(&curl_free)> held(raw, curl_free);
        return status == CURLUE_OK && raw ? std::string(raw) : std::string();
    };
    auto scheme = part(CURLUPART_SCHEME);
    if (scheme != "http" && scheme != "https") return std::unexpected("invalid_url");
    if (curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0) != CURLUE_OK)
        return std::unexpected("invalid_url");
    auto text = part(CURLUPART_URL);
    if (!UrlText(text)) return std::unexpected("invalid_url");
    return HttpUrl{std::move(text), std::move(scheme)};
}
bool SameHeader(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const char c = left[i] >= 'A' && left[i] <= 'Z' ? static_cast<char>(left[i] + ('a' - 'A')) : left[i];
        if (c != right[i]) return false;
    }
    return true;
}
class DefaultWebFetchTransport final : public WebFetchTransport {
public:
    std::expected<net::FullHttpResponse, net::FullHttpError> Get(const net::FullHttpRequest& request,
        const net::FullHttpLimits& limits, const std::atomic<bool>* cancel) override {
        return net::PerformFullHttpRequest(request, limits, cancel, nullptr);
    }
};
Tool::Result FetchError(const std::string& code, const std::string& text) {
    Tool::Result result{"web_fetch." + code + ": " + text, true};
    result.error_code = "web_fetch." + code;
    result.outcome = code == "cancelled" ? "cancelled_during_run" : code == "timeout" ? "timed_out" : "tool_error";
    return result;
}
Tool::Result TransportError(const net::FullHttpError& error) {
    using Kind = net::FullHttpErrorKind;
    // Newer libcurl rejects differing Location headers before delivering the
    // second header callback. Preserve a stable rejection for an actually
    // received redirect status; do not infer it from an arbitrary error string
    // or treat a failed response as successful redirect material.
    if (error.kind == Kind::NetworkFailed &&
        error.curl_code == static_cast<long>(cpr::ErrorCode::WEIRD_SERVER_REPLY) &&
        (error.response_status == 301 || error.response_status == 302 || error.response_status == 303 ||
         error.response_status == 307 || error.response_status == 308))
        return FetchError("redirect_invalid", "重定向响应不合规");
    // The existing transport stores cpr::ErrorCode here (despite the historical
    // field name), not the numeric CURLcode; CPR maps CURLE_BAD_CONTENT_ENCODING.
    if (error.kind == Kind::NetworkFailed &&
        error.curl_code == static_cast<long>(cpr::ErrorCode::BAD_CONTENT_ENCODING))
        return FetchError("unsupported_encoding", "响应编码无法由当前传输解码");
    switch (error.kind) {
        case Kind::Cancelled: return FetchError("cancelled", "请求已取消");
        case Kind::Timeout: return FetchError("timeout", "请求超过宿主时限");
        case Kind::ResponseHeaderTooLarge: return FetchError("header_limit", "累计响应头超过宿主上限");
        case Kind::ResponseBodyTooLarge: return FetchError("download_limit", "累计下载超过宿主上限");
        case Kind::DnsFailed: return FetchError("dns_failed", "网络请求失败: DNS 解析失败");
        case Kind::TlsFailed: return FetchError("tls_failed", "网络请求失败: TLS 校验失败");
        case Kind::NetworkFailed: return FetchError("network_failed", "网络请求失败");
    }
    return FetchError("network_failed", "网络请求失败");
}
} // namespace

bool WebFetchSupportsGzip() {
    cpr::Session initialized;
    if (!initialized.GetCurlHolder() || !initialized.GetCurlHolder()->handle)
        throw std::runtime_error("web_fetch.capabilities_unavailable");
    const auto* version = curl_version_info(CURLVERSION_NOW);
    if (!version) throw std::runtime_error("web_fetch.capabilities_unavailable");
    return (version->features & CURL_VERSION_LIBZ) != 0;
}

std::expected<void, std::string> ValidateWebFetchOptions(const WebFetchOptions& options) {
    if (!HeaderText(options.user_agent) || options.connect_timeout_ms <= 0 || options.connect_timeout_ms > 30'000 ||
        options.total_timeout_ms <= 0 || options.total_timeout_ms > 120'000 ||
        options.connect_timeout_ms > options.total_timeout_ms || options.max_header_bytes == 0 ||
        options.max_header_bytes > 512 * 1024 || options.max_download_bytes == 0 ||
        options.max_download_bytes > 8 * 1024 * 1024 || options.max_output_bytes < 256 ||
        options.max_output_bytes > 1024 * 1024 || options.max_redirects > 10)
        return std::unexpected("web_fetch.invalid_options");
    return {};
}

WebFetchTool::WebFetchTool(std::string user_agent)
    : WebFetchTool([&] { WebFetchOptions options; options.user_agent = std::move(user_agent); return options; }()) {}
WebFetchTool::WebFetchTool(WebFetchOptions options, std::shared_ptr<WebFetchTransport> transport)
    : options_(std::move(options)), transport_(transport ? std::move(transport) : std::make_shared<DefaultWebFetchTransport>()) {}

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

Tool::Result WebFetchTool::execute(const nlohmann::json& input) { return execute(input, {}); }

Tool::Result WebFetchTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    try {
        if (!ValidateWebFetchOptions(options_)) return FetchError("invalid_options", "宿主配置不合规");
        const auto cancelled = [&] { return context.cancel && context.cancel->load(); };
        if (cancelled()) return FetchError("cancelled", "请求已取消");
        if (!input.is_object() || !input.contains("url") || !input.at("url").is_string())
            return FetchError("invalid_input", "缺少必填参数 url(字符串)");
        std::uint64_t requested_output = kDefaultMaxBytes;
        if (const auto value = input.find("max_bytes"); value != input.end() && !value->is_null()) {
            if (!value->is_number_integer() || (!value->is_number_unsigned() && value->get<std::int64_t>() <= 0))
                return FetchError("invalid_input", "max_bytes 必须是正整数");
            requested_output = value->get<std::uint64_t>();
            if (requested_output == 0) return FetchError("invalid_input", "max_bytes 必须是正整数");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.total_timeout_ms);
        auto url = ResolveHttpUrl(input.at("url").get<std::string>());
        if (!url) return FetchError("invalid_url", "URL 须为有界 HTTP(S) 地址且不含凭据");
        std::uint64_t header_left = options_.max_header_bytes, body_left = options_.max_download_bytes;
        std::set<std::string> visited;
        for (std::uint32_t hop = 0;; ++hop) {
            if (cancelled()) return FetchError("cancelled", "请求已取消");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return FetchError("timeout", "请求超过宿主时限");
            if (!visited.insert(url->text).second) return FetchError("redirect_loop", "重定向形成循环");
            if (header_left == 0) return FetchError("header_limit", "累计响应头超过宿主上限");
            if (body_left == 0) return FetchError("download_limit", "累计下载超过宿主上限");
            net::FullHttpRequest request{"GET", url->text, {{"User-Agent", options_.user_agent}}, {}};
            net::FullHttpLimits limits;
            limits.connect_timeout_ms = (std::min)(options_.connect_timeout_ms, remaining);
            limits.hard_timeout_ms = remaining;
            limits.response_header_bytes = static_cast<std::int64_t>(header_left);
            limits.response_body_bytes = static_cast<std::int64_t>(body_left);
            auto response = transport_->Get(request, limits, context.cancel);
            if (cancelled()) return FetchError("cancelled", "请求已取消");
            if (!response) return TransportError(response.error());
            if (std::chrono::steady_clock::now() >= deadline) return FetchError("timeout", "请求超过宿主时限");
            if (response->received_header_bytes > header_left) return FetchError("header_limit", "累计响应头超过宿主上限");
            if (response->body.size() > body_left) return FetchError("download_limit", "累计下载超过宿主上限");
            header_left -= response->received_header_bytes;
            body_left -= response->body.size();
            const int status = response->status;
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                if (hop >= options_.max_redirects) return FetchError("redirect_limit", "重定向超过宿主次数上限");
                std::string location;
                unsigned count = 0;
                for (const auto& [name, value] : response->headers) if (SameHeader(name, "location")) { location = value; ++count; }
                if (count != 1) return FetchError("redirect_invalid", "重定向缺少唯一 Location");
                auto next = ResolveHttpUrl(location, url->text);
                if (!next) return FetchError("redirect_invalid", "Location 不是允许的 HTTP(S) 地址");
                if (url->scheme == "https" && next->scheme != "https")
                    return FetchError("redirect_downgrade", "拒绝 HTTPS 降级重定向");
                url = std::move(next);
                continue;
            }
            if (status < 200 || status >= 300) return FetchError("http_status", "服务端返回 HTTP " + std::to_string(status));
            if (response->body.find('\0') != std::string::npos)
                return FetchError("binary_body", "web_fetch 只支持文本，不接二进制内容");
            std::string content_type;
            for (const auto& [name, value] : response->headers)
                if (SameHeader(name, "content-type")) content_type = value;
            content_type = TruncateUtf8(platform::SanitizeExternalText(content_type), 128);
            const auto available = static_cast<std::size_t>((std::min)(requested_output, options_.max_output_bytes));
            auto prepared = PrepareFetchedBody(content_type, response->body, available);
            // Keep enough space for the longer truncation label before deciding
            // how much body fits. The entire successful text obeys the host cap.
            const std::string prefix = "URL: " + url->text + " (HTTP " + std::to_string(status) + ", " +
                (content_type.empty() ? std::string("未知类型") : content_type);
            const std::string truncated_suffix = ", 已截断)\n\n";
            if (prefix.size() + truncated_suffix.size() >= options_.max_output_bytes)
                return FetchError("output_limit", "响应元信息超过宿主返回上限");
            const auto body_budget = static_cast<std::size_t>(options_.max_output_bytes - prefix.size() - truncated_suffix.size());
            if (prepared.text.size() > body_budget) {
                prepared.text = TruncateUtf8(prepared.text, body_budget);
                prepared.truncated = true;
            }
            if (cancelled()) return FetchError("cancelled", "请求已取消");
            if (std::chrono::steady_clock::now() >= deadline) return FetchError("timeout", "请求超过宿主时限");
            Tool::Result result{prefix + (prepared.truncated ? truncated_suffix : ", 未截断)\n\n") + prepared.text, false};
            result.outcome = "succeeded";
            result.details = {{"http_status", status}, {"redirects", hop},
                {"downloaded_bytes", options_.max_download_bytes - body_left},
                {"response_header_bytes", options_.max_header_bytes - header_left}, {"truncated", prepared.truncated}};
            return result;
        }
    } catch (...) { return FetchError("internal_error", "请求处理失败"); }
}

}  // namespace lubancode::tools
