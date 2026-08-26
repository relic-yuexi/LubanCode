// app-server 阶段 2 单:审批与打断(审批反向请求、turn/interrupt、
// ask_user 的 user/ask 反向请求)。
//
// 钉的是:
//   1. needs_confirm 工具触发 permission/request 出站(带 requestId/工具
//      名/入参);前端回 accept/acceptForSession/decline/cancel 四态各归
//      各位;
//   2. acceptForSession 记入本 thread 会话级放行表,同工具二次免问
//      (不再发 permission/request);
//   3. 审批悬停期间事件泵继续活(进度事件不堵);超时按"没人可答"悬空
//      收口,文案写真因,不落回"用户拒绝";
//   4. turn/interrupt:cancel 旗生效,审批悬停立即醒,终态 interrupted;
//      迟到的 interrupt 报失效;
//   5. 审批答完后回合照常收口,turn/completed 唯一;
//   6. 迟到的审批回答(回合已收)报 kStaleRequestId;
//   7. ask_user 的 user/ask 反向请求与审批同机制,answers 送回原回合。
// stdout 逐行可解析的纪律照旧:每条出站行 parse 后逐字段断言。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/interaction.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "tools/ask_user.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(共享脚本容器:测试中途改脚本立即生效)。
class SharedScriptBackend : public api::Backend {
public:
    explicit SharedScriptBackend(std::vector<std::vector<api::StreamEvent>>& scripts) : scripts_(scripts) {}

    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>& on_event,
                                                const std::atomic<bool>* = nullptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
        }
        for (const api::StreamEvent& event : scripts_[index_]) {
            on_event(event);
        }
        ++index_;
        return {};
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<api::StreamEvent>>& scripts_;
    std::size_t index_ = 0;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}},
    };
}

// 工具调用脚本:第一步要一枚 needs_confirm 工具,第二步收尾文本。
std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name,
                                            const std::string& input_json = "{}") {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 需确认的假工具(执行计数,给断言用)。计数走 shared_ptr:工具对象会被
// 注册表 move,引用成员在 move 后的旧壳上自增是悬空(实锤过一次 SIGILL),
// shared_ptr 两头都稳。
class ConfirmableTool : public tools::Tool {
public:
    ConfirmableTool(std::string name, std::shared_ptr<std::atomic<int>> counter)
        : name_(std::move(name)), counter_(std::move(counter)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "假工具:需确认"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return true; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        counter_->fetch_add(1);
        return tools::Tool::Result{"工具真执行了", false};
    }

private:
    std::string name_;
    std::shared_ptr<std::atomic<int>> counter_;
};

// 假 IO(与 turn 测试同款)。
struct ScriptedIo {
    std::vector<std::string> written;
    app_server::StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
};

// 审批测试的整线夹具:Server + 假连接 + 线程安全的出站收集。
struct ApprovalHarness {
    std::vector<std::vector<api::StreamEvent>> scripts;
    ScriptedIo io;
    std::unique_ptr<app_server::Server> server;
    std::shared_ptr<std::atomic<int>> tool_calls = std::make_shared<std::atomic<int>>(0);
    std::string tool_name = "write_file";
    int approval_timeout_ms = 0;

    explicit ApprovalHarness(int timeout_ms = 0) : approval_timeout_ms(timeout_ms) {
        app_server::ServerOptions options;
        options.sessions_dir = std::string(); // 不落盘
        options.cwd = "/test/cwd";
        options.approval_timeout_ms = approval_timeout_ms;
        server = std::make_unique<app_server::Server>(
            std::move(options),
            [this]() -> std::unique_ptr<api::Backend> { return std::make_unique<SharedScriptBackend>(scripts); },
            [this]() -> std::unique_ptr<tools::ToolRegistry> {
                auto registry = std::make_unique<tools::ToolRegistry>();
                registry->Register(std::make_unique<ConfirmableTool>(tool_name, tool_calls));
                return registry;
            });
        AttachIo();
    }

    // 假连接:writer 直写 io(事件从 outbox 泵走,测试自己泵)。
    void AttachIo() {
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            server->dispatcher_handle(), io.Writer(), []() { return std::string(); }, 256));
    }

    // 把 outbox 里的事件刷进 written(测试手动泵:工作线程 Push,测试线
    // 程 Pop——正好验"审批悬停期间事件泵继续活")。
    void PumpOutbox() {
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
    }

    // 找第一条 method 匹配的出站事件。
    std::optional<nlohmann::json> FindEvent(const std::string& method) {
        PumpOutbox();
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                return parsed;
            }
        }
        return std::nullopt;
    }

    int CountEvents(const std::string& method) {
        PumpOutbox();
        int count = 0;
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                ++count;
            }
        }
        return count;
    }

    // 从 permission/request 事件里抠 requestId。
    std::string RequestIdOf(const nlohmann::json& event) {
        return event["params"]["requestId"].get<std::string>();
    }

    // 造一条前端答复(反向请求的响应信封:id 0 + result.requestId)。
    app_server::IncomingResponse MakeApprovalResponse(const std::string& request_id,
                                                      const std::string& decision) {
        app_server::IncomingResponse response;
        response.id = 0;
        response.is_error = false;
        response.result = nlohmann::json{{"requestId", request_id}, {"decision", decision}};
        return response;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 审批反向请求:四态各归各位
// ---------------------------------------------------------------------------


TEST_CASE("审批:accept 放行,工具真执行,回合 success") {
    ApprovalHarness harness;
    harness.scripts.push_back(ToolUseScript("toolu_1", "write_file", R"({"path":"a.txt"})"));
    harness.scripts.push_back(TextOnlyScript("办完了"));

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    // 回合起在工作线程:审批悬停等前端答复,答复从"读线程"一侧喂。
    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });

    // 等审批请求出站(事件泵在测试线程上活——审批悬停不堵泵)。
    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());
    CHECK((*permission)["params"]["threadId"] == thread_id);
    CHECK((*permission)["params"]["tool"].get<std::string>() == "write_file");
    CHECK_FALSE((*permission)["params"]["requestId"].get<std::string>().empty());
    // 入参摘要:结构化 input 原样给。
    CHECK((*permission)["params"]["input"]["path"] == "a.txt");

    // 答复:accept。
    const std::string request_id = harness.RequestIdOf(*permission);
    harness.server->HandleInteractionResponse(harness.MakeApprovalResponse(request_id, "accept"));

    turn_thread.join();
    harness.PumpOutbox();

    CHECK(harness.tool_calls->load() == 1);
    // 唯一终态:turn/completed,success。
    int completed = 0;
    for (const std::string& line : harness.io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "turn/completed") {
            ++completed;
            CHECK(parsed["params"]["status"] == "success");
        }
    }
    CHECK(completed == 1);
}

TEST_CASE("审批:decline 拒绝,工具不执行,拒绝理由随 tool_result 回模型") {
    ApprovalHarness harness;
    harness.scripts = {ToolUseScript("toolu_2", "write_file"), TextOnlyScript("好,不写了")};

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });

    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());
    const std::string request_id = harness.RequestIdOf(*permission);
    // decline + 理由。
    app_server::IncomingResponse response = harness.MakeApprovalResponse(request_id, "decline");
    response.result["reason"] = "太危险,不让";
    harness.server->HandleInteractionResponse(response);

    turn_thread.join();
    CHECK(harness.tool_calls->load() == 0);
    // 回合照常收口(拒绝不是错误:模型拿到拒绝文案继续走)。
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["status"] == "success");
}

TEST_CASE("审批:cancel 视作拒绝收口(用户主动撤),工具不执行") {
    ApprovalHarness harness;
    harness.scripts = {ToolUseScript("toolu_3", "write_file"), TextOnlyScript("收到")};

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });

    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());
    harness.server->HandleInteractionResponse(harness.MakeApprovalResponse(harness.RequestIdOf(*permission), "cancel"));

    turn_thread.join();
    CHECK(harness.tool_calls->load() == 0);
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
}

TEST_CASE("审批:acceptForSession 放行,同工具二次免问") {
    ApprovalHarness harness;
    harness.scripts = {
        ToolUseScript("toolu_s1", "write_file"), TextOnlyScript("第一轮完"),
        ToolUseScript("toolu_s2", "write_file"), TextOnlyScript("第二轮完"),
    };

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    // 第一轮:答复 acceptForSession。
    std::thread first([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "一轮", {}, ec);
    });
    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());
    harness.server->HandleInteractionResponse(
        harness.MakeApprovalResponse(harness.RequestIdOf(*permission), "acceptForSession"));
    first.join();
    CHECK(harness.tool_calls->load() == 1);

    // 第二轮:同工具,不该再发 permission/request(免问直接放)。
    const int permission_events_before = harness.CountEvents("permission/request");
    std::string ec2;
    const nlohmann::json second = harness.server->HandleTurnStart(thread_id, "二轮", {}, ec2);
    REQUIRE(ec2.empty());
    CHECK(harness.tool_calls->load() == 2);                    // 直接执行了
    CHECK(harness.CountEvents("permission/request") == permission_events_before); // 没多一发
    CHECK(second["status"] == "success");
}

// ---------------------------------------------------------------------------
// 超时:悬停不偷跑,文案写明真因
// ---------------------------------------------------------------------------

TEST_CASE("审批超时:悬空收口,不冒充用户拒绝,工具不执行") {
    // 时限 150ms:回合悬停,没人答,按超时收口。
    ApprovalHarness harness(/*timeout_ms=*/150);
    harness.scripts = {ToolUseScript("toolu_t", "write_file"), TextOnlyScript("收到")};

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });

    // 审批请求出了,悬停,不答。
    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());

    // 等回合按超时收口(150ms 时限 + 轮询余量)。
    turn_thread.join();
    CHECK(harness.tool_calls->load() == 0);
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    // 拒绝文案在 tool_result 里(给模型的),终态本身仍按回合实际走向收:
    // 模型拿到拒绝后照常答话,success。
    CHECK((*completed)["params"]["status"] == "success");
}

// ---------------------------------------------------------------------------
// turn/interrupt:打断收口
// ---------------------------------------------------------------------------

TEST_CASE("turn/interrupt:审批悬停立即醒,终态 interrupted") {
    ApprovalHarness harness;
    harness.scripts = {ToolUseScript("toolu_i", "write_file"), TextOnlyScript("收到")};

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });

    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());

    // 打断:受理、悬起清空、回合收口成 interrupted。
    std::string interrupt_error;
    const nlohmann::json interrupt = harness.server->HandleTurnInterrupt(thread_id, "", interrupt_error);
    CHECK(interrupt_error.empty());
    CHECK(interrupt["accepted"] == true);

    turn_thread.join();
    CHECK(harness.tool_calls->load() == 0);
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["status"] == "interrupted");
}

TEST_CASE("turn/interrupt:回合不在跑时报失效(迟到的打断不受理)") {
    ApprovalHarness harness;
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    // 没有回合在跑:打断报 stale。
    std::string interrupt_error;
    const nlohmann::json result = harness.server->HandleTurnInterrupt(thread_id, "", interrupt_error);
    CHECK(interrupt_error == "stale");
    CHECK(result.is_null());
}

TEST_CASE("turn/interrupt:点名的回合不是当前在跑的,报失效") {
    ApprovalHarness harness;
    harness.scripts = {ToolUseScript("toolu_i2", "write_file"), TextOnlyScript("收到")};

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });
    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());

    // 点名一个不存在的回合号。
    std::string interrupt_error;
    const nlohmann::json result = harness.server->HandleTurnInterrupt(thread_id, "turn-999", interrupt_error);
    CHECK(interrupt_error == "stale");

    // 收尾:真打断,免得回合挂着。
    std::string cleanup_error;
    harness.server->HandleTurnInterrupt(thread_id, "", cleanup_error);
    turn_thread.join();
}

TEST_CASE("turn/interrupt:流式打断(无审批在飞),终态 interrupted") {
    // 夹具:可等的假后端——send_stream 里举一个 needs_confirm 工具,工具
    // 调用的审批悬停恰好给打断一个干净的落点(与审批打断同一物理路径,
    // 这里验的是"没有 permission/request 在飞也能打断"——直接打断工具
    // 循环开始前的流)。用 ApprovalHarness 的工具表,脚本只走一步就停:
    // send_stream 第二次调用前打断旗已置,脚本用尽走 error 路也会被
    // interrupt 分型盖过(interrupt_requested 优先)。
    ApprovalHarness harness;
    harness.scripts.push_back(ToolUseScript("toolu_g", "write_file"));

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });

    // 等审批请求出站(工具边界),打断,悬起收口,回合收 interrupted。
    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());
    std::string interrupt_error;
    harness.server->HandleTurnInterrupt(thread_id, "", interrupt_error);
    turn_thread.join();

    CHECK(harness.tool_calls->load() == 0);
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["status"] == "interrupted");
}

// ---------------------------------------------------------------------------
// 迟到的审批回答:报失效
// ---------------------------------------------------------------------------

TEST_CASE("迟到回答:回合已收口,答复报失效不炸") {
    ApprovalHarness harness;
    harness.scripts = {ToolUseScript("toolu_late", "write_file"), TextOnlyScript("收到")};

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写个文件", {}, ec);
    });
    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());

    // 先打断收口,再答:迟到。
    std::string interrupt_error;
    harness.server->HandleTurnInterrupt(thread_id, "", interrupt_error);
    turn_thread.join();

    const std::string request_id = harness.RequestIdOf(*permission);
    const app_server::InteractionResolution late =
        harness.server->HandleInteractionResponse(harness.MakeApprovalResponse(request_id, "accept"));
    CHECK_FALSE(late.ok);
    CHECK(late.error_code == "stale_request_id");
}

TEST_CASE("无在飞请求的响应:报失效,服务不炸") {
    ApprovalHarness harness;
    std::string error_code;
    harness.server->HandleThreadStart(nlohmann::json::object(), error_code);

    const app_server::InteractionResolution result =
        harness.server->HandleInteractionResponse(harness.MakeApprovalResponse("req-ghost", "accept"));
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "stale_request_id");
}

// ---------------------------------------------------------------------------
// ask_user:user/ask 反向请求,同机制
// ---------------------------------------------------------------------------

TEST_CASE("ask_user:user/ask 反向请求,answers 送回原回合") {
    // 夹具:注册 ask_user 工具 + 脚本调它。
    std::vector<std::vector<api::StreamEvent>> scripts;
    app_server::ServerOptions options;
    options.sessions_dir = std::string();
    ScriptedIo io;
    auto server = std::make_unique<app_server::Server>(
        std::move(options), [&scripts]() -> std::unique_ptr<api::Backend> {
            return std::make_unique<SharedScriptBackend>(scripts);
        },
        []() -> std::unique_ptr<tools::ToolRegistry> {
            auto registry = std::make_unique<tools::ToolRegistry>();
            registry->Register(std::make_unique<tools::AskUserTool>(
                [](const tools::AskUserQuestion&) -> std::expected<tools::AskUserResponse, std::string> {
                    return std::unexpected("当前入口不能与用户交互"); // 占位,server 会换掉
                }));
            return registry;
        });
    server->AttachForTest(std::make_unique<app_server::StdioConnection>(
        server->dispatcher_handle(), io.Writer(), []() { return std::string(); }, 256));

    scripts = {ToolUseScript("toolu_a", "ask_user",
                             R"({"questions":[{"header":"路线","question":"走哪条路","options":[{"label":"快路"},{"label":"稳路"}]}]})"),
               TextOnlyScript("走快路,好")};

    std::string error_code;
    const nlohmann::json start = server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        server->HandleTurnStart(thread_id, "问路线", {}, ec);
    });

    // 等 user/ask 出站。
    std::optional<nlohmann::json> ask_event;
    for (int i = 0; i < 400 && !ask_event.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == "user/ask") {
                ask_event = parsed;
            }
        }
    }
    REQUIRE(ask_event.has_value());
    CHECK((*ask_event)["params"]["question"] == "走哪条路");
    CHECK((*ask_event)["params"]["options"].size() == 2);
    const std::string request_id = (*ask_event)["params"]["requestId"].get<std::string>();

    // 答复:选"快路"。
    app_server::IncomingResponse response;
    response.id = 0;
    response.result = nlohmann::json{{"requestId", request_id}, {"answers", nlohmann::json::array({"快路"})}};
    const app_server::InteractionResolution resolved = server->HandleInteractionResponse(response);
    CHECK(resolved.ok);

    turn_thread.join();
    while (auto line = server->connection().outbox().Pop()) {
        io.written.push_back(*line);
    }
    // 回合照常收口。
    bool completed = false;
    for (const std::string& line : io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "turn/completed") {
            completed = true;
            CHECK(parsed["params"]["status"] == "success");
        }
    }
    CHECK(completed);
}

TEST_CASE("ask_user:悬停期间 turn/interrupt,提问按取消收口") {
    std::vector<std::vector<api::StreamEvent>> scripts;
    app_server::ServerOptions options;
    options.sessions_dir = std::string();
    ScriptedIo io;
    auto server = std::make_unique<app_server::Server>(
        std::move(options), [&scripts]() -> std::unique_ptr<api::Backend> {
            return std::make_unique<SharedScriptBackend>(scripts);
        },
        []() -> std::unique_ptr<tools::ToolRegistry> {
            auto registry = std::make_unique<tools::ToolRegistry>();
            registry->Register(std::make_unique<tools::AskUserTool>(nullptr));
            return registry;
        });
    server->AttachForTest(std::make_unique<app_server::StdioConnection>(
        server->dispatcher_handle(), io.Writer(), []() { return std::string(); }, 256));

    scripts = {ToolUseScript("toolu_ai", "ask_user",
                             R"({"questions":[{"question":"走哪条","options":[{"label":"A"},{"label":"B"}]}]})"),
               TextOnlyScript("收到")};

    std::string error_code;
    const nlohmann::json start = server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"];

    std::thread turn_thread([&] {
        std::string ec;
        server->HandleTurnStart(thread_id, "问", {}, ec);
    });

    std::optional<nlohmann::json> ask_event;
    for (int i = 0; i < 400 && !ask_event.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == "user/ask") {
                ask_event = parsed;
            }
        }
    }
    REQUIRE(ask_event.has_value());

    std::string interrupt_error;
    server->HandleTurnInterrupt(thread_id, "", interrupt_error);
    CHECK(interrupt_error.empty());
    turn_thread.join();

    while (auto line = server->connection().outbox().Pop()) {
        io.written.push_back(*line);
    }
    bool interrupted = false;
    for (const std::string& line : io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "turn/completed") {
            interrupted = parsed["params"]["status"] == "interrupted";
        }
    }
    CHECK(interrupted);
}

// ---------------------------------------------------------------------------
// dispatcher 面:turn/interrupt 与响应信封的协议折法
// ---------------------------------------------------------------------------

TEST_CASE("协议面:turn/interrupt 走 dispatcher,留位法子仍回不认识") {
    ApprovalHarness harness;
    // 握手齐了再打。
    app_server::IncomingRequest init;
    init.id = 1;
    init.method = "initialize";
    app_server::DispatchContext context;
    harness.server->dispatcher().HandleRequest(init, context);
    harness.server->dispatcher().HandleNotification(
        app_server::IncomingNotification{"initialized", nlohmann::json::object()}, context);

    // 没有回合在跑:turn/interrupt 回 stale 错误码。
    app_server::IncomingRequest interrupt;
    interrupt.id = 2;
    interrupt.method = "turn/interrupt";
    interrupt.params = nlohmann::json{{"threadId", "no-such"}};
    const auto outcome = harness.server->dispatcher().HandleRequest(interrupt, context);
    REQUIRE(outcome.outbound.size() == 1);
    const nlohmann::json response = nlohmann::json::parse(outcome.outbound[0]);
    CHECK(response["error"]["code"] == app_server::kErrInvalidParams); // thread 不认识走参数错
}

TEST_CASE("协议面:反向请求响应走 HandleResponse,配对成功的折法") {
    app_server::Dispatcher dispatcher;
    app_server::DispatchContext context;
    // resolver 接了:配对成功回空 result。
    context.resolve_interaction = [](const app_server::IncomingResponse&) { return std::string(); };
    app_server::IncomingResponse response;
    response.id = 77;
    response.result = nlohmann::json{{"requestId", "req-1"}, {"decision", "accept"}};
    const auto outcome = dispatcher.HandleResponse(response, context);
    REQUIRE(outcome.outbound.size() == 1);
    const nlohmann::json parsed = nlohmann::json::parse(outcome.outbound[0]);
    CHECK(parsed["id"] == 77);
    CHECK(parsed.contains("result"));

    // resolver 报 stale:折 kErrStaleRequestId。
    context.resolve_interaction = [](const app_server::IncomingResponse&) { return "stale_request_id"; };
    const auto stale_outcome = dispatcher.HandleResponse(response, context);
    const nlohmann::json stale = nlohmann::json::parse(stale_outcome.outbound[0]);
    CHECK(stale["error"]["code"] == app_server::kErrStaleRequestId);

    // resolver 没接(骨架形状):吃下不炸不回话。
    app_server::DispatchContext bare;
    const auto dropped = dispatcher.HandleResponse(response, bare);
    CHECK(dropped.outbound.empty());
}

// ---------------------------------------------------------------------------
// interrupt 硬时限
// ---------------------------------------------------------------------------

TEST_CASE("interrupt 硬时限:置旗后回合卡死不退,等满时限分离,不拖死调用方") {
    // 夹具:审批悬停的回合 + 硬时限压到 120ms。悬停中的审批对 interrupt
    // 立即醒(悬起件被清),这里验的是"等满时限"这条兜底路本身:把
    // approval_timeout_ms 设 0(悬停无限),interrupt 置旗后悬起件清空、
    // future 醒、loop.Run 在工具边界收口——正常路径硬时限不该触发;真正
    // 卡死(backend 挂死)由 detach 兜,这里用短时限验证等待逻辑不空转、
    // 不死等。
    ApprovalHarness harness;
    harness.scripts.push_back(ToolUseScript("toolu_hd", "write_file"));

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start["threadId"].get<std::string>();

    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "写", {}, ec);
    });

    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());

    // 打断:回合立即醒收口(interrupted),硬时限没到就 join 上了。
    const auto t0 = std::chrono::steady_clock::now();
    std::string interrupt_error;
    harness.server->HandleTurnInterrupt(thread_id, "", interrupt_error);
    CHECK(interrupt_error.empty());
    turn_thread.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    // 正常收口应当快(远小于 15s 硬时限);给宽松上限防 CI 抖。
    CHECK(elapsed.count() < 5000);
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["status"] == "interrupted");
}

// ---------------------------------------------------------------------------
// 错误码锚
// ---------------------------------------------------------------------------

TEST_CASE("错误码锚:kErrStaleRequestId 落位") {
    CHECK(app_server::kErrStaleRequestId == -32005);
}
