// QQ 接入单 Q7 命令分派册:菜单/面板回调的命令识别与回正文构建(纯函数)。
// 回调事实:官方新式菜单 send_message / 面板 command 点击只填入输入框,
// 用户发送后成为普通 C2C 文本——识别按整串等值;控制命令零模型直答。
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "channel/channel_commands.hpp"

using namespace lubancode::channel;

namespace {

std::vector<ChannelCommandBindingUserConfig> SampleCommands() {
    std::vector<ChannelCommandBindingUserConfig> commands;
    ChannelCommandBindingUserConfig help;
    help.match = "/帮助";
    help.action = "help";
    commands.push_back(help);
    ChannelCommandBindingUserConfig file_help;
    file_help.match = "/文件说明";
    file_help.action = "file_help";
    commands.push_back(file_help);
    ChannelCommandBindingUserConfig list;
    list.match = "/查看任务";
    list.action = "list_reminders";
    commands.push_back(list);
    ChannelCommandBindingUserConfig preset;
    preset.match = "/整理";
    preset.action = "prompt";
    preset.prompt = "请整理我当前的待办并给出摘要。";
    preset.require_tools = {"read_file"};
    commands.push_back(preset);
    return commands;
}

}  // namespace

TEST_CASE("命令识别:整串等值;空白容忍;改过/带参数不命中") {
    const auto commands = SampleCommands();
    // 整串命中。
    const auto hit = MatchChannelCommand(commands, "/查看任务");
    REQUIRE(hit.has_value());
    CHECK(hit->action == "list_reminders");
    // 前后空白容忍(平台填入/客户端换行)。
    const auto padded = MatchChannelCommand(commands, "  /整理 \r\n");
    REQUIRE(padded.has_value());
    CHECK(padded->action == "prompt");
    CHECK(padded->prompt.find("待办") != std::string::npos);
    // 带参数不命中(用户改过——照常进模型,不猜)。
    CHECK_FALSE(MatchChannelCommand(commands, "/整理 邮箱").has_value());
    CHECK_FALSE(MatchChannelCommand(commands, "/查看任务 今天").has_value());
    CHECK_FALSE(MatchChannelCommand(commands, "").has_value());
    CHECK_FALSE(MatchChannelCommand(commands, "随便聊聊").has_value());
}

TEST_CASE("帮助正文:只由命令表生成,零私有信息") {
    const auto text = MakeChannelHelpText(SampleCommands());
    CHECK(text.find("/帮助") != std::string::npos);
    CHECK(text.find("/查看任务") != std::string::npos);
    CHECK(text.find("/整理") != std::string::npos);
    CHECK(text.find("定时任务") != std::string::npos);
    // 不出现任务名/路径/凭据一类的私货(表里没有就不会有)。
    CHECK(text.find("D:\\") == std::string::npos);
    CHECK(text.find("secret") == std::string::npos);
}

TEST_CASE("文件说明:引导 QQ 原生附件入口,不虚报文件选择器") {
    const auto text = MakeChannelFileHelpText();
    CHECK(text.find("QQ") != std::string::npos);
    CHECK(text.find("文件") != std::string::npos);
}

TEST_CASE("任务清单正文:空表/有任务/下次触发可读") {
    const std::int64_t now = 1'724'700'000'000;  // 2024-08-26 附近
    // 空表。
    CHECK(FormatReminderListText(nlohmann::json{{"jobs", nlohmann::json::array()}}, now)
              .find("还没有") != std::string::npos);
    // 有任务(payload 形状 = ChannelAutomationBridge::ListReminders 的回执)。
    const nlohmann::json payload = nlohmann::json::object(
        {{"jobs", nlohmann::json::array({
                      nlohmann::json{{"jobId", "job-1"},
                                     {"state", "active"},
                                     {"kind", "cron"},
                                     {"timezone", "Asia/Shanghai"},
                                     {"prompt", "每天九点提醒我喝水"},
                                     {"nextFireMs", 1'724'701'140'000}},
                      nlohmann::json{{"jobId", "job-2"},
                                     {"state", "cancelled"},
                                     {"kind", "once"},
                                     {"prompt", "明天上午十点开会"},
                                     // nextFireMs 缺失(非 active)合法。
                                  },
                  })}});
    const std::string text = FormatReminderListText(payload, now);
    CHECK(text.find("job-1") != std::string::npos);
    CHECK(text.find("job-2") != std::string::npos);
    CHECK(text.find("进行中") != std::string::npos);
    CHECK(text.find("已取消") != std::string::npos);
    CHECK(text.find("喝水") != std::string::npos);
    CHECK(text.find("2024-") != std::string::npos);  // 下次触发折成可读时刻
    CHECK(text.find("UTC") != std::string::npos);
}

TEST_CASE("权限外拒绝与域外说明的稳定正文") {
    const auto denied = MakeMenuCommandDeniedText({"create_reminder"});
    CHECK(denied.find("授权范围") != std::string::npos);
    CHECK(denied.find("create_reminder") != std::string::npos);
    const auto unavailable = MakeMenuCommandUnavailableText();
    CHECK(unavailable.find("任务查询") != std::string::npos);
}

TEST_CASE("UTC 时刻格式化") {
    // 2024-08-26 15:30:00 UTC = 1724686200000。
    CHECK(FormatUtcTimestamp(1'724'686'200'000) == "2024-08-26 15:30 UTC");
    CHECK(FormatUtcTimestamp(0) == "1970-01-01 00:00 UTC");
}
