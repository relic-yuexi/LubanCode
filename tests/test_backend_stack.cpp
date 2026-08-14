// BackendStack 请求改写层的性状测试:按 InteractiveLoop 的真实包装次序
// (index -> spinner -> instructions -> soul -> think -> model -> 真实 client)
// 组装各层,对最终发出的 Request 做断言——包装次序一错,系统提示里
// "延迟工具索引段 / 模型专属指令段 / 魂段"的先后就错,这类回归编译期看
// 不出来,只能靠对最终 Request 断言。SpinnerBackend 依赖真终端的
// cli::Spinner(实现在可执行文件一侧),单测不构造它;它不改 Request,
// 略去不影响次序断言。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "api/backend.hpp"
#include "app/backend_stack.hpp"

namespace {

// 假内芯:记录收到的 Request,不当真发网络。
class CapturingBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)on_event;
        (void)cancel;
        captured = request;
        ++calls;
        return {};
    }

    lubancode::api::Request captured;
    int calls = 0;
};

lubancode::api::Request BaseRequest() {
    lubancode::api::Request request;
    request.model = "config-model";
    request.system = "BASE-SYSTEM";
    return request;
}

constexpr auto kNoEvent = [](const lubancode::api::StreamEvent&) {};

}  // namespace

using namespace lubancode::app;

TEST_CASE("包装次序:索引段 -> 模型指令段 -> 魂段依次追加,魂压轴") {
    CapturingBackend inner;
    auto current_model = std::make_shared<std::string>("glm-x");
    auto current_think = std::make_shared<std::string>("high");
    auto current_instructions = std::make_shared<std::string>("MODEL-ONLY-INSTRUCTIONS");
    auto current_soul = std::make_shared<std::string>("<!-- 说明:给人看的,不注入 -->\nSOUL-BODY");

    ModelOverrideBackend model_backend(inner, current_model);
    ThinkOverrideBackend think_backend(model_backend, current_think, current_model, nullptr);
    SoulOverlayBackend soul_backend(think_backend, current_soul);
    ModelInstructionsBackend instructions_backend(soul_backend, current_instructions);
    DeferredIndexBackend index_backend(instructions_backend, [] { return std::string("INDEX-SEGMENT"); });

    CHECK(index_backend.send_stream(BaseRequest(), kNoEvent).has_value());

    REQUIRE(inner.calls == 1);
    const auto& sent = inner.captured;
    CHECK(sent.model == "glm-x");
    CHECK(sent.reasoning_effort == "high");

    const std::size_t base_pos = sent.system.find("BASE-SYSTEM");
    const std::size_t index_pos = sent.system.find("INDEX-SEGMENT");
    const std::size_t instructions_pos = sent.system.find("MODEL-ONLY-INSTRUCTIONS");
    const std::size_t soul_pos = sent.system.find("SOUL-BODY");
    REQUIRE(base_pos != std::string::npos);
    REQUIRE(index_pos != std::string::npos);
    REQUIRE(instructions_pos != std::string::npos);
    REQUIRE(soul_pos != std::string::npos);
    CHECK(base_pos < index_pos);
    CHECK(index_pos < instructions_pos);
    CHECK(instructions_pos < soul_pos);
}

TEST_CASE("全空透传:空 think/指令/索引段与纯注释的魂不改动 system") {
    CapturingBackend inner;
    auto current_model = std::make_shared<std::string>("glm-x");
    auto current_think = std::make_shared<std::string>("");
    auto current_instructions = std::make_shared<std::string>("");
    auto current_soul = std::make_shared<std::string>("<!-- 默认魂:整个文件只有一行注释 -->");

    ModelOverrideBackend model_backend(inner, current_model);
    ThinkOverrideBackend think_backend(model_backend, current_think, current_model, nullptr);
    SoulOverlayBackend soul_backend(think_backend, current_soul);
    ModelInstructionsBackend instructions_backend(soul_backend, current_instructions);
    DeferredIndexBackend index_backend(instructions_backend, [] { return std::string(); });

    CHECK(index_backend.send_stream(BaseRequest(), kNoEvent).has_value());

    CHECK(inner.captured.system == "BASE-SYSTEM");
    CHECK(inner.captured.reasoning_effort.empty());
    CHECK(inner.captured.model == "glm-x");
    CHECK(inner.captured.extra_body.empty());
}

TEST_CASE("会话中改 shared_ptr 指的值,下一次请求立即生效") {
    CapturingBackend inner;
    auto current_model = std::make_shared<std::string>("glm-a");
    auto current_think = std::make_shared<std::string>("low");

    ModelOverrideBackend model_backend(inner, current_model);
    ThinkOverrideBackend think_backend(model_backend, current_think, current_model, nullptr);

    CHECK(think_backend.send_stream(BaseRequest(), kNoEvent).has_value());
    CHECK(inner.captured.model == "glm-a");
    CHECK(inner.captured.reasoning_effort == "low");

    *current_model = "glm-b";
    *current_think = "high";
    CHECK(think_backend.send_stream(BaseRequest(), kNoEvent).has_value());
    CHECK(inner.captured.model == "glm-b");
    CHECK(inner.captured.reasoning_effort == "high");
}

TEST_CASE("RebuildableBackend:构造/重建/析构不崩,对外引用地址不变") {
    lubancode::config::Config config;
    config.wire = lubancode::config::Wire::Anthropic;
    config.base_url = "http://127.0.0.1:9";
    config.auth_token = "test-key";

    auto rebuildable = std::make_unique<RebuildableBackend>(config);
    lubancode::api::Backend* address = rebuildable.get();
    rebuildable->Rebuild(config);
    rebuildable->Rebuild(config);
    CHECK(rebuildable.get() == address);
    rebuildable.reset();  // 真析构
}
