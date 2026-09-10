// 内置生产槽位(LuaHook 单 P0-B,§4.36):估算公式 ceil(utf8Bytes/4)、
// 媒体剥离与 coverage、容量三档决定、required 槽位合同(同名替换不能解除)、
// 注册发布后经 dispatch 真跑——估算没有绕开 hook 的旁路。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "hooks/middleware.hpp"
#include "hooks/middleware_builtins.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;

namespace {

MiddlewareDispatcher MakeDispatcherWithBuiltins(std::vector<MiddlewareDefinition> extra = {}) {
    MiddlewarePool pool;
    AddBuiltinRequestSlots(pool);
    for (auto& def : extra) {
        pool.AddDefinition(std::move(def));
    }
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    return MiddlewareDispatcher(std::move(*published));
}

}  // namespace

TEST_CASE("估算公式:UTF-8 字节 ÷ 4,总量一次向上取整") {
    // 纯 ASCII:紧凑序列化恰好 128000 字节(结构 13 字节 + 正文 127987)
    // = 32000 token(§4.36 例)。
    nlohmann::json ascii_snapshot = nlohmann::json{{"system", std::string(127987, 'a')}};
    REQUIRE(ascii_snapshot.dump().size() == 128000);
    const auto ascii = ComputeUtf8BytesDiv4Estimate(ascii_snapshot);
    CHECK(ascii["estimator"] == "utf8_bytes_div4");
    CHECK(ascii["estimatorVersion"] == 1);
    CHECK(ascii["rounding"] == "ceil");
    CHECK(ascii["coverage"] == "text_proxy");
    CHECK(ascii["inputUtf8Bytes"] == 128000);
    CHECK(ascii["estimatedInputTokens"] == 32000);

    // 任意快照:token 恒等于 ceil(bytes/4),字节 = 同一 json 的紧凑 dump
    // 长度(可复现序列化,键序确定)。
    const std::string han = "一二三四五六七八九十";
    const nlohmann::json han_snapshot = nlohmann::json{{"text", han}, {"n", 7}};
    const auto han_estimate = ComputeUtf8BytesDiv4Estimate(han_snapshot);
    const std::uint64_t han_bytes = han_estimate["inputUtf8Bytes"];
    CHECK(han_bytes == han_snapshot.dump().size());
    CHECK(han_estimate["estimatedInputTokens"].get<std::uint64_t>() ==
          han_bytes / 4 + (han_bytes % 4 != 0 ? 1 : 0));

    // emoji(4 字节码点)照 UTF-8 字节计,不算字符数。
    const auto emoji = ComputeUtf8BytesDiv4Estimate(nlohmann::json{{"text", "🎉"}});
    CHECK(emoji["inputUtf8Bytes"].get<std::uint64_t>() >= 4);

    // 空 object 也走同一公式(0 字节 = 0 token)。
    const auto empty = ComputeUtf8BytesDiv4Estimate(nlohmann::json::object());
    CHECK(empty["inputUtf8Bytes"] == 2);  // "{}"
    CHECK(empty["estimatedInputTokens"] == 1);  // ceil(2/4)
}

TEST_CASE("媒体剥离:image 块不进 bytes/4,coverage 落 partial") {
    // 逐块拼(深嵌套 initializer 在 MSVC 下解析不稳)。
    nlohmann::json text_block = nlohmann::json{{"type", "text"}, {"text", "看图"}};
    nlohmann::json image_block;
    image_block["type"] = "image";
    image_block["media_type"] = "image/png";
    image_block["filename"] = "shot.png";
    nlohmann::json audio_block;
    audio_block["type"] = "audio";
    audio_block["data"] = "AAAA";
    nlohmann::json content = nlohmann::json::array({text_block, image_block, audio_block});
    nlohmann::json message;
    message["role"] = "user";
    message["content"] = std::move(content);
    nlohmann::json with_image;
    with_image["messages"] = nlohmann::json::array({std::move(message)});

    const auto estimate = ComputeUtf8BytesDiv4Estimate(with_image);
    CHECK(estimate["coverage"] == "partial");
    const auto modalities = estimate["unestimatedModalities"];
    REQUIRE(modalities.is_array());
    CHECK(modalities.size() == 2);  // image + audio,各记一次
    // 无媒体则 text_proxy。
    CHECK(ComputeUtf8BytesDiv4Estimate(nlohmann::json{{"text", "hello"}})["coverage"] == "text_proxy");
}

TEST_CASE("容量三档:allow/recover/reject,输出预留与协议余量分账") {
    // allow:估算 + 预留 + 512 余量在窗口内。
    const auto allow = DecideRequestCapacity(nlohmann::json{{"estimatedInputTokens", 1000},
                                                             {"outputReserveTokens", 200},
                                                             {"contextWindowTokens", 4096}});
    CHECK(allow["decision"] == "allow");

    // recover:总账超窗但输入自身加余量装得下——压缩有望救。
    const auto recover = DecideRequestCapacity(nlohmann::json{{"estimatedInputTokens", 3000},
                                                               {"outputReserveTokens", 2000},
                                                               {"contextWindowTokens", 4096}});
    CHECK(recover["decision"] == "recover");

    // reject:输入自身 + 余量已超窗,压缩历史无济于事。
    const auto reject = DecideRequestCapacity(nlohmann::json{{"estimatedInputTokens", 5000},
                                                              {"outputReserveTokens", 2000},
                                                              {"contextWindowTokens", 4096}});
    CHECK(reject["decision"] == "reject");

    // 窗口未知(0)不拦:容量判断不制造新硬闸。
    CHECK(DecideRequestCapacity(nlohmann::json{{"estimatedInputTokens", 999999},
                                                {"outputReserveTokens", 1},
                                                {"contextWindowTokens", 0}})["decision"] == "allow");
}

TEST_CASE("槽位定义:PreRequest 两枚 required 槽,阶段各归各") {
    const auto estimator = BuiltinTokenEstimateSlot();
    CHECK(estimator.point == HookPoint::PreRequest);
    CHECK(estimator.stage == Stage::Estimate);
    CHECK(estimator.name == "context.token_estimate");
    CHECK(estimator.required);
    CHECK(estimator.layer == SourceLayer::Builtin);
    CHECK(estimator.implementation_ref == "builtin.token_estimate_v1");

    const auto capacity = BuiltinCapacityCheckSlot();
    CHECK(capacity.stage == Stage::Capacity);
    CHECK(capacity.name == "context.capacity_check");
    CHECK(capacity.required);
    REQUIRE(capacity.after.size() == 1);
    CHECK(capacity.after[0] == "context.token_estimate");  // 依赖:容量消费估算
}

TEST_CASE("发布后经 dispatch 真跑:估算段产出结构化结果,无旁路") {
    MiddlewareDispatcher dispatcher = MakeDispatcherWithBuiltins();
    // 估算段:冻结快照进,EST1 出——runtime 侧没有第二条估算路径可走。
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"system", "你是 LubanCode。"}, {"messages", nlohmann::json::array()}};
    trigger.stage_filter = Stage::Estimate;
    DispatchOutcome outcome = dispatcher.Dispatch(HookPoint::PreRequest, trigger);
    REQUIRE(outcome.Ok());
    CHECK(outcome.value.is_object());
    CHECK(outcome.value["estimator"] == "utf8_bytes_div4");
    CHECK(outcome.value.contains("estimatedInputTokens"));
    const auto* record = outcome.FindRecord("PreRequest/context.token_estimate");
    REQUIRE(record != nullptr);
    CHECK(record->outcome == "completed");
    CHECK(record->handler_kind == "builtin");
    // 容量段在段外:stage_filter 挡住,记 skipped 不是执行。
    const auto* capacity_record = outcome.FindRecord("PreRequest/context.capacity_check");
    REQUIRE(capacity_record != nullptr);
    CHECK(capacity_record->outcome == "skipped_no_match");
}

TEST_CASE("同名替换:用户 Lua/内置接管估算槽,required 不解除,只跑获选项") {
    // 用户层同键定义替换内置估算器(P0-A 的同键选实现;这里验槽位合同)。
    MiddlewareDefinition replacement;
    replacement.point = HookPoint::PreRequest;
    replacement.stage = Stage::Estimate;
    replacement.name = "context.token_estimate";
    replacement.layer = SourceLayer::User;
    replacement.source_label = "user ~/.lubancode/hooks";
    replacement.implementation_ref = "hooks/my_estimator#estimate";
    replacement.builtin = [](const InvocationCtx&, const nlohmann::json& input,
                             NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
        next();
        return HandlerReturn::Value(nlohmann::json{{"estimator", "my_custom"},
                                                   {"estimatedInputTokens", 42}});
    };

    MiddlewarePool pool;
    AddBuiltinRequestSlots(pool);
    pool.AddDefinition(replacement);
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));

    // required 以 builtin 槽位声明为准:获选的用户实现保持 required。
    const auto& selected = dispatcher.registry().Selected(HookPoint::PreRequest);
    bool found = false;
    for (const auto& def : selected) {
        if (def->name == "context.token_estimate") {
            found = true;
            CHECK(def->required);  // 槽位要求不许解除(§4.48)
            CHECK(def->implementation_ref == "hooks/my_estimator#estimate");
        }
    }
    CHECK(found);
    // 被覆盖的 builtin 保留来源,不进执行计划(§3.1)。
    CHECK(!dispatcher.registry().Overridden().empty());

    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"system", "x"}};
    trigger.stage_filter = Stage::Estimate;
    DispatchOutcome outcome = dispatcher.Dispatch(HookPoint::PreRequest, trigger);
    REQUIRE(outcome.Ok());
    CHECK(outcome.value["estimator"] == "my_custom");
    CHECK(outcome.value["estimatedInputTokens"] == 42);
}
