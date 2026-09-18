// 渠道连接状态宿主输出件册(连接状态单 §三 P0-A):限频打印、边沿立即
// 打印、脱敏清洗、跨进程快照发布(boot ID/更新时间/节流刷新)。
// 快照来源注入手造 ConnectionSnapshot——本册只钉输出与发布行为,不涉真
// 网络与线程(adapter 侧的状态生命周期在 test_qq_adapter.cpp 钉)。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <istream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/channel_connection_reporter.hpp"

namespace lubancode::app {
namespace {

using channel::qq::ConnectionSnapshot;
using channel::qq::ConnectionFailure;

struct Captured {
    std::mutex mutex;
    std::vector<std::string> lines;

    void Capture() {
        emit = [this](const std::string& line) {
            const std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(line);
        };
    }
    // 取走并清空:断言只对"自上次取走以来"的新输出成立。
    std::vector<std::string> Take() {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out = std::move(lines);
        lines.clear();
        return out;
    }
    std::function<void(const std::string&)> emit;
};

// 手造快照源(测试直接改 current,Observe 时取)。
struct SnapshotSource {
    std::mutex mutex;
    ConnectionSnapshot current;

    ChannelConnectionReporter::Account AccountOf(const std::string& channel = "qqbot",
                                                 const std::string& account = "main") {
        ChannelConnectionReporter::Account out;
        out.channel_id = channel;
        out.account_id = account;
        out.snapshot = [this]() {
            const std::lock_guard<std::mutex> lock(mutex);
            return current;
        };
        return out;
    }
};

ConnectionFailure Fail(const char* code, const char* stage = "connecting") {
    return ConnectionFailure{stage, code, "some sanitized reason", 12345};
}

std::filesystem::path MakeTempRoot(const char* tag) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lubancode-conn-reporter-test-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

nlohmann::json ReadJsonFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

}  // namespace

TEST_CASE("reporter: 开场→失败→在线→断线→停止的边沿都立即打印") {
    SnapshotSource source;
    Captured captured;
    captured.Capture();
    const std::filesystem::path root = MakeTempRoot("edges");
    ChannelConnectionReporter::Deps deps;
    deps.channels_state_root = root;
    deps.emit = captured.emit;
    deps.accounts.push_back(source.AccountOf());
    ChannelConnectionReporter reporter(std::move(deps));

    std::int64_t now = 1'000;
    source.current.thread_alive = true;
    source.current.stage = channel::qq::kStageFetchingToken;
    reporter.Observe("boot-1", 42, now);
    // 失败(首条):立即。
    source.current.last_failure = Fail("connect_refused");
    reporter.Observe("boot-1", 42, now += 100);
    auto lines = captured.Take();
    REQUIRE(lines.size() >= 2);
    CHECK(lines[0].find("已装配") != std::string::npos);
    CHECK(lines[1].find("连接失败") != std::string::npos);
    CHECK(lines[1].find("connect_refused") != std::string::npos);

    // 在线:立即。
    source.current.last_failure.reset();
    source.current.connected = true;
    source.current.stage = channel::qq::kStageConnected;
    reporter.Observe("boot-1", 42, now += 100);
    lines = captured.Take();
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().find("已连接 QQ,等待消息") != std::string::npos);

    // 断线:立即带根因，不再重复报一次连接失败。
    source.current.connected = false;
    source.current.last_failure = Fail("read_closed", channel::qq::kStageConnected);
    source.current.next_retry_at_ms = now + 2'000;
    reporter.Observe("boot-1", 42, now += 100);
    lines = captured.Take();
    REQUIRE_FALSE(lines.empty());
    bool saw_disconnect = false;
    for (const std::string& line : lines) {
        if (line.find("连接断开") != std::string::npos) {
            saw_disconnect = true;
            CHECK(line.find("read_closed") != std::string::npos);
        }
    }
    CHECK(saw_disconnect);
    CHECK(std::none_of(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("连接失败:") != std::string::npos;
    }));

    // 停止:立即。
    source.current.stage = channel::qq::kStageStopped;
    source.current.next_retry_at_ms = 0;
    reporter.Observe("boot-1", 42, now += 100);
    lines = captured.Take();
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().find("已停止") != std::string::npos);
}

TEST_CASE("reporter: 同码重复失败限频合并;原因变化立即显示") {
    SnapshotSource source;
    Captured captured;
    captured.Capture();
    const std::filesystem::path root = MakeTempRoot("ratelimit");
    ChannelConnectionReporter::Deps deps;
    deps.channels_state_root = root;
    deps.emit = captured.emit;
    deps.accounts.push_back(source.AccountOf());
    ChannelConnectionReporter reporter(std::move(deps));

    std::int64_t now = 10'000;
    source.current.thread_alive = true;
    source.current.stage = channel::qq::kStageConnecting;
    source.current.last_failure = Fail("connect_refused");
    reporter.Observe("boot-1", 42, now);
    CHECK(captured.Take().size() == 2);  // 开场 + 首条失败

    // 同码在 30 秒窗内重复 5 次:全部静默(合并)。
    for (int i = 0; i < 5; ++i) {
        reporter.Observe("boot-1", 42, now += 1'000);
    }
    CHECK(captured.Take().empty());

    // 窗外第一条:补一条"仍在失败",不沉默。
    reporter.Observe("boot-1", 42, now += 26'000);
    {
        const auto lines = captured.Take();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find("尚未恢复连接") != std::string::npos);
        CHECK(lines[0].find("connect_refused") != std::string::npos);
    }

    // 原因变化(TLS 根问题):立即显示,不等窗。
    source.current.last_failure = Fail("tls_cert_not_trusted", channel::qq::kStageConnecting);
    reporter.Observe("boot-1", 42, now += 1'000);
    {
        const auto lines = captured.Take();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find("tls_cert_not_trusted") != std::string::npos);
    }
}

TEST_CASE("reporter: 轮询不是失败次数,QQ 重连只报一次根因") {
    SnapshotSource source;
    Captured captured;
    captured.Capture();
    ChannelConnectionReporter::Deps deps;
    deps.channels_state_root = MakeTempRoot("reconnect-polling");
    deps.emit = captured.emit;
    deps.accounts.push_back(source.AccountOf());
    ChannelConnectionReporter reporter(std::move(deps));
    source.current.connected = true;
    source.current.stage = channel::qq::kStageConnected;
    reporter.Observe("boot", 42, 1000);
    captured.Take();
    source.current.connected = false;
    source.current.stage = "backoff";
    source.current.last_failure = Fail("server_reconnect_requested", channel::qq::kStageConnected);
    source.current.next_retry_at_ms = 64000;
    reporter.Observe("boot", 42, 2000);
    auto lines = captured.Take();
    CHECK(std::count_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("server_reconnect_requested") != std::string::npos;
    }) == 1);
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("QQ 要求重新连接") != std::string::npos &&
               line.find("62 秒后重试") != std::string::npos;
    }));
    for (int i = 1; i <= 600; ++i) reporter.Observe("boot", 42, 2000 + i * 100);
    lines = captured.Take();
    REQUIRE(lines.size() == 2);
    for (const auto& line : lines) {
        CHECK(line.find("尚未恢复连接") != std::string::npos);
        CHECK(line.find("第 ") == std::string::npos);
        CHECK(line.find("次") == std::string::npos);
    }
    // 恢复时即便来源仍留着旧错误，也不能继续报失败。
    source.current.connected = true;
    source.current.stage = channel::qq::kStageConnected;
    reporter.Observe("boot", 42, 100000);
    lines = captured.Take();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("已连接 QQ,等待消息") != std::string::npos);
}

TEST_CASE("reporter: 快照发布——boot/pid/connected/updated_at;5 秒节流刷新") {
    SnapshotSource source;
    Captured captured;
    captured.Capture();
    const std::filesystem::path root = MakeTempRoot("snapshot");
    ChannelConnectionReporter::Deps deps;
    deps.channels_state_root = root;
    deps.emit = captured.emit;
    deps.accounts.push_back(source.AccountOf());
    ChannelConnectionReporter reporter(std::move(deps));

    const std::filesystem::path file =
        ChannelConnectionReporter::SnapshotFilePath(root, "qqbot", "main");
    std::int64_t now = 100'000;
    source.current.thread_alive = true;
    source.current.connected = true;
    source.current.stage = channel::qq::kStageConnected;
    source.current.connected_since_ms = now - 500;
    reporter.Observe("boot-abc", 4242, now);

    const nlohmann::json first = ReadJsonFile(file);
    REQUIRE_FALSE(first.is_discarded());
    CHECK(first.at("connected") == true);
    CHECK(first.at("boot_id") == "boot-abc");
    CHECK(first.at("pid") == 4242);
    CHECK(first.at("updated_at_ms") == now);
    CHECK(first.at("last_failure").is_null());
    CHECK(first.at("channel_id") == "qqbot");
    CHECK(first.at("account_id") == "main");

    // 内容没变:4 秒内不重写(updated_at 不动)。
    reporter.Observe("boot-abc", 4242, now += 4'000);
    CHECK(ReadJsonFile(file).at("updated_at_ms").get<std::int64_t>() == 100'000);

    // 过 5 秒:刷新 updated_at(保新鲜,CLI 过期判定有据)。
    reporter.Observe("boot-abc", 4242, now += 1'100);
    CHECK(ReadJsonFile(file).at("updated_at_ms").get<std::int64_t>() == now);

    // 失败进快照(detail 已清洗)。
    source.current.connected = false;
    source.current.last_failure = ConnectionFailure{
        "connecting", "tls_trust_store_empty", "trust store empty: no CA certificates provided",
        now};
    reporter.Observe("boot-abc", 4242, now += 6'000);
    const nlohmann::json third = ReadJsonFile(file);
    CHECK(third.at("connected") == false);
    CHECK(third.at("last_failure").at("error_code") == "tls_trust_store_empty");
}

TEST_CASE("reporter: 脱敏清洗——控制字符折叠、超长截断") {
    const std::string noisy = "line1\nline2\twith\x01\x02control and " +
                              std::string(400, 'x');
    const std::string clean = RedactConnectionDetail(noisy);
    CHECK(clean.find('\n') == std::string::npos);
    CHECK(clean.find('\t') == std::string::npos);
    CHECK(clean.find('\x01') == std::string::npos);
    CHECK(clean.size() <= 200);
}

TEST_CASE("reporter: 快照新鲜度判定——60 秒阈值") {
    CHECK_FALSE(IsConnectionSnapshotStale(100'000, 100'000 - 59'000));
    CHECK(IsConnectionSnapshotStale(100'000, 100'000 - 61'000));
    CHECK(IsConnectionSnapshotStale(100'000, 0));  // 没盖戳 = 过期
}

}  // namespace lubancode::app
