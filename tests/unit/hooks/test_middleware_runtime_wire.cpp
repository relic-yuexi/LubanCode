// LuaHook 单 P0-B 遗留②(P1-C 补验):生产接线与旁路行为五笔。
//   ① BindMiddlewareSessionWriter 通电:dispatcher 槽位 sink 被三个 Run*
//      Middleware 自动取用(调用方不递 sink 参数);幂等与换场换绑;
//   ② steer/followup:delivery_mode 进匹配条件(§7.2:普通输入 steer、
//      子报告按目标 followup,不混跑);
//   ③ compact 旁路请求切槽:估算经 PreRequest/estimate 槽位(用户替换
//      实现对 compact 同样生效;purpose=compact 进事件账;核缺场回落
//      内置公式;槽失败 fail closed),RunV3Compact 的门禁吃槽产出;
//   ④ 首行 system:v3 文件行 1 是完整 system,hook 事件同文件落账
//      (一个 writer);
//   ⑤ Esc interrupted:取消旗置位 -> dispatch 整体 cancelled,context
//      候选不被采用;输出预留:容量段消费估算+预留,三档决定齐全。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"
#include "hooks/middleware_builtins.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "runtime/v3_compact_runtime.hpp"
#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;
using namespace lubancode::trajectory::v3;

namespace {

class FixedClock final : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct V3Dir {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit V3Dir(const char* tag) {
        dir = std::filesystem::temp_directory_path() / ("lubancode-mw-wire-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260911-150000-WR01", "run-000001",
                                       "你是 LubanCode。", nlohmann::json::object(),
                                       V3WriterOptions{}, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }
};

std::vector<nlohmann::json> ReadEvents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        events.push_back(nlohmann::json::parse(line));
    }
    return events;
}

MiddlewareDefinition PassDef(HookPoint point, const std::string& name,
                             std::vector<std::string> capabilities = {}) {
    MiddlewareDefinition def;
    def.point = point;
    def.name = name;
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin." + name;
    def.capabilities = std::move(capabilities);
    def.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        return HandlerReturn::Value(next().value);
    };
    return def;
}

// 带 sink 通电的 dispatcher:内置估算/容量槽位 + 可选附加定义。
hooks::HookDispatcher MakeWiredDispatcher(V3Dir& v3, MiddlewarePool& pool) {
    AddBuiltinRequestSlots(pool);
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher dispatcher;
    dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    BindMiddlewareSessionWriter(&dispatcher, nullptr, &*v3.writer);
    return dispatcher;
}

}  // namespace

// ---------------------------------------------------------------------------
// ① sink 通电:dispatcher 槽位被 Run* 自动取用;幂等/换绑/解绑
// ---------------------------------------------------------------------------

TEST_CASE("通电:Run*Middleware 不递 sink 时自动用 dispatcher 槽位,事件落 v3 主账") {
    V3Dir v3("power");
    MiddlewarePool pool;
    pool.AddDefinition(PassDef(HookPoint::PreUser, "prompt.observe"));
    hooks::HookDispatcher dispatcher = MakeWiredDispatcher(v3, pool);

    MiddlewareHookContext context;
    context.origin = "human";
    const PreUserGate gate = RunPreUserMiddleware(&dispatcher, "第一句", context);
    REQUIRE(gate.dispatched);
    REQUIRE(gate.outcome.Ok());

    // sink 槽位生效:hook.dispatch.requested 落在同一份 v3 文件。
    const auto events = ReadEvents(v3.jsonl);
    bool saw_requested = false;
    for (const auto& event : events) {
        if (event.value("kind", "") == "hook.dispatch.requested") {
            saw_requested = true;
            CHECK(event.at("payload").at("hookPoint") == "PreUser");
            REQUIRE(event.at("payload").at("matchedHandlers").size() == 1);
            CHECK(event.at("payload").at("matchedHandlers").at(0).at("hookId") ==
                  "PreUser/prompt.observe");
        }
    }
    CHECK(saw_requested);

    // 幂等:同写者重绑不换 sink(现有事件不丢、不重建)。
    HookHostServiceCenter center;
    BindMiddlewareSessionWriter(&dispatcher, &center, &*v3.writer);
    const PreUserGate again = RunPreUserMiddleware(&dispatcher, "第二句", context);
    REQUIRE(again.dispatched);
    int requested_count = 0;
    for (const auto& event : ReadEvents(v3.jsonl)) {
        if (event.value("kind", "") == "hook.dispatch.requested") {
            ++requested_count;
        }
    }
    CHECK(requested_count == 2);

    // 换场:新写者换绑后事件落新文件;null 解绑后不再落账。
    V3Dir v3_next("power-next");
    BindMiddlewareSessionWriter(&dispatcher, &center, &*v3_next.writer);
    const PreUserGate next_turn = RunPreUserMiddleware(&dispatcher, "换场后", context);
    REQUIRE(next_turn.dispatched);
    CHECK_FALSE(ReadEvents(v3_next.jsonl).empty());
    BindMiddlewareSessionWriter(&dispatcher, &center, nullptr);
    const PreUserGate unbound = RunPreUserMiddleware(&dispatcher, "解绑后", context);
    REQUIRE(unbound.dispatched);
    const std::size_t before = ReadEvents(v3_next.jsonl).size();
    const PreUserGate still_unbound = RunPreUserMiddleware(&dispatcher, "还是解绑", context);
    (void)still_unbound;
    CHECK(ReadEvents(v3_next.jsonl).size() == before);
}

// ---------------------------------------------------------------------------
// ② steer/followup:delivery_mode 进匹配
// ---------------------------------------------------------------------------

TEST_CASE("steer/followup:普通输入与子报告各跑各的钩子,不混跑") {
    V3Dir v3("delivery");
    MiddlewarePool pool;
    {
        MiddlewareDefinition steer_hook = PassDef(HookPoint::PreUser, "prompt.steer_only");
        steer_hook.match.delivery_mode = "steer";
        MiddlewareDefinition followup_hook = PassDef(HookPoint::PreUser, "prompt.followup_only");
        followup_hook.match.delivery_mode = "followup";
        pool.AddDefinition(std::move(steer_hook));
        pool.AddDefinition(std::move(followup_hook));
    }
    hooks::HookDispatcher dispatcher = MakeWiredDispatcher(v3, pool);

    MiddlewareHookContext context;
    context.origin = "human";
    context.delivery_mode = "steer";
    const PreUserGate steer_gate = RunPreUserMiddleware(&dispatcher, "插一句话", context);
    REQUIRE(steer_gate.dispatched);
    REQUIRE(steer_gate.outcome.records.size() == 2);  // 两项都在计划里,一项跑一项跳
    CHECK(steer_gate.outcome.FindRecord("PreUser/prompt.steer_only")->outcome == "completed");
    CHECK(steer_gate.outcome.FindRecord("PreUser/prompt.followup_only")->outcome ==
          "skipped_no_match");

    context.delivery_mode = "followup";
    const PreUserGate followup_gate = RunPreUserMiddleware(&dispatcher, "子报告", context);
    REQUIRE(followup_gate.dispatched);
    CHECK(followup_gate.outcome.FindRecord("PreUser/prompt.followup_only")->outcome == "completed");
    CHECK(followup_gate.outcome.FindRecord("PreUser/prompt.steer_only")->outcome == "skipped_no_match");
}

// ---------------------------------------------------------------------------
// ③ compact 旁路请求切槽(估算 = 内置 hook 覆盖旁路请求)
// ---------------------------------------------------------------------------

TEST_CASE("切槽:EstimateBypassRequestTokens 走 estimate 槽,用户替换实现同样生效") {
    V3Dir v3("slot");
    MiddlewarePool pool;
    hooks::HookDispatcher dispatcher = MakeWiredDispatcher(v3, pool);
    const nlohmann::json snapshot = nlohmann::json{{"system", "s"}, {"messages", nlohmann::json::array()}};

    // 内置槽:bytes/4 公式,EST1 形状。
    {
        MiddlewareHookContext context;
        context.purpose = "compact";
        const auto estimate = EstimateBypassRequestTokens(&dispatcher, snapshot, context);
        REQUIRE(estimate.has_value());
        CHECK((*estimate).at("estimator") == "utf8_bytes_div4");
        CHECK((*estimate).contains("estimatedInputTokens"));
    }

    // 用户同名替换(User 层压过 builtin):翻倍估算器生效——旁路不自带
    // 第二份公式(§4.36)。
    MiddlewarePool replaced_pool;
    AddBuiltinRequestSlots(replaced_pool);
    {
        MiddlewareDefinition doubler;
        doubler.point = HookPoint::PreRequest;
        doubler.stage = Stage::Estimate;
        doubler.name = std::string(kTokenEstimateSlot);
        doubler.layer = SourceLayer::User;
        doubler.source_label = "user ~/.lubancode/hooks/double";
        doubler.implementation_ref = "hooks/double#run";
        doubler.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            nlohmann::json estimate = ComputeUtf8BytesDiv4Estimate(input);
            estimate["estimatedInputTokens"] =
                estimate.at("estimatedInputTokens").get<std::uint64_t>() * 2;
            estimate["estimator"] = "double_bytes_div4";
            if (next.calls() == 0) {
                next();
            }
            return HandlerReturn::Value(estimate);
        };
        replaced_pool.AddDefinition(std::move(doubler));
    }
    auto replaced_published = replaced_pool.Publish();
    REQUIRE(replaced_published.has_value());
    hooks::HookDispatcher replaced_dispatcher;
    replaced_dispatcher.SetMiddleware(
        std::make_shared<MiddlewareDispatcher>(std::move(*replaced_published)));
    BindMiddlewareSessionWriter(&replaced_dispatcher, nullptr, &*v3.writer);
    {
        MiddlewareHookContext context;
        context.purpose = "compact";
        const auto builtin_estimate = EstimateBypassRequestTokens(&dispatcher, snapshot, context);
        const auto doubled_estimate =
            EstimateBypassRequestTokens(&replaced_dispatcher, snapshot, context);
        REQUIRE(builtin_estimate.has_value());
        REQUIRE(doubled_estimate.has_value());
        CHECK((*doubled_estimate).at("estimator") == "double_bytes_div4");
        CHECK((*doubled_estimate).at("estimatedInputTokens") ==
              (*builtin_estimate).at("estimatedInputTokens").get<std::uint64_t>() * 2);
    }

    // purpose=compact 进事件账:hook.dispatch.requested 带估算 invocation。
    // (purpose 进匹配条件;这里记录型断言——槽跑过就有账。)
    bool saw_estimate_dispatch = false;
    for (const auto& event : ReadEvents(v3.jsonl)) {
        if (event.value("kind", "") == "hook.dispatch.requested" &&
            event.at("payload").at("hookPoint") == "PreRequest") {
            saw_estimate_dispatch = true;
        }
    }
    CHECK(saw_estimate_dispatch);

    // 核缺场:回落内置公式(与槽内置同式,不换口径)。
    {
        hooks::HookDispatcher empty;
        MiddlewareHookContext context;
        context.purpose = "compact";
        const auto fallback = EstimateBypassRequestTokens(&empty, snapshot, context);
        REQUIRE(fallback.has_value());
        CHECK((*fallback).at("estimator") == "utf8_bytes_div4");
    }

    // 槽失败 fail closed:替换实现抛错 -> 错误,不假装核过。
    MiddlewarePool broken_pool;
    AddBuiltinRequestSlots(broken_pool);
    {
        MiddlewareDefinition breaker;
        breaker.point = HookPoint::PreRequest;
        breaker.stage = Stage::Estimate;
        breaker.name = std::string(kTokenEstimateSlot);
        breaker.layer = SourceLayer::User;
        breaker.source_label = "user ~/.lubancode/hooks/broken";
        breaker.implementation_ref = "hooks/broken#run";
        breaker.builtin = [](const InvocationCtx&, const nlohmann::json&, NextCall&) {
            return std::unexpected(HandlerError{"hook.handler.failed", "估算器坏了"});
        };
        broken_pool.AddDefinition(std::move(breaker));
    }
    auto broken_published = broken_pool.Publish();
    REQUIRE(broken_published.has_value());
    hooks::HookDispatcher broken_dispatcher;
    broken_dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*broken_published)));
    {
        MiddlewareHookContext context;
        context.purpose = "compact";
        const auto failed = EstimateBypassRequestTokens(&broken_dispatcher, snapshot, context);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().rfind("compact.estimate_failed", 0) == 0);
    }
}

namespace {

// 压缩模型桩:回一份合格摘要。
class ManifestClient final : public V3CompactModelClient {
public:
    int calls = 0;
    V3CompactModelReply Send(const std::string&, const std::vector<nlohmann::json>&) override {
        ++calls;
        V3CompactModelReply reply;
        reply.ok = true;
        reply.text = "压缩后的交接摘要:本轮验证了旁路请求切槽的门禁行为,槽产出放行时模型被真实调用,"
                     "摘要正文须超过最短码点门槛,这里把任务目标、约束与进展都交代清楚。";
        nlohmann::json manifest = nlohmann::json::object(
            {{"goal", "把会话接下去"}, {"constraints", nlohmann::json::array({"不许动旧档"})},
             {"open_items", nlohmann::json::array({"继续验证"})}, {"next_action", "继续干活"}});
        reply.text += "\n```json\n" + manifest.dump() + "\n```\n";
        return reply;
    }
};

void SeedTurn(V3Writer& writer, const std::string& turn_id, const std::string& user_text) {
    MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = MessagePurpose::Conversation;
    user.origin = MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", user_text}});
    const WriteReceipt user_receipt = writer.AppendMessage(std::move(user), Durability::PowerLoss);
    if (user_receipt.status != WriteReceipt::Status::Committed) {
        const std::string why = user_receipt.error_code + ": " + user_receipt.error_message;
        CHECK_MESSAGE(false, why);
    }
    REQUIRE(user_receipt.status == WriteReceipt::Status::Committed);

    MessageDraft assistant;
    assistant.turn_id = turn_id;
    assistant.request_id = "request-" + turn_id;
    assistant.provider = "moonshot";
    assistant.wire = "openai-chat-completions";
    assistant.model = "kimi-k2.6";
    assistant.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
    assistant.response_model = nlohmann::json("kimi-k2.6");  // schema:assistant 必带(缺失实报 null)
    assistant.origin = MessageOrigin::SessionRuntime;
    assistant.message = nlohmann::json::object({{"role", "assistant"}, {"content", "收到。"}});
    const WriteReceipt receipt = writer.AppendMessage(std::move(assistant), Durability::PowerLoss);
    REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    // user 与 assistant 都进链(只进 assistant 会让压缩材料缩成两条短回执,
    // 收益检查必挂)。
    REQUIRE(writer.AdmitMessages({user_receipt.id, receipt.id}).status ==
            WriteReceipt::Status::Committed);
}

V3CompactProfile TinyProfile() {
    V3CompactProfile profile;
    profile.compact_window_tokens = 40000;
    profile.compact_output_reserve_tokens = 512;
    profile.compact_margin_tokens = 512;
    profile.provider = "moonshot";
    profile.wire = "openai-chat-completions";
    profile.model = "kimi-k2.6";
    return profile;
}

// 两轮长材料:保证"压缩后严格变小"的收益检查可过(摘要远短于原文)。
// 填充用 ASCII(多字节字面量进 char 会被截成非法 UTF-8,canonical 不过)。
std::string LongTurnText(const char* head) {
    return std::string(head) + std::string(6000, 'x') + "。";
}

}  // namespace

TEST_CASE("切槽:RunV3Compact 门禁吃槽产出(恒小估算放行/巨量估算拒收不空调模型)") {
    // 场景 A:槽估 10 tokens(恒小)——门禁放行,真发模型,applied。
    {
        V3Dir v3("compact-small");
        SeedTurn(*v3.writer, "turn-1", LongTurnText("第一轮的内容").c_str());
        SeedTurn(*v3.writer, "turn-2", LongTurnText("第二轮的内容").c_str());
        ManifestClient client;
        V3CompactRunInput input;
        input.trigger = "manual";
        input.estimate = [](const nlohmann::json&) {
            return std::expected<nlohmann::json, std::string>(
                nlohmann::json{{"estimator", "stub"}, {"estimatedInputTokens", 10}});
        };
        const V3CompactRunResult result = RunV3Compact(*v3.writer, client, TinyProfile(), std::move(input));
        if (!result.applied) {
            const std::string why = result.terminal_kind + " / " + result.reason;
            CHECK_MESSAGE(false, why);
            for (const std::string& note : result.notes) {
                CHECK_MESSAGE(false, note);
            }
        }
        CHECK(result.applied);
        CHECK(client.calls == 1);
    }
    // 场景 B:槽估 1e9 tokens——门禁拒收(input_capacity_exceeded),模型零调用。
    {
        V3Dir v3("compact-big");
        SeedTurn(*v3.writer, "turn-1", LongTurnText("第一轮的内容").c_str());
        SeedTurn(*v3.writer, "turn-2", LongTurnText("第二轮的内容").c_str());
        ManifestClient client;
        V3CompactRunInput input;
        input.trigger = "manual";
        input.estimate = [](const nlohmann::json&) {
            return std::expected<nlohmann::json, std::string>(
                nlohmann::json{{"estimator", "stub"}, {"estimatedInputTokens", 1000000000}});
        };
        const V3CompactRunResult result = RunV3Compact(*v3.writer, client, TinyProfile(), std::move(input));
        CHECK(result.terminal_kind == "rejected");
        CHECK(result.reason.rfind("input_capacity_exceeded", 0) == 0);
        CHECK(client.calls == 0);  // 不空调模型
    }
    // 场景 C:槽产出坏形状 -> fail closed 收口,不回落内置公式。
    {
        V3Dir v3("compact-bad");
        SeedTurn(*v3.writer, "turn-1", LongTurnText("第一轮的内容").c_str());
        SeedTurn(*v3.writer, "turn-2", LongTurnText("第二轮的内容").c_str());
        ManifestClient client;
        V3CompactRunInput input;
        input.trigger = "manual";
        input.estimate = [](const nlohmann::json&) {
            return std::expected<nlohmann::json, std::string>(nlohmann::json{{"nope", 1}});
        };
        const V3CompactRunResult result = RunV3Compact(*v3.writer, client, TinyProfile(), std::move(input));
        CHECK(result.terminal_kind == "rejected");
        CHECK(result.reason.rfind("compact.estimate_bad_shape", 0) == 0);
        CHECK(client.calls == 0);
    }
}

// ---------------------------------------------------------------------------
// ④ 首行 system:一个 writer,hook 事件同文件
// ---------------------------------------------------------------------------

TEST_CASE("首行 system:v3 文件行 1 是完整 system;hook 事件后续落同一份账") {
    V3Dir v3("firstline");
    MiddlewarePool pool;
    pool.AddDefinition(PassDef(HookPoint::PostUser, "ctx.observe"));
    hooks::HookDispatcher dispatcher = MakeWiredDispatcher(v3, pool);

    MiddlewareHookContext context;
    const PostUserAppend append = RunPostUserMiddleware(&dispatcher, "问一句", context);
    REQUIRE(append.dispatched);

    const auto events = ReadEvents(v3.jsonl);
    REQUIRE_FALSE(events.empty());
    const nlohmann::json& first = events.at(0);
    CHECK(first.at("type") == "message");
    CHECK(first.at("message").at("role") == "system");
    CHECK(first.at("message").at("content") == "你是 LubanCode。");
    // hook 事件在同一份文件里(session 内共用 seq,§7.1 一个 writer)。
    bool saw_hook = false;
    for (const auto& event : events) {
        if (event.value("kind", "") == "hook.dispatch.requested") {
            saw_hook = true;
            CHECK(event.at("seq").get<std::uint64_t>() > first.at("seq").get<std::uint64_t>());
        }
    }
    CHECK(saw_hook);
}

// ---------------------------------------------------------------------------
// ⑤ Esc interrupted 与输出预留
// ---------------------------------------------------------------------------

TEST_CASE("Esc:取消旗置位 -> dispatch 整体取消,项记 skipped_cancelled,效果不采用") {
    V3Dir v3("esc");
    MiddlewarePool pool;
    {
        MiddlewareDefinition appender = PassDef(HookPoint::PostUser, "ctx.would_append");
        appender.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall&) {
            HandlerReturn out;
            out.output = input;
            out.effects.push_back(
                Effect{EffectType::ContextAppend, nlohmann::json{{"type", "context.append"},
                                                                 {"text", "不该被采用的候选"}}});
            return out;
        };
        pool.AddDefinition(std::move(appender));
    }
    hooks::HookDispatcher dispatcher = MakeWiredDispatcher(v3, pool);

    std::atomic<bool> cancelled{true};
    MiddlewareHookContext context;
    context.cancel = &cancelled;
    const PostUserAppend append = RunPostUserMiddleware(&dispatcher, "按 Esc 的一轮", context);
    REQUIRE(append.dispatched);
    CHECK(append.outcome.kind == DispatchOutcome::Kind::Failed);
    CHECK(append.outcome.error_code == "hook.dispatch.cancelled");
    CHECK(append.outcome.FindRecord("PostUser/ctx.would_append")->outcome == "skipped_cancelled");
    CHECK(append.context_appends.empty());  // 取消后不再提交业务改写(§四)
}

TEST_CASE("输出预留:容量段消费估算与输出预留,allow/recover/reject 三档齐全") {
    V3Dir v3("reserve");
    MiddlewarePool pool;
    hooks::HookDispatcher dispatcher = MakeWiredDispatcher(v3, pool);
    const nlohmann::json snapshot = nlohmann::json{{"system", "你是 LubanCode。"},
                                                   {"messages", nlohmann::json::array(
                                                                    {{{"role", "user"},
                                                                      {"content", "短问"}}})}};
    MiddlewareHookContext context;
    context.purpose = "interactive";

    // 宽窗 + 预留:allow。
    {
        const PreRequestStages stages =
            RunPreRequestMiddleware(&dispatcher, snapshot, /*context_window=*/100000,
                                    /*output_reserve=*/4096, context);
        REQUIRE(stages.dispatched);
        CHECK(stages.decision == "allow");
        CHECK(stages.token_estimate.at("estimatedInputTokens").is_number_unsigned());
    }
    // 预留挤爆窗口但输入自身装得下:recover(压缩/降档可救)。
    {
        const std::uint64_t estimated = [&] {
            MiddlewareHookContext c;
            auto estimate = EstimateBypassRequestTokens(&dispatcher, snapshot, c);
            REQUIRE(estimate.has_value());
            return (*estimate).at("estimatedInputTokens").get<std::uint64_t>();
        }();
        const PreRequestStages stages = RunPreRequestMiddleware(
            &dispatcher, snapshot, /*context_window=*/estimated + 600,
            /*output_reserve=*/4096, context);
        REQUIRE(stages.dispatched);
        CHECK(stages.decision == "recover");  // 预留(而非输入)把窗口顶破
    }
    // 输入自身 + 协议余量就超窗:reject。
    {
        const PreRequestStages stages =
            RunPreRequestMiddleware(&dispatcher, snapshot, /*context_window=*/8,
                                    /*output_reserve=*/0, context);
        REQUIRE(stages.dispatched);
        CHECK(stages.decision == "reject");
    }
}
