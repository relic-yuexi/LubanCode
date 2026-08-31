// 多渠道消息接入单阶段 1:channel.yaml schema 1 parser 册。照
// channel-manifest.md 冻结件的字段表、取值集、占位符信任规矩逐条核:
// 未知字段报错、坏类型报错、坏占位符报错、越界报错、capabilities 六项
// 数组只认冻结取值集、risk 只认唯一值。
#include <doctest/doctest.h>

#include "channel/manifest.hpp"

using namespace lubancode::channel;
namespace fs = std::filesystem;

namespace {

// channel-manifest.md §2 的完整示例(逐字抄,越贴文档越能当回归锚)。
const char* kFullExample = R"yaml(
schema: 1
id: qqbot
name: QQ Bot
description: Connect QQ Bot accounts through the official QQ Bot API.

runtime:
  kind: process
  command: node
  args:
    - ${channel_dir}/runtime/dist/index.js
  protocol: lubancode-channel/1
  startup_timeout_ms: 15000
  shutdown_timeout_ms: 5000
  requires:
    executables:
      - name: node
        version: ">=20"

capabilities:
  transports: [websocket, webhook]
  conversations: [direct, group, guild, thread]
  inbound: [text, image, audio, video, file, mention, reply]
  outbound: [text, image, audio, video, file, reply]
  delivery: [send, edit, native_stream, typing]
  login: [credentials, qr]

limits:
  text_chars: 2000
  media_bytes: 20971520
  outbound_requests_per_minute: 60

state:
  format: 1
  migrator: ${channel_dir}/runtime/dist/migrate.js
)yaml";

}  // namespace

TEST_CASE("channel.yaml:冻结文档完整示例逐字段过") {
    const fs::path package_root = fs::path("/pkg");
    const fs::path channel_dir = package_root / "channels" / "qqbot";
    const auto parsed = ParseChannelManifestYaml(kFullExample, package_root, channel_dir);
    REQUIRE(parsed.has_value());
    const ChannelManifest& manifest = *parsed;
    CHECK(manifest.schema == 1);
    CHECK(manifest.id == "qqbot");
    CHECK(manifest.name == "QQ Bot");
    CHECK(manifest.runtime.kind == "process");
    CHECK(manifest.runtime.command == "node");
    REQUIRE(manifest.runtime.args.size() == 1);
    CHECK(manifest.runtime.args[0] == "${channel_dir}/runtime/dist/index.js");
    CHECK(manifest.runtime.protocol == "lubancode-channel/1");
    CHECK(manifest.runtime.startup_timeout_ms == 15000);
    CHECK(manifest.runtime.shutdown_timeout_ms == 5000);
    REQUIRE(manifest.runtime.requires_executables.size() == 1);
    CHECK(manifest.runtime.requires_executables[0].name == "node");
    REQUIRE(manifest.runtime.requires_executables[0].version.has_value());
    CHECK(*manifest.runtime.requires_executables[0].version == ">=20");

    CHECK(manifest.capabilities.transports == std::vector<std::string>{"websocket", "webhook"});
    CHECK(manifest.capabilities.conversations ==
          std::vector<std::string>{"direct", "group", "guild", "thread"});
    CHECK(manifest.capabilities.delivery ==
          std::vector<std::string>{"send", "edit", "native_stream", "typing"});
    CHECK(manifest.capabilities.login == std::vector<std::string>{"credentials", "qr"});

    REQUIRE(manifest.limits.text_chars.has_value());
    CHECK(*manifest.limits.text_chars == 2000);
    REQUIRE(manifest.limits.media_bytes.has_value());
    CHECK(*manifest.limits.media_bytes == 20971520);

    CHECK(manifest.state.format == 1);
    REQUIRE(manifest.state.migrator.has_value());
    CHECK(*manifest.state.migrator == "${channel_dir}/runtime/dist/migrate.js");
    CHECK_FALSE(manifest.risk.has_value());
}

TEST_CASE("channel.yaml:只认 schema 1") {
    const auto parsed =
        ParseChannelManifestYaml("schema: 2\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n"
                                 "  command: node\n  protocol: lubancode-channel/1\n",
                                 "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
    bool found = false;
    for (const auto& error : parsed.error()) {
        if (error.field == "schema") found = true;
    }
    CHECK(found);
}

TEST_CASE("channel.yaml:未知顶层字段报错") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nbogus_field: 1\nruntime:\n  kind: process\n"
        "  command: node\n  protocol: lubancode-channel/1\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
    bool found = false;
    for (const auto& error : parsed.error()) {
        if (error.detail.find("bogus_field") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST_CASE("channel.yaml:runtime.kind 只认 process") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: native\n  command: node\n"
        "  protocol: lubancode-channel/1\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
}

TEST_CASE("channel.yaml:runtime.protocol 只认 lubancode-channel/1") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  protocol: some-other/1\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
}

TEST_CASE("channel.yaml:args 只许 ${channel_dir} 占位符") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  args:\n    - ${package_dir}/index.js\n  protocol: lubancode-channel/1\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
    bool found = false;
    for (const auto& error : parsed.error()) {
        if (error.detail.find("认不得的占位符") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST_CASE("channel.yaml:${channel_dir} 展开逃出包根报错") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  args:\n    - ${channel_dir}/../../../etc/passwd\n  protocol: lubancode-channel/1\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
    bool found = false;
    for (const auto& error : parsed.error()) {
        if (error.detail.find("path_escape") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST_CASE("channel.yaml:capabilities 取值不在冻结集合内报错") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  protocol: lubancode-channel/1\ncapabilities:\n  transports: [carrier_pigeon]\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
}

TEST_CASE("channel.yaml:id 须小写 kebab-case") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: QQ_Bot\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  protocol: lubancode-channel/1\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
}

TEST_CASE("channel.yaml:risk 只认 unofficial_personal_account") {
    const auto bad = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  protocol: lubancode-channel/1\nrisk: yolo\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(bad.has_value());

    const auto ok = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  protocol: lubancode-channel/1\nrisk: unofficial_personal_account\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE(ok.has_value());
    REQUIRE(ok->risk.has_value());
    CHECK(*ok->risk == "unofficial_personal_account");
}

TEST_CASE("channel.yaml:limits 负数报错") {
    const auto parsed = ParseChannelManifestYaml(
        "schema: 1\nid: x\nname: X\ndescription: d\nruntime:\n  kind: process\n  command: node\n"
        "  protocol: lubancode-channel/1\nlimits:\n  text_chars: -5\n",
        "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
}

TEST_CASE("channel.yaml:根不是映射报错") {
    const auto parsed = ParseChannelManifestYaml("- just\n- a\n- list\n", "/pkg", "/pkg/channels/x");
    REQUIRE_FALSE(parsed.has_value());
}
