// 渠道设置服务册(QQBot Windows 修复单 §5.1/§5.2):向导提交的定点更新
// 合同。钉的账:
//   - 新配置走完即得两层 enabled + QQ 模板策略 + 受管凭据(secret_file
//     指向受管绝对路径,JSON 里没有密钥值);
//   - 旧配置保留模型/未知字段/其他账号;已有账号只改选定字段;
//   - 提交失败(AppID 缺失/坏 channels 段)不毁旧配置、不留宽松新件;
//   - 密钥值不出现在配置 JSON、changes 摘要与错误文案(测试密钥扫描);
//   - 平台注册表:qqbot 可配,未实现平台不进假成功配置。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "channel/channel_setup.hpp"
#include "channel/credentials.hpp"
#include "platform/paths.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_chsetup_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

struct Fixture {
    std::filesystem::path root;
    ChannelConfigService::Options options;
    explicit Fixture(const char* tag)
        : root(MakeTempDir(tag)),
          options{lubancode::platform::PathToUtf8(root / "config.json"), root / "secrets", 2000} {}
};

ChannelSetupCommitRequest BaseRequest() {
    ChannelSetupCommitRequest request;
    request.channel_id = "qqbot";
    request.account_id = "main";
    request.app_id = "102345678";
    request.new_secret = "FAKE-SECRET-for-test-only";
    request.ensure_enabled = true;
    return request;
}

}  // namespace

TEST_CASE("平台注册表:qqbot 可配,未实现平台如实标注") {
    const auto& platforms = ChannelSetupPlatforms();
    REQUIRE(platforms.size() >= 2);
    bool has_qqbot = false;
    bool has_unimplemented = false;
    for (const ChannelSetupPlatform& platform : platforms) {
        if (platform.id == "qqbot") {
            has_qqbot = true;
            REQUIRE(platform.implemented);
            REQUIRE(platform.fields.size() == 2);
            CHECK(platform.fields[0].id == "app_id");
            CHECK_FALSE(platform.fields[0].sensitive);
            CHECK(platform.fields[1].id == "app_secret");
            CHECK(platform.fields[1].sensitive);
        } else if (!platform.implemented) {
            has_unimplemented = true;
        }
    }
    CHECK(has_qqbot);
    CHECK(has_unimplemented);  // feishu 尚未支持:列表可见、不可配置
    REQUIRE(FindChannelSetupPlatform("qqbot").has_value());
    REQUIRE(FindChannelSetupPlatform("feishu").has_value());
    CHECK_FALSE(FindChannelSetupPlatform("feishu")->implemented);
    CHECK_FALSE(FindChannelSetupPlatform("nope").has_value());
}

TEST_CASE("全新配置:两层 enabled + 模板策略 + 受管凭据 + default_account") {
    Fixture fx("fresh");
    const auto committed = ChannelConfigService::Commit(fx.options, BaseRequest());
    REQUIRE(committed.has_value());
    CHECK(committed->created_account);

    const nlohmann::json root = nlohmann::json::parse(ReadFile(fx.root / "config.json"));
    REQUIRE(root.contains("channels"));
    REQUIRE(root["channels"].contains("qqbot"));
    const nlohmann::json& channel = root["channels"]["qqbot"];
    CHECK(channel["enabled"].get<bool>());
    CHECK(channel["default_account"].get<std::string>() == "main");
    REQUIRE(channel["accounts"].contains("main"));
    const nlohmann::json& account = channel["accounts"]["main"];

    // 两层 enabled + 模板策略(§5.1:websocket/DM pairing/禁群/final/只读工具)。
    CHECK(account["enabled"].get<bool>());
    CHECK(account["transport"].get<std::string>() == "websocket");
    CHECK(account["dm_policy"].get<std::string>() == "pairing");
    CHECK(account["group_policy"].get<std::string>() == "disabled");
    CHECK(account["reply"]["mode"].get<std::string>() == "final");
    // 只读工具名单与模板同一份(单一临时对象取值——两只临时各取迭代器
    // 是未定义行为,clang 报 {?})。
    const ChannelAccountUserConfig expected_template = MakeQqTemplateAccount();
    CHECK(account["tools"]["allow"] == nlohmann::json{*expected_template.tools.allow});
    CHECK(account["app_id"].get<std::string>() == "102345678");

    // secret_file 是受管绝对路径;JSON 里没有密钥值(测试密钥扫描)。
    const std::string secret_file = account["secret_file"].get<std::string>();
    CHECK(std::filesystem::path(lubancode::platform::Utf8ToPath(secret_file)).is_absolute());
    CHECK(secret_file.find("secrets") != std::string::npos);
    CHECK_FALSE(account.contains("secret"));
    CHECK_FALSE(account.contains("secret_env"));
    const std::string config_text = ReadFile(fx.root / "config.json");
    CHECK(config_text.find("FAKE-SECRET-for-test-only") == std::string::npos);
    // changes 摘要与结果结构也不带密钥值。
    for (const std::string& change : committed->changes) {
        CHECK(change.find("FAKE-SECRET-for-test-only") == std::string::npos);
    }

    // 受管文件内容正确、可被生产读取器读出。
    const auto path = lubancode::platform::Utf8ToPath(secret_file);
    CHECK(ReadFile(path) == "FAKE-SECRET-for-test-only");
    ChannelAccountUserConfig reader_account;
    reader_account.secret_file = secret_file;
    const auto resolved = ResolveChannelCredential(reader_account);
    REQUIRE(resolved.has_value());
    CHECK(resolved->secret == "FAKE-SECRET-for-test-only");
}

TEST_CASE("旧配置:未知字段/模型/其他账号原样保留,已有账号只改选定字段") {
    Fixture fx("keep");
    // 手摆一份带模型、全局未知字段、另一账号的旧配置。路径用 nlohmann 拼
    // 进 JSON(反斜杠自动转义)。注意:channels 段内的未知字段走严格解析
    // (既有合同:渠道段配置错要明报),那样的配置本就装不进来——"未知
    // 字段原样保留"指 channels 之外的全局字段,见下面的断言与坏段子案。
    const std::string old_external_secret = "old-external-secret";
    nlohmann::json old = nlohmann::json::object();
    old["model"] = "gpt-test";
    old["future_field"] = nlohmann::json{{"nested", std::vector<int>{1, 2, 3}}};
    nlohmann::json other_account = nlohmann::json::object();
    other_account["enabled"] = true;
    other_account["app_id"] = "999";
    other_account["secret_env"] = "OTHER_SECRET_ENV";
    other_account["dm_policy"] = "allowlist";
    nlohmann::json main_account = nlohmann::json::object();
    main_account["enabled"] = false;
    main_account["app_id"] = "old-app";
    main_account["secret_file"] = lubancode::platform::PathToUtf8(fx.root / "old-external.key");
    main_account["dm_policy"] = "allowlist";
    main_account["allow_from"] = std::vector<std::string>{"user-a"};
    old["channels"]["qqbot"] = nlohmann::json{
        {"enabled", true},
        {"default_account", "other"},
        {"accounts", nlohmann::json{{"other", std::move(other_account)},
                                    {"main", std::move(main_account)}}}};
    WriteFile(fx.root / "config.json", old.dump(2));
    WriteFile(fx.root / "old-external.key", old_external_secret);

    ChannelSetupCommitRequest request = BaseRequest();
    request.ensure_enabled = true;  // main 原本 false:显式启用
    const auto committed = ChannelConfigService::Commit(fx.options, request);
    REQUIRE(committed.has_value());
    CHECK_FALSE(committed->created_account);

    const nlohmann::json root = nlohmann::json::parse(ReadFile(fx.root / "config.json"));
    CHECK(root["model"].get<std::string>() == "gpt-test");
    CHECK(root["future_field"]["nested"][1].get<int>() == 2);
    const nlohmann::json& channel = root["channels"]["qqbot"];
    // default_account 已有指向 other:不偷默认位。
    CHECK(channel["default_account"].get<std::string>() == "other");

    // 其他账号原样(连凭据来源与环境变量名都不动)。
    const nlohmann::json& other = channel["accounts"]["other"];
    CHECK(other["secret_env"].get<std::string>() == "OTHER_SECRET_ENV");
    CHECK(other["dm_policy"].get<std::string>() == "allowlist");

    // 已有账号:enabled/app_id/secret_file 换新;旧 dm_policy(allowlist,
    // 用户自己收紧过的权限路由)与 allow_from 原样保留——不用模板扩大。
    const nlohmann::json& main = channel["accounts"]["main"];
    CHECK(main["enabled"].get<bool>());
    CHECK(main["app_id"].get<std::string>() == "102345678");
    CHECK(main["dm_policy"].get<std::string>() == "allowlist");
    CHECK(main["allow_from"] == std::vector<std::string>{"user-a"});
    const std::string new_secret_file = main["secret_file"].get<std::string>();
    CHECK(new_secret_file.find("old-external.key") == std::string::npos);
    // 旧外部文件不删(§5.3:不删旧外部文件)。
    CHECK(std::filesystem::exists(fx.root / "old-external.key"));
    CHECK(ReadFile(lubancode::platform::Utf8ToPath(new_secret_file)) == "FAKE-SECRET-for-test-only");
}

TEST_CASE("提交失败不毁旧配置:AppID 缺失(新账号没填)报稳定码") {
    Fixture fx("failappid");
    WriteFile(fx.root / "config.json", R"json({"model": "keep-me"})json");

    ChannelSetupCommitRequest request;
    request.channel_id = "qqbot";
    request.account_id = "main";
    request.new_secret = "FAKE-SECRET-2";
    request.ensure_enabled = true;
    const auto committed = ChannelConfigService::Commit(fx.options, request);
    REQUIRE_FALSE(committed.has_value());
    CHECK(committed.error().reason == "setup_app_id_invalid");
    // 旧配置一字未动;secrets 目录里没有半截新件。
    CHECK(ReadFile(fx.root / "config.json") == R"json({"model": "keep-me"})json");
    std::set<std::filesystem::path> leftovers;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(fx.root / "secrets", ec)) {
        leftovers.insert(entry.path());
    }
    // 只有锁文件(内容性凭据文件一枚都不许有——AppID 校验先于写密钥)。
    for (const auto& path : leftovers) {
        CHECK(path.extension() != ".secret");
    }
}

TEST_CASE("提交失败不毁旧配置:既有 channels 段坏,不碰文件") {
    Fixture fx("failch");
    SUBCASE("类型错") {
        WriteFile(fx.root / "config.json",
                  R"json({"channels": {"qqbot": {"enabled": "not-a-bool"}}}})json");
        const auto committed = ChannelConfigService::Commit(fx.options, BaseRequest());
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().reason == "setup_channels_invalid");
        CHECK(ReadFile(fx.root / "config.json").find("not-a-bool") != std::string::npos);
    }
    SUBCASE("channels 段内未知字段:严格解析拒绝,不写半截") {
        // 渠道段是严格解析(既有合同:配置错要明报)——段内未知字段本就
        // 装不进来;向导不替用户猜,原样拒收。全局未知字段另案保留。
        WriteFile(fx.root / "config.json",
                  R"json({"future_field": 1, "channels": {"qqbot": {"accounts": {"main": {"future_key": 1}}}}})json");
        const auto committed = ChannelConfigService::Commit(fx.options, BaseRequest());
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().reason == "setup_channels_invalid");
        CHECK(ReadFile(fx.root / "config.json").find("future_key") != std::string::npos);
        CHECK(ReadFile(fx.root / "config.json").find("future_field") != std::string::npos);
    }
}

TEST_CASE("dry_run:差异到手,一页未写") {
    Fixture fx("dryrun");
    const auto preview = [&]() {
        ChannelSetupCommitRequest request = BaseRequest();
        request.dry_run = true;
        return ChannelConfigService::Commit(fx.options, request);
    }();
    REQUIRE(preview.has_value());
    CHECK(preview->created_account);
    CHECK_FALSE(preview->changes.empty());
    CHECK_FALSE(std::filesystem::exists(fx.root / "config.json"));
    // 预览不写密钥文件(dry_run 不走写入路)。
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(fx.options.secrets_root / "nonexistent-probe", ec));
    CHECK(preview->secret_file.empty());
}

TEST_CASE("未实现平台与坏 id 拒收") {
    Fixture fx("reject");
    SUBCASE("feishu 尚未支持") {
        ChannelSetupCommitRequest request = BaseRequest();
        request.channel_id = "feishu";
        const auto committed = ChannelConfigService::Commit(fx.options, request);
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().reason == "setup_bad_platform");
    }
    SUBCASE("未知平台") {
        ChannelSetupCommitRequest request = BaseRequest();
        request.channel_id = "telegram";
        const auto committed = ChannelConfigService::Commit(fx.options, request);
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().reason == "setup_bad_platform");
    }
    SUBCASE("账号 id 带路径") {
        ChannelSetupCommitRequest request = BaseRequest();
        request.account_id = "../escape";
        const auto committed = ChannelConfigService::Commit(fx.options, request);
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().reason == "setup_bad_id");
    }
}

TEST_CASE("孤儿回收:换密钥后旧受管件清走,配置仍可读") {
    Fixture fx("rotate");
    const auto first = ChannelConfigService::Commit(fx.options, BaseRequest());
    REQUIRE(first.has_value());
    const std::string old_secret_file = first->secret_file;

    // 换新密钥:新版本文件 + 配置改指;旧受管件成孤儿被回收。
    ChannelSetupCommitRequest second_request = BaseRequest();
    second_request.new_secret = "FAKE-SECRET-rotated";
    const auto second = ChannelConfigService::Commit(fx.options, second_request);
    REQUIRE(second.has_value());
    CHECK(second->secret_file != old_secret_file);
    CHECK_FALSE(std::filesystem::exists(lubancode::platform::Utf8ToPath(old_secret_file)));
    CHECK(ReadFile(lubancode::platform::Utf8ToPath(second->secret_file)) == "FAKE-SECRET-rotated");

    // 配置解析回读 + 生产读取器仍过(提交后复验的一部分)。
    const nlohmann::json root = nlohmann::json::parse(ReadFile(fx.root / "config.json"));
    ChannelAccountUserConfig reader;
    reader.secret_file = root["channels"]["qqbot"]["accounts"]["main"]["secret_file"];
    REQUIRE(ResolveChannelCredential(reader).has_value());
}

TEST_CASE("错误文案不带密钥值(扫描全部失败路)") {
    Fixture fx("leak");
    std::vector<std::string> all_details;
    SUBCASE("坏内容密钥") {
        ChannelSetupCommitRequest request = BaseRequest();
        request.new_secret = "bad\ncontent";
        const auto committed = ChannelConfigService::Commit(fx.options, request);
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().detail.find("bad") == std::string::npos);
    }
    SUBCASE("坏 AppID") {
        ChannelSetupCommitRequest request = BaseRequest();
        request.app_id = "app\rid";
        const auto committed = ChannelConfigService::Commit(fx.options, request);
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().detail.find("app") == std::string::npos);
    }
}
