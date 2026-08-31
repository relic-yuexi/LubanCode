// ResolvedPromptBuilder(Token 账本单 A1)的接线测试:同一份 PromptOptions
// 下,ResolveFinalPrompt 产出的最终文本与 agent/loop.cpp 原三行后叠
//(WithDeferredToolsIndex -> WithModelInstructions -> WithSoul)逐字节一致;
// manifest 的段账/顶层字段/稳定前缀 hash 从这次真实拼装现场产出,不是
// analyzer 事后拆字符串(§6.4)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "agent/prompt_assembler.hpp"
#include "agent/prompt_manifest.hpp"
#include "agent/prompts.hpp"
#include "agent/resolved_prompt_builder.hpp"
#include "hooks/hash.hpp"

using namespace lubancode;
using namespace lubancode::agent;

namespace {

PromptOptions BaseOptions() {
    PromptOptions options;
    options.cwd = "D:/demo";
    options.current_date = "2026-08-31";
    options.skills_segment = "- demo_skill: 演示技能";
    return options;
}

}  // namespace

TEST_CASE("ResolveFinalPrompt 文本与原三行后叠逐字节一致") {
    const PromptOptions options = BaseOptions();
    const ResolvedPromptBase base = BuildResolvedPromptBase(options);
    // 旧路:裸 AssembleSystemPrompt + loop.cpp 原三行。
    PromptSourceLedger scratch_ledger;
    const std::string legacy_text =
        WithSoul(WithModelInstructions(WithDeferredToolsIndex(AssembleSystemPrompt(options, &scratch_ledger),
                                                              "延迟工具索引一枚"),
                                       "模型目录指令一枚"),
                 "魂一段");
    // 新路:base + 同三层,经 ResolveFinalPrompt。
    const AssembledPrompt assembled =
        ResolveFinalPrompt(base, "延迟工具索引一枚", "模型目录指令一枚", "魂一段", "custom");
    CHECK(assembled.text == legacy_text);
    CHECK(base.text == AssembleSystemPrompt(options, nullptr));
}

TEST_CASE("manifest 段账从拼装现场产出:ledger 段齐、三层后叠各有账") {
    const PromptOptions options = BaseOptions();
    const ResolvedPromptBase base = BuildResolvedPromptBase(options);
    REQUIRE_FALSE(base.ledger.entries.empty());
    const AssembledPrompt assembled =
        ResolveFinalPrompt(base, "延迟工具索引一枚", "模型目录指令一枚", "魂一段", "custom");
    const PromptManifest& manifest = assembled.manifest;

    CHECK(manifest.assembly_version == "prompt-assembler-v1");
    CHECK(manifest.resolved_prompt_hash == hooks::Sha256Hex(assembled.text));
    CHECK(manifest.resolved_prompt_tokens_estimated > 0);
    // ledger 的每一段都进了 manifest,order 单调。
    REQUIRE(manifest.segments.size() == base.ledger.entries.size() + 1);  // + 延迟索引段
    bool seen_deferred = false;
    int previous_order = -1;
    for (const auto& segment : manifest.segments) {
        CHECK(segment.order > previous_order);
        previous_order = segment.order;
        CHECK_FALSE(segment.rendered_hash.empty());
        if (segment.segment_id == "runtime/deferred_tool_index") {
            seen_deferred = true;
            CHECK(segment.source_kind == "host_generated");
            CHECK(segment.volatile_segment);
        }
    }
    CHECK(seen_deferred);
    // ledger 段的 content_hash/order 与 manifest 一一对应(同一次拼装产出)。
    const PromptSegment& first = manifest.segments.front();
    CHECK(first.rendered_hash == base.ledger.entries.front().content_hash);
    CHECK(first.order == base.ledger.entries.front().order);

    // soul 与 model instructions 走顶层字段(§6.4 schema)。
    CHECK(manifest.soul.name == "custom");
    CHECK_FALSE(manifest.soul.hash.empty());
    CHECK(manifest.soul.tokens_estimated > 0);
    CHECK_FALSE(manifest.model_instructions.hash.empty());
    CHECK(manifest.model_instructions.tokens_estimated > 0);
    // resolved hash 是全文的 hash:去掉 soul 段就不再相等(证明真算了)。
    const AssembledPrompt no_soul =
        ResolveFinalPrompt(base, "延迟工具索引一枚", "模型目录指令一枚", "", "");
    CHECK(no_soul.manifest.soul.name == "default");
    CHECK(no_soul.manifest.soul.hash.empty());
    CHECK(no_soul.manifest.resolved_prompt_hash != manifest.resolved_prompt_hash);
}

TEST_CASE("稳定前缀 hash 只吃非 volatile 段:运行环境一改就断,延迟索引不动它") {
    const PromptOptions options = BaseOptions();
    const ResolvedPromptBase base = BuildResolvedPromptBase(options);
    const AssembledPrompt with_index_a =
        ResolveFinalPrompt(base, "延迟工具索引 AAA", "", "", "");
    const AssembledPrompt with_index_b =
        ResolveFinalPrompt(base, "延迟工具索引 BBB", "", "", "");
    // 延迟索引逐请求变(volatile),不进稳定前缀:两份的 stable hash 相同。
    CHECK(with_index_a.manifest.stable_prefix_hash == with_index_b.manifest.stable_prefix_hash);
    CHECK_FALSE(with_index_a.manifest.stable_prefix_hash.empty());

    // 运行环境段(cwd/日期,volatile 同样不进);换 cwd 重拼 base,非
    // volatile 段(core 等)不变,稳定前缀也不变;动 persona(整段换 core)
    // 才断。
    PromptOptions other_cwd = BaseOptions();
    other_cwd.cwd = "D:/elsewhere";
    const ResolvedPromptBase base_other = BuildResolvedPromptBase(other_cwd);
    const AssembledPrompt assembled_other = ResolveFinalPrompt(base_other, "", "", "", "");
    CHECK(assembled_other.manifest.stable_prefix_hash == with_index_a.manifest.stable_prefix_hash);

    PromptOptions persona_options = BaseOptions();
    persona_options.persona = "你是另一位人格。";
    const ResolvedPromptBase base_persona = BuildResolvedPromptBase(persona_options);
    const AssembledPrompt assembled_persona = ResolveFinalPrompt(base_persona, "", "", "", "");
    CHECK(assembled_persona.manifest.stable_prefix_hash != with_index_a.manifest.stable_prefix_hash);
}

TEST_CASE("ledger 段级 hash/token/order 在 AssembleSystemPrompt 现场记") {
    PromptSourceLedger ledger;
    const PromptOptions options = BaseOptions();
    (void)AssembleSystemPrompt(options, &ledger);
    REQUIRE_FALSE(ledger.entries.empty());
    int previous_order = -1;
    for (const auto& entry : ledger.entries) {
        // content_hash 是这段渲染正文的 SHA-256(64 位十六进制)。
        CHECK(entry.content_hash.size() == 64);
        CHECK(entry.order > previous_order);
        previous_order = entry.order;
        // 嵌入默认段没被覆盖:overrides 恒空;解析现场如实记,不猜。
        if (entry.origin == PromptModuleOrigin::EmbeddedDefault) {
            CHECK(entry.overrides.empty());
        }
    }
    // 用户目录里放一份覆盖模块:overrides 记"被压掉的上一层"。
    // (写到临时目录,清掉再来,不碰用户真目录。)
    const auto prompts_dir = std::filesystem::temp_directory_path() / "lubancode-a1-ledger-prompts";
    std::error_code ec;
    std::filesystem::remove_all(prompts_dir, ec);
    std::filesystem::create_directories(prompts_dir / "core", ec);
    {
        std::ofstream out(prompts_dir / "core" / "10-identity.md", std::ios::binary);
        out << "用户自己的身份段。";
    }
    PromptOptions user_options = BaseOptions();
    user_options.prompts_dir = prompts_dir.generic_string();
    PromptSourceLedger user_ledger;
    (void)AssembleSystemPrompt(user_options, &user_ledger);
    bool seen_override = false;
    for (const auto& entry : user_ledger.entries) {
        if (entry.origin == PromptModuleOrigin::UserDefault) {
            seen_override = true;
            CHECK_FALSE(entry.overrides.empty());
        }
    }
    CHECK(seen_override);
    std::filesystem::remove_all(prompts_dir, ec);
}
