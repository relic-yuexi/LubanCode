// 记忆抽取取消误报 ESC 单 Bug 2 的 v3 旁路账合同:TrajectoryBypassBridge
// 的 v3 写模式 + MemoryTurnLedger 的 v3 写口。
//
// 覆盖:
//   1. 旁路全流(成功例):system/转写 user 落消息行(purpose=memory_extract,
//      turnId=memory-turn-*,parentTurnId 挂触发主回合),prepared(带
//      purpose/timeoutBudgetSecs/引用)→ sent → started → completed +
//      assistant 成行(usage 带、display hidden、不进 conversation 链);
//   2. 超时例:短预算 + 挂死 backend → cancelled(reason=internal_cancel,
//      不提按键)+ model.usage.appended(usage owner 落账),无伪造 assistant;
//   3. 旁路不混主对话:链投影只含主回合输入,记忆行一个不进;
//   4. MemoryTurnLedger 的 v3 写口:assessed(跳过原因/收口材料/墙钟/失败
//      稳定码)与 receipted 事实行;重开会话单凭事件答得出"哪次抽取、
//      预算多久、实际多久、谁叫停";
//   5. 工厂门:v3 场只有 memory_extract 接桥,title/doctor 维持 nullptr
//      (§四清册钉死);v2 构造口照旧可用;
//   6. schema 往返:memory_extract purpose 与两枚 memory.* kind 名字稳定;
//   7. VerifyV3File 全程收卷(哈希链不破)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "agent/sample_model.hpp"
#include "app/memory_extract.hpp"
#include "memory/project_memory.hpp"  // MemoryWriteReceipt(MemoryTurnLedger 的收件口)
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"  // FindV3SessionStream
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(同款见
// test_v3_write_wiring.cpp)。
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
                     ("lubancode-v3-memory-bypass-" + std::string(tag));
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
    options.lubancode_version = "0.26.251-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

// 假后端:一次带 usage 的完整回答。
class ReplyBackend final : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>*) override {
        on_event(api::MessageStart{"resp-mem-1", "cheap-model"});
        on_event(api::TextDelta{"{\"task_type\":\"code\",\"summary\":\"成了\",\"candidates\":[]}"});
        on_event(api::ContentBlockDone{0});
        api::MessageDone done;
        done.stop_reason = "end_turn";
        done.usage.input_tokens = 812;
        done.usage.output_tokens = 96;
        on_event(done);
        return {};
    }
};

// 挂死后端:吃取消旗,旗不拉就等(超时例的靶子)。
class HangingBackend final : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* cancel) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel != nullptr && cancel->load()) {
                return std::unexpected(
                    api::Error{api::ErrorKind::Cancelled, "请求被取消信号中止", 0});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return {};
    }
};

agent::SampleRequest MemorySample() {
    agent::SampleRequest request;
    request.model = "cheap-model";
    request.system = "把这段回合收成结构化总结。";
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"[用户] 修一下入口的 bug\n[助手] 已修。"});
    request.messages.push_back(std::move(message));
    request.max_tokens = 1500;
    return request;
}

std::vector<nlohmann::json> StreamLines(const std::filesystem::path& stream) {
    std::vector<nlohmann::json> lines;
    const auto raw = trajectory::ReadJournalLines(stream);
    REQUIRE(raw.has_value());
    for (const std::string& line : *raw) {
        lines.push_back(nlohmann::json::parse(line, nullptr, false));
        REQUIRE_FALSE(lines.back().is_discarded());
    }
    return lines;
}

const nlohmann::json* FindLine(const std::vector<nlohmann::json>& lines, const char* type,
                               const char* key, const std::string& value) {
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != type) continue;
        if (line.value(key, std::string()) == value) return &line;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("v3 旁路全流(成功例): 消息行 + prepared/sent/started/completed + usage/预算在账") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("success");
    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;

    // 主回合先立号:旁路行的 parentTurnId 要挂它。
    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-000001", "external_user");
    api::Message user_message;
    user_message.role = api::Role::User;
    user_message.content.push_back(api::TextBlock{"修一下入口的 bug"});
    turn->RecordInput(user_message);

    auto bypass = ledger.NewBypassBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "host"},
                                         accounting::RequestPurpose::MemoryExtract);
    REQUIRE(bypass != nullptr);
    agent::SampleOptions options;
    options.boundary_recorder = bypass.get();
    options.purpose = accounting::RequestPurpose::MemoryExtract;
    options.timeout_secs = 45;
    ReplyBackend backend;
    const agent::SampleResult result = agent::SampleModel(backend, MemorySample(), options);
    REQUIRE(result.ok);
    CHECK(bypass->recent_errors().empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);

    // 1) system 消息:purpose=memory_extract、turnId 恒 null、正文是抽取提示词。
    const nlohmann::json* system_line = nullptr;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.value("purpose", std::string()) != "memory_extract") continue;
        if (line.at("message").value("role", std::string()) == "system") {
            system_line = &line;
            break;
        }
    }
    REQUIRE(system_line != nullptr);
    CHECK(system_line->at("turnId").is_null());
    CHECK(system_line->at("message").at("content").get<std::string>().find("结构化总结") !=
          std::string::npos);

    // 2) 转写 user 消息:内部回合号 + parentTurnId 挂主回合。
    const nlohmann::json* user_line = nullptr;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.value("purpose", std::string()) != "memory_extract") continue;
        if (line.at("message").value("role", std::string()) == "user") {
            user_line = &line;
            break;
        }
    }
    REQUIRE(user_line != nullptr);
    const std::string bypass_turn = user_line->at("turnId").get<std::string>();
    CHECK(bypass_turn.rfind("memory-turn-", 0) == 0);
    CHECK(user_line->value("parentTurnId", std::string()) == "turn-000001");
    CHECK(user_line->value("display", std::string()) == "collapsed");

    // 3) prepared:purpose/预算/引用/模型齐,turnId+stepId 在信封。
    const auto* prepared = FindLine(lines, "event", "kind", "model.request.prepared");
    REQUIRE(prepared != nullptr);
    CHECK(prepared->at("payload").value("purpose", std::string()) == "memory_extract");
    CHECK(prepared->at("payload").value("timeoutBudgetSecs", 0) == 45);
    CHECK(prepared->at("payload").value("model", std::string()) == "cheap-model");
    CHECK(prepared->at("payload").value("provider", std::string()) == "kimi");
    CHECK(prepared->at("payload").value("wire", std::string()) == "responses");
    CHECK(prepared->at("payload").at("systemMessageRef").get<std::string>() ==
          system_line->at("messageId").get<std::string>());
    REQUIRE(prepared->at("payload").at("inputMessageRefs").size() == 1);
    CHECK(prepared->at("payload").at("inputMessageRefs")[0] == user_line->at("messageId"));
    const std::string request_id = prepared->value("requestId", std::string());
    REQUIRE_FALSE(request_id.empty());
    CHECK(prepared->value("turnId", std::string()) == bypass_turn);
    CHECK(prepared->value("stepId", std::string()).empty() == false);

    // 4) sent → started → completed 单一终态链。
    int sent_count = 0;
    int started_count = 0;
    int completed_count = 0;
    int failed_count = 0;
    int cancelled_count = 0;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("requestId", std::string()) != request_id) continue;
        const std::string kind = line.value("kind", std::string());
        if (kind == "model.request.sent") ++sent_count;
        if (kind == "model.response.started") ++started_count;
        if (kind == "model.response.completed") ++completed_count;
        if (kind == "model.response.failed") ++failed_count;
        if (kind == "model.response.cancelled") ++cancelled_count;
    }
    CHECK(sent_count == 1);
    CHECK(started_count == 1);
    CHECK(completed_count == 1);
    CHECK(failed_count == 0);
    CHECK(cancelled_count == 0);

    // 5) assistant 成行:usage/finishReason/requestId 在账,display hidden。
    const nlohmann::json* assistant = nullptr;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.at("message").value("role", std::string()) != "assistant") continue;
        if (line.value("requestId", std::string()) == request_id) {
            assistant = &line;
            break;
        }
    }
    REQUIRE(assistant != nullptr);
    CHECK(assistant->value("purpose", std::string()) == "memory_extract");
    CHECK(assistant->value("display", std::string()) == "hidden");
    CHECK(assistant->value("turnId", std::string()) == bypass_turn);
    CHECK(assistant->value("parentTurnId", std::string()) == "turn-000001");
    CHECK(assistant->at("usage").value("inputTokens", std::int64_t{0}) == 812);
    CHECK(assistant->at("usage").value("outputTokens", std::int64_t{0}) == 96);
    CHECK(assistant->value("provider", std::string()) == "kimi");
    CHECK(assistant->value("model", std::string()) == "cheap-model");

    // 6) 链投影:旁路输入输出一个不进 conversation(主链只有根 system + 主回合 user)。
    auto read_back = trajectory::v3::ReadV3Ledger(*stream);
    REQUIRE(read_back.has_value());
    const auto context = trajectory::v3::ProjectModelContext(*read_back);
    CHECK(context.system_content == "你是 LubanCode,读写跑都走工具。");  // 抽取提示词没顶掉根
    REQUIRE(context.inputs.size() == 1);  // 只有主回合 user;旁路转写/assistant 一个不进
    for (const auto& input : context.inputs) {
        CHECK(input.purpose != trajectory::v3::MessagePurpose::MemoryExtract);
    }

    // 7) 验卷:哈希链收。
    CHECK(trajectory::v3::VerifyV3File(*stream).ok);
}

TEST_CASE("v3 旁路超时例: cancelled(internal_cancel) + usage appended,不伪造 assistant") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("timeout");
    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;

    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-000004", "external_user");

    auto bypass = ledger.NewBypassBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "host"},
                                         accounting::RequestPurpose::MemoryExtract);
    REQUIRE(bypass != nullptr);
    agent::SampleOptions options;
    options.boundary_recorder = bypass.get();
    options.purpose = accounting::RequestPurpose::MemoryExtract;
    options.timeout_secs = 1;  // 短预算,不按生产 45 秒等
    HangingBackend backend;
    const agent::SampleResult result = agent::SampleModel(backend, MemorySample(), options);
    CHECK_FALSE(result.ok);
    CHECK(result.error.kind == api::ErrorKind::Cancelled);
    CHECK(result.error.api_code == "local_deadline");
    CHECK(bypass->recent_errors().empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);

    const auto* prepared = FindLine(lines, "event", "kind", "model.request.prepared");
    REQUIRE(prepared != nullptr);
    CHECK(prepared->at("payload").value("timeoutBudgetSecs", 0) == 1);
    const std::string request_id = prepared->value("requestId", std::string());

    const auto* cancelled = FindLine(lines, "event", "kind", "model.response.cancelled");
    REQUIRE(cancelled != nullptr);
    CHECK(cancelled->value("requestId", std::string()) == request_id);
    CHECK(cancelled->value("status", std::string()) == "cancelled");
    CHECK(cancelled->at("payload").value("reason", std::string()) == "internal_cancel");
    CHECK(cancelled->at("payload").value("reason", std::string()).find("user_interrupt") ==
          std::string::npos);

    // 失败/取消也把 usage owner 账出了(appended):这里 provider 没报,
    // usage 键照现(null 不冒充 0)。
    const auto* appended = FindLine(lines, "event", "kind", "model.usage.appended");
    REQUIRE(appended != nullptr);
    CHECK(appended->value("requestId", std::string()) == request_id);
    CHECK(appended->at("payload").at("usage").is_null());
    CHECK_FALSE(appended->at("payload").value("reportedByProvider", true));

    // 不伪造 assistant:该请求名下没有 assistant 行。
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.at("message").value("role", std::string()) != "assistant") continue;
        CHECK(line.value("requestId", std::string()) != request_id);
    }
    // 单一终态:没有 completed/failed 抢位。
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("requestId", std::string()) != request_id) continue;
        const std::string kind = line.value("kind", std::string());
        CHECK(kind != "model.response.completed");
        CHECK(kind != "model.response.failed");
    }
    CHECK(trajectory::v3::VerifyV3File(*stream).ok);
}

TEST_CASE("MemoryTurnLedger 的 v3 写口: assessed 与 receipted 事实行") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("ledger");
    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;

    // 甲回合:抽取被叫了、超时收场——assessed 带 wall/失败码/预算关联。
    app::MemoryTurnLedger turns(&ledger);
    turns.BeginTurn(ledger.session_id(), "turn-000010", "修一下构建脚本");
    turns.NoteExtractionCalled();
    app::MemoryTurnLedger::ExtractOutcome outcome;
    outcome.ok = false;
    outcome.extract_wall_ms = 45123;
    outcome.error_code = "deadline_timeout";
    turns.NoteExtractionOutcome(outcome);
    turns.FinishTurn(/*foreground_tail_ms=*/88);

    // 乙回合:门就拦下——assessed 带 skip_reason;回合内顺手收一枚写路回执。
    turns.BeginTurn(ledger.session_id(), "turn-000011", "好的");
    turns.NoteExtractionSkipped(app::ExtractionSkipReason::AcknowledgementOnly);
    memory::MemoryWriteReceipt receipt;
    receipt.source = memory::MemoryWriteSource::AutoExtraction;
    receipt.operation = "save";
    receipt.outcome = memory::MemoryWriteReceiptOutcome::Queued;
    receipt.layer = "project";
    receipt.kind = "fact";
    receipt.job_id = "job-000001";
    turns.OnMemoryWriteReceipt(receipt);
    turns.FinishTurn(/*foreground_tail_ms=*/12);

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);

    const auto* first = FindLine(lines, "event", "kind", "memory.extraction.assessed");
    REQUIRE(first != nullptr);
    CHECK(first->value("turnId", std::string()) == "turn-000010");
    CHECK(first->at("payload").value("decision", std::string()) == "called");
    CHECK(first->at("payload").value("extractOutcome", std::string()) == "failed");
    CHECK(first->at("payload").value("errorCode", std::string()) == "deadline_timeout");
    CHECK(first->at("payload").value("extractWallMs", std::int64_t{0}) == 45123);
    CHECK(first->at("payload").value("foregroundTailMs", std::int64_t{0}) == 88);
    CHECK(first->at("payload").value("trigger", std::string()) == "every_turn");
    CHECK(first->at("payload").contains("userTextStats"));

    int assessed_count = 0;
    const nlohmann::json* second = nullptr;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("kind", std::string()) != "memory.extraction.assessed") continue;
        if (line.value("turnId", std::string()) == "turn-000011") second = &line;
        ++assessed_count;
    }
    CHECK(assessed_count == 2);
    REQUIRE(second != nullptr);
    CHECK(second->at("payload").value("decision", std::string()) == "skipped");
    CHECK(second->at("payload").value("skipReason", std::string()) == "acknowledgement_only");
    CHECK(second->at("payload").contains("extractOutcome") == false);

    const auto* receipted = FindLine(lines, "event", "kind", "memory.write.receipted");
    REQUIRE(receipted != nullptr);
    CHECK(receipted->value("turnId", std::string()) == "turn-000011");  // 回合开着收的回执带回合号
    CHECK(receipted->at("payload").value("operation", std::string()) == "save");
    CHECK(receipted->at("payload").value("outcome", std::string()) == "queued");
    CHECK(receipted->at("payload").value("jobId", std::string()) == "job-000001");

    CHECK(trajectory::v3::VerifyV3File(*stream).ok);
}

TEST_CASE("工厂门(§四清册): v3 场只有 memory_extract 接桥,其余维持 nullptr") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("gate");
    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    TrajectoryTurnBridge::Identity identity{"kimi", "responses", "host"};
    CHECK(ledger.NewBypassBridge(identity, accounting::RequestPurpose::MemoryExtract) != nullptr);
    // 起名:升旗人是精炼器自己,消息 purpose 的 system 白名单还没放行
    // session_title——不接,等后续单(§四清册在案)。
    CHECK(ledger.NewBypassBridge(identity, accounting::RequestPurpose::TitleRefine) == nullptr);
    // doctor 探针:v3 合同未铺,旧路照走。
    CHECK(ledger.NewBypassBridge(identity, accounting::RequestPurpose::DoctorProbe) == nullptr);
    // compact 一族:v3 有自己的全链运行时(RunV3Compact),不走旁路桥。
    CHECK(ledger.NewBypassBridge(identity, accounting::RequestPurpose::CompactMap) == nullptr);
    // 旧调用不传 purpose:默认 other_host_request,同样 nullptr。
    CHECK(ledger.NewBypassBridge(identity) == nullptr);
}

TEST_CASE("schema 往返: memory_extract purpose 与 memory.* kind 名字稳定") {
    CHECK(std::string(trajectory::v3::MessagePurposeName(trajectory::v3::MessagePurpose::MemoryExtract)) ==
          "memory_extract");
    CHECK(trajectory::v3::MessagePurposeFromName("memory_extract") ==
          trajectory::v3::MessagePurpose::MemoryExtract);
    CHECK_FALSE(trajectory::v3::MessagePurposeFromName("memory").has_value());
    CHECK(trajectory::v3::EventKindV3FromName("memory.extraction.assessed") ==
          trajectory::v3::EventKindV3::MemoryExtractionAssessed);
    CHECK(trajectory::v3::EventKindV3FromName("memory.write.receipted") ==
          trajectory::v3::EventKindV3::MemoryWriteReceipted);
    // statusless 事实行:不携带 status。
    CHECK_FALSE(trajectory::v3::RequiredStatusForKind(
                    trajectory::v3::EventKindV3::MemoryExtractionAssessed)
                    .has_value());
    CHECK_FALSE(
        trajectory::v3::RequiredStatusForKind(trajectory::v3::EventKindV3::MemoryWriteReceipted)
            .has_value());
    // system 白名单放行 memory_extract(旁路自带抽取提示词;直验结构体,
    // 信封发号归 writer,这里只钉语义校验)。
    {
        trajectory::v3::MessageLine line;
        line.message = nlohmann::json{{"role", "system"}, {"content", "抽取提示"}};
        line.purpose = trajectory::v3::MessagePurpose::MemoryExtract;
        line.system_meta = nlohmann::json{{"cause", "memory_extraction_prompt"}};
        CHECK_FALSE(trajectory::v3::ValidateMessageLine(line).has_value());
    }
}
