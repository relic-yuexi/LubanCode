// channel pairing 子命令的投递门册(QQ 接入单 Q1b):JudgePairingGate
// 纯裁决——无持锁实例明确报错指引,不拿空 manager 冒充批准成功。
// 文件投递/回执往返的管道面在 tests/unit/gateway/test_gateway_pairing_command。
#include <doctest/doctest.h>

#include <string>

#include "cli/channel_pairing_command.hpp"

namespace lubancode::cli {
namespace {

const std::string kBoot = "boot-1";

}  // namespace

TEST_CASE("pairing gate: 无锁文件——报错指引,不投") {
    const ChannelPairingGate gate = JudgePairingGate(/*lock_present=*/false,
                                                    /*lock_readable=*/false,
                                                    /*holder_alive=*/false, "", 0);
    CHECK(gate.status == PairingGateStatus::NotRunning);
    CHECK(gate.detail.find("没有 Gateway 在跑") != std::string::npos);
    CHECK(gate.detail.find("gateway run") != std::string::npos);
    // 指引必须讲清"不许空账冒充批准"这条纪律。
    CHECK(gate.detail.find("不冒充批准") != std::string::npos);
}

TEST_CASE("pairing gate: 锁在但读不懂——保守拒,不投") {
    const ChannelPairingGate gate = JudgePairingGate(/*lock_present=*/true,
                                                    /*lock_readable=*/false,
                                                    /*holder_alive=*/false, "", 0);
    CHECK(gate.status == PairingGateStatus::BrokenLock);
    CHECK(gate.detail.find("读不懂") != std::string::npos);
}

TEST_CASE("pairing gate: 陈旧锁(持有者死透)——按没在跑处理,给指引") {
    const ChannelPairingGate gate = JudgePairingGate(/*lock_present=*/true,
                                                    /*lock_readable=*/true,
                                                    /*holder_alive=*/false, kBoot, 4242);
    CHECK(gate.status == PairingGateStatus::StaleLock);
    CHECK(gate.detail.find("陈旧") != std::string::npos);
}

TEST_CASE("pairing gate: 活持有者——放行投递,带 boot 与 pid") {
    const ChannelPairingGate gate = JudgePairingGate(/*lock_present=*/true,
                                                    /*lock_readable=*/true,
                                                    /*holder_alive=*/true, kBoot, 4242);
    CHECK(gate.status == PairingGateStatus::Submit);
    CHECK(gate.boot_id == kBoot);
    CHECK(gate.pid == 4242);
    CHECK(gate.detail.empty());
}

}  // namespace lubancode::cli
