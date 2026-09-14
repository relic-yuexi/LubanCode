// channel status 子命令裁决册(连接状态单 §三 P0-A):JudgeChannelStatus
// 纯函数——进程死/快照过期/未连接/在线四路裁决;在线只认 connected=true
// 且进程活且快照新鲜(不凭 PID 宣告成功,不把旧快照当在线)。
// RunChannelStatusCommand 的文件定位/打印壳不再钉(同款模板在
// test_gateway_status 类册的口径;纯逻辑在此全覆盖)。
#include <doctest/doctest.h>

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "cli/channel_status_command.hpp"

namespace lubancode::cli {
namespace {

nlohmann::json BaseSnapshot() {
    return nlohmann::json{
        {"schema", 1},
        {"channel_id", "qqbot"},
        {"account_id", "main"},
        {"connected", false},
        {"thread_alive", true},
        {"stage", "connecting"},
        {"last_failure",
         nlohmann::json{{"stage", "connecting"},
                        {"error_code", "connect_refused"},
                        {"detail", "connect refused"},
                        {"at_ms", 100}}},
        {"retry_count", 2},
        {"next_retry_at_ms", 1'900},
        {"connected_since_ms", 0},
        {"boot_id", "boot-1"},
        {"pid", 4242},
        {"updated_at_ms", 1'000}};
}

const std::function<bool(unsigned long)> kAlive = [](unsigned long) { return true; };
const std::function<bool(unsigned long)> kDead = [](unsigned long) { return false; };

}  // namespace

TEST_CASE("channel status: 无快照——不在线,verdict=no_snapshot") {
    const ChannelStatusVerdict verdict =
        JudgeChannelStatus(nullptr, "qqbot", "main", 2'000, kAlive);
    CHECK_FALSE(verdict.online);
    CHECK(verdict.exit_code != 0);
    CHECK(verdict.report.at("verdict") == "no_snapshot");
    CHECK(verdict.lines.at(0).find("没有连接状态快照") != std::string::npos);
}

TEST_CASE("channel status: 进程已退——旧快照不当作在线") {
    nlohmann::json snapshot = BaseSnapshot();
    snapshot["connected"] = true;  // 即使快照自称在线
    const ChannelStatusVerdict verdict =
        JudgeChannelStatus(&snapshot, "qqbot", "main", 2'000, kDead);
    CHECK_FALSE(verdict.online);
    CHECK(verdict.exit_code != 0);
    CHECK(verdict.report.at("verdict") == "process_dead");
}

TEST_CASE("channel status: 快照过期——在线状态未知,按不在线处理") {
    nlohmann::json snapshot = BaseSnapshot();
    snapshot["connected"] = true;
    snapshot["updated_at_ms"] = 1'000;  // now=70'000:69 秒没刷新
    const ChannelStatusVerdict verdict =
        JudgeChannelStatus(&snapshot, "qqbot", "main", 70'000, kAlive);
    CHECK_FALSE(verdict.online);
    CHECK(verdict.exit_code != 0);
    CHECK(verdict.report.at("verdict") == "stale_snapshot");
}

TEST_CASE("channel status: 未连接——退非零,带阶段与最近失败") {
    nlohmann::json snapshot = BaseSnapshot();  // connected=false,新鲜,进程活
    const ChannelStatusVerdict verdict =
        JudgeChannelStatus(&snapshot, "qqbot", "main", 2'000, kAlive);
    CHECK_FALSE(verdict.online);
    CHECK(verdict.exit_code != 0);
    CHECK(verdict.report.at("verdict") == "not_connected");
    const std::string& line = verdict.lines.at(0);
    CHECK(line.find("未连接") != std::string::npos);
    CHECK(line.find("connect_refused") != std::string::npos);
    CHECK(line.find("connecting") != std::string::npos);
}

TEST_CASE("channel status: 在线——connected=true 且进程活且新鲜才退 0") {
    nlohmann::json snapshot = BaseSnapshot();
    snapshot["connected"] = true;
    snapshot["stage"] = "connected";
    snapshot["connected_since_ms"] = 900;
    snapshot["last_failure"] = nullptr;
    const ChannelStatusVerdict verdict =
        JudgeChannelStatus(&snapshot, "qqbot", "main", 2'000, kAlive);
    CHECK(verdict.online);
    CHECK(verdict.exit_code == 0);
    CHECK(verdict.report.at("verdict") == "connected");
    CHECK(verdict.lines.at(0).find("已连接 QQ") != std::string::npos);
    CHECK(verdict.lines.at(0).find("boot-1") != std::string::npos);
    // pid 活但不在线的情况在上面 not_connected 案——PID 活不等于成功。
}

TEST_CASE("channel status: 快照缺键(pid/updated_at 缺失)按死/过期处理,不越界") {
    nlohmann::json snapshot = nlohmann::json{{"connected", true}};
    // 没 pid:process_dead 路径。
    const ChannelStatusVerdict no_pid =
        JudgeChannelStatus(&snapshot, "qqbot", "main", 2'000, kAlive);
    CHECK(no_pid.report.at("verdict") == "process_dead");
    // 有 pid 活但没 updated_at:stale 路径。
    snapshot["pid"] = 4242;
    const ChannelStatusVerdict no_stamp =
        JudgeChannelStatus(&snapshot, "qqbot", "main", 2'000, kAlive);
    CHECK(no_stamp.report.at("verdict") == "stale_snapshot");
}

}  // namespace lubancode::cli
