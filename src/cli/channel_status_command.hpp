// channel status 子命令(连接状态单 §三 P0-A):跨进程只读连接快照。
//
// 读 Gateway 发布的脱敏快照(<渠道状态根>/<channel>/<account>/
// connection-status.json,由 ChannelConnectionReporter 每 tick 维护,带
// boot ID 与更新时间),校验进程存活与快照新鲜度后裁决:
//   - 进程已退 / 快照过期 → 不当在线(退 1);
//   - connected != true → 不在线(退 1,带最近失败账);
//   - connected == true → 在线(退 0)。
// 不凭 PID 宣告成功(PID 活只证明 Gateway 在,不证明 QQ 连上);快照不是
// 连接状态权威——权威在运行中的适配器,这里只是只读投影。零副作用:
// 不起 Gateway、不连接平台、不碰任何账。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::cli {

struct ChannelStatusCommandArgs {
    std::string channel_id;  // 如 qqbot
    std::string account_id;  // 如 main
    bool json = false;       // --json:stdout 吐快照 + verdict
};

// 快照裁决结果(纯逻辑,测试直接喂)。
struct ChannelStatusVerdict {
    bool online = false;         // 只有 connected=true 且进程活且快照新鲜
    int exit_code = 1;           // 命令退出码(在线 0,其余非零)
    std::vector<std::string> lines;  // 人话输出(已脱敏——快照本身脱敏)
    nlohmann::json report;       // --json 用的完整报告
};

// 裁决(纯函数):snapshot 来自盘上快照文件;alive 校验与 now 注入。
// snapshot 为 nullptr = 快照文件不在/读不懂。
ChannelStatusVerdict JudgeChannelStatus(const nlohmann::json* snapshot,
                                        const std::string& channel_id,
                                        const std::string& account_id,
                                        std::int64_t now_ms,
                                        const std::function<bool(unsigned long)>& is_alive);

// ---- 四步状态(QQBot Windows 修复单 §5.1 末条,Q1b) ------------------------
// 配置已存 / QQ 在线 / 身份已配对 / 模型能回复——如实分栏,哪步卡住指
// 哪步。输入是各探针的只读结果(纯逻辑,测试直接喂);探针本身在
// RunChannelStatusCommand 里做(全局配置/连接快照/pairing 账/模型配置)。
struct ChannelFourStateInput {
    // 第一步:全局配置里这只账号已存(在册且启用、AppID 与凭据来源都配了)。
    bool account_configured = false;
    std::string config_detail;  // 未配时的人话(指引;可空)
    // 第二步:连接快照裁决(在线 = connected 且进程活且新鲜)。
    bool online = false;
    std::string online_detail;  // 未在线时的摘要(阶段/最近失败;可空)
    // 第三步:配对账只读投影(已批准/待批准计数)。
    bool pairing_present = false;
    bool pairing_parse_ok = true;
    std::size_t pairing_approved = 0;
    std::size_t pairing_pending = 0;
    // 第四步:模型配置态(沿 #85 assistant config/status 的 configured 面:
    // config::RequireConfigured 过没过,不重造判据)。
    bool model_configured = false;
    std::string model_detail;  // 未配齐时缺什么(可空)
};
struct ChannelFourStateView {
    std::vector<std::string> lines;  // 四行,固定次序(配置→在线→配对→模型)
    nlohmann::json report;           // --json 用的 four_state 对象
};
ChannelFourStateView BuildChannelFourState(const std::string& channel_id,
                                           const std::string& account_id,
                                           const ChannelFourStateInput& input);

// 命令入口(cli_app 调):解析参数 → 定位快照 → 裁决 → 打印 → 退出码。
int RunChannelStatusCommand(const ChannelStatusCommandArgs& args);

}  // namespace lubancode::cli
