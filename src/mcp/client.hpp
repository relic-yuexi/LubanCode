// M8:MCP 协议层。在传输层(逐行字节流)之上,说 JSON-RPC 2.0 的方言:
// initialize 握手、tools/list 拿工具清单、tools/call 真正执行一次工具。
//
// Transport 是个抽象接口,不直接认 mcp::StdioTransport——这样测试能注入一个
// FakeTransport,专门验证"发出去的请求长什么样、收到什么响应该配对给谁、
// 超时/进程死了该怎么收场",不用真起 Python 子进程。生产路径用
// StartProcess() 在内部造一个真正的 StdioTransport。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/transport.hpp"
#include "tools/tool.hpp"

namespace lubancode::mcp {

// 协议层看到的传输层:只要能一行行写、能关停、能查活着没、能拿 stderr 尾巴。
// StdioTransport(见 transport.hpp)满足这个接口,测试里的 FakeTransport 也
// 满足——两边都不用互相知道对方的存在。
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool WriteLine(const std::string& line) = 0;
    virtual void Shutdown(int wait_ms) = 0;
    virtual bool IsAlive() const = 0;
    virtual std::string StderrTail() const = 0;
};

// StdioTransport 直接实现 Transport 接口,充当生产环境的默认实现。
class StdioTransportAdapter : public Transport {
public:
    StdioTransportAdapter() = default;

    // 转发给内部的 StdioTransport::Start,返回值同 TransportStartResult。
    TransportStartResult Start(const std::string& command, const std::vector<std::string>& args,
                                const std::vector<std::pair<std::string, std::string>>& env,
                                std::function<void(std::string)> on_line) {
        return impl_.Start(command, args, env, std::move(on_line));
    }

    bool WriteLine(const std::string& line) override { return impl_.WriteLine(line); }
    void Shutdown(int wait_ms) override { impl_.Shutdown(wait_ms); }
    bool IsAlive() const override { return impl_.IsAlive(); }
    std::string StderrTail() const override { return impl_.StderrTail(); }

private:
    StdioTransport impl_;
};

// 一个 MCP 服务器暴露的工具描述(来自 tools/list 里的一项)。
struct ToolInfo {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

// 一个 MCP 服务器的客户端:管一条 stdio 连接的握手、请求配对、tools/list、
// tools/call。CallTool 保证不抛异常——协议错误/超时/进程死掉,统统翻译成
// tools::Tool::Result{is_error=true, ...},上层(main.cpp/agent loop)不用
// 关心底下具体是什么故障。
class Client {
public:
    explicit Client(std::string server_name);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 生产路径:真起一个子进程当传输层。
    TransportStartResult StartProcess(const std::string& command, const std::vector<std::string>& args,
                                       const std::vector<std::pair<std::string, std::string>>& env);

    // 测试路径:注入一个假的 Transport(调用方保留所有权,Client 只持有裸指针,
    // 生命周期由调用方保证——测试里 FakeTransport 通常和 Client 同栈帧)。
    void AttachTransportForTest(Transport* transport);

    // 传输层每读到一整行,就转发到这里来解析。公开是因为
    // StdioTransportAdapter::Start 的 on_line 回调要绑到这个方法上;测试里
    // FakeTransport 模拟"服务器发来一行"时也直接调这个方法。
    void OnLine(const std::string& line);

    // 握手:发 initialize 请求,等响应,再发 notifications/initialized 通知。
    std::expected<void, std::string> Initialize();

    // 拿工具清单(tools/list)。
    std::expected<std::vector<ToolInfo>, std::string> ListTools();

    // 执行一次工具调用(tools/call)。不抛异常,失败统统体现在返回值的
    // is_error 字段里。
    // jsonrpc_request_id_out:逐枚追踪单,外层 execution 挂内层请求账用。
    tools::Tool::Result CallTool(const std::string& tool_name, const nlohmann::json& arguments,
                                 std::int64_t* jsonrpc_request_id_out = nullptr);

    // 关停传输层(见 Transport::Shutdown 的语义)。
    void Shutdown();

    // 传输层是否还活着。
    bool Alive() const;

    // 出错时给人看的 stderr 尾巴。
    std::string StderrTail() const;

    const std::string& server_name() const { return server_name_; }

    // 仅供测试用:把默认/tools-call 超时收窄成几十毫秒,免得"验证超时会
    // 变成 is_error"这条测试也要真等 30s/120s。生产路径永远用 kDefaultTimeoutMs/
    // kToolCallTimeoutMs 这两个常量,不调这个方法。
    void SetTimeoutsForTest(int default_timeout_ms, int tool_call_timeout_ms);

private:
    struct PendingEntry {
        bool done = false;
        nlohmann::json response;
    };

    // 发一个带 id 的请求,等回应(或超时/进程死掉/写失败)。返回 result 字段,
    // 或者 unexpected<string> 装着人话错误。
    // jsonrpc_request_id_out(逐枚追踪单):非空时回填本次内层 JSON-RPC id,
    // 外层 tool execution 拿它挂账(超时删 pending 后迟到响应的丢弃也靠这
    // 个 id 关联,不投给新调用)。
    std::expected<nlohmann::json, std::string> SendRequestAndWait(const std::string& method,
                                                                    const nlohmann::json& params, int timeout_ms,
                                                                    std::int64_t* jsonrpc_request_id_out = nullptr);

    // 发一个不带 id 的通知,不等回应。
    bool SendNotification(const std::string& method, const nlohmann::json& params);

    // 逐枚追踪单:迟到响应的留账口。超时删 pending 后响应才到时,读线程
    // 用原 JSON-RPC id 调它(在 reader 线程上,实现自己管线程安全);宿主
    // 的实现翻成 mcp_late_response_dropped trace 事件,关联原 execution。
    void SetLateResponseSink(std::function<void(std::int64_t)> sink) { late_response_sink_ = std::move(sink); }

    std::function<void(std::int64_t)> late_response_sink_;
    std::string server_name_;

    std::unique_ptr<StdioTransportAdapter> owned_transport_;  // StartProcess 路径下持有
    Transport* transport_ = nullptr;                          // 实际使用的传输层(生产/测试路径统一走这个指针)

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::int64_t, std::shared_ptr<PendingEntry>> pending_;
    std::int64_t next_id_ = 1;

    int default_timeout_ms_;
    int tool_call_timeout_ms_;
};

}  // namespace lubancode::mcp
