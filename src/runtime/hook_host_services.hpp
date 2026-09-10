// LuaHook 单 P1-C:hook 的宿主能力束(§五"开放宿主能力"的合同与执行件)。
//
// 分层规矩(§五):宿主开放什么由三方交集定——清单 capabilities 申请 ∩
// 槽位允许 ∩ 宿主授权(grants)。本文件管两半:
//   1. grants(宿主授权账:HTTP/文件/状态/日志/工具的许可与限额);
//   2. per-invocation 服务束(LuaHookServices):每次 hook 调用新鲜一份,
//      收集器(context 候选/执行事实)不跨调用;Lua 侧只经
//      runtime/plugin_lua_host.cpp 的 luban.* Host API 触达,拿不到裸指针。
//
// 不开的口(§五默认拒):原始 SessionRuntime/ContextManager/writer、可变
// 历史、宿主地址、任意 C 模块加载、shell。文件写与工具调用都走本层执行
// 件——脚本不能先绕过宿主写完文件再补一句"我执行了"。
//
// 工具桥(§5.1):Lua handler -> HookToolExecutionService -> 注册表解析/
// 准入 -> Tool::execute(模型 Action 同一底座) -> 子执行按 v3 §4.49 记账
// (tool.execution.* 全链,linkage 带 hookInvocationId)。不直调 mcp::
// Client,不借 tool call 的皮。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"        // InvocationCtx
#include "runtime/plugin_http.hpp"     // PluginHttpCallSpec(受控 HTTP 底座)
#include "tools/registry.hpp"          // ToolRegistry
#include "tools/tool.hpp"              // Tool::Result
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 能力令牌(清单 capabilities 的申请词;宿主 grants 按同词授权)。
// ---------------------------------------------------------------------------
inline constexpr std::string_view kHookCapHttp     = "http";      // luban.http/luban.secrets
inline constexpr std::string_view kHookCapFsRead   = "fs.read";   // luban.fs.read/list
inline constexpr std::string_view kHookCapFsWrite  = "fs.write";  // luban.fs.write
inline constexpr std::string_view kHookCapState    = "state";     // luban.state.*
inline constexpr std::string_view kHookCapContext  = "context";   // luban.context.append
inline constexpr std::string_view kHookCapLog      = "log";       // luban.log.event
inline constexpr std::string_view kHookCapTools    = "tools";     // luban.tools.call/list

// ---------------------------------------------------------------------------
// 稳定错误码(hook Host API;只增不改)。
// ---------------------------------------------------------------------------
namespace hookapi_err {
inline constexpr std::string_view kNotHookContext       = "hook.not_hook_context";
inline constexpr std::string_view kCapabilityNotGranted = "hook.capability_not_granted";
inline constexpr std::string_view kFsBadPath            = "hook.fs.bad_path";
inline constexpr std::string_view kFsPathDenied         = "hook.fs.path_denied";
inline constexpr std::string_view kFsTooLarge           = "hook.fs.too_large";
inline constexpr std::string_view kFsMissing            = "hook.fs.missing";
inline constexpr std::string_view kFsIoError            = "hook.fs.io_error";
inline constexpr std::string_view kStateBadKey          = "hook.state.bad_key";
inline constexpr std::string_view kStateBadValue        = "hook.state.bad_value";
inline constexpr std::string_view kStateQuota           = "hook.state.quota_exceeded";
inline constexpr std::string_view kContextBadText       = "hook.context.bad_text";
inline constexpr std::string_view kContextQuota         = "hook.context.quota_exceeded";
inline constexpr std::string_view kLogBadFields         = "hook.log.bad_fields";
inline constexpr std::string_view kToolRegistryMissing  = "hook.tool.registry_missing";
inline constexpr std::string_view kToolUnknown          = "hook.tool.unknown";
inline constexpr std::string_view kToolNotAllowed       = "hook.tool.not_allowed";
inline constexpr std::string_view kToolBadInput         = "hook.tool.invalid_arguments";
inline constexpr std::string_view kToolCallCap          = "hook.tool.call_cap";
inline constexpr std::string_view kToolRepeatCap        = "hook.tool.repeat_cap";
inline constexpr std::string_view kToolRecursionDepth   = "hook.tool.recursion_depth";
}  // namespace hookapi_err

// Lua 失败表的 C++ 形状(与 plugin_http 的 err 表同款;hook Host API 共用)。
struct HookApiError {
    std::string code;      // hookapi_err:: 稳定串
    std::string message;   // 人话(不含 Secret/路径外的事实)
};

// ---------------------------------------------------------------------------
// 受控文件(§五:授权根、规范路径、符号链接、字节帽、原子写与执行事实)。
// ---------------------------------------------------------------------------
struct LuaHookServices;  // 执行件声明在前,定义在后(只用到引用)

struct HookFsGrant {
    // 相对路径的解析根(通常是 hook 包根)。绝对授权根直接放 read/write_roots。
    std::filesystem::path base;
    // 授权子树(相对 base 的路径或绝对路径;lexically_normal 后比对)。
    std::vector<std::filesystem::path> read_roots;
    std::vector<std::filesystem::path> write_roots;
    std::uint64_t max_read_bytes = 256 * 1024;   // 单次读帽
    std::uint64_t max_write_bytes = 256 * 1024;  // 单次写帽
    int max_list_entries = 512;                  // 单次列目帽
};

// 路径授权的纯函数半边:候选路径 -> 授权根内的规范路径;越界/符号链接
// 逃逸拒绝。exists=false 时按"父目录在场"口径验(写新文件用)。
std::expected<std::filesystem::path, HookApiError> AuthorizeHookFsPath(const HookFsGrant& grant,
                                                                       std::string_view raw_path,
                                                                       bool for_write);

// 文件执行件(plugin_lua_host 的 luban.fs.* 调这里;执行事实记 fs_ops)。
std::expected<std::string, HookApiError> ReadHookFsFile(LuaHookServices& services, std::string_view raw_path);
std::expected<bool, HookApiError> WriteHookFsFile(LuaHookServices& services, std::string_view raw_path,
                                                  std::string_view content);
std::expected<std::vector<std::pair<std::string, bool>>, HookApiError> ListHookFsDir(
    LuaHookServices& services, std::string_view raw_path);

// ---------------------------------------------------------------------------
// 插件状态(§五:按包隔离、限额;首版进程内,持久化/恢复归 P1-D 生命周期)。
// ---------------------------------------------------------------------------
class HookStateStore {
public:
    struct Limits {
        std::size_t max_keys_per_package = 64;
        std::uint64_t max_value_bytes = 64 * 1024;
        std::uint64_t max_total_bytes_per_package = 1024 * 1024;
    };

    // 默认实参写显式构造:gcc 对默认实参语境的花括号聚合初始化报
    // "could not convert brace-list"(MSVC 收),Limits{} 两家都收。
    explicit HookStateStore(Limits limits = Limits{}) : limits_(limits) {}

    std::expected<nlohmann::json, HookApiError> Get(const std::string& package, const std::string& key) const;
    std::expected<bool, HookApiError> Set(const std::string& package, const std::string& key,
                                          nlohmann::json value);
    std::vector<std::string> Keys(const std::string& package) const;  // 排序序

private:
    static std::optional<HookApiError> ValidateKey(const std::string& key);
    std::uint64_t ValueBytes(const nlohmann::json& value) const { return value.dump().size(); }

    Limits limits_;
    mutable std::mutex mutex_;
    // package -> (key -> value);总字节按包记,dump 口径。
    std::map<std::string, std::map<std::string, nlohmann::json>> values_;
    std::map<std::string, std::uint64_t> totals_;
};

// ---------------------------------------------------------------------------
// context 候选(§五:申请追加消息——候选先收,采用经 ContextAppend 效果
// 走中间件核的宿主校验,Lua 不直接改 context)。
// ---------------------------------------------------------------------------
struct HookContextCollector {
    struct Limits {
        std::size_t max_entries = 16;
        std::uint64_t max_total_bytes = 64 * 1024;
    };
    struct Entry {
        std::string text;
        std::string source;  // 来源标识("hook:<hookId>")
    };

    Limits limits;
    std::vector<Entry> entries;
    std::uint64_t total_bytes = 0;

    // 满了/超帽即拒(不悄悄截断候选正文——采用与否是宿主的事)。
    std::optional<HookApiError> Append(std::string text, std::string source);
};

// ---------------------------------------------------------------------------
// 结构化日志(§五:提交结构化日志;显示投影归宿主,不自动注入模型)。
// ---------------------------------------------------------------------------
// fields 里的长字符串截到 2 KiB 并标 truncated(截断归宿主,§五)。
nlohmann::json SanitizeHookLogFields(const nlohmann::json& fields);
using HookLogSink = std::function<void(const std::string& level, const nlohmann::json& fields)>;

// ---------------------------------------------------------------------------
// 子执行记账(§5.1/v3 §4.49:独立 executionId、parentHookInvocationId、
// 可选 parentActionId、实际 server/tool、参数、终态、结果引用与错误)。
// ---------------------------------------------------------------------------
struct HookSubExecutionRecord {
    std::string execution_id;  // tool.execution.* 的 actionId(hookexec-*)
    std::string hook_dispatch_id;
    std::string hook_invocation_id;
    std::optional<std::string> parent_action_id;
    std::string hook_id;
    std::string logical_tool;
    std::string backend;  // 注册来源(builtin/mcp:<server>/plugin:…)
    std::optional<std::string> turn_id, step_id;
    nlohmann::json input;
    // 终态:admitted=false -> rejected;否则 finished|failed|cancelled|unknown。
    // unknown = 超时/断连/取消——"缺记录不能证明未执行"(§5.1),不自动重试。
    bool admitted = false;
    std::string terminal;
    bool execution_uncertain = false;  // 远端可能已执行而响应丢失
    std::uint64_t duration_ms = 0;
    std::string error_code;         // outcome/error_code 透传(mcp.timeout 等)
    nlohmann::json result_summary;  // 内容摘要(截断;全文不入事件)

    nlohmann::json ToJson() const;
};

// 一枚子执行的记账句柄。V3 实现走 ToolActionSession 全链(pending ->
// started -> 终态);测试可用空实现只看 record。
class HookSubExecutionLedger {
public:
    class Session {
    public:
        virtual ~Session() = default;
        virtual void Started(const nlohmann::json& tool_identity, const std::string& effective_args_ref) {}
        virtual void Reject(const std::string& reason) {}
        virtual void Finish(std::optional<std::int64_t> exit_code) {}
        virtual void Fail(const std::string& error_code) {}
        virtual void Cancel(const std::string& reason) {}
        virtual void MarkUnknown(const std::string& reason) {}
    };

    virtual ~HookSubExecutionLedger() = default;
    // pending(reason=hook_subexecution,linkage 带 hookInvocationId)。
    virtual std::unique_ptr<Session> Open(const HookSubExecutionRecord& record) = 0;
};

// v3 主账实现:tool.execution.pending/started/rejected/finished/failed/
// cancelled/unknown,linkage payload 与 hooks.cpp BeginSubExecution 同形
//(parentActionId/hookDispatchId/hookInvocationId/logicalTool/backend)。
// 不向 provider 塞 tool 消息(§5.1):结果选用归 hook 效果,不在这造。
class V3HookSubExecutionLedger final : public HookSubExecutionLedger {
public:
    explicit V3HookSubExecutionLedger(trajectory::v3::V3Writer& writer) : writer_(&writer) {}
    std::unique_ptr<Session> Open(const HookSubExecutionRecord& record) override;

private:
    trajectory::v3::V3Writer* writer_;
};

// 子执行 id 发号:hookexec-<进程内单调>-<毫秒时间戳>(对账够用,不替
// turnId 代班——与 middleware.cpp 的 dispatch/invocation 发号同一口径)。
std::string NextHookSubExecutionId();

// ---------------------------------------------------------------------------
// 统一工具执行服务(§5.1):hook 子执行路。模型 Action 路(agent::
// RunOneTool)与本路共用注册表解析、schema 复验与 Tool::execute 底座;
// 身份与记录分开(hook 子执行不冒充模型 tool_call)。
//
// 递归治理(§5.1):单 invocation 调用总帽 + 同名工具重复帽 + 嵌套深度帽
//(子执行里再触发 hook 又调工具,thread_local 深度记账)。拒绝无界递归,
// 但不为避递归跳过权限检查——准入照走。
// ---------------------------------------------------------------------------
class HookToolExecutionService {
public:
    struct Options {
        tools::ToolRegistry* registry = nullptr;  // 空 = registry_missing
        std::shared_ptr<HookSubExecutionLedger> ledger;
        // 准入名单(空 = 一律拒;发现不等于授权,deferred_resolver 同规矩)。
        std::vector<std::string> allow_tools;
        int max_calls_per_invocation = 8;
        int max_same_tool_calls = 3;
        int max_recursion_depth = 2;
        std::uint64_t result_preview_bytes = 32 * 1024;  // 回 Lua 的正文帽
    };

    explicit HookToolExecutionService(Options options) : options_(std::move(options)) {}

    struct Result {
        bool admitted = false;
        std::string status;      // rejected|finished|failed|cancelled|unknown
        std::string error_code;  // hookapi_err::*(rejected)或工具 outcome 码
        std::string message;
        std::string content;     // 结果正文(截到 preview 帽,截断即标)
        bool content_truncated = false;
        bool is_error = false;
        nlohmann::json details;
        std::string execution_id;
        nlohmann::json record;  // HookSubExecutionRecord::ToJson(执行事实)
    };

    // 一次子执行。绝不抛;取消旗贯通 Tool::execute(ToolExecutionContext)。
    Result Call(const std::string& tool_name, const nlohmann::json& input, const std::string& hook_id,
                const std::string& hook_dispatch_id, const std::string& hook_invocation_id,
                const std::optional<std::string>& parent_action_id,
                const std::optional<std::string>& turn_id, const std::optional<std::string>& step_id,
                const std::atomic<bool>* cancel);

    // 授权名单内、注册表现存的工具清单(luban.tools.list)。
    std::vector<std::pair<std::string, std::string>> ListAuthorized() const;

    const Options& options() const { return options_; }

private:
    Options options_;
    int total_calls_ = 0;
    std::map<std::string, int> calls_per_tool_;
};

// ---------------------------------------------------------------------------
// per-invocation 服务束:LuaCallContext(Kind::Hook)携带;每次 hook 调用
// 新鲜一份,活到 CallHook 返回。
// ---------------------------------------------------------------------------
struct LuaHookServices {
    std::string package_id;  // 包 id(state 命名空间/log 来源)

    // HTTP/Secret(P1-C 第 1 条):hook 自己的权限与预算,不借 tool call。
    bool http_granted = false;
    PluginHttpCallSpec http;  // cancel 由 MakeLuaHookHandler 灌 invocation 旗

    // 受控文件。fs_granted=false 时 fs.* 一律 capability_not_granted。
    bool fs_granted = false;
    HookFsGrant fs;
    std::vector<nlohmann::json> fs_ops;  // 执行事实(op/path/bytes;宿主账)

    // 插件状态(共享 store,按 package_id 隔离)。
    HookStateStore* state = nullptr;

    // context 候选(收集器;采用经效果管道)。
    bool context_granted = false;
    HookContextCollector context;

    // 结构化日志。
    HookLogSink log;

    // 工具桥。
    std::unique_ptr<HookToolExecutionService> tools;
};

// ---------------------------------------------------------------------------
// 服务中心:进程级 grants 持有者 + per-invocation 服务束工厂。装配层
//(SetupHookRuntime)配置一次;MakeLuaHookHandler 每次 invocation 调 Build。
// grants 的交集规则在这里落:清单没申请的(capabilities 不含)不授权;
// 申请了而 grants 没开的同样不授权。
// ---------------------------------------------------------------------------
class HookHostServiceCenter {
public:
    // 宿主授权账。生产缺省:state/log 开(无害),http/fs/tools 关(P1-D
    // 作者文档与配置面落定后再按包授权)。
    struct Grants {
        std::optional<PluginHttpCallSpec> http;  // nullopt = 宿主未授权 HTTP
        std::optional<HookFsGrant> fs;           // nullopt = 未授权文件
        std::shared_ptr<HookStateStore> state;
        HookLogSink log;
        tools::ToolRegistry* tool_registry = nullptr;
        std::vector<std::string> allow_tools;  // 空 = 拒全部
    };

    HookHostServiceCenter() = default;
    explicit HookHostServiceCenter(Grants grants) : grants_(std::move(grants)) {}

    void SetGrants(Grants grants) { grants_ = std::move(grants); }
    const Grants& grants() const { return grants_; }

    // 子执行账的 v3 主写者(会话开卷/换场时换绑;null = 不落 v3 事件)。
    void SetSubExecutionWriter(trajectory::v3::V3Writer* writer) { writer_.store(writer); }
    trajectory::v3::V3Writer* sub_execution_writer() const { return writer_.load(); }

    // per-invocation 服务束:申请 ∩ 授权。package_id 进 state 命名空间与
    // 日志来源;cancel 灌进 HTTP/工具桥的取消链。
    std::unique_ptr<LuaHookServices> Build(const std::vector<std::string>& requested_capabilities,
                                           const hooks::middleware::InvocationCtx& ctx,
                                           const std::string& package_id);

private:
    Grants grants_;
    std::atomic<trajectory::v3::V3Writer*> writer_{nullptr};
};

// 进程级缺省中心(与 app::HookRuntime 同一代价取向:进程一份,退出不析构
// 争夺)。SetupHookRuntime 配置;测试一般自建中心,不动这只。
HookHostServiceCenter& DefaultHookServiceCenter();

}  // namespace lubancode::runtime
