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
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"
#include "channel/ingress_store.hpp"
#include "cli/theme.hpp"

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

// ---- 四步状态(QQBot Windows 修复单 §5.1 末条,Q1b;P1 修订) ---------------
// 配置已存 / QQ 在线 / 身份已配对 / 模型配置齐全——如实分栏,哪步卡住指
// 哪步。输入是各探针的只读结果(纯逻辑,测试直接喂);探针本身在
// RunChannelStatusCommand 里做(全局配置/连接快照/pairing 账/模型配置)。
// P1(静默失败单):第四步只报"配置齐全",另报最近真实执行结局(账号
// ingress 账 + dead-letter 旁路账)——配置齐全不能冒充"模型能回复"。
struct ChannelFourStateInput {
    // 第一步:全局配置里这只账号已存(在册且启用、AppID 与凭据来源都配了)。
    bool account_configured = false;
    std::string config_detail;  // 未配时的人话(指引;可空)
    // 第二步:连接快照裁决(在线 = connected 且进程活且新鲜)。
    bool online = false;
    std::string online_detail;  // 未在线时的摘要(阶段/最近失败;可空)
    // 第三步:配对账只读投影(已批准/待批准计数)。账号级计数,不冒充
    // "当前来信身份已获批"——单条来信放不放行看准入策略与配对范围。
    bool pairing_present = false;
    bool pairing_parse_ok = true;
    std::size_t pairing_approved = 0;
    std::size_t pairing_pending = 0;
    // 第四步:模型配置态(沿 #85 assistant config/status 的 configured 面:
    // config::RequireConfigured 过没过,不重造判据)。
    bool model_configured = false;
    std::string model_detail;  // 未配齐时缺什么(可空)
    // 最近真实执行结局(账号 ingress 账只读投影;没跑过 = present=false,
    // 不拿配置冒充调用成功)。
    bool recent_turn_present = false;
    bool recent_turn_ok = false;
    std::string recent_turn_detail;   // 失败时的稳定原因(截断;可空)
    std::int64_t recent_turn_at_ms = 0;
    std::int64_t recent_turn_sid = 0;
};
struct ChannelFourStateView {
    std::vector<std::string> lines;  // 状态行(四步 + 最近执行结局行)
    nlohmann::json report;           // --json 用的 four_state 对象
};
ChannelFourStateView BuildChannelFourState(const std::string& channel_id,
                                           const std::string& account_id,
                                           const ChannelFourStateInput& input);

// 从最近来信链取"最近真实执行结局"(P1:CLI channel status 与助理页面
// 两副面孔共用同一份判据——配置齐全不冒充能回复)。链 sid 降序,第一枚
// "执行过"的来信(绑过场或死信/已回)即最近结局。
void DeriveRecentTurnOutcome(const channel::ChannelIngressRecentChain& chain,
                             ChannelFourStateInput* input);

// ---- 最近来信链(P1:来信→准入→执行→投递,不手翻 JSONL) ------------------
// 输入是三本账的只读投影(ingress 链/outbox 段/work 绑定),纯逻辑拼行;
// 探针在 RunChannelStatusCommand 里做。
struct ChannelRecentChainInput {
    bool ledger_present = false;
    std::vector<channel::ChannelIngressRecentEntry> ingress;  // sid 降序
    // 投递段(outbox 只读投影里按 source_ref 前缀筛出的)。
    struct Delivery {
        std::int64_t sid = 0;
        bool is_failure_notice = false;  // source_ref 前缀 turnfail:(失败提示)
        std::string state;               // pending|sending|sent|delivered|
                                         // delivery_unknown|failed|flagged
        std::string delivery_error;      // failed/unknown 时的稳定码
        std::uint32_t ordinal = 0;
    };
    std::vector<Delivery> deliveries;
    // 执行绑定(work ledger 只读投影:sid -> V3 场 id)。
    std::map<std::int64_t, std::string> bound_sessions;
};
struct ChannelRecentChainView {
    std::vector<std::string> lines;
    nlohmann::json report;  // --json 用的 recent_chain 数组
};
ChannelRecentChainView BuildChannelRecentChain(const ChannelRecentChainInput& input,
                                               std::size_t limit);

// ---- 配置探针(W3 起与助理页面共用;判据单一真源) --------------------------
// 读配置文件的 channels 段并解析(只读;文件不在/读不懂/channels 段坏都
// 如实落 detail,不冒充空配置)。
struct ChannelsConfigProbe {
    bool ok = false;                    // channels 段读到了且解析过了
    std::string detail;                 // 失败原因(人话;可空)
    std::map<std::string, channel::ChannelUserConfig> channels;  // ok 时有效
};
ChannelsConfigProbe LoadChannelsUserConfigFromFile(const std::filesystem::path& config_path);

// 账号配置判据(纯逻辑):在册且启用、AppID 与凭据来源都配了才算
// configured。detail 给"卡在哪"。与 RunChannelStatusCommand 第一步同尺
// ——助理页面(W3)吃同一份,不开第二份判据。
struct ChannelAccountConfigProbe {
    bool configured = false;
    std::string detail;
};
ChannelAccountConfigProbe ProbeChannelAccountConfig(
    const std::map<std::string, channel::ChannelUserConfig>& channels,
    const std::string& channel_id, const std::string& account_id);

// 命令入口(cli_app 调):解析参数 → 定位快照 → 裁决 → 打印 → 退出码。
int RunChannelStatusCommand(const ChannelStatusCommandArgs& args);

// TUI 排版批 7:人看输出的 frame 渲染(纯函数,形状册直调)。三节 lines
// 的文本由上面三个纯构造器定(旧册与 Web 面共用的数据面,一字不动),
// 这里只管排版:四步/连接明细逐句按冒号拆列进键值对框,来信链首行(尾
// 冒号剥掉)做列表标题、日志样行整行进列表。
std::vector<std::string> RenderChannelStatusView(
    const std::string& channel_id, const std::string& account_id,
    const std::vector<std::string>& four_state_lines,
    const std::vector<std::string>& verdict_lines, const std::vector<std::string>& chain_lines,
    const Theme& theme, int width);

}  // namespace lubancode::cli
