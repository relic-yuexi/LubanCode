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

// ---- Q1b 四步状态(QQBot Windows 修复单 §5.1 末条) -------------------------

TEST_CASE("four_state: 四步全绿——四行如实,report 齐字段") {
    ChannelFourStateInput input;
    input.account_configured = true;
    input.online = true;
    input.online_detail = "boot boot-1";
    input.pairing_present = true;
    input.pairing_approved = 2;
    input.model_configured = true;
    const ChannelFourStateView view = BuildChannelFourState("qqbot", "main", input);
    REQUIRE(view.lines.size() == 4);
    CHECK(view.lines[0].find("1. 配置已存:是") != std::string::npos);
    CHECK(view.lines[1].find("2. QQ 在线:是") != std::string::npos);
    CHECK(view.lines[1].find("boot-1") != std::string::npos);
    CHECK(view.lines[2].find("3. 身份已配对:是") != std::string::npos);
    CHECK(view.lines[2].find("2 个身份") != std::string::npos);
    CHECK(view.lines[3].find("4. 模型能回复:是") != std::string::npos);
    CHECK(view.report.at("config_saved") == true);
    CHECK(view.report.at("online") == true);
    CHECK(view.report.at("paired") == true);
    CHECK(view.report.at("model_ready") == true);
}

TEST_CASE("four_state: 全空——每步指下一步,不拿后面状态粉饰前面缺口") {
    ChannelFourStateInput input;  // 全默认否
    input.config_detail = "账号 main 不在配置里";
    input.online_detail = "没有连接状态快照";
    input.model_detail = "缺 model";
    const ChannelFourStateView view = BuildChannelFourState("qqbot", "main", input);
    // 步行里穿插"下一步"行,断言按内容找,不赌行号。
    const auto has = [&view](const std::string& needle) {
        for (const std::string& line : view.lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    REQUIRE(view.lines.size() >= 7);  // 四步 + 至少三行"下一步"
    CHECK(has("1. 配置已存:否——账号 main 不在配置里"));
    CHECK(has("2. QQ 在线:否"));
    CHECK(has("3. 身份已配对:否"));
    CHECK(has("还没有人配对"));
    CHECK(has("4. 模型能回复:否——缺 model"));
    CHECK(has("channel setup qqbot"));
    CHECK(has("gateway run"));
    CHECK(view.report.at("config_saved") == false);
    CHECK(view.report.at("online") == false);
    CHECK(view.report.at("paired") == false);
    CHECK(view.report.at("model_ready") == false);
}

TEST_CASE("four_state: 待批准一笔——指引 approve 命令与配对码去处") {
    ChannelFourStateInput input;
    input.account_configured = true;
    input.online = true;
    input.pairing_present = true;
    input.pairing_pending = 1;
    input.model_configured = true;
    const ChannelFourStateView view = BuildChannelFourState("qqbot", "main", input);
    const auto has = [&view](const std::string& needle) {
        for (const std::string& line : view.lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    REQUIRE(view.lines.size() >= 5);
    CHECK(has("有待批准的配对 1 笔"));
    CHECK(has("channel pairing approve qqbot main"));
    CHECK(view.report.at("pairing_pending") == 1);
}

TEST_CASE("four_state: 配对账读不懂——如实报未知,不冒充 0 个") {
    ChannelFourStateInput input;
    input.pairing_present = true;
    input.pairing_parse_ok = false;
    const ChannelFourStateView view = BuildChannelFourState("qqbot", "main", input);
    const auto has = [&view](const std::string& needle) {
        for (const std::string& line : view.lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    CHECK(has("未知"));
    CHECK(has("pairing.json"));
    CHECK(view.report.at("paired") == "unreadable");
}

}  // namespace lubancode::cli
