#include "agent/resolved_prompt_builder.hpp"

#include <algorithm>
#include <string_view>

#include "agent/context.hpp"  // EstimateUtf8Tokens
#include "agent/prompts.hpp"  // ModelInstructionsSegment/SoulSegment/StripPromptComments/With*
#include "hooks/hash.hpp"     // Sha256Hex

namespace lubancode::agent {

namespace {

// PromptModuleOrigin -> PromptSegment.source_kind(§6.4 六选一)。
std::string SourceKindOf(PromptModuleOrigin origin) {
    switch (origin) {
        case PromptModuleOrigin::EmbeddedDefault:
        case PromptModuleOrigin::EmbeddedHostPolicy:
            return "embedded";
        case PromptModuleOrigin::UserDefault:
            return "user_prompt_dir";
        case PromptModuleOrigin::EmbeddedProfile:
        case PromptModuleOrigin::UserProfile:
        case PromptModuleOrigin::ProjectProfile:
            return "prompt_profile";
        case PromptModuleOrigin::PackageProfile:
            return "package";
        case PromptModuleOrigin::Persona:
            return "agent_inline";
        case PromptModuleOrigin::RuntimeEnvironment:
        case PromptModuleOrigin::ProjectInstructions:
            return "host_generated";
    }
    return "host_generated";
}

// rel_path 前缀 -> PromptSegment.role(粗分,人看的分组,不是强合同)。
std::string RoleOf(const std::string& rel_path) {
    if (rel_path.rfind("core/", 0) == 0 || rel_path == "core/*") return "core";
    if (rel_path.rfind("features/", 0) == 0) return "features";
    if (rel_path.rfind("platforms/", 0) == 0) return "platform";
    if (rel_path.rfind("modes/", 0) == 0) return "runtime";
    if (rel_path == "(runtime environment)") return "runtime";
    if (rel_path == "(project instructions)") return "context";
    return "other";
}

std::string SourceRefOf(const PromptSourceLedgerEntry& entry) {
    if (!entry.file.empty()) {
        return "file:" + entry.file;
    }
    switch (entry.origin) {
        case PromptModuleOrigin::Persona:
            return "inline:persona";
        case PromptModuleOrigin::RuntimeEnvironment:
            return "runtime:environment";
        case PromptModuleOrigin::ProjectInstructions:
            return "runtime:project_instructions";
        default:
            return "embedded:" + entry.rel_path;
    }
}

PromptSegment ToSegment(const PromptSourceLedgerEntry& entry) {
    PromptSegment segment;
    segment.segment_id = entry.rel_path;
    segment.role = RoleOf(entry.rel_path);
    segment.source_kind = SourceKindOf(entry.origin);
    segment.source_ref = SourceRefOf(entry);
    if (!entry.file.empty()) {
        // 磁盘来源:渲染内容即文件正文,未经二次变换,source_hash 与
        // rendered_hash 同值——两个字段分开写是给"渲染有变换"的将来留口,
        // 现在如实填同一份。
        segment.source_hash = entry.content_hash;
    }
    segment.rendered_hash = entry.content_hash;
    segment.rendered_tokens_estimated = entry.content_tokens_estimated;
    segment.order = entry.order;
    // 运行环境段现填(工作目录/日期/系统),内容随宿主当下状态变,标
    // volatile——§6.4 示例 JSON 的 runtime/environment 就是这个口径。
    segment.volatile_segment = entry.origin == PromptModuleOrigin::RuntimeEnvironment;
    segment.overrides = entry.overrides;
    return segment;
}

}  // namespace

ResolvedPromptBase BuildResolvedPromptBase(const PromptOptions& options) {
    ResolvedPromptBase base;
    base.text = AssembleSystemPrompt(options, &base.ledger);
    return base;
}

AssembledPrompt ResolveFinalPrompt(const ResolvedPromptBase& base, const std::string& deferred_index_segment,
                                   const std::string& model_instructions, const std::string& soul_content,
                                   const std::string& soul_name) {
    AssembledPrompt out;

    PromptManifest manifest;
    manifest.assembly_version = base.assembly_version;
    manifest.layers = {"embedded", "user_prompt_dir", "package", "prompt_profile", "agent_inline"};

    int next_order = 0;
    for (const PromptSourceLedgerEntry& entry : base.ledger.entries) {
        manifest.segments.push_back(ToSegment(entry));
        next_order = std::max(next_order, entry.order + 1);
    }

    // 三层后叠(次序与 agent/loop.cpp 原三行一字不差:延迟工具索引 ->
    // 模型目录指令 -> 魂)。With* 本身内部已判空,这里镜像同一判空逻辑
    // 只为决定"要不要记一条 manifest",文本拼装仍走 With* 本尊,不重复
    // 造一遍拼接规则。
    std::string text = base.text;
    text = WithDeferredToolsIndex(text, deferred_index_segment);
    if (!deferred_index_segment.empty()) {
        PromptSegment segment;
        segment.segment_id = "runtime/deferred_tool_index";
        segment.role = "tools";
        segment.source_kind = "host_generated";
        segment.source_ref = "runtime:deferred_tool_index";
        segment.rendered_hash = hooks::Sha256Hex(deferred_index_segment);
        segment.rendered_tokens_estimated =
            static_cast<std::int64_t>(EstimateUtf8Tokens(deferred_index_segment));
        segment.order = next_order++;
        // 随 tool_search 命中集合逐请求变化(§6.4"动态段插中间"的那类
        // 段——这里恒殿后,不插中间,但内容本身仍逐请求变,标 volatile)。
        segment.volatile_segment = true;
        manifest.segments.push_back(std::move(segment));
    }

    text = WithModelInstructions(text, model_instructions);
    if (!model_instructions.empty()) {
        const std::string rendered = ModelInstructionsSegment(model_instructions);
        manifest.model_instructions.hash = hooks::Sha256Hex(rendered);
        manifest.model_instructions.tokens_estimated = static_cast<std::int64_t>(EstimateUtf8Tokens(rendered));
    }

    text = WithSoul(text, soul_content);
    manifest.soul.name = soul_name.empty() ? "default" : soul_name;
    const std::string stripped_soul = StripPromptComments(soul_content);
    if (!stripped_soul.empty()) {
        const std::string rendered = SoulSegment(stripped_soul);
        manifest.soul.hash = hooks::Sha256Hex(rendered);
        manifest.soul.tokens_estimated = static_cast<std::int64_t>(EstimateUtf8Tokens(rendered));
    }

    manifest.resolved_prompt_hash = hooks::Sha256Hex(text);
    manifest.resolved_prompt_tokens_estimated = static_cast<std::int64_t>(EstimateUtf8Tokens(text));

    // 稳定前缀 hash:非 volatile 段(按 order)的 rendered_hash 链——只有
    // 稳定前缀因内容变化才断,运行环境/延迟索引这类逐请求变的段不参与,
    // 与 cache 前缀记账同一条道理(易断 cache 的段本就不该算进"稳定")。
    std::vector<PromptSegment> ordered = manifest.segments;
    std::sort(ordered.begin(), ordered.end(),
             [](const PromptSegment& a, const PromptSegment& b) { return a.order < b.order; });
    std::string stable_chain;
    for (const PromptSegment& segment : ordered) {
        if (segment.volatile_segment) continue;
        stable_chain += segment.rendered_hash;
        stable_chain += '|';
    }
    manifest.stable_prefix_hash = stable_chain.empty() ? std::string() : hooks::Sha256Hex(stable_chain);

    out.text = std::move(text);
    out.manifest = std::move(manifest);
    return out;
}

}  // namespace lubancode::agent
