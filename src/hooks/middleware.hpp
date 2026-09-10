// hook 中间件体系:统一合同与执行核(LuaHook 单 P0-A)。
//
// 这一层按轨迹 v3 §4.22、§4.36、§4.47-§4.48 冻结"中间件版"hook 合同:
// 逻辑键 (hookPoint,name) 标识功能槽位;同键按来源层级选唯一实现(builtin
// < 已启用扩展 < project < user < 显式 session 配置),只跑获选项,不沿用
// 旧 dispatcher 的"来源相加";顺序由阶段约束 + before/after 依赖优先,余下
// 按 priority 小者先、同值按逻辑键稳定排;dispatch 计划在派发时冻结,
// 执行中不许改注册表。
//
// 与旧 src/hooks/dispatcher.* 的关系:P0-A 新核并行落地,老进程 hook 路径
// 照旧(挂点逐个迁移是 P0-B 的事);HookDispatcher 持一枚可选的中间件核
// 引用作接线缝,空 = 零行为。
//
// 本文件冻结合同(挂点清单/阶段/效果枚举/错误码/ctx-input-result-next 形状);
// 执行语义在 middleware.cpp(同名选实现、不可变计划、至多一次 next、候选
// 采用、短路、跨 invocation 拒绝、观察者并发)。事件账按 trajectory::v3
// 的 hooks 事件合同(§4.22)出记录接口(MiddlewareEventSink);P0-B 再接
// V3Writer,本批不碰 writer。
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::hooks::middleware {

// ---------------------------------------------------------------------------
// 挂点(v3 §4.47 首批)。P0-A 只冻结清单与合同,不接线生产路径。
// ---------------------------------------------------------------------------
enum class HookPoint {
    PreSystem,
    PostSystem,
    PreUser,
    PostUser,
    PreAssistant,
    PostAssistant,
    PreTurn,
    PostTurn,
    PreStep,
    PostStep,
    PreRequest,
    PreAction,
    PostAction,
};

std::string_view ToString(HookPoint point);
// 配置键 -> 挂点;不认得的键返回 false。
bool ParseHookPoint(std::string_view name, HookPoint& out);

// ---------------------------------------------------------------------------
// 阶段。PreRequest 专用四段里的三段(v3 §4.36/§4.48):
//   mutate -> [freeze(宿主掌握,不给 handler 注册)] -> estimate -> capacity
// 其余挂点一律 Default。
// ---------------------------------------------------------------------------
enum class Stage { Default = 0, Mutate = 1, Estimate = 2, Capacity = 3 };

std::string_view ToString(Stage stage);
bool ParseStage(std::string_view name, Stage& out);
// 挂点与阶段是否匹配:非 PreRequest 只许 Default;PreRequest 只许
// Mutate/Estimate/Capacity(freeze 是宿主提交边界,不是可注册阶段)。
bool StageAllowed(HookPoint point, Stage stage);

// ---------------------------------------------------------------------------
// 效果枚举(v3 §4.22 首批:参数改写、后端选择、结果替换/补充/过滤、准入
// 决定、上下文追加与 system 变更)。新增效果须有版本化类型和作用对象。
// ---------------------------------------------------------------------------
enum class EffectType {
    InputRewrite,      // 改写输入候选(经 next(candidate) 或返回效果)
    BackendSelect,     // 选择执行后端(PreAction;原 Action 身份保留)
    ResultReplace,     // 替换已保存原始结果(PostAction)
    ResultSupplement,  // 补充结果材料
    ResultFilter,      // 过滤结果条目
    AdmissionDecision, // 准入决定(deny/allow/recover)
    ContextAppend,     // 追加上下文(带来源,提交边界由宿主收口)
    SystemChange,      // system 变更(沿 system 版本合同)
};

std::string_view ToString(EffectType type);
bool ParseEffectType(std::string_view name, EffectType& out);

// 挂点(+阶段)合同:该位置允不允许这枚效果。这是"合法效果"的冻结矩阵:
//   PreRequest/Mutate   可改输入;Estimate 只产出估算值,输入已冻结;
//   Capacity 只给准入决定(允许发送/要求恢复/拒绝)。
//   PostUser 不能回写原 user(§4.47);PostAction 副作用已发生,无准入;
//   PreAssistant 不改模型原始回复,只准入。
bool EffectAllowed(HookPoint point, Stage stage, EffectType type);
// 该位置能不能经 next(candidate) 改写输入(= InputRewrite 是否合法的
// 候选采用口径)。
bool InputRewriteAllowed(HookPoint point, Stage stage);

// ---------------------------------------------------------------------------
// 稳定错误码表(P0-A 冻结;只增不改)。业务 deny 不是错误——它走
// DispatchOutcome::Denied 与 AdmissionDecision 效果,不落这些码。
// ---------------------------------------------------------------------------
namespace err {
inline constexpr std::string_view kPlanSameKeyConflict  = "hook.plan.same_key_conflict";  // 同层同键重复
inline constexpr std::string_view kPlanDependencyCycle  = "hook.plan.dependency_cycle";   // 循环依赖
inline constexpr std::string_view kPlanMissingDep       = "hook.plan.missing_dependency"; // 依赖的逻辑键不存在
inline constexpr std::string_view kPlanStageInversion   = "hook.plan.stage_inversion";    // 依赖与阶段矛盾
inline constexpr std::string_view kPlanStageMismatch    = "hook.plan.stage_mismatch";     // 替代实现改槽位阶段
inline constexpr std::string_view kPlanBadStage         = "hook.plan.bad_stage";          // 挂点与阶段不匹配
inline constexpr std::string_view kPlanObserverWithDeps = "hook.plan.observer_with_deps"; // 观察者不许带依赖
inline constexpr std::string_view kPlanObserverRequired = "hook.plan.observer_required";  // required 槽位不许观察
inline constexpr std::string_view kPlanNoLuaFactory     = "hook.plan.no_lua_factory";     // lua 定义而无 lua 工厂
inline constexpr std::string_view kManifestInvalid      = "hook.manifest.invalid";        // 清单不合 schema
inline constexpr std::string_view kLuaCompileError      = "hook.lua.compile_error";       // 编译/顶层/对账失败
inline constexpr std::string_view kLuaRuntimeError      = "hook.lua.runtime_error";       // handler 运行错
inline constexpr std::string_view kLuaBudgetInstruction = "hook.lua.budget_instruction";  // 指令预算耗尽
inline constexpr std::string_view kLuaBudgetMemory      = "hook.lua.budget_memory";       // 内存帽落锤
inline constexpr std::string_view kLuaBudgetWallClock   = "hook.lua.budget_wallclock";    // 墙钟到点
inline constexpr std::string_view kLuaStateReentry      = "hook.lua.state_reentry";       // 嵌套重入同一 state(§4.1)
inline constexpr std::string_view kNestingExceeded      = "hook.lua.nesting_exceeded";    // 链深超限
inline constexpr std::string_view kNextAlreadyConsumed  = "hook.next.already_consumed";   // 重复 next
inline constexpr std::string_view kNextNotAllowed       = "hook.next.not_allowed";        // 观察者无 next
inline constexpr std::string_view kNextExpired          = "hook.next.expired";            // 终态后/跨 invocation
inline constexpr std::string_view kNextBadCandidate     = "hook.next.bad_candidate";      // 候选形状不合法
inline constexpr std::string_view kHandlerFailed        = "hook.handler.failed";          // builtin handler 抛错
inline constexpr std::string_view kResultInvalid        = "hook.result.invalid";          // 返回形状不合合同
inline constexpr std::string_view kEffectRejected       = "hook.effect.rejected";         // 效果被宿主拒绝(记录用)
inline constexpr std::string_view kDispatchCancelled    = "hook.dispatch.cancelled";      // 取消
}  // namespace err

// ---------------------------------------------------------------------------
// 来源层级。只高不低:builtin 是默认项,显式 session 配置最高;插件不能
// 自报 session 提权(层级由装载位置决定,清单里没有 source 字段)。
// ---------------------------------------------------------------------------
enum class SourceLayer { Builtin = 0, Extension = 1, Project = 2, User = 3, Session = 4 };

std::string_view ToString(SourceLayer layer);
// 数值大者胜出;此层级只用来选实现,不与执行 priority 混为一谈。
constexpr int LayerRank(SourceLayer layer) { return static_cast<int>(layer); }

// ---------------------------------------------------------------------------
// 触发条件(先按同名规则选实现,再对触发对象匹配;未命中不暗跑旧实现)。
// P0-A 只收精确匹配;通配留后续批次。
// ---------------------------------------------------------------------------
struct DispatchTrigger;

struct MatchRule {
    std::optional<std::string> origin;         // human/hook/skill/compact/…
    std::optional<std::string> purpose;        // interactive/title/compact/…
    std::optional<std::string> delivery_mode;  // direct/steer/followup/…

    bool Matches(const DispatchTrigger& trigger) const;
    bool Empty() const { return !origin.has_value() && !purpose.has_value() && !delivery_mode.has_value(); }
};

// ---------------------------------------------------------------------------
// 预算(§六)。建议初值是待验证配置值,不是性能结论。
// ---------------------------------------------------------------------------
struct HandlerLimits {
    std::uint64_t instruction_budget = 200'000'000;        // Lua 指令
    std::size_t memory_cap_bytes = 256ull * 1024 * 1024;   // state 内存帽
    std::chrono::milliseconds wall_budget{500};            // handler 自用墙钟
};

// 失败处置(随定义快照落档;required 槽位强制 Abort,替代实现不能自行解除):
//   Abort       失败即整体失败,下游不再执行(下游未跑则跳过,已跑则留收据)
//   KeepOriginal optional 纯转换失败且未消费 next:以进入本 handler 的版本
//               继续一次;已消费 next:采用下游已完成的收据
enum class FailurePolicy { Abort, KeepOriginal };

std::string_view ToString(FailurePolicy policy);
bool ParseFailurePolicy(std::string_view name, FailurePolicy& out);

// ---------------------------------------------------------------------------
// Lua 实现的声明件。脚本正文由装载方读进内存(目录发现/越界检查不在
// P0-A);entry 是 handler 表里的函数名。定义 hash 覆盖清单、入口与脚本。
// ---------------------------------------------------------------------------
struct LuaHandlerSpec {
    std::string script;       // Lua 源码(内存,不走盘)
    std::string chunk_name;   // 报错与 trace 用(如 "hooks/my_memory/main.lua")
    std::string entry;        // handler 表字段名(如 "recall")
};

// ---------------------------------------------------------------------------
// handler 调用合同(C++ 内置与 Lua 同一形状;Lua 由 runtime 侧适配)。
// ---------------------------------------------------------------------------
// 一次 dispatch 的宿主发行身份(只读;hook 不自行开真人回合)。
struct InvocationCtx {
    std::string dispatch_id;
    std::string invocation_id;
    std::uint64_t registry_revision = 0;
    HookPoint point = HookPoint::PreUser;
    Stage stage = Stage::Default;
    std::string hook_id;  // 逻辑键 "Point/name"
    std::string definition_hash;
    int depth = 0;  // 链上位置(next 框架深度;宿主嵌套预算用)
    std::optional<std::string> turn_id, step_id, action_id, request_id;
    const std::atomic<bool>* cancel = nullptr;  // 取消旗(可空)
};

// next 的下游返回:业务结局是值,不是 Lua 错;协议违规才走错误。
struct DownstreamOutcome {
    enum class Kind { Value, Denied, Failed, Invalid };
    Kind kind = Kind::Value;
    nlohmann::json value;
    std::string code;     // Denied/Failed 的码;Invalid 的错误码
    std::string message;
};

// 至多一次 next 的边界。第二次调用不跑下游,回 Invalid(hook.next.already_consumed);
// 跨 invocation 拒绝由槽位过期机制保证(runtime 侧),执行核这边每次
// invocation 造新 NextCall。
class NextCall {
public:
    NextCall() = default;
    // impl:执行核注入的"候选采用 + 跑下游"。NextCall 自己把守至多一次。
    explicit NextCall(std::function<DownstreamOutcome(const std::optional<nlohmann::json>&)> impl)
        : impl_(std::move(impl)) {}

    DownstreamOutcome Call(std::optional<nlohmann::json> candidate = std::nullopt);
    DownstreamOutcome operator()(std::optional<nlohmann::json> candidate = std::nullopt) {
        return Call(std::move(candidate));
    }
    // 观察者/无下游者的 next:调用恒回 Invalid(hook.next.not_allowed)。
    static NextCall Forbidden();

    bool consumed() const { return consumed_; }
    int calls() const { return calls_; }
    // 最近一次有效下游结果(KeepOriginal 已消费失败时采用下游收据用)。
    const DownstreamOutcome* last() const { return last_ ? &(*last_) : nullptr; }

private:
    std::function<DownstreamOutcome(const std::optional<nlohmann::json>&)> impl_;
    bool consumed_ = false;
    int calls_ = 0;
    std::optional<DownstreamOutcome> last_;
};

struct Effect {
    EffectType type = EffectType::ContextAppend;
    nlohmann::json payload = nlohmann::json::object();  // 类型专属字段(宿主校验)
};

// handler 的一次正常返回。
struct HandlerReturn {
    nlohmann::json output;               // 本帧产出值;空且已消费 next = 透传下游值
    std::vector<Effect> effects;         // 返回携带的效果(宿主逐项校验采用)
    bool deny = false;                   // 业务拒绝(与脚本错误分开)
    std::string deny_code, deny_message;

    static HandlerReturn Value(nlohmann::json output) {
        HandlerReturn out;
        out.output = std::move(output);
        return out;
    }
    static HandlerReturn Denied(std::string code, std::string message) {
        HandlerReturn out;
        out.deny = true;
        out.deny_code = std::move(code);
        out.deny_message = std::move(message);
        return out;
    }
};

struct HandlerError {
    std::string code;     // 稳定码(err:: 表)
    std::string message;  // 人话(不冒充稳定码)
};

using Handler = std::function<std::expected<HandlerReturn, HandlerError>(const InvocationCtx&,
                                                                         const nlohmann::json& input,
                                                                         NextCall& next)>;

// ---------------------------------------------------------------------------
// 运行期定义。逻辑键 = (point,name);实现为 builtin(C++ Handler)或 lua。
// required/阶段/结果合同属于功能槽位:builtin 候选在场时以 builtin 声明
// 为准,替代实现不能解除(发布期落定)。
// ---------------------------------------------------------------------------
struct MiddlewareDefinition {
    HookPoint point = HookPoint::PreUser;
    std::string name;                 // 逻辑功能名,如 "memory.recall"
    SourceLayer layer = SourceLayer::Builtin;
    std::string source_label;         // 展示串("builtin" / "user ~/.lubancode/hooks/…")
    std::string implementation_ref;   // 稳定实现引用("builtin.memory_recall_v1" / "hooks/my_memory#recall")
    std::string definition_hash;      // 空 = 添加时按声明内容计算

    bool is_lua = false;
    Handler builtin;                  // is_lua=false 时必填
    LuaHandlerSpec lua;               // is_lua=true 时必填

    Stage stage = Stage::Default;
    int priority = 100;               // 数值小者先
    std::vector<std::string> before;  // 逻辑键("Point/name";裸名按同挂点解析)
    std::vector<std::string> after;   // 本项在这些键之后跑
    MatchRule match;
    std::vector<std::string> capabilities;   // 申请的能力(P1-C 起与宿主授权对账)
    FailurePolicy failure_policy = FailurePolicy::Abort;
    bool required = false;            // 槽位要求(替代实现不能解除)
    bool observer = false;            // 只读观察者:可并发,不能 next/改链
    HandlerLimits limits;

    std::string Key() const { return std::string(ToString(point)) + "/" + name; }
};

// ---------------------------------------------------------------------------
// 清单(hook.json,schemaVersion 1)。定义元数据集中在清单,Lua 只返回
// handler 表,不复制清单。来源层级由调用方按发现位置给(清单不自报)。
// ---------------------------------------------------------------------------
struct ManifestError {
    std::string code;     // err::kManifestInvalid 等
    std::string message;  // 人话(带条目定位)
};

// script 是入口文件正文(entry 字段只做形状校验与记录;目录发现不在 P0-A)。
std::expected<std::vector<MiddlewareDefinition>, ManifestError> ParseHookManifest(
    const nlohmann::json& manifest, SourceLayer layer, const std::string& source_label,
    const std::string& script);

// ---------------------------------------------------------------------------
// 注册池与发布。发布 = 选实现(同键取最高层,同层冲突拒绝)+ 排序(阶段
// -> 依赖 -> priority -> 逻辑键)+ 槽位定档(required/阶段以 builtin 声明
// 为准)。发布成功得不可变 FrozenRegistry;失败整版拒绝,保留旧版。
// ---------------------------------------------------------------------------
struct PlanError {
    std::string code;
    std::string message;
};

class FrozenRegistry {
public:
    std::uint64_t revision() const { return revision_; }
    // 该挂点获选定义(稳定执行序)。无 = 空表。
    const std::vector<std::shared_ptr<const MiddlewareDefinition>>& Selected(HookPoint point) const;
    // 被同名覆盖挤下的定义(保留定义来源,不进执行计划;§3.1)。
    const std::vector<std::shared_ptr<const MiddlewareDefinition>>& Overridden() const { return overridden_; }
    // 解析后的实际计划(展示用:每挂点获选项、来源、顺序 + 被覆盖清单)。
    nlohmann::json DescribePlan() const;

private:
    friend class MiddlewarePool;
    std::uint64_t revision_ = 0;
    std::map<HookPoint, std::vector<std::shared_ptr<const MiddlewareDefinition>>> selected_;
    std::vector<std::shared_ptr<const MiddlewareDefinition>> overridden_;
};

class MiddlewarePool {
public:
    struct Options {
        // Lua 声明 -> 可执行 Handler 的工厂(runtime 侧注入;缺省 = 不许
        // lua 定义入池,发布时报 hook.plan.no_lua_factory)。
        std::function<std::expected<Handler, std::string>(const LuaHandlerSpec&, const HandlerLimits&)> lua_factory;
    };

    explicit MiddlewarePool(Options options = {}) : options_(std::move(options)) {}

    // 候选定义入池(不校验计划;Publish 时统一裁决)。definition_hash 空
    // 则按声明内容补算。
    void AddDefinition(MiddlewareDefinition definition);
    // 清单解析入池;解析失败整份拒绝(不带半个定义进来)。
    std::expected<void, ManifestError> AddManifest(const nlohmann::json& manifest, SourceLayer layer,
                                                   const std::string& source_label, const std::string& script);

    // 发布。同层同键冲突/循环依赖/缺失依赖/阶段倒置/阶段不匹配/观察者带
    // 依赖——整版拒绝(带 err:: 码与人话);成功得新 revision 的冻结注册表。
    // 在途 dispatch 持旧 FrozenRegistry 的 shared_ptr,不受发布/重载影响。
    std::expected<std::shared_ptr<const FrozenRegistry>, PlanError> Publish();

    std::size_t candidate_count() const;

private:
    Options options_;
    mutable std::mutex mutex_;  // 候选表读写(Add 与 Publish/读取互斥)
    std::vector<std::shared_ptr<const MiddlewareDefinition>> candidates_;
    std::uint64_t next_revision_ = 1;
};

// ---------------------------------------------------------------------------
// 事件账接口(trajectory::v3 §4.22 hooks 事件合同的宿主外形状)。P0-A 出
// 接口 + 空实现;P0-B 接 V3Writer。次序合同:requested -> (skipped 汇总)
// -> started -> completed/failed/cancelled -> effects.applied/rejected;
// continuation.consumed 记 next 的一次性执行权(§7.1)。
// ---------------------------------------------------------------------------
struct DispatchMeta {
    std::string dispatch_id;
    std::string hook_point;
    std::uint64_t registry_revision = 0;
    std::optional<std::string> turn_id, step_id, action_id, request_id;
};

struct HandlerSnapshot {
    std::string hook_id;         // "Point/name"
    std::string definition_hash;
    std::string handler_kind;    // builtin/lua
    int definition_order = 0;
    std::string failure_policy;
};

struct InvocationMeta {
    std::string dispatch_id;
    std::string invocation_id;
    std::string hook_id;
    std::string handler_kind;
    std::string definition_hash;
    int definition_order = 0;
};

class MiddlewareEventSink {
public:
    virtual ~MiddlewareEventSink() = default;
    virtual void OnDispatchRequested(const DispatchMeta&, const std::vector<HandlerSnapshot>&) {}
    virtual void OnSkipped(const DispatchMeta&, std::string_view reason) {}
    virtual void OnInvocationStarted(const InvocationMeta&) {}
    // handler 正常返回(deny/rewrite 也是 completed,decision 说明决定)。
    virtual void OnInvocationCompleted(const InvocationMeta&, std::optional<std::string> decision,
                                       std::uint64_t duration_ms) {}
    virtual void OnInvocationFailed(const InvocationMeta&, std::string_view error_code, std::uint64_t duration_ms) {}
    virtual void OnInvocationCancelled(const InvocationMeta&, std::string_view reason) {}
    // completed != 改写已采用:候选先存,验证后 applied/rejected。
    virtual void OnEffectApplied(const InvocationMeta&, std::string_view effect_type) {}
    virtual void OnEffectRejected(const InvocationMeta&, std::string_view effect_type, std::string_view reason) {}
    virtual void OnContinuationConsumed(const InvocationMeta&) {}
};

// ---------------------------------------------------------------------------
// 派发与执行核。
// ---------------------------------------------------------------------------
struct DispatchTrigger {
    nlohmann::json input;  // 挂点专用候选副本(修改不改 session;采用经宿主)
    std::optional<std::string> origin, purpose, delivery_mode;  // 匹配条件(§4.47)
    std::optional<std::string> turn_id, step_id, action_id, request_id;
    const std::atomic<bool>* cancel = nullptr;  // 取消旗(帧边界查,Lua 灌 guard)
    // PreRequest 分段驱动(§4.36):宿主按 mutate -> freeze -> estimate ->
    // capacity 逐段放行;估算输出由宿主落稳后再进容量段的输入,估算/容量
    // 阶段没有改写权(freeze 之后候选一律拒)。不填 = 整挂点一段跑(挂点
    // 只有 Default 阶段时二者等价)。
    std::optional<Stage> stage_filter;
};

struct EffectRecord {
    std::string type;
    bool applied = false;
    std::string reject_reason;  // 未采用的原因(err::kEffectRejected 的细分人话)
    nlohmann::json payload;
};

// 一枚 handler 在本次 dispatch 的账(含跳过项——不伪造执行,也不刷屏)。
struct InvocationRecord {
    std::string key;  // "Point/name"
    std::string implementation_ref;
    std::string definition_hash;
    std::string source_label;
    std::string handler_kind;  // builtin/lua
    int definition_order = 0;
    bool observer = false;
    // completed | completed_short_circuit | denied | failed |
    // skipped_no_match | skipped_short_circuit | skipped_failed_upstream |
    // skipped_cancelled
    std::string outcome;
    bool next_consumed = false;
    int next_calls = 0;  // next 调用计(>1 的调用被拒,不增加下游执行)
    std::string error_code, detail;
    std::vector<EffectRecord> effects;
    std::uint64_t duration_ms = 0;
};

struct DispatchOutcome {
    enum class Kind { Completed, Denied, Failed };
    Kind kind = Kind::Completed;
    std::string dispatch_id;
    std::uint64_t registry_revision = 0;
    nlohmann::json value;          // 最终值(短路值或链尾产出)
    nlohmann::json adopted_input;  // 喂给链尾的输入(改写采用后;短路时 = 进入短路帧的版本)
    bool input_frozen = false;     // PreRequest:是否越过 freeze 边界(mutate 段收尾)
    int terminal_runs = 0;         // 链尾执行次数(正常 1;短路 0)
    std::string deny_code, deny_message;
    std::string error_code, error_detail;
    std::vector<InvocationRecord> records;  // 计划序(含跳过项与观察者)
    std::vector<std::string> context_appends;  // 已采用的 context.append 文本(计划序)

    bool Ok() const { return kind == Kind::Completed; }
    const InvocationRecord* FindRecord(std::string_view key) const {
        for (const auto& record : records) {
            if (record.key == key) {
                return &record;
            }
        }
        return nullptr;
    }
};

// 链尾的被围阶段:洋葱只围住一次明确阶段(§四)。P0-A 由调用方给(测试
// 的计数器 / P0-B 的接纳、发送关口);空 = 链尾原样返回输入。
using TerminalFn = std::function<nlohmann::json(const nlohmann::json& input)>;

// 持一份冻结注册表执行 dispatch。线程模型:单线程调用(与宿主主回合同
// 线程);观察者在内部短命线程里并发跑、join 后才返回。事件 sink 若非空,
// 观察者线程也会调它——sink 实现须自理互斥。
class MiddlewareDispatcher {
public:
    explicit MiddlewareDispatcher(std::shared_ptr<const FrozenRegistry> registry)
        : registry_(std::move(registry)) {}

    const FrozenRegistry& registry() const { return *registry_; }

    DispatchOutcome Dispatch(HookPoint point, const DispatchTrigger& trigger, TerminalFn terminal = nullptr,
                             MiddlewareEventSink* sink = nullptr);

    // 链上嵌套深度上限(§4.1 嵌套限制的宿主侧一档;计划长度本身也有限)。
    static constexpr int kMaxChainDepth = 256;

private:
    std::shared_ptr<const FrozenRegistry> registry_;
};

// dispatch/invocation 身份(进程内单调 + 时间戳,对账够用;不替 turnId 代班)。
std::string NextMiddlewareDispatchId();
std::string NextMiddlewareInvocationId(const std::string& dispatch_id, int order);

}  // namespace lubancode::hooks::middleware
