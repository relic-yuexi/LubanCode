// QQ 接入单 Q7 菜单/面板册:官方 v2_menu/v2_panels 载荷 fixture + 发布器
// 全流程(假 HTTP + 真 token 管理 + 状态文件)。
//
// fixture 事实源(官方 autogen 页,2026-09-16 逐字核读):
//   - PUT /v2/menu 请求示例(menu 三型:send_message/link/menu 折叠);
//   - GET /v2/menu 响应示例 {"menu":{...},"version":1};
//   - GET /v2/panels 响应示例(records/next_cursor/is_end);
//   - POST /v2/panels 响应 {"panel_id":"p_x8k2x8k2x8k2"}。
// 发布纪律:显式启用才碰;先读远端,远端被人工改过不覆盖(冲突提示);
// 面板按备注所有权前缀认领复用,不反复创建;独立 QPM 限速。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_menu.hpp"

using namespace lubancode::channel::qq;

namespace {

// 假 HTTP(按序消费;耗尽后 500)。捕获方法/URL/体/头。
struct ScriptedHttp {
    struct Call {
        std::string method;
        std::string url;
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    struct Reply {
        int status = 200;
        std::string body;
        std::string error;
    };

    mutable std::mutex mutex;
    std::vector<Call> calls;
    std::vector<Reply> replies;

    QqHttpFunc Func() {
        return [this](const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
            const std::lock_guard<std::mutex> lock(mutex);
            calls.push_back(Call{request.method, request.url, request.body, request.headers});
            if (replies.empty()) {
                return QqHttpResponse{500, R"({"code":50055002})"};
            }
            const Reply reply = replies.front();
            replies.erase(replies.begin());
            if (!reply.error.empty()) {
                return std::unexpected(reply.error);
            }
            return QqHttpResponse{reply.status, reply.body};
        };
    }

    std::size_t CountBy(const std::string& method, const std::string& url_part) const {
        const std::lock_guard<std::mutex> lock(mutex);
        std::size_t count = 0;
        for (const auto& call : calls) {
            if (call.method == method && call.url.find(url_part) != std::string::npos) {
                ++count;
            }
        }
        return count;
    }
};

// 恒定发 token 的假 HTTP(token 管理器专用,与菜单脚手架分开)。
QqHttpFunc MakeTokenHttp() {
    return [](const QqHttpRequest&) {
        return QqHttpResponse{200, R"({"access_token":"TOKEN1","expires_in":7200})"};
    };
}

struct PublisherFixture {
    std::filesystem::path root;
    ScriptedHttp http;
    // token 管理器走恒定 token 的独立 HTTP(与菜单脚手架分开,脚本只装
    // 菜单/面板调用)。
    std::unique_ptr<QqTokenManager> tokens;
    std::int64_t now = 100'000;
    QqMenuPanelPublisher::Options options;

    explicit PublisherFixture(const char* tag)
        : root(std::filesystem::temp_directory_path() /
               ("lubancode-qq-menu-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        QqTokenManager::Options token_options;
        token_options.app_id = "APP1";
        token_options.client_secret = "SECRET1";
        token_options.http = MakeTokenHttp();
        token_options.now_ms = [this] { return now; };
        token_options.token_url = "https://bots.test/app/getAppAccessToken";
        token_options.refresh_margin_secs = 300;
        tokens = std::make_unique<QqTokenManager>(std::move(token_options));
        options.http = http.Func();
        options.tokens = tokens.get();
        options.api_base = "https://api.test";
        options.state_file = root / "menu-panel.json";
        options.now_ms = [this] { return now; };
    }
};

ChannelMenuUserConfig SampleMenu() {
    ChannelMenuUserConfig menu;
    menu.publish = true;
    ChannelMenuItemUserConfig help;
    help.name = "帮助";
    help.type = "send_message";
    help.send_message = "/help";
    menu.items.push_back(help);
    ChannelMenuItemUserConfig site;
    site.name = "官网";
    site.type = "link";
    site.link = "https://example.com";
    menu.items.push_back(site);
    ChannelMenuItemUserConfig more;
    more.name = "更多";
    more.type = "menu";
    ChannelMenuSubItemUserConfig settings;
    settings.name = "设置";
    settings.type = "send_message";
    settings.send_message = "/settings";
    more.sub_menu_items.push_back(settings);
    menu.items.push_back(more);
    return menu;
}

ChannelPanelUserConfig SamplePanel(std::string target_type = "all") {
    ChannelPanelUserConfig panel;
    panel.enabled = true;
    panel.scope = "c2c";
    panel.target_type = std::move(target_type);
    panel.remark = "主面板";
    ChannelPanelItemUserConfig item;
    item.name = "查询天气";
    item.desc = "查询当前天气";
    item.type = "command";
    panel.items.push_back(item);
    return panel;
}

// 官方 GET /v2/menu 响应的期望菜单(与 SampleMenu 的 PUT 载荷同形)。
nlohmann::json SampleMenuRemote() {
    return BuildMenuPutBody(SampleMenu()).at("menu");
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
}

}  // namespace

// ---------------------------------------------------------------------------
// 载荷 fixture(官方示例逐字)
// ---------------------------------------------------------------------------

TEST_CASE("菜单 PUT 载荷:官方三型示例(send_message/link/menu 折叠)") {
    const nlohmann::json body = BuildMenuPutBody(SampleMenu());
    const nlohmann::json expected = nlohmann::json::parse(R"({
      "menu": {
        "items": [
          {"type": "send_message", "name": "帮助", "send_message": "/help"},
          {"type": "link", "name": "官网", "link": "https://example.com"},
          {"type": "menu", "name": "更多",
           "sub_menu_items": [
             {"type": "send_message", "name": "设置", "send_message": "/settings"}
           ]}
        ]
      }
    })");
    CHECK(body == expected);
}

TEST_CASE("菜单 GET 响应:官方示例;未设置过菜单时 menu 缺席") {
    std::string error;
    const auto parsed = ParseMenuGetBody(nlohmann::json::parse(R"({
      "menu": {
        "items": [
          {"type": "send_message", "name": "帮助", "send_message": "/help"}
        ]
      },
      "version": 1
    })"),
                                         &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->has_menu);
    CHECK(parsed->version == 1);
    CHECK(parsed->menu.at("items").size() == 1);

    const auto empty = ParseMenuGetBody(nlohmann::json::parse(R"({"version": 0})"), &error);
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->has_menu);
    CHECK(empty->version == 0);

    // 数值两态容忍(平台可能回字符串——真机教训)。
    const auto loose = ParseMenuGetBody(nlohmann::json::parse(R"({"version": "3"})"), &error);
    REQUIRE(loose.has_value());
    CHECK(loose->version == 3);

    CHECK_FALSE(ParseMenuGetBody(nlohmann::json::parse(R"({"menu": {"items": "x"}})"), &error)
                    .has_value());
}

TEST_CASE("面板载荷:创建/更新/目标;所有权备注前缀") {
    const std::string remark = MakePanelRemark("qqbot", "main", "主面板");
    CHECK(remark == "lubancode:qqbot:main:主面板");

    const nlohmann::json create = BuildPanelCreateBody(SamplePanel(), remark);
    CHECK(create.at("scope") == "c2c");
    CHECK(create.at("target_type") == "all");
    CHECK(create.at("panel").at("remark") == remark);
    const nlohmann::json expected_items = nlohmann::json::parse(
        R"([{"type":"command","name":"查询天气","desc":"查询当前天气"}])");
    CHECK(create.at("panel").at("items") == expected_items);

    const nlohmann::json update = BuildPanelUpdateBody(SamplePanel(), remark);
    CHECK(update.at("panel").at("items") == expected_items);
    CHECK_FALSE(update.contains("scope"));  // 更新只带 panel

    const nlohmann::json target = BuildPanelTargetBody("add", {"openid-1", "openid-2"});
    CHECK(target == nlohmann::json::parse(
                       R"({"op":"add","user_openids":["openid-1","openid-2"]})"));

    std::string error;
    const auto created = ParsePanelCreateBody(nlohmann::json::parse(R"({"panel_id":"p_x8k2x8k2x8k2"})"), &error);
    REQUIRE(created.has_value());
    CHECK(*created == "p_x8k2x8k2x8k2");
    CHECK_FALSE(ParsePanelCreateBody(nlohmann::json::parse(R"({})"), &error).has_value());

    const auto version = ParsePanelVersionBody(nlohmann::json::parse(R"({"version": 2})"), &error);
    REQUIRE(version.has_value());
    CHECK(*version == 2);
}

TEST_CASE("面板列表响应:官方示例 + 分页游标 + 数值两态") {
    std::string error;
    const auto parsed = ParsePanelsListBody(nlohmann::json::parse(R"({
      "records": [
        {
          "panel_id": "p_102030405_x8k2",
          "scope": "c2c",
          "target_type": "all",
          "panel": {
            "items": [{"type": "command", "name": "查询天气", "desc": "查询当前天气"}]
          },
          "version": 1
        }
      ],
      "next_cursor": "",
      "is_end": true
    })"),
                                             &error);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->records.size() == 1);
    CHECK(parsed->records[0].panel_id == "p_102030405_x8k2");
    CHECK(parsed->records[0].target_type == "all");
    CHECK(parsed->records[0].version == 1);
    CHECK(parsed->is_end);

    // 分页:next_cursor 非空且 is_end=false。
    const auto paged = ParsePanelsListBody(nlohmann::json::parse(
                                               R"({"records":[],"next_cursor":"c2","is_end":false})"),
                                           &error);
    REQUIRE(paged.has_value());
    CHECK_FALSE(paged->is_end);
    CHECK(paged->next_cursor == "c2");

    // version 字符串两态。
    const auto loose = ParsePanelsListBody(
        nlohmann::json::parse(
            R"({"records":[{"panel_id":"p1","version":"7"}],"next_cursor":"","is_end":true})"),
        &error);
    REQUIRE(loose.has_value());
    CHECK(loose->records[0].version == 7);
}

// ---------------------------------------------------------------------------
// 发布器流程
// ---------------------------------------------------------------------------

TEST_CASE("首次发布:读远端→PUT 菜单;面板列表空→POST 创建;状态落盘") {
    PublisherFixture fixture("first-publish");
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_menu = true;
    input.menu = SampleMenu();
    input.publish_panel = true;
    input.panel = SamplePanel();

    fixture.http.replies = {
        {200, R"({"version": 0})"},                                        // GET menu(空)
        {200, R"({"version": 3})"},                                        // PUT menu
        {200, R"({"records":[],"next_cursor":"","is_end":true})"},         // GET panels
        {200, R"({"panel_id":"p_new_1"})"},                                // POST panel
    };
    const auto report = publisher.Sync(input);
    CHECK(report.menu_published);
    CHECK(report.menu_version == 3);
    CHECK(report.panel_created);
    CHECK(report.panel_id == "p_new_1");
    CHECK(report.error_code.empty());

    // 首次覆盖提示不瞎报:远端本来就没菜单,无"覆盖"提示。
    REQUIRE(report.notices.empty());

    // PUT 载荷逐字段对 fixture。
    REQUIRE(fixture.http.calls.size() == 4);
    CHECK(fixture.http.calls[0].method == "GET");
    CHECK(fixture.http.calls[0].url == "https://api.test/v2/menu");
    CHECK(fixture.http.calls[1].method == "PUT");
    CHECK(fixture.http.calls[1].url == "https://api.test/v2/menu");
    CHECK(nlohmann::json::parse(fixture.http.calls[1].body) == BuildMenuPutBody(SampleMenu()));
    CHECK(fixture.http.calls[2].url == "https://api.test/v2/panels?scope=c2c&limit=50");
    CHECK(fixture.http.calls[3].method == "POST");
    CHECK(fixture.http.calls[3].url == "https://api.test/v2/panels");
    CHECK(nlohmann::json::parse(fixture.http.calls[3].body) ==
          BuildPanelCreateBody(SamplePanel(), MakePanelRemark("qqbot", "main", "主面板")));
    // 鉴权头(token 不进日志/报告,只进请求头)。
    bool has_auth = false;
    for (const auto& [name, value] : fixture.http.calls[1].headers) {
        if (name == "Authorization") {
            has_auth = value == "QQBot TOKEN1";
        }
    }
    CHECK(has_auth);
    CHECK(report.error_detail.find("TOKEN1") == std::string::npos);

    // 状态落盘:菜单摘要 + panel_id。
    const nlohmann::json state = ReadJsonFile(fixture.root / "menu-panel.json");
    REQUIRE(state.contains("menu"));
    CHECK(state.at("menu").at("version") == 3);
    REQUIRE(state.contains("panel"));
    CHECK(state.at("panel").at("panel_id") == "p_new_1");
}

TEST_CASE("重启不重复创建:远端已同→零写;面板按备注认领复用") {
    PublisherFixture fixture("idempotent");
    {
        QqMenuPanelPublisher publisher(fixture.options);
        QqMenuPanelPublisher::SyncInput input;
        input.channel_id = "qqbot";
        input.account_id = "main";
        input.publish_menu = true;
        input.menu = SampleMenu();
        input.publish_panel = true;
        input.panel = SamplePanel();
        fixture.http.replies = {
            {200, R"({"version": 0})"},
            {200, R"({"version": 3})"},
            {200, R"({"records":[],"next_cursor":"","is_end":true})"},
            {200, R"({"panel_id":"p_keep_1"})"},
        };
        REQUIRE(publisher.Sync(input).error_code.empty());
    }
    // "重启":新发布器实例,同一份状态文件;远端已是期望内容。
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_menu = true;
    input.menu = SampleMenu();
    input.publish_panel = true;
    input.panel = SamplePanel();
    const std::string remark = MakePanelRemark("qqbot", "main", "主面板");
    nlohmann::json remote_panel = BuildPanelPayload(SamplePanel(), remark);
    fixture.http.replies = {
        {200, (nlohmann::json{{"menu", SampleMenuRemote()}, {"version", 3}}).dump()},
        {200, (nlohmann::json{{"records",
                              nlohmann::json::array({nlohmann::json{
                                  {"panel_id", "p_keep_1"},
                                  {"scope", "c2c"},
                                  {"target_type", "all"},
                                  {"panel", remote_panel},
                                  {"version", 2}})},
                              {"next_cursor", ""},
                              {"is_end", true}}})
                 .dump()},
    };
    const auto report = publisher.Sync(input);
    CHECK(report.menu_up_to_date);
    CHECK(report.panel_up_to_date);
    CHECK(report.panel_id == "p_keep_1");
    CHECK(report.error_code.empty());
    // 只有两笔 GET,零 PUT/POST(不耗 5 QPM 菜单写额、不重复建面板)。
    REQUIRE(fixture.http.calls.size() == 6);  // 前轮 4 + 本轮 2
    CHECK(fixture.http.CountBy("PUT", "/v2/menu") == 1);
    CHECK(fixture.http.CountBy("POST", "/v2/panels") == 1);
}

TEST_CASE("版本冲突:远端被人工改过→不覆盖,给本地提示") {
    PublisherFixture fixture("conflict");
    QqMenuPanelPublisher::Options seeded = fixture.options;
    {
        // 先发布一轮(落摘要)。
        QqMenuPanelPublisher publisher(seeded);
        QqMenuPanelPublisher::SyncInput input;
        input.channel_id = "qqbot";
        input.account_id = "main";
        input.publish_menu = true;
        input.menu = SampleMenu();
        fixture.http.replies = {
            {200, R"({"version": 0})"},
            {200, R"({"version": 3})"},
        };
        REQUIRE(publisher.Sync(input).error_code.empty());
    }
    // 远端被人工改成别的(与我们发布的摘要不符)。
    QqMenuPanelPublisher publisher(seeded);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_menu = true;
    input.menu = SampleMenu();
    fixture.http.replies = {
        {200, (nlohmann::json{{"menu", nlohmann::json{{"items", nlohmann::json::array(
                                                                {nlohmann::json{{"type", "send_message"},
                                                                                 {"name", "人工"},
                                                                                 {"send_message", "/manual"}}})}}},
                              {"version", 9}})
                 .dump()},
    };
    const auto report = publisher.Sync(input);
    CHECK(report.menu_conflict);
    CHECK_FALSE(report.menu_published);
    CHECK(report.error_code.empty());  // 冲突不是错误:是"不覆盖"的明确决定
    REQUIRE(report.notices.size() == 1);
    CHECK(report.notices[0].find("不覆盖") != std::string::npos);
    CHECK(fixture.http.CountBy("PUT", "/v2/menu") == 1);  // 只有首轮那一次
}

TEST_CASE("面板配额:POST 回 40030013 → 稳定码报告,不刷屏重试") {
    PublisherFixture fixture("quota");
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_menu = false;  // 只测面板路
    input.publish_panel = true;
    input.panel = SamplePanel();
    fixture.http.replies = {
        {200, R"({"records":[],"next_cursor":"","is_end":true})"},
        {400, R"({"code":40030013,"message":"超出数量限制"})"},
    };
    const auto report = publisher.Sync(input);
    CHECK(report.error_code == "menu.quota_exceeded");
    CHECK(report.retry_at_ms == 0);  // 配额耗尽不是退避重试能解的:等人处理
    CHECK_FALSE(report.panel_created);
}

TEST_CASE("specific 面板:按本地目标账增删关联对象(撤销时移除)") {
    PublisherFixture fixture("targets");
    const std::string remark = MakePanelRemark("qqbot", "main", "主面板");
    nlohmann::json remote_panel = BuildPanelPayload(SamplePanel("specific"), remark);
    nlohmann::json remote_record = nlohmann::json{
        {"panel_id", "p_t1"},
        {"scope", "c2c"},
        {"target_type", "specific"},
        {"panel", remote_panel},
        {"version", 1}};
    const auto panels_page = (nlohmann::json{{"records", nlohmann::json::array({remote_record})},
                                             {"next_cursor", ""},
                                             {"is_end", true}})
                                 .dump();
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_panel = true;
    input.panel = SamplePanel("specific");

    // 第一轮:desired = [b, c](本地账空 → 全 add)。
    input.desired_targets = {"openid-b", "openid-c"};
    fixture.http.replies = {
        {200, panels_page},
        {200, ""},  // PUT target op=add(无响应体)
    };
    auto report = publisher.Sync(input);
    CHECK(report.error_code.empty());
    REQUIRE(report.targets_added.size() == 2);
    CHECK(fixture.http.CountBy("PUT", "/target") == 1);
    CHECK(nlohmann::json::parse(fixture.http.calls[1].body) ==
          nlohmann::json::parse(R"({"op":"add","user_openids":["openid-b","openid-c"]})"));
    const nlohmann::json state1 = ReadJsonFile(fixture.root / "menu-panel.json");
    CHECK(state1.at("panel").at("targets").size() == 2);

    // 第二轮:desired 只剩 [b](c 撤销配对)→ op=del。
    input.desired_targets = {"openid-b"};
    fixture.http.replies = {
        {200, panels_page},
        {200, ""},
    };
    report = publisher.Sync(input);
    CHECK(report.error_code.empty());
    REQUIRE(report.targets_removed.size() == 1);
    CHECK(nlohmann::json::parse(fixture.http.calls[3].body) ==
          nlohmann::json::parse(R"({"op":"del","user_openids":["openid-c"]})"));
    // 第三轮:无变化 → 不碰 target 接口。
    input.desired_targets = {"openid-b"};
    fixture.http.replies = {
        {200, panels_page},
    };
    report = publisher.Sync(input);
    CHECK(report.error_code.empty());
    CHECK(report.targets_added.empty());
    CHECK(report.targets_removed.empty());
    CHECK(fixture.http.CountBy("PUT", "/target") == 2);
}

TEST_CASE("面板列表分页:next_cursor 翻页到认领") {
    PublisherFixture fixture("paging");
    const std::string remark = MakePanelRemark("qqbot", "main", "主面板");
    nlohmann::json remote_panel = BuildPanelPayload(SamplePanel(), remark);
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_panel = true;
    input.panel = SamplePanel();
    fixture.http.replies = {
        {200, R"({"records":[{"panel_id":"p_other","panel":{"remark":"别人的"}}],"next_cursor":"c2","is_end":false})"},
        {200, (nlohmann::json{{"records",
                              nlohmann::json::array({nlohmann::json{
                                  {"panel_id", "p_page2"},
                                  {"scope", "c2c"},
                                  {"target_type", "all"},
                                  {"panel", remote_panel},
                                  {"version", 1}})},
                              {"next_cursor", ""},
                              {"is_end", true}}})
                 .dump()},
    };
    const auto report = publisher.Sync(input);
    CHECK(report.error_code.empty());
    CHECK(report.panel_up_to_date);
    CHECK(report.panel_id == "p_page2");
    REQUIRE(fixture.http.calls.size() == 2);
    CHECK(fixture.http.calls[1].url.find("&cursor=c2") != std::string::npos);
}

TEST_CASE("QPM 限速:menu 写 5/分钟,第 6 次被门拦住(不发 HTTP)") {
    // 滑窗逻辑直测。
    MenuQpmLimiter limiter;
    MenuQpmLimiter::Budget budget{5, 60'000};
    std::int64_t retry_at = 0;
    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.TryAcquire(budget, 1'000, &retry_at));
    }
    CHECK_FALSE(limiter.TryAcquire(budget, 2'000, &retry_at));
    CHECK(retry_at == 61'000);  // 首笔出窗时刻
    // 出窗后配额回来。
    CHECK(limiter.TryAcquire(budget, 61'000, &retry_at));

    // 发布器级:菜单写预算 5 次,第 6 次同步在 PUT 前被拦(GET 已发)。
    PublisherFixture fixture("qpm");
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_menu = true;
    input.menu = SampleMenu();
    // 期望菜单逐轮变;远端回"上一轮发布的菜单"(摘要与本地已发布账一致,
    // 不触发冲突支路),每轮都真实 PUT。
    for (int round = 0; round < 5; ++round) {
        input.menu.items[0].send_message = "/help-" + std::to_string(round);
        if (round == 0) {
            fixture.http.replies.push_back({200, R"({"version": 0})"});
        } else {
            ChannelMenuUserConfig previous = SampleMenu();
            previous.items[0].send_message = "/help-" + std::to_string(round - 1);
            fixture.http.replies.push_back(
                {200, (nlohmann::json{{"menu", BuildMenuPutBody(previous).at("menu")},
                                      {"version", 9 + round}})
                          .dump()});
        }
        fixture.http.replies.push_back({200, (nlohmann::json{{"version", 10 + round}}).dump()});
        const auto report = publisher.Sync(input);
        CHECK(report.menu_published);
    }
    input.menu.items[0].send_message = "/help-5";
    ChannelMenuUserConfig last = SampleMenu();
    last.items[0].send_message = "/help-4";
    fixture.http.replies.push_back(
        {200, (nlohmann::json{{"menu", BuildMenuPutBody(last).at("menu")}, {"version", 14}})
                  .dump()});
    const auto blocked = publisher.Sync(input);
    CHECK(blocked.error_code == "menu.rate_limited");
    CHECK(blocked.retry_at_ms > fixture.now);
    CHECK(fixture.http.CountBy("PUT", "/v2/menu") == 5);
}

TEST_CASE("token 失效:401 → invalidate + 稳定码") {
    PublisherFixture fixture("auth");
    QqMenuPanelPublisher publisher(fixture.options);
    QqMenuPanelPublisher::SyncInput input;
    input.channel_id = "qqbot";
    input.account_id = "main";
    input.publish_menu = true;
    input.menu = SampleMenu();
    fixture.http.replies = {
        {401, R"({"code":100030})"},
    };
    const auto report = publisher.Sync(input);
    CHECK(report.error_code == "menu.auth_failed");
    CHECK(report.retry_at_ms > fixture.now);
}
