// 助理宿主的本地 Web 服务(常驻助理 Web 主界面单 W1,§七合同):
// HTTP 与 WS 同一只监听、同一本地 origin。只绑 127.0.0.1(首版拒绝非
// 回环监听);资源走随包固定 manifest 白名单(HTML/JS/CSS,无外部 CDN);
// 资源缺失启动即明错,不起空壳服务。
//
// 复用 ws_transport 底层:socket 薄层(ws_sockets)、握手算料与帧编解码
// (ws_frames)、升级完成后的 Session(帧读写/自动应 ping/close 收线)
// 全是同一套——这里只新增"受限静态资源 + 认证 + 同源校验"的承载面,
// 不为静态文件手写第二套 HTTP parser 之外的任何东西(解析仍走
// ws::ParseHttpRequestHead/ParseUpgradeRequest 这两只纯函数)。
//
// 认证(§七"本机也要认证"):不沿用"回环免鉴权"。
//   - 严格校验 Host(必须 127.0.0.1:<port> / localhost:<port>,防 DNS
//     rebinding);Origin 若在场必须同源(防跨站)。
//   - 随包静态页面壳允许未登录加载,以便脚本读取 fragment 并换取 cookie。
//     WS 与 artifact 仍要有效会话 cookie(HttpOnly、SameSite=Strict,
//     由 bootstrap 交换发放);没有/无效 → 401,不开放用户数据。
//   - bootstrap 交换:POST /auth/exchange,body 是 URL fragment 里的一次性
//     凭据(不进访问日志/错误话);一次性、限时效、恒时比较(由宿主的
//     WebAuthService 承担),防重放——用过/过期一律 403。
//   - GET /healthz 不鉴权,只回身份(bootId/profile/version/pid,零秘密):
//     重复启动的第二个 CLI 进程凭它对锁账验明正身。
//   - POST /control/open 吃锁文件里的 control_secret(不进 URL),回一张
//     新的 bootstrap URL——同用户本地进程才读得到锁文件,跨用户/远程
//     都拿不到这枚凭据。
//
// 界限(有界):头部 16KB、POST body 8KB、单枚资源 4MB、manifest 之外
// 一律 404(目录穿越/编码绕过/目录列表无从谈起——白名单本身就是墙)。
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/ws_sockets.hpp"
#include "app_server/ws_transport.hpp"

namespace lubancode::app_server {

// 随包网页资源的一条白名单项:URL 路径 -> MIME。manifest 就是全部真相,
// assets_root 里存在但不在表内的文件不伺候。
struct WebAsset {
    std::string path;  // URL 路径(以 / 开头,如 "/index.html")
    std::string mime;  // 应答的 Content-Type
};

// 固定 manifest 的标准形状(助理主界面;文件在 web/assistant/)。
// 换包/加文件:改这里 + 换 assets 目录,不做目录列表。
std::vector<WebAsset> AssistantWebManifest();

struct LocalWebOptions {
    int port = 0;                         // 0 = 系统分配;指定端口被占 = Start 明错
    std::string bind_host = "127.0.0.1";  // 首版只认回环,Start 里拒非回环
    std::filesystem::path assets_root;    // 随包网页根;manifest 全员须在场
    std::vector<WebAsset> manifest;       // 固定白名单
    std::size_t max_asset_bytes = 4 * 1024 * 1024;  // 单枚资源上限

    // 会话 cookie 校验(thread-safe;accept 线程调):值 -> 是否有效。
    std::function<bool(const std::string&)> validate_session;
    std::string cookie_name = "lubancode_assistant_session";

    // bootstrap 交换(thread-safe):一次性凭据 -> 新会话 cookie 值;
    // nullopt = 无效/过期/已用过(403,不区分话,不给试探省事)。
    std::function<std::optional<std::string>(const std::string&)> consume_bootstrap;

    // 健康探测(不鉴权):身份 JSON(bootId/profile/version/pid,零秘密)。
    std::function<nlohmann::json()> health_body;

    // 控制口 POST /control/open:control_secret -> {"url": 新 bootstrap URL};
    // nullopt = 凭据不对(403)。
    std::function<std::optional<nlohmann::json>(const std::string&)> handle_open;

    // artifact 字节口子(与 WS 承载同款名字形状):空 = 不开(404)。
    std::string artifact_dir;
};

class LocalWebServer {
public:
    explicit LocalWebServer(LocalWebOptions options);
    ~LocalWebServer();

    LocalWebServer(const LocalWebServer&) = delete;
    LocalWebServer& operator=(const LocalWebServer&) = delete;

    // 起监听。失败路(都 false + last_error 人话):
    //   - 非回环 bind_host(首版合同,远程访问不靠改 host 无声开启);
    //   - 绑定失败(指定端口被占——带端口号,不偷偷换端口);
    //   - manifest 项在 assets_root 下缺失/超限(资源缺失明错,不起空壳)。
    bool Start();
    // 叫停 accept(下一次轮询粒度内返回)。
    void Stop() { listener_.Stop(); }
    bool started() const { return started_; }
    int actual_port() const { return listener_.actual_port(); }
    const std::string& last_error() const { return last_error_; }

    // accept 主循环(阻塞,宿主跑在自己一条线程上):HTTP 就地应答后继续
    // 等下一条;WS 升级成功的 Session 连同这条连接的请求头(宿主接管旗
    // 这类查询参数从这儿来)经 on_ws_session 交出去(在 accept 线程上调
    // 用——宿主自己排队/串行服务)。监听叫停/监听层错后返回。
    void Run(std::function<void(std::unique_ptr<WsTransport::Session>,
                                const ws::HttpRequestHead& head)> on_ws_session);

    // 同源校验的纯函数(单测直接钉):
    //   HostMustBeLoopback:host 头(原文)对 <port> 合法吗——接受
    //     "127.0.0.1:<port>" 与 "localhost:<port>"(大小写宽容)。
    //   OriginSameLocal:Origin 头对 <port> 是本地同源吗——接受
    //     "http://127.0.0.1:<port>" 与 "http://localhost:<port>";空串
    //     (非浏览器客户端不带)另判:返回 true 只表示"形状上不越界",
    //     是否放行由调用方按资源门再裁。
    static bool HostIsLocalLoopback(const std::string& host_header, int port);
    static bool OriginIsLocalSame(const std::string& origin_header, int port);

private:
    // 一条连接的完整处置。true = WS 升级完成(Session 已应答 101 并交棒,
    // socket 被 move 走);false = HTTP 已就地应答完或对端断(连接随 RAII
    // 关),继续等下一条。
    bool HandleConnection(
        net::Socket& socket,
        const std::function<void(std::unique_ptr<WsTransport::Session>, const ws::HttpRequestHead&)>& on_ws_session);

    LocalWebOptions options_;
    net::Listener listener_;
    bool started_ = false;
    std::string last_error_;
};

// 恒时比较(与 ws_transport 同一把尺;这里独立放一份给 bootstrap/control
// 凭据用,不跨编译单元借私有静态)。
bool WebConstantTimeEqual(std::string_view given, std::string_view expected);

}  // namespace lubancode::app_server
