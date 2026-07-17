// LSP 协议层。在传输层(Content-Length 分帧的字节流)之上说 JSON-RPC 2.0
// 的方言:initialize/initialized 握手、textDocument/didOpen、定位类请求
// (definition/references/documentSymbol),以及诊断缓存——诊断不是"请求-
// 响应",是服务器 didOpen 之后主动推 textDocument/publishDiagnostics 通知,
// 这里存进 map<uri, 诊断>,查询时读缓存,没到就限时等一小会儿。
//
// Transport 是个抽象接口,不直接认 lsp::StdioTransport——测试能注入一个
// FakeTransport,专门验证"发出去的请求长什么样、响应怎么配对、诊断通知
// 怎么进缓存",不用真起子进程。生产路径用 StartProcess() 在内部造一个
// 真正的 StdioTransport。写法跟 mcp/client.hpp 一个路数,但这是 lsp/ 自己
// 的一份,不 include mcp/ 的任何东西。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lsp/transport.hpp"

namespace lubancode::lsp {

// 本地文件路径 <-> file:// URI。纯函数,单测直接盯得住。
// Windows 盘符路径 "D:\a b\f.py" -> "file:///D:/a%20b/f.py";反向解析时
// 顺带把百分号转义解掉、正斜杠还原成反斜杠(Windows 下)。
std::string PathToUri(const std::string& path_utf8);
std::string UriToPath(const std::string& uri);

// 协议层看到的传输层:能整条整条写消息、能关停、能查活着没、能拿 stderr
// 尾巴。StdioTransport(见 transport.hpp)满足这个接口,测试里的
// FakeTransport 也满足。
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool WriteMessage(const std::string& body) = 0;
    virtual void Shutdown(int wait_ms) = 0;
    virtual bool IsAlive() const = 0;
    virtual std::string StderrTail() const = 0;
};

// StdioTransport 直接实现 Transport 接口,充当生产环境的默认实现。
class StdioTransportAdapter : public Transport {
public:
    StdioTransportAdapter() = default;

    TransportStartResult Start(const std::string& command, const std::vector<std::string>& args,
                                std::function<void(std::string)> on_message) {
        return impl_.Start(command, args, std::move(on_message));
    }

    bool WriteMessage(const std::string& body) override { return impl_.WriteMessage(body); }
    void Shutdown(int wait_ms) override { impl_.Shutdown(wait_ms); }
    bool IsAlive() const override { return impl_.IsAlive(); }
    std::string StderrTail() const override { return impl_.StderrTail(); }

private:
    StdioTransport impl_;
};

// 一个 LSP 服务器的客户端:管一条 stdio 连接的握手、didOpen 去重、请求
// 配对、诊断缓存。请求失败(超时/进程死掉/协议错误)统统翻译成
// unexpected<string> 人话错误,不抛异常。
class Client {
public:
    // name 是给报错信息用的标识(通常是语言名,比如 "cpp")。
    explicit Client(std::string name);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 生产路径:真起一个子进程当传输层。
    TransportStartResult StartProcess(const std::string& command, const std::vector<std::string>& args);

    // 测试路径:注入一个假的 Transport(调用方保留所有权,Client 只持有
    // 裸指针,生命周期由调用方保证)。
    void AttachTransportForTest(Transport* transport);

    // 传输层每凑齐一条完整消息正文,就转发到这里来解析。公开理由同
    // mcp::Client::OnLine:生产回调要绑它,测试模拟"服务器发来一条"也直接调。
    // 消息分三路:带 id 无 method 的是响应(按 id 配对);带 method 无 id 的
    // 是通知(只认 textDocument/publishDiagnostics,进诊断缓存);带 method
    // 又带 id 的是服务器发来的反向请求(clangd 会发 window/workDoneProgress/
    // create 这类),统一回一条 result=null 的空响应,免得服务器等着。
    void OnMessage(const std::string& body);

    // 握手:发 initialize(rootUri 用调用方给的,capabilities 最小),等
    // 响应,再发 initialized 通知。
    std::expected<void, std::string> Initialize(const std::string& root_uri);

    // 首次碰某个文件时发 textDocument/didOpen(通知,不等回应);同一个 uri
    // 只发一次,后续调用是空操作。text 是文件内容(调用方读盘),
    // language_id 按扩展路由出来的语言标识(clangd 认 "cpp",pyright 认
    // "python")。
    void EnsureDidOpen(const std::string& uri, const std::string& language_id, const std::string& text);

    // 定位类请求。line/character 都是 0 基(LSP 原生),1 基转 0 基是
    // 工具层的事。返回响应的 result 字段原样(格式化交给工具层)。
    std::expected<nlohmann::json, std::string> Definition(const std::string& uri, int line, int character);
    std::expected<nlohmann::json, std::string> References(const std::string& uri, int line, int character);
    std::expected<nlohmann::json, std::string> DocumentSymbol(const std::string& uri);

    // 诊断:读缓存;缓存里还没有这个 uri 的诊断,就最多等 wait_ms 毫秒
    // (didOpen 之后服务器会推,通常很快)。等完还没有,返回 nullopt——
    // 工具层译成"暂无诊断"。
    std::optional<nlohmann::json> WaitDiagnostics(const std::string& uri, int wait_ms);

    // 体面关停:发 shutdown 请求(短等)、发 exit 通知,再让传输层等 2s、
    // 不退就杀。幂等。
    void Shutdown();

    bool Alive() const;
    std::string StderrTail() const;

    const std::string& name() const { return name_; }

    // 仅供测试:把请求超时收窄成几十毫秒,免得"验证超时"这条测试也要真等
    // 15s。生产路径永远用默认的 15s。
    void SetTimeoutForTest(int request_timeout_ms);

private:
    struct PendingEntry {
        bool done = false;
        nlohmann::json response;
    };

    std::expected<nlohmann::json, std::string> SendRequestAndWait(const std::string& method,
                                                                    const nlohmann::json& params);
    bool SendNotification(const std::string& method, const nlohmann::json& params);

    std::string name_;

    std::unique_ptr<StdioTransportAdapter> owned_transport_;  // StartProcess 路径下持有
    Transport* transport_ = nullptr;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::int64_t, std::shared_ptr<PendingEntry>> pending_;
    std::map<std::string, nlohmann::json> diagnostics_;  // uri -> publishDiagnostics 的 diagnostics 数组
    std::set<std::string> opened_uris_;
    std::int64_t next_id_ = 1;

    int request_timeout_ms_;
    bool shutdown_done_ = false;
};

}  // namespace lubancode::lsp
