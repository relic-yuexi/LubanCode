// 按代理状态投影单 P0:诊断台账(ui_trace)的回归册。
//
// 三关卡(接收/应用/提交)各记一行,owner 带 session_generation 与
// task_id;Describe 是一行可对账文本;默认关、装了录音机才记——诊断
// 自己不成为写屏者(不碰 TermOut/TermErr)。

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cli/agent_view_state.hpp"
#include "cli/ui_trace.hpp"

using namespace lubancode::cli;

TEST_CASE("P0 台账: 三关卡各记一行,owner/世代/修订号齐全") {
    std::vector<ui_trace::Record> records;
    ui_trace::InstallSink([&](const ui_trace::Record& record) { records.push_back(record); });
    REQUIRE(ui_trace::Enabled());

    const AgentViewKey owner{42, 7};
    ui_trace::NoteReceived(owner, 3, "text_delta", 101);
    ui_trace::NoteApplied(owner, 3, "text_delta", 102);
    ui_trace::NoteCommitted(owner, 3, "tool_done", 102, "sink");

    REQUIRE(records.size() == 3);
    CHECK(records[0].stage == ui_trace::Stage::Received);
    CHECK(records[0].owner == owner);
    CHECK(records[0].owner.session_generation == 42);
    CHECK(records[0].owner.task_id == 7);
    CHECK(records[0].view_epoch == 3);
    CHECK(records[0].revision == 101);
    CHECK(records[1].stage == ui_trace::Stage::Applied);
    CHECK(records[1].revision == 102);
    CHECK(records[2].stage == ui_trace::Stage::Committed);
    CHECK(records[2].writer == "sink");
    CHECK(records[2].revision == 102);
    for (const auto& record : records) {
        CHECK(record.timestamp_ms > 0);
    }

    // 单行文本:关卡/世代/任务/纪元/修订号/写屏者都在。
    const std::string line = ui_trace::Describe(records[2]);
    CHECK(line.find("commit") != std::string::npos);
    CHECK(line.find("gen=42") != std::string::npos);
    CHECK(line.find("task=7") != std::string::npos);
    CHECK(line.find("epoch=3") != std::string::npos);
    CHECK(line.find("rev=102") != std::string::npos);
    CHECK(line.find("sink") != std::string::npos);

    // 拆掉录音机即关:一个字不记。
    ui_trace::InstallSink(nullptr);
    CHECK_FALSE(ui_trace::Enabled());
    ui_trace::NoteReceived(owner, 3, "text_delta", 103);
    CHECK(records.size() == 3);
}

TEST_CASE("P0 台账: main 也是合法成员——task=0 不另造号") {
    std::vector<ui_trace::Record> records;
    ui_trace::InstallSink([&](const ui_trace::Record& record) { records.push_back(record); });

    const AgentViewKey main_owner{1, 0};
    ui_trace::NoteApplied(main_owner, 0, "usage", 5);
    REQUIRE(records.size() == 1);
    CHECK(records[0].owner.is_main());
    CHECK(ui_trace::Describe(records[0]).find("task=0") != std::string::npos);

    ui_trace::InstallSink(nullptr);
}
