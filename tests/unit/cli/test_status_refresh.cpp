// 两单流式输入/状态栏 bug 的回归:
//  1) "context 状态栏回合内不刷新":
//     - ContextTracker::ApplyUsage:实测覆盖、缺 usage(四项全零)不清零只
//       标旧值、带 cache 的组、两次请求覆盖不累加;
//     - 状态行局部更新(WithContextUpdate):只改 context/tokens 两段,其他
//       段原样保住;旧值渲染带 ~ 前缀;
//     - BuildCallbacks::on_usage 接线:主请求 usage 更新 tracker 并发布
//       状态;子代理 usage 只进累计花销,不碰 tracker、不发布状态;
//     - footer 挂起期间发布:只改数据、不落一个字节(发布与重画分开)。
//  2) "流式中 Shift+Tab 切档失效":切档与空闲路同源——SetConfirmMode/
//     CurrentConfirmMode 读写同一枚 SharedEditor 档位,轮转走同一个
//     NextConfirmMode 纯函数(三档循环本身另有 test_line_editor 钉着);
//     真终端的流式按键手感留作手测,不进 ctest。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/turn_runner.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/live_transcript.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "config/config.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端,写法同 test_agent_tool.cpp:每调一次 send_stream
// 按调用次序取下一组脚本吐出去。
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

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text, api::Usage usage) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", usage},
    };
}

cli::StatusPanelData BasePanelData() {
    cli::StatusPanelData data;
    data.model = "test-model";
    data.cwd = "D:\\proj";
    data.git_branch = "main";
    data.provider = "prov";
    data.effort = "high";
    data.rec = "REC · demo";
    data.context_percent = 7;
    data.used_tokens = 70;
    data.window_tokens = 1000;
    return data;
}

}  // namespace

// ---------------------------------------------------------------------------
// ContextTracker::ApplyUsage:实测/缺 usage/带 cache/覆盖不累加
// ---------------------------------------------------------------------------

TEST_CASE("ContextTracker::ApplyUsage: 实测 usage 覆盖占用并清掉旧值标记") {
    cli::ContextTracker tracker(1000);
    tracker.ApplyUsage(api::Usage{300, 50});
    CHECK(tracker.current_tokens() == 350);
    CHECK(tracker.UsagePercent() == 35);
    CHECK_FALSE(tracker.usage_stale());
}

TEST_CASE("ContextTracker::ApplyUsage: 四项全零(provider 没回 usage)不清零、只标旧值") {
    cli::ContextTracker tracker(1000);
    tracker.ApplyUsage(api::Usage{300, 50});
    tracker.ApplyUsage(api::Usage{});  // 缺 usage:全零默认值
    CHECK(tracker.current_tokens() == 350);  // 保留最近一次已确认值
    CHECK(tracker.UsagePercent() == 35);
    CHECK(tracker.usage_stale());  // 但标明这不是本次实测
    // 下一次实测到达,覆盖旧数、清掉标记。
    tracker.ApplyUsage(api::Usage{100, 20});
    CHECK(tracker.current_tokens() == 120);
    CHECK_FALSE(tracker.usage_stale());
}

TEST_CASE("ContextTracker::ApplyUsage: 带 cache_read/cache_creation 的组按完整公式入账") {
    cli::ContextTracker tracker(2000);
    tracker.ApplyUsage(api::Usage{144, 0, 1472, 0});
    CHECK(tracker.current_tokens() == 1616);  // input + cache_read
    CHECK(tracker.last_cache_read_tokens() == 1472);
    tracker.ApplyUsage(api::Usage{100, 20, 300, 500});
    CHECK(tracker.current_tokens() == 920);  // 四项全加
}

TEST_CASE("ContextTracker::ApplyUsage: 两次请求覆盖,不累加") {
    cli::ContextTracker tracker(1000);
    tracker.ApplyUsage(api::Usage{300, 50});
    tracker.ApplyUsage(api::Usage{100, 20});
    CHECK(tracker.current_tokens() == 120);
}

// ---------------------------------------------------------------------------
// 状态行局部更新:只动 context/tokens,其他段保住;旧值渲染带 ~
// ---------------------------------------------------------------------------

TEST_CASE("WithContextUpdate: 只改 context/tokens 两段,其余字段原样保住") {
    cli::StatusPanelData data = BasePanelData();
    const cli::StatusPanelData updated = cli::WithContextUpdate(data, 42, 420, 1000, true);
    CHECK(updated.context_percent == 42);
    CHECK(updated.used_tokens == 420);
    CHECK(updated.window_tokens == 1000);
    CHECK_FALSE(updated.context_stale);
    // 其他段一个不动。
    CHECK(updated.model == "test-model");
    CHECK(updated.cwd == "D:\\proj");
    CHECK(updated.git_branch == "main");
    CHECK(updated.provider == "prov");
    CHECK(updated.effort == "high");
    CHECK(updated.rec == "REC · demo");
    // 入参原值不被就地改动(纯函数,拷贝进拷贝出)。
    CHECK(data.context_percent == 7);
}

TEST_CASE("WithContextUpdate: measured=false 把数字标成旧值") {
    const cli::StatusPanelData updated = cli::WithContextUpdate(BasePanelData(), 7, 70, 1000, false);
    CHECK(updated.context_stale);
}

TEST_CASE("WithContextUpdate: 缓存注记跟着局部更新,缺省时抹掉旧注记") {
    const cli::StatusPanelData updated =
        cli::WithContextUpdate(BasePanelData(), 7, 70, 1000, true, "缓存命中 1.2k(60%)");
    CHECK(updated.cache_note == "缓存命中 1.2k(60%)");
    // 不带注记参数(缺省)时抹掉旧注记,不是留着上一帧的。
    const cli::StatusPanelData cleared = cli::WithContextUpdate(updated, 7, 70, 1000, true);
    CHECK(cleared.cache_note.empty());
    // 注记挂在 tokens 段尾部渲染(REC 非空会恒挂第一段,清掉只钉 tokens)。
    const std::vector<std::string> items{"tokens"};
    cli::StatusPanelData note_only = updated;
    note_only.rec.clear();
    const auto segments = cli::BuildStatusPanelSegments(items, cli::ConfirmMode::Confirm, note_only);
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].text == "70/1000 · 缓存命中 1.2k(60%)");
}

TEST_CASE("BuildCacheNote: 一次实测都没有留空;有命中/未报告/未启用/零命中各有说法") {
    cli::SetLanguage("zh-CN");  // 断言按中文文案钉死,不跟系统语言走
    cli::ContextTracker tracker(1000);
    CHECK(cli::BuildCacheNote(tracker, true).empty());

    // 有命中:摆本场累计命中与命中率(1k hit / 2k 输入 = 50%)。
    tracker.ApplyUsage(api::Usage{1000, 10, 1000, 0});
    CHECK(cli::BuildCacheNote(tracker, true) == "缓存命中 1000(50%)");

    // 零命中、服务端结论未知:如实"缓存 0 命中",不伪造 0%。
    cli::ContextTracker cold(1000);
    cold.ApplyUsage(api::Usage{1000, 10, 0, 0});
    CHECK(cli::BuildCacheNote(cold, true) == "缓存 0 命中");

    // 零命中、服务端明说禁用:写"服务端未启用缓存"。
    cold.set_server_prefix_caching(false);
    CHECK(cli::BuildCacheNote(cold, true) == "服务端未启用缓存");

    // 最近一次没回 usage:写"缓存未报告"。
    cli::ContextTracker silent(1000);
    silent.ApplyUsage(api::Usage{1000, 10, 0, 0});
    CHECK(cli::BuildCacheNote(silent, false) == "缓存未报告");
}

TEST_CASE("BuildStatusPanelSegments: 旧值时 context/tokens 段带 ~ 前缀,实测时不带") {
    const std::vector<std::string> items{"context", "tokens"};  // permission_mode 不在,mode 无关
    cli::StatusPanelData measured = BasePanelData();
    measured.rec.clear();  // REC 非空会恒挂第一段,这里只钉 context/tokens 两段
    auto segments = cli::BuildStatusPanelSegments(items, cli::ConfirmMode::Confirm, measured);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].text == "context 7%");
    CHECK(segments[1].text == "70/1000");

    cli::StatusPanelData stale = BasePanelData();
    stale.rec.clear();
    stale = cli::WithContextUpdate(stale, 7, 70, 1000, false);
    segments = cli::BuildStatusPanelSegments(items, cli::ConfirmMode::Confirm, stale);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].text == "~context 7%");
    CHECK(segments[1].text == "~70/1000");
}

// ---------------------------------------------------------------------------
// BuildCallbacks::on_usage 接线
// ---------------------------------------------------------------------------

TEST_CASE("BuildCallbacks::on_usage: 主请求 usage 更新 tracker 并发布状态,其他段保住") {
    cli::SetStatusLineData(BasePanelData(), {"permission_mode", "model", "cwd", "git_branch", "context", "tokens"},
                           " · ");

    cli::ContextTracker tracker(1000);
    runtime::TurnUsageStats stats;
    cli::Theme theme;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> expanded{false};
    cli::ToolDisplay display(transcript, theme, /*console=*/false, nullptr, &cancel_flag, &expanded);
    cli::StreamBodyTracker body(theme, /*enabled=*/false);
    tools::ToolRegistry registry;  // 没有 agent 工具:主回调路径
    std::set<std::string> always_allowed;
    hooks::HookDispatcher hooks;  // 空 dispatcher:不挂 hook 回调,与"没配 hooks"同待遇

    const agent::Callbacks callbacks =
        app::BuildCallbacks(/*auto_confirm=*/false, always_allowed, theme, stats, tracker, registry, &hooks, display,
                            body, /*allow_commands=*/{}, /*deny_commands=*/{});

    callbacks.on_usage(api::UsageReport{api::Usage{300, 50}, 0, "msg_1", "test-model"});
    CHECK(tracker.current_tokens() == 350);
    CHECK(stats.input_tokens() == 300);
    CHECK(stats.output_tokens() == 50);
    CHECK(stats.request_count() == 1);
    // 逐步流水账:on_usage 落的是一条 StepUsageRecord,身份齐。
    REQUIRE(stats.steps.size() == 1);
    CHECK(stats.steps[0].step_index == 0);
    CHECK(stats.steps[0].request_id == "msg_1");
    CHECK(stats.steps[0].model == "test-model");
    CHECK(stats.steps[0].reported);

    const cli::StatusPanelData snapshot = cli::SnapshotStatusLineValues();
    CHECK(snapshot.context_percent == 35);
    CHECK(snapshot.used_tokens == 350);
    CHECK(snapshot.window_tokens == 1000);
    CHECK_FALSE(snapshot.context_stale);
    // 发布是局部更新:其他段还是整份重建那一份,一个不丢。
    CHECK(snapshot.model == "test-model");
    CHECK(snapshot.cwd == "D:\\proj");
    CHECK(snapshot.git_branch == "main");
    CHECK(snapshot.provider == "prov");
    CHECK(snapshot.effort == "high");
    CHECK(snapshot.rec == "REC · demo");
}

TEST_CASE("BuildCallbacks::on_usage: 第二次请求覆盖发布,不累加;缺 usage 标旧值不清零") {
    cli::SetStatusLineData(BasePanelData(), {"context", "tokens"}, " · ");
    cli::ContextTracker tracker(1000);
    runtime::TurnUsageStats stats;
    cli::Theme theme;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> expanded{false};
    cli::ToolDisplay display(transcript, theme, false, nullptr, &cancel_flag, &expanded);
    cli::StreamBodyTracker body(theme, false);
    tools::ToolRegistry registry;
    std::set<std::string> always_allowed;
    hooks::HookDispatcher hooks;  // 空 dispatcher:不挂 hook 回调,与"没配 hooks"同待遇
    const agent::Callbacks callbacks =
        app::BuildCallbacks(false, always_allowed, theme, stats, tracker, registry, &hooks, display, body,
                            /*allow_commands=*/{}, /*deny_commands=*/{});

    callbacks.on_usage(api::UsageReport{api::Usage{300, 50}, 0, "m1", "test-model"});
    callbacks.on_usage(api::UsageReport{api::Usage{100, 20}, 1, "m2", "test-model"});
    // tracker 覆盖式:120;花销统计累加式:400/70/2——两本账各归各。
    CHECK(tracker.current_tokens() == 120);
    CHECK(stats.input_tokens() == 400);
    CHECK(stats.output_tokens() == 70);
    CHECK(stats.request_count() == 2);
    CHECK(cli::SnapshotStatusLineValues().used_tokens == 120);

    callbacks.on_usage(api::UsageReport{api::Usage{}, 2, "m3", "test-model"});  // provider 没回 usage
    CHECK(tracker.current_tokens() == 120);  // 不清零
    CHECK(stats.request_count() == 3);       // 请求次数照记
    const cli::StatusPanelData snapshot = cli::SnapshotStatusLineValues();
    CHECK(snapshot.used_tokens == 120);
    CHECK(snapshot.context_stale);  // 状态行数据标旧,渲染带 ~
}

TEST_CASE("BuildCallbacks::on_usage: 子代理 usage 只进累计花销,不碰 tracker、不发布状态") {
    cli::SetStatusLineData(BasePanelData(), {"context", "tokens"}, " · ");

    FakeBackend backend;
    backend.scripts = {TextOnlyScript("子代理的结论", api::Usage{500, 100})};
    tools::ToolRegistry sub_registry;

    cli::ContextTracker tracker(1000);
    runtime::TurnUsageStats stats;
    cli::Theme theme;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> expanded{false};
    cli::ToolDisplay display(transcript, theme, false, nullptr, &cancel_flag, &expanded);
    cli::StreamBodyTracker body(theme, false);
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<tools::AgentTool>(backend, sub_registry, "/work/dir"));
    std::set<std::string> always_allowed;
    hooks::HookDispatcher hooks;  // 空 dispatcher:不挂 hook 回调,与"没配 hooks"同待遇
    const agent::Callbacks callbacks =
        app::BuildCallbacks(false, always_allowed, theme, stats, tracker, registry, &hooks, display, body,
                            /*allow_commands=*/{}, /*deny_commands=*/{});

    // BuildCallbacks 内部给 agent 工具灌了转发钩子;跑一轮子代理(500+100
    // tokens),花销统计要吃到,主 context 与状态行数据都不能动。
    const tools::Tool::Result result = registry.Find("agent")->execute(nlohmann::json{{"title", "干点活"}, {"prompt", "干点活"}});
    CHECK_FALSE(result.is_error);
    CHECK(stats.input_tokens() == 500);
    CHECK(stats.output_tokens() == 100);
    CHECK(stats.request_count() == 1);
    CHECK(tracker.current_tokens() == 0);  // 子代理的上下文不并进主 context
    CHECK(tracker.usage_stale() == false);
    const cli::StatusPanelData snapshot = cli::SnapshotStatusLineValues();
    CHECK(snapshot.used_tokens == 70);  // 还是预置那份,没被子代理发布碰过
    CHECK(snapshot.context_percent == 7);
    CHECK_FALSE(snapshot.context_stale);
}

// ---------------------------------------------------------------------------
// footer 挂起期间发布:只改数据,不落笔
// ---------------------------------------------------------------------------

TEST_CASE("footer 挂起期间发布:数据更新,一个字节都不落笔;恢复后取到新值") {
    cli::SetStatusLineData(BasePanelData(), {"context", "tokens"}, " · ");
    cli::BeginStreamFooter(cli::Theme{}, /*enabled=*/false);  // 无真控制台:footer 不启用

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    {
        const cli::StreamFooterSuspendScope suspend;  // 挂起(ask_user/确认菜单同款)
        cli::UpdateStatusLineContext(30, 300, 1000, true);
        CHECK(cli::SnapshotStatusLineValues().used_tokens == 300);  // 数据改了
    }  // 摘挂起:footer 没启用,补画是空操作,同样不落笔
    std::cout.rdbuf(old_buf);

    CHECK(captured.str().empty());  // 发布与重画分开:挂起期间不抢屏
    const cli::StatusPanelData snapshot = cli::SnapshotStatusLineValues();
    CHECK(snapshot.used_tokens == 300);  // 恢复后第一帧的数据源就是新值
    CHECK(snapshot.model == "test-model");  // 局部更新照旧保住其他段
    cli::EndStreamFooter();
}

// ---------------------------------------------------------------------------
// RunTurn 静默档(查看态下的后台回流单):用户正看某只子代理时,main 的
// 回流轮照常跑(模型请求、usage/context 记账),但一切输出只进 transcript
// 台账、一个字节不上屏;正文归档成 assistant 条目,回 main 重铺可见。
// ---------------------------------------------------------------------------

// 不需确认的假工具:静默档里真跑一遍,断言条目照进台账。
class QuietProbeTool : public tools::Tool {
public:
    std::string name() const override { return "quiet_probe"; }
    std::string description() const override { return "probe"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++calls;
        last_input = input;
        return {"探针完成", false};
    }

    int calls = 0;
    nlohmann::json last_input = nlohmann::json::object();
};

TEST_CASE("RunTurn 静默档:正文与统计不上屏,归档成 assistant 条目,usage 照记") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("结论:方案可行。\n依据有三。", api::Usage{400, 60})};
    tools::ToolRegistry registry;
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    cli::ContextTracker tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    const app::RunTurnResult result =
        app::RunTurn(loop, "后台子代理有新结果送达", /*auto_confirm=*/false, always_allowed, cli::Theme{}, tracker,
                    registry, /*hook_dispatcher=*/nullptr, /*is_console=*/false, transcript,
                    /*todo_state=*/nullptr, &expanded, /*allow_commands=*/{}, /*deny_commands=*/{},
                    /*completion_agent=*/nullptr, /*recorder=*/nullptr, /*silent=*/true);
    std::cout.rdbuf(old_buf);

    CHECK(result.status == 0);
    CHECK(captured.str().empty());           // 静默收货:一个字节都没上屏(查看帧零扰动)
    CHECK(tracker.current_tokens() == 460);  // context 照常入账(第三桩是上游症状,这里钉死通路)
    CHECK(cli::SnapshotStatusLineValues().used_tokens == 460);  // 状态行数据源同步发布

    // 正文归档成 assistant 条目:回 main 时重铺/Ctrl+E 全文可见(不静默丢输出)。
    REQUIRE(transcript.size() == 1);
    CHECK(transcript[0].tool_name == "assistant");
    CHECK(transcript[0].status == cli::TranscriptStatus::Ok);
    CHECK(transcript[0].full_output.find("方案可行") != std::string::npos);
    REQUIRE(transcript[0].summary_lines.size() == 2);
    CHECK(transcript[0].summary_lines[0] == "结论:方案可行。");
    CHECK(transcript[0].summary_lines[1] == "依据有三。");
}

TEST_CASE("RunTurn 非静默对照:同一轮照常上屏,不因静默档的闸误伤正路") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("正常轮的正文", api::Usage{100, 10})};
    tools::ToolRegistry registry;
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    cli::ContextTracker tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    const app::RunTurnResult result =
        app::RunTurn(loop, "用户的话", /*auto_confirm=*/false, always_allowed, cli::Theme{}, tracker, registry,
                     /*hook_dispatcher=*/nullptr, /*is_console=*/false, transcript,
                     /*todo_state=*/nullptr, &expanded, /*allow_commands=*/{}, /*deny_commands=*/{},
                     /*completion_agent=*/nullptr, /*recorder=*/nullptr, /*silent=*/false);
    std::cout.rdbuf(old_buf);

    CHECK(result.status == 0);
    CHECK(captured.str().find("正常轮的正文") != std::string::npos);  // 正文照打
    CHECK(captured.str().find("100") != std::string::npos);           // 统计行照打(输入 100)
    CHECK(tracker.current_tokens() == 110);
    CHECK(transcript.empty());  // 非静默档不归档 assistant 条目(正文已在屏上)
}

TEST_CASE("RunTurn 静默档:工具与思考只进台账,工具真执行、条目齐全") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"想一想再动手"},
            api::ContentBlockDone{0},
            api::ToolUseStart{1, "toolu_1", "quiet_probe"},
            api::ToolUseInputDelta{1, "{\"n\":7}"},
            api::ContentBlockDone{1},
            api::MessageDone{"tool_use", api::Usage{200, 30}},
        },
        TextOnlyScript("干完了。", api::Usage{150, 20}),
    };
    QuietProbeTool* probe = new QuietProbeTool();
    tools::ToolRegistry registry;
    registry.Register(std::unique_ptr<QuietProbeTool>(probe));
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    cli::ContextTracker tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    const app::RunTurnResult result =
        app::RunTurn(loop, "后台子代理有新结果送达", /*auto_confirm=*/true, always_allowed, cli::Theme{}, tracker,
                    registry, /*hook_dispatcher=*/nullptr, /*is_console=*/false, transcript,
                    /*todo_state=*/nullptr, &expanded, /*allow_commands=*/{}, /*deny_commands=*/{},
                    /*completion_agent=*/nullptr, /*recorder=*/nullptr, /*silent=*/true);
    std::cout.rdbuf(old_buf);

    CHECK(result.status == 0);
    CHECK(captured.str().empty());      // 工具卡/思考块/正文全都没上屏
    CHECK(probe->calls == 1);           // 工具真执行了(静默是显示档,不是跳过)
    REQUIRE(probe->last_input.value("n", 0) == 7);

    // 台账齐全:思考块 + 工具条目 + assistant 正文条目。
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].kind == cli::TranscriptKind::Thinking);
    CHECK(transcript[0].full_output == "想一想再动手");
    CHECK(transcript[1].kind == cli::TranscriptKind::Tool);
    CHECK(transcript[1].tool_name == "quiet_probe");
    CHECK(transcript[1].status == cli::TranscriptStatus::Ok);
    CHECK(transcript[2].tool_name == "assistant");
    CHECK(transcript[2].full_output == "干完了。");
    CHECK(tracker.current_tokens() == 170);  // 两次请求的 usage 都入账(覆盖式:150+20)
}

// ---------------------------------------------------------------------------
// 流式 Shift+Tab 切档:与空闲路同一枚 SharedEditor 档位、同一个轮转纯函数
// ---------------------------------------------------------------------------

TEST_CASE("切档与空闲路同源:CurrentConfirmMode/SetConfirmMode 读写同一枚 SharedEditor 档位") {
    cli::SetConfirmMode(cli::ConfirmMode::Confirm);
    // 监听线程那条新分支做的正是这两步:NextConfirmMode 轮转 + 写回
    // SharedEditor;档位只有这一处存储,footer/空闲状态行都现查它。
    cli::SetConfirmMode(cli::NextConfirmMode(cli::CurrentConfirmMode()));
    CHECK(cli::CurrentConfirmMode() == cli::ConfirmMode::Auto);
    cli::SetConfirmMode(cli::NextConfirmMode(cli::CurrentConfirmMode()));
    CHECK(cli::CurrentConfirmMode() == cli::ConfirmMode::Yolo);
    cli::SetConfirmMode(cli::NextConfirmMode(cli::CurrentConfirmMode()));
    CHECK(cli::CurrentConfirmMode() == cli::ConfirmMode::Confirm);  // 连切一圈回原点
    // 复位,别把会话级状态泄漏给别的测试。
    CHECK(cli::CurrentConfirmMode() == cli::ConfirmMode::Confirm);
}

// ---------------------------------------------------------------------------
// 逐步 usage 流水账(前缀缓存守恒单第一期):三步台账各有身份,第二笔
// 命中率单独可见,整轮按 token 总和重算,不回报记 unknown 不伪造 0%。
// ---------------------------------------------------------------------------

TEST_CASE("UsageStats: 逐步流水账——三笔各有 step/request id,命中率按 token 总和重算") {
    runtime::TurnUsageStats stats;
    // 第一步:冷启动全 miss。
    stats.Add(api::UsageReport{api::Usage{50000, 80, 0, 0}, 0, "req_a", "deepseek-v4-pro"});
    // 第二步:工具往返,大命中(49k hit / 1k miss = 98%)。
    stats.Add(api::UsageReport{api::Usage{1000, 50, 49000, 0}, 1, "req_b", "deepseek-v4-pro"});
    // 第三步:工具表变了,冷 miss,带 epoch 断因。
    api::UsageReport third{api::Usage{51000, 60, 0, 0}, 2, "req_c", "deepseek-v4-pro"};
    third.cache_epoch = 2;
    third.epoch_break_reason = "tools_changed";
    stats.Add(third);

    REQUIRE(stats.steps.size() == 3);
    CHECK(stats.steps[0].step_index == 0);
    CHECK(stats.steps[1].step_index == 1);
    CHECK(stats.steps[2].step_index == 2);
    CHECK(stats.steps[0].request_id == "req_a");
    CHECK(stats.steps[1].request_id == "req_b");
    CHECK(stats.steps[2].request_id == "req_c");
    CHECK(stats.steps[2].epoch_break_reason == "tools_changed");

    // 第二笔命中率单独显示,不被整轮平均吞掉。
    CHECK(stats.steps[1].cache_hit_percent() == 98);
    CHECK(stats.steps[0].cache_hit_percent() == 0);  // 实测过,真 0%
    CHECK(stats.steps[2].cache_hit_percent() == 0);

    // 整轮命中率按 token 总和重算:hit=49000,total input=50000+50000+51000
    // =151000,49000/151000≈32.45%→32。三只百分比平均是 (0+98+0)/3≈33,
    // 两种算法的差正好被钉住。
    CHECK(stats.total_input_tokens() == 151000);
    CHECK(stats.cache_hit_percent() == 32);

    CHECK(stats.input_tokens() == 102000);
    CHECK(stats.cache_read_tokens() == 49000);
    CHECK(stats.output_tokens() == 190);
    CHECK(stats.request_count() == 3);
}

TEST_CASE("UsageStats: provider 不回 usage 记 unknown,不伪造 0%") {
    runtime::TurnUsageStats stats;
    // 四项全零 = provider 没在流末给 usage。
    stats.Add(api::UsageReport{api::Usage{}, 0, "", "m"});
    REQUIRE(stats.steps.size() == 1);
    CHECK_FALSE(stats.steps[0].reported);
    CHECK(stats.steps[0].cache_hit_percent() == -1);  // unknown,不是 0
    CHECK(stats.cache_hit_percent() == -1);           // 整轮一笔实测都没有
    CHECK(stats.request_count() == 1);                // 请求照数
}
