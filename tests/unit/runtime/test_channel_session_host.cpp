// ChannelSessionHost 单测(多渠道消息接入单阶段 3):单飞、限额并行、
// per-session FIFO、confirm fail closed 纯函数、TurnIngress 投影与
// WorkKind::ChannelTurn 同档优先级。
//
// 真源:TODO §15.3/§16.1;configuration.md §8。引擎用记账假件,不起模型。

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "channel/channel_router.hpp"
#include "runtime/channel_session_host.hpp"
#include "runtime/session_work_scheduler.hpp"
#include "runtime/turn_ingress.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

// 记账假引擎:每个 session_key 一只,记下跑过的轮与可控行为。
class RecordingEngine : public ChannelTurnEngine {
public:
    RecordingEngine(std::string key, std::vector<std::string>* ran_log)
        : key_(std::move(key)), ran_log_(ran_log) {}

    agent::RunOutcome RunTurn(const TurnIngress& ingress, std::string* reply_text,
                              std::string* error) override {
        ran_log_->push_back(key_ + ":" + FirstText(ingress));
        if (reply_text != nullptr) *reply_text = "reply@" + key_;
        if (error != nullptr) error->clear();
        return agent::RunOutcome{};
    }

private:
    static std::string FirstText(const TurnIngress& ingress) {
        for (const auto& block : ingress.message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                return text->text;
            }
        }
        return "";
    }
    std::string key_;
    std::vector<std::string>* ran_log_;
};

TurnIngress MakeIngress(const std::string& session_key, const std::string& text,
                        const std::string& delivery_id) {
    TurnIngress ingress;
    ingress.source = TurnSource::Channel;
    ingress.session_key = session_key;
    ingress.ingress_delivery_id = delivery_id;
    ingress.message.role = api::Role::User;
    ingress.message.content.push_back(api::TextBlock{text});
    return ingress;
}

}  // namespace

TEST_CASE("confirm fail closed:allowlist 明确允许才放行,deny 永远赢") {
    channel::ToolRoutePolicy tools;
    tools.allow = {"read_file", "search"};
    tools.deny = {"search", "shell"};

    CHECK(ChannelConfirmAllows(tools, "read_file"));
    CHECK_FALSE(ChannelConfirmAllows(tools, "search"));    // deny 压过 allow
    CHECK_FALSE(ChannelConfirmAllows(tools, "shell"));     // deny
    CHECK_FALSE(ChannelConfirmAllows(tools, "write_file"));  // 不在名单

    // allow 为空(binding 没设上限,也没有任何明确允许):须确认的工具
    // 全拒——渠道会话没有审批渠道,这正是 §16.1 的"没有远端审批能力:
    // 拒绝",不是"走 Agent 默认确认"。
    channel::ToolRoutePolicy deny_only;
    deny_only.deny = {"shell"};
    CHECK_FALSE(ChannelConfirmAllows(deny_only, "read_file"));
    CHECK_FALSE(ChannelConfirmAllows(deny_only, "shell"));
    channel::ToolRoutePolicy empty;
    CHECK_FALSE(ChannelConfirmAllows(empty, "read_file"));

    // 拒绝文案:点名审批渠道缺失,不冒充"用户拒绝"。
    const std::string denial = ChannelToolDenialText("shell");
    CHECK(denial.find("渠道会话") != std::string::npos);
    CHECK(denial.find("shell") != std::string::npos);
    CHECK(denial.find("用户拒绝") == std::string::npos);
}

TEST_CASE("WorkKind::ChannelTurn 与用户排队消息同档") {
    CHECK(WorkPriority(WorkKind::ChannelTurn) == WorkPriority(WorkKind::UserQueuedTurn));
    CHECK(WorkPriority(WorkKind::ChannelTurn) < WorkPriority(WorkKind::GoalContinuation));
    CHECK(WorkPriority(WorkKind::ChannelTurn) < WorkPriority(WorkKind::LoopTick));
    CHECK(WorkPriority(WorkKind::ChannelTurn) < WorkPriority(WorkKind::Maintenance));
    // 公平泵里 ChannelTurn 与 UserQueuedTurn 同档 FIFO。
    SessionWork channel_work;
    channel_work.kind = WorkKind::ChannelTurn;
    channel_work.id = "ch-1";
    SessionWork user_work;
    user_work.kind = WorkKind::UserQueuedTurn;
    user_work.id = "u-1";
    FairnessCounter fairness;
    const auto picked = PumpNextWork({channel_work, user_work}, fairness);
    REQUIRE(picked.has_value());
    CHECK(picked->id == "ch-1");  // 同档按到达序,先到先得
}

TEST_CASE("host:同 session 单飞,不同 session 并行限额") {
    ChannelSessionHost::Options options;
    options.max_active_channel_turns = 2;
    ChannelSessionHost host(options);

    CHECK(host.TryBeginTurn("s1") == ChannelSessionHost::BeginRefusal::None);
    CHECK(host.TryBeginTurn("s2") == ChannelSessionHost::BeginRefusal::None);
    CHECK(host.TryBeginTurn("s1") == ChannelSessionHost::BeginRefusal::SingleFlight);
    CHECK(host.TryBeginTurn("s3") == ChannelSessionHost::BeginRefusal::Capacity);
    host.EndTurn("s1");
    CHECK(host.TryBeginTurn("s3") == ChannelSessionHost::BeginRefusal::None);
    host.EndTurn("s2");
    host.EndTurn("s3");
    CHECK(host.active_turn_count() == 0);
    CHECK_FALSE(host.session_busy("s1"));
}

TEST_CASE("host:PumpOne 按 FIFO 跑,同 session 排队,不同 session 各建引擎") {
    std::vector<std::string> ran;
    ChannelSessionHost host;
    host.SetEngineFactory([&ran](const std::string& key) {
        return std::make_unique<RecordingEngine>(key, &ran);
    });

    // 同一 session 两条 + 另一 session 一条:提交序 s1:a, s1:b, s2:c。
    host.Submit(MakeIngress("s1", "a", "d1"));
    host.Submit(MakeIngress("s1", "b", "d2"));
    host.Submit(MakeIngress("s2", "c", "d3"));
    REQUIRE(host.pending_count() == 3);

    auto first = host.PumpOne();
    REQUIRE(first.has_value());
    CHECK(first->session_key == "s1");
    CHECK(first->ok);
    CHECK(first->reply_text == "reply@s1");
    CHECK(first->ingress_delivery_id == "d1");

    auto second = host.PumpOne();
    REQUIRE(second.has_value());
    CHECK(second->session_key == "s1");
    CHECK(second->ingress_delivery_id == "d2");

    auto third = host.PumpOne();
    REQUIRE(third.has_value());
    CHECK(third->session_key == "s2");

    CHECK_FALSE(host.PumpOne().has_value());  // 队列空
    REQUIRE(ran.size() == 3);
    CHECK(ran[0] == "s1:a");
    CHECK(ran[1] == "s1:b");  // 同 session 保序
    CHECK(ran[2] == "s2:c");
    CHECK(host.engine_count() == 2);  // 两只 conversation 两场 session
}

TEST_CASE("host:busy 的 session 让路,别的 session 先跑") {
    std::vector<std::string> ran;
    ChannelSessionHost host;
    host.SetEngineFactory([&ran](const std::string& key) {
        return std::make_unique<RecordingEngine>(key, &ran);
    });
    host.Submit(MakeIngress("s1", "a", "d1"));
    host.Submit(MakeIngress("s2", "b", "d2"));

    // 人为占住 s1(模拟另一泵线程正在跑):PumpOne 只能跑 s2。
    REQUIRE(host.TryBeginTurn("s1") == ChannelSessionHost::BeginRefusal::None);
    auto picked = host.PumpOne();
    REQUIRE(picked.has_value());
    CHECK(picked->session_key == "s2");
    host.EndTurn("s1");
    // s1 空出来后能跑了。
    picked = host.PumpOne();
    REQUIRE(picked.has_value());
    CHECK(picked->session_key == "s1");
}

TEST_CASE("host:没装工厂 Submit 报 NoFactory,PumpOne 空转") {
    ChannelSessionHost host;
    auto result = host.Submit(MakeIngress("s1", "a", "d1"));
    CHECK(result.status == ChannelSessionHost::SubmitResult::Status::NoFactory);
    CHECK_FALSE(host.PumpOne().has_value());
}

TEST_CASE("host:限额之下的两路并行(两线程各跑一只 turn)") {
    std::vector<std::string> ran;
    ChannelSessionHost::Options options;
    options.max_active_channel_turns = 2;
    ChannelSessionHost host(options);
    host.SetEngineFactory([&ran](const std::string& key) {
        return std::make_unique<RecordingEngine>(key, &ran);
    });
    host.Submit(MakeIngress("s1", "a", "d1"));
    host.Submit(MakeIngress("s2", "b", "d2"));

    auto first = host.PumpOne();
    auto second = host.PumpOne();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->session_key != second->session_key);
    // 两只都收场,闸归零。
    CHECK(host.active_turn_count() == 0);
}
