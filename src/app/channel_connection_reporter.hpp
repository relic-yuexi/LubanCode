// 渠道连接状态宿主输出件(连接状态单 §三 P0-A):
//
// gateway run 默认打印账号的开始连接、首次失败、在线、断线与停止;重复
// 失败合并限频(同一稳定码 30 秒窗内不刷屏,原因变化立即显示);输出来源
// 是适配器的 ConnectionSnapshot 结构化快照——本件不另养一份猜测状态。
//
// 同一只件负责发布跨进程只读快照(<渠道状态根>/<channel>/<account>/
// connection-status.json):带 boot ID 与更新时间,脱敏(零泄露:不落
// AppSecret/token/Authorization/原始响应体/带敏感参数的 URL——快照字段
// 全部来自适配器的稳定账)。CLI(lubancode channel status)读它并校验
// 存活与过期,不把旧快照当在线;快照不是连接状态权威(权威在适配器)。
//
// 线程模型:Observe 由 Gateway 主循环(wiring 的 TickOnce)单线程调;
// 快照取数经适配器的 ConnectionState()(自带锁)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "channel/connection_state.hpp"
#include "channel/qq/qq_gateway.hpp"  // kStage*(阶段稳定名)与 qq:: 快照迁移期别名(StageText/既有测试册消费)

namespace lubancode::app {

// 日志/快照共用的脱敏清洗:剥控制字符、折叠空白、限长。防御性一道——
// detail 来源已脱敏,这里再兜底(平台报文绝不原样拼日志)。
std::string RedactConnectionDetail(const std::string& detail);

// 快照新鲜度判定(CLI 与 reporter 共用):updated_at 距 now 超过
// kConnectionSnapshotStaleMs 视为过期——过期快照不当在线,只作诊断。
inline constexpr std::int64_t kConnectionSnapshotStaleMs = 60'000;
bool IsConnectionSnapshotStale(std::int64_t now_ms, std::int64_t updated_at_ms);

// 快照文件的稳定节拍:内容没变也至少每 5 秒刷一次 updated_at(保新鲜)。
inline constexpr std::int64_t kConnectionSnapshotRefreshMs = 5'000;
// 同一稳定码的重复失败打印限频窗。
inline constexpr std::int64_t kConnectionFailureRepeatWindowMs = 30'000;

class ChannelConnectionReporter {
public:
    struct Account {
        std::string channel_id;
        std::string account_id;
        // 取适配器快照(wiring 装配时包 adapter;测试注入手造快照)。
        std::function<channel::ConnectionSnapshot()> snapshot;
    };
    struct Deps {
        std::vector<Account> accounts;
        std::filesystem::path channels_state_root;
        // 输出口(生产:stderr 一行一条;测试捕获)。
        std::function<void(const std::string& line)> emit;
    };

    ChannelConnectionReporter(Deps deps);
    ~ChannelConnectionReporter() = default;

    ChannelConnectionReporter(const ChannelConnectionReporter&) = delete;
    ChannelConnectionReporter& operator=(const ChannelConnectionReporter&) = delete;

    // 每个 gateway tick 调一次。boot_id/pid 来自 Gateway 锁内身份
    // (wiring 的 owner_epoch = boot_id;pid 现取)。
    void Observe(const std::string& boot_id, unsigned long pid, std::int64_t now_ms);

    // 连接状态快照文件路径(channels_state_root/<channel>/<account>/
    // connection-status.json;CLI 读同一棵树)。
    static std::filesystem::path SnapshotFilePath(const std::filesystem::path& channels_root,
                                                  const std::string& channel_id,
                                                  const std::string& account_id);

private:
    struct PerAccountState {
        bool introduced = false;          // 首次 Observe 打过开场行
        bool was_connected = false;
        bool was_stopped = false;
        std::string last_stage;
        std::string last_failure_code;
        std::int64_t last_failure_emit_ms = 0;
        std::int64_t last_snapshot_write_ms = 0;
        std::string last_written_body;     // 除 updated_at 外的内容指纹
    };

    void ObserveAccount(const Account& account, PerAccountState& state,
                        const std::string& boot_id, unsigned long pid, std::int64_t now_ms);
    void WriteSnapshot(const Account& account, const channel::ConnectionSnapshot& snapshot,
                       PerAccountState& state, const std::string& boot_id, unsigned long pid,
                       std::int64_t now_ms);

    Deps deps_;
    // 与 accounts_ 同下标(构造时展开)。
    std::vector<PerAccountState> states_;
};

}  // namespace lubancode::app
