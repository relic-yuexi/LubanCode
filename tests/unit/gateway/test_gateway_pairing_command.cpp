// QQ 接入单 Q1b:配对控制命令的文件面册(与 stop.json 同款通道纪律)。
//   - 命令/回执的写-轮询-取走往返;
//   - boot_id 对不上 = 陈旧命令,删掉不产回执(不追新实例);
//   - 坏命令文件消费即删;结果文件不带 .result 后缀的不会被命令轮询吃掉;
//   - command_id 守门(拼文件名的单段串);
//   - 严格解析:缺字段/坏 action 报错不投。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "gateway/pairing_command.hpp"

using namespace lubancode::gateway;

namespace {

std::filesystem::path MakeControlDir(const char* test_name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-pairing-cmd-" + std::string(test_name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

GatewayPairingCommand SampleCommand() {
    GatewayPairingCommand command;
    command.boot_id = "boot-1";
    command.command_id = "abc123";
    command.action = "approve";
    command.channel_id = "qqbot";
    command.account_id = "main";
    command.token = "ABCD2345";
    command.requested_at_ms = 1'000;
    return command;
}

std::string ReadFileText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("命令写-轮询-取走往返:boot 对上即收到,文件即删") {
    const auto dir = MakeControlDir("roundtrip");
    REQUIRE(WritePairingCommand(dir, SampleCommand()).empty());

    const auto polled = PollPairingCommands(dir, "boot-1");
    REQUIRE(polled.size() == 1);
    CHECK(polled[0].command_id == "abc123");
    CHECK(polled[0].action == "approve");
    CHECK(polled[0].token == "ABCD2345");
    CHECK(polled[0].channel_id == "qqbot");
    // 消费即删:再轮询为空。
    CHECK(PollPairingCommands(dir, "boot-1").empty());

    // 回执:写-读往返,读到即删。
    GatewayPairingCommandResult result;
    result.command_id = "abc123";
    result.action = "approve";
    result.ok = true;
    result.sender_id = "stranger-1";
    REQUIRE(WritePairingCommandResult(dir, result).empty());
    std::string error;
    const auto taken = TakePairingCommandResult(dir, "abc123", &error);
    REQUIRE(taken.has_value());
    CHECK(taken->ok);
    CHECK(taken->sender_id == "stranger-1");
    CHECK(error.empty());
    // 读走即删:再取为空,error 空(不是错)。
    CHECK_FALSE(TakePairingCommandResult(dir, "abc123", &error).has_value());
    CHECK(error.empty());
}

TEST_CASE("陈旧命令:boot_id 对不上,删掉不产回执") {
    const auto dir = MakeControlDir("stale");
    REQUIRE(WritePairingCommand(dir, SampleCommand()).empty());
    CHECK(PollPairingCommands(dir, "boot-2").empty());  // 别的实例不吃
    // 文件已删(不追新实例)。
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(dir / "pairing-abc123.json", ec));
    // 空 boot_id(不指名)的最老语义:任何实例都吃。
    auto command = SampleCommand();
    command.boot_id = "";
    command.command_id = "open0000";
    REQUIRE(WritePairingCommand(dir, command).empty());
    CHECK(PollPairingCommands(dir, "boot-2").size() == 1);
}

TEST_CASE("结果文件不被命令轮询误吃;坏命令文件消费即删") {
    const auto dir = MakeControlDir("hygiene");
    GatewayPairingCommandResult result;
    result.command_id = "res00001";
    result.action = "reject";
    result.ok = false;
    result.error = "expired";
    REQUIRE(WritePairingCommandResult(dir, result).empty());
    // 命令轮询只吃 pairing-<id>.json,结果文件(.result.json)原样留着。
    CHECK(PollPairingCommands(dir, "boot-1").empty());
    std::error_code ec;
    CHECK(std::filesystem::exists(dir / "pairing-res00001.result.json", ec));

    // 坏命令文件(写一半的竞态尾巴):消费即删,不崩不产回执。
    std::ofstream(dir / "pairing-bad0001.json", std::ios::trunc) << "{ half";
    CHECK(PollPairingCommands(dir, "boot-1").empty());
    CHECK_FALSE(std::filesystem::exists(dir / "pairing-bad0001.json", ec));

    // 无关文件:不动。
    std::ofstream(dir / "stop.json", std::ios::trunc) << "{}";
    std::ofstream(dir / "readme.txt", std::ios::trunc) << "x";
    CHECK(PollPairingCommands(dir, "boot-1").empty());
    CHECK(std::filesystem::exists(dir / "readme.txt", ec));
    CHECK(std::filesystem::exists(dir / "stop.json", ec));
}

TEST_CASE("command_id 守门与严格解析") {
    CHECK(IsValidPairingCommandId("abc-123"));
    CHECK_FALSE(IsValidPairingCommandId(""));
    CHECK_FALSE(IsValidPairingCommandId("a/b"));
    CHECK_FALSE(IsValidPairingCommandId(".."));
    CHECK_FALSE(IsValidPairingCommandId("a b"));
    CHECK_FALSE(IsValidPairingCommandId(std::string(65, 'a')));

    // 坏 id 写不进(拼不出越界路径)。
    const auto dir = MakeControlDir("guard");
    auto command = SampleCommand();
    command.command_id = "../escape";
    CHECK_FALSE(WritePairingCommand(dir, command).empty());

    // 严格解析:缺字段/坏 action。
    std::string error;
    auto parsed = GatewayPairingCommand::FromJson(
        nlohmann::json{{"boot_id", "b"}, {"command_id", "c"}, {"action", "nuke"},
                       {"channel_id", "qqbot"}, {"account_id", "main"}, {"token", "t"}},
        &error);
    CHECK_FALSE(parsed.has_value());
    CHECK_FALSE(error.empty());
    parsed = GatewayPairingCommand::FromJson(nlohmann::json{{"boot_id", "b"}}, &error);
    CHECK_FALSE(parsed.has_value());
    // 好命令全字段通过。
    parsed = GatewayPairingCommand::FromJson(SampleCommand().ToJson(), &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->token == "ABCD2345");
}
