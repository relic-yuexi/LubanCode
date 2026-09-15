// 菜单/面板回调的宿主侧命令识别与回正文构建(QQ 接入单 Q7 §十三)。
//
// 回调管线的事实依据(官方 menu/panels 文档,单 §十三 13.x 核读):新式
// 自定义菜单的 send_message 与指令面板的 command 点击后只是把文本填入
// 聊天输入框——用户发送后才成为聊天指令,照常走 C2C 入站链(ingress 账/
// 准入/路由/五层工具闸一条不少)。旧式快捷菜单的 type=12 feature_id 互动
// 与消息内审批按钮的 type=11 button_data 是另外两族解码,分别归后续批/
// Q6;本件只管"文本进来 → 命令表识别"这一段。
//
// 分派规矩(§十三):
//   - 识别 = 去首尾空白后与命令表 match 整串等值;用户改过、带参数的不
//     命中,照常进模型,不猜。
//   - 控制命令(help/file_help/list_reminders)由宿主直接答,零模型,
//     不扩权:list_reminders 走 Q5 任务桥的归属闸,只看自己的任务。
//   - 预设输入(prompt)换正文照常进模型;require_tools 逐名过本轮冻结
//     策略(五层交集),名单外拒——绑什么工具就得什么权限。
//   - 菜单正文零私有信息:不列私有路径、任务名、凭据;帮助正文只由命令
//     表本身生成。
//
// 纯函数件:零 IO、零线程。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"

namespace lubancode::channel {

// 命令识别:命中返回命令绑定;不命中 nullopt(调用方照常走模型路)。
std::optional<ChannelCommandBindingUserConfig> MatchChannelCommand(
    const std::vector<ChannelCommandBindingUserConfig>& commands, const std::string& text);

// help 正文:固定引导行 + 命令表逐项(match + prompt 型带一句说明)。
// 只含配置里明写的用户可见文本,零私有信息。
std::string MakeChannelHelpText(const std::vector<ChannelCommandBindingUserConfig>& commands);

// 文件说明正文:引导从 QQ 原生附件入口发送(官方菜单接口没有通用文件
// 选择器,不虚报"可选任意 PDF/ZIP")。
std::string MakeChannelFileHelpText();

// list_reminders 回执(ChannelAutomationBridge::ListReminders 的 payload,
// 只含归属自己的任务)→ 用户正文;nextFireMs 折成 UTC 可读时刻。空表
// 给"没有定时任务"。
std::string FormatReminderListText(const nlohmann::json& payload, std::int64_t now_ms);

// 权限外菜单项的稳定拒绝正文(missing_tools = 冻结策略名单外的工具名)。
std::string MakeMenuCommandDeniedText(const std::vector<std::string>& missing_tools);

// 域外稳定说明(automation 域不在/桥未装配,list_reminders 暂不可用)。
std::string MakeMenuCommandUnavailableText();

// epoch 毫秒 → "YYYY-MM-DD HH:MM UTC"(用户正文用;不做时区换算——
// 任务时区在 job 账上,正文里已带)。
std::string FormatUtcTimestamp(std::int64_t epoch_ms);

}  // namespace lubancode::channel
