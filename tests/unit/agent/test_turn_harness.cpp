// agent::TurnHarness(骨架拆解批三:harness 合流)的单测。钉四样:
//   1. turn 数式(单子定案):一次 turn = 1 条本轮接纳的 user 消息 + M 个
//      step(M 次模型请求)+ M 条 assistant 消息,末条不含 ToolUseBlock——
//      主回合与子代理共用的 DriveTurn 交回来的账要正好对上这条数式;
//   2. Stop 续跑环:钩子拉闸(continue=false)且没续过 -> 再收口一轮,最多
//      续一次(stop_hook_active 防咬尾);没人拉闸一轮就停;
//   3. CancelChain:单信号直通(不起线程)、多信号合并(任一置位即并置)、
//      Stop() 唤醒 join 不漏线程;
//   4. ClassifyTurnEnd 收场分型:completed/stopped/budget/failed 各态与
//      裁定次序(墙钟 > 打断 > 步数闸 > 报错 > 空历史 > 输出预算 > 无结论);
//   5. 续投批的退回语义:领了批的那轮失败,restore 必被调——"取走了不等
//      于送到了"。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/turn_harness.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(与 test_agent_tool.cpp 同款):每调一次 send_stream
// 取下一组脚本,顺带记请求,方便对账。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class FakeTool : public tools::Tool {
public:
    std::string name() const override { return "fake_tool"; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"工具结果", false}; }
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "fake_tool"},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::size_t CountToolUseBlocks(const agent::Agent& loop) {
    std::size_t count = 0;
    for (const auto& message : loop.history()) {
        if (message.role != api::Role::Assistant) {
            continue;
        }
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                ++count;
            }
        }
    }
    return count;
}

// 字符串 -> user 消息(Agent::Run 字符串重载的同一包装)。
api::Message UserText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

const api::Message* LastAssistant(const agent::Agent& loop) {
    for (auto it = loop.history().rbegin(); it != loop.history().rend(); ++it) {
        if (it->role == api::Role::Assistant) {
            return &*it;
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// turn 数式(单子钉死的那条):1 user + M step + M assistant,末条无工具块。
// ---------------------------------------------------------------------------
TEST_CASE("DriveTurn:两步工具 + 一步正文,turn 数式对账——1 user + 3 step + 3 assistant,末条无 ToolUseBlock") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        ToolUseScript("t2"),
        TextScript("收工:结论在此。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::DriveOptions options;
    const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("干三步活"), options);

    CHECK(report.ok);
    CHECK_FALSE(report.cancelled);
    CHECK_FALSE(report.hit_step_limit);
    CHECK(report.steps_used == 3);  // M = 3 次模型请求
    REQUIRE(backend.captured_requests.size() == 3);

    // 数式:恰 1 条本轮 user 消息(不含 tool_result 的合成 user——那是 step
    // 的工具批,另计)、M 条 assistant。
    const auto& history = loop.history();
    int user_texts = 0;
    int assistants = 0;
    int tool_result_messages = 0;
    for (const auto& message : history) {
        if (message.role != api::Role::User) {
            if (message.role == api::Role::Assistant) {
                ++assistants;
            }
            continue;
        }
        bool has_tool_result = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolResultBlock>(block)) {
                has_tool_result = true;
            }
        }
        if (has_tool_result) {
            ++tool_result_messages;
        } else {
            ++user_texts;
        }
    }
    CHECK(user_texts == 1);                  // 1 条本轮接纳的 user 消息
    CHECK(assistants == 3);                  // M 条 assistant
    CHECK(tool_result_messages == 2);        // M-1 批工具结果回填
    CHECK(CountToolUseBlocks(loop) == 2);    // 工具块只在前 M-1 条
    // 末条 assistant 无 ToolUseBlock,是交给调用方的最终回复。
    const api::Message* last = LastAssistant(loop);
    REQUIRE(last != nullptr);
    bool last_has_tool_use = false;
    for (const auto& block : last->content) {
        if (std::holds_alternative<api::ToolUseBlock>(block)) {
            last_has_tool_use = true;
        }
    }
    CHECK_FALSE(last_has_tool_use);
}

TEST_CASE("DriveTurn:报错轮 ok=false 带错误文案,步数不计") {
    FakeBackend backend;
    backend.scripts = {};  // 脚本空:第一次请求即失败
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("问一句"), agent::DriveOptions{});
    CHECK_FALSE(report.ok);
    CHECK(report.error.find("脚本用完") != std::string::npos);
    CHECK(report.steps_used == 0);
}

TEST_CASE("DriveTurn:续投源领批后再跑一轮,封账即收;失败轮按批退回") {
    SUBCASE("领批再跑一轮") {
        FakeBackend backend;
        backend.scripts = {
            TextScript("第一轮完。"),
            TextScript("第二轮处理增量。"),
        };
        tools::ToolRegistry registry;
        agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

        int pulls = 0;
        agent::DriveOptions options;
        options.continuation = [&pulls]() -> std::optional<agent::ContinuationBatch> {
            ++pulls;
            if (pulls == 1) {
                agent::ContinuationBatch batch;
                batch.input = "[主会话用户介入] 补一句";
                return batch;
            }
            return std::nullopt;  // 第二次问询:封账
        };
        const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("开工"), options);
        CHECK(report.ok);
        CHECK(report.steps_used == 2);
        CHECK(pulls == 2);
        REQUIRE(backend.captured_requests.size() == 2);
        // 续投轮的输入就是批次拼好的那段。
        bool saw_continuation = false;
        for (const auto& request : backend.captured_requests) {
            for (const auto& message : request.messages) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<api::TextBlock>(&block);
                        text != nullptr && text->text.find("主会话用户介入") != std::string::npos) {
                        saw_continuation = true;
                    }
                }
            }
        }
        CHECK(saw_continuation);
    }

    SUBCASE("领批那轮失败,restore 必被调") {
        FakeBackend backend;
        backend.scripts = {
            TextScript("第一轮完。"),
            // 第二次请求:脚本用完 -> 报错,领批的那轮失败。
        };
        tools::ToolRegistry registry;
        agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

        bool restored = false;
        agent::DriveOptions options;
        options.continuation = [&restored]() -> std::optional<agent::ContinuationBatch> {
            agent::ContinuationBatch batch;
            batch.input = "[主会话用户介入] 补一句";
            batch.restore = [&restored]() { restored = true; };
            return batch;
        };
        const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("开工"), options);
        CHECK_FALSE(report.ok);  // 续投轮报错如实交账
        CHECK(restored);         // 批退回未送——"取走了不等于送到了"
    }
}

// ---------------------------------------------------------------------------
// Stop 续跑环。
// ---------------------------------------------------------------------------
TEST_CASE("RunStopContinuation:拉闸一次续一轮,防咬尾最多一次;没人拉闸一轮即停") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("第一轮的结论。"),
        TextScript("续跑轮的结论。"),
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    REQUIRE(agent::AgentLoop::Run(loop, UserText("先收口一轮"), agent::TurnWiring{}).has_value());

    agent::DriveReport report;
    agent::StopOptions options;
    options.label = "[stop 钩子续跑,非用户输入] ";
    options.final_text = [] { return std::string("第一轮的结论。"); };

    SUBCASE("没人拉闸:emit 一次就收") {
        int emits = 0;
        options.emit = [&emits](bool, const std::string& last_text) {
            ++emits;
            CHECK(last_text == "第一轮的结论。");
            return hooks::HookEventResult{};  // blocked=false
        };
        agent::RunStopContinuation(loop, agent::TurnWiring{}, options, report);
        CHECK(emits == 1);
        CHECK(report.steps_used == 0);  // 没续跑,不加步数
        CHECK(backend.captured_requests.size() == 1);
    }

    SUBCASE("拉闸一次:续跑一轮后第二次 emit 带防咬尾旗,不再续") {
        int emits = 0;
        int continue_requests = 0;
        options.emit = [&emits](bool stop_hook_active, const std::string&) {
            ++emits;
            hooks::HookEventResult result;
            if (!stop_hook_active) {
                result.blocked = true;  // 第一次拉闸"还不能停"
                result.block_reason = "还有活没收口";
            }
            return result;
        };
        options.on_continue_request = [&continue_requests](const std::string& reason) {
            ++continue_requests;
            CHECK(reason == "还有活没收口");
        };
        agent::RunStopContinuation(loop, agent::TurnWiring{}, options, report);
        CHECK(emits == 2);                       // 第二次带 stop_hook_active=true,不再续
        CHECK(continue_requests == 1);           // 报信口恰一次
        CHECK(report.steps_used == 1);           // 续跑一轮记一步
        CHECK(backend.captured_requests.size() == 2);
        // 续跑输入带标识前缀,不装用户输入。
        const api::Request& continuation_request = backend.captured_requests.back();
        bool saw_label = false;
        for (const auto& message : continuation_request.messages) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block);
                    text != nullptr && text->text.find("[stop 钩子续跑,非用户输入]") != std::string::npos) {
                    saw_label = true;
                }
            }
        }
        CHECK(saw_label);
    }

    SUBCASE("emit 为空:整环跳过") {
        agent::RunStopContinuation(loop, agent::TurnWiring{}, options, report);
        CHECK(report.steps_used == 0);
    }
}

// ---------------------------------------------------------------------------
// CancelChain。
// ---------------------------------------------------------------------------
TEST_CASE("CancelChain:单信号直通,多信号合并,Stop 收线程") {
    std::atomic<bool> a{false};
    std::atomic<bool> b{false};

    SUBCASE("零信号:nullptr") {
        agent::CancelChain chain;
        CHECK(chain.Start() == nullptr);
        chain.Stop();
    }

    SUBCASE("单信号:直通原地址,置位立即可见") {
        agent::CancelChain chain;
        chain.Add(&a);
        const std::atomic<bool>* merged = chain.Start();
        REQUIRE(merged == &a);  // 直通即原址:与合流前的单指针透传一字不差
        CHECK_FALSE(merged->load());
        a.store(true);
        CHECK(merged->load());
        chain.Stop();  // 无线程可收,空操作不崩
    }

    SUBCASE("多信号:任一置位即合并置位,Stop 唤醒 join") {
        agent::CancelChain chain;
        chain.Add(&a);
        chain.Add(&b);
        const std::atomic<bool>* merged = chain.Start();
        REQUIRE(merged != nullptr);
        REQUIRE(merged != &a);
        REQUIRE(merged != &b);
        CHECK_FALSE(merged->load());
        b.store(true, std::memory_order_release);
        // 合并线程 20ms 一拍,给它一点时间。
        for (int i = 0; i < 100 && !merged->load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(merged->load());
        chain.Stop();  // 必须能收(join),不挂线程
        CHECK(true);
    }
}

// ---------------------------------------------------------------------------
// 收场分型。
// ---------------------------------------------------------------------------
TEST_CASE("ClassifyTurnEnd:各态分型与裁定次序") {
    SUBCASE("正常完成") {
        agent::TurnEndgame end;
        end.final_text = "结论";
        const auto verdict = agent::ClassifyTurnEnd(end);
        CHECK(verdict.status == agent::TurnVerdict::Status::Completed);
        CHECK(verdict.reason == agent::TurnVerdict::Reason::None);
        CHECK(std::string(agent::TurnVerdict::StatusTag(verdict.status)) == "completed");
    }
    SUBCASE("无结论(子代理要文本):NoFinalText 失败;主回合不要文本则 Worked") {
        agent::TurnEndgame sub;
        sub.final_text = "";
        sub.require_final_text = true;
        const auto sub_verdict = agent::ClassifyTurnEnd(sub);
        CHECK(sub_verdict.status == agent::TurnVerdict::Status::Failed);
        CHECK(sub_verdict.reason == agent::TurnVerdict::Reason::NoFinalText);

        agent::TurnEndgame main_turn;
        main_turn.final_text = "";
        main_turn.require_final_text = false;
        CHECK(agent::ClassifyTurnEnd(main_turn).status == agent::TurnVerdict::Status::Completed);
    }
    SUBCASE("打断:Stopped/UserStop") {
        agent::TurnEndgame end;
        end.cancelled = true;
        end.final_text = "半截";
        const auto verdict = agent::ClassifyTurnEnd(end);
        CHECK(verdict.status == agent::TurnVerdict::Status::Stopped);
        CHECK(verdict.reason == agent::TurnVerdict::Reason::UserStop);
    }
    SUBCASE("墙钟压过打断:Failed/WallClockTimeout") {
        agent::TurnEndgame end;
        end.cancelled = true;  // 看门狗把流掐断,loop 按打断收场
        end.wall_clock = true;
        const auto verdict = agent::ClassifyTurnEnd(end);
        CHECK(verdict.status == agent::TurnVerdict::Status::Failed);
        CHECK(verdict.reason == agent::TurnVerdict::Reason::WallClockTimeout);
    }
    SUBCASE("步数闸:BudgetExhausted/StepLimit") {
        agent::TurnEndgame end;
        end.hit_step_limit = true;
        end.final_text = "部分结论";
        const auto verdict = agent::ClassifyTurnEnd(end);
        CHECK(verdict.status == agent::TurnVerdict::Status::BudgetExhausted);
        CHECK(verdict.reason == agent::TurnVerdict::Reason::StepLimit);
    }
    SUBCASE("报错:ApiError;错误文案含『上下文』按 MaxContext") {
        agent::TurnEndgame end;
        end.error = "请求失败:连接被拒";
        const auto verdict = agent::ClassifyTurnEnd(end);
        CHECK(verdict.status == agent::TurnVerdict::Status::Failed);
        CHECK(verdict.reason == agent::TurnVerdict::Reason::ApiError);

        agent::TurnEndgame ctx;
        ctx.error = "历史超长,上下文装不下";
        CHECK(agent::ClassifyTurnEnd(ctx).reason == agent::TurnVerdict::Reason::MaxContext);
    }
    SUBCASE("空历史:ProtocolError") {
        agent::TurnEndgame end;
        end.history_empty = true;
        CHECK(agent::ClassifyTurnEnd(end).reason == agent::TurnVerdict::Reason::ProtocolError);
    }
    SUBCASE("输出预算耗尽:BudgetExhausted/OutputBudget(独立状态,不混 NoFinalText)") {
        agent::TurnEndgame end;
        end.output_budget_exhausted = true;
        end.final_text = "";
        const auto verdict = agent::ClassifyTurnEnd(end);
        CHECK(verdict.status == agent::TurnVerdict::Status::BudgetExhausted);
        CHECK(verdict.reason == agent::TurnVerdict::Reason::OutputBudget);
    }
    SUBCASE("裁定次序:打断压过步数闸,步数闸压过无结论") {
        agent::TurnEndgame end;
        end.cancelled = true;
        end.hit_step_limit = true;
        end.final_text = "";
        CHECK(agent::ClassifyTurnEnd(end).reason == agent::TurnVerdict::Reason::UserStop);

        agent::TurnEndgame limit;
        limit.hit_step_limit = true;
        limit.final_text = "";
        CHECK(agent::ClassifyTurnEnd(limit).reason == agent::TurnVerdict::Reason::StepLimit);
    }
}
