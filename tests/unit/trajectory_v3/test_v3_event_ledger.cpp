// v3 事件账 writer profile 册(Workflow 接入 v3 第一棒):非 agent owner
// 的受校验事件账——只写 event 行、首行即开账事件、共用信封/seq/哈希链。
// 与 agent 会话账(V3Writer)的分界钉死:system/message 在这个 profile
// 里结构性写不进去。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "trajectory/v3/event_ledger.hpp"

namespace {

namespace fs = std::filesystem;

using namespace lubancode::trajectory::v3;

fs::path TempFile(const char* tag) {
    static int counter = 0;
    ++counter;
    return fs::temp_directory_path() /
           ("lubancode_v3_event_ledger_" + std::string(tag) + "_" + std::to_string(counter) +
            ".jsonl");
}

std::vector<std::string> ReadLines(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

EventDraft DefinitionLoadedDraft() {
    EventDraft draft;
    draft.kind = EventKindV3::WorkflowDefinitionLoaded;
    draft.payload = nlohmann::json{{"workflowId", std::string("demo-flow")},
                                   {"definitionHash", std::string(64, 'a')},
                                   {"definitionRef", "definition.json"}};
    return draft;
}

}  // namespace

TEST_CASE("开卷:首行即事件,create-new,不造 system") {
    const fs::path path = TempFile("start");
    std::error_code ec;
    fs::remove(path, ec);
    auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
    REQUIRE(ledger.has_value());
    const auto lines = ReadLines(path);
    REQUIRE(lines.size() == 1);
    const nlohmann::json first = nlohmann::json::parse(lines[0]);
    CHECK(first.at("type").get<std::string>() == "event");
    CHECK(first.at("kind").get<std::string>() == "workflow.definition.loaded");
    CHECK(first.at("sessionId").get<std::string>() == "run-1");
    CHECK(first.at("runId").get<std::string>() == "seg-1");
    CHECK(first.at("seq").get<std::uint64_t>() == 1);
    CHECK(first.at("prevHash").get<std::string>() == kGenesisHash);

    SUBCASE("已存在即拒开(单写者,create-new)") {
        auto again = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
        CHECK_FALSE(again.has_value());
    }
    fs::remove(path, ec);
}

TEST_CASE("追加与哈希链:seq 连续、事件账验卷全绿") {
    const fs::path path = TempFile("chain");
    std::error_code ec;
    fs::remove(path, ec);
    auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
    REQUIRE(ledger.has_value());
    EventDraft reserved;
    reserved.kind = EventKindV3::WorkflowNodeReserved;
    reserved.payload = nlohmann::json{{"nodeId", std::string("a")},
                                      {"nodeExecutionId", std::string("run-1-a-d1")},
                                      {"nodeKind", std::string("template")},
                                      {"attempt", 1},
                                      {"inputHash", std::string(64, 'b')}};
    const auto receipt = ledger->Append(reserved, Durability::PowerLoss);
    REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    CHECK(receipt.seq == 2);

    const auto report = VerifyV3EventLedgerFile(path);
    CHECK(report.ok);
    CHECK(report.lines == 2);
    CHECK(report.last_seq == 2);
    CHECK(report.session_id == "run-1");
    CHECK(report.run_id == "seg-1");
    fs::remove(path, ec);
}

TEST_CASE("续卷:验链后可写、发号续号不撞") {
    const fs::path path = TempFile("continue");
    std::error_code ec;
    fs::remove(path, ec);
    {
        auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
        REQUIRE(ledger.has_value());
        (void)ledger->NextId("evt");
        (void)ledger->NextId("evt");
    }
    auto resumed = V3EventLedger::Continue(path);
    REQUIRE(resumed.has_value());
    CHECK(resumed->last_seq() == 1);
    CHECK(resumed->next_seq() == 2);
    // evt-000001/000002 已在卷内,续号从 000003 起。
    CHECK(resumed->NextId("evt") == "evt-000003");
    EventDraft skipped;
    skipped.kind = EventKindV3::WorkflowNodeSkipped;
    skipped.payload = nlohmann::json{{"nodeId", std::string("x")}, {"reason", std::string("r")}};
    const auto receipt = resumed->Append(skipped);
    CHECK(receipt.status == WriteReceipt::Status::Committed);
    CHECK(receipt.seq == 2);
    CHECK(VerifyV3EventLedgerFile(path).ok);
    fs::remove(path, ec);
}

TEST_CASE("事件账 profile 拒收 message 行:agent 会话账不认这个门") {
    const fs::path path = TempFile("message");
    std::error_code ec;
    fs::remove(path, ec);
    {
        auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
        REQUIRE(ledger.has_value());
    }
    // 手工塞一行合法的 v3 message(体系生成的 system)到卷尾。
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        nlohmann::json line = nlohmann::json::object();
        line["type"] = "message";
        line["schemaVersion"] = 3;
        line["sessionId"] = "run-1";
        line["runId"] = "seg-1";
        line["seq"] = 2;
        line["timestamp"] = "2026-09-11T00:00:00.000Z";
        line["messageId"] = "msg-000001";
        line["turnId"] = nullptr;
        line["purpose"] = "conversation";
        line["origin"] = "session_runtime";
        line["message"] = nlohmann::json{{"role", "system"}, {"content", "x"}};
        file << line.dump() << "\n";
    }
    const auto report = VerifyV3EventLedgerFile(path);
    CHECK_FALSE(report.ok);
    CHECK(report.error_code == "v3ledger.message_line_rejected");
    CHECK_FALSE(V3EventLedger::Continue(path).has_value());
    fs::remove(path, ec);
}

TEST_CASE("尾行截断明报:写入侧不偷偷裁") {
    const fs::path path = TempFile("trunc");
    std::error_code ec;
    fs::remove(path, ec);
    {
        auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
        REQUIRE(ledger.has_value());
    }
    // 抹掉末行换行,模拟崩溃截断。
    std::string data;
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        data = buffer.str();
    }
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) data.pop_back();
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << data;
    }
    const auto report = VerifyV3EventLedgerFile(path);
    CHECK_FALSE(report.ok);
    CHECK(report.truncated_tail);
    CHECK(report.error_code == "v3ledger.truncated_tail");
    CHECK_FALSE(V3EventLedger::Continue(path).has_value());
    fs::remove(path, ec);
}

TEST_CASE("注入提交失败:句柄断,后续提交空收") {
    const fs::path path = TempFile("iofail");
    std::error_code ec;
    fs::remove(path, ec);
    V3EventLedgerOptions options;
    options.inject_io_failure = [] { return std::optional<std::string>("test.injected"); };
    auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft(), options);
    // 开账事件就要注入失败:不留 0 字节正式账。
    CHECK_FALSE(ledger.has_value());
    CHECK_FALSE(fs::exists(path, ec));

    // 开账成功后断在半路:首事件过,第二枚注入失败,此后 broken。
    V3EventLedgerOptions fail_after_first;
    int calls = 0;
    fail_after_first.inject_io_failure = [&calls] {
        ++calls;
        return calls >= 2 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    auto ledger2 = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft(),
                                        fail_after_first);
    REQUIRE(ledger2.has_value());
    CHECK(ledger2->broken() == false);
    EventDraft skipped;
    skipped.kind = EventKindV3::WorkflowNodeSkipped;
    skipped.payload = nlohmann::json{{"nodeId", std::string("x")}, {"reason", std::string("r")}};
    const auto rejected = ledger2->Append(skipped);
    CHECK(rejected.status == WriteReceipt::Status::IoFailed);
    CHECK(ledger2->broken());
    const auto after = ledger2->Append(skipped);
    CHECK(after.status == WriteReceipt::Status::Rejected);
    CHECK(after.error_code == "v3ledger.broken");
    fs::remove(path, ec);
}

TEST_CASE("载荷合同随 schema3 把关:workflow kind 的必带字段") {
    const fs::path path = TempFile("schema");
    std::error_code ec;
    fs::remove(path, ec);
    auto ledger = V3EventLedger::Start(path, "run-1", "seg-1", DefinitionLoadedDraft());
    REQUIRE(ledger.has_value());
    // 缺 inputHash 的 reserve 事件:校验层拒(Rejected,不算 io 失败)。
    EventDraft bad;
    bad.kind = EventKindV3::WorkflowNodeReserved;
    bad.payload = nlohmann::json{{"nodeId", std::string("a")},
                                 {"nodeExecutionId", std::string("run-1-a-d1")},
                                 {"nodeKind", std::string("template")},
                                 {"attempt", 1}};
    const auto receipt = ledger->Append(bad);
    CHECK(receipt.status == WriteReceipt::Status::Rejected);
    CHECK(receipt.error_code == "schema3.bad_type");
    CHECK_FALSE(ledger->broken());  // 校验拒绝不断账,只是这行写不进
    fs::remove(path, ec);
}
