// QQ 接入单 Q7 配置册:菜单/面板/命令绑定段的严格解析与校验。
// 官方形状的事实源:autogen v2_menu.put / v2_panels.post / v2_panels.get
// (items ≤10、子菜单 ≤5 不嵌套、面板元素 ≤20、名称按平台字符口径——
// 一个中文汉字算 2 字符、link 须 https://)。老配置不写 menu/commands
// 零行为变化。
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"

using namespace lubancode::channel;

namespace {

std::optional<std::map<std::string, ChannelUserConfig>> Parse(const std::string& json_text,
                                                              std::string* error = nullptr) {
    const nlohmann::json json = nlohmann::json::parse(json_text);
    return ParseChannelsUserConfig(json, "config.json", error);
}

const char* kFullMenuSample = R"({
  "qqbot": {
    "enabled": true,
    "default_account": "main",
    "accounts": {
      "main": {
        "enabled": true,
        "transport": "websocket",
        "app_id": "123456",
        "secret_env": "QQBOT_SECRET",
        "menu": {
          "publish": true,
          "items": [
            {"name": "帮助", "type": "send_message", "send_message": "/帮助"},
            {"name": "文件", "type": "send_message", "send_message": "/文件说明"},
            {"name": "任务", "type": "send_message", "send_message": "/查看任务"},
            {"name": "官网", "type": "link", "link": "https://example.com"},
            {
              "name": "更多", "type": "menu",
              "sub_menu_items": [
                {"name": "设置", "type": "send_message", "send_message": "/设置"},
                {"name": "文档", "type": "link", "link": "https://docs.example.com"}
              ]
            },
            {"name": "搜索偏好", "type": "switch", "switch_id": "search", "switch_default": true}
          ],
          "panel": {
            "enabled": true,
            "scope": "c2c",
            "target_type": "all",
            "remark": "主面板",
            "items": [
              {"name": "查看任务", "desc": "查看我的定时任务", "type": "command"},
              {"name": "更多服务", "desc": "打开官网", "type": "link",
               "only_admin": false, "link": "https://example.com"}
            ]
          }
        },
        "commands": [
          {"match": "/帮助", "action": "help"},
          {"match": "/文件说明", "action": "file_help"},
          {"match": "/查看任务", "action": "list_reminders"},
          {"match": "/整理", "action": "prompt",
           "prompt": "请整理我当前的待办并给出摘要。",
           "require_tools": ["read_file", "list_reminders"]}
        ]
      }
    }
  }
})";

}  // namespace

TEST_CASE("Q7 菜单/面板/命令绑定完整样例解析") {
    const auto parsed = Parse(kFullMenuSample);
    REQUIRE(parsed.has_value());
    const auto& account = parsed->at("qqbot").accounts.at("main");
    REQUIRE(account.menu.has_value());
    CHECK(account.menu->publish);
    REQUIRE(account.menu->items.size() == 6);
    CHECK(account.menu->items[0].name == "帮助");
    CHECK(account.menu->items[0].type == "send_message");
    CHECK(account.menu->items[0].send_message == "/帮助");
    CHECK(account.menu->items[3].type == "link");
    CHECK(account.menu->items[3].link == "https://example.com");
    REQUIRE(account.menu->items[4].sub_menu_items.size() == 2);
    CHECK(account.menu->items[4].sub_menu_items[0].type == "send_message");
    CHECK(account.menu->items[4].sub_menu_items[1].link == "https://docs.example.com");
    CHECK(account.menu->items[5].type == "switch");
    CHECK(account.menu->items[5].switch_id == "search");
    CHECK(account.menu->items[5].switch_default);

    REQUIRE(account.menu->panel.has_value());
    CHECK(account.menu->panel->enabled);
    CHECK(account.menu->panel->scope == "c2c");
    CHECK(account.menu->panel->target_type == "all");
    CHECK(account.menu->panel->remark == "主面板");
    REQUIRE(account.menu->panel->items.size() == 2);
    CHECK(account.menu->panel->items[0].type == "command");
    CHECK(account.menu->panel->items[1].type == "link");
    CHECK(account.menu->panel->items[1].only_admin == false);

    REQUIRE(account.commands.size() == 4);
    CHECK(account.commands[0].match == "/帮助");
    CHECK(account.commands[0].action == "help");
    CHECK(account.commands[2].action == "list_reminders");
    CHECK(account.commands[3].action == "prompt");
    CHECK(account.commands[3].prompt.find("待办") != std::string::npos);
    REQUIRE(account.commands[3].require_tools.size() == 2);
    CHECK(account.commands[3].require_tools[0] == "read_file");
}

TEST_CASE("老配置不写 menu/commands 零行为变化") {
    const auto parsed = Parse(R"({"qqbot": {"accounts": {"main": {"enabled": true}}}})");
    REQUIRE(parsed.has_value());
    const auto& account = parsed->at("qqbot").accounts.at("main");
    CHECK_FALSE(account.menu.has_value());
    CHECK(account.commands.empty());
}

TEST_CASE("菜单校验:数量/长度/类型/链接形状") {
    std::string error;
    // items 超 10。
    std::string eleven = R"({"qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [)";
    for (int i = 0; i < 11; ++i) {
        eleven += R"({"name": "n)" + std::to_string(i) + R"(", "type": "send_message", "send_message": "/c)";
        eleven += std::to_string(i) + R"("})";
        if (i != 10) eleven += ",";
    }
    eleven += R"(]}}}}})";
    CHECK_FALSE(Parse(eleven, &error).has_value());
    CHECK(error.find("最多 10") != std::string::npos);

    // 名称超平台字符上限("帮助"=4 字符;六个汉字=12 > 10)。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [
        {"name": "一二三四五六", "type": "send_message", "send_message": "/x"}
      ]}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("超长") != std::string::npos);

    // link 不带 https://。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [
        {"name": "官网", "type": "link", "link": "http://example.com"}
      ]}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("https://") != std::string::npos);

    // 子菜单超 5。
    std::string six_subs = R"({"qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [
      {"name": "更多", "type": "menu", "sub_menu_items": [)";
    for (int i = 0; i < 6; ++i) {
        six_subs += R"({"name": "s)" + std::to_string(i) + R"(", "type": "send_message", "send_message": "/s)";
        six_subs += std::to_string(i) + R"("})";
        if (i != 5) six_subs += ",";
    }
    six_subs += R"(]}]}}}}})";
    CHECK_FALSE(Parse(six_subs, &error).has_value());
    CHECK(error.find("最多 5") != std::string::npos);

    // 子菜单不许再嵌套 menu 型。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [
        {"name": "更多", "type": "menu", "sub_menu_items": [
          {"name": "再嵌套", "type": "menu"}
        ]}
      ]}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("send_message/link") != std::string::npos);

    // switch 缺 switch_id。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [
        {"name": "偏好", "type": "switch"}
      ]}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("switch_id") != std::string::npos);

    // 未知字段拒绝。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"publish": true, "items": [
        {"name": "帮助", "type": "send_message", "send_message": "/h", "anchor": 1}
      ]}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("认不得的字段") != std::string::npos);

    // 启用发布但 items 空。
    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"menu": {"publish": true}}}}})",
                      &error)
                    .has_value());
    CHECK(error.find("items 为空") != std::string::npos);
}

TEST_CASE("面板校验:scope 只认 c2c、元素上限、desc 上限") {
    std::string error;
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"panel": {"enabled": true, "scope": "group",
        "items": [{"name": "群签到", "type": "command"}]}}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("只认 c2c") != std::string::npos);

    // desc 超 30 平台字符(16 个汉字 = 32 > 30)。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"panel": {"enabled": true, "scope": "c2c",
        "items": [{"name": "查询", "desc": "一二三四五六七八九十一二三四五六", "type": "command"}]}}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("超长") != std::string::npos);

    // target_type 只认 all/specific。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"panel": {"enabled": true, "scope": "c2c",
        "target_type": "everyone", "items": [{"name": "查询", "type": "command"}]}}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("all/specific") != std::string::npos);

    // 面板元素 type 只认 command/link。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"menu": {"panel": {"enabled": true, "scope": "c2c",
        "items": [{"name": "查询", "type": "switch"}]}}}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("command/link") != std::string::npos);
}

TEST_CASE("命令绑定校验:action 枚举/prompt 配对/重复 match 拒") {
    std::string error;
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"commands": [
        {"match": "/x", "action": "explode"}
      ]}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("action 只认") != std::string::npos);

    // action=prompt 缺 prompt。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"commands": [
        {"match": "/x", "action": "prompt"}
      ]}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("prompt 缺失") != std::string::npos);

    // 非 prompt 动作挂 require_tools 拒(控制命令不经模型,无工具面)。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"commands": [
        {"match": "/x", "action": "help", "require_tools": ["read_file"]}
      ]}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("require_tools 只在 action=prompt") != std::string::npos);

    // 重复 match 拒(不按文件次序碰运气)。
    CHECK_FALSE(Parse(R"({
      "qqbot": {"accounts": {"main": {"commands": [
        {"match": "/帮助", "action": "help"},
        {"match": "/帮助", "action": "file_help"}
      ]}}}
    })",
                      &error)
                    .has_value());
    CHECK(error.find("重复的 match") != std::string::npos);
}

TEST_CASE("平台字符计数:汉字 2、ASCII 1") {
    CHECK(CountPlatformChars("") == 0);
    CHECK(CountPlatformChars("help") == 4);
    CHECK(CountPlatformChars("帮助") == 4);
    CHECK(CountPlatformChars("查看任务") == 8);
    CHECK(CountPlatformChars("/帮助") == 5);
}
