// T06 / V3-GAP-01(SessionV3 旧设计清理单):离线 usage 读 v3 的读面册。
//
// ReadSessionUsage 的 v3 分派 + ProjectV3Usage(owner 驱动)逐项钉:
//   - 主账逐请求小账人工可复算(缺实报 null 不补 0);
//   - 两层子代理递归全收,父/子/孙 run_kind、purpose 各归各;
//   - 重复 spawn 同一子 session:树内去重,不重复计费;
//   - resume 源链祖先不混入本场(physical spend 按实际发生);
//   - 迟到实报(model.usage.appended)只作观察单列,不二次累计;
//   - compact 内部请求记 CompactReduce,Goal 验收 purpose unmapped 如实点名;
//   - 失败请求无 assistant:unknown sample + appended 观察覆盖缺口;
//   - 坏账 ok=false 不伪装零消耗;空目录走 v2 老路;格式冲突/异版本拒读。
// 合法账一律经 V3Writer 现场生成;坏形状在合法账上注入。
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "accounting/session_usage_reader.hpp"
#include "accounting/usage_projector.hpp"
#include "accounting/usage_sample.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/writer.hpp"

namespace v3 = lubancode::trajectory::v3;
using namespace lubancode::accounting;

namespace {

// 本册 format-neutral:直接 V3Writer/ReadV3Ledger,不经 SessionManager 建场
// 开关(与 test_v3_shared_fixtures 同类)。特别注意不许放进程级 EnvGuard——
// 全部测试编在同一只二进制里,全局改写环境变量会把其余册的 v2 老路全
// 掀翻(2026-09-12 首轮 CI 的教训)。

class FixedClock : public v3::V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct SessionsRoot {
    FixedClock clock;
    std::filesystem::path root;  // sessions/<id>/<id>.jsonl

    explicit SessionsRoot(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-v3-usage-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    std::filesystem::path Dir(const std::string& session_id) {
        std::error_code ec;
        const std::filesystem::path dir = root / session_id;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path Ledger(const std::string& session_id) {
        return Dir(session_id) / (session_id + ".jsonl");
    }
};

std::optional<v3::V3Writer> StartSession(SessionsRoot& root, const std::string& session_id,
                                         const std::string& system = "你是 LubanCode。") {
    auto writer = v3::V3Writer::Start(root.Ledger(session_id), session_id,
                                      "run-" + session_id, system, nlohmann::json::object(),
                                      v3::V3WriterOptions{}, &root.clock);
    if (!writer.has_value()) {
        return std::nullopt;
    }
    return std::move(*writer);
}

struct RoundResult {
    std::string request_id;
    std::string message_id;
};

// 一轮完整模型请求链:prepared → sent → completed → assistant(usage owner;
// usage 传 json(nullptr) = 缺实报不补 0)。内部回合(compact/goal)不进
// main 链,但请求身份与实报一样不少——各记本次真实请求。
RoundResult InstallRequest(v3::V3Writer& writer, const std::string& turn_id,
                           const nlohmann::json& usage,
                           v3::MessagePurpose purpose = v3::MessagePurpose::Conversation,
                           std::optional<std::string> compact_id = std::nullopt) {
    if (purpose == v3::MessagePurpose::Conversation) {
        v3::MessageDraft user;
        user.turn_id = turn_id;
        user.purpose = v3::MessagePurpose::Conversation;
        user.origin = v3::MessageOrigin::Human;
        user.message = nlohmann::json::object({{"role", "user"}, {"content", "问:" + turn_id}});
        v3::WriteReceipt user_receipt =
            writer.AppendMessage(std::move(user), v3::Durability::PowerLoss);
        REQUIRE(user_receipt.status == v3::WriteReceipt::Status::Committed);
        REQUIRE(writer.AdmitMessages({user_receipt.id}).status ==
                v3::WriteReceipt::Status::Committed);
    }
    std::vector<std::string> input_refs;
    // prepared:inputMessageRefs = 当前链上根之后的有序输入(生产桥同款)。
    const auto& chain = writer.context().chain;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        input_refs.push_back(chain[i].message_ref);
    }
    const std::string request_id = writer.NewRequestId();
    const std::string step_id = writer.NewStepId();
    nlohmann::json snapshot = nlohmann::json::object(
        {{"provider", "stub"}, {"wire", "openai"}, {"model", "stub-mini"}});
    REQUIRE(writer
                .PrepareRequest(request_id, turn_id, step_id, v3::MessagePurposeName(purpose),
                                writer.context().system_message_ref, input_refs,
                                std::move(snapshot), compact_id, v3::Durability::PowerLoss)
                .status == v3::WriteReceipt::Status::Committed);
    {
        v3::EventDraft sent;
        sent.kind = v3::EventKindV3::ModelRequestSent;
        sent.status = v3::OpStatus::Done;
        sent.request_id = request_id;
        sent.turn_id = turn_id;
        sent.step_id = step_id;
        sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
        REQUIRE(writer.AppendEvent(std::move(sent), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    {
        v3::EventDraft done;
        done.kind = v3::EventKindV3::ModelResponseCompleted;
        done.status = v3::OpStatus::Done;
        done.request_id = request_id;
        done.turn_id = turn_id;
        done.step_id = step_id;
        done.payload = nlohmann::json{{"finishReason", "stop"}};
        REQUIRE(writer.AppendEvent(std::move(done), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    v3::MessageDraft assistant;
    assistant.turn_id = turn_id;
    assistant.step_id = step_id;
    assistant.request_id = request_id;
    assistant.compact_id = compact_id;
    assistant.purpose = purpose;
    assistant.origin = v3::MessageOrigin::SessionRuntime;
    assistant.provider = "stub";
    assistant.wire = "openai";
    assistant.model = "stub-mini";
    assistant.response_model = nlohmann::json(nullptr);
    assistant.usage = usage;
    assistant.message = nlohmann::json::object(
        {{"role", "assistant"}, {"content", "答:" + turn_id}});
    v3::WriteReceipt receipt =
        writer.AppendMessage(std::move(assistant), v3::Durability::PowerLoss);
    REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
    if (purpose == v3::MessagePurpose::Conversation) {
        REQUIRE(writer.AdmitMessages({receipt.id}).status == v3::WriteReceipt::Status::Committed);
    }
    return RoundResult{request_id, receipt.id};
}

// 迟到实报观察(§五 owner 表:不倒改旧 message,不参与累计)。
void AppendLateUsage(v3::V3Writer& writer, const std::string& request_id,
                     const std::string& turn_id, std::int64_t in, std::int64_t out) {
    v3::EventDraft appended;
    appended.kind = v3::EventKindV3::ModelUsageAppended;
    appended.request_id = request_id;
    appended.turn_id = turn_id;
    appended.payload = nlohmann::json{{"usage", nlohmann::json{{"inputTokens", in},
                                                               {"outputTokens", out}}},
                                      {"reportedByProvider", true},
                                      {"providerResponseId", "resp-late"}};
    REQUIRE(writer.AppendEvent(std::move(appended), v3::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << bytes;
}

const UsageSample* FindSample(const SessionUsageRead& read, const std::string& request_id) {
    for (const auto& sample : read.samples) {
        if (sample.request_id == request_id) {
            return &sample;
        }
    }
    return nullptr;
}

bool HasWarning(const SessionUsageRead& read, const std::string& needle) {
    for (const auto& warning : read.warnings) {
        if (warning.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 主账:逐请求小账可复算;缺实报 null 不补 0
// ---------------------------------------------------------------------------

TEST_CASE("v3 主账:两轮请求逐笔对账,缺实报的照投 unknown") {
    SessionsRoot root("main");
    {
        auto writer = StartSession(root, "S-MAIN");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001",
                      nlohmann::json{{"inputTokens", 120}, {"outputTokens", 30}});
        InstallRequest(*writer, "turn-000002", nlohmann::json(nullptr));
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-MAIN"));
    REQUIRE(read.ok);
    CHECK(read.format == "v3");
    CHECK(read.samples.size() == 2);
    // 第一笔:实报 120/30,完整输入=120,口径 TotalInputTokens。
    REQUIRE(read.samples[0].usage.has_value());
    CHECK(read.samples[0].usage_source == UsageSource::ProviderReported);
    CHECK(read.samples[0].total_input_tokens == 120);
    CHECK(read.samples[0].total_billed_shape_tokens == 150);
    CHECK(read.samples[0].purpose == RequestPurpose::MainTurn);
    CHECK(read.samples[0].run_kind == "main_session");
    CHECK(read.samples[0].request_outcome == "completed");
    CHECK(read.samples[0].provider == "stub");
    CHECK(read.samples[0].model == "stub-mini");
    CHECK(read.samples[0].source_event.has_value());
    CHECK_FALSE(read.samples[0].source_event->event_id.empty());
    // 第二笔:缺实报——unknown,不带 token 字段,不折 0。
    CHECK_FALSE(read.samples[1].usage.has_value());
    CHECK(read.samples[1].usage_source == UsageSource::Unknown);
    CHECK(read.samples[1].total_input_tokens == 0);
}

TEST_CASE("v3 主账:真零(开场未问)与坏账(验卷不过)分开,不伪装零消耗") {
    // 真零:账合法、一条请求没有——ok=true、空样本、format=v3。
    {
        SessionsRoot root("zero");
        auto writer = StartSession(root, "S-ZERO");
        REQUIRE(writer.has_value());
        const SessionUsageRead read = ReadSessionUsage(root.Dir("S-ZERO"));
        REQUIRE(read.ok);
        CHECK(read.format == "v3");
        CHECK(read.samples.empty());
    }
    // 坏账:合法账截尾(崩溃形状)——ok=false,不拿空样本冒充零消耗。
    {
        SessionsRoot root("bad");
        {
            auto writer = StartSession(root, "S-BAD");
            REQUIRE(writer.has_value());
            InstallRequest(*writer, "turn-000001",
                           nlohmann::json{{"inputTokens", 100}, {"outputTokens", 20}});
        }
        const std::string good = ReadFileBytes(root.Ledger("S-BAD"));
        REQUIRE(!good.empty());
        WriteFileBytes(root.Ledger("S-BAD"), good.substr(0, good.size() - 5));
        const SessionUsageRead read = ReadSessionUsage(root.Dir("S-BAD"));
        CHECK_FALSE(read.ok);
        CHECK(read.error_code == "usage.v3_ledger_unreadable");
        CHECK(read.samples.empty());
    }
}

// ---------------------------------------------------------------------------
// 子 session:递归全收 / 重复 spawn 去重 / resume 源不混入
// ---------------------------------------------------------------------------

TEST_CASE("两层子代理:父子孙三账全收,purpose/run_kind 各归各") {
    SessionsRoot root("subtree");
    {
        auto parent = StartSession(root, "S-PARENT");
        REQUIRE(parent.has_value());
        InstallRequest(*parent, "turn-000001",
                       nlohmann::json{{"inputTokens", 100}, {"outputTokens", 20}});

        const v3::ChildSessionRef child_ref{"S-CHILD", "run-S-CHILD",
                                            "subagents/S-CHILD/S-CHILD.jsonl"};
        const v3::ParentActionRef parent_action{"S-PARENT", "run-S-PARENT", "turn-000001",
                                                "step-000001", "action-000001", "msg-000001"};
        auto spawn = v3::SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                                "step-000001", "task-000001", child_ref,
                                                parent_action, nlohmann::json::object(),
                                                nlohmann::json::object());
        auto boot = spawn.BootstrapChild(*parent, "run-S-CHILD", "你是子代理一层。", "查一层。");
        REQUIRE(boot.child_writer.has_value());
        REQUIRE(spawn.Link(*parent, boot.checkpoint).status == v3::WriteReceipt::Status::Committed);
        InstallRequest(*boot.child_writer, "turn-000001",
                       nlohmann::json{{"inputTokens", 200}, {"outputTokens", 40}});

        const v3::ChildSessionRef grandchild_ref{"S-GRAND", "run-S-GRAND",
                                                 "subagents/S-GRAND/S-GRAND.jsonl"};
        const v3::ParentActionRef child_action{"S-CHILD", "run-S-CHILD", "turn-000001",
                                               "step-000001", "action-000002", "msg-000002"};
        auto spawn2 = v3::SubagentSpawn::Request(*boot.child_writer, "action-000002",
                                                 "turn-000001", "step-000001", "task-000002",
                                                 grandchild_ref, child_action,
                                                 nlohmann::json::object(),
                                                 nlohmann::json::object());
        auto boot2 = spawn2.BootstrapChild(*boot.child_writer, "run-S-GRAND",
                                           "你是子代理二层。", "再查一层。");
        REQUIRE(boot2.child_writer.has_value());
        REQUIRE(spawn2.Link(*boot.child_writer, boot2.checkpoint).status ==
                v3::WriteReceipt::Status::Committed);
        InstallRequest(*boot2.child_writer, "turn-000001",
                       nlohmann::json{{"inputTokens", 300}, {"outputTokens", 60}});
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-PARENT"));
    REQUIRE(read.ok);
    REQUIRE(read.samples.size() == 3);
    std::int64_t total_input = 0;
    std::int64_t subagent_turns = 0;
    std::int64_t main_turns = 0;
    for (const auto& sample : read.samples) {
        REQUIRE(sample.usage.has_value());
        total_input += sample.total_input_tokens;
        if (sample.run_kind == "subagent") {
            CHECK(sample.purpose == RequestPurpose::SubagentTurn);
            subagent_turns += 1;
        } else if (sample.run_kind == "main_session") {
            CHECK(sample.purpose == RequestPurpose::MainTurn);
            main_turns += 1;
        }
    }
    // 人工可复算:100 + 200 + 300,一个不多一个不少。
    CHECK(total_input == 600);
    CHECK(subagent_turns == 2);
    CHECK(main_turns == 1);
}

TEST_CASE("重复 spawn 同一子 session:树内去重,子账只计一遍") {
    SessionsRoot root("cycle");
    {
        auto parent = StartSession(root, "S-PARENT2");
        REQUIRE(parent.has_value());
        InstallRequest(*parent, "turn-000001",
                       nlohmann::json{{"inputTokens", 50}, {"outputTokens", 10}});
        const v3::ChildSessionRef child_ref{"S-DUP", "run-S-DUP",
                                            "subagents/S-DUP/S-DUP.jsonl"};
        const v3::ParentActionRef parent_action{"S-PARENT2", "run-S-PARENT2", "turn-000001",
                                                "step-000001", "action-000001", "msg-000001"};
        // 第一次 spawn:真起子账并 link。
        auto spawn = v3::SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                                "step-000001", "task-000001", child_ref,
                                                parent_action, nlohmann::json::object(),
                                                nlohmann::json::object());
        auto boot = spawn.BootstrapChild(*parent, "run-S-DUP", "你是孩子。", "去查。");
        REQUIRE(boot.child_writer.has_value());
        REQUIRE(spawn.Link(*parent, boot.checkpoint).status == v3::WriteReceipt::Status::Committed);
        InstallRequest(*boot.child_writer, "turn-000001",
                       nlohmann::json{{"inputTokens", 80}, {"outputTokens", 8}});
        // 第二次 spawn 同一 child(sessionId 相同):重复派发形状。
        auto spawn2 = v3::SubagentSpawn::Request(*parent, "action-000002", "turn-000001",
                                                 "step-000001", "task-000002", child_ref,
                                                 parent_action, nlohmann::json::object(),
                                                 nlohmann::json::object());
        (void)spawn2;
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-PARENT2"));
    REQUIRE(read.ok);
    // 父一笔 + 子账只计一遍 = 2;不因重复 spawn 翻倍。
    REQUIRE(read.samples.size() == 2);
    std::int64_t total_input = 0;
    for (const auto& sample : read.samples) {
        REQUIRE(sample.usage.has_value());
        total_input += sample.total_input_tokens;
    }
    CHECK(total_input == 50 + 80);
    CHECK(HasWarning(read, "usage.v3_subsession_cycle_dedup: S-DUP"));
}

TEST_CASE("resume 来源链:本场只计本场请求,祖先不重复计费") {
    SessionsRoot root("resume");
    {
        auto source = StartSession(root, "S-OLDEST2");
        REQUIRE(source.has_value());
        InstallRequest(*source, "turn-000001",
                       nlohmann::json{{"inputTokens", 111}, {"outputTokens", 11}});
    }
    {
        const auto source_ledger = v3::ReadV3Ledger(root.Ledger("S-OLDEST2"));
        REQUIRE(source_ledger.has_value());
        auto resumee = StartSession(root, "S-NEWEST2");
        REQUIRE(resumee.has_value());
        // resume.source.attached:五键指源末行。
        const auto last = source_ledger->LastEntry();
        REQUIRE(last.has_value());
        const std::string id =
            last->is_message
                ? source_ledger->messages[last->index].message_id
                : source_ledger->events[last->index].event_id;
        const std::string hash =
            last->is_message ? source_ledger->messages[last->index].line_hash
                             : source_ledger->events[last->index].line_hash;
        v3::EventDraft attached;
        attached.kind = v3::EventKindV3::ResumeSourceAttached;
        attached.payload = nlohmann::json::object(
            {{"sourceRef", nlohmann::json::object({{"sessionId", source_ledger->session_id},
                                                   {"runId", source_ledger->run_id},
                                                   {"seq", last->seq},
                                                   {"id", id},
                                                   {"hash", hash}})},
             {"contextRevision", source_ledger->context.revision},
             {"systemMessageRef", source_ledger->context.system_message_ref},
             {"branch", "main"}});
        REQUIRE(resumee
                    ->AppendEvent(std::move(attached), v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        InstallRequest(*resumee, "turn-000001",
                       nlohmann::json{{"inputTokens", 222}, {"outputTokens", 22}});
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-NEWEST2"));
    REQUIRE(read.ok);
    REQUIRE(read.samples.size() == 1);
    REQUIRE(read.samples[0].usage.has_value());
    CHECK(read.samples[0].total_input_tokens == 222);
    CHECK(read.samples[0].session_id == "S-NEWEST2");
}

// ---------------------------------------------------------------------------
// 观察与内部请求:appended / compact / goal / 失败请求
// ---------------------------------------------------------------------------

TEST_CASE("迟到实报:appended 单列观察,owner 仍缺实报不累计") {
    SessionsRoot root("late");
    {
        auto writer = StartSession(root, "S-LATE2");
        REQUIRE(writer.has_value());
        const auto round =
            InstallRequest(*writer, "turn-000001", nlohmann::json(nullptr));
        AppendLateUsage(*writer, round.request_id, "turn-000001", 130, 35);
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-LATE2"));
    REQUIRE(read.ok);
    REQUIRE(read.samples.size() == 1);
    CHECK_FALSE(read.samples[0].usage.has_value());  // owner 未倒改
    CHECK(read.samples[0].usage_source == UsageSource::Unknown);
    CHECK(read.samples[0].total_input_tokens == 0);
    CHECK(HasWarning(read, "usage.v3_appended_observed"));
    CHECK(HasWarning(read, "in=130"));
}

TEST_CASE("compact 内部请求记 CompactReduce,Goal 验收 unmapped 如实点名") {
    SessionsRoot root("internal");
    std::string compact_request;
    std::string goal_request;
    {
        auto writer = StartSession(root, "S-INT");
        REQUIRE(writer.has_value());
        const auto compact_round =
            InstallRequest(*writer, "turn-compact", nlohmann::json{{"inputTokens", 900},
                                                                    {"outputTokens", 90}},
                           v3::MessagePurpose::Compact, "compact-000001");
        compact_request = compact_round.request_id;
        const auto goal_round =
            InstallRequest(*writer, "turn-goal", nlohmann::json{{"inputTokens", 400},
                                                                 {"outputTokens", 40}},
                           v3::MessagePurpose::GoalEvaluation);
        goal_request = goal_round.request_id;
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-INT"));
    REQUIRE(read.ok);
    REQUIRE(read.samples.size() == 2);
    const auto* compact_sample = FindSample(read, compact_request);
    REQUIRE(compact_sample != nullptr);
    CHECK(compact_sample->purpose == RequestPurpose::CompactReduce);
    CHECK(compact_sample->usage.has_value());
    CHECK(compact_sample->total_input_tokens == 900);
    const auto* goal_sample = FindSample(read, goal_request);
    REQUIRE(goal_sample != nullptr);
    CHECK_FALSE(goal_sample->purpose.has_value());  // RequestPurpose 无对应,不装懂
    CHECK(goal_sample->usage.has_value());          // 请求与实报照记,不漏
    CHECK(goal_sample->total_input_tokens == 400);
    CHECK(HasWarning(read, "usage.v3_purpose_unmapped: goal_evaluation"));
}

TEST_CASE("失败请求无 assistant:unknown sample + appended 观察覆盖缺口") {
    SessionsRoot root("failed");
    std::string request_id;
    {
        auto writer = StartSession(root, "S-FAIL");
        REQUIRE(writer.has_value());
        request_id = writer->NewRequestId();
        const std::string step_id = writer->NewStepId();
        REQUIRE(writer
                    ->PrepareRequest(request_id, "turn-000001", step_id, "conversation",
                                     writer->context().system_message_ref, {},
                                     nlohmann::json::object({{"provider", "stub"},
                                                             {"wire", "openai"},
                                                             {"model", "stub-mini"}}),
                                     std::nullopt, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        v3::EventDraft sent;
        sent.kind = v3::EventKindV3::ModelRequestSent;
        sent.status = v3::OpStatus::Done;
        sent.request_id = request_id;
        sent.turn_id = "turn-000001";
        sent.step_id = step_id;
        sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
        REQUIRE(writer->AppendEvent(std::move(sent), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        v3::EventDraft failed;
        failed.kind = v3::EventKindV3::ModelResponseFailed;
        failed.status = v3::OpStatus::Failed;
        failed.request_id = request_id;
        failed.turn_id = "turn-000001";
        failed.step_id = step_id;
        failed.payload = nlohmann::json{{"error", "provider 5xx"}};
        REQUIRE(writer->AppendEvent(std::move(failed), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        // 错误请求的观察用量:单列,不升级成 owner。
        AppendLateUsage(*writer, request_id, "turn-000001", 70, 0);
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-FAIL"));
    REQUIRE(read.ok);
    REQUIRE(read.samples.size() == 1);
    CHECK(read.samples[0].request_id == request_id);
    CHECK(read.samples[0].request_outcome == "failed");
    CHECK_FALSE(read.samples[0].usage.has_value());
    CHECK(read.samples[0].usage_source == UsageSource::Unknown);
    CHECK(HasWarning(read, "usage.v3_appended_observed"));
    CHECK_FALSE(HasWarning(read, "usage.v3_owner_missing"));  // 失败无消息是正常形状
}

TEST_CASE("prepared 未发出的请求不计,completed 而消息丢失点名") {
    SessionsRoot root("nosend");
    {
        auto writer = StartSession(root, "S-NOSEND");
        REQUIRE(writer.has_value());
        // 只 prepared、从未 sent:physical spend 不虚计。
        REQUIRE(writer
                    ->PrepareRequest(writer->NewRequestId(), "turn-000001", writer->NewStepId(),
                                     "conversation", writer->context().system_message_ref, {},
                                     nlohmann::json::object({{"provider", "stub"},
                                                             {"wire", "openai"},
                                                             {"model", "stub-mini"}}),
                                     std::nullopt, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        // sent + completed 但 assistant 没落盘(消息丢失形状)。
        const std::string request_id = writer->NewRequestId();
        const std::string step_id = writer->NewStepId();
        REQUIRE(writer
                    ->PrepareRequest(request_id, "turn-000001", step_id, "conversation",
                                     writer->context().system_message_ref, {},
                                     nlohmann::json::object({{"provider", "stub"},
                                                             {"wire", "openai"},
                                                             {"model", "stub-mini"}}),
                                     std::nullopt, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        v3::EventDraft sent;
        sent.kind = v3::EventKindV3::ModelRequestSent;
        sent.status = v3::OpStatus::Done;
        sent.request_id = request_id;
        sent.turn_id = "turn-000001";
        sent.step_id = step_id;
        sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
        REQUIRE(writer->AppendEvent(std::move(sent), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        v3::EventDraft done;
        done.kind = v3::EventKindV3::ModelResponseCompleted;
        done.status = v3::OpStatus::Done;
        done.request_id = request_id;
        done.turn_id = "turn-000001";
        done.step_id = step_id;
        done.payload = nlohmann::json{{"finishReason", "stop"}};
        REQUIRE(writer->AppendEvent(std::move(done), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    const SessionUsageRead read = ReadSessionUsage(root.Dir("S-NOSEND"));
    REQUIRE(read.ok);
    // 未发出的不计;completed 无 owner 投 unknown + 点名(coverage 靠它数)。
    REQUIRE(read.samples.size() == 1);
    CHECK_FALSE(read.samples[0].usage.has_value());
    CHECK(read.samples[0].request_outcome == "completed");
    CHECK(HasWarning(read, "usage.v3_owner_missing"));
}

// ---------------------------------------------------------------------------
// 格式分派:v2 老路 / 冲突 / 异版本
// ---------------------------------------------------------------------------

TEST_CASE("格式分派:空目录走 v2 老路;冲突与异版本拒读") {
    // 光杆目录:既无 main.jsonl 也无 <id>.jsonl——v2 老路零请求(合法空账)。
    {
        SessionsRoot root("empty");
        root.Dir("S-EMPTY");
        const SessionUsageRead read = ReadSessionUsage(root.Dir("S-EMPTY"));
        REQUIRE(read.ok);
        CHECK(read.format == "v2");
        CHECK(read.samples.empty());
        CHECK(HasWarning(read, "usage.session_manifest_missing"));
    }
    // 两种主账并存:冲突拒读。
    {
        SessionsRoot root("conflict");
        {
            auto writer = StartSession(root, "S-DUAL");
            REQUIRE(writer.has_value());
        }
        WriteFileBytes(root.Dir("S-DUAL") / "main.jsonl", "{}\n");
        const SessionUsageRead read = ReadSessionUsage(root.Dir("S-DUAL"));
        CHECK_FALSE(read.ok);
        CHECK(read.error_code == "usage.session_format_conflict");
        CHECK(read.samples.empty());
    }
    // <id>.jsonl 首行 schemaVersion != 3:unsupported,不伪装成空会话。
    {
        SessionsRoot root("foreign");
        WriteFileBytes(root.Dir("S-FOREIGN") / "S-FOREIGN.jsonl",
                       "{\"schemaVersion\":2,\"type\":\"event\"}\n");
        const SessionUsageRead read = ReadSessionUsage(root.Dir("S-FOREIGN"));
        CHECK_FALSE(read.ok);
        CHECK(read.error_code == "usage.session_format_unsupported");
        CHECK(read.samples.empty());
    }
    // 首行读不出(坏 JSON):unreadable。
    {
        SessionsRoot root("badfirst");
        WriteFileBytes(root.Dir("S-BADFIRST") / "S-BADFIRST.jsonl", "not-json\n");
        const SessionUsageRead read = ReadSessionUsage(root.Dir("S-BADFIRST"));
        CHECK_FALSE(read.ok);
        CHECK(read.error_code == "usage.session_format_unreadable");
    }
}
