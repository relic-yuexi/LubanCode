// /usage 端到端冒烟驱动(Token 账本单 A2,手动跑不进 ctest):
//   usage_smoke_driver <临时目录>
// 幕一(flag 开):真 TrajectorySessionLedger 开账,真 AgentLoop+假后端跑
//   一轮工具回合(两笔 main_turn 请求,usage 真报),再经旁路桥手记一笔
//   compact_reduce;/usage 与 /usage --by purpose、--json 的输出逐行打给
//   肉眼对账(数字要跟假后端钉死的 usage 对得上)。
// 幕二(flag 关):trajectory 传空,/usage 走内存粗账降级,明说"账未开"。
// 退出码 0 = 两幕都跑完;失败打印到 stderr 退 1。
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/model_router.hpp"  // ModelUsageLedger(幕二降级面的内存粗账)
#include "agent/prompt_assembler.hpp"
#include "agent/resolved_prompt_builder.hpp"
#include "api/backend.hpp"
#include "app/commands/usage_commands.hpp"
#include "cli/theme.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 假后端:第一轮 tool_use(900 in/1200 read/80 out),第二轮收口
//(1500 in/30 out)。数字钉死,报告对不上就是链路坏了。
class TurnBackend final : public api::Backend {
public:
    int calls = 0;
    std::expected<void, api::Error> send_stream(
        const api::Request&, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        ++calls;
        if (calls == 1) {
            on_event(api::MessageStart{"resp-usage-1", "test-model"});
            on_event(api::ToolUseStart{0, "call-1", "fake_read"});
            on_event(api::ToolUseInputDelta{0, "{\"path\":\"README.md\"}"});
            on_event(api::ContentBlockDone{0});
            api::MessageDone done;
            done.stop_reason = "tool_use";
            done.usage.input_tokens = 900;
            done.usage.output_tokens = 80;
            done.usage.cache_read_tokens = 1200;
            done.usage_reported = true;
            on_event(done);
            return {};
        }
        on_event(api::MessageStart{"resp-usage-2", "test-model"});
        on_event(api::TextDelta{"读完了,42 行。"});
        on_event(api::ContentBlockDone{0});
        api::MessageDone done;
        done.stop_reason = "end_turn";
        done.usage.input_tokens = 1500;
        done.usage.output_tokens = 30;
        done.usage_reported = true;
        on_event(done);
        return {};
    }
};

class FakeTool final : public tools::Tool {
public:
    std::string name() const override { return "fake_read"; }
    std::string description() const override { return "usage smoke fake tool"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"共 42 行。", false}; }
};

int Fail(const std::string& message) {
    std::fprintf(stderr, "usage_smoke_driver: %s\n", message.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "用法: usage_smoke_driver <临时目录>\n");
        return 1;
    }
    const std::filesystem::path root = argv[1];
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "workspace", ec);

    const cli::Theme theme;

    // ---- 幕一:flag 开,真账本 ----
    runtime::TrajectorySessionLedger::Options options;
    options.workspace_root = root / "workspace";
    options.trajectories_root = root / "trajectories";
    options.readable_workspace_name = "usage-smoke";
    options.launch_cwd = "D:/demo";
    options.lubancode_version = "smoke";
    auto ledger = runtime::TrajectorySessionLedger::Open(std::move(options));
    if (!ledger.has_value()) {
        return Fail("账本开不出: " + ledger.error());
    }
    {
        runtime::TrajectoryTurnBridge::Identity identity{"demo", "responses", "terminal"};
        auto bridge = ledger->NewTurnBridge(identity);
        bridge->BeginTurn("turn-0001", "external_user");
        api::Message user;
        user.role = api::Role::User;
        user.content.push_back(api::TextBlock{"读一下 README 并数行数"});
        bridge->RecordInput(user);

        agent::PromptOptions prompt_options;
        prompt_options.cwd = "D:/demo";
        prompt_options.current_date = "2026-08-31";
        const agent::ResolvedPromptBase base = agent::BuildResolvedPromptBase(prompt_options);
        agent::AgentProfile profile;
        profile.request.model = "test-model";
        profile.system_prompt = base.text;
        profile.resolved_prompt_base = base;
        profile.purpose = accounting::RequestPurpose::MainTurn;

        TurnBackend backend;
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<FakeTool>());
        agent::Agent loop(backend, registry, profile);
        agent::TurnWiring wiring;
        wiring.boundary_recorder = bridge.get();
        const auto outcome = loop.Run("读一下 README 并数行数", wiring);
        if (!outcome.has_value() || backend.calls != 2) {
            return Fail("假后端回合没跑完两笔请求");
        }
        bridge->EndTurn(true, false, "done");
    }
    {  // 旁路桥手记一笔 compact_reduce(500 in/40 out)。
        runtime::TrajectoryTurnBridge::Identity identity{"demo", "responses", "host"};
        auto bypass = ledger->NewBypassBridge(identity);
        api::Request request;
        request.model = "test-model";
        api::Message material;
        material.role = api::Role::User;
        material.content.push_back(api::TextBlock{"压缩这份历史"});
        request.messages.push_back(material);
        agent::RequestPreparedContext ctx;
        ctx.purpose = accounting::RequestPurpose::CompactReduce;
        const std::string request_id = bypass->OnRequestPrepared(request, ctx);
        if (request_id.empty()) {
            return Fail("旁路桥 prepared 落不住");
        }
        bypass->OnRequestSent(request_id);
        api::Usage usage;
        usage.input_tokens = 500;
        usage.output_tokens = 40;
        bypass->OnUsageRecorded(request_id, usage, /*reported_by_provider=*/true, "resp-usage-3");
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"压缩完成"});
        if (!bypass->OnOutputCompleted(request_id, assistant, "end_turn", "resp-usage-3")) {
            return Fail("旁路桥收不了口");
        }
    }

    std::printf("==================== 幕一:flag 开 /usage ====================\n");
    {
        app::UsageCommandContext context{theme};
        context.trajectory = &*ledger;
        context.sessions_root = ledger->session_dir().parent_path();
        app::HandleUsageCommand("", context);
    }
    std::printf("\n-------------- /usage --by purpose --------------\n");
    {
        app::UsageCommandContext context{theme};
        context.trajectory = &*ledger;
        context.sessions_root = ledger->session_dir().parent_path();
        app::HandleUsageCommand("--by purpose", context);
    }
    std::printf("\n-------------- /usage --json(节选核对数字) --------------\n");
    {
        app::UsageCommandContext context{theme};
        context.trajectory = &*ledger;
        context.sessions_root = ledger->session_dir().parent_path();
        app::HandleUsageCommand("--json", context);
    }
    // 幕一收尾:对账数字(假后端钉死的三笔)。
    {
        const auto read = accounting::ReadSessionUsage(ledger->session_dir());
        if (!read.ok) {
            return Fail("读不回自己刚写的账: " + read.message);
        }
        if (read.samples.size() != 3) {
            return Fail("应有 3 笔 sample(main×2 + compact×1),实得 " +
                        std::to_string(read.samples.size()));
        }
        const auto aggregate = accounting::AggregateUsage(read.samples);
        const std::int64_t expected_input = (900 + 1200) + 1500 + 500;
        const std::int64_t expected_output = 80 + 30 + 40;
        if (aggregate.totals.total_input_tokens != expected_input) {
            return Fail("合计输入对不上:期望 " + std::to_string(expected_input) + ",实得 " +
                        std::to_string(aggregate.totals.total_input_tokens));
        }
        if (aggregate.totals.output_tokens != expected_output) {
            return Fail("合计输出对不上:期望 " + std::to_string(expected_output) + ",实得 " +
                        std::to_string(aggregate.totals.output_tokens));
        }
        if (aggregate.totals.requests_with_usage != 3 ||
            aggregate.totals.requests_unknown != 0) {
            return Fail("coverage 对不上:3 笔全报才对");
        }
        std::printf("[幕一对账] 3 笔全报 · 合计输入 %lld · 合计输出 %lld —— 与假后端钉死的数字一致\n",
                    static_cast<long long>(expected_input), static_cast<long long>(expected_output));
    }
    const auto close = ledger->CloseSession("smoke_done");
    if (!close.error_code.empty()) {
        return Fail("封口失败: " + close.error_code);
    }

    // ---- 幕二:flag 关,降级内存粗账 ----
    std::printf("\n==================== 幕二:flag 关 /usage(降级) ====================\n");
    {
        agent::ModelUsageLedger memory;
        api::Usage normal_usage;
        normal_usage.input_tokens = 3000;
        normal_usage.output_tokens = 120;
        memory.Record(agent::ModelRole::Normal, "test-model", normal_usage, /*duration_ms=*/0,
                      /*reported=*/true);
        app::UsageCommandContext context{theme};
        context.memory_ledger = &memory;
        app::HandleUsageCommand("", context);
    }
    std::printf("\n[冒烟完] 两幕跑完,退出 0\n");
    return 0;
}
