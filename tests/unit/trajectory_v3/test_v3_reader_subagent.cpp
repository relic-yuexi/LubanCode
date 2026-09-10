// v3 读取侧父子账测试(P2,§4.31-4.33):跨会话五键引用验 hash
//(§4.2"不存裸 seq");subagents/ 递归遍历、子账缺失标缺口、环不下钻。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::create_directories(to.parent_path());
    std::ifstream input(from, std::ios::binary);
    std::ofstream output(to, std::ios::binary);
    output << input.rdbuf();
}

}  // namespace

TEST_CASE("跨会话五键:验得过、错各归各(§3.1/§4.2)") {
    auto parent = ReadV3Ledger(Fixture("subagent_parent.jsonl"));
    REQUIRE(parent.has_value());
    // 子账首行 systemMeta.spawnEventRef 是合用的五键(指向父账 seq10)。
    CrossSessionRef good;
    good.session_id = parent->session_id;
    good.run_id = parent->run_id;
    good.seq = 10;
    good.id = "evt-000007";
    good.hash = parent->events[9].line_hash;
    CrossSessionRefCheck check = VerifyCrossSessionRef(good, *parent);
    CHECK(check.ok);

    struct Case {
        const char* name;
        CrossSessionRef ref;
        const char* expect_reason;
    };
    CrossSessionRef bad = good;
    bad.hash = std::string(64, 'f');
    CrossSessionRef wrong_seq = good;
    wrong_seq.seq = 999;
    CrossSessionRef wrong_id = good;
    wrong_id.id = "evt-000999";
    CrossSessionRef wrong_session = good;
    wrong_session.session_id = "20260910-170000-OTHER";
    CrossSessionRef wrong_run = good;
    wrong_run.run_id = "run-other";
    for (const Case& item : {Case{"hash", bad, "hash_mismatch"},
                             Case{"seq", wrong_seq, "seq_out_of_range"},
                             Case{"id", wrong_id, "id_mismatch"},
                             Case{"session", wrong_session, "session_mismatch"},
                             Case{"run", wrong_run, "run_mismatch"}}) {
        CAPTURE(item.name);
        auto check = VerifyCrossSessionRef(item.ref, *parent);
        CHECK_FALSE(check.ok);
        CHECK(check.reason == item.expect_reason);
    }
}

TEST_CASE("父子账:fixture 两册互证,linked 检查点与派生来源都验 hash") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-tree";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    // 还原目录合同:session 根 + subagents/<child>/<child>.jsonl。
    CopyFile(Fixture("subagent_parent.jsonl"), dir / "PARENT.jsonl");
    CopyFile(Fixture("subagent_child.jsonl"),
             dir / "subagents" / "20260910-170000-CHILD" /
                 "20260910-170000-CHILD.jsonl");

    SubagentSessionNode root = WalkSessionTree(dir / "PARENT.jsonl");
    CHECK(root.session_id == "20260910-170000-PARENT");
    REQUIRE(root.children.size() == 1);
    const SubagentSessionNode& child = root.children[0];
    CHECK(child.session_id == "20260910-170000-CHILD");
    CHECK(child.task_id == "task-000001");
    CHECK(child.parent_action_id == "action-000001");
    CHECK(child.link_status == "linked");
    // 子账在,可读;子账首行 systemMeta.spawnEventRef 五键对父账验 hash。
    REQUIRE(child.ledger.has_value());
    CHECK(child.source_check.ok);
    CHECK(child.ledger->context.chain.size() == 2);
    CHECK(child.children.empty());
}

TEST_CASE("父子账:子账缺失 → 标 child_missing,不宣称完整恢复") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-missing";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    CopyFile(Fixture("subagent_parent.jsonl"), dir / "PARENT.jsonl");
    // 故意不放 subagents/ 子账。

    SubagentSessionNode root = WalkSessionTree(dir / "PARENT.jsonl");
    REQUIRE(root.children.size() == 1);
    CHECK(root.children[0].link_status == "child_missing");  // §4.33"子账缺失"
    CHECK_FALSE(root.children[0].ledger.has_value());
}

TEST_CASE("父子账:嵌套递归,每层完整布局(§4.31)") {
    class FixedClock : public V3Clock {
    public:
        std::int64_t WallMs() const override { return 1759468800000LL; }
    };
    FixedClock clock;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-nested";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "subagents" / "C1" / "subagents" / "C2", ec);
    std::filesystem::path parent_jsonl = dir / "P1.jsonl";
    std::filesystem::path child_jsonl = dir / "subagents" / "C1" / "C1.jsonl";
    std::filesystem::path grandchild_jsonl = dir / "subagents" / "C1" / "subagents" / "C2" /
                                             "C2.jsonl";
    {
        auto parent = V3Writer::Start(parent_jsonl, "P1", "run-P1", "父 system",
                                      nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(parent.has_value());
        EventDraft spawn;
        spawn.kind = EventKindV3::SubagentSpawnRequested;
        spawn.action_id = "action-000001";
        spawn.task_id = "task-000001";
        spawn.turn_id = "turn-000001";
        spawn.payload = nlohmann::json::object(
            {{"taskId", "task-000001"},
             {"childSessionRef",
              nlohmann::json::object({{"sessionId", "C1"},
                                      {"runId", "run-C1"},
                                      {"journalPath", "subagents/C1/C1.jsonl"}})},
             {"attempt", 1}});
        REQUIRE(parent->AppendEvent(std::move(spawn), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
        EventDraft linked;
        linked.kind = EventKindV3::SubagentLinked;
        linked.action_id = "action-000001";
        linked.task_id = "task-000001";
        linked.status = OpStatus::Done;
        linked.payload = nlohmann::json::object(
            {{"taskId", "task-000001"},
             {"childCheckpointRef",
              nlohmann::json::object({{"sessionId", "C1"},
                                      {"runId", "run-C1"},
                                      {"seq", 2},
                                      {"lineHash", std::string(64, '0')})}});
        REQUIRE(parent->AppendEvent(std::move(linked), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    }
    {
        auto child = V3Writer::Start(child_jsonl, "C1", "run-C1", "子 system",
                                     nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(child.has_value());
        EventDraft spawn;
        spawn.kind = EventKindV3::SubagentSpawnRequested;
        spawn.action_id = "action-000001";
        spawn.task_id = "task-000002";
        spawn.payload = nlohmann::json::object(
            {{"taskId", "task-000002"},
             {"childSessionRef",
              nlohmann::json::object({{"sessionId", "C2"},
                                      {"runId", "run-C2"},
                                      {"journalPath", "subagents/C2/C2.jsonl"}})},
             {"attempt", 1}});
        REQUIRE(child->AppendEvent(std::move(spawn), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    }
    {
        auto grandchild = V3Writer::Start(grandchild_jsonl, "C2", "run-C2", "孙 system",
                                          nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(grandchild.has_value());
    }
    SubagentSessionNode root = WalkSessionTree(parent_jsonl);
    REQUIRE(root.children.size() == 1);
    CHECK(root.children[0].session_id == "C1");
    CHECK(root.children[0].link_status == "linked");
    REQUIRE(root.children[0].children.size() == 1);  // 递归下钻
    CHECK(root.children[0].children[0].session_id == "C2");
    CHECK(root.children[0].children[0].link_status == "not_linked");
    REQUIRE(root.children[0].children[0].ledger.has_value());
}

TEST_CASE("父子账:环(sessionId 回头)标 cycle,不再下钻") {
    class FixedClock : public V3Clock {
    public:
        std::int64_t WallMs() const override { return 1759468800000LL; }
    };
    FixedClock clock;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                "lubancode-v3-reader-cycle";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "subagents" / "LOOP2", ec);
    std::filesystem::path first = dir / "LOOP1.jsonl";
    std::filesystem::path second = dir / "subagents" / "LOOP2" / "LOOP2.jsonl";
    auto write_spawn = [](V3Writer& writer, const std::string& child_id) {
        EventDraft spawn;
        spawn.kind = EventKindV3::SubagentSpawnRequested;
        spawn.action_id = "action-000001";
        spawn.task_id = "task-loop";
        spawn.payload = nlohmann::json::object(
            {{"taskId", "task-loop"},
             {"childSessionRef",
              nlohmann::json::object({{"sessionId", child_id},
                                      {"runId", "run-" + child_id},
                                      {"journalPath", "subagents/" + child_id + "/" +
                                                          child_id + ".jsonl"}})},
             {"attempt", 1}});
        REQUIRE(writer.AppendEvent(std::move(spawn), Durability::PowerLoss).status ==
                WriteReceipt::Status::Committed);
    };
    {
        auto loop1 = V3Writer::Start(first, "LOOP1", "run-LOOP1", "一",
                                     nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(loop1.has_value());
        write_spawn(*loop1, "LOOP2");
    }
    {
        auto loop2 = V3Writer::Start(second, "LOOP2", "run-LOOP2", "二",
                                     nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(loop2.has_value());
        write_spawn(*loop2, "LOOP1");  // 指回祖先 → 环
    }
    SubagentSessionNode root = WalkSessionTree(first);
    REQUIRE(root.children.size() == 1);
    CHECK(root.children[0].session_id == "LOOP2");
    REQUIRE(root.children[0].children.size() == 1);
    CHECK(root.children[0].children[0].link_status == "cycle");  // 不再下钻
    CHECK(root.children[0].children[0].children.empty());
}
