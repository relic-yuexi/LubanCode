// 常驻助理 Web 主界面单 W3:渠道面(只读投影 + 配对转发)与系统托管
// 只读投影。
//
//   - AssistantChannelFace:渠道设置页的后端面。读侧是既有服务的只读
//     投影——channel status 四态(#90 BuildChannelFourState)、连接快照
//     (#81 ChannelConnectionReporter 的跨进程快照)、pairing 账(#90 的
//     ReadProjection/ReadPendingList);写侧只有配对批准/拒绝,经 gateway
//     的 pairing 控制命令合同(#90 GatewayPairingCommand)转发到同一控制
//     面——与 CLI `channel pairing approve|reject` 同一条路,不另开第二
//     份批准口。
//   - 安全边界(单 §五):渠道凭据(AppSecret/secret)不在网页录入——
//     secret 不过浏览器。页面只读状态;配置走 `lubancode im` /
//     `lubancode channel setup`(指引文案如实给)。
//   - 系统托管(V4,W4 接入):只读投影——gateway 服务安装记录
//     (install.json)与 gateway 实例活态(锁文件探活)。安装/卸载的
//     执行走 CLI(`lubancode gateway service install|uninstall`),页面
//     不代跑安装;注册状态的对账走 `lubancode gateway doctor`。没装的
//     不画假开关。
//
// 线程模型:方法面在读线程(Server 的连接服务线程),全部文件 IO 短小
// 有界;写侧(pairing respond)轮询回执至多 timeout_ms(缺省 10 秒),
// 与 CLI 同尺。测试经 Options 的 seam 注入(now/探活/配置文本)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"
#include "gateway/profile.hpp"

namespace lubancode::app_server {
class Dispatcher;
}

namespace lubancode::app {

class AssistantChannelFace {
public:
    struct Options {
        // 渠道状态根(<状态根>/channels;连接快照与 pairing 账的树)。
        std::filesystem::path channels_state_root;
        // gateway profile 树(锁文件/控制目录:pairing 命令的投递面;
        // 服务安装记录 service/install.json)。
        gateway::GatewayProfilePaths gateway_paths;
        // 全局配置文件路径(生产:config::GlobalConfigFilePath)。测试注入。
        std::optional<std::filesystem::path> config_path;
        // 模型配置判据(生产:config::LoadFromEnv + RequireConfigured;
        // 四态的第四步)。空 = 该步如实报"未探"(测试可省)。
        std::function<std::pair<bool, std::string>()> model_probe;
        // 进程探活(连接快照裁决用)。生产 platform::IsProcessAlive。
        std::function<bool(unsigned long)> is_alive;
        std::function<std::int64_t()> now_ms;
        // pairing 回执等待上限(缺省 10s,与 CLI 同尺)。
        std::int64_t pairing_timeout_ms = 10'000;
    };

    explicit AssistantChannelFace(Options options);

    // ---- 方法实现(handler 的内脏;错误走 out_error_*,返回 result) ----

    // channel/list:{}。全局 channels 段的全部账号,每只一份四态投影 +
    // 待批准清单。空配置/配置读不懂如实回(channelsError 字段)。
    nlohmann::json HandleChannelList(const nlohmann::json& params, int& out_error_code,
                                     std::string& out_error_message);
    // channel/status:{channelId, accountId}。单账号细图:四态 + 连接明细
    // (verdict lines + 快照脱敏投影)。
    nlohmann::json HandleChannelStatus(const nlohmann::json& params, int& out_error_code,
                                       std::string& out_error_message);
    // channel/pairing/respond:{channelId, accountId, token,
    // action:"approve"|"reject"}。转发到 gateway pairing 控制命令面
    // (锁探测 → 写命令 → 等回执);无持锁 gateway = 稳定错误。
    nlohmann::json HandleChannelPairingRespond(const nlohmann::json& params, int& out_error_code,
                                               std::string& out_error_message);
    // gateway/service/status:{}。V4 服务安装的只读投影:install.json 在
    // 不在(装没装)、版本/exe 记录、gateway 实例活态。零副作用。
    nlohmann::json HandleServiceStatus(const nlohmann::json& params, int& out_error_code,
                                       std::string& out_error_message);

    // ---- 纯投影(单测钉形状;生产面共用) ----

    // 一只账号的四态 + pending 清单(读盘上三份事实:配置/快照/pairing)。
    // channel_config = 该账号所在渠道的配置段(空 = 不在册,四态卡第一
    // 步如实报);include_detail 带连接明细行与脱敏快照(单账号细图用)。
    nlohmann::json BuildAccountProjection(const std::string& channel_id,
                                          const std::string& account_id,
                                          const channel::ChannelUserConfig* channel_config,
                                          bool include_detail) const;

private:
    Options options_;
};

// 方法注册(assistant_host 的 extra_method_registrar 与单测直驱共用)。
// face 以 shared_ptr 持有(每条连接的 dispatcher 都捕一份)。
void RegisterAssistantChannelMethods(app_server::Dispatcher& dispatcher,
                                      const std::shared_ptr<AssistantChannelFace>& face);

}  // namespace lubancode::app
