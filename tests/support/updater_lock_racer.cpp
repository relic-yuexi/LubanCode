// 更新器安装锁双进程竞争的靶子(批二第③单,照 gateway_lock_racer 模式):
// 真起两只本进程并发抢同一把 updates/.lock,事件流落盘供测试端对账。
// 跑的是生产同一份 InstallRootLock::TryAcquire——不另写一套锁逻辑,
// 测的就是产品代码。
//
// 用法: updater_lock_racer <install_root> <rounds> <hold_ms> <events_out>
//   每轮:TryAcquire -> 成功则持锁 hold_ms 再 Release;拒绝(活持有者/
//   坏锁窗口)则退避 2ms 重试。事件一行一 JSON:
//     {"t":<now_ms>,"event":"acquire"|"release"|"refused"|"stale"|"selfheal"|
//                        "contention"|"error"}
//   时间戳统一取 WallClockNowMs(与测试端同源同钟)。acquire 的 t 在取锁
//   成功之后、release 的 t 在放锁之前——[t_acq,t_rel] 是实际持锁区间的
//   子集,两只 racer 的区间相交即真双持(无假阳性)。
//   stale 事件是清陈锁的证据(竞争册里不应出现——两只都活着;出现了
//   也如实记账,由测试端裁决)。
//   selfheal 事件是清"自家残账"的证据:Release 的 fclose+remove 在
//   Windows 上可能撞上对手并发读账的句柄(MSVC ifstream 缺省共享模式
//   不含 FILE_SHARE_DELETE,DeleteFile 吃 sharing violation),账还立着
//   而句柄早关了。TryAcquire 只有读到 pid=自己且 token=自己才返回
//   RefusedReentrant,而本 racer 每轮头上的 TryAcquire 从不发生在持有
//   中——所以这个态在本处只可能是残账,替 Release 把没删成的那刀补上,
//   清掉接着抢(这刀也可能再撞读账,烧一轮下轮再来)。
//   contention 事件是三轮占位全撞并发尾巴(安装锁竞争失败):竞争本身的
//   极端交错,退避重试,互斥不变量由事件流对账把守。
// 收尾补一行汇总:{"summary":true,"acquired":N,"refused":N,"stale":N,
//                 "selfhealed":N,"contended":N,"errors":N}。
// 退出码:0 = 没发生非预期错误(撞 refused/selfheal/contention 都不算错,
//   是竞争本身或它的善后);1 = 有(只剩 IoError 一类真意外)。
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "updater/layout.hpp"
#include "updater/lock.hpp"

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "用法: updater_lock_racer <install_root> <rounds> <hold_ms> <events_out>\n");
        return 2;
    }
    const std::filesystem::path install_root = argv[1];
    const int rounds = std::stoi(argv[2]);
    const int hold_ms = std::stoi(argv[3]);
    const std::filesystem::path events_out = argv[4];

    std::error_code ec;
    std::filesystem::create_directories(events_out.parent_path(), ec);
    std::ofstream events(events_out, std::ios::binary | std::ios::trunc);
    if (!events) {
        std::fprintf(stderr, "updater_lock_racer: 事件文件打不开\n");
        return 2;
    }
    const auto emit = [&](const char* event, std::int64_t at_ms) {
        events << "{\"t\":" << at_ms << ",\"event\":\"" << event << "\"}\n";
        events.flush();
    };

    const auto paths = lubancode::updater::MakeLayoutPaths(install_root);
    int acquired = 0;
    int refused = 0;
    int stale = 0;
    int selfhealed = 0;
    int contended = 0;
    int errors = 0;
    for (int round = 0; round < rounds; ++round) {
        lubancode::updater::InstallRootLock lock;
        const auto result = lubancode::updater::InstallRootLock::TryAcquire(paths, &lock);
        using Status = lubancode::updater::InstallRootLock::AcquireResult::Status;
        if (result.status == Status::Acquired) {
            ++acquired;
            if (result.cleared_stale) {
                ++stale;
                emit("stale", lubancode::platform::WallClockNowMs());
            }
            emit("acquire", lubancode::platform::WallClockNowMs());  // 记录时刻 >= 实际取到锁
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms > 0 ? hold_ms : 1));
            emit("release", lubancode::platform::WallClockNowMs());  // 记录时刻 <= 实际放掉锁
            lock.Release();
        } else if (result.status == Status::RefusedAliveHolder ||
                   result.status == Status::RefusedBrokenLock) {
            // BrokenLock 是对手 create-new 与写账之间的毫秒窗口(锁的既有
            // 取舍),与活持有者拒绝同路:退避重试,不算错。
            ++refused;
            emit("refused", lubancode::platform::WallClockNowMs());
        } else if (result.status == Status::RefusedReentrant) {
            // 自家残账(来路见文件头):账上 pid/token 都是自己,而本轮头
            // 上我们不持有——替 Release 补删,下轮接着抢。别的进程不会动
            // 活持有者的账(活拒/坏锁不动,死 pid 才清),这刀删的只会是
            // 自己那笔。
            ++selfhealed;
            emit("selfheal", lubancode::platform::WallClockNowMs());
            std::error_code remove_ec;
            std::filesystem::remove(lubancode::updater::InstallLockPath(paths), remove_ec);
        } else if (result.status == Status::ContentionFailure) {
            // 三轮占位全撞并发尾巴:竞争本身的极端交错,不是锁的病——
            // 退避重试,互斥不变量由测试端的事件流对账把守。
            ++contended;
            emit("contention", lubancode::platform::WallClockNowMs());
        } else {
            ++errors;
            emit("error", lubancode::platform::WallClockNowMs());
        }
        // 轮间退避(成功失败都退):失败方的裸重试会在对手一个持锁期里
        // 烧完全部轮数;成功方的紧凑循环又把空窗挤没了。对称退避让两只
        // 的 create 在时间上松散交错,双方都拿到过锁、也真撞过拒绝——
        // 这才是可判的竞争(gateway_lock_racer 同款考量)。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    events << "{\"summary\":true,\"acquired\":" << acquired << ",\"refused\":" << refused
           << ",\"stale\":" << stale << ",\"selfhealed\":" << selfhealed
           << ",\"contended\":" << contended << ",\"errors\":" << errors << "}\n";
    return errors == 0 ? 0 : 1;
}
