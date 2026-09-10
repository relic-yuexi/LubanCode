// v3 fixture 与 C++ 校验器互证:python scripts/validate_trajectory_v3.py
// 校过的五份样例,C++ VerifyV3File 也必须全绿——canonical/哈希/schema 两边
// 任一漂移,这册先红。
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "trajectory/v3/compact.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

}  // namespace

TEST_CASE("startup:首行 system + session.started,链单节点") {
    V3VerifyReport report = VerifyV3File(Fixture("startup.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.lines == 2);
    CHECK(report.context.revision == 1);
    REQUIRE(report.context.chain.size() == 1);
    CHECK(!report.context.chain[0].prev_message_ref.has_value());
}

TEST_CASE("soul_switch:三步切换,恢复视图取新根") {
    V3VerifyReport report = VerifyV3File(Fixture("soul_switch.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.context.revision == 3);
    REQUIRE(report.context.chain.size() == 2);
    CHECK(report.context.system_message_ref == "msg-000003");
    CHECK(report.context.chain[1].message_ref == "msg-000002");
    CHECK(report.context.chain[1].prev_message_ref == "msg-000003");
}

TEST_CASE("tool_round:一轮对话+工具,prepared 引用与链一致") {
    V3VerifyReport report = VerifyV3File(Fixture("tool_round.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.lines == 19);
    // 链:system -> user -> assistant -> tool。
    REQUIRE(report.context.chain.size() == 4);
    CHECK(report.context.chain[0].message_ref == "msg-000001");
    CHECK(report.context.chain[3].message_ref == "msg-000004");
}

TEST_CASE("hook_effects:PreAction 换后端 + PostAction 补充,效果采用分层") {
    V3VerifyReport report = VerifyV3File(Fixture("hook_effects.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.lines == 23);
    CHECK(report.context.revision == 4);
    REQUIRE(report.context.chain.size() == 4);
    // 模型声明不改写:tool 消息仍配对原 action(§4.22)。
    CHECK(report.context.chain[3].message_ref == "msg-000004");
}

TEST_CASE("preview_reduction:32->16 KiB 派生消息换链,revision 前移") {
    V3VerifyReport report = VerifyV3File(Fixture("preview_reduction.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.lines == 17);
    CHECK(report.context.revision == 5);
    REQUIRE(report.context.chain.size() == 4);
    // 链尾是派生消息 R1_16(msg-000005),R1_32(msg-000004)退链仍在档(§4.38)。
    CHECK(report.context.chain[3].message_ref == "msg-000005");
    CHECK(report.context.chain[3].prev_message_ref.value_or("") == "msg-000003");
    CHECK(report.context.preview_budget_bytes == 16384);
}

TEST_CASE("subagent 父子两账:各自成卷,链与身份不串") {
    V3VerifyReport parent = VerifyV3File(Fixture("subagent_parent.jsonl"));
    REQUIRE(parent.ok);
    CHECK(parent.lines == 17);
    CHECK(parent.context.revision == 4);

    V3VerifyReport child = VerifyV3File(Fixture("subagent_child.jsonl"));
    REQUIRE(child.ok);
    CHECK(child.lines == 5);
    REQUIRE(child.context.chain.size() == 2);  // 子 system 根 + 委派 user
    CHECK(child.context.chain[0].prev_message_ref.has_value() == false);
    CHECK(child.context.chain[1].message_ref == "msg-000002");
}

TEST_CASE("compact_full:八行全链,applied 后链重接为 system+摘要+保留") {
    V3VerifyReport report = VerifyV3File(Fixture("compact_full.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.lines == 25);
    // 新链:msg-000001(system) -> msg-000008(摘要) -> msg-000004 ->
    // msg-000005(retained) -> msg-000009(compact 后新输入)。
    REQUIRE(report.context.chain.size() == 5);
    CHECK(report.context.chain[0].message_ref == "msg-000001");
    CHECK(report.context.chain[1].message_ref == "msg-000008");
    CHECK(report.context.chain[1].prev_message_ref == "msg-000001");
    CHECK(report.context.chain[2].message_ref == "msg-000004");
    CHECK(report.context.chain[2].prev_message_ref == "msg-000008");
    CHECK(report.context.chain[3].message_ref == "msg-000005");
    CHECK(report.context.chain[4].message_ref == "msg-000009");
    CHECK(report.context.revision == 7);
    CHECK(report.context.open_compact_ids.empty());  // applied 收口
}

TEST_CASE("stream_interrupted:Esc 定稿 interrupted assistant,usage null") {
    V3VerifyReport report = VerifyV3File(Fixture("stream_interrupted.jsonl"));
    REQUIRE(report.ok);
    CHECK(report.lines == 13);
    // interrupted assistant 已接纳进链(§4.63:正式消息)。
    REQUIRE(report.context.chain.size() == 3);
    CHECK(report.context.chain[2].message_ref == "msg-000003");
}

TEST_CASE("fixture 全家福:C++ 写者能 Continue 每一份(读取侧地基)") {
    for (const char* name :
         {"startup.jsonl", "soul_switch.jsonl", "tool_round.jsonl", "compact_full.jsonl",
          "stream_interrupted.jsonl", "hook_effects.jsonl", "preview_reduction.jsonl",
          "subagent_parent.jsonl", "subagent_child.jsonl"}) {
        CAPTURE(name);
        auto writer = V3Writer::Continue(Fixture(name), V3WriterOptions{});
        REQUIRE(writer.has_value());
        CHECK(writer->next_seq() > 1);
    }
}
