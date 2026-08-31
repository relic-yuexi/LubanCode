// app-server 的 WebSocket 承载(多前端外壳单阶段 A):把同一条
// protocol/dispatcher 线搬到 WS 上。承载形状与 stdio 对齐——一条 WS 文本
// 帧 = 一行协议消息(分帧由 WS 自己扛,上层不感知)。
//
// 职责边界:
//   - ws_frames:纯函数握手算料与帧编解码;
//   - ws_sockets:跨平台 socket 薄层;
//   - 这层:监听、连接接入(HTTP 升级)、首帧 token 门、把 Session 折成
//     StdioConnection 的 writer/reader。
//
// 鉴权规矩(todo §六):回环绑定首版免鉴权;配了 token(旗标或
// LUBANCODE_APPSERVER_TOKEN)就启用首帧门——升级完成后第一条文本帧必须
// 是 {"method":"app_server/auth","params":{"token":...}},不过即断(close
// 4001)。token 不落日志、不进错误话。
#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "app_server/ws_frames.hpp"
#include "app_server/ws_sockets.hpp"

namespace lubancode::app_server {

// WS 承载选项。
struct WsOptions {
    std::string bind_host = "127.0.0.1";  // 空串同 127.0.0.1
    int port = 0;                         // 0 = 系统分配(测试)
    std::string token;                    // 空 = 免鉴权;非空 = 首帧门
    int accept_poll_ms = 200;             // accept 轮询粒度(叫停响应速度)
    // artifact 字节口子(阶段 D):GET /artifact/<内容寻址名> 只在这个目录
    // 里取文件(浏览器截图/镜像帧的落盘处)。空 = 口子不开(404)。
    std::string artifact_dir;
};

class WsTransport {
public:
    explicit WsTransport(WsOptions options);
    ~WsTransport();

    // 起监听。失败 false,last_error 有话(可进日志,不含 token)。
    bool Start();
    // 叫停 Accept(等下一次轮询粒度内返回)。
    void Stop() { listener_.Stop(); }
    bool started() const { return started_; }
    int actual_port() const { return listener_.actual_port(); }
    const WsOptions& options() const { return options_; }
    const std::string& last_error() const { return listener_.last_error(); }

    // 一条升级完成的连接:文本帧读写,自动应 ping,对端 Close 即收线。
    class Session {
    public:
        ~Session();

        // 阻塞读下一条文本消息(一段 TCP 里挤着的多条会先落收件箱,逐条
        // 交,不丢)。nullopt = 对端断/协议错/收线。
        std::optional<std::string> ReadMessage();

        // 发一条文本消息(线程安全:写锁串行)。
        bool SendMessage(std::string_view payload);

        // 尽力收线:发 close 帧再关 socket。
        void Close();

    private:
        friend class WsTransport;
        Session(net::Socket socket) : socket_(std::move(socket)) {}
        bool SendRaw(std::string_view bytes);
        bool SendFrame(std::string_view frame);

        net::Socket socket_;
        ws::FrameDecoder decoder_;
        std::mutex write_mutex_;
        bool close_sent_ = false;
        std::deque<std::string> inbox_; // 已解出待交的文本消息
    };

    // 阻塞等一条连接:TCP accept + HTTP 升级 + (配了 token)首帧鉴权。
    // 鉴权不过/升级失败/对端跑了:对端已断,返回 nullopt(不算监听层错,
    // 继续等下一条);监听被叫停或监听层真错也返回 nullopt——调用方看
    // stopped 与 last_error 分辨。
    //
    // 只读 GET(阶段 D):`GET /artifact/<内容寻址名>` 在这层就地应答并
    // 继续等下一条连接(不占 Session,不进协议线)。token 门与 WS 同
    // 规矩——配了 token 的服务,GET 也要票(Bearer 头或 ?token=,恒时
    // 比较,不落日志)。
    std::unique_ptr<Session> Accept();

    // GET /artifact 的执行体:token 门 → 名字形状 → 目录内读文件 → 应答。
    // 应答完连接即关(Connection: close),不污染 WS 面。
    void ServeArtifactGet(net::Socket& socket, const ws::HttpRequestHead& head) const;

private:
    WsOptions options_;
    net::Listener listener_;
    bool started_ = false;
};

// 首帧鉴权消息的形状:{"method":"app_server/auth","params":{"token":...}}。
// 纯函数(单测直接钉):token 对上 true;消息形状不对/串不对 false。
// 恒时比较(逐字节不短路),不给计时侧信道留口。
bool CheckAuthTokenFrame(std::string_view message, std::string_view expected_token);

}  // namespace lubancode::app_server
