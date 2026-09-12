// Soul 会话冻结单 P0(agent 层锁定边界,§5.1/§5.3):用假后端捕获真实
// 请求构建结果,断言——
//   锁定前可改;首请求锁定;锁定后下一请求 system 字节不变;首请求
//   失败/取消仍锁定、重试沿用快照;新会话(新 Agent)采用新值;换场
//   (AdoptSessionSoul)整份重灌;锁定回调恰触发一次;轮次后到达的
//   "排队命令"(SetSoul)被拒——并发命令与首请求的竞态按锁定边界判定,
//   不靠 response 是否返回判断。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/prompts.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端:记下收到的每份 Request(断言 system 字节用)。
// cancel_after_event_index 有值时,发完脚本里下标为该值的事件就模拟
// "流被 ESC 掐断"(ErrorKind::Cancelled)——测取消路径的锁定。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;
    std::optional<std::size_t> cancel_after_event_index;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        const auto& script = scripts[idx];
        for (std::size_t i = 0; i < script.size(); ++i) {
            on_event(script[i]);
            if (cancel_after_event_index.has_value() && i == *cancel_after_event_index) {
                return std::unexpected(api::Error{api::ErrorKind::Cancelled, "FakeBackend: 模拟取消", 0});
            }
        }
        return {};
    }
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

agent::AgentProfile SoulProfile(const std::string& soul, const std::string& soul_name) {
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system prompt";
    profile.soul = soul;
    profile.soul_name = soul_name;
    return profile;
}

}  // namespace

TEST_CASE("锁定前可改:首请求之前 SetSoul 生效,system 带新魂") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("", "default"));
    agent::TurnWiring callbacks;

    CHECK_FALSE(loop.soul_locked());
    CHECK(loop.SetSoul("文风一:简短有力"));
    CHECK(loop.SetSoulName("wenge-1"));

    REQUIRE(loop.Run("问", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    // 魂经 WithSoul 压轴注入,正文剥注释后进 system。
    CHECK(agent::WithSoul("system prompt", "文风一:简短有力") == backend.captured_requests[0].system);
}

TEST_CASE("首请求锁定:此后 SetSoul 被拒,下一请求 system 字节不变") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好"), TextOnlyScript("又好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("文风甲", "jia"));
    agent::TurnWiring callbacks;

    REQUIRE(loop.Run("第一问", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    REQUIRE(loop.soul_locked());

    // 锁定后:策略同步(SetSoul/SetSoulName)一律被拒,快照一字不动。
    CHECK_FALSE(loop.SetSoul("文风乙"));
    CHECK_FALSE(loop.SetSoulName("yi"));

    REQUIRE(loop.Run("第二问", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    // 重试/续轮沿用快照:魂段字节与首请求完全一致。
    const std::string& first = backend.captured_requests[0].system;
    const std::string& second = backend.captured_requests[1].system;
    const std::size_t soul_at = first.rfind("文风甲");
    REQUIRE(soul_at != std::string::npos);
    // 整份 system 的魂段(压轴段)两请求逐字节一致:先截出首请求的魂段
    // 起点之后全文,第二份同位置起比对。
    CHECK(first.substr(soul_at) == second.substr(second.rfind("文风甲")));
}

TEST_CASE("首请求失败(脚本用尽报错)仍锁定:重试沿用快照,换魂被拒") {
    FakeBackend backend;
    // 一份脚本都不给:第一次请求就报"脚本用完了"(Api 错误)。
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("文风甲", "jia"));
    agent::TurnWiring callbacks;

    CHECK_FALSE(loop.Run("必败之问", callbacks).has_value());
    // 失败不解锁(§5.1):锁定发生在请求准备一开始,不是 response 返回后。
    CHECK(loop.soul_locked());
    CHECK_FALSE(loop.SetSoul("中途换魂"));

    backend.scripts = {TextOnlyScript("好了")};
    REQUIRE(loop.Run("重试", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    CHECK(backend.captured_requests[1].system.find("文风甲") != std::string::npos);
}

TEST_CASE("首请求中途取消仍锁定:重试沿用快照") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("长答")};
    backend.cancel_after_event_index = 0;  // 发完 MessageStart 就掐断
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("文风甲", "jia"));
    agent::TurnWiring callbacks;

    const auto result = loop.Run("会被打断的问", callbacks);
    // 取消不是错误;无论返回形状如何,锁定已经发生。
    CHECK(loop.soul_locked());
    CHECK_FALSE(loop.SetSoul("取消后换魂"));
    (void)result;

    backend.cancel_after_event_index.reset();
    backend.scripts = {TextOnlyScript("重试成功")};
    REQUIRE(loop.Run("重试", callbacks).has_value());
    CHECK(backend.captured_requests.back().system.find("文风甲") != std::string::npos);
}

TEST_CASE("新会话采用新值:新 Agent 读新默认,与旧会话快照互不相干") {
    FakeBackend backend_a;
    backend_a.scripts = {TextOnlyScript("好")};
    FakeBackend backend_b;
    backend_b.scripts = {TextOnlyScript("好")};
    tools::ToolRegistry registry;
    agent::TurnWiring callbacks;

    agent::Agent session_a(backend_a, registry, SoulProfile("旧会话魂", "old"));
    REQUIRE(session_a.Run("问", callbacks).has_value());
    REQUIRE(session_a.soul_locked());

    // 新会话(比如 /clear 之后)拿新默认建 Agent:照常吃自己的魂。
    agent::Agent session_b(backend_b, registry, SoulProfile("新会话魂", "new"));
    REQUIRE(session_b.Run("问", callbacks).has_value());
    CHECK(backend_b.captured_requests[0].system.find("新会话魂") != std::string::npos);
    CHECK(backend_b.captured_requests[0].system.find("旧会话魂") == std::string::npos);
    // 旧会话的锁定不串进新会话,新会话首请求前仍是可改草稿——但此刻
    // 它自己也发了首请求,自然锁定。
    CHECK(session_b.soul_locked());
    // 多会话并存:旧会话的锁定快照不受新会话影响。
    CHECK_FALSE(session_a.SetSoul("旧会话中途换魂"));
}

TEST_CASE("换场重灌(AdoptSessionSoul):resume/clear 换场即换魂,不受旧锁挡") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好"), TextOnlyScript("又好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("旧场魂", "old"));
    agent::TurnWiring callbacks;

    REQUIRE(loop.Run("问", callbacks).has_value());
    REQUIRE(loop.soul_locked());

    // resume 恢复源场快照(或 /clear 重读默认):整份换,含锁定态。
    loop.AdoptSessionSoul("resumed", "恢复场魂", /*locked=*/true);
    REQUIRE(loop.Run("再问", callbacks).has_value());
    CHECK(backend.captured_requests[1].system.find("恢复场魂") != std::string::npos);
    CHECK(loop.soul_locked());

    // /clear 语义:换默认草稿、未锁定。
    loop.AdoptSessionSoul("default", std::string(), /*locked=*/false);
    CHECK_FALSE(loop.soul_locked());
    CHECK(loop.SetSoul("clear 后可改"));
}

TEST_CASE("锁定回调恰触发一次:幂等重入与重建场景不多发") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好"), TextOnlyScript("又好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("文风甲", "jia"));
    agent::TurnWiring callbacks;

    int fired = 0;
    agent::AgentWiring wiring;
    wiring.on_session_soul_locked = [&fired]() { ++fired; };
    loop.SetWiring(std::move(wiring));

    // 显式幂等调用 + 两轮 Run:回调只响第一次。
    loop.LockSessionSoul();
    loop.LockSessionSoul();
    REQUIRE(loop.Run("问", callbacks).has_value());
    REQUIRE(loop.Run("再问", callbacks).has_value());
    CHECK(fired == 1);
}

TEST_CASE("并发命令与首请求的竞态:轮次收口后处理的 /soul(SetSoul)被拒") {
    // 主线程串行模型:命令与轮次不并发执行,竞态落在"排队命令在轮次
    // 收口后才被处理"——那时锁定已发生,SetSoul 拒绝,system 不变。
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好"), TextOnlyScript("再好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, SoulProfile("排队前的魂", "before"));
    agent::TurnWiring callbacks;

    REQUIRE(loop.Run("首问", callbacks).has_value());
    // 用户在轮次进行中敲的 /soul,轮末才处理:
    CHECK_FALSE(loop.SetSoul("排队进来的新魂"));
    const std::string system_before = backend.captured_requests[0].system;
    REQUIRE(loop.Run("下一问", callbacks).has_value());
    CHECK(backend.captured_requests[1].system == system_before);
}
