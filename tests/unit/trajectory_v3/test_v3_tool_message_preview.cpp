// V3-REAL-05(真实会话审计单):工具结果已存仓,不得再把巨量全文当模型
// 预览。现场病理:run_command 递归列目录 2MB 正文原样进了 tool 消息(seq
// 331 / 2,097,251 字节),结果仓、模型预览与临时裁剪各走一段。修后合同:
// 原文按 artifact 不可变归仓;tool 消息的 content 是模型可见的最终预览
// 版本——汇总(combined)后总字节超当前档(默认 32 KiB)才走 §4.17 预览
// 路(说明区标签路径也占预算 + 头尾节选,如实交代 truncated),线内原样;
// run_command 一类副作用工具同样受帽。夹具脱敏缩小(300 KB / 150 KB,
// 保持"超帽巨肥"故障结构),不复制用户会话原档。
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

// 一轮"输入 → 请求/回复(声明 run_command)→ 执行 → 巨肥结果回喂"的
// 完整流,与生产桥口同形。
void DriveFatToolTurn(TrajectoryTurnBridge& bridge, const std::string& system,
                      const std::string& call_id, const std::string& result_content) {
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
    bridge.OnToolResultsCommitted("batch-1", ToolResultMessage(call_id, result_content));
    bridge.EndTurn(/*ok=*/true, /*cancelled=*/false, "");
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

TEST_CASE("超帽结果:原文归仓,tool 消息只带 32 KiB 预算内预览,如实交代截断") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("fat");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);

    // 脱敏缩样的 2MB 递归列目录:300 KB,头尾各留可辨认标记,中间填充。
    const std::string head_mark = "HEAD-recursive-listing-begin\n";
    const std::string tail_mark = "\nTAIL-recursive-listing-end";
    std::string fat = head_mark + std::string(300000, 'x') + tail_mark;
    DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_fat_01", fat);
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());

    // tool 消息:总帽 32 KiB,头尾节选,说明区含截断/全文路径。
    const std::filesystem::path stream = ledger->session_dir() / platform::Utf8ToPath(
        platform::PathToUtf8(ledger->session_dir().filename()) + ".jsonl");
    const auto rows = ReadLines(stream);
    const nlohmann::json* tool_message = FindToolMessage(rows);
    REQUIRE(tool_message != nullptr);
    const std::string preview_text = tool_message->at("message").at("content").get<std::string>();
    CHECK(preview_text.size() <= 32768);  // 总帽:说明标签路径头尾全算
    CHECK(preview_text.size() < fat.size());
    CHECK(preview_text.find("truncated: true") != std::string::npos);
    CHECK(preview_text.find("capture_complete: true") != std::string::npos);
    CHECK(preview_text.find("full_output: [\"artifacts/res-000001.combined.txt\"]") !=
          std::string::npos);  // 全文去处可追
    CHECK(preview_text.find("[开头内容]") != std::string::npos);
    CHECK(preview_text.find("[中间内容已省略]") != std::string::npos);
    CHECK(preview_text.find("[结尾内容]") != std::string::npos);
    CHECK(preview_text.find("HEAD-recursive-listing-begin") != std::string::npos);  // 头部节选
    CHECK(preview_text.find("TAIL-recursive-listing-end") != std::string::npos);    // 尾部节选
    CHECK(platform::IsValidUtf8(preview_text));  // 刀口不劈半个字

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

    // 账本自洽:验卷过,工具链折叠 done(预览不破配对/链)。
    const auto verify = lubancode::trajectory::v3::VerifyV3File(stream);
    CHECK(verify.ok);
    auto read = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(read.has_value());
    const auto snapshots = lubancode::trajectory::v3::FoldToolActions(*read);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].folded_status == "done");
    // 链上 tool 消息的正文就是预览(读取侧按 result_ref 展开,不靠解析正文)。
    const auto context = lubancode::trajectory::v3::ProjectModelContext(*read);
    REQUIRE(context.inputs.size() == 3);
    CHECK(context.inputs[2].message.at("content").get<std::string>() == preview_text);
}

TEST_CASE("线内结果:未超限原样返回,不硬套预览壳;原文照旧归仓") {
    EnvGuard v3on("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("lean");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);

    const std::string lean = "src/app/main.cpp\nsrc/core/engine.cpp\n共 2 个文件。";
    DriveFatToolTurn(*bridge, "SYSTEM-PREVIEW", "call_lean_01", lean);
    (void)ledger->CloseSession("exit");

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

TEST_CASE("多字节边界:中文超帽结果的预览仍是合法 UTF-8,头尾汉字不劈半") {
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
