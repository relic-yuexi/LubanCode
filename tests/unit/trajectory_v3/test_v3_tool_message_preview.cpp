// V3-REAL-05(真实会话审计单):工具结果已存仓,不得再把巨量全文当模型
// 预览。现场病理:run_command 递归列目录 2MB 正文原样进了 tool 消息(seq
// 331 / 2,097,251 字节),结果仓、模型预览与临时裁剪各走一段。修后合同:
// 原文按 artifact 不可变归仓;tool 消息的 content 是模型可见的最终预览
// 版本——汇总(combined)后总字节超当前档(默认 32 KiB)才走 §4.17 预览
// 路(说明区标签路径也占预算 + 头尾节选,如实交代 truncated),线内原样;
// run_command 一类副作用工具同样受帽。夹具脱敏缩小(300 KB / 150 KB,
// 保持“超帽巨肥”故障结构),不复制用户会话原档。
//
// 两条路都钉:
//   - 全链(hub 接 rewrite_tool_results_for_history):loop 在消息入史前
//     调 RewriteToolResultsForHistory——原文归仓+预览写回 content,模型
//     实发的运行时历史与 v3 tool 消息吃同一份预览(账实一致);
//   - 回退(没接钩子的 wiring,如子代理直连桥):批次尾 OnToolResultsCommitted
//     现场归仓+预览,tool 消息仍受帽。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // RequestPreparedContext(桥口同形,只引不改)
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "api/gemini/request.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
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
                     ("lubancode-v3-tool-preview-" + std::string(tag));
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

api::Message ToolResultMessage(const std::string& call_id, const std::string& content) {
    api::Message message;
    message.role = api::Role::User;
    api::ToolResultBlock result;
    result.tool_use_id = call_id;
    result.content = content;
    message.content.push_back(std::move(result));
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

// 一轮“输入 → 请求/回复(声明 run_command)→ 执行 → 巨肥结果回喂”的
// 完整流,与生产桥口同形。use_history_hook = 走全链(loop 入史前先调
// RewriteToolResultsForHistory,再批次尾回调——hub 挂的正是这个次序);
// false = 回退路(没接钩子的 wiring,批次尾现场归仓)。
// 返回值:rewrite 钩子写回的 content(运行时历史吃的那份);回退路返回
// 空串(没走钩子)。
std::string DriveFatToolTurn(TrajectoryTurnBridge& bridge, const std::string& system,
                             const std::string& call_id, const std::string& result_content,
                             bool use_history_hook = false, bool capture_complete = true,
                             std::size_t preview_budget = 32768, bool structured = false) {
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage("列出全部文件"));
    const std::string request_id =
        bridge.OnRequestPrepared(MakeRequest(system, {UserMessage("列出全部文件")}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    bridge.OnRequestSent(request_id);
    bridge.OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true,
                           "resp-123", 0, true, false);
    REQUIRE(bridge.OnOutputCompleted(request_id, AssistantWithRunCommandCall(call_id), "tool_calls",
                                     "resp-123"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    agent::ToolTraceEvent started = TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id);
    started.outcome = agent::ToolOutcome::Succeeded;
    bridge.OnToolTrace(started);
    agent::ToolTraceEvent finished = TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
    finished.outcome = agent::ToolOutcome::Succeeded;
    finished.duration_ms = 42;
    finished.details = nlohmann::json{{"exit_code", 0}};
    bridge.OnToolTrace(finished);
    api::Message results = ToolResultMessage(call_id, result_content);
    auto& result = std::get<api::ToolResultBlock>(results.content[0]);
    result.capture_complete = capture_complete;
    result.capture_reason = capture_complete ? "" : "quota";
    result.preview_budget_bytes = preview_budget;
    if (with_rich) {
        tools::EmbeddedTextResourceContent resource;
        resource.uri = "file:///source-a.txt";
        resource.mime_type = "text/plain";
        resource.text = std::string(131072, 'a');
        result.blocks.push_back(resource);
        resource.uri = "file:///source-b.txt";
        resource.text = std::string(131072, 'b');
        result.blocks.push_back(std::move(resource));
    }

    if (structured) result.structured_content = nlohmann::json{{"raw_structured", std::string(65536, 'z')}};
    std::string history_content;
    if (use_history_hook) {
        // loop 的次序(hub 挂 rewrite_tool_results_for_history):消息入史前
        // 先过预览钩子,再批次尾回调。钩子改写后的 content 就是运行时历史
        // 与 v3 tool 消息共用的那份。
        const auto receipt = bridge.RewriteToolResultsForHistory(results);
        REQUIRE(receipt.status == runtime::ToolResultsCommitReceipt::Status::Committed);
        history_content = std::get<api::ToolResultBlock>(results.content[0]).content;
        if (structured) {
            CHECK_FALSE(std::get<api::ToolResultBlock>(results.content[0]).structured_content.has_value());
            auto wire_request = MakeRequest(system, {AssistantWithRunCommandCall(call_id), results});
            const auto wire = api::gemini::BuildRequestJson(wire_request).dump();
            CHECK(wire.find("raw_structured") == std::string::npos);
            CHECK(wire.find("adopted-preview") != std::string::npos);
        }
    }
    bridge.OnToolResultsCommitted("batch-1", results);
    bridge.EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    return history_content;
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

// 从 JSONL 里取链上那条 role=tool 的消息行。
const nlohmann::json* FindToolMessage(const std::vector<nlohmann::json>& rows) {
    const nlohmann::json* found = nullptr;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) == "message" &&
            row.at("message").value("role", std::string()) == "tool") {
            found = &row;
        }
    }
    return found;
}

}  // namespace

TEST_CASE("超帽结果(全链):入史前钩子归仓换预览,运行时历史与 tool 消息同一份") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("fat");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);

    // 脱敏缩样的 2MB 递归列目录:300 KB,头尾各留可辨认标记,中间填充。
    const std::string head_mark = "HEAD-recursive-listing-begin\n";
    const std::string tail_mark = "\nTAIL-recursive-listing-end";
    std::string fat = head_mark + std::string(2 * 1024 * 1024, 'x') + tail_mark;
    const std::string history_content =
        DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_fat_01", fat, /*use_history_hook=*/true);
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());

    // 预览钩子写回运行时历史的那份:总帽 32 KiB,头尾节选,如实交代截断
    //(loop 压进双账的正是它——模型实发不再吃全文)。
    REQUIRE_FALSE(history_content.empty());
    CHECK(history_content.size() <= 32768);  // 总帽:说明标签路径头尾全算
    CHECK(history_content.size() < fat.size());
    CHECK(history_content.find("truncated: true") != std::string::npos);
    CHECK(history_content.find("capture_complete: true") != std::string::npos);
    CHECK(history_content.find("full_output: [\"artifacts/res-000001.combined.txt\"]") !=
          std::string::npos);  // 全文去处可追
    CHECK(history_content.find("[开头内容]") != std::string::npos);
    CHECK(history_content.find("[中间内容已省略]") != std::string::npos);
    CHECK(history_content.find("[结尾内容]") != std::string::npos);
    CHECK(history_content.find("HEAD-recursive-listing-begin") != std::string::npos);  // 头部节选
    CHECK(history_content.find("TAIL-recursive-listing-end") != std::string::npos);    // 尾部节选
    CHECK(platform::IsValidUtf8(history_content));  // 刀口不劈半个字

    // tool 消息与运行时历史同一份预览(账实一致:V3AdoptToolResult 一份
    // 输入,两路消费)。
    const std::filesystem::path stream = ledger->session_dir() / platform::Utf8ToPath(
        platform::PathToUtf8(ledger->session_dir().filename()) + ".jsonl");
    const auto rows = ReadLines(stream);
    const nlohmann::json* tool_message = FindToolMessage(rows);
    REQUIRE(tool_message != nullptr);
    const std::string preview_text = tool_message->at("message").at("content").get<std::string>();
    CHECK(preview_text == history_content);

    // 原文归仓:artifacts 下 combined 通道原文与不可变描述都在,一字不缺。
    const std::filesystem::path combined =
        ledger->session_dir() / "artifacts" / "res-000001.combined.txt";
    REQUIRE(std::filesystem::exists(combined));
    CHECK(std::filesystem::file_size(combined) == fat.size());
    {
        std::ifstream file(combined, std::ios::binary);
        std::string stored(std::istreambuf_iterator<char>(file), {});
        CHECK(stored == fat);
    }
    const std::filesystem::path metadata = ledger->session_dir() / "artifacts" / "res-000001.json";
    REQUIRE(std::filesystem::exists(metadata));
    std::ifstream metadata_file(metadata, std::ios::binary);
    std::string metadata_text(std::istreambuf_iterator<char>(metadata_file), {});
    const nlohmann::json description = nlohmann::json::parse(metadata_text);
    CHECK(description.at("preview_policy").at("maxPreviewBytes") == 32768);
    CHECK(description.at("preview_policy").at("budgetsLadder").size() == 4);  // 32/16/8/4
    REQUIRE(description.at("outputs").size() == 1);
    CHECK(description.at("outputs")[0].at("channel") == "combined");
    CHECK(description.at("outputs")[0].at("output_bytes") == fat.size());
    CHECK(description.at("outputs")[0].at("capture_complete") == true);

    // 账本自洽:验卷过,工具链折叠 done(预览不破配对/链);结果链次序
    // 合同(§4.18):finished → persisted → selected → tool 消息。
    const auto verify = lubancode::trajectory::v3::VerifyV3File(stream);
    CHECK(verify.ok);
    auto read = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(read.has_value());
    const auto snapshots = lubancode::trajectory::v3::FoldToolActions(*read);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].folded_status == "done");
    CHECK(snapshots[0].selected_event_ref.has_value());
    REQUIRE(snapshots[0].persisted_event_refs.size() == 1);  // 钩子归仓一次,批次尾不重复
    std::size_t finished_at = 0;
    std::size_t persisted_at = 0;
    std::size_t selected_at = 0;
    std::size_t tool_message_at = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string kind = rows[i].value("kind", std::string());
        if (kind == "tool.execution.finished") {
            finished_at = i;
        } else if (kind == "tool.result.persisted") {
            persisted_at = i;
        } else if (kind == "tool.result.selected") {
            selected_at = i;
        } else if (&rows[i] == tool_message) {
            tool_message_at = i;
        }
    }
    CHECK(finished_at < persisted_at);
    CHECK(persisted_at < selected_at);
    CHECK(selected_at < tool_message_at);
    // 链上 tool 消息的正文就是预览(读取侧按 result_ref 展开,不靠解析正文)。
    const auto context = lubancode::trajectory::v3::ProjectModelContext(*read);
    REQUIRE(context.inputs.size() == 3);
    CHECK(context.inputs[2].message.at("content").get<std::string>() == history_content);
}

TEST_CASE("线内结果(全链):钩子原样穿透,不硬套预览壳;原文照旧归仓") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("lean");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);

    const std::string lean = "src/app/main.cpp\nsrc/core/engine.cpp\n共 2 个文件。";
    const std::string history_content =
        DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_lean_01", lean, /*use_history_hook=*/true);
    (void)ledger->CloseSession("exit");

    // 钩子不改写线内结果:入史与 tool 消息都是原文(§4.17)。
    CHECK(history_content == lean);
    const std::filesystem::path stream = ledger->session_dir() / platform::Utf8ToPath(
        platform::PathToUtf8(ledger->session_dir().filename()) + ".jsonl");
    const auto rows = ReadLines(stream);
    const nlohmann::json* tool_message = FindToolMessage(rows);
    REQUIRE(tool_message != nullptr);
    const std::string content = tool_message->at("message").at("content").get<std::string>();
    CHECK(content == lean);  // 线内 = 原文,§4.17 不硬套说明壳
    // 原文仍归仓:短结果也走结果仓,与超帽同一条链。
    const std::filesystem::path combined =
        ledger->session_dir() / "artifacts" / "res-000001.combined.txt";
    REQUIRE(std::filesystem::exists(combined));
    CHECK(std::filesystem::file_size(combined) == lean.size());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("多字节边界(回退路:批次尾归仓预览):中文超帽预览仍是合法 UTF-8") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("utf8");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);

    // 全中文正文:每字 3 字节,50 字 ≈ 150 KB,远超 32 KiB 帽。
    const std::string unit = "文件路径与目录清单样例正文";
    std::string fat;
    while (fat.size() < 150000) {
        fat += unit;
    }
    DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_cjk_01", fat);
    (void)ledger->CloseSession("exit");

    const std::filesystem::path stream = ledger->session_dir() / platform::Utf8ToPath(
        platform::PathToUtf8(ledger->session_dir().filename()) + ".jsonl");
    const auto rows = ReadLines(stream);
    const nlohmann::json* tool_message = FindToolMessage(rows);
    REQUIRE(tool_message != nullptr);
    const std::string preview_text = tool_message->at("message").at("content").get<std::string>();
    CHECK(preview_text.size() <= 32768);
    CHECK(preview_text.size() < fat.size());
    CHECK(platform::IsValidUtf8(preview_text));  // 截断刀口对齐码点边界
    CHECK(preview_text.find("truncated: true") != std::string::npos);
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("Capture quota remains incomplete in adopted preview and immutable metadata") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("capture-quota");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    const auto preview = DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_quota", "captured bytes", true, false);
    CHECK(preview.find("capture_complete: false") != std::string::npos);
    CHECK(preview.find("quota") != std::string::npos);
    CHECK(preview.size() <= 32768);
    std::ifstream file(ledger->session_dir() / "artifacts" / "res-000001.json");
    nlohmann::json metadata;
    file >> metadata;
    CHECK_FALSE(metadata.at("outputs")[0].at("capture_complete").get<bool>());
    CHECK(metadata.at("outputs")[0].at("capture_reason") == "quota");
}

TEST_CASE("Complete small result fits a batch cap without preview framing") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("small-cap");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    CHECK(DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_small_cap", "ok", true, true, 2) == "ok");
}

TEST_CASE("Gemini sends adopted preview while raw structured result stays in the store") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(FreshRoot("structured-preview")));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"gemini", "gemini", "terminal"});
    REQUIRE(bridge != nullptr);
    CHECK(DriveFatToolTurn(*bridge, "SYSTEM", "call_structured", "adopted-preview", true, true, 1024, true) == "adopted-preview");
    std::ifstream file(ledger->session_dir() / "artifacts" / "res-000001.json");
    nlohmann::json metadata;
    file >> metadata;
    CHECK(metadata.at("structured_content").at("raw_structured").get<std::string>().size() == 65536);
}

TEST_CASE("Native multi-resource text is immutable and all source channels share one preview cap") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(FreshRoot("native-payload")));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    const auto preview = DriveFatToolTurn(*bridge, "SYSTEM", "call_native", "short projection", true, true, 32768, false, true);
    CHECK(preview.size() <= 32768);
    CHECK(preview.find("res-000001.combined.txt") != std::string::npos);
    CHECK(preview.find("res-000001.raw_payload.json") != std::string::npos);
    std::ifstream file(ledger->session_dir() / "artifacts" / "res-000001.raw_payload.json");
    nlohmann::json raw;
    file >> raw;
    REQUIRE(raw.size() == 2);
    CHECK(raw[0].at("text") == std::string(131072, 'a'));
    CHECK(raw[1].at("text") == std::string(131072, 'b'));
    CHECK(raw[0].at("uri") == "file:///source-a.txt");
    const auto stream = ledger->session_dir() / platform::Utf8ToPath(platform::PathToUtf8(ledger->session_dir().filename()) + ".jsonl");
    const auto rows = ReadLines(stream);
    const auto* message = FindToolMessage(rows);
    REQUIRE(message != nullptr);
    CHECK(message->at("message").at("content") == preview);
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}
