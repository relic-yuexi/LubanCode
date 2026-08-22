// IdAuthority(显示系统剥离单第四步:补稳定 id)。
//
// thread/turn/item/request 的 id 与事件 seq 由 Runtime 统一发——序号单调、
// thread 内唯一(event.hpp 文件头定过的约定)。app-server 线在等这套号:
// 只此一家,不许各处再造第二套(旧 server.cpp 里的 turn_counter/next_item_seq
// 骨架期临时账,接线时换成本处)。
//
// 形状:
//   thread-<n>   一场会话
//   turn-<n>     一问一答
//   item-<n>     条目(工具/思考/正文块/命令/diff/todo/子代理都落成 item)
//   req-<n>      审批/提问的 request_id(InteractionRequestId 的 value)
//   seq          事件序号,thread 内单调递增,1 起,0 不发
//
// 计数器进程内唯一(atomic 单调);id 字符串前缀区分层,跨 thread 也不撞。
// 线程安全:全部 fetch_add,任意线程可发号;会话层的轮次边界照常串行。
//
// 依赖铁律同合同头:只认标准库,零实现依赖,纯头文件。

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace lubancode::runtime {

// 发号局:一个进程一份(static 计数)。号码只涨不回收;轮次/条目取消了号
// 也作废在序列里(不重排——单调性比紧凑性重要,前端凭 seq 补账)。
class IdAuthority {
public:
    IdAuthority() = default;
    IdAuthority(const IdAuthority&) = delete;
    IdAuthority& operator=(const IdAuthority&) = delete;

    // thread 内单调递增的事件序号。1 起,0 不发(event.hpp 约定 1)。
    std::uint64_t NextSeq() { return seq_.fetch_add(1) + 1; }

    // 三层身份:thread(一场会话)> turn(一问一答)> item(条目)。
    std::string NextThreadId() { return "thread-" + std::to_string(thread_.fetch_add(1) + 1); }
    std::string NextTurnId() { return "turn-" + std::to_string(turn_.fetch_add(1) + 1); }
    std::string NextItemId() { return "item-" + std::to_string(item_.fetch_add(1) + 1); }

    // 审批/提问共用的 request_id(InteractionRequestId::value)。
    std::string NextRequestId() { return "req-" + std::to_string(request_.fetch_add(1) + 1); }

    // 只读(诊断/单测):当前发到几。
    std::uint64_t seq_issued() const { return seq_.load(); }
    std::uint64_t items_issued() const { return item_.load(); }

private:
    std::atomic<std::uint64_t> seq_{0};
    std::atomic<std::uint64_t> thread_{0};
    std::atomic<std::uint64_t> turn_{0};
    std::atomic<std::uint64_t> item_{0};
    std::atomic<std::uint64_t> request_{0};
};

// 进程级发号局。会话层(SessionRuntime,第五步)之后持一份专属的;在那
// 之前(app-server 接线、条目 id 试运行)共用这一份——号码进程内唯一,
// 跨 thread 不撞的保证靠它。
IdAuthority& ProcessIdAuthority();

}  // namespace lubancode::runtime
