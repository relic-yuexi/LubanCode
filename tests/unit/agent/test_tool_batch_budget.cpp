#include <doctest/doctest.h>

#include "agent/tool_batch_budget.hpp"
#include "api/model_input_snapshot.hpp"
#include "hooks/middleware_builtins.hpp"

using namespace lubancode;

namespace {
api::Message Batch(std::initializer_list<std::size_t> sizes) {
    api::Message message;
    message.role = api::Role::User;
    for (auto size : sizes) {
        api::ToolResultBlock result;
        result.tool_use_id = "call-" + std::to_string(message.content.size());
        result.content.assign(size, 'x');
        message.content.push_back(std::move(result));
    }
    return message;
}
}

TEST_CASE("new tool batch shares the remaining 20K without changing source") {
    const auto source = Batch({16000, 16000, 16000, 16000, 16000,
                               16000, 16000, 16000, 16000, 16000});
    const auto plan = agent::PlanToolBatchBudget(source, 20000);
    REQUIRE(plan.error.empty());
    CHECK(plan.reduced);
    CHECK(plan.total_preview_bytes == 20000);
    for (auto bytes : plan.preview_bytes) CHECK(bytes == 2000);
    CHECK(std::get<api::ToolResultBlock>(source.content.front()).content.size() == 16000);
}

TEST_CASE("batch allocator keeps small results whole and redistributes space") {
    const auto plan = agent::PlanToolBatchBudget(Batch({10, 30000, 30000}), 12010);
    REQUIRE(plan.error.empty());
    CHECK(plan.preview_bytes == std::vector<std::size_t>{10, 6000, 6000});
    CHECK(plan.total_preview_bytes == 12010);
}

TEST_CASE("batch allocation has an explicit minimum failure") {
    const auto source = Batch({32768, 32768});
    const auto plan = agent::PlanToolBatchBudget(source, 4096);
    REQUIRE(plan.error.empty());
    CHECK(plan.preview_bytes == std::vector<std::size_t>{2048, 2048});
    CHECK_FALSE(agent::PlanToolBatchBudget(source, 2047).error.empty());
}

TEST_CASE("closed tool group rejects duplicate missing and foreign results") {
    api::Message calls;
    calls.content.push_back(api::ToolUseBlock{"call-0", "read", nlohmann::json::object()});
    calls.content.push_back(api::ToolUseBlock{"call-1", "read", nlohmann::json::object()});
    auto results = Batch({10, 10});
    CHECK(agent::ToolBatchPairingMatches(calls, results));
    std::get<api::ToolResultBlock>(results.content.back()).tool_use_id = "call-0";
    CHECK_FALSE(agent::ToolBatchPairingMatches(calls, results));
    CHECK_FALSE(agent::PlanToolBatchBudget(results, 100).error.empty());
    results.content.pop_back();
    CHECK_FALSE(agent::ToolBatchPairingMatches(calls, results));
}

TEST_CASE("adapter input snapshot counts one escaped UTF-8 total and omits output controls") {
    const nlohmann::json wire = {
        {"model", "not-input"}, {"max_tokens", 8192}, {"stream", true},
        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "中文\n\"x\""}}})},
        {"tools", nlohmann::json::array()}};
    const auto snapshot = api::ModelInputSnapshotFromWire(wire.dump());
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->contains("max_tokens"));
    CHECK_FALSE(snapshot->contains("model"));
    const auto estimate = hooks::middleware::ComputeUtf8BytesDiv4Estimate(*snapshot);
    const auto bytes = snapshot->dump().size();
    CHECK(estimate.at("inputUtf8Bytes").get<std::size_t>() == bytes);
    CHECK(estimate.at("estimatedInputTokens").get<std::size_t>() == bytes / 4 + (bytes % 4 != 0));
}

TEST_CASE("all adapter input families retain their selected input fields") {
    for (const char* key : {"messages", "input", "contents"}) {
        const auto snapshot = api::ModelInputSnapshotFromWire(nlohmann::json{{key, "input"}, {"instructions", "sys"}}.dump());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->contains(key));
        CHECK(snapshot->at("instructions") == "sys");
    }
    CHECK_FALSE(api::ModelInputSnapshotFromWire("").has_value());
    CHECK_FALSE(api::ModelInputSnapshotFromWire("[]").has_value());
}

TEST_CASE("media and opaque reasoning require a separate capacity policy") {
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"type", "image_url"}, {"image_url", "data:image/png;base64,AAAA"}}));
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"inlineData", {{"data", "AAAA"}}}}));
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"type", "redacted_thinking"}, {"data", "opaque"}}));
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"type", "reasoning"}, {"encrypted_content", "opaque"}}));
    CHECK_FALSE(api::HasUnestimatedInput(nlohmann::json{{"type", "tool_use"}, {"input", {{"type", "image"}}}}));
    CHECK_FALSE(api::HasUnestimatedInput(nlohmann::json{{"tools", {{{"type", "image"}}}}}));
}

// QQBot 静默失败单 P0 刀一:四类内容的分类口径。anthropic wire 第二轮把
// 带 signature 的 thinking 历史回传——签名元数据(防重放必需、体积有界)
// 不再误伤成"不可估算";真媒体/加密思考照旧拒收。
TEST_CASE("signature metadata is measurable and no longer trips the unestimated gate") {
    // 第 2 类:thinking 块带签名(anthropic 续会话回传的协议必需形状)。
    CHECK_FALSE(api::HasUnestimatedInput(
        nlohmann::json{{"type", "thinking"}, {"thinking", "想了一段"}, {"signature", "sig-opaque"}}));
    CHECK_FALSE(api::HasUnestimatedInput(
        nlohmann::json{{"type", "thinking"}, {"thinking", "想了一段"}, {"signature", ""}}));
    CHECK_FALSE(api::HasUnestimatedInput(
        nlohmann::json{{"type", "thinking"}, {"thinking", "想了一段"}, {"signature", nullptr}}));
    // gemini 的 thoughtSignature 同族(适配器现行不回传,口径先钉住)。
    CHECK_FALSE(api::HasUnestimatedInput(
        nlohmann::json{{"text", "正文"}, {"thoughtSignature", "sig-opaque"}}));
    // 豁免不是填 0:字节进估算器的账(估算器数全 JSON 字节)。
    const nlohmann::json snapshot = nlohmann::json{{"messages", nlohmann::json::array(
        {nlohmann::json{{"role", "assistant"}, {"content", nlohmann::json::array(
            {nlohmann::json{{"type", "thinking"}, {"thinking", "想了一段"}, {"signature", "sig-opaque"}}})}}})}};
    const auto diagnosis = api::DiagnoseUnestimatedInput(snapshot);
    CHECK_FALSE(diagnosis.refused());
    CHECK(diagnosis.signature_metadata_fields == 1);
    CHECK(diagnosis.signature_metadata_bytes == std::string("sig-opaque").size());
}

TEST_CASE("oversized or malformed signature fields stay refused") {
    const std::string oversized(api::kSignatureMetadataBudgetBytes + 1, 's');
    CHECK(api::HasUnestimatedInput(
        nlohmann::json{{"type", "thinking"}, {"thinking", "t"}, {"signature", oversized}}));
    CHECK(api::HasUnestimatedInput(
        nlohmann::json{{"type", "thinking"}, {"signature", nlohmann::json::array({"a", "b"})}}));
}

TEST_CASE("unestimated diagnosis reports structure and counts, never values") {
    const nlohmann::json snapshot = nlohmann::json{
        {"messages", nlohmann::json::array({
            nlohmann::json{{"role", "user"}, {"content", "纯文本"}},
            nlohmann::json{{"role", "assistant"}, {"content", nlohmann::json::array({
                nlohmann::json{{"type", "redacted_thinking"}, {"data", "SECRET-BLOB"}},
            })}},
        })},
    };
    const auto diagnosis = api::DiagnoseUnestimatedInput(snapshot);
    REQUIRE(diagnosis.findings.size() == 1);
    CHECK(diagnosis.findings[0].kind == "encrypted_reasoning");
    CHECK(diagnosis.findings[0].block_type == "redacted_thinking");
    CHECK(diagnosis.findings[0].key == "type");
    CHECK(diagnosis.findings[0].path.find("messages[1]") != std::string::npos);
    const std::string summary = diagnosis.Summary();
    CHECK(summary.find("findings=1") != std::string::npos);
    CHECK(summary.find("encrypted_reasoning") != std::string::npos);
    CHECK(summary.find("SECRET-BLOB") == std::string::npos);  // 不记字段值
}
