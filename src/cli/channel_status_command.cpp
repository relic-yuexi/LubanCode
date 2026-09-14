// channel status 子命令实现:合同见 hpp 注释。
#include "cli/channel_status_command.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>

#include "app/channel_connection_reporter.hpp"
#include "channel/channel_config.hpp"
#include "channel/manager.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::cli {

namespace {

// 快照字段的安全读取(json 缺键一律 contains(),const operator[] 查缺键
// 是 UB——纪律第 6 条)。
std::string SnapshotString(const nlohmann::json& snapshot, const char* key) {
    return snapshot.contains(key) && snapshot.at(key).is_string()
               ? snapshot.at(key).get<std::string>()
               : std::string();
}

bool SnapshotBool(const nlohmann::json& snapshot, const char* key) {
    return snapshot.contains(key) && snapshot.at(key).is_boolean() &&
           snapshot.at(key).get<bool>();
}

std::int64_t SnapshotInt(const nlohmann::json& snapshot, const char* key) {
    return snapshot.contains(key) && snapshot.at(key).is_number_integer()
               ? snapshot.at(key).get<std::int64_t>()
               : 0;
}

unsigned long SnapshotPid(const nlohmann::json& snapshot) {
    const std::int64_t pid = SnapshotInt(snapshot, "pid");
    return pid > 0 ? static_cast<unsigned long>(pid) : 0;
}

}  // namespace

ChannelStatusVerdict JudgeChannelStatus(const nlohmann::json* snapshot,
                                        const std::string& channel_id,
                                        const std::string& account_id,
                                        std::int64_t now_ms,
                                        const std::function<bool(unsigned long)>& is_alive) {
    ChannelStatusVerdict verdict;
    const std::string who = channel_id + "/" + account_id;
    if (snapshot == nullptr || !snapshot->is_object()) {
        verdict.lines.push_back(who + ": 没有连接状态快照(Gateway 未运行,或该账号未装配)");
        verdict.report["verdict"] = "no_snapshot";
        return verdict;
    }
    const nlohmann::json& body = *snapshot;
    const unsigned long pid = SnapshotPid(body);
    const std::int64_t updated_at_ms = SnapshotInt(body, "updated_at_ms");
    const bool connected = SnapshotBool(body, "connected");

    // 裁决序:进程存活 > 快照新鲜 > connected。每一层失败都如实说,
    // 不拿下一层的结果粉饰。
    if (pid == 0 || (is_alive && !is_alive(pid))) {
        verdict.lines.push_back(who + ": 不在线——发布快照的 Gateway 进程已退出"
                                      "(pid " + std::to_string(pid) + ",旧快照不当作在线)");
        verdict.report["verdict"] = "process_dead";
        verdict.report["snapshot"] = body;
        return verdict;
    }
    if (app::IsConnectionSnapshotStale(now_ms, updated_at_ms)) {
        verdict.lines.push_back(
            who + ": 在线状态未知——快照过期(距上次更新 " +
            std::to_string((now_ms - updated_at_ms + 999) / 1000) +
            " 秒,阈值 60 秒;Gateway 在跑但没在刷新,按不在线处理)");
        verdict.report["verdict"] = "stale_snapshot";
        verdict.report["snapshot"] = body;
        return verdict;
    }
    verdict.report["snapshot"] = body;
    if (!connected) {
        const std::string stage = SnapshotString(body, "stage");
        std::string failure_text;
        if (body.contains("last_failure") && body.at("last_failure").is_object()) {
            const nlohmann::json& failure = body.at("last_failure");
            failure_text = ",最近失败: " + SnapshotString(failure, "error_code") + "(" +
                           SnapshotString(failure, "stage") + ")";
            const std::string detail = SnapshotString(failure, "detail");
            if (!detail.empty()) {
                failure_text += " " + detail;
            }
        }
        const std::int64_t retry_at = SnapshotInt(body, "next_retry_at_ms");
        const std::string retry_text =
            retry_at > 0
                ? ";" + std::to_string((retry_at - now_ms + 999) / 1000) + " 秒后重试"
                : std::string();
        verdict.lines.push_back(who + ": 未连接(阶段 " + stage + failure_text + ")" +
                                retry_text);
        verdict.report["verdict"] = "not_connected";
        return verdict;
    }
    verdict.online = true;
    verdict.exit_code = 0;
    verdict.lines.push_back(who + ": 已连接 QQ(boot " + SnapshotString(body, "boot_id") +
                                  ",pid " + std::to_string(pid) + ")");
    verdict.report["verdict"] = "connected";
    return verdict;
}

int RunChannelStatusCommand(const ChannelStatusCommandArgs& args) {
    // 渠道/账号 id 先过守门:它们直接拼状态根下的路径,带路径段 = 越界。
    if (!channel::IsValidChannelId(args.channel_id)) {
        std::fprintf(stderr, "channel status: 渠道 id 不合法(非空、无路径段、无控制字符): %s\n",
                     args.channel_id.c_str());
        return 1;
    }
    if (!channel::IsValidChannelAccountId(args.account_id)) {
        std::fprintf(stderr, "channel status: 账号 id 不合法(非空、无路径段、无控制字符): %s\n",
                     args.account_id.c_str());
        return 1;
    }
    const std::filesystem::path channels_root = channel::DefaultChannelsStateRoot();
    if (channels_root.empty()) {
        std::fprintf(stderr,
                     "channel status: 渠道状态根不可用(应用根变量坏或找不到主目录)\n");
        return 1;
    }
    const std::filesystem::path file =
        app::ChannelConnectionReporter::SnapshotFilePath(channels_root, args.channel_id,
                                                         args.account_id);
    std::error_code ec;
    std::optional<nlohmann::json> snapshot;
    if (std::filesystem::is_regular_file(file, ec) && !ec) {
        std::ifstream stream(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
        const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded()) {
            snapshot = parsed;
        }
    }
    const ChannelStatusVerdict verdict = JudgeChannelStatus(
        snapshot.has_value() ? &*snapshot : nullptr, args.channel_id, args.account_id,
        platform::WallClockNowMs(),
        [](unsigned long pid) { return platform::IsProcessAlive(pid); });
    if (args.json) {
        std::printf("%s\n", verdict.report.dump().c_str());
    } else {
        for (const std::string& line : verdict.lines) {
            std::printf("%s\n", line.c_str());
        }
    }
    return verdict.exit_code;
}

}  // namespace lubancode::cli
