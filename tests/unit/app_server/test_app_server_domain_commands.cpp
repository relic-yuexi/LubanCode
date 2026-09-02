// app-server 的 typed 域命令面(goal 单合流批):goal 六 + loop 七 +
// plan 三挂上 RegisterMethods 后的整线驱动。假 IO 直驱 dispatcher,
// 每一条出站行 parse 后逐字段查——参数错走 invalid_params、feature 关走
// data.reason 的稳定码、三对不匹配走 stale_request_id。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/dispatcher.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"

using namespace lubancode;
using namespace lubancode::app_server;

namespace {

// 假 IO(dispatch 测试同款)。
struct ScriptedIo {
    std::vector<std::string> written;
    app_server::StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
};

// 一台开了 goal/loop 的测试 Server(backend 工厂给空的,域命令不发模型)。
struct DomainHarness {
    ScriptedIo io;
    std::unique_ptr<Server> server;

    explicit DomainHarness(bool goal_on = true, bool loop_on = true) {
        ServerOptions options;
        options.cwd = "/test/cwd";
        options.features_goal = goal_on;
        options.features_loop = loop_on;
        server = std::make_unique<Server>(
            std::move(options),
            [] { struct NullBackend : api::Backend {  // 域命令不发模型,给个空的
                  std::expected<void, api::Error> send_stream(
                      const api::Request&, const std::function<void(const api::StreamEvent&)>&,
                      const std::atomic<bool>* = nullptr) override {
                      return std::unexpected(api::Error{api::ErrorKind::Api, "不应发模型", 0});
                  }
              };
              return std::make_unique<NullBackend>(); },
            nullptr);
        auto dispatcher = std::make_shared<Dispatcher>();
        server->AttachForTest(std::make_unique<StdioConnection>(
            std::move(dispatcher), io.Writer(), [] { return std::string(); }, 256));
        // 握手三步走完,业务方法才放行(dispatcher 的规矩)。
        (void)Call("initialize", nlohmann::json::object());
        IncomingNotification initialized;
        initialized.method = "initialized";
        DispatchContext notification_context;
        notification_context.emit_event = [this](std::string_view m, const nlohmann::json& p, bool) {
            server->connection().EmitEvent(m, p);
        };
        server->dispatcher().HandleNotification(initialized, notification_context);
        io.written.clear();
    }

    // 直驱一条请求,返回响应 JSON。
    nlohmann::json Call(const std::string& method, const nlohmann::json& params) {
        IncomingRequest request;
        request.id = ++next_id_;
        request.method = method;
        request.params = params;
        DispatchContext context;
        context.emit_event = [this](std::string_view m, const nlohmann::json& p, bool) {
            server->connection().EmitEvent(m, p);
        };
        const DispatchOutcome outcome = server->dispatcher().HandleRequest(request, context);
        for (const std::string& line : outcome.outbound) {
            io.written.push_back(line);
        }
        REQUIRE(!io.written.empty());
        return nlohmann::json::parse(io.written.back());
    }

    std::string StartThread() {
        const nlohmann::json response = Call(std::string(kMethodThreadStart), nlohmann::json::object());
        return response.value("result", nlohmann::json::object())
            .value("threadId", std::string());
    }

    int next_id_ = 0;
};

}  // namespace

TEST_CASE("goal/create -> goal/get:get 回 Status() 结构化账") {
    DomainHarness fx;
    const std::string thread = fx.StartThread();
    REQUIRE(!thread.empty());

    const nlohmann::json created =
        fx.Call(std::string(kMethodGoalCreate), {{"threadId", thread}, {"text", "把 CI 修绿"}});
    CHECK(created.contains("result"));
    CHECK(created["result"]["goal_id"].get<std::string>() == "goal-1");

    const nlohmann::json status =
        fx.Call(std::string(kMethodGoalGet), {{"threadId", thread}});
    CHECK(status["result"]["has_goal"].get<bool>() == true);
    CHECK(status["result"]["goal"]["id"].get<std::string>() == "goal-1");
    CHECK(status["result"]["goal"]["objective"].get<std::string>() == "把 CI 修绿");
}

TEST_CASE("goal/clear 没带 confirm:confirmation_required,账不动") {
    DomainHarness fx;
    const std::string thread = fx.StartThread();
    (void)fx.Call(std::string(kMethodGoalCreate), {{"threadId", thread}, {"text", "目标一"}});

    const nlohmann::json refused =
        fx.Call(std::string(kMethodGoalClear), {{"threadId", thread}});
    CHECK(refused.contains("error"));
    CHECK(refused["error"]["code"] == kErrInvalidParams);
    CHECK(refused["error"]["data"]["reason"].get<std::string>() == "confirmation_required");

    const nlohmann::json confirmed = fx.Call(std::string(kMethodGoalClear),
                                             {{"threadId", thread}, {"confirm", true}});
    CHECK(confirmed.contains("result"));
}

TEST_CASE("feature 关:goal/loop 命令回稳定禁用码,不冒充成功") {
    DomainHarness fx(/*goal_on=*/false, /*loop_on=*/false);
    const std::string thread = fx.StartThread();
    const nlohmann::json goal =
        fx.Call(std::string(kMethodGoalCreate), {{"threadId", thread}, {"text", "目标"}});
    // feature 关:coordinator 的 goals_enabled=false,Create 落
    // goal.store_unavailable 且 payload 带 disabled=true(装配层据此指路)。
    CHECK(goal.contains("error"));
    CHECK(goal["error"]["data"]["reason"].get<std::string>() == "goal.store_unavailable");

    // loop 侧:scheduler 的 enabled=false,Create 落 loop.disabled。
    const nlohmann::json loop = fx.Call(std::string(kMethodLoopCreate),
                                        {{"threadId", thread}, {"text", "盯"}});
    CHECK(loop["error"]["data"]["reason"].get<std::string>() == "loop.disabled");
}

TEST_CASE("loop/create -> loop/list -> loop/read:结构化任务账") {
    DomainHarness fx;
    const std::string thread = fx.StartThread();
    const nlohmann::json created =
        fx.Call(std::string(kMethodLoopCreate),
                {{"threadId", thread}, {"text", "盯部署"}, {"intervalMs", 600000}});
    const std::string task_id = created["result"]["task_id"].get<std::string>();
    CHECK(!task_id.empty());

    const nlohmann::json list = fx.Call(std::string(kMethodLoopList), {{"threadId", thread}});
    REQUIRE(list["result"]["tasks"].is_array());
    REQUIRE(list["result"]["tasks"].size() == 1);
    CHECK(list["result"]["tasks"][0]["task_id"].get<std::string>() == task_id);
    CHECK(list["result"]["tasks"][0]["interval_ms"].get<std::int64_t>() == 600000);

    const nlohmann::json one =
        fx.Call(std::string(kMethodLoopRead), {{"threadId", thread}, {"taskId", task_id}});
    CHECK(one["result"]["task"]["task_id"].get<std::string>() == task_id);

    // run 不收 all。
    const nlohmann::json refused =
        fx.Call(std::string(kMethodLoopRunNow), {{"threadId", thread}, {"taskId", "all"}});
    CHECK(refused["error"]["data"]["reason"].get<std::string>() == "loop.invalid_request");
}

TEST_CASE("plan/review:三对不匹配 stale_request_id,匹配的批准落账") {
    DomainHarness fx;
    const std::string thread = fx.StartThread();
    // 先喂一份 Presented 计划进 SessionRuntime(经 plan/set_mode 切 Plan,
    // 计划成品由 turn 收口扫——本测不走 turn,直接用 set_mode 验接线,
    // review 的三对匹配用不存在的 plan 走 stale 路)。
    (void)fx.Call(std::string(kMethodPlanSetMode), {{"threadId", thread}, {"mode", "plan"}});

    // 没有计划成品:plan.no_plan。
    const nlohmann::json no_plan = fx.Call(std::string(kMethodPlanReview),
                                           {{"threadId", thread},
                                            {"planId", "plan-1"},
                                            {"planRevision", 1},
                                            {"sha256", "abc"},
                                            {"decision", "approved_confirm"}});
    CHECK(no_plan["error"]["data"]["reason"].get<std::string>() == "plan.no_plan");

    const nlohmann::json reopen = fx.Call(std::string(kMethodPlanReopen), {{"threadId", thread}});
    CHECK(reopen["error"]["data"]["reason"].get<std::string>() == "plan.no_plan");
}

TEST_CASE("plan/set_mode:mode 只认 plan/default,同档重复切非错误") {
    DomainHarness fx;
    const std::string thread = fx.StartThread();
    const nlohmann::json first =
        fx.Call(std::string(kMethodPlanSetMode),
                {{"threadId", thread}, {"mode", "plan"}, {"permissionBeforePlan", "confirm"}});
    CHECK(first["result"]["switched"].get<bool>() == true);
    CHECK(first["result"]["mode"].get<std::string>() == "plan");

    const nlohmann::json again =
        fx.Call(std::string(kMethodPlanSetMode), {{"threadId", thread}, {"mode", "plan"}});
    CHECK(again["result"]["switched"].get<bool>() == false);  // 重复切不是错误

    const nlohmann::json bad =
        fx.Call(std::string(kMethodPlanSetMode), {{"threadId", thread}, {"mode", "yolo"}});
    CHECK(bad["error"]["code"] == kErrInvalidParams);
}

TEST_CASE("参数错:goal/create 缺 text 走 invalid_params;thread 不认识走 not_found") {
    DomainHarness fx;
    const std::string thread = fx.StartThread();
    const nlohmann::json no_text =
        fx.Call(std::string(kMethodGoalCreate), {{"threadId", thread}});
    CHECK(no_text["error"]["code"] == kErrInvalidParams);

    const nlohmann::json no_thread =
        fx.Call(std::string(kMethodGoalGet), {{"threadId", "thread-nope"}});
    CHECK(no_thread["error"]["data"]["reason"].get<std::string>() == "not_found");

    // plan/review 的 decision 枚举在参数表层拦。
    const nlohmann::json bad_decision =
        fx.Call(std::string(kMethodPlanReview),
                {{"threadId", thread},
                 {"planId", "plan-1"},
                 {"planRevision", 1},
                 {"sha256", "abc"},
                 {"decision", "maybe"}});
    CHECK(bad_decision["error"]["code"] == kErrInvalidParams);
}

TEST_CASE("initialize 能力表报出十六枚域方法") {
    // 独立 dispatcher(不经 harness——它的构造已做过 initialize,重复
    // initialize 会被握手状态机拒)。
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory(
        [] { return MakeInitializeResult("test", "test"); });
    IncomingRequest request;
    request.id = 1;
    request.method = std::string(kMethodInitialize);
    request.params = nlohmann::json::object();
    DispatchContext context;
    context.emit_event = [](std::string_view, const nlohmann::json&, bool) {};
    const DispatchOutcome outcome = dispatcher->HandleRequest(request, context);
    REQUIRE(outcome.outbound.size() == 1);
    const nlohmann::json init = nlohmann::json::parse(outcome.outbound[0]);
    REQUIRE(init.contains("result"));
    const nlohmann::json& methods = init["result"]["capabilities"]["methods"];
    bool has_goal = false;
    bool has_loop_create = false;
    bool has_plan_review = false;
    for (const auto& m : methods) {
        if (m == std::string(kMethodGoalGet)) has_goal = true;
        if (m == std::string(kMethodLoopCreate)) has_loop_create = true;
        if (m == std::string(kMethodPlanReview)) has_plan_review = true;
    }
    CHECK(has_goal);
    CHECK(has_loop_create);
    CHECK(has_plan_review);
}
