// 本机回环假 HTTP 服务(测试专用,Lua 受控 HTTP 单·阶段 2)。
//
// 章法(设计单 §13.3:HTTP 假服务矩阵全案):
//   - 只绑 127.0.0.1,不碰公网;夹具 Key 一律 FAKE_ 前缀;
//   - 每条连接独立线程,收一份完整请求(状态行+头+Content-Length 体;
//     带 Expect: 100-continue 的先回 100),按编排脚本回响应后关连接;
//   - 脚本按请求到达顺序消耗,耗尽回 500;
//   - 记录收到的每份请求(方法/目标/头/体)供断言——"越权不发包"
//     "3xx 不跟""Secret 只在最终发包头"一类断言都靠这本账;
//   - 连接数单独计数,供"零连接"断言用。
//
// 线程收尾:收连接的线程 detach(挂死类脚本会睡到进程退出,join 会拖死
// 测试);共享状态放 shared_ptr,线程按值捕获,服务对象先走也不悬空。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::test_support {

// 编排的一笔响应。
struct FakeHttpResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // 应答前装死多久(0 = 立刻回)。"等首字节挂死"场景给个大数,让
    // 客户端的墙钟/取消来落锤。
    std::chrono::milliseconds delay_before_response{0};
    // 发多少字节响应体后挂死(默认全部发完)。"读到半截"场景用。
    std::size_t stall_after_body_bytes = static_cast<std::size_t>(-1);
};

// 收到的一份请求(记账快照)。
struct FakeHttpRequest {
    std::string method;   // "GET"/"POST"
    std::string target;   // path?query 原样
    std::vector<std::pair<std::string, std::string>> headers;  // 名字已小写化,断言省心
    std::string body;
};

class FakeHttpServer {
public:
    FakeHttpServer();
    ~FakeHttpServer();

    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    int port() const { return port_; }

    // 编排下一笔响应(按请求到达顺序消耗)。
    void Enqueue(FakeHttpResponse response);

    // 收到的请求账(锁内拷贝快照)。
    std::vector<FakeHttpRequest> requests() const;
    int connection_count() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    int port_ = 0;
};

}  // namespace lubancode::test_support
