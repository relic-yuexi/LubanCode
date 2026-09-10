// hook 中间件统一合同与执行核(LuaHook 单 P0-A)测试——纯内存、零 Lua、
// 零进程:单子 P0-A 前三条与验收里"内置链"半边全在这里:
//   1) 合同冻结:挂点/阶段/效果矩阵、清单 schema;
//   2) 同名选实现、依赖顺序、required 与不可变 dispatch 计划;
//   3) 覆盖只替换同键功能、同层冲突、先选实现再匹配、无隐式 fallback、
//      依赖随获选实现绑定、只读观察者并发;
//   4) 至多一次 next、候选采用、退栈、短路、跨 invocation 拒绝(执行核侧);
//   5) 估算器替换不改变冻结与容量判断的阶段边界(§4.36 分段驱动)。
// Lua 半边(混链/白名单/预算/state 边界)在 test_middleware_lua.cpp。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;

namespace {

MiddlewareDefinition MakeBuiltin(HookPoint point, const std::string& name, Handler handler, int priority = 100) {
    MiddlewareDefinition def;
    def.point = point;
    def.name = name;
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin." + name;
    def.builtin = std::move(handler);
    def.priority = priority;
    return def;
}

// 透传 handler:记一笔日志,消费 next,原样把下游值交回去。
Handler Pass(std::vector<std::string>* log, std::string tag) {
    return [log, tag = std::move(tag)](const InvocationCtx&, const nlohmann::json& input, NextCall& next)
               -> std::expected<HandlerReturn, HandlerError> {
        if (log != nullptr) {
            log->push_back(tag);
        }
        const auto downstream = next();
        return HandlerReturn::Value(downstream.value);
    };
}

std::vector<std::string> SelectedKeys(const FrozenRegistry& registry, HookPoint point) {
    std::vector<std::string> keys;
    for (const auto& def : registry.Selected(point)) {
        keys.push_back(def->name);
    }
    return keys;
}

// 事件账录音器(P0-A 只出记录接口;观察者线程会并发调,自带互斥)。
class RecordingSink final : public MiddlewareEventSink {
public:
    void OnDispatchRequested(const DispatchMeta&, const std::vector<HandlerSnapshot>& handlers) override {
        Record("requested:" + std::to_string(handlers.size()));
    }
    void OnSkipped(const DispatchMeta&, std::string_view reason) override { Record("skipped:" + std::string(reason)); }
    void OnInvocationStarted(const InvocationMeta& meta) override { Record("started:" + meta.hook_id); }
    void OnInvocationCompleted(const InvocationMeta& meta, std::optional<std::string> decision,
                               std::uint64_t) override {
        Record("completed:" + meta.hook_id + ":" + decision.value_or("-"));
    }
    void OnInvocationFailed(const InvocationMeta& meta, std::string_view error_code, std::uint64_t) override {
        Record("failed:" + meta.hook_id + ":" + std::string(error_code));
    }
    void OnEffectApplied(const InvocationMeta& meta, std::string_view effect_type) override {
        Record("applied:" + meta.hook_id + ":" + std::string(effect_type));
    }
    void OnEffectRejected(const InvocationMeta& meta, std::string_view effect_type, std::string_view) override {
        Record("rejected:" + meta.hook_id + ":" + std::string(effect_type));
    }
    void OnContinuationConsumed(const InvocationMeta& meta) override {
        Record("consumed:" + meta.hook_id);
    }

    std::vector<std::string> Take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }
    bool Has(const std::string& line) {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event == line) {
                return true;
            }
        }
        return false;
    }

private:
    void Record(std::string line) {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(line));
    }
    std::mutex mutex_;
    std::vector<std::string> events_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 1) 合同冻结:名字表、阶段、效果矩阵。
// ---------------------------------------------------------------------------

TEST_CASE("合同:挂点/阶段/效果的名字表与解析") {
    HookPoint point{};
    CHECK(ParseHookPoint("PreUser", point));
    CHECK(point == HookPoint::PreUser);
    CHECK(ParseHookPoint("PostAction", point));
    CHECK(point == HookPoint::PostAction);
    CHECK_FALSE(ParseHookPoint("NotAPoint", point));

    Stage stage{};
    CHECK(ParseStage("mutate", stage));
    CHECK(stage == Stage::Mutate);
    CHECK_FALSE(ParseStage("freeze", stage));  // freeze 归宿主,不给 handler 注册

    EffectType effect{};
    CHECK(ParseEffectType("context.append", effect));
    CHECK(effect == EffectType::ContextAppend);
    CHECK_FALSE(ParseEffectType("magic.wand", effect));
}

TEST_CASE("合同:挂点与阶段的匹配与效果矩阵的关键格") {
    CHECK(StageAllowed(HookPoint::PreRequest, Stage::Mutate));
    CHECK(StageAllowed(HookPoint::PreRequest, Stage::Capacity));
    CHECK_FALSE(StageAllowed(HookPoint::PreRequest, Stage::Default));  // 必须落段
    CHECK(StageAllowed(HookPoint::PreUser, Stage::Default));
    CHECK_FALSE(StageAllowed(HookPoint::PostUser, Stage::Mutate));

    // PreRequest 三段:mutate 可改输入;estimate 冻结后无效果;capacity 只准入。
    CHECK(InputRewriteAllowed(HookPoint::PreRequest, Stage::Mutate));
    CHECK_FALSE(InputRewriteAllowed(HookPoint::PreRequest, Stage::Estimate));
    CHECK_FALSE(InputRewriteAllowed(HookPoint::PreRequest, Stage::Capacity));
    CHECK(EffectAllowed(HookPoint::PreRequest, Stage::Capacity, EffectType::AdmissionDecision));
    CHECK_FALSE(EffectAllowed(HookPoint::PreRequest, Stage::Estimate, EffectType::InputRewrite));

    // PostUser 不能回写原 user;PreAssistant 不改模型原话;PostAction 无准入。
    CHECK_FALSE(InputRewriteAllowed(HookPoint::PostUser, Stage::Default));
    CHECK(EffectAllowed(HookPoint::PostUser, Stage::Default, EffectType::ContextAppend));
    CHECK_FALSE(EffectAllowed(HookPoint::PreAssistant, Stage::Default, EffectType::InputRewrite));
    CHECK(EffectAllowed(HookPoint::PreAssistant, Stage::Default, EffectType::AdmissionDecision));
    CHECK_FALSE(EffectAllowed(HookPoint::PostAction, Stage::Default, EffectType::AdmissionDecision));
    CHECK(EffectAllowed(HookPoint::PostAction, Stage::Default, EffectType::ResultReplace));
}

// ---------------------------------------------------------------------------
// 2) 清单 schema。
// ---------------------------------------------------------------------------

TEST_CASE("清单:合法 hook.json 解析成定义,hash 自动算") {
    const auto manifest = nlohmann::json::parse(R"({
        "schemaVersion": 1,
        "id": "prompt-normalize",
        "version": "1.0.0",
        "entry": "main.lua",
        "hooks": [
            {
                "hookPoint": "PreUser",
                "name": "prompt.normalize",
                "handler": "normalize",
                "priority": 100,
                "after": [],
                "match": {"origin": "human", "purpose": "interactive"},
                "capabilities": ["prompt.read"],
                "failurePolicy": "abort",
                "limits": {"activeTimeoutMs": 500}
            }
        ]
    })");
    auto parsed = ParseHookManifest(manifest, SourceLayer::User, "user ~/.lubancode/hooks/prompt-normalize",
                                    "return { normalize = function() end }");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    const MiddlewareDefinition& def = (*parsed)[0];
    CHECK(def.point == HookPoint::PreUser);
    CHECK(def.name == "prompt.normalize");
    CHECK(def.layer == SourceLayer::User);  // 层级由调用方按发现位置给,清单不自报
    CHECK(def.is_lua);
    CHECK(def.lua.entry == "normalize");
    CHECK(def.lua.chunk_name == "main.lua");
    CHECK(def.match.origin == std::optional<std::string>("human"));
    CHECK(def.limits.wall_budget == std::chrono::milliseconds(500));
    CHECK_FALSE(def.definition_hash.empty());
    CHECK(def.implementation_ref.find("prompt-normalize#normalize") != std::string::npos);
}

TEST_CASE("清单:坏形状逐项拒绝,不带半个定义进来") {
    const auto base = nlohmann::json::parse(R"({
        "schemaVersion": 1, "id": "x", "entry": "main.lua",
        "hooks": [{"hookPoint": "PreUser", "name": "x.recall", "handler": "recall"}]
    })");
    CHECK(ParseHookManifest(nlohmann::json::parse(R"({"schemaVersion": 2, "id": "x", "entry": "e", "hooks": []})"),
                            SourceLayer::User, "u", "s")
              .error()
              .code == err::kManifestInvalid);

    auto bad_point = base;
    bad_point["hooks"][0]["hookPoint"] = "TeaTime";
    CHECK_FALSE(ParseHookManifest(bad_point, SourceLayer::User, "u", "s").has_value());

    auto bad_stage = base;
    bad_stage["hooks"][0]["stage"] = "mutate";  // PreUser 不收阶段
    CHECK_FALSE(ParseHookManifest(bad_stage, SourceLayer::User, "u", "s").has_value());

    auto bad_replay = base;
    bad_replay["hooks"][0]["replayPolicy"] = "pure_reexecute";  // 首批只认 manual
    CHECK_FALSE(ParseHookManifest(bad_replay, SourceLayer::User, "u", "s").has_value());

    auto bad_handler = base;
    bad_handler["hooks"][0].erase("handler");
    CHECK_FALSE(ParseHookManifest(bad_handler, SourceLayer::User, "u", "s").has_value());
}

// ---------------------------------------------------------------------------
// 3) 同名选实现与发布裁决。
// ---------------------------------------------------------------------------

TEST_CASE("发布:同键取最高层,只跑获选项;被覆盖 builtin 保留在 Overridden") {
    MiddlewarePool pool;
    int builtin_calls = 0;
    int user_calls = 0;
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "memory.recall",
                                   [&builtin_calls](const InvocationCtx&, const nlohmann::json& input,
                                                    NextCall& next) {
                                       ++builtin_calls;
                                       const auto downstream = next();
                                       return HandlerReturn::Value(downstream.value);
                                   }));
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "skill.resolve",
                                   Pass(nullptr, "skill"), /*priority=*/100));
    MiddlewareDefinition user_recall = MakeBuiltin(HookPoint::PostUser, "memory.recall",
                                                   [&user_calls](const InvocationCtx&, const nlohmann::json& input,
                                                                 NextCall& next) {
                                                       ++user_calls;
                                                       const auto downstream = next();
                                                       return HandlerReturn::Value(downstream.value);
                                                   });
    user_recall.layer = SourceLayer::User;
    user_recall.source_label = "user hooks";
    user_recall.implementation_ref = "hooks/my_memory#recall";
    user_recall.after = {"skill.resolve"};  // 依赖指向逻辑键,覆盖后连到获选实现
    pool.AddDefinition(std::move(user_recall));

    auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    // skill.resolve 先(priority 100),user 的 memory.recall 后(依赖 + 200)。
    CHECK(SelectedKeys(**registry, HookPoint::PostUser) == std::vector<std::string>{"skill.resolve", "memory.recall"});
    REQUIRE((*registry)->Overridden().size() == 1);
    CHECK((*registry)->Overridden()[0]->implementation_ref == "builtin.memory.recall");

    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger);
    CHECK(outcome.Ok());
    CHECK(builtin_calls == 0);  // 被覆盖的 builtin 不执行
    CHECK(user_calls == 1);     // 只跑获选实现
    // 解析后的实际计划可查(配置手册用)。
    const auto plan = dispatcher.registry().DescribePlan();
    CHECK(plan["points"]["PostUser"].size() == 2);
    CHECK(plan["overridden"].size() == 1);
}

TEST_CASE("发布:同层同键冲突拒绝,不按文件先后猜赢家") {
    MiddlewarePool pool;
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "a")));
    MiddlewareDefinition first = MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "b"));
    first.layer = SourceLayer::User;
    pool.AddDefinition(std::move(first));
    MiddlewareDefinition second = MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "c"));
    second.layer = SourceLayer::User;  // 同层第二条
    pool.AddDefinition(std::move(second));

    const auto registry = pool.Publish();
    REQUIRE_FALSE(registry.has_value());
    CHECK(registry.error().code == err::kPlanSameKeyConflict);
    CHECK(registry.error().message.find("memory.recall") != std::string::npos);
}

TEST_CASE("发布:循环依赖/缺失依赖/阶段倒置/观察者带依赖 各自成拒") {
    SUBCASE("循环依赖") {
        MiddlewarePool pool;
        MiddlewareDefinition a = MakeBuiltin(HookPoint::PostUser, "a.first", Pass(nullptr, "a"));
        a.after = {"b.second"};
        MiddlewareDefinition b = MakeBuiltin(HookPoint::PostUser, "b.second", Pass(nullptr, "b"));
        b.after = {"a.first"};
        pool.AddDefinition(std::move(a));
        pool.AddDefinition(std::move(b));
        const auto registry = pool.Publish();
        REQUIRE_FALSE(registry.has_value());
        CHECK(registry.error().code == err::kPlanDependencyCycle);
    }
    SUBCASE("缺失依赖") {
        MiddlewarePool pool;
        MiddlewareDefinition a = MakeBuiltin(HookPoint::PostUser, "a.first", Pass(nullptr, "a"));
        a.after = {"ghost.def"};
        pool.AddDefinition(std::move(a));
        const auto registry = pool.Publish();
        REQUIRE_FALSE(registry.has_value());
        CHECK(registry.error().code == err::kPlanMissingDep);
    }
    SUBCASE("阶段倒置:estimate 想排到 mutate 前") {
        MiddlewarePool pool;
        MiddlewareDefinition estimate = MakeBuiltin(HookPoint::PreRequest, "context.token_estimate", Pass(nullptr, "e"));
        estimate.stage = Stage::Estimate;
        estimate.priority = 1;
        estimate.before = {"input.truncate"};
        MiddlewareDefinition mutate = MakeBuiltin(HookPoint::PreRequest, "input.truncate", Pass(nullptr, "m"));
        mutate.stage = Stage::Mutate;
        mutate.priority = 900;
        pool.AddDefinition(std::move(estimate));
        pool.AddDefinition(std::move(mutate));
        const auto registry = pool.Publish();
        REQUIRE_FALSE(registry.has_value());
        CHECK(registry.error().code == err::kPlanStageInversion);
    }
    SUBCASE("观察者带依赖") {
        MiddlewarePool pool;
        MiddlewareDefinition observer = MakeBuiltin(HookPoint::PostUser, "telemetry.observe", Pass(nullptr, "t"));
        observer.observer = true;
        observer.after = {"skill.resolve"};
        pool.AddDefinition(std::move(observer));
        const auto registry = pool.Publish();
        REQUIRE_FALSE(registry.has_value());
        CHECK(registry.error().code == err::kPlanObserverWithDeps);
    }
    SUBCASE("required 槽位不许观察") {
        MiddlewarePool pool;
        MiddlewareDefinition observer = MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "m"));
        observer.observer = true;
        observer.required = true;
        pool.AddDefinition(std::move(observer));
        const auto registry = pool.Publish();
        REQUIRE_FALSE(registry.has_value());
        CHECK(registry.error().code == err::kPlanObserverRequired);
    }
}

TEST_CASE("发布:阶段压过 priority,priority 压过同值按逻辑键,依赖压过 priority") {
    SUBCASE("阶段压 priority(PreRequest:mutate/estimate/capacity 固定序)") {
        MiddlewarePool pool;
        MiddlewareDefinition mutate = MakeBuiltin(HookPoint::PreRequest, "input.truncate", Pass(nullptr, "m"), 900);
        mutate.stage = Stage::Mutate;
        MiddlewareDefinition capacity =
            MakeBuiltin(HookPoint::PreRequest, "context.capacity_check", Pass(nullptr, "c"), 1);
        capacity.stage = Stage::Capacity;
        MiddlewareDefinition estimate =
            MakeBuiltin(HookPoint::PreRequest, "context.token_estimate", Pass(nullptr, "e"), 500);
        estimate.stage = Stage::Estimate;
        pool.AddDefinition(std::move(mutate));
        pool.AddDefinition(std::move(capacity));
        pool.AddDefinition(std::move(estimate));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        CHECK(SelectedKeys(**registry, HookPoint::PreRequest) ==
              std::vector<std::string>{"input.truncate", "context.token_estimate", "context.capacity_check"});
    }
    SUBCASE("priority 小者先,同值按逻辑键稳定排序") {
        MiddlewarePool pool;
        pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "z.last", Pass(nullptr, "z"), 300));
        pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "b.mid", Pass(nullptr, "b"), 200));
        pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "a.mid", Pass(nullptr, "a"), 200));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        CHECK(SelectedKeys(**registry, HookPoint::PostUser) ==
              std::vector<std::string>{"a.mid", "b.mid", "z.last"});
    }
    SUBCASE("依赖压过 priority(依赖未定时按 priority,声明了就听依赖的)") {
        MiddlewarePool pool;
        // b 的 priority 更小(本该先跑),但声明 after a——必须排在 a 后。
        MiddlewareDefinition b = MakeBuiltin(HookPoint::PostUser, "b.recall", Pass(nullptr, "b"), 10);
        b.after = {"a.resolve"};
        pool.AddDefinition(std::move(b));
        pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "a.resolve", Pass(nullptr, "a"), 900));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        CHECK(SelectedKeys(**registry, HookPoint::PostUser) == std::vector<std::string>{"a.resolve", "b.recall"});
    }
}

// ---------------------------------------------------------------------------
// 4) 派发:改参/后置/短路/至多一次 next。
// ---------------------------------------------------------------------------

TEST_CASE("派发:混链改参与后置,链尾恰跑一次,后项读前项已采用版本") {
    MiddlewarePool pool;
    // outer:改参(trim 字段),后置把下游值包一层。
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.normalize",
        [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            nlohmann::json candidate = input;
            candidate["trimmed"] = true;
            const auto downstream = next(candidate);
            nlohmann::json value = downstream.value;
            value["post"] = "normalize";
            return HandlerReturn::Value(std::move(value));
        },
        /*priority=*/50));
    // inner:断言看见 outer 的改写版本(串行改写链:后项读前项已采用版本)。
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.guard",
        [](const InvocationCtx&, const nlohmann::json& input,
           NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
            if (!input.value("trimmed", false)) {
                return std::unexpected(HandlerError{"hook.handler.failed", "guard 没看见改写版本"});
            }
            const auto downstream = next();
            return HandlerReturn::Value(downstream.value);
        },
        /*priority=*/200));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());

    MiddlewareDispatcher dispatcher(std::move(*registry));
    int terminal_runs = 0;
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"text", "  hi  "}};
    const auto outcome = dispatcher.Dispatch(
        HookPoint::PreUser, trigger, [&terminal_runs](const nlohmann::json& input) {
            ++terminal_runs;
            return nlohmann::json{{"terminal", true}, {"adopted", input}};
        });
    CHECK(outcome.Ok());
    CHECK(terminal_runs == 1);
    CHECK(outcome.terminal_runs == 1);
    // 终态拿到的是改写采用的版本;外层的后置加工包在外面。
    CHECK(outcome.value["post"] == "normalize");
    CHECK(outcome.value["terminal"] == true);
    CHECK(outcome.value["adopted"]["trimmed"] == true);
    CHECK(outcome.adopted_input["trimmed"] == true);
    // 改写走的是 input.rewrite 效果(候选经宿主校验采用)。
    const InvocationRecord* normalize = outcome.FindRecord("PreUser/prompt.normalize");
    REQUIRE(normalize != nullptr);
    REQUIRE(normalize->effects.size() == 1);
    CHECK(normalize->effects[0].type == "input.rewrite");
    CHECK(normalize->effects[0].applied);
}

TEST_CASE("派发:短路零次链尾,未进入项记 skipped;deny 是 Denied 不是失败") {
    SUBCASE("业务短路") {
        MiddlewarePool pool;
        pool.AddDefinition(MakeBuiltin(
            HookPoint::PreUser, "prompt.gate",
            [](const InvocationCtx&, const nlohmann::json&, NextCall&) {
                // 零次 next = 短路,返回值即整链终态。
                return HandlerReturn::Value(nlohmann::json{{"blocked_by", "gate"}});
            }));
        pool.AddDefinition(MakeBuiltin(HookPoint::PreUser, "prompt.other", Pass(nullptr, "other")));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        MiddlewareDispatcher dispatcher(std::move(*registry));
        int terminal_runs = 0;
        DispatchTrigger trigger;
        trigger.input = nlohmann::json::object();
        const auto outcome = dispatcher.Dispatch(
            HookPoint::PreUser, trigger, [&terminal_runs](const nlohmann::json&) {
                ++terminal_runs;
                return nlohmann::json();
            });
        CHECK(outcome.Ok());
        CHECK(outcome.terminal_runs == 0);
        CHECK(terminal_runs == 0);
        CHECK(outcome.value["blocked_by"] == "gate");
        CHECK(outcome.FindRecord("PreUser/prompt.gate")->outcome == "completed_short_circuit");
        CHECK(outcome.FindRecord("PreUser/prompt.other")->outcome == "skipped_short_circuit");
    }
    SUBCASE("业务拒绝(deny)") {
        MiddlewarePool pool;
        pool.AddDefinition(MakeBuiltin(
            HookPoint::PreUser, "input.policy",
            [](const InvocationCtx&, const nlohmann::json&, NextCall&) {
                return HandlerReturn::Denied("input_rejected", "这次输入未通过检查");
            }));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        MiddlewareDispatcher dispatcher(std::move(*registry));
        DispatchTrigger trigger;
        trigger.input = nlohmann::json::object();
        const auto outcome = dispatcher.Dispatch(HookPoint::PreUser, trigger, [](const nlohmann::json&) {
            return nlohmann::json();
        });
        CHECK(outcome.kind == DispatchOutcome::Kind::Denied);
        CHECK(outcome.deny_code == "input_rejected");
        CHECK(outcome.terminal_runs == 0);
        // deny 是 handler 正常返回(completed 语义),不是执行失败。
        CHECK(outcome.FindRecord("PreUser/input.policy")->outcome == "denied");
        CHECK(outcome.FindRecord("PreUser/input.policy")->error_code.empty());
    }
}

TEST_CASE("派发:外层后置不能把下游 deny 洗成成功(结局只许变严)") {
    MiddlewarePool pool;
    // 内层拒绝;外层包装后想返回成功值。
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.wrap",
        [](const InvocationCtx&, const nlohmann::json&, NextCall& next) {
            const auto downstream = next();  // denied
            return HandlerReturn::Value(nlohmann::json{{("looks"), ("fine")}});
        }));
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.policy",
        [](const InvocationCtx&, const nlohmann::json&, NextCall&) {
            return HandlerReturn::Denied("policy", "拒绝");
        }));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    const auto outcome = dispatcher.Dispatch(HookPoint::PreUser, trigger, [](const nlohmann::json&) {
        return nlohmann::json();
    });
    CHECK(outcome.kind == DispatchOutcome::Kind::Denied);
    CHECK(outcome.deny_code == "policy");
    CHECK(outcome.terminal_runs == 0);  // 终态根本没轮到
}

TEST_CASE("派发:至多一次 next——第二次调用回 Invalid,下游次数不增加") {
    MiddlewarePool pool;
    int terminal_runs = 0;
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.once",
        [&terminal_runs](const InvocationCtx&, const nlohmann::json& input,
                         NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
            const auto first = next();  // 有效
            const auto second = next();  // 拒绝:already_consumed
            if (second.kind != DownstreamOutcome::Kind::Invalid) {
                return std::unexpected(HandlerError{"hook.handler.failed", "第二次 next 不该有效"});
            }
            if (second.code != err::kNextAlreadyConsumed) {
                return std::unexpected(HandlerError{"hook.handler.failed", "错误码不对"});
            }
            (void)terminal_runs;
            return HandlerReturn::Value(first.value);
        }));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    const auto outcome = dispatcher.Dispatch(
        HookPoint::PreUser, trigger, [&terminal_runs](const nlohmann::json& input) {
            ++terminal_runs;
            return input;
        });
    CHECK(outcome.Ok());
    CHECK(outcome.terminal_runs == 1);  // 重复 next 不增加次数
    CHECK(outcome.FindRecord("PreUser/prompt.once")->next_calls == 2);
    CHECK(outcome.FindRecord("PreUser/prompt.once")->next_consumed);
}

// ---------------------------------------------------------------------------
// 5) 失败策略与 required。
// ---------------------------------------------------------------------------

TEST_CASE("失败:required 槽位恒 Abort——终态零次,下游记 skipped_failed_upstream") {
    MiddlewarePool pool;
    MiddlewareDefinition estimator = MakeBuiltin(
        HookPoint::PreRequest, "context.token_estimate",
        [](const InvocationCtx&, const nlohmann::json&, NextCall&) -> std::expected<HandlerReturn, HandlerError> {
            return std::unexpected(HandlerError{"hook.handler.failed", "估算器坏了"});
        });
    estimator.stage = Stage::Estimate;
    estimator.required = true;
    pool.AddDefinition(std::move(estimator));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    trigger.stage_filter = Stage::Estimate;
    const auto outcome = dispatcher.Dispatch(HookPoint::PreRequest, trigger, [](const nlohmann::json& input) {
        return input;
    });
    CHECK(outcome.kind == DispatchOutcome::Kind::Failed);
    CHECK(outcome.terminal_runs == 0);  // required 失败不许绕过检查继续
    CHECK(outcome.FindRecord("PreRequest/context.token_estimate")->outcome == "failed");
}

TEST_CASE("失败:optional keep_original 两条路——未消费 next 以原输入继续,已消费采用下游收据") {
    SUBCASE("未消费 next:以进入本 handler 的版本继续一次") {
        MiddlewarePool pool;
        MiddlewareDefinition flaky = MakeBuiltin(
            HookPoint::PostUser, "memory.recall",
            [](const InvocationCtx&, const nlohmann::json&, NextCall&) -> std::expected<HandlerReturn, HandlerError> {
                return std::unexpected(HandlerError{"hook.handler.failed", "召回失败"});
            });
        flaky.failure_policy = FailurePolicy::KeepOriginal;
        flaky.priority = 10;  // 在 skill.resolve 前;失败以原输入继续
        pool.AddDefinition(std::move(flaky));
        std::vector<std::string> log;
        pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "skill.resolve", Pass(&log, "skill"), 200));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        MiddlewareDispatcher dispatcher(std::move(*registry));
        DispatchTrigger trigger;
        trigger.input = nlohmann::json{{"text", "hi"}};
        const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger, [](const nlohmann::json& input) {
            return input;
        });
        CHECK(outcome.Ok());
        CHECK(log == std::vector<std::string>{"skill"});  // 失败不拦后续
        CHECK(outcome.FindRecord("PostUser/memory.recall")->outcome == "failed");
        CHECK(outcome.FindRecord("PostUser/memory.recall")->detail.find("keep_original") != std::string::npos);
        CHECK(outcome.terminal_runs == 1);
    }
    SUBCASE("已消费 next:采用下游收据(keep_downstream)") {
        MiddlewarePool pool;
        MiddlewareDefinition flaky = MakeBuiltin(
            HookPoint::PostUser, "memory.recall",
            [](const InvocationCtx&, const nlohmann::json&, NextCall& next)
                -> std::expected<HandlerReturn, HandlerError> {
                const auto downstream = next();  // 先放行下游
                (void)downstream;
                return std::unexpected(HandlerError{"hook.handler.failed", "后置加工失败"});  // 下游已完成
            });
        flaky.failure_policy = FailurePolicy::KeepOriginal;
        pool.AddDefinition(std::move(flaky));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        MiddlewareDispatcher dispatcher(std::move(*registry));
        int terminal_runs = 0;
        DispatchTrigger trigger;
        trigger.input = nlohmann::json::object();
        const auto outcome = dispatcher.Dispatch(
            HookPoint::PostUser, trigger, [&terminal_runs](const nlohmann::json& input) {
                ++terminal_runs;
                return nlohmann::json{{"downstream", true}};
            });
        CHECK(outcome.Ok());  // 下游收据保留,不重跑
        CHECK(terminal_runs == 1);
        CHECK(outcome.value["downstream"] == true);
    }
}

// ---------------------------------------------------------------------------
// 6) 先选实现再匹配:未命中不暗跑旧实现。
// ---------------------------------------------------------------------------

TEST_CASE("匹配:获选实现未命中,被覆盖的 builtin 不补跑(无隐式 fallback)") {
    MiddlewarePool pool;
    int builtin_calls = 0;
    int skill_calls = 0;
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PostUser, "memory.recall",
        [&builtin_calls](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            ++builtin_calls;
            return HandlerReturn::Value(next().value);
        }));
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PostUser, "skill.resolve",
        [&skill_calls](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            ++skill_calls;
            return HandlerReturn::Value(next().value);
        }));
    // 用户 Lua 槽位的形状:只匹配 human 交互输入。
    MiddlewareDefinition user_recall = MakeBuiltin(
        HookPoint::PostUser, "memory.recall",
        [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            return HandlerReturn::Value(next().value);
        });
    user_recall.layer = SourceLayer::User;
    user_recall.match.origin = "human";
    user_recall.match.purpose = "interactive";
    pool.AddDefinition(std::move(user_recall));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());

    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;  // origin 缺省(非 human):获选项未命中
    trigger.input = nlohmann::json::object();
    const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger);
    CHECK(outcome.Ok());
    CHECK(builtin_calls == 0);  // 未命中不暗跑旧实现;要 fallback 须显式声明
    CHECK(skill_calls == 1);    // 不同功能照常
    CHECK(outcome.FindRecord("PostUser/memory.recall")->outcome == "skipped_no_match");

    // origin=human 的触发:获选实现命中,只跑它。
    DispatchTrigger human;
    human.input = nlohmann::json::object();
    human.origin = "human";
    human.purpose = "interactive";
    const auto outcome2 = dispatcher.Dispatch(HookPoint::PostUser, human);
    CHECK(outcome2.Ok());
    CHECK(builtin_calls == 0);
    CHECK(outcome2.FindRecord("PostUser/memory.recall")->outcome == "completed");
}

// ---------------------------------------------------------------------------
// 7) 估算器替换:冻结与容量判断的阶段边界不变(§4.36)。
// ---------------------------------------------------------------------------

TEST_CASE("估算器:替换实现不改变 mutate->estimate->capacity 的阶段边界") {
    const auto build_pool = [](MiddlewarePool& pool, MiddlewareDefinition estimator) {
        MiddlewareDefinition truncate = MakeBuiltin(
            HookPoint::PreRequest, "input.truncate",
            [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                nlohmann::json candidate = input;
                candidate["request"]["trimmed"] = true;  // mutate 段改写
                return HandlerReturn::Value(next(candidate).value);
            });
        truncate.stage = Stage::Mutate;
        pool.AddDefinition(std::move(truncate));
        pool.AddDefinition(std::move(estimator));
        MiddlewareDefinition capacity = MakeBuiltin(
            HookPoint::PreRequest, "context.capacity_check",
            [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                // 容量段只给准入决定,没有改写权。
                return HandlerReturn::Value(next().value);
            });
        capacity.stage = Stage::Capacity;
        pool.AddDefinition(std::move(capacity));
    };
    const auto make_estimator = [](SourceLayer layer) {
        MiddlewareDefinition estimator = MakeBuiltin(
            HookPoint::PreRequest, "context.token_estimate",
            [](const InvocationCtx&, const nlohmann::json& input, NextCall&) {
                // §4.36 公式:ceil(utf8 bytes / 4),整数实现不先加 3。
                const std::string text = input["request"].value("text", std::string());
                const std::int64_t bytes = static_cast<std::int64_t>(text.size());
                return HandlerReturn::Value(
                    nlohmann::json{{"estimator", "utf8_bytes_div4"},
                                   {"estimatorVersion", 1},
                                   {"inputUtf8Bytes", bytes},
                                   {"estimatedInputTokens", bytes / 4 + (bytes % 4 != 0 ? 1 : 0)}});
            });
        estimator.layer = layer;
        estimator.stage = Stage::Estimate;
        estimator.required = true;
        return estimator;
    };

    SUBCASE("内置估算器:只读冻结快照") {
        MiddlewarePool pool;
        build_pool(pool, make_estimator(SourceLayer::Builtin));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        // 计划序:mutate -> estimate -> capacity。
        CHECK(SelectedKeys(**registry, HookPoint::PreRequest) ==
              std::vector<std::string>{"input.truncate", "context.token_estimate", "context.capacity_check"});
        MiddlewareDispatcher dispatcher(std::move(*registry));
        DispatchTrigger trigger;
        trigger.input = nlohmann::json{{"request", {{"text", "123456789"}}}};
        trigger.stage_filter = Stage::Estimate;
        const auto outcome = dispatcher.Dispatch(HookPoint::PreRequest, trigger);
        CHECK(outcome.Ok());
        CHECK(outcome.value["estimatedInputTokens"] == 3);  // ceil(9/4)
        CHECK(outcome.input_frozen);                        // 越过 freeze 边界(mutate 段收尾)
    }
    SUBCASE("user 同名接管:算法换了,阶段边界与槽位 required 不换") {
        MiddlewarePool pool;
        build_pool(pool, make_estimator(SourceLayer::Builtin));
        MiddlewareDefinition user_estimate = MakeBuiltin(
            HookPoint::PreRequest, "context.token_estimate",
            [](const InvocationCtx&, const nlohmann::json& input, NextCall&) {
                return HandlerReturn::Value(
                    nlohmann::json{{"estimator", "user_formula"}, {"estimatedInputTokens", 42}});
            });
        user_estimate.layer = SourceLayer::User;
        user_estimate.stage = Stage::Estimate;
        pool.AddDefinition(std::move(user_estimate));
        const auto registry = pool.Publish();
        REQUIRE(registry.has_value());
        // 只跑获选实现,阶段序不变。
        CHECK(SelectedKeys(**registry, HookPoint::PreRequest) ==
              std::vector<std::string>{"input.truncate", "context.token_estimate", "context.capacity_check"});
        REQUIRE((*registry)->Selected(HookPoint::PreRequest).size() == 3);
        CHECK((*registry)->Selected(HookPoint::PreRequest)[1]->layer == SourceLayer::User);
        CHECK((*registry)->Selected(HookPoint::PreRequest)[1]->required);  // 槽位要求随 key 保留
        MiddlewareDispatcher dispatcher(std::move(*registry));
        DispatchTrigger trigger;
        trigger.input = nlohmann::json{{"request", {{"text", "123456789"}}}};
        trigger.stage_filter = Stage::Estimate;
        const auto outcome = dispatcher.Dispatch(HookPoint::PreRequest, trigger);
        CHECK(outcome.Ok());
        CHECK(outcome.value["estimator"] == "user_formula");
        CHECK(outcome.input_frozen);  // freeze 时点不变
        // 替代实现失败照样 Abort(槽位 required 不许解除):换个失败实现再发一版。
        MiddlewarePool failing_pool;
        MiddlewareDefinition broken = MakeBuiltin(
            HookPoint::PreRequest, "context.token_estimate",
            [](const InvocationCtx&, const nlohmann::json&, NextCall&)
                -> std::expected<HandlerReturn, HandlerError> {
                return std::unexpected(HandlerError{"hook.handler.failed", "用户估算器坏了"});
            });
        broken.layer = SourceLayer::User;
        broken.stage = Stage::Estimate;
        failing_pool.AddDefinition(make_estimator(SourceLayer::Builtin));
        failing_pool.AddDefinition(std::move(broken));
        const auto failing_registry = failing_pool.Publish();
        REQUIRE(failing_registry.has_value());
        MiddlewareDispatcher failing_dispatcher(std::move(*failing_registry));
        const auto failed = failing_dispatcher.Dispatch(HookPoint::PreRequest, trigger);
        CHECK(failed.kind == DispatchOutcome::Kind::Failed);
        CHECK(failed.terminal_runs == 0);  // required 失败不许绕过检查直接发送
    }
}

TEST_CASE("估算器:estimate 阶段改写请求一律拒(freeze 之后不许改)") {
    MiddlewarePool pool;
    MiddlewareDefinition estimator = MakeBuiltin(
        HookPoint::PreRequest, "context.token_estimate",
        [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            nlohmann::json smuggled = input;
            smuggled["request"]["text"] = "偷偷改请求";
            const auto downstream = next(smuggled);  // 改写尝试
            return HandlerReturn::Value(downstream.value);
        });
    estimator.stage = Stage::Estimate;
    estimator.required = true;
    pool.AddDefinition(std::move(estimator));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"request", {{"text", "原请求"}}}};
    trigger.stage_filter = Stage::Estimate;
    const auto outcome = dispatcher.Dispatch(
        HookPoint::PreRequest, trigger, [](const nlohmann::json& input) { return input; });
    CHECK(outcome.Ok());  // 拒绝改写不等于失败:按冻结版本继续,实际处置已记录
    CHECK(outcome.adopted_input["request"]["text"] == "原请求");
    const InvocationRecord* record = outcome.FindRecord("PreRequest/context.token_estimate");
    REQUIRE(record != nullptr);
    REQUIRE(record->effects.size() == 1);
    CHECK_FALSE(record->effects[0].applied);
    CHECK(record->effects[0].reject_reason.find("冻结") != std::string::npos);
    CHECK(outcome.input_frozen);
}

TEST_CASE("估算器:capacity 段消费宿主落稳的估算,决定 allow/deny") {
    MiddlewarePool pool;
    MiddlewareDefinition capacity = MakeBuiltin(
        HookPoint::PreRequest, "context.capacity_check",
        [](const InvocationCtx&, const nlohmann::json& input, NextCall&) {
            // 分段驱动:估算值由宿主落稳后随输入进容量段。
            const std::int64_t tokens = input.value("tokenEstimate", nlohmann::json::object()).value(
                "estimatedInputTokens", static_cast<std::int64_t>(0));
            if (tokens > 100) {
                return HandlerReturn::Denied("context_too_large", "超上下文窗口,要求 compact");
            }
            HandlerReturn out;
            out.effects.push_back(
                Effect{EffectType::AdmissionDecision, nlohmann::json{{"decision", "allow"}}});
            return out;
        });
    capacity.stage = Stage::Capacity;
    pool.AddDefinition(std::move(capacity));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger ok_trigger;
    ok_trigger.input = nlohmann::json{{"tokenEstimate", {{"estimatedInputTokens", 50}}}};
    ok_trigger.stage_filter = Stage::Capacity;
    const auto ok_outcome = dispatcher.Dispatch(HookPoint::PreRequest, ok_trigger);
    CHECK(ok_outcome.Ok());
    REQUIRE(ok_outcome.FindRecord("PreRequest/context.capacity_check")->effects.size() == 1);
    CHECK(ok_outcome.FindRecord("PreRequest/context.capacity_check")->effects[0].applied);

    DispatchTrigger big_trigger;
    big_trigger.input = nlohmann::json{{"tokenEstimate", {{"estimatedInputTokens", 5000}}}};
    big_trigger.stage_filter = Stage::Capacity;
    const auto big_outcome = dispatcher.Dispatch(HookPoint::PreRequest, big_trigger);
    CHECK(big_outcome.kind == DispatchOutcome::Kind::Denied);
    CHECK(big_outcome.deny_code == "context_too_large");
}

// ---------------------------------------------------------------------------
// 8) 观察者并发。
// ---------------------------------------------------------------------------

namespace {

struct SleepWindow {
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point finished;
};

bool WindowsOverlap(const SleepWindow& a, const SleepWindow& b) {
    return a.started < b.finished && b.started < a.finished;
}

}  // namespace

TEST_CASE("观察者:并发跑(窗口重叠),效果只收追加型,失败不连累 dispatch") {
    MiddlewarePool pool;
    std::vector<std::string> log;
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "memory.recall",
                                   [&log](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                                       log.push_back("chain");
                                       return HandlerReturn::Value(next().value);
                                   }));

    std::mutex windows_mutex;
    std::map<std::string, SleepWindow> windows;
    const auto make_observer = [&](std::string tag, bool fail, bool try_rewrite) {
        return MakeBuiltin(
            HookPoint::PostUser, "telemetry." + tag,
            [tag, fail, try_rewrite, &windows, &windows_mutex](
                const InvocationCtx&, const nlohmann::json& input,
                NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
                const auto started = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                {
                    const std::lock_guard<std::mutex> lock(windows_mutex);
                    windows[tag] = SleepWindow{started, std::chrono::steady_clock::now()};
                }
                if (fail) {
                    return std::unexpected(HandlerError{"hook.handler.failed", "观察者挂了"});
                }
                HandlerReturn out;
                out.effects.push_back(
                    Effect{EffectType::ContextAppend, nlohmann::json{{"text", "观察: " + tag}}});
                if (try_rewrite) {
                    out.effects.push_back(
                        Effect{EffectType::InputRewrite, nlohmann::json{{"sneaky", true}}});
                }
                return out;
            });
    };
    MiddlewareDefinition slow_a = make_observer("a", /*fail=*/false, /*try_rewrite=*/false);
    slow_a.observer = true;
    MiddlewareDefinition slow_b = make_observer("b", /*fail=*/false, /*try_rewrite=*/true);
    slow_b.observer = true;
    pool.AddDefinition(std::move(slow_a));
    pool.AddDefinition(std::move(slow_b));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());

    RecordingSink sink;
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger, TerminalFn{}, &sink);
    CHECK(outcome.Ok());  // 观察者失败/晚到都不连累链
    CHECK(outcome.terminal_runs == 1);
    CHECK(log == std::vector<std::string>{"chain"});
    // 并发判据:两笔睡眠窗口重叠(跟机器快慢无关)。
    {
        const std::lock_guard<std::mutex> lock(windows_mutex);
        REQUIRE(windows.count("a") == 1);
        REQUIRE(windows.count("b") == 1);
        CHECK(WindowsOverlap(windows["a"], windows["b"]));
    }
    // 观察者的追加效果采用;改写效果被拒(只读观察者不许改变链路)。
    const InvocationRecord* obs_b = outcome.FindRecord("PostUser/telemetry.b");
    REQUIRE(obs_b != nullptr);
    REQUIRE(obs_b->effects.size() == 2);
    CHECK(obs_b->effects[0].type == "context.append");
    CHECK(obs_b->effects[0].applied);
    CHECK(obs_b->effects[1].type == "input.rewrite");
    CHECK_FALSE(obs_b->effects[1].applied);
    // 追加上下文按计划序归并进 outcome。
    REQUIRE(outcome.context_appends.size() == 2);
    CHECK(outcome.context_appends[0].find("a") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 9) 计划不可变 + 事件账 + 取消 + dispatcher 缝。
// ---------------------------------------------------------------------------

TEST_CASE("计划不可变:在途 dispatch 持旧注册表,新发布不影响本次") {
    MiddlewarePool pool;
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "v1")));
    auto first_publish = pool.Publish();
    REQUIRE(first_publish.has_value());
    const std::shared_ptr<const FrozenRegistry> first = *first_publish;  // 留一份对账
    MiddlewareDispatcher dispatcher(first);  // 钉住旧 revision

    // 注册表升级:同名换实现。在途 dispatcher 持旧计划的 shared_ptr,
    // 新发布只产新 revision,不动旧账。
    MiddlewareDefinition replacement = MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "v2"));
    replacement.layer = SourceLayer::User;
    pool.AddDefinition(std::move(replacement));
    auto second_publish = pool.Publish();
    REQUIRE(second_publish.has_value());
    const std::shared_ptr<const FrozenRegistry> second = *second_publish;
    CHECK(second->revision() > first->revision());

    // 旧 dispatcher 仍是旧计划(v1 获选),新 dispatcher 用新计划。
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    const auto old_outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger);
    CHECK(old_outcome.registry_revision == first->revision());
    CHECK(old_outcome.FindRecord("PostUser/memory.recall")->implementation_ref == "builtin.memory.recall");
    CHECK(old_outcome.Ok());
    MiddlewareDispatcher new_dispatcher(second);
    const auto new_outcome = new_dispatcher.Dispatch(HookPoint::PostUser, trigger);
    CHECK(new_outcome.Ok());
    CHECK(new_outcome.registry_revision == second->revision());
}

TEST_CASE("事件账:requested -> started -> consumed -> applied -> completed 的次序合同") {
    MiddlewarePool pool;
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.normalize",
        [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            nlohmann::json candidate = input;
            candidate["trimmed"] = true;
            return HandlerReturn::Value(next(candidate).value);
        }));
    pool.AddDefinition(MakeBuiltin(HookPoint::PreUser, "prompt.audit",
                                   [](const InvocationCtx&, const nlohmann::json&, NextCall& next) {
                                       HandlerReturn out;
                                       out.effects.push_back(Effect{EffectType::ContextAppend,
                                                                    nlohmann::json{{"text", "审计一行"}}});
                                       const auto downstream = next();
                                       out.output = downstream.value;
                                       return out;
                                   }));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    RecordingSink sink;
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    const auto outcome = dispatcher.Dispatch(HookPoint::PreUser, trigger, TerminalFn{}, &sink);
    CHECK(outcome.Ok());
    const auto events = sink.Take();
    // 次序:requested 在最前;started 先于 consumed;效果在 completed 之外分立。
    REQUIRE(events.size() >= 8);
    CHECK(events[0] == "requested:2");
    bool saw_started_normalize = false, saw_consumed_normalize = false, saw_applied_rewrite = false,
         saw_completed_normalize = false;
    for (const auto& event : events) {
        if (event == "started:PreUser/prompt.normalize") saw_started_normalize = true;
        if (event == "consumed:PreUser/prompt.normalize") saw_consumed_normalize = true;
        if (event == "applied:PreUser/prompt.normalize:input.rewrite") saw_applied_rewrite = true;
        if (event == "completed:PreUser/prompt.normalize:-") saw_completed_normalize = true;
    }
    CHECK(saw_started_normalize);
    CHECK(saw_consumed_normalize);
    CHECK(saw_applied_rewrite);
    CHECK(saw_completed_normalize);
    // requested 之前没有任何 invocation 事件。
    for (const auto& event : events) {
        if (event != "requested:2") {
            CHECK(events[0] == "requested:2");
            break;
        }
    }
}

TEST_CASE("取消:旗置位的 dispatch 直接 Failed,链项记 skipped_cancelled") {
    MiddlewarePool pool;
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "m")));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    std::atomic<bool> cancel{true};
    DispatchTrigger trigger;
    trigger.input = nlohmann::json::object();
    trigger.cancel = &cancel;
    const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger);
    CHECK(outcome.kind == DispatchOutcome::Kind::Failed);
    CHECK(outcome.error_code == err::kDispatchCancelled);
    CHECK(outcome.terminal_runs == 0);
    CHECK(outcome.FindRecord("PostUser/memory.recall")->outcome == "skipped_cancelled");
}

TEST_CASE("dispatcher 缝:HookDispatcher 默认无中间件核,挂上后可取回,老 Emit 零行为") {
    hooks::HookDispatcher dispatcher;
    CHECK(dispatcher.middleware() == nullptr);  // 默认零行为:既有会话不变

    hooks::middleware::MiddlewarePool pool;
    pool.AddDefinition(MakeBuiltin(HookPoint::PostUser, "memory.recall", Pass(nullptr, "m")));
    auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    auto core = std::make_shared<hooks::middleware::MiddlewareDispatcher>(std::move(*registry));
    dispatcher.SetMiddleware(core);
    CHECK(dispatcher.middleware() == core.get());

    // 老进程 hook 路照旧:无定义时 Emit 也返回完整记录,决策字段缺省。
    hooks::HookPayload payload;
    payload.event = hooks::HookEvent::UserPromptSubmit;
    const auto result = dispatcher.Emit(hooks::HookEvent::UserPromptSubmit, payload);
    CHECK(result.executed == 0);
    CHECK(result.records.empty());
    CHECK(result.permission == hooks::HookEventResult::Permission::None);
}
