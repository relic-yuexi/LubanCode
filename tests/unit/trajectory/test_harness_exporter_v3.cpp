// One-shot V3 轨迹Harness导出修复单(todos/OneShot_V3轨迹Harness导出修复_
// Harbor非零退出.todo)验收册:V3 主账/子账 → harness-v1 真投影,与格式
// 分派矩阵。全部经真实 v3::V3Writer 建场(§三"不得只测手造 main.jsonl")。
//
// 覆盖:
//   1. v3 单轮真投影:messages/requests/tools/usage/environment/outcome
//      逐项对真实账核对,SHA-256 收据可复算,重复补导两次都成功。
//   2. 子代理子流:两行 record,child 的 parent_run_id 指回 main run。
//   3. 格式矩阵:缺主账/空文件/坏首行/坏 schema/V2V3 并存/坏链/缺子账
//      各给稳定诊断或 fail-closed 存根,无假成功。
//
// v2 格式的既有覆盖(940 行 test_harness_exporter.cpp)与本单不冲突:
// 本单未改 BuildStreamHarnessRecord/FoldStreamReplay 的 v2 折叠逻辑,只
// 是把它从"唯一路径"改成"格式分派后的 v2 分支",那本册原样是回归网。
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "trajectory/harness_exporter.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace v3 = lubancode::trajectory::v3;
using lubancode::trajectory::ExportSessionHarnessV1;
using lubancode::trajectory::HarnessExportOptions;
using lubancode::trajectory::HarnessExportReport;

namespace {

class FixedClock : public v3::V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

// sessions/<id>/<id>.jsonl 布局,与生产 TrajectoryDirectory::CreateSessionV3
// 同一约定(§1.2)。
struct SessionsRoot {
    FixedClock clock;
    std::filesystem::path root;

    explicit SessionsRoot(const char* tag) {
        root = std::filesystem::temp_directory_path() / ("lubancode-harness-v3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    std::filesystem::path Dir(const std::string& session_id) {
        std::error_code ec;
        const auto dir = root / session_id;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path Ledger(const std::string& session_id) { return Dir(session_id) / (session_id + ".jsonl"); }
};

v3::V3Writer StartMain(SessionsRoot& root, const std::string& session_id,
                       const std::string& run_kind = "one_shot") {
    v3::V3WriterOptions options;
    options.launch_cwd = "/workspace/demo";
    options.run_kind = run_kind;
    auto writer = v3::V3Writer::Start(root.Ledger(session_id), session_id, "run-" + session_id,
                                      "你是 LubanCode。", nlohmann::json::object(), options, &root.clock);
    REQUIRE(writer.has_value());
    return std::move(*writer);
}

struct RequestIds {
    std::string request_id;
    std::string step_id;
};

// 一次请求预备 + 收口事件(model.request.prepared -> model.response.
// completed):assistant/Conversation 消息须挂真实 requestId(schema3 合同,
// §4.4),不能空手就落一条 assistant 消息——两处子代理夹具都要这一步,
// 拆出来避免各自遗漏。
RequestIds PrepareConversationRequest(v3::V3Writer& writer, const std::string& turn_id,
                                      const std::string& step_id, const char* finish_reason) {
    RequestIds ids;
    ids.request_id = writer.NewRequestId();
    ids.step_id = step_id;
    std::vector<std::string> input_refs;
    for (std::size_t i = 1; i < writer.context().chain.size(); ++i) {
        input_refs.push_back(writer.context().chain[i].message_ref);
    }
    REQUIRE(writer
                .PrepareRequest(ids.request_id, turn_id, ids.step_id, "conversation",
                                writer.context().system_message_ref, input_refs,
                                nlohmann::json{{"provider", "anthropic"}, {"wire", "anthropic"},
                                              {"model", "claude-test"}},
                                std::nullopt, v3::Durability::PowerLoss)
                .status == v3::WriteReceipt::Status::Committed);
    v3::EventDraft done;
    done.kind = v3::EventKindV3::ModelResponseCompleted;
    done.status = v3::OpStatus::Done;
    done.request_id = ids.request_id;
    done.turn_id = turn_id;
    done.step_id = ids.step_id;
    done.payload = nlohmann::json{{"finishReason", finish_reason}};
    REQUIRE(writer.AppendEvent(std::move(done), v3::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
    return ids;
}

// 一枚完整对话轮:user 输入 -> assistant 声明 tool_call -> 工具执行终态 ->
// tool 消息 -> assistant 最终文本作答(带 usage)。返回 action_id,供调用
// 方继续断言/派生子代理。
std::string InstallToolTurn(v3::V3Writer& writer, const std::string& turn_id,
                            const std::string& tool_name, const nlohmann::json& tool_args) {
    v3::MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = v3::MessagePurpose::Conversation;
    user.origin = v3::MessageOrigin::Human;
    user.message = nlohmann::json{
        {"role", "user"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "帮我查一下天气"}}})}};
    const auto user_receipt = writer.AppendMessage(std::move(user), v3::Durability::PowerLoss);
    REQUIRE(user_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status == v3::WriteReceipt::Status::Committed);

    const std::string action_id = writer.NewActionId();
    const std::string step_id = writer.NewStepId();
    const std::string request_id = writer.NewRequestId();
    std::vector<std::string> input_refs;
    for (std::size_t i = 1; i < writer.context().chain.size(); ++i) {
        input_refs.push_back(writer.context().chain[i].message_ref);
    }
    REQUIRE(writer
                .PrepareRequest(request_id, turn_id, step_id, "conversation",
                                writer.context().system_message_ref, input_refs,
                                nlohmann::json{{"provider", "anthropic"}, {"wire", "anthropic"},
                                              {"model", "claude-test"}},
                                std::nullopt, v3::Durability::PowerLoss)
                .status == v3::WriteReceipt::Status::Committed);
    {
        v3::EventDraft done;
        done.kind = v3::EventKindV3::ModelResponseCompleted;
        done.status = v3::OpStatus::Done;
        done.request_id = request_id;
        done.turn_id = turn_id;
        done.step_id = step_id;
        done.payload = nlohmann::json{{"finishReason", "tool_use"}};
        REQUIRE(writer.AppendEvent(std::move(done), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    v3::MessageDraft assistant_call;
    assistant_call.turn_id = turn_id;
    assistant_call.step_id = step_id;
    assistant_call.request_id = request_id;
    assistant_call.purpose = v3::MessagePurpose::Conversation;
    assistant_call.origin = v3::MessageOrigin::SessionRuntime;
    assistant_call.provider = "anthropic";
    assistant_call.wire = "anthropic";
    assistant_call.model = "claude-test";
    assistant_call.response_model = nlohmann::json(nullptr);
    assistant_call.usage = nlohmann::json{{"input_tokens", 120}, {"output_tokens", 18},
                                          {"cache_read_tokens", 0}, {"cache_creation_tokens", 0},
                                          {"reasoning_tokens", 0}, {"reported_by_provider", true}};
    assistant_call.message = nlohmann::json{
        {"role", "assistant"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "这就查。"}}})},
        {"tool_calls", nlohmann::json::array({nlohmann::json{
                           {"id", action_id},
                           {"type", "function"},
                           {"function", nlohmann::json{{"name", tool_name},
                                                       {"arguments", tool_args.dump()}}}}})}};
    const auto call_receipt = writer.AppendMessage(std::move(assistant_call), v3::Durability::PowerLoss);
    REQUIRE(call_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({call_receipt.id}).status == v3::WriteReceipt::Status::Committed);

    auto action = v3::ToolActionSession::Admit(writer, turn_id, step_id, action_id, "queued",
                                               call_receipt.id, action_id);
    REQUIRE(action.Start(writer, "args-ref-1", v3::ToolIdentity{tool_name, "builtin", "1.0", "workspace"},
                         std::nullopt)
                .status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.Finish(writer, std::int64_t{0}, std::uint64_t{88}).status ==
            v3::WriteReceipt::Status::Committed);
    const nlohmann::json result_ref = nlohmann::json::array(
        {v3::MakeArtifactRef("res-" + action_id, "result_metadata", "artifacts/res-" + action_id + ".json",
                             std::string(64, '7'), 96, "application/json")});
    const auto persisted =
        action.PersistedResult(writer, result_ref.get<std::vector<nlohmann::json>>(), action.last_event_id());
    REQUIRE(persisted.status == v3::WriteReceipt::Status::Committed);
    const auto selected = action.SelectResult(writer, {persisted.id}, {}, "done");
    REQUIRE(selected.status == v3::WriteReceipt::Status::Committed);
    const auto tool_message =
        action.AppendToolMessage(writer, "26 度,晴。", action.selected_event_id(), false);
    REQUIRE(tool_message.status == v3::WriteReceipt::Status::Committed);

    // 收尾一次纯文本 assistant 回合(usage 缺实报给 null,不补 0)。
    const std::string final_request_id = writer.NewRequestId();
    const std::string final_step_id = writer.NewStepId();
    std::vector<std::string> final_refs;
    for (std::size_t i = 1; i < writer.context().chain.size(); ++i) {
        final_refs.push_back(writer.context().chain[i].message_ref);
    }
    REQUIRE(writer
                .PrepareRequest(final_request_id, turn_id, final_step_id, "conversation",
                                writer.context().system_message_ref, final_refs,
                                nlohmann::json{{"provider", "anthropic"}, {"wire", "anthropic"},
                                              {"model", "claude-test"}},
                                std::nullopt, v3::Durability::PowerLoss)
                .status == v3::WriteReceipt::Status::Committed);
    {
        v3::EventDraft done;
        done.kind = v3::EventKindV3::ModelResponseCompleted;
        done.status = v3::OpStatus::Done;
        done.request_id = final_request_id;
        done.turn_id = turn_id;
        done.step_id = final_step_id;
        done.payload = nlohmann::json{{"finishReason", "end_turn"}};
        REQUIRE(writer.AppendEvent(std::move(done), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    v3::MessageDraft final_answer;
    final_answer.turn_id = turn_id;
    final_answer.step_id = final_step_id;
    final_answer.request_id = final_request_id;
    final_answer.purpose = v3::MessagePurpose::Conversation;
    final_answer.origin = v3::MessageOrigin::SessionRuntime;
    final_answer.provider = "anthropic";
    final_answer.wire = "anthropic";
    final_answer.model = "claude-test";
    final_answer.response_model = nlohmann::json(nullptr);
    final_answer.usage = nlohmann::json(nullptr);  // 缺实报,读取不补 0
    final_answer.message = nlohmann::json{
        {"role", "assistant"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "今天 26 度,晴。"}}})}};
    const auto final_receipt = writer.AppendMessage(std::move(final_answer), v3::Durability::PowerLoss);
    REQUIRE(final_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({final_receipt.id}).status == v3::WriteReceipt::Status::Committed);
    return action_id;
}

void EndSession(v3::V3Writer& writer) {
    v3::EventDraft ended;
    ended.kind = v3::EventKindV3::SessionEnded;
    ended.payload = nlohmann::json::object();
    REQUIRE(writer.AppendEvent(std::move(ended), v3::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
}

std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE_FALSE(parsed.is_discarded());
        lines.push_back(parsed);
    }
    return lines;
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 单轮真投影
// ---------------------------------------------------------------------------

TEST_CASE("v3 主账单轮:messages/requests/tools/usage/environment/outcome 真投影") {
    SessionsRoot root("single-turn");
    const std::string session_id = "20260907-000001-AAAAAA";
    auto writer = StartMain(root, session_id);
    const std::string turn_id = "turn-000001";
    const std::string action_id =
        InstallToolTurn(writer, turn_id, "get_weather", nlohmann::json{{"city", "上海"}});
    EndSession(writer);

    const auto session_dir = root.Dir(session_id);
    const auto target = session_dir / "out.harness.jsonl";
    const HarnessExportReport report = ExportSessionHarnessV1(session_dir, target, HarnessExportOptions{}, 0);
    REQUIRE(report.ok());
    CHECK(report.records == 1);
    CHECK(report.session_id == session_id);
    CHECK_FALSE(report.sha256.empty());

    const auto lines = ReadJsonl(target);
    REQUIRE(lines.size() == 1);
    const nlohmann::json& record = lines.front();

    CHECK(record["session_id"] == session_id);
    CHECK(record["run_kind"] == "one_shot");
    CHECK(record["parent_run_id"].is_null());
    CHECK(record["source"]["format"] == "v3");

    // 环境:未采集时如实报缺(本册没跑 CaptureEnvironment)。
    CHECK(record["environment"]["snapshot_available"] == false);

    // turns:字段不兼容处版本化 —— v3 没有 turn.completed,terminal 用
    // v3.turn_* 词表,不冒充 v2 同名值。
    REQUIRE(record["turns"].size() == 1);
    CHECK(record["turns"][0]["turn_id"] == turn_id);
    CHECK(record["turns"][0]["terminal"] == "v3.turn_complete");
    CHECK(record["turns"][0]["trigger"] == "human");

    // messages:user/assistant(带 tool_calls)/tool/assistant 四条,顺序
    // 与落盘序一致。
    const auto& messages = record["messages"];
    REQUIRE(messages.size() == 4);
    CHECK(messages[0]["role"] == "user");
    CHECK(messages[0]["content"][0]["text"] == "帮我查一下天气");
    CHECK(messages[1]["role"] == "assistant");
    REQUIRE(messages[1].contains("tool_calls"));
    CHECK(messages[1]["tool_calls"][0]["call_id"] == action_id);
    CHECK(messages[1]["tool_calls"][0]["name"] == "get_weather");
    CHECK(messages[1]["tool_calls"][0]["arguments"]["city"] == "上海");
    CHECK(messages[2]["role"] == "tool");
    CHECK(messages[2]["call_id"] == action_id);
    CHECK(messages[2]["is_error"] == false);
    CHECK(messages[2]["content"][0]["text"] == "26 度,晴。");
    CHECK(messages[3]["role"] == "assistant");
    CHECK(messages[3]["content"][0]["text"] == "今天 26 度,晴。");

    // requests:两次请求,usage 唯一 owner 是 assistant message;缺实报
    // 给 null,不补 0。
    REQUIRE(record["requests"].size() == 2);
    CHECK(record["requests"][0]["output_state"] == "committed");
    CHECK(record["requests"][0]["usage"]["input_tokens"] == 120);
    CHECK(record["requests"][0]["stop_reason"] == "tool_use");
    CHECK(record["requests"][1]["usage"].is_null());
    CHECK(record["usage_totals"]["input_tokens"] == 120);
    CHECK(record["usage_totals"]["requests_with_reported_usage"] == 1);

    // tools:折叠出的终态与结果正文。
    REQUIRE(record["tools"].size() == 1);
    CHECK(record["tools"][0]["call_id"] == action_id);
    CHECK(record["tools"][0]["tool_name"] == "get_weather");
    CHECK(record["tools"][0]["outcome"] == "done");
    CHECK(record["tools"][0]["exit_code"] == 0);
    CHECK(record["tools"][0]["result"]["content"][0]["text"] == "26 度,晴。");

    // outcome:session.ended 落账、无工具缺口、末回合 complete → success。
    CHECK(record["outcome"]["status"] == "success");
    CHECK(record["outcome"]["session_ended"] == true);
    CHECK(record["outcome"]["process_exit_code"] == 0);

    // 收据:SHA-256 与实际文件字节一致。
    CHECK(report.sha256 == lubancode::hooks::Sha256Hex(ReadFileBytes(target)));

    // 重复补导:不留半截,第二次导出仍成功且可覆盖(补导命令语义)。
    const HarnessExportReport again = ExportSessionHarnessV1(session_dir, target, HarnessExportOptions{}, 0);
    REQUIRE(again.ok());
    CHECK(again.records == 1);
}

// ---------------------------------------------------------------------------
// 2. 子代理子流:parent_run_id 真关联
// ---------------------------------------------------------------------------

TEST_CASE("v3 子代理子流:两行 record,child 的 parent_run_id 指回 main run") {
    SessionsRoot root("subagent");
    const std::string session_id = "20260907-000002-BBBBBB";
    auto writer = StartMain(root, session_id);
    const std::string turn_id = "turn-000001";

    v3::MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = v3::MessagePurpose::Conversation;
    user.origin = v3::MessageOrigin::Human;
    user.message = nlohmann::json{
        {"role", "user"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "派个子代理去查资料"}}})}};
    const auto user_receipt = writer.AppendMessage(std::move(user), v3::Durability::PowerLoss);
    REQUIRE(user_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status == v3::WriteReceipt::Status::Committed);

    const std::string action_id = writer.NewActionId();
    const RequestIds call_request = PrepareConversationRequest(writer, turn_id, "step-000001", "tool_use");
    v3::MessageDraft call;
    call.turn_id = turn_id;
    call.step_id = call_request.step_id;
    call.request_id = call_request.request_id;
    call.purpose = v3::MessagePurpose::Conversation;
    call.origin = v3::MessageOrigin::SessionRuntime;
    call.provider = "anthropic";
    call.wire = "anthropic";
    call.model = "claude-test";
    call.response_model = nlohmann::json(nullptr);
    call.usage = nlohmann::json(nullptr);
    call.message = nlohmann::json{
        {"role", "assistant"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "派工。"}}})},
        {"tool_calls", nlohmann::json::array({nlohmann::json{
                           {"id", action_id},
                           {"type", "function"},
                           {"function", nlohmann::json{{"name", "agent"},
                                                       {"arguments", "{}"}}}}})}};
    const auto call_receipt = writer.AppendMessage(std::move(call), v3::Durability::PowerLoss);
    REQUIRE(call_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({call_receipt.id}).status == v3::WriteReceipt::Status::Committed);

    const std::string child_id = "20260907-000002-CCCCCC";
    v3::SubagentSpawn spawn = v3::SubagentSpawn::Request(
        writer, action_id, turn_id, "step-000001", "task-000001",
        v3::ChildSessionRef{child_id, "run-" + child_id, "subagents/" + child_id + "/" + child_id + ".jsonl"},
        v3::ParentActionRef{session_id, "run-" + session_id, turn_id, "step-000001", action_id, call_receipt.id},
        nlohmann::json::object(), nlohmann::json::object());
    auto bootstrap = spawn.BootstrapChild(writer, "run-" + child_id, "你是子代理。", "帮主代理查资料");
    REQUIRE(bootstrap.child_writer.has_value());
    REQUIRE(spawn.Link(writer, bootstrap.checkpoint).status == v3::WriteReceipt::Status::Committed);

    // 子账自跑一轮简单对话并收口。
    v3::MessageDraft child_reply;
    child_reply.turn_id = "goaleval-turn-1";
    child_reply.purpose = v3::MessagePurpose::Conversation;
    child_reply.origin = v3::MessageOrigin::SessionRuntime;
    child_reply.provider = "anthropic";
    child_reply.wire = "anthropic";
    child_reply.model = "claude-test";
    child_reply.response_model = nlohmann::json(nullptr);
    child_reply.usage = nlohmann::json{{"input_tokens", 40}, {"output_tokens", 10},
                                       {"cache_read_tokens", 0}, {"cache_creation_tokens", 0},
                                       {"reasoning_tokens", 0}, {"reported_by_provider", true}};
    child_reply.message = nlohmann::json{
        {"role", "assistant"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "查到了。"}}})}};
    const auto child_reply_receipt =
        bootstrap.child_writer->AppendMessage(std::move(child_reply), v3::Durability::PowerLoss);
    REQUIRE(child_reply_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(bootstrap.child_writer->AdmitMessages({child_reply_receipt.id}).status ==
            v3::WriteReceipt::Status::Committed);
    EndSession(*bootstrap.child_writer);
    EndSession(writer);

    const auto session_dir = root.Dir(session_id);
    const auto target = session_dir / "out.harness.jsonl";
    const HarnessExportReport report = ExportSessionHarnessV1(session_dir, target, HarnessExportOptions{});
    REQUIRE(report.ok());
    CHECK(report.records == 2);

    const auto lines = ReadJsonl(target);
    REQUIRE(lines.size() == 2);
    const nlohmann::json* main_record = nullptr;
    const nlohmann::json* child_record = nullptr;
    for (const auto& line : lines) {
        if (line["session_id"] == session_id) {
            main_record = &line;
        } else if (line["session_id"] == child_id) {
            child_record = &line;
        }
    }
    REQUIRE(main_record != nullptr);
    REQUIRE(child_record != nullptr);
    CHECK((*main_record)["parent_run_id"].is_null());
    CHECK((*child_record)["parent_run_id"] == (*main_record)["run_id"]);
    CHECK((*child_record)["run_kind"] == "subagent");
    // 主账那枚工具行留了 child_run_id,能对回子流。
    bool found_child_link = false;
    for (const auto& tool : (*main_record)["tools"]) {
        if (tool.contains("child_run_id") && tool["child_run_id"] == (*child_record)["run_id"]) {
            found_child_link = true;
        }
    }
    CHECK(found_child_link);
}

// ---------------------------------------------------------------------------
// 3. 格式矩阵:各给稳定诊断,不吞成"没有流",不装 clean success
// ---------------------------------------------------------------------------

TEST_CASE("v3 格式矩阵:主账缺失(空目录)给既有 export.no_streams,不新立顶层码") {
    SessionsRoot root("fmt-missing");
    const std::string session_id = "20260907-000003-DDDDDD";
    root.Dir(session_id);  // 建目录但不写任何账
    const auto session_dir = root.Dir(session_id);
    const auto report =
        ExportSessionHarnessV1(session_dir, session_dir / "out.jsonl", HarnessExportOptions{});
    CHECK_FALSE(report.ok());
    // 目录在、两种主账都不在:与"没开过 trajectory"同一件事,复用既有
    // export.no_streams 契约(不擅自新立顶层错误码);格式感知的诊断价值
    // 体现在文案带了实际目录与"格式=unknown",而不是新码。
    CHECK(report.error_code == "export.no_streams");
    CHECK(report.message.find(lubancode::platform::PathToUtf8(session_dir)) != std::string::npos);
}

TEST_CASE("v3 格式矩阵:空首行给 export.session_format_unreadable") {
    SessionsRoot root("fmt-empty");
    const std::string session_id = "20260907-000004-EEEEEE";
    const auto dir = root.Dir(session_id);
    std::ofstream(dir / (session_id + ".jsonl"), std::ios::binary).close();  // 触碰空文件
    const auto report = ExportSessionHarnessV1(dir, dir / "out.jsonl", HarnessExportOptions{});
    CHECK_FALSE(report.ok());
    CHECK(report.error_code == "export.session_format_unreadable");
}

TEST_CASE("v3 格式矩阵:坏首行(非 JSON)给 export.session_format_unreadable") {
    SessionsRoot root("fmt-bad-line");
    const std::string session_id = "20260907-000005-FFFFFF";
    const auto dir = root.Dir(session_id);
    std::ofstream out(dir / (session_id + ".jsonl"), std::ios::binary);
    out << "这不是 JSON\n";
    out.close();
    const auto report = ExportSessionHarnessV1(dir, dir / "out.jsonl", HarnessExportOptions{});
    CHECK_FALSE(report.ok());
    CHECK(report.error_code == "export.session_format_unreadable");
}

TEST_CASE("v3 格式矩阵:坏 schema(schemaVersion 不认)给 export.session_format_unsupported") {
    SessionsRoot root("fmt-bad-schema");
    const std::string session_id = "20260907-000006-GGGGGG";
    const auto dir = root.Dir(session_id);
    std::ofstream out(dir / (session_id + ".jsonl"), std::ios::binary);
    out << nlohmann::json{{"type", "message"}, {"schemaVersion", 99}, {"sessionId", session_id}}.dump()
        << "\n";
    out.close();
    const auto report = ExportSessionHarnessV1(dir, dir / "out.jsonl", HarnessExportOptions{});
    CHECK_FALSE(report.ok());
    CHECK(report.error_code == "export.session_format_unsupported");
}

TEST_CASE("v3 格式矩阵:V2/V3 并存给 export.session_format_conflict") {
    SessionsRoot root("fmt-conflict");
    const std::string session_id = "20260907-000007-HHHHHH";
    auto writer = StartMain(root, session_id);
    InstallToolTurn(writer, "turn-000001", "noop", nlohmann::json::object());
    EndSession(writer);
    const auto dir = root.Dir(session_id);
    std::ofstream legacy(dir / "main.jsonl", std::ios::binary);
    legacy << "{}\n";  // 内容不重要,存在即触发冲突判定
    legacy.close();
    const auto report = ExportSessionHarnessV1(dir, dir / "out.jsonl", HarnessExportOptions{});
    CHECK_FALSE(report.ok());
    CHECK(report.error_code == "export.session_format_conflict");
}

TEST_CASE("v3 格式矩阵:坏链(哈希链断)fail-closed 存根,不装 clean success") {
    SessionsRoot root("fmt-broken-chain");
    const std::string session_id = "20260907-000008-IIIIII";
    auto writer = StartMain(root, session_id);
    InstallToolTurn(writer, "turn-000001", "noop", nlohmann::json::object());
    EndSession(writer);
    const auto dir = root.Dir(session_id);
    const auto ledger_path = dir / (session_id + ".jsonl");

    // 篡改中间一行的可见字段,链哈希对不上(首行仍是合法 v3 schema,探针
    // 判 V3Stream;WalkSessionTree/ReadV3Ledger 验链时才炸)。
    auto lines = ReadJsonl(ledger_path);
    REQUIRE(lines.size() > 3);
    lines[2]["tamper"] = "坏链注入";
    std::ofstream rewritten(ledger_path, std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) {
        rewritten << line.dump() << "\n";
    }
    rewritten.close();

    const auto report = ExportSessionHarnessV1(dir, dir / "out.jsonl", HarnessExportOptions{});
    // export.no_streams/session_format_* 都不该报——探针只看首行,判定
    // 出 V3Stream;坏链发生在 WalkSessionTree 深处,按引擎既有 fail-closed
    // 纪律出一行 outcome=unknown 存根,导出本身不报错(与 v2"验链失败"
    // 存根同一口径,见 test_harness_exporter.cpp 同名案例)。
    REQUIRE(report.ok());
    CHECK(report.records == 1);
    const auto lines_out = ReadJsonl(dir / "out.jsonl");
    REQUIRE(lines_out.size() == 1);
    CHECK(lines_out[0]["outcome"]["status"] == "unknown");
    CHECK(lines_out[0]["source"]["fold_error"] == "v3.ledger_unreadable");
}

TEST_CASE("v3 格式矩阵:缺子账给存根行,主账仍如实导出") {
    SessionsRoot root("fmt-missing-child");
    const std::string session_id = "20260907-000009-JJJJJJ";
    auto writer = StartMain(root, session_id);
    const std::string turn_id = "turn-000001";

    v3::MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = v3::MessagePurpose::Conversation;
    user.origin = v3::MessageOrigin::Human;
    user.message = nlohmann::json{
        {"role", "user"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "派个子代理"}}})}};
    const auto user_receipt = writer.AppendMessage(std::move(user), v3::Durability::PowerLoss);
    REQUIRE(user_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status == v3::WriteReceipt::Status::Committed);

    const std::string action_id = writer.NewActionId();
    const RequestIds call_request = PrepareConversationRequest(writer, turn_id, "step-000001", "tool_use");
    v3::MessageDraft call;
    call.turn_id = turn_id;
    call.step_id = call_request.step_id;
    call.request_id = call_request.request_id;
    call.purpose = v3::MessagePurpose::Conversation;
    call.origin = v3::MessageOrigin::SessionRuntime;
    call.provider = "anthropic";
    call.wire = "anthropic";
    call.model = "claude-test";
    call.response_model = nlohmann::json(nullptr);
    call.usage = nlohmann::json(nullptr);
    call.message = nlohmann::json{
        {"role", "assistant"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "派工。"}}})},
        {"tool_calls", nlohmann::json::array({nlohmann::json{
                           {"id", action_id},
                           {"type", "function"},
                           {"function", nlohmann::json{{"name", "agent"}, {"arguments", "{}"}}}}})}};
    const auto call_receipt = writer.AppendMessage(std::move(call), v3::Durability::PowerLoss);
    REQUIRE(call_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({call_receipt.id}).status == v3::WriteReceipt::Status::Committed);

    // 只记派发事实(步 1),不建子目录/不 BootstrapChild/不 Link——子账
    // 连不上(child_missing/not_linked 二选一,视 schema 落法,均归"缺
    // 子账"这一类,下面按 fold_error 前缀断言即可)。
    const std::string child_id = "20260907-000009-KKKKKK";
    v3::SubagentSpawn::Request(
        writer, action_id, turn_id, "step-000001", "task-000001",
        v3::ChildSessionRef{child_id, "run-" + child_id, "subagents/" + child_id + "/" + child_id + ".jsonl"},
        v3::ParentActionRef{session_id, "run-" + session_id, turn_id, "step-000001", action_id, call_receipt.id},
        nlohmann::json::object(), nlohmann::json::object());
    EndSession(writer);

    const auto dir = root.Dir(session_id);
    const auto report = ExportSessionHarnessV1(dir, dir / "out.jsonl", HarnessExportOptions{});
    REQUIRE(report.ok());
    CHECK(report.records == 2);  // 主账一行 + 缺子账存根一行,不静默丢失

    const auto lines_out = ReadJsonl(dir / "out.jsonl");
    REQUIRE(lines_out.size() == 2);
    bool found_stub = false;
    for (const auto& line : lines_out) {
        if (line["session_id"] == child_id) {
            found_stub = true;
            CHECK(line["outcome"]["status"] == "unknown");
            CHECK(line["source"]["fold_error"].get<std::string>().rfind("v3.", 0) == 0);
        }
    }
    CHECK(found_stub);
}
