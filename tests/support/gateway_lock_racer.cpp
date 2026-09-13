// 锁双进程竞争的靶子(常驻总装 V0 第四件事):真起两只本进程并发抢同一把
// 锁,事件流落盘供测试端对账。跑的是生产同一份 TryAcquire(gateway 或
// channel account 两把锁,mode 选)——不另写一套锁逻辑,测的就是产品代码。
//
// 用法: gateway_lock_racer <mode:gateway|account> <lock_file> <boot_id> <rounds> <hold_ms> <events_out>
//   每轮:TryAcquire → 成功则持锁 hold_ms 再 Release;活持有者拒绝则
//   立即重试(无间隔,模拟最凶的启动竞态)。事件一行一 JSON:
//     {"t":<now_ms>,"event":"acquire"|"release"|"refused"|"error"}
//   时间戳统一取 WallClockNowMs(与测试端同源同钟)。
//   acquire 的 t 在取锁成功之后、release 的 t 在放锁之前——[t_acq,t_rel]
//   是实际持锁区间的子集,两只 racer 的区间相交即真双持(无假阳性)。
// 收尾补一行汇总:{"acquired":N,"refused":N,"errors":N}。
// 退出码:0 = 没发生非预期错误(撞 refused 不算错,是竞争本身);1 = 有。
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "channel/account_lock.hpp"
#include "gateway/process.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

namespace {

std::int64_t NowMs() {
    return lubancode::platform::WallClockNowMs();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                     "用法: gateway_lock_racer <mode:gateway|account> <lock_file> <boot_id> "
                     "<rounds> <hold_ms> <events_out>\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::filesystem::path lock_file = argv[2];
    const std::string boot_id = argv[3];
    const int rounds = std::stoi(argv[4]);
    const int hold_ms = std::stoi(argv[5]);
    const std::filesystem::path events_out = argv[6];

    std::error_code ec;
    std::filesystem::create_directories(events_out.parent_path(), ec);
    std::ofstream events(events_out, std::ios::binary | std::ios::trunc);
    if (!events) {
        std::fprintf(stderr, "gateway_lock_racer: 事件文件打不开\n");
        return 2;
    }
    const auto emit = [&](const char* event, std::int64_t at_ms) {
        events << "{\"t\":" << at_ms << ",\"event\":\"" << event << "\"}\n";
        events.flush();
    };

    // 账号锁的记录按 ChannelManager 真实形状组:pid+start_time_ms+
    // instance_token(=boot_id)+generation;gateway 锁一 boot 一 epoch。
    lubancode::channel::AccountLockRecord account_self;
    account_self.pid = lubancode::platform::CurrentProcessId();
    account_self.start_time_ms = NowMs();
    account_self.acquired_at_ms = NowMs();
    account_self.generation = 1;
    account_self.instance_token = boot_id;

    lubancode::gateway::GatewayLockRecord gateway_self;
    gateway_self.pid = lubancode::platform::CurrentProcessId();
    gateway_self.start_token = lubancode::trajectory::CurrentProcessStartToken();
    gateway_self.boot_id = boot_id;
    gateway_self.owner_epoch = boot_id;  // 一 boot 一 epoch(V0)
    gateway_self.acquired_at_ms = NowMs();

    int acquired = 0;
    int refused = 0;
    int errors = 0;
    for (int round = 0; round < rounds; ++round) {
        if (mode == "account") {
            lubancode::channel::AccountLock lock;
            const auto result = lubancode::channel::AccountLock::TryAcquire(
                lock_file, account_self, lubancode::channel::AccountLock::DefaultAliveChecker(),
                &lock);
            using Status = lubancode::channel::AccountLock::AcquireResult::Status;
            if (result.status == Status::Acquired) {
                ++acquired;
                emit("acquire", NowMs());
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms > 0 ? hold_ms : 1));
                emit("release", NowMs());
                lock.Release();
            } else if (result.status == Status::RefusedAliveHolder) {
                ++refused;
                emit("refused", NowMs());
            } else {
                ++errors;
                emit("error", NowMs());
            }
            continue;
        }
        lubancode::gateway::GatewayLock lock;
        const auto result =
            lubancode::gateway::GatewayLock::TryAcquire(lock_file, gateway_self, &lock);
        using Status = lubancode::gateway::GatewayLock::AcquireResult::Status;
        if (result.status == Status::Acquired) {
            ++acquired;
            emit("acquire", NowMs());  // 记录时刻 >= 实际取到锁
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms > 0 ? hold_ms : 1));
            emit("release", NowMs());  // 记录时刻 <= 实际放掉锁
            lock.Release();
        } else if (result.status == Status::RefusedAliveHolder) {
            ++refused;  // 竞争本身的证据,不是错
            emit("refused", NowMs());
        } else {
            ++errors;
            emit("error", NowMs());
        }
    }
    events << "{\"summary\":true,\"acquired\":" << acquired << ",\"refused\":" << refused
           << ",\"errors\":" << errors << "}\n";
    return errors == 0 ? 0 : 1;
}
