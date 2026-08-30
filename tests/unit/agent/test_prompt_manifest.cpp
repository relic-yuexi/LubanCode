// PromptManifest 与 request snapshot 合同测试(Token 账本单 §15.1 A0)。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "agent/prompt_manifest.hpp"

using namespace lubancode::agent;

namespace {

PromptManifest MakeManifest() {
    PromptManifest manifest;
    manifest.assembly_version = "prompt-assembler-v1";
    manifest.resolved_prompt_hash = std::string(64, 'a');
    manifest.resolved_prompt_tokens_estimated = 18420;
    manifest.stable_prefix_hash = std::string(64, 'b');
    PromptSegment embedded;
    embedded.segment_id = "core/10-identity.md";
    embedded.role = "core";
    embedded.source_kind = "embedded";
    embedded.source_ref = "embedded:core/10-identity.md";
    embedded.source_hash = std::string(64, 'c');
    embedded.rendered_hash = std::string(64, 'd');
    embedded.rendered_tokens_estimated = 1320;
    embedded.order = 10;
    manifest.segments.push_back(embedded);
    PromptSegment runtime;
    runtime.segment_id = "runtime/environment";
    runtime.role = "runtime";
    runtime.source_kind = "host_generated";
    runtime.source_ref = "runtime:environment";
    runtime.rendered_hash = std::string(64, 'e');
    runtime.rendered_tokens_estimated = 180;
    runtime.order = 90;
    runtime.volatile_segment = true;
    manifest.segments.push_back(runtime);
    manifest.layers = {"embedded", "user_prompt_dir"};
    manifest.soul = {"default", std::string(64, 'f'), 0};
    manifest.model_instructions = {std::string(64, '7'), 220};
    return manifest;
}

}  // namespace

TEST_CASE("manifest round-trip 与次序稳定") {
    const PromptManifest manifest = MakeManifest();
    const nlohmann::json json = manifest.ToJson();
    std::string error;
    const auto parsed = PromptManifest::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    INFO(error.c_str());
    CHECK(parsed->assembly_version == "prompt-assembler-v1");
    REQUIRE(parsed->segments.size() == 2);
    CHECK(parsed->segments[0].segment_id == "core/10-identity.md");
    CHECK(parsed->segments[0].source_hash.has_value());
    CHECK(parsed->segments[1].volatile_segment);
    CHECK(!parsed->segments[1].source_hash.has_value());
    CHECK(parsed->ToJson() == json);

    // 次序稳定:乱序塞进结构,写出前按 order 排。
    PromptManifest shuffled = manifest;
    std::swap(shuffled.segments[0], shuffled.segments[1]);
    CHECK(shuffled.ToJson() == json);
}

TEST_CASE("manifest 坏形拒收") {
    std::string error;
    nlohmann::json json = MakeManifest().ToJson();
    // 未知键。
    nlohmann::json unknown = json;
    unknown["body"] = "正文不许进 manifest";
    CHECK(!PromptManifest::FromJsonStrict(unknown, &error).has_value());
    // 段缺必填。
    nlohmann::json missing = json;
    missing["segments"][0].erase("rendered_hash");
    CHECK(!PromptManifest::FromJsonStrict(missing, &error).has_value());
    // soul 键多。
    nlohmann::json bad_soul = json;
    bad_soul["soul"]["text"] = "魂正文不许进";
    CHECK(!PromptManifest::FromJsonStrict(bad_soul, &error).has_value());
    // schema 版本。
    nlohmann::json bad_version = json;
    bad_version["schema_version"] = 2;
    CHECK(!PromptManifest::FromJsonStrict(bad_version, &error).has_value());
}

TEST_CASE("request snapshot round-trip,content_policy 钉死") {
    RequestSnapshotMetadata snapshot;
    snapshot.request_shape.model = "gpt-5.6-sol";
    snapshot.request_shape.message_count = 17;
    snapshot.request_shape.tool_count = 42;
    snapshot.request_shape.parameters_hash = std::string(64, '1');
    snapshot.request_shape.toolset_hash = std::string(64, '2');
    snapshot.request_shape.tool_definition_tokens_estimated = 9200;
    snapshot.prompt_manifest = MakeManifest();
    const nlohmann::json json = snapshot.ToJson();
    std::string error;
    const auto parsed = RequestSnapshotMetadata::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->request_shape.tool_count == 42);
    CHECK(parsed->prompt_manifest.segments.size() == 2);
    CHECK(parsed->content_policy == "metadata_only");
    CHECK(parsed->ToJson() == json);

    // 正文策略只认 metadata_only:full 形状另立 schema,不许偷塞。
    nlohmann::json bad_policy = json;
    bad_policy["content_policy"] = "full";
    CHECK(!RequestSnapshotMetadata::FromJsonStrict(bad_policy, &error).has_value());
    // snapshot 顶层不许带 messages/tools 正文。
    nlohmann::json leak = json;
    leak["messages"] = nlohmann::json::array();
    CHECK(!RequestSnapshotMetadata::FromJsonStrict(leak, &error).has_value());
}
