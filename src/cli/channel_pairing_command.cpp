// channel pairing 子命令实现:合同见 hpp 注释。
#include "cli/channel_pairing_command.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>

#include "channel/channel_config.hpp"
#include "gateway/pairing_command.hpp"
#include "gateway/process.hpp"
#include "gateway/profile.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::cli {

ChannelPairingGate JudgePairingGate(bool lock_present, bool lock_readable,
                                    bool holder_alive, const std::string& boot_id,
                                    unsigned long pid) {
    ChannelPairingGate gate;
    const std::string guidance =
        "配对批准必须提交给运行中的 Gateway——普通进程里的空账不冒充批准。"
        "请先启动 lubancode gateway run,再执行本命令。";
    if (!lock_present) {
        gate.status = PairingGateStatus::NotRunning;
        gate.detail = "gateway.not_running: 没有 Gateway 在跑(锁文件不在)。" + guidance;
        return gate;
    }
    if (!lock_readable) {
        gate.status = PairingGateStatus::BrokenLock;
        gate.detail = "gateway.lock_unreadable: 锁文件在但读不懂,不敢投命令——"
                      "人工核锁(profile 目录下 gateway.lock)。";
        return gate;
    }
    if (!holder_alive) {
        gate.status = PairingGateStatus::StaleLock;
        gate.detail = "gateway.not_running: 锁是陈旧的(持有进程已死透),下次启动自动清。"
                      + guidance;
        return gate;
    }
    gate.status = PairingGateStatus::Submit;
    gate.boot_id = boot_id;
    gate.pid = pid;
    return gate;
}

namespace {

// 对号串:单段 [A-Za-z0-9-](做命令/回执文件名)。
std::string NewCommandId() {
    std::random_device device;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llx-%lx-%lx",
                  static_cast<unsigned long long>(platform::WallClockNowMs()),
                  static_cast<unsigned long>(device()),
                  static_cast<unsigned long>(device()));
    return buffer;
}

}  // namespace

int RunChannelPairingCommand(const ChannelPairingCommandArgs& args) {
    // 渠道/账号 id 先过守门:命令要进 Gateway 的账号目录体系。
    if (!channel::IsValidChannelId(args.channel_id)) {
        std::fprintf(stderr, "channel pairing: 渠道 id 不合法(非空、无路径段、无控制字符): %s\n",
                     args.channel_id.c_str());
        return 1;
    }
    if (!channel::IsValidChannelAccountId(args.account_id)) {
        std::fprintf(stderr, "channel pairing: 账号 id 不合法(非空、无路径段、无控制字符): %s\n",
                     args.account_id.c_str());
        return 1;
    }
    if (args.token.empty()) {
        std::fprintf(stderr, "channel pairing: 配对码或身份不能为空\n");
        return 1;
    }

    // 锁探测(只读,零建目录):投递门——不许拿空 manager 冒充批准成功。
    const std::filesystem::path gateway_root = gateway::DefaultGatewayRoot();
    if (gateway_root.empty()) {
        std::fprintf(stderr,
                     "channel pairing: gateway 状态根不可用(应用根变量坏或找不到主目录)\n");
        return 2;
    }
    const std::string profile_name =
        args.profile.empty() ? std::string(gateway::kDefaultGatewayProfile) : args.profile;
    const gateway::GatewayProfilePaths paths =
        gateway::ResolveGatewayProfilePaths(gateway_root, profile_name);
    std::error_code ec;
    const bool lock_present = std::filesystem::exists(paths.lock_file, ec) && !ec;
    std::string lock_error;
    const auto holder =
        lock_present ? gateway::ReadGatewayLockFile(paths.lock_file, &lock_error)
                     : std::nullopt;
    bool holder_alive = false;
    if (holder.has_value()) {
        const trajectory::SessionLockOwner owner{holder->pid, holder->start_token, 0};
        holder_alive = trajectory::ProbeLockHolder(owner) != trajectory::LockHolderState::Dead;
    }
    const ChannelPairingGate gate =
        JudgePairingGate(lock_present, holder.has_value(), holder_alive,
                         holder.has_value() ? holder->boot_id : std::string(),
                         holder.has_value() ? holder->pid : 0);
    if (gate.status != PairingGateStatus::Submit) {
        std::fprintf(stderr, "channel pairing: %s\n", gate.detail.c_str());
        return 2;
    }

    // 投命令(带目标 boot_id;持锁实例消费后写回执)。
    gateway::GatewayPairingCommand command;
    command.boot_id = gate.boot_id;
    command.command_id = NewCommandId();
    command.action = args.action;
    command.channel_id = args.channel_id;
    command.account_id = args.account_id;
    command.token = args.token;
    command.requested_at_ms = platform::WallClockNowMs();
    const std::string write_error = gateway::WritePairingCommand(paths.control_dir, command);
    if (!write_error.empty()) {
        std::fprintf(stderr, "channel pairing: 命令写不进控制面——%s\n", write_error.c_str());
        return 2;
    }
    std::fprintf(stdout, "已向 Gateway(boot %s)提交 %s,等待回执...\n", gate.boot_id.c_str(),
                 args.action == "approve" ? "批准" : "拒绝");

    // 等回执(100ms 轮询;读到即由 TakePairingCommandResult 删走)。
    const std::int64_t deadline = platform::WallClockNowMs() + args.timeout_ms;
    while (platform::WallClockNowMs() < deadline) {
        std::string result_error;
        const auto result = gateway::TakePairingCommandResult(paths.control_dir,
                                                              command.command_id, &result_error);
        if (result.has_value()) {
            if (result->ok) {
                std::fprintf(stdout, "%s %s/%s 的配对身份:%s\n",
                             args.action == "approve" ? "已批准" : "已拒绝",
                             args.channel_id.c_str(), args.account_id.c_str(),
                             result->sender_id.c_str());
                std::fprintf(stdout, "提醒:批准不会自动补跑对方已发的消息——让 TA 重新发一遍。\n");
                return 0;
            }
            std::fprintf(stderr, "channel pairing: %s 失败(%s)——%s\n", args.action.c_str(),
                         result->error.c_str(), result->detail.c_str());
            return 1;
        }
        if (!result_error.empty()) {
            std::fprintf(stderr, "channel pairing: 回执读不懂——%s\n", result_error.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr,
                 "channel pairing: 命令已投出但没有回执(等了 %dms)——Gateway 在跑却没消费,"
                 "可能是渠道账号未装配或版本旧;命令文件已被实例收下,如需重试请再执行一次。\n",
                 args.timeout_ms);
    return 3;
}

}  // namespace lubancode::cli
