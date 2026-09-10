// v3 subagent 独立账测试(P1 其余,§4.31-4.33):五步交接、子目录子会话、
// 派生来源、检查点、observed、spawn.failed、父子账互证与独立 Continue。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-subagent-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "sessions" / "P1" / "P1.jsonl";
        std::filesystem::create_directories(jsonl.parent_path(), ec);
    }

    std::optional<V3Writer> Start() {
        auto writer = V3Writer::Start(jsonl, "P1", "run-parent", "你是父代理。",
                                      nlohmann::json::object(), V3WriterOptions{}, &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
    }
};

std::vector<nlohmann::json> ReadJson(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

}  // namespace

TEST_CASE("五步交接:requested → 子账就绪 → linked → 子账独立记账") {
    Harness harness("handshake");
    auto parent = harness.Start();
    REQUIRE(parent.has_value());

    // 步 1:父账预留(§4.32)。
    ChildSessionRef child_ref{"C1", "run-child", "subagents/C1/C1.jsonl"};
    ParentActionRef parent_ref{"P1", "run-parent", "turn-000001", "step-000001",
                               "action-000001", "msg-000002"};
    SubagentSpawn spawn = SubagentSpawn::Request(
        *parent, "action-000001", "turn-000001", "step-000001", parent->NewTaskId(),
        child_ref, parent_ref, nlohmann::json::object({{"goal", "调研"}}),
        nlohmann::json::object({{"model", "stub"}}));
    CHECK(spawn.task_id() == "task-000001");

    // 步 2:子目录 + 子账(首行 system 带派生来源 + 委派 user + task.started)。
    auto boot = spawn.BootstrapChild(*parent, "run-child", "你是子代理。", "去查三份资料。",
                                     Durability::PowerLoss);
    REQUIRE(boot.child_writer.has_value());
    CHECK(boot.checkpoint.session_id == "C1");
    CHECK(boot.checkpoint.seq >= 5);  // system+started+user+admit+task.started

    const std::filesystem::path child_jsonl = harness.dir / "sessions" / "P1" / "subagents" /
                                               "C1" / "C1.jsonl";
    REQUIRE(std::filesystem::exists(child_jsonl));

    // 步 3:父账 linked(引用子账检查点)。
    WriteReceipt linked = spawn.Link(*parent, boot.checkpoint);
    REQUIRE(linked.status == WriteReceipt::Status::Committed);

    // 步 4:子账独立记账 + 父账 observed(只存检查点)。
    EventDraft child_work;
    child_work.kind = EventKindV3::ModelRequestPrepared;
    child_work.turn_id = "turn-000001";
    child_work.step_id = "step-000001";
    child_work.request_id = "request-000001";
    child_work.payload = nlohmann::json::object(
        {{"purpose", "conversation"},
         {"contextId", "main"},
         {"contextRevision", boot.child_writer->context().revision},
         {"systemMessageRef", boot.child_writer->context().system_message_ref},
         {"inputMessageRefs", nlohmann::json::array()},
         {"readThroughSeq", boot.child_writer->next_seq() - 1},
         {"readThroughHash", boot.child_writer->last_line_hash()}});
    REQUIRE(boot.child_writer->AppendEvent(child_work).status == WriteReceipt::Status::Committed);
    ChildCheckpointRef later{boot.checkpoint.session_id, boot.checkpoint.run_id,
                              boot.child_writer->next_seq() - 1,
                              boot.child_writer->last_line_hash()};
    WriteReceipt observed = spawn.Observe(*parent, later,
                                          nlohmann::json::object({{"phase", "working"}}));
    REQUIRE(observed.status == WriteReceipt::Status::Committed);

    // 父账互证:requested/linked/observed 各就位。
    auto parent_lines = ReadJson(harness.jsonl);
    int handshake = 0;
    for (const auto& line : parent_lines) {
        const std::string kind = line.value("kind", "");
        if (kind == "subagent.spawn.requested") {
            ++handshake;
            CHECK(line["actionId"] == "action-000001");
            CHECK(line["taskId"] == "task-000001");
            CHECK(line["payload"]["childSessionRef"]["sessionId"] == "C1");
            CHECK(line["payload"]["attempt"] == 1);
            CHECK(line["payload"]["parentActionRef"]["declaredMessageRef"] == "msg-000002");
        }
        if (kind == "subagent.linked") {
            ++handshake;
            CHECK(line["status"] == "done");
            CHECK(line["payload"]["childCheckpointRef"]["seq"] == boot.checkpoint.seq);
        }
        if (kind == "subagent.observed") {
            ++handshake;
            CHECK(line["payload"]["childCheckpointRef"]["lineHash"] == later.line_hash);
        }
    }
    CHECK(handshake == 3);
    CHECK(VerifyV3File(harness.jsonl).ok);

    // 子账互证:首行 system(seq=1、turnId=null、systemMeta 带派生来源)。
    auto child_lines = ReadJson(child_jsonl);
    REQUIRE(!child_lines.empty());
    CHECK(child_lines[0]["type"] == "message");
    CHECK(child_lines[0]["message"]["role"] == "system");
    CHECK(child_lines[0]["seq"] == 1);
    CHECK(child_lines[0]["turnId"].is_null());
    CHECK(child_lines[0]["sessionId"] == "C1");
    CHECK(child_lines[0]["systemMeta"]["parentActionRef"]["actionId"] == "action-000001");
    CHECK(child_lines[0]["systemMeta"]["taskId"] == "task-000001");
    // spawnEventRef 五键齐全(§3.1 跨会话引用:sessionId/runId/seq/id/hash)。
    CHECK(child_lines[0]["systemMeta"]["spawnEventRef"]["id"].get<std::string>().rfind("evt-", 0) == 0);
    CHECK(child_lines[0]["systemMeta"]["spawnEventRef"]["sessionId"] == "P1");
    CHECK(child_lines[0]["systemMeta"]["spawnEventRef"]["seq"].is_number_unsigned());
    CHECK(VerifyV3File(child_jsonl).ok);

    // 委派任务:origin=parent_agent 的真实 user 输入(§4.31)。
    bool saw_delegation = false;
    bool saw_task_started = false;
    for (const auto& line : child_lines) {
        if (line.value("type", "") == "message" && line["message"]["role"] == "user") {
            saw_delegation = true;
            CHECK(line["origin"] == "parent_agent");
            CHECK(line["message"]["content"] == "去查三份资料。");
        }
        if (line.value("kind", "") == "task.started") {
            saw_task_started = true;
            CHECK(line["taskId"] == "task-000001");
            CHECK(line["payload"]["parentActionId"] == "action-000001");
        }
    }
    CHECK(saw_delegation);
    CHECK(saw_task_started);
}

TEST_CASE("子账独立 Continue:seq 自家口径,父账不动") {
    Harness harness("independent");
    auto parent = harness.Start();
    REQUIRE(parent.has_value());
    ChildSessionRef child_ref{"C1", "run-child", "subagents/C1/C1.jsonl"};
    ParentActionRef parent_ref{"P1", "run-parent", "turn-000001", "step-000001",
                               "action-000001", "msg-000002"};
    SubagentSpawn spawn = SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                                 "step-000001", "task-000001", child_ref,
                                                 parent_ref, nlohmann::json::object(),
                                                 nlohmann::json::object());
    auto boot = spawn.BootstrapChild(*parent, "run-child", "你是子代理。", "干活。",
                                     Durability::PowerLoss);
    REQUIRE(boot.child_writer.has_value());
    // linked 落稳后子执行器才开工(§4.32)。
    REQUIRE(spawn.Link(*parent, boot.checkpoint).status == WriteReceipt::Status::Committed);

    // 崩溃恢复:两本账各自 Continue,互不依赖(§4.31"每层完整 session 布局")。
    const std::uint64_t parent_seq = parent->next_seq();
    auto resumed_child = V3Writer::Continue(
        harness.dir / "sessions" / "P1" / "subagents" / "C1" / "C1.jsonl", V3WriterOptions{},
        &harness.clock);
    REQUIRE(resumed_child.has_value());
    CHECK(resumed_child->session_id() == "C1");
    CHECK(resumed_child->next_seq() == boot.child_writer->next_seq());
    CHECK(resumed_child->context().chain.size() == 2);  // system + 委派 user
    // 子账 Continue 不写父账:父卷行数不变。
    CHECK(parent->next_seq() == parent_seq);

    auto resumed_parent = V3Writer::Continue(harness.jsonl, V3WriterOptions{}, &harness.clock);
    REQUIRE(resumed_parent.has_value());
    CHECK(resumed_parent->session_id() == "P1");
}

TEST_CASE("初始化失败:父账 spawn.failed(phase/reason),不借父账续记") {
    Harness harness("spawn-failed");
    auto parent = harness.Start();
    REQUIRE(parent.has_value());
    ChildSessionRef child_ref{"C1", "run-child", "subagents/C1/C1.jsonl"};
    ParentActionRef parent_ref{"P1", "run-parent", "turn-000001", "step-000001",
                               "action-000001", "msg-000002"};
    SubagentSpawn spawn = SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                                 "step-000001", "task-000001", child_ref,
                                                 parent_ref, nlohmann::json::object(),
                                                 nlohmann::json::object());
    // 子目录已造好但子账开头失败的场景这里直接演练 Fail 收口(§4.32:
    // 初始化失败在父账记具体阶段,不回退成借父 logger 继续运行)。
    WriteReceipt failed = spawn.Fail(*parent, "child_first_line", "disk_unwritable");
    REQUIRE(failed.status == WriteReceipt::Status::Committed);
    auto lines = ReadJson(harness.jsonl);
    for (const auto& line : lines) {
        if (line.value("kind", "") == "subagent.spawn.failed") {
            CHECK(line["status"] == "failed");
            CHECK(line["taskId"] == "task-000001");
            CHECK(line["payload"]["phase"] == "child_first_line");
            CHECK(line["payload"]["reason"] == "disk_unwritable");
        }
    }
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("重复 spawn 撞目录:子账已存在则 Bootstrap 拒开(create-new)") {
    Harness harness("dir-clash");
    auto parent = harness.Start();
    REQUIRE(parent.has_value());
    ChildSessionRef child_ref{"C1", "run-child", "subagents/C1/C1.jsonl"};
    ParentActionRef parent_ref{"P1", "run-parent", "turn-000001", "step-000001",
                               "action-000001", "msg-000002"};
    SubagentSpawn spawn = SubagentSpawn::Request(*parent, "action-000001", "turn-000001",
                                                 "step-000001", "task-000001", child_ref,
                                                 parent_ref, nlohmann::json::object(),
                                                 nlohmann::json::object());
    auto first = spawn.BootstrapChild(*parent, "run-child", "你是子代理。", "第一回。",
                                      Durability::PowerLoss);
    REQUIRE(first.child_writer.has_value());
    first.child_writer.reset();  // 关写者(进程内模拟重启)

    // 同一预留重投:子账文件已存在 → 拒开,不覆盖(§4.32)。
    auto second = spawn.BootstrapChild(*parent, "run-child", "你是子代理。", "第二回。",
                                       Durability::PowerLoss);
    CHECK(!second.child_writer.has_value());
    CHECK(!second.error.empty());
}

TEST_CASE("嵌套:子账可再开 subagents/<C2>,每层完整布局") {
    Harness harness("nested");
    auto parent = harness.Start();
    REQUIRE(parent.has_value());
    ChildSessionRef c1{"C1", "run-c1", "subagents/C1/C1.jsonl"};
    SubagentSpawn spawn_c1 = SubagentSpawn::Request(
        *parent, "action-000001", "turn-000001", "step-000001", "task-000001", c1,
        ParentActionRef{"P1", "run-parent", "turn-000001", "step-000001", "action-000001",
                        "msg-000002"},
        nlohmann::json::object(), nlohmann::json::object());
    auto boot_c1 = spawn_c1.BootstrapChild(*parent, "run-c1", "你是子代理。", "干活。",
                                           Durability::PowerLoss);
    REQUIRE(boot_c1.child_writer.has_value());
    REQUIRE(spawn_c1.Link(*parent, boot_c1.checkpoint).status == WriteReceipt::Status::Committed);

    // C1 再派生 C2:目录在 C1 自己的 subagents/ 下(§4.31 递归布局)。
    ChildSessionRef c2{"C2", "run-c2", "subagents/C2/C2.jsonl"};
    SubagentSpawn spawn_c2 = SubagentSpawn::Request(
        *boot_c1.child_writer, "action-000002", "turn-000001", "step-000001", "task-000002",
        c2,
        ParentActionRef{"C1", "run-c1", "turn-000001", "step-000001", "action-000002",
                        "msg-000002"},
        nlohmann::json::object(), nlohmann::json::object());
    auto boot_c2 = spawn_c2.BootstrapChild(*boot_c1.child_writer, "run-c2", "你是孙代理。",
                                           "细分活。", Durability::PowerLoss);
    REQUIRE(boot_c2.child_writer.has_value());
    const std::filesystem::path c2_jsonl = harness.dir / "sessions" / "P1" / "subagents" /
                                            "C1" / "subagents" / "C2" / "C2.jsonl";
    REQUIRE(std::filesystem::exists(c2_jsonl));
    CHECK(VerifyV3File(c2_jsonl).ok);
    auto c2_lines = ReadJson(c2_jsonl);
    CHECK(c2_lines[0]["systemMeta"]["parentActionRef"]["sessionId"] == "C1");  // 直接父
    CHECK(c2_lines[0]["systemMeta"]["spawnEventRef"]["sessionId"] == "C1");
}
