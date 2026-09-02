// DeferredToolResolver/DiscoveryLedger(动态工具 PromptCache 守恒单 P1):
// 铸 ref、验 ref、恢复账的合同。§5.5/§9/§12.1 的条目在这钉——发现不等于
// 授权(解析只答"看过谁、哪版 schema"),跨会话/跨侧引用、schema 漂移、
// 工具消失各有稳定拒绝码;恢复只认正式 discovery event。

#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "api/types.hpp"
#include "tools/deferred_tool_resolver.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_search.hpp"

using namespace lubancode;

namespace {

// schema 可变的假延迟工具:漂移用例要它中途换 schema。
class MutableDeferredTool : public tools::Tool {
public:
    explicit MutableDeferredTool(std::string name) : name_(std::move(name)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "mutable deferred tool for resolver tests"; }
    nlohmann::json input_schema() const override { return schema_; }
    bool deferred() const override { return true; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"ok", false}; }

    void set_schema(nlohmann::json schema) { schema_ = std::move(schema); }

private:
    std::string name_;
    nlohmann::json schema_ = nlohmann::json{{"type", "object"}, {"properties", {{"q", {{"type", "string"}}}}}};
};

// 注册一枚带来源账的延迟工具(source_instance 给 github,漂移用例靠它换)。
tools::ToolRegistry RegistryWith(const std::string& name, const std::string& instance) {
    tools::ToolRegistry registry;
    tools::ToolRegistration registration;
    registration.tool = std::make_unique<MutableDeferredTool>(name);
    registration.source_kind = tools::ToolSourceKind::Mcp;
    registration.source_instance = instance;
    registry.Register(std::move(registration));
    return registry;
}

api::ToolUseBlock InvokeCall(const std::string& ref, nlohmann::json arguments = nlohmann::json::object()) {
    // 调用点写 {} 会折成 null(不是空对象),先归一——Resolve 的形状门
    // 认的是 object。
    if (arguments.is_null()) {
        arguments = nlohmann::json::object();
    }
    return api::ToolUseBlock{"call_1", "tool_invoke",
                             nlohmann::json{{"tool_ref", ref}, {"arguments", std::move(arguments)}}};
}

api::Message AssistantToolUse(const std::string& id, const std::string& name, const std::string& input_json) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(
        api::ToolUseBlock{id, name, nlohmann::json::parse(input_json, nullptr, /*allow_exceptions=*/false)});
    return message;
}

api::Message UserToolResult(const std::string& id, const std::string& content, bool is_error = false) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::ToolResultBlock{id, content, is_error});
    return message;
}

}  // namespace

// ---------------------------------------------------------------------------
// 模式解析(§四)
// ---------------------------------------------------------------------------

TEST_CASE("DeferredToolMode: 配置词与枚举对上(P3 起 native 放行)") {
    // 2026-09-03 切默认(P4-2):空串 = 未配置,不再钉死 legacy——回 nullopt,
    // 档位仲裁归 ResolveDeferredToolMode(空串与 auto 同待遇,见下一册)与
    // 直构路(落 kRecommendedDeferredToolMode)。
    CHECK(tools::ParseDeferredToolMode("") == std::nullopt);
    CHECK(tools::ParseDeferredToolMode("legacy_expand") == tools::DeferredToolMode::LegacyExpand);
    CHECK(tools::ParseDeferredToolMode("disabled") == tools::DeferredToolMode::Disabled);
    CHECK(tools::ParseDeferredToolMode("proxy_reference") == tools::DeferredToolMode::ProxyReference);
    // P3:native_reference 放行——配置层收下,生效过装配期两道门。
    CHECK(tools::ParseDeferredToolMode("native_reference") == tools::DeferredToolMode::NativeReference);
    // P4:"auto" 是解析策略不是一档模式,不从 Parse 出(由 ResolveDeferred
    // ToolMode 单独判,见下一册)——这里钉着它不悄悄变成哪一档。
    CHECK(tools::ParseDeferredToolMode("auto") == std::nullopt);
    CHECK(tools::ParseDeferredToolMode("垃圾值") == std::nullopt);
    CHECK(tools::DeferredToolModeName(tools::DeferredToolMode::ProxyReference) == "proxy_reference");
    CHECK(tools::DeferredToolModeName(tools::DeferredToolMode::LegacyExpand) == "legacy_expand");
    CHECK(tools::DeferredToolModeName(tools::DeferredToolMode::Disabled) == "disabled");
    CHECK(tools::DeferredToolModeName(tools::DeferredToolMode::NativeReference) == "native_reference");
}

// ---------------------------------------------------------------------------
// 有效模式判定(P3·§四/§七:wire + 目录能力两道门,兼容端不得误开)
// ---------------------------------------------------------------------------

TEST_CASE("ResolveDeferredToolMode: 两道门全开才 native,门不开大声回落") {
    // 门全开:配置 native + anthropic wire + 目录声明 -> native + 变体递进。
    const auto native = tools::ResolveDeferredToolMode("native_reference", /*wire_is_anthropic=*/true,
                                                        /*catalog_native_declared=*/true, "regex");
    CHECK(native.mode == tools::DeferredToolMode::NativeReference);
    CHECK(native.server_tool_search == "regex");
    CHECK(native.native_denial.empty());

    // 门一(非 anthropic wire):回落 legacy,denial 点名 wire。
    const auto wrong_wire = tools::ResolveDeferredToolMode("native_reference", /*wire_is_anthropic=*/false,
                                                           /*catalog_native_declared=*/true, "regex");
    CHECK(wrong_wire.mode == tools::DeferredToolMode::LegacyExpand);
    REQUIRE_FALSE(wrong_wire.native_denial.empty());
    CHECK(wrong_wire.native_denial.find("anthropic wire") != std::string::npos);

    // 门二(目录没声明):回落 legacy,denial 点名目录声明——第三方 Anthropic
    // 兼容端点(不声明)天然不开,不按厂名猜。
    const auto no_catalog = tools::ResolveDeferredToolMode("native_reference", /*wire_is_anthropic=*/true,
                                                           /*catalog_native_declared=*/false, "");
    CHECK(no_catalog.mode == tools::DeferredToolMode::LegacyExpand);
    REQUIRE_FALSE(no_catalog.native_denial.empty());
    CHECK(no_catalog.native_denial.find("deferred_tools") != std::string::npos);

    // 目录声明了但配置没点名 native:2026-09-03 切默认后空串与 auto 同
    // 待遇——两道门都开就走 native(P4-3 的默认半边),不再钉死 legacy。
    const auto not_requested =
        tools::ResolveDeferredToolMode("", /*wire_is_anthropic=*/true, /*catalog_native_declared=*/true, "regex");
    CHECK(not_requested.mode == tools::DeferredToolMode::NativeReference);
    CHECK(not_requested.native_denial.empty());
    // 未配置且门不开(如 openai 线):落推荐档 proxy_reference(2026-09-03
    // 真机质量对照过门后翻转,P4-2)。
    const auto unset_fallback =
        tools::ResolveDeferredToolMode("", /*wire_is_anthropic=*/false, /*catalog_native_declared=*/true, "regex");
    CHECK(unset_fallback.mode == tools::DeferredToolMode::ProxyReference);
    CHECK(unset_fallback.native_denial.empty());

    // 非三值的旧档照旧,不受两道门影响。
    const auto proxy = tools::ResolveDeferredToolMode("proxy_reference", /*wire_is_anthropic=*/false,
                                                      /*catalog_native_declared=*/false, "");
    CHECK(proxy.mode == tools::DeferredToolMode::ProxyReference);
    CHECK(proxy.native_denial.empty());
}

// ---------------------------------------------------------------------------
// "auto" 档(动态工具 P4·§十三 P4-2/P4-3 的机制半边):能力驱动——两道门
// 都开走 native(P4-3"native 成为明确支持模型的默认"的机制),门不开落
// 宿主推荐档(kRecommendedDeferredToolMode,2026-09-03 真机 §12.5 质量
// 对照过门后已翻 proxy,证据 eval/deferred_quality/report.md;空串与
// auto 同待遇)。与"点名 native 被拒"的待遇不同:点名的回落是意外要大声
// 报,auto 的回落是合同行为静默落,只在落 native 时给一行 mode_note。
// ---------------------------------------------------------------------------
TEST_CASE("ResolveDeferredToolMode: auto 档能力驱动——门开走原生带说明,门不开静默落推荐档") {
    // 门全开:auto -> native + 变体递进 + mode_note 告知生效档与退路。
    const auto native = tools::ResolveDeferredToolMode("auto", /*wire_is_anthropic=*/true,
                                                       /*catalog_native_declared=*/true, "bm25");
    CHECK(native.mode == tools::DeferredToolMode::NativeReference);
    CHECK(native.server_tool_search == "bm25");
    CHECK(native.native_denial.empty());
    REQUIRE_FALSE(native.mode_note.empty());
    CHECK(native.mode_note.find("native_reference") != std::string::npos);
    CHECK(native.mode_note.find("proxy_reference") != std::string::npos);  // 指明强制通用路的退路

    // 门一不开(非 anthropic wire):静默落推荐档,不响 denial 也不响 note
    // ——auto 的回落是合同行为,不是意外。
    const auto wrong_wire =
        tools::ResolveDeferredToolMode("auto", /*wire_is_anthropic=*/false, /*catalog_native_declared=*/true, "regex");
    CHECK(wrong_wire.mode == tools::kRecommendedDeferredToolMode);
    CHECK(wrong_wire.mode == tools::DeferredToolMode::ProxyReference);  // 2026-09-03 切默认:推荐档 = proxy(P4-2 过门)
    CHECK(wrong_wire.native_denial.empty());
    CHECK(wrong_wire.mode_note.empty());

    // 门二不开(目录没声明,第三方兼容端点):同上静默落推荐档。
    const auto no_catalog =
        tools::ResolveDeferredToolMode("auto", /*wire_is_anthropic=*/true, /*catalog_native_declared=*/false, "");
    CHECK(no_catalog.mode == tools::kRecommendedDeferredToolMode);
    CHECK(no_catalog.native_denial.empty());
    CHECK(no_catalog.mode_note.empty());

    // 对照:点名 native 门不开仍是大声 denial(P3 既有合同,auto 不洗白它)。
    const auto named = tools::ResolveDeferredToolMode("native_reference", /*wire_is_anthropic=*/false,
                                                       /*catalog_native_declared=*/true, "");
    CHECK(named.mode == tools::DeferredToolMode::LegacyExpand);
    CHECK_FALSE(named.native_denial.empty());
}

// ---------------------------------------------------------------------------
// 铸 ref 与解引用(§5.3/§5.5)
// ---------------------------------------------------------------------------

TEST_CASE("Resolver: 发现铸 ref,同工具同 digest 复用同一枚") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    tools::DeferredToolResolver resolver("main");
    auto& tool = *registry.All().front();

    const auto first = resolver.Discover(tool, registry.RegistrationOf(tool.name()), "rev-1", "evt-1", true);
    const auto second = resolver.Discover(tool, registry.RegistrationOf(tool.name()), "rev-1", "evt-2", true);
    CHECK(first.tool_ref == second.tool_ref);
    CHECK(first.tool_ref.rfind("dt_", 0) == 0);
    CHECK(first.canonical_name == "mcp__github__search_issues");
    CHECK(first.source_instance == "github");
    CHECK(first.session_scope == "main");
    CHECK(first.schema_expanded);
    CHECK(resolver.ledger().Size() == 1);
}

TEST_CASE("Resolver: 解引用拿到真名与 arguments,ref 不等于执行") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    tools::DeferredToolResolver resolver("main");
    auto& tool = *registry.All().front();
    const auto record =
        resolver.Discover(tool, registry.RegistrationOf(tool.name()), "rev-1", "evt-1", true);

    const auto resolved = resolver.Resolve(registry, InvokeCall(record.tool_ref, {{"q", "cache"}}));
    REQUIRE(resolved.has_value());
    CHECK(resolved->target_name == "mcp__github__search_issues");
    CHECK(resolved->arguments == nlohmann::json{{"q", "cache"}});
    CHECK(resolved->tool_ref == record.tool_ref);
    CHECK(resolved->schema_digest == record.schema_digest);
}

TEST_CASE("Resolver: 伪拼/缺字段/跨侧的 ref 都报 unknown_tool_ref") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    tools::DeferredToolResolver main_resolver("main");
    tools::DeferredToolResolver sub_resolver("sub");
    auto& tool = *registry.All().front();
    const auto record =
        main_resolver.Discover(tool, registry.RegistrationOf(tool.name()), "rev-1", "evt-1", true);

    // 伪拼(模型自己造的号)。
    const auto fabricated = main_resolver.Resolve(registry, InvokeCall("dt_deadbeef_999", {}));
    REQUIRE(!fabricated.has_value());
    CHECK(fabricated.error().code == tools::kErrToolRefUnknown);

    // 缺字段/坏类型:同样指路重新 tool_search。
    const auto no_ref = main_resolver.Resolve(registry, api::ToolUseBlock{"c", "tool_invoke", nlohmann::json{{"arguments", nlohmann::json::object()}}});
    REQUIRE(!no_ref.has_value());
    CHECK(no_ref.error().code == tools::kErrToolRefUnknown);
    const auto bad_args = main_resolver.Resolve(
        registry, api::ToolUseBlock{"c", "tool_invoke", nlohmann::json{{"tool_ref", record.tool_ref}}});
    REQUIRE(!bad_args.has_value());
    CHECK(bad_args.error().code == tools::kErrToolRefUnknown);

    // 跨侧:main 铸的 ref 进 sub 的账就是 unknown——父亲的 ref 不给儿子当
    // 通行牌(§5.5;子代理侧要用自己的 tool_search 重发现)。
    const auto cross = sub_resolver.Resolve(registry, InvokeCall(record.tool_ref, {}));
    REQUIRE(!cross.has_value());
    CHECK(cross.error().code == tools::kErrToolRefUnknown);
}

TEST_CASE("Resolver: schema 漂移或来源实例变了报 stale_tool_ref") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    tools::DeferredToolResolver resolver("main");
    auto* tool = static_cast<MutableDeferredTool*>(registry.All().front().get());
    const auto record =
        resolver.Discover(*tool, registry.RegistrationOf(tool->name()), "rev-1", "evt-1", true);

    // schema 变了:digest 不符,stale,不许按旧参数放行(§8.3)。
    tool->set_schema(nlohmann::json{{"type", "object"}, {"properties", {{"别的", {{"type", "number"}}}}}});
    const auto stale = resolver.Resolve(registry, InvokeCall(record.tool_ref, {}));
    REQUIRE(!stale.has_value());
    CHECK(stale.error().code == tools::kErrToolRefStale);

    // 恢复原 schema,换来源实例(MCP 换 server):同样 stale。
    tools::ToolRegistry other_instance = RegistryWith("mcp__github__search_issues", "github-mirror");
    const auto moved = resolver.Resolve(other_instance, InvokeCall(record.tool_ref, {}));
    REQUIRE(!moved.has_value());
    CHECK(moved.error().code == tools::kErrToolRefStale);
}

TEST_CASE("Resolver: 注册表缺工具只报 tool_unavailable,不留悬空指针") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    tools::DeferredToolResolver resolver("main");
    auto& tool = *registry.All().front();
    const auto record =
        resolver.Discover(tool, registry.RegistrationOf(tool.name()), "rev-1", "evt-1", true);

    tools::ToolRegistry empty_registry;  // MCP 断开/插件卸载后的空表
    const auto unavailable = empty_registry.Find("mcp__github__search_issues");
    CHECK(unavailable == nullptr);
    const auto refused = resolver.Resolve(empty_registry, InvokeCall(record.tool_ref, {}));
    REQUIRE(!refused.has_value());
    CHECK(refused.error().code == tools::kErrToolRefUnavailable);
    // 账留着可审计(§9.2:记录保持可审计,标 unavailable/stale)。
    CHECK(resolver.ledger().Size() == 1);
}

TEST_CASE("Resolver: catalog revision 随延迟工具集变,eager 工具不掺和") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    const std::string rev1 = tools::DeferredToolResolver::CatalogRevisionOf(registry);
    CHECK(tools::DeferredToolResolver::CatalogRevisionOf(registry) == rev1);  // 同表稳定
    registry.Register(std::make_unique<MutableDeferredTool>("mcp__github__get_issue"));
    const std::string rev2 = tools::DeferredToolResolver::CatalogRevisionOf(registry);
    CHECK(rev2 != rev1);
    registry.Register(std::make_unique<tools::ToolSearchTool>(  // eager 工具不进账
        registry, std::make_shared<std::set<std::string>>()));
    CHECK(tools::DeferredToolResolver::CatalogRevisionOf(registry) == rev2);
}

// ---------------------------------------------------------------------------
// 恢复账(§9.2):只认正式 discovery event
// ---------------------------------------------------------------------------

TEST_CASE("Resolver: RebuildFromHistory 从 tool_search 的成对 tool_result 重建") {
    tools::ToolRegistry registry = RegistryWith("mcp__github__search_issues", "github");
    tools::DeferredToolResolver resolver("main");
    auto& tool = *registry.All().front();
    const auto record =
        resolver.Discover(tool, registry.RegistrationOf(tool.name()), "rev-1", "evt-9", true);
    const std::string result_json = nlohmann::json{
        {"catalog_revision", "rev-1"},
        {"matches", nlohmann::json::array({nlohmann::json{{"tool_ref", record.tool_ref},
                                                           {"name", "mcp__github__search_issues"},
                                                           {"schema_digest", record.schema_digest},
                                                           {"source_instance", "github"},
                                                           {"input_schema", tool.input_schema()}}})},
    }.dump();

    tools::DeferredToolResolver restored("main");
    std::vector<api::Message> history;
    history.push_back(AssistantToolUse("evt-9", "tool_search", R"({"query":"github issue"})"));
    history.push_back(UserToolResult("evt-9", result_json));
    CHECK(restored.RebuildFromHistory(history) == 1);

    // 重建出的 ref 照常解引用(工具还在、digest 没漂)。
    const auto resolved = restored.Resolve(registry, InvokeCall(record.tool_ref, {{"q", "x"}}));
    REQUIRE(resolved.has_value());
    CHECK(resolved->target_name == "mcp__github__search_issues");

    // 只补缺不覆盖:活账已有同 ref 的记录,恢复不再翻写。
    CHECK(restored.RebuildFromHistory(history) == 0);
}

TEST_CASE("Resolver: RebuildFromHistory 不认自由文本、坏 JSON 与错误结果") {
    tools::DeferredToolResolver restored("main");
    std::vector<api::Message> history;
    history.push_back(AssistantToolUse("evt-1", "tool_search", R"({"query":"x"})"));
    history.push_back(UserToolResult("evt-1", "命中 1 个工具:..."));           // 非结构化(legacy 文案)
    history.push_back(AssistantToolUse("evt-2", "tool_search", R"({"query":"y"})"));
    history.push_back(UserToolResult("evt-2", "{not json", true));              // 坏 JSON + is_error
    history.push_back(UserToolResult("evt-404", R"({"matches":[]})"));          // 无配对 tool_use
    CHECK(restored.RebuildFromHistory(history) == 0);
    CHECK(restored.ledger().Size() == 0);
}

TEST_CASE("Resolver: schema_too_large 的条目(无 tool_ref)不进恢复账") {
    tools::DeferredToolResolver restored("main");
    std::vector<api::Message> history;
    history.push_back(AssistantToolUse("evt-1", "tool_search", R"({"query":"x"})"));
    history.push_back(UserToolResult(
        "evt-1", nlohmann::json{{"catalog_revision", "r"},
                                {"matches", nlohmann::json::array({nlohmann::json{
                                                {"name", "huge_tool"},
                                                {"schema_digest", "abc"},
                                                {"error", "schema_too_large"},
                                            }})}}
                       .dump()));
    CHECK(restored.RebuildFromHistory(history) == 0);
}
