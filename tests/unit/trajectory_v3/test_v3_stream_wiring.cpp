// 轨迹 v3 D1 修复钉:生产桥(TrajectorySessionLedger 真开卷 + 假后端回合)
// 的 assistant 落账必须走流式三件套(§4.43)——model.response.started →
// (model.response.delta 批次)→ model.response.completed + 完整 assistant
// (预留 messageId 成行)→ 接纳。验卷双保险:C++ VerifyV3File + 与
// scripts/validate_trajectory_v3.py 同口径的"assistant 必有定稿事件"语义
// 断言(第一轮验收 7 红全是它)。四幕:
//   1. 流式:片段攒批(4 KiB 窗口批满即落)+ 终态前放行尾巴;
//   2. 非流式后端:零片段同样 started+completed+assistant 闭环(形状一致);
//   3. Esc 中段(§4.63):已收内容经 InterruptStreamResponse 定稿成
//      interrupted assistant,水位留档,迟到 usage 走 appended;
//   4. 流未起即取消:裸 cancelled,不伪造流不伪造 assistant。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // RequestPreparedContext/OutputCancelSource(桥的模型边界口)
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
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
    const auto dir =
        std::filesystem::temp_directory_path() / ("lubancode-v3-stream-wiring-" + std::string(tag));
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
    options.lubancode_version = "0.26.245-test";
    options.v3_system_content = "你是 LubanCode,流式账要三件套。";
    return options;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

api::Usage SampleUsage() {
    api::Usage usage;
    usage.input_tokens = 1180;
    usage.output_tokens = 24;
    return usage;
}

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
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

std::vector<std::string> KindsOf(const std::vector<nlohmann::json>& rows) {
    std::vector<std::string> kinds;
    for (const auto& row : rows) {
        // json 缺键一律 contains():operator[] 缺键是 UB(平台坑清单)。
        const auto it = row.find("kind");
        kinds.push_back(it != row.end() && it->is_string() ? it->get<std::string>() : std::string());
    }
    return kinds;
}

// 与 scripts/validate_trajectory_v3.py 的语义断言同口径:每枚 assistant 的
// messageId 恰有一次定稿事件(completed/cancelled)。第一轮验收 7 红全是
// "assistant 无定稿事件"——这枚断言就是 D1 的回归钉。
std::vector<std::string> AssistantFinalizationProblems(const std::vector<nlohmann::json>& rows) {
    std::map<std::string, int> finalized;
    for (const auto& row : rows) {
        const std::string kind = row.value("kind", std::string());
        if (kind != "model.response.completed" && kind != "model.response.cancelled") {
            continue;
        }
        if (row.contains("payload") && row.at("payload").contains("messageId") &&
            row.at("payload").at("messageId").is_string()) {
            ++finalized[row.at("payload").at("messageId").get<std::string>()];
        }
    }
    std::vector<std::string> problems;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) != "message" || !row.contains("message") ||
            !row.at("message").is_object() ||
            row.at("message").value("role", std::string()) != "assistant") {
            continue;
        }
        const std::string id = row.value("messageId", std::string());
        const auto it = finalized.find(id);
        const int count = it != finalized.end() ? it->second : 0;
        if (count == 0) {
            problems.push_back("assistant " + id + " 无定稿事件");
        } else if (count > 1) {
            problems.push_back("assistant " + id + " 定稿事件多于一次");
        }
    }
    return problems;
}

}  // namespace

// ---------------------------------------------------------------------------
// 幕一:流式——started → 片段攒批(批满即落)→ completed + assistant 闭环
// ---------------------------------------------------------------------------

TEST_CASE("流式三件套: 攒批落 delta,completed 定稿唯一 assistant,验卷过") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("stream");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);

    const std::string big_text = std::string(5000, 'A');  // > 4 KiB 窗口,批满即落
    const std::string tail_text = "收尾一段。";
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("讲讲构建系统的分层。"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-STREAM", {UserMessage("讲讲构建系统的分层。")}),
            agent::RequestPreparedContext{});
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        // SSE 消费点同拍:MessageStart 到 = 响应开始;增量为片段。
        bridge->OnResponseStarted(request_id);
        bridge->OnStreamDelta(request_id, "text", big_text);     // 批满即落(1 批)
        bridge->OnStreamDelta(request_id, "text", tail_text);    // 不足一批,尾巴
        bridge->OnStreamDelta(request_id, "reasoning", "先想");  // 尾巴
        bridge->OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true,
                                "resp-stream-1", 0, true, false);
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText(big_text + tail_text),
                                          "end_turn", "resp-stream-1"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());

    const auto rows = ReadLines(stream);
    const auto kinds = KindsOf(rows);
    // 三件套齐:started、delta 批次、completed;assistant 恰一枚。
    REQUIRE(std::find(kinds.begin(), kinds.end(), "model.response.started") != kinds.end());
    REQUIRE(std::find(kinds.begin(), kinds.end(), "model.response.completed") != kinds.end());
    CHECK(std::count(kinds.begin(), kinds.end(), "model.response.delta") == 3);
    CHECK(std::count_if(rows.begin(), rows.end(), [](const nlohmann::json& row) {
              return row.value("type", std::string()) == "message" && row.contains("message") &&
                     row.at("message").value("role", std::string()) == "assistant";
          }) == 1);

    // started 预留的 messageId 即最终 assistant 的 messageId(§4.43 预留 id 成行)。
    std::string reserved_id;
    std::string assistant_id;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == "model.response.started") {
            reserved_id = row.at("payload").value("messageId", std::string());
            CHECK(row.at("payload").value("streamId", std::string()) != "");
        }
        if (row.value("type", std::string()) == "message" && row.contains("message") &&
            row.at("message").value("role", std::string()) == "assistant") {
            assistant_id = row.value("messageId", std::string());
            // 来源三件套与 usage(§4.44/§4.12):assistant 自带。
            CHECK(row.value("provider", std::string()) == "moonshot");
            CHECK(row.value("wire", std::string()) == "openai-chat-completions");
            CHECK(row.value("model", std::string()) == "kimi-k2.6");
            CHECK(row.value("responseModel", std::string()) == "resp-stream-1");
            REQUIRE(row.contains("usage"));
            CHECK(row.at("usage").value("inputTokens", 0) == 1180);
            // 完整 assistant 的正文是块数组(单文本块;与既有生产桥同形)。
            REQUIRE(row.at("message").contains("content"));
            REQUIRE(row.at("message").at("content").is_array());
            REQUIRE(row.at("message").at("content").size() == 1);
            CHECK(row.at("message").at("content").at(0).value("type", std::string()) == "text");
            CHECK(row.at("message").at("content").at(0).value("text", std::string()) ==
                  big_text + tail_text);
        }
    }
    REQUIRE_FALSE(reserved_id.empty());
    CHECK(assistant_id == reserved_id);

    // delta 批次:序号 1..3 连续递增,首批是批满的大段,终态前放行的尾巴在后。
    std::vector<std::pair<std::uint64_t, std::string>> deltas;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) != "model.response.delta") {
            continue;
        }
        REQUIRE(row.contains("payload"));
        deltas.emplace_back(row.at("payload").value("sequence", std::uint64_t{0}),
                            row.at("payload").value("deltaType", std::string()));
    }
    REQUIRE(deltas.size() == 3);
    CHECK(deltas[0].first == 1);
    CHECK(deltas[0].second == "text");
    CHECK(deltas[1].first == 2);
    CHECK(deltas[1].second == "reasoning");
    CHECK(deltas[2].first == 3);
    CHECK(deltas[2].second == "text");
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) != "model.response.delta") {
            continue;
        }
        if (row.at("payload").value("sequence", std::uint64_t{0}) == 1) {
            CHECK(row.at("payload").at("content").value("text", std::string()) == big_text);
        }
    }
    // completed 是定稿事件:status done、带 finishReason、引用同一 messageId。
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) != "model.response.completed") {
            continue;
        }
        CHECK(row.value("status", std::string()) == "done");
        REQUIRE(row.contains("payload"));
        CHECK(row.at("payload").value("messageId", std::string()) == reserved_id);
        CHECK(row.at("payload").value("finishReason", std::string()) == "end_turn");
    }

    // D1 回归钉(与 validate_trajectory_v3.py 同口径)+ 验卷。
    CHECK(AssistantFinalizationProblems(rows).empty());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// 幕二:非流式后端——零片段,同样 started+completed+assistant 闭环
// ---------------------------------------------------------------------------

TEST_CASE("非流式后端: 零 delta 批次,三件套形状一致,验卷过") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("oneshot");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("一次性回我。"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-ONESHOT", {UserMessage("一次性回我。")}),
            agent::RequestPreparedContext{});
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        // 非流式:没有 OnResponseStarted/OnStreamDelta,直接完整回复。
        bridge->OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true,
                                "resp-oneshot", 0, true, false);
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("一次答完。"), "end_turn",
                                          "resp-oneshot"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());

    const auto rows = ReadLines(stream);
    const auto kinds = KindsOf(rows);
    // started 懒起:零 delta 批次,started → completed → assistant 仍闭环。
    CHECK(std::find(kinds.begin(), kinds.end(), "model.response.started") != kinds.end());
    CHECK(std::count(kinds.begin(), kinds.end(), "model.response.delta") == 0);
    CHECK(std::find(kinds.begin(), kinds.end(), "model.response.completed") != kinds.end());
    CHECK(std::count_if(rows.begin(), rows.end(), [](const nlohmann::json& row) {
              return row.value("type", std::string()) == "message" && row.contains("message") &&
                     row.at("message").value("role", std::string()) == "assistant";
          }) == 1);
    CHECK(AssistantFinalizationProblems(rows).empty());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// 幕三:Esc 中段(§4.63)——已收内容定稿 interrupted assistant,水位留档
// ---------------------------------------------------------------------------

TEST_CASE("流中断: 已收内容定稿 interrupted assistant,迟到 usage 走 appended") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("interrupt");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("重构整个构建系统。"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-ESC", {UserMessage("重构整个构建系统。")}),
            agent::RequestPreparedContext{});
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        bridge->OnResponseStarted(request_id);
        bridge->OnStreamDelta(request_id, "reasoning", "构建分三层看。");
        bridge->OnStreamDelta(request_id, "text", "先看配置生成层。");
        // 中断前 provider 报过部分 usage。
        api::Usage partial;
        partial.input_tokens = 640;
        bridge->OnUsageRecorded(request_id, partial, /*reported_by_provider=*/true, "resp-esc", 0,
                                true, false);
        bridge->OnOutputCancelled(request_id, agent::OutputCancelSource::UserInterrupt);
        // 定稿后迟到的 usage:追加事件,不倒改旧 message(§4.43/§4.12)。
        api::Usage late;
        late.input_tokens = 640;
        late.output_tokens = 5;
        bridge->OnUsageRecorded(request_id, late, /*reported_by_provider=*/true, "resp-esc", 0,
                                true, false);
        bridge->EndTurn(/*ok=*/false, /*cancelled=*/true, "user_interrupt");
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());

    const auto rows = ReadLines(stream);
    const auto kinds = KindsOf(rows);
    REQUIRE(std::find(kinds.begin(), kinds.end(), "model.response.started") != kinds.end());
    REQUIRE(std::find(kinds.begin(), kinds.end(), "model.response.cancelled") != kinds.end());
    CHECK(std::count(kinds.begin(), kinds.end(), "model.response.delta") == 2);
    CHECK(std::count(kinds.begin(), kinds.end(), "model.usage.appended") == 1);
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) != "model.usage.appended") {
            continue;
        }
        // 迟到的更正进追加事件(outputTokens=5),不倒改已定稿的 assistant。
        REQUIRE(row.contains("payload"));
        CHECK(row.at("payload").at("usage").value("inputTokens", 0) == 640);
        CHECK(row.at("payload").at("usage").value("outputTokens", 0) == 5);
    }

    std::string interrupted_id;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == "model.response.cancelled") {
            CHECK(row.value("status", std::string()) == "cancelled");
            REQUIRE(row.contains("payload"));
            // 接收水位 = 已落批次数(§4.63)。
            CHECK(row.at("payload").value("receivedThrough", std::uint64_t{0}) == 2);
            interrupted_id = row.at("payload").value("messageId", std::string());
        }
    }
    REQUIRE_FALSE(interrupted_id.empty());

    int interrupted_assistants = 0;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) != "message" || !row.contains("message") ||
            row.at("message").value("role", std::string()) != "assistant") {
            continue;
        }
        ++interrupted_assistants;
        CHECK(row.value("messageId", std::string()) == interrupted_id);
        CHECK(row.value("completionStatus", std::string()) == "interrupted");
        // 中断时已收的部分 usage 照实内联(生产折算是五键全量:中断时还没
        // 收到的 outputTokens 记 0,与实报同套键);迟到的更正不倒改这里
        //(走 appended,见下)。
        REQUIRE(row.contains("usage"));
        CHECK(row.at("usage").value("inputTokens", 0) == 640);
        CHECK(row.at("usage").value("outputTokens", 0) == 0);
        // 已收内容成行(thinking + text 两块,§4.63);未收齐的不伪造。
        REQUIRE(row.at("message").contains("content"));
        REQUIRE(row.at("message").at("content").is_array());
        REQUIRE(row.at("message").at("content").size() == 2);
        CHECK(row.at("message").at("content").at(0).value("type", std::string()) == "thinking");
        CHECK(row.at("message").at("content").at(0).value("text", std::string()) ==
              "构建分三层看。");
        CHECK(row.at("message").at("content").at(1).value("type", std::string()) == "text");
        CHECK(row.at("message").at("content").at(1).value("text", std::string()) ==
              "先看配置生成层。");
    }
    CHECK(interrupted_assistants == 1);
    CHECK(AssistantFinalizationProblems(rows).empty());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// 幕四:流未起即取消——裸 cancelled,不伪造流不伪造 assistant
// ---------------------------------------------------------------------------

TEST_CASE("流未起即取消: 裸 cancelled 事件,无 assistant,不伪造") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("early-cancel");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("发出就反悔。"));
        const std::string request_id = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-CANCEL", {UserMessage("发出就反悔。")}),
            agent::RequestPreparedContext{});
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        // 没有 OnResponseStarted:一个片段都没收到。
        bridge->OnOutputCancelled(request_id, agent::OutputCancelSource::Internal);
        bridge->EndTurn(/*ok=*/false, /*cancelled=*/true, "internal_cancel");
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());

    const auto rows = ReadLines(stream);
    const auto kinds = KindsOf(rows);
    CHECK(std::find(kinds.begin(), kinds.end(), "model.response.cancelled") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "model.response.started") == kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "model.response.delta") == kinds.end());
    CHECK(std::count_if(rows.begin(), rows.end(), [](const nlohmann::json& row) {
              return row.value("type", std::string()) == "message" && row.contains("message") &&
                     row.at("message").value("role", std::string()) == "assistant";
          }) == 0);
    CHECK(AssistantFinalizationProblems(rows).empty());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}
