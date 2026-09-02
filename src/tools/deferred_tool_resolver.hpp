// 延迟工具的引用解析账(动态工具 PromptCache 守恒单·P1 通用 ProxyReference)。
//
// 三条路不能混作一条(单子 §四):Disabled 全量常驻;LegacyExpand 搜到后把
// schema 扩写回顶层 tools(cache-hostile 兼容路);ProxyReference 是本文件的
// 活——延迟工具永远不进顶层 tools,tool_search 把命中工具的名字、说明、
// 完整 schema 与不透明 tool_ref 放进 tool result 追加到历史尾部,模型随后
// 调固定的 tool_invoke({tool_ref, arguments}),宿主按 ref 找回真工具、用真
// schema 复验参数,再从 RunOneTool 正门执行。顶层定义恒定,前缀缓存不断。
//
// 本文件只管引用与版本,不管执行(单子 §6.3):Resolve 不碰 Tool::execute,
// 执行仍走 agent::RunOneTool 唯一正门。引 footgun 注记:发现不等于授权——
// ref 只回答"模型看过哪枚工具、当时看过哪版 schema",不回答"现在能不能
// 执行";执行时的角色/策略/确认/Hook 全在 RunOneTool 链里重走一遍。
//
// 线程规矩:主侧与子侧各一只 resolver;子侧被多只后台任务并发共享(与
// legacy 路共享 loaded 集合同一先例),账本与发号器内加锁,接口按值返回
// 记录,不往外递内部指针。

#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"  // Message/ToolUseBlock:恢复账从历史走,工具层已有此先例
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// ---------------------------------------------------------------------------
// 模式(单子 §四的定案表)。模式由配置/模型能力决定,不认 provider 名字。
// NativeReference(P3 已落):defer_loading 常驻声明 + provider 服务端搜索,
// 生效与否由装配期 ResolveDeferredToolMode 过 wire+目录能力两道门——本
// 文件仍是 proxy 路的家,native 路没有本地 ref/ledger 可管。
// ---------------------------------------------------------------------------
enum class DeferredToolMode {
    Disabled,        // 全量工具照旧常驻
    ProxyReference,  // 本地 search + invoke(P1)
    NativeReference, // provider defer_loading + tool_reference(P3)
    LegacyExpand,    // 旧路:发现后扩写目标 schema,只作兼容
};

// ---------------------------------------------------------------------------
// P4 默认切换开关(§十三 P4-2/P4-3):宿主推荐档。"auto" 配置值在 native
// 两道门不开时回落到它;未配置(空串)同样按 auto 解析。2026-09-03 真机
// §12.5 质量对照过门后由 LegacyExpand 翻成 ProxyReference(证据:
// eval/deferred_quality/report.md——proxy 任务成功 9/9 不低于两档,参数
// 首发合格 11/11 持平,误选三档全零;legacy 的 cache-hostile 真机可见,
// 7/8 任务命中即断 cache epoch,非缓存重付全场最高)。翻转 SOP 三处同笔:
// 这枚常量 + ParseDeferredToolMode 空串分支 + 文档默认值行,钉在
// docs/reference/tools.md"迁移窗"一节。回退无需改码:显式写
// legacy_expand/disabled 任一即压过默认。
// ---------------------------------------------------------------------------
inline constexpr DeferredToolMode kRecommendedDeferredToolMode = DeferredToolMode::ProxyReference;

// ""(未配)不再钉死 LegacyExpand——切默认后空串与 "auto" 同待遇,档位
// 仲裁归 ResolveDeferredToolMode(装配层按 wire+目录能力判)与直构路
//(落 kRecommendedDeferredToolMode),所以空串在这里回 nullopt,表示
// "未指定,交给能力仲裁"。显式 "legacy_expand" 仍落 LegacyExpand(迁移窗
// 兼容档);认不得的值回 nullopt,调用方负责把人话报清。P3 起
// native_reference 放行——但配置层收下不等于生效:装配期还要过 wire 与
// 目录能力两道门(ResolveDeferredToolMode)。P4 起新增 "auto"(能力驱动
// 档)——auto 不是一档模式而是解析策略(native 门开走原生、门不开落推荐
// 档),不从这里出,由 ResolveDeferredToolMode 单独判。
std::optional<DeferredToolMode> ParseDeferredToolMode(const std::string& text);

// 稳定名字(disabled/proxy_reference/native_reference/legacy_expand),展示
// 与 /context 共用;与配置字符串同一套词。
std::string DeferredToolModeName(DeferredToolMode mode);

// ---------------------------------------------------------------------------
// 有效模式判定(动态工具 P3·§四/§七):配置串 + wire + 目录能力声明 ->
// 有效模式。native_reference 只在 wire=anthropic 且目录显式声明
// deferred_tools 能力时生效,不按厂名猜(单子红线 2:兼容端不得误开)。
// 两道门任一不开而配置又点名要 native:落 LegacyExpand,native_denial 带
// 人话由装配层大声报出——不悄悄换路。纯函数,好单测。
// ---------------------------------------------------------------------------
struct DeferredToolModeResolution {
    DeferredToolMode mode = DeferredToolMode::LegacyExpand;
    std::string server_tool_search;  // native 生效时的搜索变体("regex"/"bm25")
    std::string native_denial;       // 非空 = native 被拒的人话(含建议配置)
    // 非空 = 生效说明(P4:auto 档落 native 时告知"目录声明能力、已走原生,
    // 要强制通用路就显式写 proxy_reference"),装配层与 native_denial 同通道
    // 打一行。空串不响——auto 门不开的回落是合同行为,不是意外,不吵。
    std::string mode_note;
};
DeferredToolModeResolution ResolveDeferredToolMode(const std::string& configured_text, bool wire_is_anthropic,
                                                   bool catalog_native_declared,
                                                   const std::string& catalog_server_tool_search);

// ---------------------------------------------------------------------------
// 稳定错误码(单子 §十的错误合同)。错误 result 仍用原 wire tool_use_id
// 配对;模型下一步的指路文案在 message 侧,码给账本与测试钉。
// confirmation_denied 沿用既有 permission.declined,不另造第二枚码。
// ---------------------------------------------------------------------------
inline constexpr const char* kErrToolRefUnknown = "proxy.unknown_tool_ref";
inline constexpr const char* kErrToolRefStale = "proxy.stale_tool_ref";
inline constexpr const char* kErrToolRefUnavailable = "proxy.tool_unavailable";
inline constexpr const char* kErrToolRefNotAllowed = "proxy.tool_not_allowed";
inline constexpr const char* kErrToolRefNotActive = "proxy.tool_not_active";
inline constexpr const char* kErrToolRefInvalidArguments = "proxy.invalid_target_arguments";
inline constexpr const char* kErrToolRefSchemaTooLarge = "proxy.schema_too_large";
inline constexpr const char* kErrToolInvokeDirectCall = "proxy.tool_invoke_direct_call";

// ---------------------------------------------------------------------------
// 引用账记录(单子 §9.1 DiscoveryLedger)。只作解析账,不复制 schema 正文
// ——schema 永远归 ToolRegistry 当前工具所有,执行时用真 schema 复验。
// ---------------------------------------------------------------------------
struct DeferredToolRefRecord {
    std::string tool_ref;
    std::string session_scope;        // 绑定:哪一侧代理的账("main"/"sub"/任务 id)
    std::string canonical_name;       // 注册表里的规范名
    std::string source_instance;      // MCP server / plugin id;可空
    std::string schema_digest;        // 发现那刻的 schema 摘要(sha256,64 hex)
    std::string catalog_revision;     // 发现那刻的 catalog 版本(诊断用)
    std::string discovered_event_id;  // 发现事件对账键(tool_search 的 tool_use id);可空
    // 本场是否已把完整 schema 摊给过模型:再搜同 digest 可回短引用,免得
    // 反复塞正文(单子 §5.3)。纯展示经济,不是安全位。
    bool schema_expanded = false;
};

// 轻账本:ref -> 记录。只增不删(工具没了/版本变了,记录留着可审计,拒绝
// 由 Resolve 现判 unavailable/stale);全部接口按值出入,线程安全。
class DiscoveryLedger {
public:
    std::optional<DeferredToolRefRecord> Find(const std::string& tool_ref) const;
    // 同 (canonical_name, source_instance, schema_digest) 的既有记录:ref
    // 复用,免得同一工具铸两枚 ref。
    std::optional<DeferredToolRefRecord> FindLive(const std::string& canonical_name,
                                                  const std::string& source_instance,
                                                  const std::string& schema_digest) const;
    void Upsert(DeferredToolRefRecord record);  // 同 ref 整条覆盖(mint/恢复共用)
    std::vector<DeferredToolRefRecord> Records() const;
    std::size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, DeferredToolRefRecord> by_ref_;
};

// ---------------------------------------------------------------------------
// 解析器:铸 ref、验 ref、恢复账。不执行任何工具。
// ---------------------------------------------------------------------------
class DeferredToolResolver {
public:
    // session_scope 是绑定键:主侧与子侧各一只,账不互通(父亲的 ref 不给
    // 儿子当通行牌,单子 §5.5;子侧各任务彼此共享,与 legacy 路共享 loaded
    // 集合同一先例——同表同级,无特权差)。
    explicit DeferredToolResolver(std::string session_scope);

    const std::string& session_scope() const { return scope_; }
    const DiscoveryLedger& ledger() const { return ledger_; }

    // schema 摘要:执行时同一枚 Tool 的 input_schema 折 sha256("v1\n"+dump)。
    // 带版前缀,日后摘要口径升级不与旧串撞车。
    static std::string SchemaDigestOf(const Tool& tool);

    // catalog 版本:全部延迟工具(注册序)的 name+instance+digest 折一枚。
    // 注册表后台增删、MCP 重连都会让它变;变了只影响新搜索的记录,不追改
    // 旧消息(单子 §8.3)。
    static std::string CatalogRevisionOf(const ToolRegistry& registry);

    // 发现:登记或复用一枚 ref。schema_expanded 按"这次结果有没有摊完整
    // schema"置位(schema_too_large 的条目不铸 ref,不该走到这)。
    DeferredToolRefRecord Discover(const Tool& tool, const ToolRegistration* registration,
                                   const std::string& catalog_revision, const std::string& discovered_event_id,
                                   bool schema_expanded);

    struct Resolution {
        std::string target_name;      // 真实工具名(RunOneTool 那一枚)
        nlohmann::json arguments;     // 模型给的 arguments 原样(细校验归正门)
        std::string tool_ref;
        std::string schema_digest;    // 发现那刻的摘要(与当下比对的凭据)
        std::string canonical_name;
        std::string source_instance;
    };
    struct Refusal {
        std::string code;     // kErrToolRef* 稳定码
        std::string message;  // 给模型的人话,含下一步指路
    };

    // 解引用:入参是 wire 上 tool_invoke 那枚调用的 input({tool_ref,
    // arguments})与它的 ToolUseBlock(取 id 供错误文案对账)。按 §5.5 的链
    // 走前三步:ref 在账 -> 目标仍注册 -> digest 仍等。角色/策略/确认不在
    // 这——那是 RunOneTool 正门的事。registry 由调用方递:每只 Agent 用自己
    // 那张表解析,隔离表/独立表天然不串。
    std::expected<Resolution, Refusal> Resolve(const ToolRegistry& registry,
                                               const api::ToolUseBlock& wire_call) const;

    // 从历史 tool_search 的 tool_use/tool_result 对重建账(单子 §9.2 恢复)。
    // 只信正式 discovery event,不信自由文本;已有同 ref 的活账不覆盖。
    // 恢复的记录 schema_expanded=true(历史里已经摊过全文)。坏行跳过,
    // 返回采纳条数。
    std::size_t RebuildFromHistory(const std::vector<api::Message>& history);

private:
    std::string MintRefLocked(const DeferredToolRefRecord& record);

    std::string scope_;
    std::string nonce_;  // 本实例一次性随机前缀:重启后新铸的 ref 不与恢复进来的旧 ref 撞
    DiscoveryLedger ledger_;
    mutable std::mutex mint_mutex_;
    std::uint64_t counter_ = 0;
};

// ---------------------------------------------------------------------------
// 代理调用上下文:AgentLoop 规范化之后递给 RunOneTool 的协议证据。RunOneTool
// 拿它做两件事——目标参数按真 schema 先验一遍(invalid_target_arguments 的
// 稳定拒绝),trace 里补 transport/resolved 两层事实(单子 §6.2)。
// ---------------------------------------------------------------------------
struct ProxyCallContext {
    std::string transport_name;  // wire 上那只壳("tool_invoke")
    std::string tool_ref;
    std::string schema_digest;
};

}  // namespace lubancode::tools
