// 显式全局通知区(按代理状态投影单 P3:输入与生命周期收口)。
//
// §六"所有旁路也得归页":无法归属到某一页的诊断(hooks 归并告警、footer
// 心跳异常、上下文耗尽保留一类的系统侧提醒)不许默认落进当前代理的正文
// ——交互模式下进这一块显式通知区:长在底栏 chrome 帧的最顶一行区,带
// 过期自收,不进滚屏、不抢正文;看谁它都画在同一个位置,谁也不冒充谁的
// 输出。
//
// 非 TTY/管道/重定向的输出合同不动:ReportDiagnosticLine 在非交互终端照旧
// 走 TermErr(stderr),一个字节不改道——交互视图的过滤不许吞掉脚本输出。
#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace lubancode::cli {

// 会话级通知板:有界(留最近 kMaxNotices 条),每条带过期时刻,过期自收。
// 线程安全(内部小锁);ActiveRows 由 chrome 组帧时现拉,不长期持锁。
class GlobalNoticeBoard {
public:
    static constexpr std::size_t kMaxNotices = 3;
    static constexpr std::chrono::milliseconds kDefaultTtl{8000};

    // 压一条诊断(首尾空白裁掉;空串拒收)。同文连续重复的不叠第二条,
    // 只续命——心跳异常那类一秒五拍的源不会把板刷满。
    void Push(std::string text, std::chrono::milliseconds ttl = kDefaultTtl);

    // 此刻还没过期的通知行(按压入次序;过期的那拍就地收走)。
    std::vector<std::string> ActiveRows();

    // 有没有活通知(chrome 组帧前先问一声,空则零行)。
    bool HasActive();

    // 清会话边界收口:/clear、/resume 换代、退场时调用——旧会话的系统侧
    // 提醒不带到新会话的屏上。
    void Clear();

private:
    struct Notice {
        std::string text;
        std::chrono::steady_clock::time_point until;
    };
    void PruneLocked();

    std::mutex mutex_;
    std::vector<Notice> notices_;
};

// 进程内一只(交互会话与诊断产生方共用;单测自建局部板,不碰这份)。
GlobalNoticeBoard& SessionGlobalNotices();

// 诊断行的统一口(替代散落的"TermErr/裸 TermOut 打一行"):
//   - 交互真终端(stdout 是 console 且 stdin 可交互)→ 压进全局通知区,
//     返回 true(调用方不必再落别处);
//   - 否则(管道/重定向/plain)→ 返回 false,调用方按既有输出合同自处
//     (通常 TermErr)。
// 判定只认终端能力,不看当前查看页——全局区不属于任何页,切页不影响它。
bool ReportDiagnosticLine(std::string line);

}  // namespace lubancode::cli
