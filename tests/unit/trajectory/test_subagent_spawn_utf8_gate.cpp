// spawn 链 UTF-8 清洗门单:模型 agent 工具参数(title/prompt)可带非法
// UTF-8(provider 流式坏字节,或模型复制了脏内容),直灌 spawn 链会在
// CanonicalJsonDump 被整场拒掉——真机实录(minimax-m3 主会话并发派 4 只,
// 坏字节参数的两只死在 subagent.run.start_failed | canonical_json.
// invalid_utf8)。这里验两道门:
//   1. 入口消毒(AgentTool::ExecuteDispatch):坏字节三形态(孤立续字节/
//      截断多字节/GBK 段)spawn 成功、run.started 合法落账、warning 计数对;
//      干净参数零 warning、逐字节不变。
//   2. 账前兜底(TrajectorySessionLedger::SpawnSubagent):绕过工具入口直灌
//      坏字节 task_label,run.started 照样合法;幂等——洗过的串再过零改动。
// 全部走真 ledger + 临时目录 + LogSink 截获,断言落在盘上文件与警告账,
// 与 test_subagent_trajectory_integration 的接线同构。0.26.177 的 fail
// closed 册(注入故障拒写)在 test_subagent_spawn_integrity 里,不在这里
// 重做——防线不动,只是好数据不再被误杀。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "platform/log_sink.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8:断言合同,不赌 ACP 转出的具体字形
#include "runtime/id_authority.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "trajectory/journal.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

constexpr const char* kFffd = "\xEF\xBF\xBD";  // U+FFFD 替换字符

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 按脚本吐事件的假后端(与 test_subagent_trajectory_integration 同一套写法)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
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

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {api::MessageStart{"msg", "model"}, api::TextDelta{text}, api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{}}};
}

std::vector<std::string> KindsOf(const std::filesystem::path& stream) {
    std::vector<std::string> kinds;
    const auto lines = trajectory::ReadJournalLines(stream);
    if (!lines.has_value()) {
        return kinds;
    }
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        kinds.push_back(parsed.is_discarded() ? std::string("<bad>")
                                              : parsed.value("kind", std::string()));
    }
    return kinds;
}

std::vector<std::string> SubagentJsonlFiles(const TrajectorySessionLedger& ledger) {
    std::vector<std::string> names;
    std::error_code ec;
    const auto dir = ledger.session_dir() / "subagents";
    if (!std::filesystem::exists(dir, ec)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            names.push_back(entry.path().filename().generic_string());
        }
    }
    return names;
}

std::optional<TrajectorySessionLedger> OpenLedger(
    const std::filesystem::path& root, std::function<std::optional<std::string>()> fault = {}) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    options.subagent_start_fault = std::move(fault);
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(std::move(options));
    if (!ledger.has_value()) {
        return std::nullopt;
    }
    return std::move(*ledger);
}

// LogSink 截获(进程级单例,doctest 串行跑,各测试自挂自还原):warning
// 计数是验收项——"模型吐了脏东西"不许静默。
struct LogCapture {
    std::vector<platform::LogRecord> records;

    LogCapture() {
        platform::LogSink::Instance().SetWriter([this](const platform::LogRecord& record) {
            records.push_back(record);
        });
    }
    ~LogCapture() { platform::LogSink::Instance().SetWriter(nullptr); }

    std::size_t Count(platform::LogLevel level, const std::string& component) const {
        std::size_t count = 0;
        for (const auto& record : records) {
            if (record.level == level && record.component == component) {
                ++count;
            }
        }
        return count;
    }

    std::size_t Total() const { return records.size(); }

    bool HasWarnContaining(const std::string& component, const std::string& needle) const {
        for (const auto& record : records) {
            if (record.level == platform::LogLevel::Warn && record.component == component &&
                record.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

agent::ToolTraceEvent AgentCallEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "item-agent-1";
    event.tool_use_id = call_id;
    event.tool_name = "agent";
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.timestamp_ms = 1759000000000LL;
    if (kind == agent::ToolTraceEventKind::ExecutionStarted) {
        event.effective_input_sha256 = std::string(64, '0');
        event.effect_class = agent::EffectClass::InProcessUnknown;
        event.effective_arguments = nlohmann::json{{"prompt", "把仓库数一遍"}};
    } else if (kind == agent::ToolTraceEventKind::ExecutionFinished) {
        event.outcome = agent::ToolOutcome::Succeeded;
        event.duration_ms = 50;
        event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
        event.result_ref.sha256 = std::string(64, '2');
        event.result_ref.bytes = 20;
    }
    return event;
}

// 装配一体(与整合册同构):ledger + main bridge + hub + agent 工具;Execute
// 可带任意 title/prompt——本册就是要灌坏字节进去。
struct Wiring {
    IdAuthority ids;
    std::unique_ptr<TrajectorySessionLedger> ledger;
    std::unique_ptr<TrajectoryTurnBridge> main_bridge;
    ToolTraceHub hub{ids};
    std::string parent_request_id;

    explicit Wiring(const char* tag, api::Backend& backend, tools::ToolRegistry& sub_registry) {
        auto opened = OpenLedger(FreshDir(tag));
        REQUIRE(opened.has_value());
        ledger = std::make_unique<TrajectorySessionLedger>(std::move(*opened));
        main_bridge = ledger->NewTurnBridge({"demo", "responses", "terminal"});
        REQUIRE(main_bridge != nullptr);
        hub.AttachTrajectory(main_bridge.get());

        tools::AgentTool::Hooks hooks;
        hooks.on_tool_trace = [this](const agent::ToolTraceEvent& event) { hub.OnTrace(event); };
        hooks.trajectory_spawn = [this](const std::string& task_label, const std::string& parent_run_id,
                                        SubagentSpawnFailure* failure_out) {
            const std::string parent_call_id = hub.current_agent_call_id();
            auto child = ledger->SpawnSubagent(parent_call_id, task_label, parent_run_id);
            if (!child.has_value()) {
                ledger->NoteSubagentStartFailed(child.error(), parent_run_id, parent_call_id,
                                                main_bridge->current_turn_id());
                if (failure_out != nullptr) {
                    *failure_out = child.error();
                }
                return std::unique_ptr<TrajectorySubagentBridge>();
            }
            if (parent_run_id.empty() && !parent_call_id.empty()) {
                main_bridge->AttachChildRun(parent_call_id, (*child)->run_id());
            }
            return std::move(*child);
        };
        hooks.trajectory_child_finished = [this](const std::string& run_id, const std::string& hash) {
            main_bridge->NoteChildTerminal(run_id, hash);
        };
        tool = std::make_unique<tools::AgentTool>(backend, sub_registry, "/work/dir");
        tool->SetHooks(std::move(hooks));

        main_bridge->BeginTurn("turn-1", "external_user");
        api::Message input;
        input.role = api::Role::User;
        input.content.push_back(api::TextBlock{"派一只子代理去数仓库"});
        main_bridge->RecordInput(input);
        parent_request_id = main_bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        REQUIRE_FALSE(parent_request_id.empty());
        main_bridge->OnRequestSent(parent_request_id);
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"这就去。"});
        api::ToolUseBlock call;
        call.id = "toolu-parent";
        call.name = "agent";
        call.input = nlohmann::json{{"prompt", "把仓库数一遍"}};
        assistant.content.push_back(std::move(call));
        REQUIRE(main_bridge->OnOutputCompleted(parent_request_id, assistant, "tool_use", "resp-p"));
        hub.OnTrace(AgentCallEvent(agent::ToolTraceEventKind::Scheduled, "toolu-parent"));
        hub.OnTrace(AgentCallEvent(agent::ToolTraceEventKind::ExecutionStarted, "toolu-parent"));
    }

    tools::Tool::Result Execute(const std::string& title, const std::string& prompt) {
        return tool->execute(nlohmann::json{{"title", title}, {"prompt", prompt}});
    }

    void FinishParentTurn(bool ok, bool result_is_error, const std::string& result_text) {
        agent::ToolTraceEvent finished =
            AgentCallEvent(agent::ToolTraceEventKind::ExecutionFinished, "toolu-parent");
        if (result_is_error) {
            finished.outcome = agent::ToolOutcome::ToolError;
            finished.error_code = "trajectory.subagent_start_failed";
        }
        hub.OnTrace(finished);
        api::Message results;
        results.role = api::Role::User;
        results.content.push_back(api::ToolResultBlock{"toolu-parent", result_text, result_is_error});
        main_bridge->OnToolResultsCommitted("batch-1", results);
        main_bridge->EndTurn(ok, false, ok ? "done" : "failed");
    }

    // 子账 run.started 的 task_ref 原文(不存在返回空)。
    std::string ChildRunStartedTaskRef() const {
        const auto files = SubagentJsonlFiles(*ledger);
        if (files.size() != 1) {
            return std::string();
        }
        const auto lines =
            trajectory::ReadJournalLines(ledger->session_dir() / "subagents" / files[0]);
        if (!lines.has_value() || lines->empty()) {
            return std::string();
        }
        for (const std::string& line : *lines) {
            const auto parsed = nlohmann::json::parse(line, nullptr, false);
            if (parsed.is_discarded() || parsed.value("kind", std::string()) != "run.started") {
                continue;
            }
            return parsed["payload"].value("task_ref", std::string());
        }
        return std::string();
    }

    std::unique_ptr<tools::AgentTool> tool;

    Wiring(const Wiring&) = delete;
    Wiring& operator=(const Wiring&) = delete;
};

// 子代理第一份请求里第一枚 user 文本块(验收"逐字节不变"的观察口)。
std::string FirstUserTextOf(const api::Request& request) {
    std::string text;
    for (const auto& message : request.messages) {
        if (message.role != api::Role::User) {
            continue;
        }
        for (const auto& block : message.content) {
            if (const auto* piece = std::get_if<api::TextBlock>(&block)) {
                text += piece->text;
            }
        }
        break;
    }
    return text;
}

// 一只子代理的完整成功路:spawn 过、子账合法、task_ref 是给定值。返回
// 子账 stream 文件名(失败返回空)。
std::string RequireHealthyChild(const Wiring& w, const char* tag) {
    const auto files = SubagentJsonlFiles(*w.ledger);
    REQUIRE(files.size() == 1);
    const auto path = w.ledger->session_dir() / "subagents" / files[0];
    CAPTURE(tag);
    CHECK(trajectory::VerifyJournalFile(path).ok);
    const auto kinds = KindsOf(path);
    REQUIRE_FALSE(kinds.empty());
    CHECK(kinds.front() == "run.started");
    CHECK(kinds.back() == "run.completed");
    const auto report = w.ledger->VerifySession();
    CHECK(report.error_code.empty());
    return files[0];
}

}  // namespace

// ---------------------------------------------------------------------------
// 复现景:与现场同形状(minimax 式坏字节 title + prompt)
// ---------------------------------------------------------------------------

TEST_CASE("复现景:坏字节 title/prompt——子代理正常起跑,不再死在 run.started") {
    LogCapture logs;
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("子代理结论:仓库一共 41 个文件")};
    tools::ToolRegistry sub_registry;
    Wiring w("lubancode-traj-utf8-repro", backend, sub_registry);

    // 现场形状:P0 键贴场景一只,title 与 prompt 各带一枚孤立续字节 0x80
    //(provider 流式坏字节/模型复制脏内容)。修前:task_label 带坏字节,
    // run.started 被 canonical_json.invalid_utf8 整场拒掉,fail closed。
    std::string title = "P0 ";
    title += static_cast<char>(0x80);
    title += "键贴场景";
    std::string prompt = "把 D 盘仓库数一遍,重点看";
    prompt += static_cast<char>(0x80);
    prompt += "配置文件";

    const tools::Tool::Result result = w.Execute(title, prompt);
    REQUIRE_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);  // 子代理真的上路了
    w.FinishParentTurn(/*ok=*/true, /*result_is_error=*/false, result.content);

    const std::string task_ref = w.ChildRunStartedTaskRef();
    REQUIRE_FALSE(task_ref.empty());
    CHECK(platform::IsValidUtf8(task_ref));
    // 清洗合同:坏字节换 U+FFFD,合法片段(配置文件)原样保留。
    CHECK(task_ref.find(kFffd) != std::string::npos);
    CHECK(task_ref.find("配置文件") != std::string::npos);
    RequireHealthyChild(w, "repro");

    // warning 计数对:入口两枚字符串参数各一行,处数如实;账前兜底幂等,
    // 不再重复告警(零 trajectory 侧 warning)。
    REQUIRE(logs.Count(platform::LogLevel::Warn, "agent_tool") == 2);
    CHECK(logs.HasWarnContaining("agent_tool", "title"));
    CHECK(logs.HasWarnContaining("agent_tool", "prompt"));
    CHECK(logs.HasWarnContaining("agent_tool", "1 处非法 UTF-8"));
    CHECK(logs.Count(platform::LogLevel::Warn, "trajectory") == 0);

    // 子代理吃到的任务书也是洗过的:坏字节不再往下游递。
    const std::string user_text = FirstUserTextOf(backend.captured_requests[0]);
    CHECK(platform::IsValidUtf8(user_text));
    CHECK(user_text.find(kFffd) != std::string::npos);
    CHECK(user_text.find("配置文件") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 入口消毒:坏字节三形态
// ---------------------------------------------------------------------------

TEST_CASE("入口消毒:坏字节三形态——spawn 全成,run.started 合法落账") {
    // 三种形态:孤立续字节 0x80(流式劈叉)、截断多字节(半截"你")、
    // GBK 整段(键贴场景的 GBK 编码)。title 带毒,prompt 干净——task_ref
    // 的字节因此全确定(只随清洗合同变)。
    struct Form {
        const char* tag;
        std::string title;
    };
    std::vector<Form> forms;
    {
        std::string t = "P0 ";
        t += static_cast<char>(0x80);
        t += "修复";
        forms.push_back({"isolated", t});
    }
    {
        std::string t = "修复登录";
        t += static_cast<char>(0xE4);  // 三字节序列只到一半(你 = E4 BD A0)
        t += static_cast<char>(0xBD);
        forms.push_back({"truncated", t});
    }
    {
        std::string t;  // 键贴场景的 GBK 字节段
        for (const unsigned char byte :
             {0xBC, 0xFC, 0xCC, 0xF9, 0xB3, 0xA1, 0xBE, 0xB0}) {
            t += static_cast<char>(byte);
        }
        forms.push_back({"gbk", t});
    }

    for (const Form& form : forms) {
        CAPTURE(form.tag);
        LogCapture logs;
        FakeBackend backend;
        backend.scripts = {TextOnlyScript("办完了")};
        tools::ToolRegistry sub_registry;
        Wiring w(std::string("lubancode-traj-utf8-").append(form.tag).c_str(), backend, sub_registry);

        const tools::Tool::Result result = w.Execute(form.title, "把仓库数一遍");
        CHECK_FALSE(result.is_error);
        w.FinishParentTurn(/*ok=*/true, /*result_is_error=*/false, result.content);

        const std::string task_ref = w.ChildRunStartedTaskRef();
        REQUIRE_FALSE(task_ref.empty());
        CHECK(platform::IsValidUtf8(task_ref));
        // prompt 干净:task_ref 前缀逐字节保留。
        CHECK(task_ref.rfind("general-purpose: 把仓库数一遍", 0) == 0);
        RequireHealthyChild(w, form.tag);

        // title 那一行 warning 如实落下(处数随形态:孤立 1、截断 1、GBK 8);
        // 账前兜底对洗过的 task_ref 幂等,零重复告警。
        REQUIRE(logs.Count(platform::LogLevel::Warn, "agent_tool") == 1);
        CHECK(logs.HasWarnContaining("agent_tool", "title"));
        CHECK(logs.Count(platform::LogLevel::Warn, "trajectory") == 0);
    }
}

TEST_CASE("入口消毒:干净参数——零 warning,逐字节不变") {
    LogCapture logs;
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("子代理结论:仓库一共 41 个文件")};
    tools::ToolRegistry sub_registry;
    Wiring w("lubancode-traj-utf8-clean", backend, sub_registry);

    const std::string title = "数仓库";
    const std::string prompt = "把 D:/repo 数一遍,报告文件数";
    const tools::Tool::Result result = w.Execute(title, prompt);
    REQUIRE_FALSE(result.is_error);
    w.FinishParentTurn(/*ok=*/true, /*result_is_error=*/false, result.content);

    RequireHealthyChild(w, "clean");
    // 零告警:干净参数过门零成本,两道门都不出声。
    CHECK(logs.Total() == 0);
    // 逐字节不变:task_ref 与子代理任务书原样到字节。
    CHECK(w.ChildRunStartedTaskRef() == "general-purpose: " + prompt);
    const std::string user_text = FirstUserTextOf(backend.captured_requests[0]);
    CHECK(user_text.find(prompt) != std::string::npos);
    CHECK(user_text.find(kFffd) == std::string::npos);
}

// ---------------------------------------------------------------------------
// 账前兜底:绕过工具入口直灌 SpawnSubagent
// ---------------------------------------------------------------------------

TEST_CASE("账前兜底:坏字节 task_label 直灌 SpawnSubagent——run.started 照样合法") {
    LogCapture logs;
    const auto root = FreshDir("lubancode-traj-utf8-fallback");
    auto ledger = OpenLedger(root);
    REQUIRE(ledger.has_value());

    std::string label = "读文件";
    label += static_cast<char>(0x80);
    label += "并数行数";
    const auto child = ledger->SpawnSubagent("toolu-1", label);
    REQUIRE(child.has_value());  // 修前:canonical_json.invalid_utf8 整场拒掉
    const auto path = ledger->session_dir() / "subagents" / ((*child)->run_id() + ".jsonl");
    REQUIRE(std::filesystem::exists(path));
    CHECK(trajectory::VerifyJournalFile(path).ok);

    // 落账的 task_ref 是洗过的合法 UTF-8:坏字节换 U+FFFD,合法片段保留。
    const auto lines = trajectory::ReadJournalLines(path);
    REQUIRE(lines.has_value());
    bool saw_started = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "run.started") {
            continue;
        }
        saw_started = true;
        const std::string task_ref = parsed["payload"].value("task_ref", std::string());
        CHECK(platform::IsValidUtf8(task_ref));
        CHECK(task_ref.find(kFffd) != std::string::npos);
        CHECK(task_ref.find("并数行数") != std::string::npos);
    }
    CHECK(saw_started);

    // 兜底出手不静默:落一行 warning,处数如实。
    REQUIRE(logs.Count(platform::LogLevel::Warn, "trajectory") == 1);
    CHECK(logs.HasWarnContaining("trajectory", "1 处非法 UTF-8"));
}

TEST_CASE("账前兜底:幂等——洗过的串再过一遍,零改动零告警") {
    LogCapture logs;
    const auto root = FreshDir("lubancode-traj-utf8-idempotent");
    auto ledger = OpenLedger(root);
    REQUIRE(ledger.has_value());

    // 入口洗过的形状(合法 UTF-8):兜底原样放行,task_ref 逐字节不变。
    const std::string clean = "读文件并数行数";
    const auto child = ledger->SpawnSubagent("toolu-1", clean);
    REQUIRE(child.has_value());
    const auto path = ledger->session_dir() / "subagents" / ((*child)->run_id() + ".jsonl");
    const auto lines = trajectory::ReadJournalLines(path);
    REQUIRE(lines.has_value());
    bool saw_started = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "run.started") {
            continue;
        }
        saw_started = true;
        CHECK(parsed["payload"].value("task_ref", std::string()) == clean);
    }
    CHECK(saw_started);
    CHECK(logs.Total() == 0);  // 干净串零告警,兜底零成本
}
