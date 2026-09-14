// 渠道连接状态宿主输出件:实现见 hpp 合同注释。
#include "app/channel_connection_reporter.hpp"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "platform/atomic_write.hpp"

namespace lubancode::app {

namespace {

constexpr std::size_t kDetailCap = 200;  // 清洗后说明的长度帽

// 阶段 -> 人话(单内目标输出示例的口径)。
std::string StageText(const std::string& stage) {
    using namespace channel::qq;
    if (stage == kStageFetchingToken) {
        return "正在获取访问令牌";
    }
    if (stage == kStageFetchingGatewayUrl) {
        return "正在查询 QQ 网关地址";
    }
    if (stage == kStageConnecting) {
        return "正在连接 QQ";
    }
    if (stage == kStageIdentifying) {
        return "正在鉴权(Identify/Resume)";
    }
    if (stage == kStageConnected) {
        return "已连接 QQ";
    }
    if (stage == kStageStopped) {
        return "已停止";
    }
    return "连接中";
}

}  // namespace

std::string RedactConnectionDetail(const std::string& detail) {
    std::string out;
    out.reserve(std::min(detail.size(), kDetailCap));
    for (char c : detail) {
        // 控制字符与换行折叠成空格:日志一行一条,平台报文不透形。
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            c = ' ';
        }
        if (out.size() >= kDetailCap) {
            break;
        }
        out.push_back(c);
    }
    return out;
}

bool IsConnectionSnapshotStale(std::int64_t now_ms, std::int64_t updated_at_ms) {
    return updated_at_ms <= 0 || now_ms - updated_at_ms > kConnectionSnapshotStaleMs;
}

ChannelConnectionReporter::ChannelConnectionReporter(Deps deps) {
    states_.resize(deps.accounts.size());
    deps_ = std::move(deps);
}

std::filesystem::path ChannelConnectionReporter::SnapshotFilePath(
    const std::filesystem::path& channels_root, const std::string& channel_id,
    const std::string& account_id) {
    return channels_root / channel_id / account_id / "connection-status.json";
}

void ChannelConnectionReporter::Observe(const std::string& boot_id, unsigned long pid,
                                        std::int64_t now_ms) {
    for (std::size_t i = 0; i < deps_.accounts.size(); ++i) {
        ObserveAccount(deps_.accounts[i], states_[i], boot_id, pid, now_ms);
    }
}

void ChannelConnectionReporter::ObserveAccount(const Account& account,
                                               PerAccountState& state,
                                               const std::string& boot_id, unsigned long pid,
                                               std::int64_t now_ms) {
    const channel::qq::ConnectionSnapshot snapshot = account.snapshot
                                                         ? account.snapshot()
                                                         : channel::qq::ConnectionSnapshot{};
    const std::string prefix = "[" + account.channel_id + "/" + account.account_id + "] ";
    const auto emit_line = [&](const std::string& text) {
        if (deps_.emit) {
            deps_.emit(prefix + text);
        }
    };

    if (!state.introduced) {
        state.introduced = true;
        // 开场:启动日志报每只装配账号(失败账号此后首次失败立即打,
        // skipped 清单由装配层负责)。
        emit_line("渠道账号已装配,开始连接");
        state.last_stage = snapshot.stage;
    }

    // 在线/断线/停止边沿:立即打(§三)。
    if (snapshot.connected && !state.was_connected) {
        emit_line("已连接 QQ,等待消息");
    } else if (!snapshot.connected && state.was_connected) {
        const std::string reason =
            snapshot.last_failure.has_value()
                ? ":" + RedactConnectionDetail(snapshot.last_failure->detail) + "(" +
                      snapshot.last_failure->error_code + ")"
                : std::string();
        const std::string retry = snapshot.next_retry_at_ms > 0
                                      ? ";" + std::to_string(
                                                   (snapshot.next_retry_at_ms - now_ms + 999) /
                                                   1000) +
                                            " 秒后重试"
                                      : std::string();
        emit_line("连接断开" + reason + retry);
    }
    state.was_connected = snapshot.connected;

    if (snapshot.stage == channel::qq::kStageStopped) {
        if (!state.was_stopped) {
            emit_line("已停止");
        }
        state.was_stopped = true;
    } else {
        state.was_stopped = false;
    }

    // 失败:原因变化立即显示;同码重复合并限频(30 秒窗,超窗带累计数)。
    if (snapshot.last_failure.has_value()) {
        const auto& failure = *snapshot.last_failure;
        const bool code_changed = failure.error_code != state.last_failure_code;
        const bool outside_window =
            now_ms - state.last_failure_emit_ms >= kConnectionFailureRepeatWindowMs;
        if (code_changed) {
            const std::string retry = snapshot.next_retry_at_ms > 0
                                          ? ";" + std::to_string(
                                                       (snapshot.next_retry_at_ms - now_ms +
                                                        999) /
                                                       1000) +
                                                " 秒后重试"
                                          : std::string();
            emit_line("连接失败:" + RedactConnectionDetail(failure.detail) + "(" +
                      failure.error_code + ",阶段 " + failure.stage + ")" + retry);
            state.last_failure_code = failure.error_code;
            state.last_failure_emit_ms = now_ms;
            state.suppressed_repeats = 0;
        } else if (outside_window) {
            // 仍失败,但距上条已隔一个窗:带累计数补一条,不静默(§三
            // "不能一直沉默")。
            emit_line("仍在连接失败(" + failure.error_code + ",第 " +
                      std::to_string(1 + state.suppressed_repeats) + " 次)");
            state.last_failure_emit_ms = now_ms;
            state.suppressed_repeats = 0;
        } else {
            ++state.suppressed_repeats;
        }
    } else {
        state.last_failure_code.clear();
        state.suppressed_repeats = 0;
    }

    // 阶段推进:同阶段不重复打(重试轮次里反复 fetching/connecting 只在
    // 变化时说话)。
    if (snapshot.stage != state.last_stage && !snapshot.stage.empty()) {
        if (!snapshot.connected) {
            emit_line(StageText(snapshot.stage));
        }
        state.last_stage = snapshot.stage;
    }

    WriteSnapshot(account, snapshot, state, boot_id, pid, now_ms);
}

void ChannelConnectionReporter::WriteSnapshot(
    const Account& account, const channel::qq::ConnectionSnapshot& snapshot,
    PerAccountState& state, const std::string& boot_id, unsigned long pid,
    std::int64_t now_ms) {
    nlohmann::json body = nlohmann::json::object();
    body["schema"] = 1;
    body["channel_id"] = account.channel_id;
    body["account_id"] = account.account_id;
    body["connected"] = snapshot.connected;
    body["thread_alive"] = snapshot.thread_alive;
    body["stage"] = snapshot.stage;
    if (snapshot.last_failure.has_value()) {
        body["last_failure"] = nlohmann::json{
            {"stage", snapshot.last_failure->stage},
            {"error_code", snapshot.last_failure->error_code},
            {"detail", RedactConnectionDetail(snapshot.last_failure->detail)},
            {"at_ms", snapshot.last_failure->at_ms}};
    } else {
        body["last_failure"] = nullptr;
    }
    body["retry_count"] = snapshot.retry_count;
    body["next_retry_at_ms"] = snapshot.next_retry_at_ms;
    body["connected_since_ms"] = snapshot.connected_since_ms;
    body["boot_id"] = boot_id;
    body["pid"] = pid;

    // 写盘节拍:内容变化立即写;没变也至少每 5 秒刷一次(CLI 靠
    // updated_at 判新鲜)。
    const std::string body_without_time = body.dump();
    body["updated_at_ms"] = now_ms;
    const bool changed = body_without_time != state.last_written_body;
    const bool refresh_due =
        now_ms - state.last_snapshot_write_ms >= kConnectionSnapshotRefreshMs;
    if (!changed && !refresh_due) {
        return;
    }
    const std::filesystem::path file =
        SnapshotFilePath(deps_.channels_state_root, account.channel_id, account.account_id);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    (void)platform::AtomicWriteFile(file, body.dump(),
                                    platform::WriteDurability::ProcessCrashDurability);
    state.last_written_body = body_without_time;
    state.last_snapshot_write_ms = now_ms;
}

}  // namespace lubancode::app
