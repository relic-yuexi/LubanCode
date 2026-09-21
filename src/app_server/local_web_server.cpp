// local_web_server.hpp 的实现:监听、同源门、静态资源、bootstrap 交换、
// 健康探测、控制口、WS 升级交棒。头部/帧解析全走 ws_frames 纯函数,
// socket 全走 ws_sockets;升级后的帧读写复用 WsTransport::Session;读头、
// 恒时比较、artifact 加载走 http_support(与 WS 承载共用一份,HC-03)。
#include "app_server/local_web_server.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "app_server/http_support.hpp"
#include "platform/paths.hpp"  // PathToUtf8:filesystem 路径进 UTF-8 人话

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[assistant-web] %s\n", text.c_str());
}

// POST body 上限(auth/exchange 与 control/open 都是短凭据,不是文件上传)。
constexpr std::size_t kMaxBodyBytes = 8 * 1024;

// 读 POST body(Content-Length 已知且 ≤ 上限)。头部读入的那一段 TCP 里
// 可能已挤着 body 的前几个字节(preread),先消费它再继续收。false = 断/坏。
bool ReadBody(net::Socket& socket, const ws::HttpRequestHead& head, std::string& body,
              std::string_view preread) {
    if (!head.has_content_length || head.content_length > kMaxBodyBytes) {
        return false;
    }
    body.clear();
    body.reserve(head.content_length);
    body.append(preread.substr(0, std::min(preread.size(), head.content_length)));
    while (body.size() < head.content_length) {
        char buffer[2048];
        const std::size_t want = std::min(sizeof(buffer), head.content_length - body.size());
        const long chunk = socket.Recv(buffer, want);
        if (chunk <= 0) {
            return false;
        }
        body.append(buffer, buffer + chunk);
    }
    return true;
}

// 小应答的公共头:CSP 锁本地受控资源、nosniff、无缓存。
// CSP 的 connect-src 显式列 ws:// 回环(浏览器对 'self' 覆盖 ws 的口径
// 不齐,写死最稳);frame-ancestors 'none' 挡嵌入。运行期才知道端口,
// 所以在这里拼。
std::string SecurityHeaders(int port) {
    std::string headers;
    headers += "Content-Security-Policy: default-src 'self'; script-src 'self'; ";
    headers += "style-src 'self'; img-src 'self' data:; ";
    headers += "connect-src 'self' ws://127.0.0.1:" + std::to_string(port);
    headers += " ws://localhost:" + std::to_string(port);
    headers += "; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n";
    headers += "X-Content-Type-Options: nosniff\r\n";
    headers += "Cache-Control: no-store\r\n";
    return headers;
}

void SendResponse(net::Socket& socket, const char* status_line, const char* content_type,
                  std::string_view body, std::string_view extra_headers) {
    const std::string response =
        ws::MakeHttpResponse(status_line, content_type, body, extra_headers);
    socket.SendAll(response);
}

// 401 的配对提示页:没带/坏 cookie 时给这页(本地配对流程,不降级免认证)。
// 纯静态短文,不引外部资源,不泄任何秘密。
std::string PairingPageHtml() {
    return "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
           "<title>LubanCode 助理</title></head><body>"
           "<h1>需要从启动链接进入</h1>"
           "<p>本页受本地认证保护。请从 <code>lubancode assistant</code> 打印的地址"
           "(带 #b= 一次性凭据的那条)进入;凭据只能用一次,过期就重新启动命令。</p>"
           "</body></html>";
}

std::string LowerAscii(std::string_view text) {
    std::string lowered(text);
    for (char& c : lowered) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return lowered;
}

}  // namespace

std::vector<WebAsset> AssistantWebManifest() {
    return {
        {"/", "text/html; charset=utf-8"},
        {"/index.html", "text/html; charset=utf-8"},
        {"/assistant.css", "text/css; charset=utf-8"},
        {"/assistant_core.js", "application/javascript; charset=utf-8"},
        {"/assistant_app.js", "application/javascript; charset=utf-8"},
    };
}

bool LocalWebServer::HostIsLocalLoopback(const std::string& host_header, int port) {
    const std::string lowered = LowerAscii(host_header);
    return lowered == "127.0.0.1:" + std::to_string(port) ||
           lowered == "localhost:" + std::to_string(port);
}

bool LocalWebServer::OriginIsLocalSame(const std::string& origin_header, int port) {
    const std::string lowered = LowerAscii(origin_header);
    return lowered == "http://127.0.0.1:" + std::to_string(port) ||
           lowered == "http://localhost:" + std::to_string(port);
}

LocalWebServer::LocalWebServer(LocalWebOptions options) : options_(std::move(options)) {}

LocalWebServer::~LocalWebServer() {
    Stop();
}

bool LocalWebServer::Start() {
    // 首版合同:只绑回环。非回环监听不能靠改 host 参数无声开启。
    const std::string host = options_.bind_host.empty() ? "127.0.0.1" : options_.bind_host;
    if (host != "127.0.0.1" && host != "localhost") {
        last_error_ = "助理 Web 服务只允许绑定回环地址(127.0.0.1),拒绝: " + host +
                      "(远程访问是后续批次的设计,不在这里开)";
        return false;
    }
    // 资源门(§九发布验收:前端资产缺失明确报错,不启动空壳服务):
    // manifest 全员须在 assets_root 下、可读、不超限。缺哪枚点名哪枚。
    if (options_.assets_root.empty()) {
        last_error_ = "随包网页根未配置,拒绝启动空壳服务";
        return false;
    }
    std::string error;
    if (!listener_.Start(host, options_.port, error)) {
        last_error_ = options_.port != 0
                          ? "端口 " + std::to_string(options_.port) + " 监听起不来(多半被占用): " +
                                error + ";不偷偷换端口,请换一个或让占着端口的进程退出"
                          : "本地监听起不来: " + error;
        return false;
    }
    const int port = listener_.actual_port();
    for (const WebAsset& asset : options_.manifest) {
        std::filesystem::path relative(asset.path == "/" ? "index.html" : asset.path.substr(1));
        const std::filesystem::path full = options_.assets_root / relative;
        std::error_code ec;
        if (!std::filesystem::exists(full, ec)) {
            last_error_ = "随包网页资源缺失: " + platform::PathToUtf8(full) +
                          "(manifest 项 " + asset.path + ");拒绝启动空壳服务";
            listener_.Stop();
            return false;
        }
        const std::uintmax_t size = std::filesystem::file_size(full, ec);
        if (ec || size > options_.max_asset_bytes) {
            last_error_ = "随包网页资源读不了/超上限: " + platform::PathToUtf8(full);
            listener_.Stop();
            return false;
        }
    }
    started_ = true;
    Diagnose("助理 Web 监听: " + host + ":" + std::to_string(port));
    return true;
}

void LocalWebServer::Run(
    std::function<void(std::unique_ptr<WsTransport::Session>, const ws::HttpRequestHead& head)> on_ws_session) {
    while (true) {
        std::optional<net::Socket> accepted = listener_.Accept(200);
        if (!accepted.has_value()) {
            if (listener_.stopped() || !listener_.last_error().empty()) {
                return;
            }
            continue;  // 超时:接着等
        }
        HandleConnection(*accepted, on_ws_session);
        // HTTP 已就地应答的连接这里显式关(应答头是 Connection: close,不
        // 留半开);WS 的已交棒(socket 被 move 走,Close 无害)。
        accepted->Close();
    }
}

bool LocalWebServer::HandleConnection(
    net::Socket& socket,
    const std::function<void(std::unique_ptr<WsTransport::Session>, const ws::HttpRequestHead&)>& on_ws_session) {
    std::string request;
    if (!ReadUntilHeaderEnd(socket, request)) {
        return false;  // 对端跑了/捣乱,断掉
    }
    // 头部与"同段 TCP 挤进来的先头字节"劈开(body 的前几个字节可能已到)。
    const std::size_t head_end = request.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        return false;
    }
    const std::string header = request.substr(0, head_end + 4);
    const std::string preread = request.substr(head_end + 4);
    const ws::HttpRequestHead head = ws::ParseHttpRequestHead(header);
    const int port = listener_.actual_port();

    // ---- 健康探测:唯一不鉴权的口,只回身份(零秘密) ----
    if (head.method == "GET" && head.target == "/healthz") {
        nlohmann::json body = nlohmann::json::object();
        if (options_.health_body) {
            body = options_.health_body();
        }
        SendResponse(socket, "200 OK", "application/json; charset=utf-8", body.dump(),
                     SecurityHeaders(port));
        return false;
    }

    // ---- 同源门(DNS rebinding/跨站):Host 必须回环本端口;Origin 在场
    // 必须同源。这两道在鉴权之前——先知道"来的是谁家的请求"。 ----
    if (!HostIsLocalLoopback(head.host, port)) {
        SendResponse(socket, "403 Forbidden", "text/plain; charset=utf-8",
                     "host not allowed", SecurityHeaders(port));
        return false;
    }
    if (!head.origin.empty() && !OriginIsLocalSame(head.origin, port)) {
        SendResponse(socket, "403 Forbidden", "text/plain; charset=utf-8",
                     "cross-origin request refused", SecurityHeaders(port));
        return false;
    }

    // ---- bootstrap 交换:一次性凭据换会话 cookie ----
    if (head.method == "POST" && head.target == "/auth/exchange") {
        std::string body;
        if (!ReadBody(socket, head, body, preread)) {
            SendResponse(socket, "411 Length Required", "text/plain; charset=utf-8",
                         "body required (<= 8KB)", SecurityHeaders(port));
            return false;
        }
        // 凭据不进日志、不进错误话;403 不区分"没带/带错/过期/用过"。
        if (!options_.consume_bootstrap) {
            SendResponse(socket, "403 Forbidden", "text/plain; charset=utf-8", "not accepted",
                         SecurityHeaders(port));
            return false;
        }
        const std::optional<std::string> session = options_.consume_bootstrap(body);
        if (!session.has_value()) {
            SendResponse(socket, "403 Forbidden", "text/plain; charset=utf-8", "not accepted",
                         SecurityHeaders(port));
            return false;
        }
        // HttpOnly(JS 读不走)+ SameSite=Strict(跨站不带)+ 会话 cookie
        //(浏览器关了就没了)——本地助理的一天,合身。
        std::string extra = "Set-Cookie: " + options_.cookie_name + "=" + *session +
                            "; HttpOnly; SameSite=Strict; Path=/\r\n";
        extra += SecurityHeaders(port);
        SendResponse(socket, "204 No Content", "text/plain; charset=utf-8", "", extra);
        return false;
    }

    // ---- 控制口:重复启动的第二个 CLI 进程要一条新打开页 URL ----
    if (head.method == "POST" && head.target == "/control/open") {
        std::string body;
        if (!ReadBody(socket, head, body, preread) || !options_.handle_open) {
            SendResponse(socket, "403 Forbidden", "text/plain; charset=utf-8", "not accepted",
                         SecurityHeaders(port));
            return false;
        }
        const std::optional<nlohmann::json> result = options_.handle_open(body);
        if (!result.has_value()) {
            SendResponse(socket, "403 Forbidden", "text/plain; charset=utf-8", "not accepted",
                         SecurityHeaders(port));
            return false;
        }
        SendResponse(socket, "200 OK", "application/json; charset=utf-8", result->dump(),
                     SecurityHeaders(port));
        return false;
    }

    // artifact/WS 要会话 cookie;公开页面壳必须先能加载,才能交换 fragment。
    const std::string cookie_value = ws::CookieValue(head.cookie, options_.cookie_name);
    const bool session_ok =
        !cookie_value.empty() && options_.validate_session && options_.validate_session(cookie_value);

    // artifact 字节口子(与 WS 承载同一份加载器 http_support;这里加会话门)。
    if (head.method == "GET" && head.target.rfind("/artifact/", 0) == 0) {
        if (!session_ok) {
            SendResponse(socket, "401 Unauthorized", "text/html; charset=utf-8",
                         PairingPageHtml(), SecurityHeaders(port));
            return false;
        }
        const std::string name = head.target.substr(std::string_view("/artifact/").size());
        const ArtifactBytes artifact = LoadArtifactBytes(options_.artifact_dir, name);
        if (!artifact.ok) {
            SendResponse(socket, "404 Not Found", "text/plain; charset=utf-8", "no such artifact",
                         SecurityHeaders(port));
            return false;
        }
        SendResponse(socket, "200 OK", artifact.mime, artifact.bytes, SecurityHeaders(port));
        return false;
    }

    // ---- WS 升级:过会话门才应 101,Session 交棒宿主 ----
    const ws::UpgradeParseResult upgrade = ws::ParseUpgradeRequest(header);
    if (upgrade.valid) {
        if (!session_ok) {
            SendResponse(socket, "401 Unauthorized", "text/plain; charset=utf-8",
                         "session required", SecurityHeaders(port));
            return false;
        }
        if (!socket.SendAll(ws::MakeUpgradeResponse(ws::ComputeAcceptKey(upgrade.websocket_key)))) {
            return false;
        }
        on_ws_session(WsTransport::AdoptUpgradedSocket(std::move(socket)), head);
        return true;
    }

    // ---- 静态资源(manifest 白名单;目录列表/穿越无从谈起) ----
    if (head.method == "GET") {
        std::string target = head.target;
        if (target == "/index.html") {
            target = "/";  // 同一枚文件,manifest 里两枚名字都指它
        }
        const WebAsset* matched = nullptr;
        for (const WebAsset& asset : options_.manifest) {
            if (asset.path == target) {
                matched = &asset;
                break;
            }
        }
        if (matched == nullptr) {
            SendResponse(socket, "404 Not Found", "text/plain; charset=utf-8", "not found",
                         SecurityHeaders(port));
            return false;
        }
        // manifest 只含随包 HTML/CSS/JS,不含用户数据。首次访问没有 cookie,
        // 也须加载页面和脚本,由脚本读取 #b= 并 POST /auth/exchange。
        const std::filesystem::path relative =
            matched->path == "/" ? std::filesystem::path("index.html")
                                 : std::filesystem::path(matched->path.substr(1));
        const std::filesystem::path full = options_.assets_root / relative;
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(full, ec);
        std::ifstream file(full, std::ios::binary);
        if (ec || !file || size > options_.max_asset_bytes) {
            SendResponse(socket, "404 Not Found", "text/plain; charset=utf-8", "not found",
                         SecurityHeaders(port));
            return false;
        }
        std::string bytes(static_cast<std::size_t>(size), '\0');
        file.read(bytes.data(), static_cast<std::streamsize>(size));
        if (!file && file.gcount() != static_cast<std::streamsize>(size)) {
            SendResponse(socket, "500 Internal Server Error", "text/plain; charset=utf-8",
                         "read failed", SecurityHeaders(port));
            return false;
        }
        SendResponse(socket, "200 OK", matched->mime.c_str(), bytes, SecurityHeaders(port));
        return false;
    }

    // 其余方法/形状:一律 405,不给目录列表、不给探针。
    SendResponse(socket, "405 Method Not Allowed", "text/plain; charset=utf-8", "method not allowed",
                 SecurityHeaders(port));
    return false;
}

}  // namespace lubancode::app_server
