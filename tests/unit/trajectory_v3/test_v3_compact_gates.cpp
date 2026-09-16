// T12-B/C/D/E(V3-GAP-07,SessionV3 旧设计清理单)compact 域门的接线册:
//   - C 回合关联:TrajectorySessionLedger::OpenMainTurnId 的活动主轮簿
//     (BeginTurn/EndTurn 之间才非空,idle 不伪造 parent);
//   - E hard trim 收口:AfterHardTrim 的 v3 路把损失落到 32/16/8/4 KiB
//     派生预览与 context.tool_previews.reduced 原子提交(原 artifact 不动、
//     调用/结果不拆散),最低档之后降档梯如实收场,重读结果全量文件也被
//     当前档位把住(不绕过最低档失败门槛)。
// 干跑(B)与竞争(C)的运行时面在 test_v3_compact_runtime.cpp;loop 的
// SendOverflow 相在 tests/unit/agent/test_loop.cpp;goal 泵的溢出门在
// tests/unit/app/test_goal_v3_commands.cpp。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "app/commands/session_commands.hpp"
#include "cli/theme.hpp"
#include "hooks/hash.hpp"
#include "runtime/trajectory_session.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

// 环境变量门卫:构造置值,析构还原(不漏开关状态给别的册)。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-compact-gates-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.238-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

std::vector<nlohmann::json> ReadLines(const std::filesystem::path& stream) {
    std::vector<nlohmann::json> rows;
    std::ifstream file(stream, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

// ---- 桥口同形的回合材料(与 test_v3_tool_message_preview 同款) ------------

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithRunCommandCall(const std::string& call_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"我递归列一下目录。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = "run_command";
    call.input = nlohmann::json{{"command", "rg --files"}};
    message.content.push_back(std::move(call));
    return message;
}

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

api::Usage SampleUsage() {
    api::Usage usage;
    usage.input_tokens = 1180;
    usage.output_tokens = 24;
    return usage;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.tool_use_id = call_id;
    event.tool_name = "run_command";
    event.execution_id = "exec-" + call_id;
    event.effective_input_sha256 = "aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111";
    event.effective_arguments = nlohmann::json{{"command", "rg --files"}};
    return event;
}

// ReplaceHistory 的落点:一只最小假后端 + Agent。
class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&, const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* = nullptr) override {
        return std::unexpected(api::Error{api::ErrorKind::Api, "stub", 0});
    }
};

// AfterHardTrim 压力相的最小材料包(梯子驱动只用 trajectory + agent)。
lubancode::app::CompactSessionInputs MakePressureInputs(cli::Theme& theme,
                                                        TrajectorySessionLedger& ledger,
                                                        agent::Agent& loop) {
    lubancode::app::CompactSessionInputs in;
    in.theme = &theme;
    in.trajectory = &ledger;
    in.agent = &loop;
    return in;
}

std::string FileSha(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return hooks::Sha256Hex(text);
}

}  // namespace

// ---------------------------------------------------------------------------
// T12-C:活动主轮簿——BeginTurn 后 OpenMainTurnId 给真号,EndTurn 后
// nullopt(粘账分得清在跑与收口);v2 场 nullopt。idle 手动压缩的
// parentTurnId 按"无活动主轮"落 null(接线层只认这只簿,不再从链上猜)。
// ---------------------------------------------------------------------------
TEST_CASE("T12-C OpenMainTurnId:在跑给真号,收口给 nullopt,v2 给 nullopt") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-main-turn");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());

    CHECK_FALSE(ledger->OpenMainTurnId().has_value());  // 没开过轮
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-open-1", "external_user");
        const auto open = ledger->OpenMainTurnId();
        REQUIRE(open.has_value());
        CHECK(*open == "turn-open-1");
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    // 收口后:粘账(active_main_turn_id)还在,但"在跑"为假——idle 压缩
    // 不许拿它伪造 parent。
    CHECK_FALSE(ledger->OpenMainTurnId().has_value());
    CHECK(ledger->CloseSession("exit").error_code.empty());
}

// ---------------------------------------------------------------------------
// T12-E:AfterHardTrim 的 v3 收口——32/16/8/4 KiB 派生预览 + context
// 提交;原 artifact 不动;调不出原档的收场如实报错;梯尽(已在 4 KiB)
// 如实收场;重读全量文件按当前档把住。
// ---------------------------------------------------------------------------
TEST_CASE("T12-E hard trim 收口:降档提交换链,原 artifact 不动,梯尽如实") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("hard-trim");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream =
        ledger->session_dir() / platform::Utf8ToPath(platform::PathToUtf8(
                                                    ledger->session_dir().filename()) + ".jsonl");
    auto* writer = ledger->v3_main_writer();
    REQUIRE(writer != nullptr);

    // 一轮巨肥工具结果:链上 tool 消息吃 32 KiB 预览,原文归 artifacts。
    const std::string fat = "HEAD-recursive-listing-begin\n" + std::string(300 * 1024, 'x') +
                            "\nTAIL-recursive-listing-end";
    std::string tool_message_id;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("列出全部文件"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-PREVIEW", {UserMessage("列出全部文件")}), PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        bridge->OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true, "resp-1",
                               0, true, false);
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantWithRunCommandCall("call_fat"),
                                          "tool_calls", "resp-1"));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "call_fat"));
        agent::ToolTraceEvent started =
            TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "call_fat");
        started.outcome = agent::ToolOutcome::Succeeded;
        bridge->OnToolTrace(started);
        agent::ToolTraceEvent finished =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call_fat");
        finished.outcome = agent::ToolOutcome::Succeeded;
        finished.duration_ms = 42;
        finished.details = nlohmann::json{{"exit_code", 0}};
        bridge->OnToolTrace(finished);
        api::Message results;
        results.role = api::Role::User;
        api::ToolResultBlock result;
        result.tool_use_id = "call_fat";
        result.content = fat;
        results.content.push_back(result);
        const auto receipt = bridge->RewriteToolResultsForHistory(results);
        REQUIRE(receipt.status == lubancode::runtime::ToolResultsCommitReceipt::Status::Committed);
        bridge->OnToolResultsCommitted("batch-1", results);
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
        tool_message_id = writer->context().chain.back().message_ref;
    }
    REQUIRE(writer->context().preview_budget_bytes == 32768);

    // 原 artifact 的指纹(降档前后必须逐字节不动)。
    std::string artifact_sha;
    {
        auto opened = trajectory::v3::ReadV3Ledger(stream);
        REQUIRE(opened.has_value());
        const auto projection = trajectory::v3::ExpandResultPreview(
            *opened, ledger->session_dir(), tool_message_id);
        for (const auto& ref : projection.result_refs) {
            if (ref.value("kind", std::string()) != "result_metadata") {
                artifact_sha = FileSha(ledger->session_dir() /
                                       tools::Utf8ToPath(ref.value("path", std::string())));
            }
        }
        REQUIRE_FALSE(artifact_sha.empty());
    }

    StubBackend backend;
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "sys"});
    cli::Theme theme;
    lubancode::app::CompactSessionInputs in = MakePressureInputs(theme, *ledger, loop);

    const auto trim_pressure = [&]() {
        agent::ContextPressure pressure;
        pressure.phase = agent::ContextPressure::Phase::AfterHardTrim;
        pressure.hard_truncated_results = true;
        pressure.window_tokens = 32768;
        lubancode::app::HandleContextPressure(pressure, in);
    };

    // 第一刀:32 → 16 KiB,派生预览换链,context.tool_previews.reduced 落账。
    trim_pressure();
    CHECK(writer->context().preview_budget_bytes == 16384);
    {
        auto rows = ReadLines(stream);
        int reduced = 0;
        for (const auto& row : rows) {
            if (row.value("kind", "") == "context.tool_previews.reduced") {
                ++reduced;
                CHECK(row["payload"]["oldPreviewBudget"] == 32768);
                CHECK(row["payload"]["newPreviewBudget"] == 16384);
                CHECK(row["payload"]["replacementRefs"].size() == 1);
            }
            if (row.value("messageId", "") ==
                writer->context().chain.back().message_ref) {
                // 派生消息:同 turn/step/action,sourceToolMessageRef 指原消息。
                CHECK(row.value("origin", "") == "context_runtime");
                CHECK(row.value("sourceToolMessageRef", "") == tool_message_id);
                CHECK(row["message"].value("role", "") == "tool");
            }
        }
        CHECK(reduced == 1);
    }
    // 链上换的是派生消息(原消息仍在档);投影(= ReplaceHistory 换进 loop
    // 的那份)的 tool 正文 ≤ 16 KiB。
    CHECK(writer->context().chain.back().message_ref != tool_message_id);
    {
        const auto projected = ledger->ProjectV3ContextHistory();
        REQUIRE(projected.has_value());
        bool found_short = false;
        for (const auto& message : *projected) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::ToolResultBlock>(&block)) {
                    found_short = true;
                    CHECK(text->content.size() <= 16384);
                }
            }
        }
        CHECK(found_short);
    }
    // 原 artifact 逐字节未动。
    {
        auto reopened = trajectory::v3::ReadV3Ledger(stream);
        REQUIRE(reopened.has_value());
        const auto projection = trajectory::v3::ExpandResultPreview(
            *reopened, ledger->session_dir(), tool_message_id);
        for (const auto& ref : projection.result_refs) {
            if (ref.value("kind", std::string()) != "result_metadata") {
                CHECK(FileSha(ledger->session_dir() /
                              tools::Utf8ToPath(ref.value("path", std::string()))) == artifact_sha);
            }
        }
    }

    // 第二、三刀:16 → 8 → 4 KiB。
    trim_pressure();
    CHECK(writer->context().preview_budget_bytes == 8192);
    trim_pressure();
    CHECK(writer->context().preview_budget_bytes == 4096);
    // 第四刀:已在最低档——降档梯如实收场,不再提交,不再降。
    trim_pressure();
    CHECK(writer->context().preview_budget_bytes == 4096);
    {
        auto rows = ReadLines(stream);
        int reduced = 0;
        for (const auto& row : rows) {
            if (row.value("kind", "") == "context.tool_previews.reduced") {
                ++reduced;
            }
        }
        CHECK(reduced == 3);
    }

    // 重读结果全量文件不能绕过当前(最低)档:新一轮巨肥结果的 tool 消息
    // 按当前 4 KiB 档出预览,原文不进上下文。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-2", "external_user");
        bridge->RecordInput(UserMessage("再读一遍全量"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-PREVIEW", {UserMessage("再读一遍全量")}), PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        bridge->OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true, "resp-2",
                               0, true, false);
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantWithRunCommandCall("call_reread"),
                                          "tool_calls", "resp-2"));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "call_reread"));
        agent::ToolTraceEvent started =
            TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "call_reread");
        started.outcome = agent::ToolOutcome::Succeeded;
        bridge->OnToolTrace(started);
        agent::ToolTraceEvent finished =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call_reread");
        finished.outcome = agent::ToolOutcome::Succeeded;
        finished.duration_ms = 7;
        finished.details = nlohmann::json{{"exit_code", 0}};
        bridge->OnToolTrace(finished);
        api::Message results;
        results.role = api::Role::User;
        api::ToolResultBlock result;
        result.tool_use_id = "call_reread";
        result.content = fat;
        results.content.push_back(result);
        const auto receipt = bridge->RewriteToolResultsForHistory(results);
        REQUIRE(receipt.status == lubancode::runtime::ToolResultsCommitReceipt::Status::Committed);
        bridge->OnToolResultsCommitted("batch-2", results);
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    {
        auto reloaded = trajectory::v3::ReadV3Ledger(stream);
        REQUIRE(reloaded.has_value());
        const auto* reread_line = reloaded->FindMessage(writer->context().chain.back().message_ref);
        REQUIRE(reread_line != nullptr);
        REQUIRE(reread_line->message.value("role", std::string()) == "tool");
        const auto content_it = reread_line->message.find("content");
        REQUIRE(content_it != reread_line->message.end());
        CHECK(content_it->get_ref<const std::string&>().size() <= 4096);
        // 中段原文不进上下文:头尾标记在,填充长串不在。
        const std::string& text = content_it->get_ref<const std::string&>();
        CHECK(text.find("HEAD-recursive-listing-begin") != std::string::npos);
        CHECK(text.find("TAIL-recursive-listing-end") != std::string::npos);
        CHECK(text.find(std::string(4096, 'x')) == std::string::npos);
    }
    CHECK(trajectory::v3::VerifyV3File(stream).ok);
    CHECK(ledger->CloseSession("exit").error_code.empty());
}

// ---------------------------------------------------------------------------
// T12-E 的最低档失败门槛:预览生成器在极小档上装不下必要来源时如实报
// preview_unrepresentable,不静默放行(§4.38/§4.17)。
// ---------------------------------------------------------------------------
TEST_CASE("T12-E 最低档失败门槛:装不下必要来源明报,不硬塞") {
    trajectory::v3::PreviewRequest request;
    request.max_preview_bytes = 120;  // 连说明区都装不下的档
    trajectory::v3::PreviewChannel channel;
    channel.channel = "combined";
    channel.display_path = "artifacts/res-000001.combined.txt";
    channel.text = std::string(4096, 'z');
    channel.output_bytes = 4096;
    request.channels.push_back(std::move(channel));
    const auto preview = trajectory::v3::BuildToolPreview(request);
    CHECK(preview.preview_unrepresentable);
    CHECK(preview.text.size() <= 120);
}
