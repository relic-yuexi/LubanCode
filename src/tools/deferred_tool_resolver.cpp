// deferred_tool_resolver.hpp 的实现:摘要、铸号、解引用、历史重建。

#include "tools/deferred_tool_resolver.hpp"

#include <chrono>
#include <random>
#include <set>
#include <utility>

#include "hooks/hash.hpp"  // Sha256Hex:摘要锚(tools 层已有多处先例)

namespace lubancode::tools {

std::optional<DeferredToolMode> ParseDeferredToolMode(const std::string& text) {
    if (text.empty() || text == "legacy_expand") {
        return DeferredToolMode::LegacyExpand;
    }
    if (text == "disabled") {
        return DeferredToolMode::Disabled;
    }
    if (text == "proxy_reference") {
        return DeferredToolMode::ProxyReference;
    }
    if (text == "native_reference") {
        return DeferredToolMode::NativeReference;
    }
    return std::nullopt;
}

DeferredToolModeResolution ResolveDeferredToolMode(const std::string& configured_text, bool wire_is_anthropic,
                                                   bool catalog_native_declared,
                                                   const std::string& catalog_server_tool_search) {
    // 纯函数(动态工具 P3·§四/§7):配置串 + wire + 目录能力声明 -> 有效模式。
    // native_reference 的两道门(单子红线 2):
    //   1. wire 必须是 anthropic——兼容端(chat/responses/gemini)没有
    //      defer_loading/server tool 的形状,绝不误开;
    //   2. 模型目录必须显式声明 deferred_tools 能力——不按厂名猜,第三方
    //      Anthropic 兼容端点(目录没写)天然不开。
    // 两道门任一不开而配置又点名要 native:落 LegacyExpand(现状路),native_denial
    // 带人话——装配层大声报出来,不悄悄换路(§四"不得悄悄换路")。
    //
    // "auto"(P4·§十三 P4-2/P4-3 的机制半边):能力驱动档,用户把选择权交给
    // 宿主——两道门都开走原生(P4-3"native 成为明确支持模型的默认"的
    // 机制),门不开落 kRecommendedDeferredToolMode(当前 legacy,翻 P4-2
    // 时同笔翻成 proxy)。与"点名 native 被拒"待遇不同:点名的回落是意外,
    // 大声报;auto 的回落是合同行为,静默落(只在落 native 时给一行 mode_note
    // 告知生效档,不吵)。
    if (configured_text == "auto") {
        DeferredToolModeResolution out;
        if (wire_is_anthropic && catalog_native_declared) {
            out.mode = DeferredToolMode::NativeReference;
            out.server_tool_search = catalog_server_tool_search;
            out.mode_note =
                "deferred_tool_mode=auto:模型目录声明 deferred_tools 能力,已走 native_reference(provider "
                "服务端工具搜索 + defer_loading 保前缀);要强制通用路就显式写 proxy_reference。";
        } else {
            out.mode = kRecommendedDeferredToolMode;
        }
        return out;
    }
    const auto configured = ParseDeferredToolMode(configured_text);
    if (!configured.has_value()) {
        // 认不得的值:也落现状,denial 说明配置错在哪。配置层在解析期已拦过
        // 认不得的串,这是装配期的兜底。
        DeferredToolModeResolution out;
        out.mode = DeferredToolMode::LegacyExpand;
        out.native_denial = "deferred_tool_mode 配置值认不得: " + configured_text +
                            "(可用:auto|disabled|proxy_reference|native_reference|legacy_expand);已回落 "
                            "legacy_expand。";
        return out;
    }
    DeferredToolModeResolution out;
    if (*configured != DeferredToolMode::NativeReference) {
        out.mode = *configured;
        return out;
    }
    if (!wire_is_anthropic) {
        out.mode = DeferredToolMode::LegacyExpand;
        out.native_denial =
            "deferred_tool_mode=native_reference 只在 anthropic wire 下可用(defer_loading/服务端工具搜索是 "
            "Anthropic Messages 的原生能力);当前 wire 不认这组形状,已回落 legacy_expand。";
        return out;
    }
    if (!catalog_native_declared) {
        out.mode = DeferredToolMode::LegacyExpand;
        out.native_denial =
            "当前模型在目录里没有声明 deferred_tools 能力(deferred_tools.mode=native_reference)。原生引用"
            "只对目录明确声明的模型启用,不按 provider 名猜——第三方 Anthropic 兼容端点不声明即不开。"
            "已回落 legacy_expand;要开就在 models.json/providers 目录给该模型补声明。";
        return out;
    }
    out.mode = DeferredToolMode::NativeReference;
    out.server_tool_search = catalog_server_tool_search;
    return out;
}

std::string DeferredToolModeName(DeferredToolMode mode) {
    switch (mode) {
        case DeferredToolMode::Disabled: return "disabled";
        case DeferredToolMode::ProxyReference: return "proxy_reference";
        case DeferredToolMode::NativeReference: return "native_reference";
        case DeferredToolMode::LegacyExpand: return "legacy_expand";
    }
    return "legacy_expand";
}

// ---------------------------------------------------------------------------
// DiscoveryLedger
// ---------------------------------------------------------------------------

std::optional<DeferredToolRefRecord> DiscoveryLedger::Find(const std::string& tool_ref) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_ref_.find(tool_ref);
    if (it == by_ref_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<DeferredToolRefRecord> DiscoveryLedger::FindLive(const std::string& canonical_name,
                                                               const std::string& source_instance,
                                                               const std::string& schema_digest) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [ref, record] : by_ref_) {
        if (record.canonical_name == canonical_name && record.source_instance == source_instance &&
            record.schema_digest == schema_digest) {
            return record;
        }
    }
    return std::nullopt;
}

void DiscoveryLedger::Upsert(DeferredToolRefRecord record) {
    std::lock_guard<std::mutex> lock(mutex_);
    by_ref_[record.tool_ref] = std::move(record);
}

std::vector<DeferredToolRefRecord> DiscoveryLedger::Records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeferredToolRefRecord> out;
    out.reserve(by_ref_.size());
    for (const auto& [ref, record] : by_ref_) {
        (void)ref;
        out.push_back(record);
    }
    return out;
}

std::size_t DiscoveryLedger::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_ref_.size();
}

// ---------------------------------------------------------------------------
// DeferredToolResolver
// ---------------------------------------------------------------------------

namespace {

// 实例一次性 nonce:时间熵 + 随机器件 + scope,折进 ref 前缀。重启后新铸的
// ref 不与恢复进来的旧 ref 撞号;跨会话伪拼的 ref 进不了账,直接拒。
std::string MintNonce(const std::string& scope) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    const std::string seed = scope + "|" + std::to_string(now) + "|" + std::to_string(rd()) + "|" +
                             std::to_string(rd());
    const std::string hash = hooks::Sha256Hex(seed);
    return hash.substr(0, 10);
}

std::string SourceInstanceOf(const ToolRegistration* registration) {
    return registration != nullptr ? registration->source_instance : std::string();
}

}  // namespace

DeferredToolResolver::DeferredToolResolver(std::string session_scope)
    : scope_(std::move(session_scope)), nonce_(MintNonce(scope_)) {}

std::string DeferredToolResolver::SchemaDigestOf(const Tool& tool) {
    return hooks::Sha256Hex("v1\n" + tool.input_schema().dump());
}

std::string DeferredToolResolver::CatalogRevisionOf(const ToolRegistry& registry) {
    std::string seed = "catalog-v1";
    for (const auto& tool : registry.All()) {
        if (!tool->deferred()) {
            continue;
        }
        seed += "\n" + tool->name() + "\n" + SourceInstanceOf(registry.RegistrationOf(tool->name())) + "\n" +
                SchemaDigestOf(*tool);
    }
    return hooks::Sha256Hex(seed);
}

DeferredToolRefRecord DeferredToolResolver::Discover(const Tool& tool, const ToolRegistration* registration,
                                                     const std::string& catalog_revision,
                                                     const std::string& discovered_event_id,
                                                     bool schema_expanded) {
    const std::string instance = SourceInstanceOf(registration);
    const std::string digest = SchemaDigestOf(tool);
    {
        // 同 (name, instance, digest) 复用既有 ref:模型手里那枚继续有效,
        // 不因重搜换号。schema_expanded 只朝 true 汇(摊过全文就记摊过)。
        if (auto existing = ledger_.FindLive(tool.name(), instance, digest); existing.has_value()) {
            if (schema_expanded && !existing->schema_expanded) {
                existing->schema_expanded = true;
                ledger_.Upsert(*existing);
            }
            return *existing;
        }
    }
    DeferredToolRefRecord record;
    record.canonical_name = tool.name();
    record.source_instance = instance;
    record.schema_digest = digest;
    record.catalog_revision = catalog_revision;
    record.discovered_event_id = discovered_event_id;
    record.session_scope = scope_;
    record.schema_expanded = schema_expanded;
    {
        std::lock_guard<std::mutex> lock(mint_mutex_);
        record.tool_ref = "dt_" + nonce_ + "_" + std::to_string(++counter_);
    }
    ledger_.Upsert(record);
    return record;
}

std::expected<DeferredToolResolver::Resolution, DeferredToolResolver::Refusal>
DeferredToolResolver::Resolve(const ToolRegistry& registry, const api::ToolUseBlock& wire_call) const {
    // 入参形状先验:tool_ref(字符串) + arguments(对象)。缺字段/坏类型不
    // 算"引用不存在"的语义,但码同走 unknown_tool_ref——模型该做的是重新
    // tool_search 拿一枚新的,不是修这枚坏调用。
    if (!wire_call.input.is_object() || !wire_call.input.contains("tool_ref") ||
        !wire_call.input.at("tool_ref").is_string() || wire_call.input.at("tool_ref").get<std::string>().empty()) {
        return std::unexpected(Refusal{
            kErrToolRefUnknown,
            "tool_invoke 入参须是 {\"tool_ref\": <string>, \"arguments\": <object>}(tool_ref 来自 tool_search "
            "结果)。请重新 tool_search 获取有效的 tool_ref 再调用。"});
    }
    if (!wire_call.input.contains("arguments") || !wire_call.input.at("arguments").is_object()) {
        return std::unexpected(Refusal{
            kErrToolRefUnknown,
            "tool_invoke 的 arguments 须是要传给目标工具的 JSON 对象。请按 tool_search 结果里的 input_schema "
            "重给参数。"});
    }

    const std::string tool_ref = wire_call.input.at("tool_ref").get<std::string>();
    const auto record = ledger_.Find(tool_ref);
    if (!record.has_value()) {
        // 不在账:伪拼、跨会话、跨侧(main 的 ref 进子代理的账)都落在这。
        return std::unexpected(Refusal{
            kErrToolRefUnknown,
            "tool_ref 无效或不属于本会话(代码 " + std::string(kErrToolRefUnknown) +
                "):引用不能自行拼装,也不能跨会话/跨代理使用。请重新 tool_search 获取本会话的 tool_ref。"});
    }

    const Tool* tool = registry.Find(record->canonical_name);
    if (tool == nullptr) {
        // 注册表缺工具:只报 unavailable,不碰悬空指针,不把旧记录抹掉。
        return std::unexpected(Refusal{
            kErrToolRefUnavailable,
            "工具 " + record->canonical_name + " 当前不在注册表里(MCP 断开/插件卸载,代码 " +
                std::string(kErrToolRefUnavailable) + ")。可稍后重试 tool_search,或改用其他工具。"});
    }

    // digest 与 source 任一变了都算 stale:模型看过的那版 schema 已不是当下
    // 这版,按旧参数放行就是绕 schema(单子 §8.3)。
    const std::string digest_now = SchemaDigestOf(*tool);
    const std::string instance_now = SourceInstanceOf(registry.RegistrationOf(record->canonical_name));
    if (digest_now != record->schema_digest || instance_now != record->source_instance) {
        return std::unexpected(Refusal{
            kErrToolRefStale,
            "工具 " + record->canonical_name + " 的定义已变(schema/来源摘要不符,代码 " +
                std::string(kErrToolRefStale) + ")。请重新 tool_search 拿新版 schema 与新的 tool_ref。"});
    }

    Resolution resolution;
    resolution.target_name = record->canonical_name;
    resolution.arguments = wire_call.input.at("arguments");
    resolution.tool_ref = record->tool_ref;
    resolution.schema_digest = record->schema_digest;
    resolution.canonical_name = record->canonical_name;
    resolution.source_instance = instance_now;
    return resolution;
}

std::size_t DeferredToolResolver::RebuildFromHistory(const std::vector<api::Message>& history) {
    // 只信正式 discovery event:assistant 消息里 name=="tool_search" 的
    // tool_use 块的 id,配后续 user 消息里同 id 的 tool_result。自由文本、
    // 无配对的 result、JSON 坏行一概不认。
    std::set<std::string> search_call_ids;
    for (const auto& message : history) {
        if (message.role != api::Role::Assistant) {
            continue;
        }
        for (const auto& block : message.content) {
            if (const auto* call = std::get_if<api::ToolUseBlock>(&block);
                call != nullptr && call->name == "tool_search") {
                search_call_ids.insert(call->id);
            }
        }
    }
    if (search_call_ids.empty()) {
        return 0;
    }

    std::size_t adopted = 0;
    for (const auto& message : history) {
        if (message.role != api::Role::User) {
            continue;
        }
        for (const auto& block : message.content) {
            const auto* result = std::get_if<api::ToolResultBlock>(&block);
            if (result == nullptr || result->is_error || result->content.empty() ||
                search_call_ids.count(result->tool_use_id) == 0) {
                continue;
            }
            // 解析结构化结果(形状见 ToolSearchTool 的 proxy 输出)。单项坏
            // 跳过,不连累整册。
            nlohmann::json parsed = nlohmann::json::parse(result->content, nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_object() || !parsed.contains("matches") || !parsed["matches"].is_array()) {
                continue;
            }
            for (const auto& match : parsed["matches"]) {
                if (!match.is_object() || !match.contains("tool_ref") || !match["tool_ref"].is_string() ||
                    !match.contains("name") || !match["name"].is_string()) {
                    continue;
                }
                // schema_too_large 的条目没有 tool_ref 可用的完整发现,不含
                // tool_ref 字段,上面的形状检查自然跳过。
                DeferredToolRefRecord record;
                record.tool_ref = match["tool_ref"].get<std::string>();
                record.canonical_name = match["name"].get<std::string>();
                if (match.contains("source_instance") && match["source_instance"].is_string()) {
                    record.source_instance = match["source_instance"].get<std::string>();
                }
                if (match.contains("schema_digest") && match["schema_digest"].is_string()) {
                    record.schema_digest = match["schema_digest"].get<std::string>();
                }
                if (parsed.contains("catalog_revision") && parsed["catalog_revision"].is_string()) {
                    record.catalog_revision = parsed["catalog_revision"].get<std::string>();
                }
                record.discovered_event_id = result->tool_use_id;
                record.session_scope = scope_;
                record.schema_expanded = true;  // 历史里那一份摊过全文
                if (record.tool_ref.empty() || record.canonical_name.empty() || record.schema_digest.empty()) {
                    continue;  // 缺关键凭据的半行不收
                }
                // 只补缺不覆盖:活账(本轮会话已铸的)优先。
                if (ledger_.Find(record.tool_ref).has_value()) {
                    continue;
                }
                ledger_.Upsert(std::move(record));
                ++adopted;
            }
        }
    }
    return adopted;
}

}  // namespace lubancode::tools
