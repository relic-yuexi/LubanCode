// 常驻助理 Web 主界面单 W3 的渠道面册:channel/list、channel/status 的
// 只读四态投影(盘上三份事实:config.json channels 段 / 连接快照 /
// pairing 账)与 channel/pairing/respond 的转发合同(锁探测门 →
// pairing 控制命令 → 回执;无持锁 gateway 明拒);gateway/service/status
// 的 V4 只读投影(install.json + 实例活态)。
//
// 覆盖(单 §九 W3 验收的 CI 可验部分;真平台在线/收发两轮归 Q3 真机):
//   - 四态投影:配置/在线/配对/模型四步如实分栏(哪步卡住指哪步);
//   - 待批准清单:sender + 过期时刻(code_hash 不出账);
//   - 配对转发:伪持锁 gateway(同款 GatewayLock + PollPairingCommands
//     消费 + 回执文件)命中;锁不在/持有者死透明拒,不冒充批准;
//   - 服务状态:install.json 在/不在如实;零副作用。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/assistant_channels.hpp"
#include "app_server/protocol.hpp"
#include "gateway/pairing_command.hpp"
#include "gateway/process.hpp"
#include "gateway/profile.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "trajectory/session_lock.hpp"

using namespace lubancode;
using namespace lubancode::app;

namespace {

std::int64_t WallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

// 一套 W3 装配:临时渠道状态根 + 临时 gateway profile 树 + 注入 seam。
struct W3Fixture {
    std::filesystem::path root;
    std::filesystem::path channels_root;
    gateway::GatewayProfilePaths gateway_paths;
    std::shared_ptr<AssistantChannelFace> face;
    bool model_configured = true;

    explicit W3Fixture(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-asst-w3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        channels_root = root / "channels";
        gateway_paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");

        AssistantChannelFace::Options options;
        options.channels_state_root = channels_root;
        options.gateway_paths = gateway_paths;
        options.config_path = root / "config.json";
        options.now_ms = [this] { return now_ms_impl(); };
        // 模型探针:按案内旗回(第四步)。
        options.model_probe = [this]() -> std::pair<bool, std::string> {
            if (model_configured) {
                return {true, std::string()};
            }
            return {false, std::string("缺 model")};
        };
        // 探活:按 pid 白名单(测试进程自身恒活,别的号恒死)。
        options.is_alive = [this](unsigned long pid) { return alive_pids.count(pid) > 0; };
        face = std::make_shared<AssistantChannelFace>(std::move(options));
    }

    std::map<unsigned long, int> alive_pids;
    std::int64_t now_override_ms = 0;
    std::int64_t now_ms_impl() { return now_override_ms > 0 ? now_override_ms : WallMs(); }

    void WriteConfig(const std::string& channels_json) {
        WriteTextFile(root / "config.json", "{\"channels\": " + channels_json + "}");
    }

    void WriteSnapshot(const std::string& channel_id, const std::string& account_id,
                       const nlohmann::json& snapshot) {
        WriteTextFile(channels_root / channel_id / account_id / "connection-status.json",
                      snapshot.dump());
    }

    void WritePairing(const std::string& channel_id, const std::string& account_id,
                      const nlohmann::json& pairing) {
        WriteTextFile(channels_root / channel_id / account_id / "pairing.json", pairing.dump());
    }
};

// 连接快照(脱敏形状与 ChannelConnectionReporter 一致)。
nlohmann::json MakeSnapshot(bool connected, unsigned long pid, std::int64_t updated_at_ms) {
    nlohmann::json snapshot = nlohmann::json{
        {"schema", 1}, {"channel_id", "qqbot"}, {"account_id", "main"},
        {"connected", connected}, {"thread_alive", true},
        {"stage", connected ? "connected" : "connecting"},
        {"boot_id", "boot-gw-1"}, {"pid", pid}, {"updated_at_ms", updated_at_ms}};
    if (!connected) {
        snapshot["last_failure"] = nlohmann::json{{"stage", "connecting"},
                                                  {"error_code", "connect_refused"},
                                                  {"detail", "connect refused"},
                                                  {"at_ms", updated_at_ms}};
        snapshot["next_retry_at_ms"] = updated_at_ms + 5'000;
    }
    return snapshot;
}

}  // namespace

// ---------------------------------------------------------------------------
// 只读四态投影
// ---------------------------------------------------------------------------

TEST_CASE("channel/status: 四态如实分栏——配置在册、在线按快照裁决、配对带清单") {
    W3Fixture fixture("fourstate");
    fixture.WriteConfig(R"({
      "qqbot": {
        "enabled": true,
        "accounts": {
          "main": {"enabled": true, "app_id": "123456", "secret_env": "QQBOT_SECRET"}
        }
      }
    })");
    fixture.alive_pids[platform::CurrentProcessId()] = 1;
    fixture.WriteSnapshot("qqbot", "main",
                          MakeSnapshot(true, platform::CurrentProcessId(), WallMs()));
    fixture.WritePairing("qqbot", "main", nlohmann::json{
        {"records", nlohmann::json::array({
            nlohmann::json{{"channel_id", "qqbot"},
                           {"account_id", "main"},
                           {"sender_id", "owner-openid"},
                           {"code_hash", "deadbeef"},
                           {"created_at_ms", WallMs() - 1000},
                           {"expires_at_ms", WallMs() + 240'000},
                           {"status", "pending"}},
            nlohmann::json{{"channel_id", "qqbot"},
                           {"account_id", "main"},
                           {"sender_id", "old-friend"},
                           {"code_hash", "cafebabe"},
                           {"created_at_ms", WallMs() - 9000'000},
                           {"expires_at_ms", WallMs() - 600'000},
                           {"status", "approved"}},
        })}});

    int error_code = 0;
    std::string error_message;
    const nlohmann::json status = fixture.face->HandleChannelStatus(
        nlohmann::json{{"channelId", "qqbot"}, {"accountId", "main"}}, error_code,
        error_message);
    CHECK(error_code == 0);
    CHECK(status["channelId"] == "qqbot");
    REQUIRE(status.contains("fourState"));
    CHECK(status["fourState"]["config_saved"] == true);
    CHECK(status["fourState"]["online"] == true);
    CHECK(status["fourState"]["paired"] == true);  // 有一枚已批准
    CHECK(status["fourState"]["model_ready"] == true);
    CHECK(status["online"] == true);
    // 待批准清单:pending 一笔(sender + 过期时刻);code_hash 不出账。
    REQUIRE(status["pendingPairings"].size() == 1);
    CHECK(status["pendingPairings"][0]["senderId"] == "owner-openid");
    CHECK(status["pendingPairings"][0]["expiresAtMs"].get<std::int64_t>() > WallMs());
    const std::string dumped = status.dump();
    CHECK(dumped.find("deadbeef") == std::string::npos);
    CHECK(dumped.find("cafebabe") == std::string::npos);
    // 细图带连接明细行(单账号 include_detail)。
    CHECK(status.contains("connectionDetail"));
    CHECK(status.contains("fourStateLines"));
}

TEST_CASE("channel/status: 快照进程死/账号不在册——哪步卡住指哪步,不粉饰") {
    W3Fixture fixture("stuck");
    fixture.WriteConfig(R"({"qqbot": {"enabled": true, "accounts": {"main": {}}}})");
    // 账号在册但未启用 + 无 AppID → 第一步卡;快照 pid 死 → 第二步卡。
    fixture.WriteSnapshot("qqbot", "main", MakeSnapshot(true, 999999, WallMs()));

    int error_code = 0;
    std::string error_message;
    const nlohmann::json status = fixture.face->HandleChannelStatus(
        nlohmann::json{{"channelId", "qqbot"}, {"accountId", "main"}}, error_code,
        error_message);
    CHECK(error_code == 0);
    CHECK(status["fourState"]["config_saved"] == false);
    CHECK(status["fourState"]["config_detail"].get<std::string>().find("AppID") !=
          std::string::npos);
    CHECK(status["fourState"]["online"] == false);
    CHECK(status["fourState"]["online_detail"].get<std::string>().find("已退出") !=
          std::string::npos);
    CHECK(status["online"] == false);
    // 没人配对过:pairing 账不在,pending 清单空。
    CHECK(status["pendingPairings"].size() == 0);
    CHECK(status["fourState"]["paired"] == false);
}

TEST_CASE("channel/list: 枚举配置的全部账号;空配置如实回") {
    W3Fixture fixture("list");
    {
        int error_code = 0;
        std::string error_message;
        const nlohmann::json listed =
            fixture.face->HandleChannelList(nlohmann::json::object(), error_code,
                                            error_message);
        CHECK(error_code == 0);
        CHECK(listed["accounts"].size() == 0);
        // 配置文件不存在:如实报 channelsError,不冒充空配置。
        CHECK(listed.contains("channelsError"));
        CHECK(listed["channelsError"].get<std::string>().find("不存在") != std::string::npos);
        CHECK(listed["credentialHint"].get<std::string>().find("不在网页录入") !=
              std::string::npos);
    }
    fixture.WriteConfig(R"({
      "qqbot": {"enabled": true, "accounts": {
        "main": {"enabled": true, "app_id": "1", "secret_env": "S"},
        "second": {"enabled": true, "app_id": "2", "secret_env": "S"}
      }}
    })");
    int error_code = 0;
    std::string error_message;
    const nlohmann::json listed =
        fixture.face->HandleChannelList(nlohmann::json::object(), error_code, error_message);
    CHECK(error_code == 0);
    REQUIRE(listed["accounts"].size() == 2);
    CHECK(listed["accounts"][0]["accountId"] == "main");
    CHECK(listed["accounts"][1]["accountId"] == "second");
    // 列表是摘要面(不带明细行)。
    CHECK_FALSE(listed["accounts"][0].contains("connectionDetail"));
}

// ---------------------------------------------------------------------------
// 配对转发(同一控制面合同)
// ---------------------------------------------------------------------------

namespace {

// 伪持锁 gateway:占 GatewayLock + 消费 pairing 命令 + 写回执。与
// ChannelGatewayWiring::ConsumePairingCommands 同款通道(锁/命令/回执
// 三件),不引真渠道。
class FakePairingGateway {
public:
    FakePairingGateway(gateway::GatewayProfilePaths paths, std::string boot_id)
        : paths_(std::move(paths)), boot_id_(std::move(boot_id)) {}

    bool Start() {
        gateway::GatewayLockRecord self;
        self.pid = platform::CurrentProcessId();
        self.start_token = trajectory::CurrentProcessStartToken();
        self.boot_id = boot_id_;
        self.owner_epoch = boot_id_;
        self.acquired_at_ms = WallMs();
        auto acquire =
            gateway::GatewayLock::TryAcquire(paths_.lock_file, self, &lock_);
        return acquire.status == gateway::GatewayLock::AcquireResult::Status::Acquired;
    }

    // 消费一枚命令并写成功回执(sender 原样回)。超时没等到回 false。
    bool ApproveOne(std::string* out_command_token) {
        const std::int64_t deadline = WallMs() + 15'000;
        while (WallMs() < deadline) {
            const auto commands = gateway::PollPairingCommands(paths_.control_dir, boot_id_);
            if (!commands.empty()) {
                gateway::GatewayPairingCommandResult result;
                result.command_id = commands[0].command_id;
                result.action = commands[0].action;
                result.ok = true;
                result.sender_id = commands[0].token;
                const std::string error =
                    gateway::WritePairingCommandResult(paths_.control_dir, result);
                *out_command_token = commands[0].token;
                return error.empty();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void Stop() { lock_.Release(); }

private:
    gateway::GatewayProfilePaths paths_;
    std::string boot_id_;
    gateway::GatewayLock lock_;
};

}  // namespace

TEST_CASE("channel/pairing/respond: 转发到持锁 gateway 的控制面,回执带回 sender") {
    W3Fixture fixture("pairing");
    FakePairingGateway gateway(fixture.gateway_paths, "boot-pair-1");
    REQUIRE(gateway.Start());

    std::string consumed_token;
    std::thread consumer([&gateway, &consumed_token]() {
        std::string token;
        // doctest 断言不进线程——结果收回来主线程断。
        if (gateway.ApproveOne(&token)) {
            consumed_token = token;
        }
    });

    int error_code = 0;
    std::string error_message;
    const nlohmann::json result = fixture.face->HandleChannelPairingRespond(
        nlohmann::json{{"channelId", "qqbot"},
                       {"accountId", "main"},
                       {"token", "owner-openid"},
                       {"action", "approve"}},
        error_code, error_message);
    consumer.join();
    gateway.Stop();

    CHECK(error_code == 0);
    REQUIRE(result.value("resolved", false) == true);
    CHECK(result["senderId"] == "owner-openid");
    CHECK(consumed_token == "owner-openid");  // 命令面收到的是同一笔(转发不换口)
    // 提醒文案如实(批准不自动补跑)。
    CHECK(result["note"].get<std::string>().find("重新发一遍") != std::string::npos);
}

TEST_CASE("channel/pairing/respond: 没有持锁 gateway——明拒不冒充批准") {
    W3Fixture fixture("nogw");
    int error_code = 0;
    std::string error_message;
    const nlohmann::json result = fixture.face->HandleChannelPairingRespond(
        nlohmann::json{{"channelId", "qqbot"},
                       {"accountId", "main"},
                       {"token", "owner-openid"},
                       {"action", "approve"}},
        error_code, error_message);
    CHECK(error_code == app_server::kErrInternalError);
    CHECK(error_message.find("gateway.not_running") != std::string::npos);
    CHECK(error_message.find("先启动") != std::string::npos);
    // 投递门挡在写命令之前:控制目录压根没被建。
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(fixture.gateway_paths.control_dir, ec));
}

TEST_CASE("channel/pairing/respond: 坏参数明拒") {
    W3Fixture fixture("badparams");
    const auto expect_reject = [&fixture](const nlohmann::json& params) {
        int error_code = 0;
        std::string error_message;
        (void)fixture.face->HandleChannelPairingRespond(params, error_code, error_message);
        CHECK(error_code == app_server::kErrInvalidParams);
    };
    expect_reject(nlohmann::json{{"channelId", "qqbot"}, {"accountId", "main"},
                                 {"token", "x"}, {"action", "maybe"}});
    expect_reject(nlohmann::json{{"channelId", "qqbot"}, {"accountId", "main"},
                                 {"action", "approve"}});
    expect_reject(nlohmann::json{{"channelId", "../escape"}, {"accountId", "main"},
                                 {"token", "x"}, {"action", "approve"}});
}

// ---------------------------------------------------------------------------
// 系统托管只读投影(V4)
// ---------------------------------------------------------------------------

TEST_CASE("gateway/service/status: install.json 在——如实带版本与实例;不在——未安装") {
    W3Fixture fixture("service");
    {
        int error_code = 0;
        std::string error_message;
        const nlohmann::json status = fixture.face->HandleServiceStatus(
            nlohmann::json::object(), error_code, error_message);
        CHECK(error_code == 0);
        CHECK(status["installed"] == false);
        CHECK(status["gatewayInstance"]["state"] == "not_running");
        CHECK(status["boundary"].get<std::string>().find("页面不代跑安装") != std::string::npos);
    }
    // 落一份 install.json(与 V4 ServiceInstallRecord 同一形状)。
    gateway::ServiceInstallRecord record;
    record.platform = "windows";
    record.profile = "default";
    record.exe_path = "C:/tools/lubancode.exe";
    record.lubancode_version = "0.26.300";
    record.task_name = "LubanCode Gateway (default)";
    record.gateway_root = (fixture.root / "gateway").generic_string();
    record.service_log = "C:/logs/gateway.log";
    record.installed_at_ms = WallMs();
    WriteTextFile(fixture.gateway_paths.profile_dir / "service" / "install.json",
                  record.ToJson().dump());

    int error_code = 0;
    std::string error_message;
    const nlohmann::json status = fixture.face->HandleServiceStatus(nlohmann::json::object(),
                                                                    error_code, error_message);
    CHECK(error_code == 0);
    CHECK(status["installed"] == true);
    REQUIRE(status.contains("installRecord"));
    CHECK(status["installRecord"]["lubancodeVersion"] == "0.26.300");
    CHECK(status["installRecord"]["exePath"].get<std::string>().find("lubancode.exe") !=
          std::string::npos);
    CHECK(status["gatewayInstance"]["state"] == "not_running");
}
