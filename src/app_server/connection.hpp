// app-server 的 stdio 连接层:进程自己的 stdin/stdout。读写分开:
//   - 读线程:LineFramer 分帧,逐行交 Dispatcher;EOF/断管即收线;
//   - 写线程:从 BoundedOutbox 逐条 Pop、逐行写 stdout,一行一条完整
//     JSON,写完 flush(慢客户端堵的是这条线程与有界队列,不堵代理
//     工作线程)。
//
// stdout 纪律:本模块(及整个 src/app_server/)除协议行外一个 std::cout
// 都不许有;诊断走 stderr(std::fprintf)。
//
// 线程模型:Run() 阻塞到连接收线(EOF、exit 通知、shutdown 应答完、
// 或分帧溢出退线)。请求的处理在读线程上逐条做——骨架期没有耗时业务
// 在别的线程跑(假 backend 的整回合在测试里由驱动线程同步驱动),后续
// 接真回合执行时再引入"请求入队、工作线程出队"的入口队列(kErrServerBusy
// 的有界入口在那条线上立)。
#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_server/dispatcher.hpp"
#include "app_server/framing.hpp"
#include "app_server/outbox.hpp"

namespace lubancode::app_server {

// stdio 连接。一个进程一条;构造后 Run() 进主循环。
class StdioConnection {
public:
    // writer:一行协议消息(不带换行)写出去。生产实现写 stdout + flush;
    // 测试注入假 writer 收集行。
    using LineWriter = std::function<void(const std::string&)>;

    // reader:阻塞读一段字节回来(像 fread/std::getline 的分块版)。
    // 返回空串 = EOF/断管。生产实现包 std::cin;测试注入假 reader。
    using ChunkReader = std::function<std::string()>;

    StdioConnection(std::shared_ptr<Dispatcher> dispatcher, LineWriter writer, ChunkReader reader,
                    std::size_t outbox_capacity = 4096);

    // 主循环:读到 EOF/收到 exit/shutdown/close 为止,返回退出码。
    // 诊断(坏行、溢出)打 stderr。
    int Run();

    // 出站口:事件往这里发(服务层与测试用)。must_keep 分型见
    // outbox.hpp 的 EventMustKeep——这里按 method 自动分型,调用方不用记。
    void EmitEvent(std::string_view method, const nlohmann::json& params);

    BoundedOutbox& outbox() { return outbox_; }
    const std::shared_ptr<Dispatcher>& dispatcher() const { return dispatcher_; }

    // 连接是否已收线(测试断言用)。
    bool closed() const { return closed_.load(); }

private:
    // 处理一行入站:解析、路由、把产出推进出站队列。坏 JSON 回稳定
    // 错误码(kErrParseError,null id),不崩、不死锁、不撞死服务。
    void ProcessLine(const std::string& line);

    std::shared_ptr<Dispatcher> dispatcher_;
    LineWriter writer_;
    ChunkReader reader_;
    BoundedOutbox outbox_;
    LineFramer framer_;
    std::atomic<bool> closed_{false};
};

// 生产实现的 writer/stdout:一行 + '\n' + flush。返回 false 表示 stdout
// 已写坏(断管),调用方据此收线。
bool WriteProtocolLine(const std::string& line);

// 生产实现的 reader:从 stdin 读一段(最多 64KB),EOF/断管返回空串。
std::string ReadStdinChunk();

}  // namespace lubancode::app_server
