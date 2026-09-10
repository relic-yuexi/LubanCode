// 统一 runtime 派发点(LuaHook 单 P0-B):PreUser/PostUser/PreRequest 三段
// 经 HookDispatcher::SetMiddleware 接线缝;零注册 = 零改写(与迁移前等价);
// steer/followup 匹配;取消旗;输出预留进容量判断;快照投影。CLI/one-shot/
// app-server 共用同一函数——本册钉的就是那只函数的合同。
#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"
#include "hooks/middleware_builtins.hpp"
#include "runtime/middleware_runtime.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;

namespace {

MiddlewareDefinition BuiltinDef(HookPoint point, const std::string& name, Handler handler,
                                int priority = 100) {
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

// 带 builtin 估算/容量槽 + 可选附加定义的 dispatcher,挂进真 HookDispatcher
// (走 SetMiddleware 接线缝,与生产装配同一只口)。
hooks::HookDispatcher& MakeWiredDispatcher(std::vector<MiddlewareDefinition> extra = {}) {
    static std::vector<std::unique_ptr<hooks::HookDispatcher>> pool;
    auto dispatcher = std::make_unique<hooks::HookDispatcher>();
    MiddlewarePool middleware_pool;
    AddBuiltinRequestSlots(middleware_pool);
    for (auto& def : extra) {
        middleware_pool.AddDefinition(std::move(def));
    }
    auto published = middleware_pool.Publish();
    REQUIRE(published.has_value());
    dispatcher->SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    pool.push_back(std::move(dispatcher));
    return *pool.back();
}

MiddlewareHookContext HumanTurn(const std::string& turn_id = "turn-000001") {
    MiddlewareHookContext context;
    context.turn_id = turn_id;
    context.origin = "human";
    context.purpose = "interactive";
    context.delivery_mode = "direct";
    return context;
}

}  // namespace

// ---------------------------------------------------------------------------
// 零行为合同
// ---------------------------------------------------------------------------

TEST_CASE("零注册 = 零改写:没挂核的 dispatcher 返回恒等结果") {
    hooks::HookDispatcher dispatcher;  // 老 hook 装配的常态:没 SetMiddleware
    const PreUserGate gate = RunPreUserMiddleware(&dispatcher, "原话", HumanTurn());
    CHECK(!gate.dispatched);
    CHECK(!gate.blocked);
    CHECK(!gate.rewritten);
    CHECK(gate.prompt == "原话");
    CHECK(gate.additional_context.empty());

    const PostUserAppend append = RunPostUserMiddleware(&dispatcher, "原话", HumanTurn());
    CHECK(!append.dispatched);
    CHECK(!append.blocked);
    CHECK(append.context_appends.empty());

    const PreRequestStages stages =
        RunPreRequestMiddleware(&dispatcher, nlohmann::json{{"system", "x"}}, 4096, 512, HumanTurn());
    CHECK(!stages.dispatched);
    CHECK(stages.decision.empty());
    CHECK(!HasPreRequestMiddleware(&dispatcher));
    CHECK(!HasUserMiddleware(&dispatcher));

    // 空指针同样恒等(装配层容错)。
    CHECK(!RunPreUserMiddleware(nullptr, "原话", HumanTurn()).dispatched);
}

// ---------------------------------------------------------------------------
// PreUser
// ---------------------------------------------------------------------------

TEST_CASE("PreUser:改写采用(deny 拦截,append 随行)") {
    hooks::HookDispatcher& dispatcher = MakeWiredDispatcher({
        BuiltinDef(HookPoint::PreUser, "prompt.normalize",
                   [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                       nlohmann::json candidate = input;
                       candidate["prompt"] = "规范化后的话";
                       const auto downstream = next(candidate);
                       HandlerReturn out;
                       out.output = downstream.value;
                       out.effects.push_back(Effect{EffectType::ContextAppend,
                                                    nlohmann::json{{"text", "补充材料"}}});
                       return out;
                   }),
    });
    const PreUserGate gate = RunPreUserMiddleware(&dispatcher, "原话", HumanTurn());
    CHECK(gate.dispatched);
    CHECK(!gate.blocked);
    CHECK(gate.rewritten);
    CHECK(gate.prompt == "规范化后的话");
    REQUIRE(gate.additional_context.size() == 1);
    CHECK(gate.additional_context[0] == "补充材料");
    // UI 投影:计划序逐项账在(来源/结局/效果)。
    const nlohmann::json ui = DescribeDispatchForUi(gate.outcome);
    CHECK(ui["outcome"] == "completed");
    REQUIRE(ui["invocations"].size() == 1);
    CHECK(ui["invocations"][0]["hook"] == "PreUser/prompt.normalize");
    CHECK(ui["invocations"][0]["effects"][0]["applied"] == true);
}

TEST_CASE("PreUser:业务 deny 拦本轮,deny 与脚本错误分开") {
    hooks::HookDispatcher& dispatcher = MakeWiredDispatcher({
        BuiltinDef(HookPoint::PreUser, "prompt.gate",
                   [](const InvocationCtx&, const nlohmann::json&, NextCall&) {
                       return HandlerReturn::Denied("input_rejected", "这句话不合规矩");
                   }),
    });
    const PreUserGate gate = RunPreUserMiddleware(&dispatcher, "坏话", HumanTurn());
    CHECK(gate.dispatched);
    CHECK(gate.blocked);
    CHECK(gate.block_code == "input_rejected");
    CHECK(gate.block_reason == "这句话不合规矩");
    CHECK(!gate.rewritten);
}

TEST_CASE("PreUser:required 槽位失败不绕过(fail closed)") {
    MiddlewarePool pool;
    auto def = BuiltinDef(HookPoint::PreUser, "prompt.gate",
                          [](const InvocationCtx&, const nlohmann::json&,
                             NextCall&) -> std::expected<HandlerReturn, HandlerError> {
                              return std::unexpected(
                                  HandlerError{std::string(err::kHandlerFailed), "required 槽炸了"});
                          });
    def.required = true;
    pool.AddDefinition(std::move(def));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher required_dispatcher;
    required_dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    const PreUserGate gate = RunPreUserMiddleware(&required_dispatcher, "话", HumanTurn());
    CHECK(gate.blocked);
    CHECK(gate.block_code == std::string(err::kHandlerFailed));
}

TEST_CASE("PreUser:steer 输入按 deliveryMode 匹配,direct 不误伤") {
    // 获选定义带 delivery_mode=steer 匹配:dispatch 时按 trigger 匹配。
    MiddlewarePool pool;
    auto def = BuiltinDef(HookPoint::PreUser, "steer.annotate",
                          [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                              HandlerReturn out;
                              out.output = next(input).value;
                              out.effects.push_back(Effect{EffectType::ContextAppend,
                                                           nlohmann::json{{"text", "steer 上下文"}}});
                              return out;
                          });
    def.match.delivery_mode = "steer";
    pool.AddDefinition(std::move(def));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher steer_dispatcher;
    steer_dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));

    MiddlewareHookContext steer = HumanTurn();
    steer.delivery_mode = "steer";  // 同一 turn 中途插话(§4.47 steer 链)
    const PreUserGate steer_gate = RunPreUserMiddleware(&steer_dispatcher, "改主意了", steer);
    CHECK(steer_gate.dispatched);
    REQUIRE(steer_gate.additional_context.size() == 1);

    const PreUserGate direct_gate = RunPreUserMiddleware(&steer_dispatcher, "新问题", HumanTurn());
    CHECK(direct_gate.dispatched);
    CHECK(direct_gate.additional_context.empty());  // 未命中不暗跑(§4.48)
}

// ---------------------------------------------------------------------------
// PostUser
// ---------------------------------------------------------------------------

TEST_CASE("PostUser:追加隐藏上下文,失败阻断且原 user 保留") {
    hooks::HookDispatcher& dispatcher = MakeWiredDispatcher({
        BuiltinDef(HookPoint::PostUser, "memory.recall",
                   [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                       HandlerReturn out;
                       out.output = next(input).value;
                       out.effects.push_back(Effect{EffectType::ContextAppend,
                                                    nlohmann::json{{"text", "召回的记忆"}}});
                       return out;
                   }),
    });
    const PostUserAppend append = RunPostUserMiddleware(&dispatcher, "问点事", HumanTurn());
    CHECK(append.dispatched);
    CHECK(!append.blocked);
    REQUIRE(append.context_appends.size() == 1);
    CHECK(append.context_appends[0] == "召回的记忆");
}

// ---------------------------------------------------------------------------
// PreRequest 三段
// ---------------------------------------------------------------------------

TEST_CASE("PreRequest:mutate 空 → 估算 → 容量 allow,估算走注册槽无旁路") {
    hooks::HookDispatcher& dispatcher = MakeWiredDispatcher();
    nlohmann::json snapshot = nlohmann::json{{"system", "小请求"}, {"messages", nlohmann::json::array()}};
    const PreRequestStages stages = RunPreRequestMiddleware(&dispatcher, snapshot, 4096, 512, HumanTurn());
    CHECK(stages.dispatched);
    CHECK(!stages.reprepare_required);
    CHECK(stages.decision == "allow");
    CHECK(stages.token_estimate["estimator"] == "utf8_bytes_div4");
    CHECK(stages.token_estimate.contains("estimatedInputTokens"));
    // 估算值 = 同一快照经同一公式手算(没有第二条估算路径)。
    CHECK(stages.token_estimate["estimatedInputTokens"] ==
          ComputeUtf8BytesDiv4Estimate(snapshot)["estimatedInputTokens"]);
    // 容量段消费了估算(记录在账)。
    const auto* capacity_record = stages.capacity_outcome.FindRecord("PreRequest/context.capacity_check");
    REQUIRE(capacity_record != nullptr);
    CHECK(capacity_record->outcome == "completed");
}

TEST_CASE("PreRequest:输出预留进容量判断——超窗 recover,自身超窗 reject") {
    hooks::HookDispatcher& dispatcher = MakeWiredDispatcher();
    nlohmann::json snapshot = nlohmann::json{{"system", std::string(12000, 'x')}};
    // 预留 4000 + 估算(约 3000)+ 512 > 4096:输入自身加余量装得下 → recover。
    const PreRequestStages recover = RunPreRequestMiddleware(&dispatcher, snapshot, 4096, 4000, HumanTurn());
    CHECK(recover.decision == "recover");
    // 窗口压到 2048:输入自身 + 512 也超 → reject(压缩无济于事)。
    const PreRequestStages reject = RunPreRequestMiddleware(&dispatcher, snapshot, 2048, 4000, HumanTurn());
    CHECK(reject.decision == "reject");
    CHECK(!reject.Allowed());
}

TEST_CASE("PreRequest:mutate 段采用改写 → reprepare,不沿用旧输入的估算") {
    // mutate 槽位须声明 mutate 阶段(阶段属于槽位合同)。
    MiddlewarePool pool;
    AddBuiltinRequestSlots(pool);
    auto def = BuiltinDef(HookPoint::PreRequest, "request.slim",
                          [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                              nlohmann::json slim = input;
                              slim["system"] = "瘦身后";
                              return HandlerReturn::Value(next(slim).value);
                          });
    def.stage = Stage::Mutate;
    pool.AddDefinition(std::move(def));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher mutate_dispatcher;
    mutate_dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));

    const PreRequestStages stages =
        RunPreRequestMiddleware(&mutate_dispatcher, nlohmann::json{{"system", "胖请求"}}, 4096, 512, HumanTurn());
    CHECK(stages.dispatched);
    CHECK(stages.reprepare_required);  // §4.36:换输入版本再经 hook 估算
    CHECK(stages.decision == "reprepare");
    CHECK(stages.adopted_input["system"] == "瘦身后");
    CHECK(!stages.token_estimate.contains("estimatedInputTokens"));  // 不沿用旧 hash 的估算
}

TEST_CASE("PreRequest:估算段被同名替换实现接管,结果照常进容量段") {
    MiddlewarePool pool;
    AddBuiltinRequestSlots(pool);
    auto replacement = BuiltinDef(HookPoint::PreRequest, "context.token_estimate",
                                  [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
                                      next();
                                      return HandlerReturn::Value(
                                          nlohmann::json{{"estimator", "user_scale"},
                                                         {"estimatedInputTokens", 8000}});
                                  });
    replacement.layer = SourceLayer::User;  // 同键用户层胜出
    replacement.stage = Stage::Estimate;
    pool.AddDefinition(std::move(replacement));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher dispatcher;
    dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));

    const PreRequestStages stages = RunPreRequestMiddleware(
        &dispatcher, nlohmann::json{{"system", "x"}}, 4096, 512, HumanTurn());
    CHECK(stages.token_estimate["estimator"] == "user_scale");
    // 容量按用户估算器数字判断:8000 + 512 + 512 > 4096 且 8000+512 > 4096 → reject。
    CHECK(stages.decision == "reject");
}

TEST_CASE("Esc 取消:取消旗置位时 dispatch 收口 skipped_cancelled,不执行") {
    MiddlewarePool pool;
    int runs = 0;
    pool.AddDefinition(BuiltinDef(HookPoint::PreUser, "prompt.slow",
                                  [&runs](const InvocationCtx&, const nlohmann::json& input,
                                          NextCall& next) {
                                      ++runs;
                                      return HandlerReturn::Value(next(input).value);
                                  }));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher dispatcher;
    dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));

    std::atomic<bool> cancel{true};  // Esc 已按下
    MiddlewareHookContext context = HumanTurn();
    context.cancel = &cancel;
    const PreUserGate gate = RunPreUserMiddleware(&dispatcher, "话", context);
    CHECK(gate.dispatched);
    CHECK(gate.blocked);  // 取消按失败收口(err::kDispatchCancelled)
    CHECK(gate.block_code == std::string(err::kDispatchCancelled));
    CHECK(runs == 0);     // 帧边界拦住:handler 零执行
}

// ---------------------------------------------------------------------------
// 快照投影
// ---------------------------------------------------------------------------

TEST_CASE("BuildRequestSnapshotJson:中立投影,媒体保类型占位") {
    api::Request request;
    request.model = "m";
    request.system = "sys";
    request.max_tokens = 128;
    request.tools.push_back(api::ToolDefinition{"tool_a", "desc", nlohmann::json{{"type", "object"}}});
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"你好"});
    message.content.push_back(api::ImageBlock{"image/png", "AAAA", "a.png", 8, 8});
    request.messages.push_back(message);

    const nlohmann::json snapshot = BuildRequestSnapshotJson(request);
    CHECK(snapshot["model"] == "m");
    CHECK(snapshot["system"] == "sys");
    CHECK(snapshot["max_tokens"] == 128);
    REQUIRE(snapshot["messages"].size() == 1);
    const auto& blocks = snapshot["messages"][0]["content"];
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0]["type"] == "text");
    CHECK(blocks[1]["type"] == "image");
    CHECK(!blocks[1].contains("data"));  // base64 不进快照(估算时按媒体剥离)
    REQUIRE(snapshot["tools"].size() == 1);
    CHECK(snapshot["tools"][0]["name"] == "tool_a");

    // 估算对含图快照标 partial,文本照算。
    const auto estimate = ComputeUtf8BytesDiv4Estimate(snapshot);
    CHECK(estimate["coverage"] == "partial");
}
