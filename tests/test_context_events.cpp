// agent/context_events.hpp:规范化事件账与无损结构压缩(0.31.x 第二期)。
// 钉的规矩:事件 id 稳定、tool use/result 原子配对、同键同 hash 只留一份
// 正文 + 引用计数、文件改版不合并(旧版标 superseded)、副作用工具不判重、
// 超长结果换 artifact 引用、热区不碰、消息条数与块序不动。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

api::Message UserText(const std::string& text) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantText(const std::string& text) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantToolUse(const std::string& id, const std::string& name, const nlohmann::json& input) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::ToolUseBlock{id, name, input});
    return m;
}

api::Message UserToolResult(const std::string& tool_use_id, const std::string& content,
                            bool is_error = false) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::ToolResultBlock{tool_use_id, content, is_error});
    return m;
}

// 一枚 read_file 读取:assistant tool_use + user tool_result。
// 返回压入 out 的消息条数(固定 2)。
void AppendRead(std::vector<api::Message>& out, const std::string& call_id, const std::string& path,
                const std::string& content) {
    nlohmann::json input;
    input["path"] = path;
    out.push_back(AssistantToolUse(call_id, "read_file", input));
    out.push_back(UserToolResult(call_id, content));
}

std::string ResultTextAt(const std::vector<api::Message>& history, std::size_t message_index,
                         const std::string& tool_use_id) {
    REQUIRE(message_index < history.size());
    for (const auto& block : history[message_index].content) {
        if (std::holds_alternative<api::ToolResultBlock>(block)) {
            const auto& result = std::get<api::ToolResultBlock>(block);
            if (result.tool_use_id == tool_use_id) {
                return result.content;
            }
        }
    }
    return std::string("<no result>");
}

}  // namespace

TEST_CASE("BuildEventLedger: tool use/result 原子配对成一枚事件,id 稳定") {
    std::vector<api::Message> history;
    history.push_back(UserText("看看这个文件"));
    AppendRead(history, "t1", "src/a.cpp", "int main() {}");
    history.push_back(AssistantText("读完了"));
    history.push_back(UserText("再看看"));
    AppendRead(history, "t2", "src/b.cpp", "namespace {}");

    const auto ledger = agent::BuildEventLedger(history);
    const auto again = agent::BuildEventLedger(history);
    REQUIRE(ledger.size() == 5);  // UserText + ToolExchange + AssistantText + UserText + ToolExchange
    // id 稳定:同一份历史重算逐枚一致。
    for (std::size_t i = 0; i < ledger.size(); ++i) {
        CHECK(ledger[i].id == again[i].id);
        CHECK(ledger[i].id == "e" + std::to_string(i));
    }
    CHECK(ledger[0].kind == agent::NormalizedEventKind::UserText);
    CHECK(ledger[1].kind == agent::NormalizedEventKind::ToolExchange);
    CHECK(ledger[1].tool_name == "read_file");
    CHECK(ledger[1].result_content == "int main() {}");
    CHECK(ledger[1].dedup_key.find("src/a.cpp") != std::string::npos);
    CHECK_FALSE(ledger[1].content_hash.empty());
    CHECK(ledger[2].kind == agent::NormalizedEventKind::AssistantText);
}

TEST_CASE("BuildEventLedger: 副作用工具不判重(run_command 给空键)") {
    std::vector<api::Message> history;
    nlohmann::json input;
    input["command"] = "cmake --build .";
    history.push_back(AssistantToolUse("t1", "run_command", input));
    history.push_back(UserToolResult("t1", "Build succeeded."));

    const auto ledger = agent::BuildEventLedger(history);
    REQUIRE(ledger.size() == 1);
    CHECK(ledger[0].kind == agent::NormalizedEventKind::ToolExchange);
    CHECK(ledger[0].dedup_key.empty());  // 命令文本相同 ≠ 这次可以不跑
}

TEST_CASE("Fingerprint64: 同文同指纹,改一字即变") {
    CHECK(agent::Fingerprint64("同一份内容") == agent::Fingerprint64("同一份内容"));
    CHECK(agent::Fingerprint64("同一份内容") != agent::Fingerprint64("同一份内客"));
    CHECK(agent::Fingerprint64("").size() == 16);
}

TEST_CASE("CompressWorkingView: 同文件同内容读三次,只留一份正文与引用计数") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", std::string(2000, 'A'));
    history.push_back(UserText("第二问"));
    AppendRead(history, "t2", "src/a.cpp", std::string(2000, 'A'));
    history.push_back(UserText("第三问"));
    AppendRead(history, "t3", "src/a.cpp", std::string(2000, 'A'));
    // 末尾再来一轮纯文本,三次读取全部落进冷区。
    history.push_back(UserText("最后一问"));
    history.push_back(AssistantText("收工"));

    agent::StructuralCompressionOptions options;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);

    // 消息条数不变、配对不破。
    REQUIRE(view.size() == history.size());
    CHECK(stats.duplicate_groups == 2);  // 第二、三次是重复
    CHECK(stats.reclaimable_bytes() > 0);
    // 第一份正文保住(msg2 是 t1 的 result)。
    CHECK(ResultTextAt(view, 2, "t1") == std::string(2000, 'A'));
    // 后两份换成引用:指到第一枚事件(e1),带累计次数。
    const std::string second = ResultTextAt(view, 5, "t2");
    CHECK(second.find("e1") != std::string::npos);   // 指到保留正文的那枚事件
    CHECK(second.find("2 次") != std::string::npos);  // 这是第二遍
    CHECK(second.size() < 2000);
    const std::string third = ResultTextAt(view, 8, "t3");
    CHECK(third.find("e1") != std::string::npos);
    CHECK(third.find("3 次") != std::string::npos);  // 看过三回
}

TEST_CASE("CompressWorkingView: 文件改版再读,不合并;旧版标 superseded 保头部") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", "旧版本内容:" + std::string(2000, 'O'));
    history.push_back(AssistantText("改一改"));
    history.push_back(UserText("第二问"));
    AppendRead(history, "t2", "src/a.cpp", "新版本内容:" + std::string(2000, 'N'));
    // 末尾补一轮纯文本,把两次读取都放进冷区。
    history.push_back(UserText("最后一问"));
    history.push_back(AssistantText("收工"));

    agent::StructuralCompressionOptions options;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);

    // 两版指纹不同,绝不判重:新版正文原样。
    CHECK(ResultTextAt(view, 6, "t2") == "新版本内容:" + std::string(2000, 'N'));
    // 旧版标 superseded:保头部预览 + 指到新版事件(e4 是新版读取);
    // 依赖旧版的决定(assistant 正文)原样未动。
    const std::string old_view = ResultTextAt(view, 2, "t1");
    CHECK(old_view.find("旧版本内容:") != std::string::npos);  // 头部预览在
    CHECK(old_view.find("已改版") != std::string::npos);
    CHECK(old_view.find("e4") != std::string::npos);  // 指到新版读取那枚事件
    CHECK(old_view.size() < 2000);
    CHECK(stats.superseded_observations == 1);
    CHECK(std::get<api::TextBlock>(view[3].content[0]).text == "改一改");
}

TEST_CASE("CompressWorkingView: 超长冷结果换 artifact 引用,头尾预览保住") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    // search 结果超长(> long_result_bytes)。
    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "TODO";
    history.push_back(AssistantToolUse("t1", "search", input));
    const std::string head_marker = "HEAD_MARKER_LINE";
    const std::string tail_marker = "TAIL_MARKER_LINE";
    std::string content = head_marker + std::string(20000, 'S') + tail_marker;
    history.push_back(UserToolResult("t1", content));
    // 再来一轮,让超长结果落进冷区(热区=最后一轮)。
    history.push_back(UserText("第二问"));
    history.push_back(AssistantText("收工"));

    agent::StructuralCompressionOptions options;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);

    const std::string stub = ResultTextAt(view, 2, "t1");
    CHECK(stats.offloaded_results == 1);
    CHECK(stub.find("[artifact") == 0);
    CHECK(stub.find(head_marker) != std::string::npos);  // 头部预览在
    CHECK(stub.find(tail_marker) != std::string::npos);  // 尾部预览在
    CHECK(stub.find("sha=") != std::string::npos);
    CHECK(stub.size() < content.size());
    // JSONL 真账不动:原 history 里正文还在。
    CHECK(ResultTextAt(history, 2, "t1") == content);
}

TEST_CASE("CompressWorkingView: 热区(最后一轮)不碰,哪怕重复/超长") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", std::string(2000, 'A'));
    history.push_back(UserText("最后一问"));
    AppendRead(history, "t2", "src/a.cpp", std::string(2000, 'A'));  // 与 t1 同键同 hash

    agent::StructuralCompressionOptions options;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);

    // 热区从"最后一问"起:t2 的正文原样保留。
    CHECK(ResultTextAt(view, 5, "t2") == std::string(2000, 'A'));
}

TEST_CASE("CompressWorkingView: 短结果不折腾(换标注反而更长)") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", "short");
    history.push_back(UserText("第二问"));
    AppendRead(history, "t2", "src/a.cpp", "short");

    agent::StructuralCompressionOptions options;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);

    CHECK(stats.reclaimable_bytes() == 0);
    CHECK(ResultTextAt(view, 2, "t1") == "short");
    CHECK(ResultTextAt(view, 5, "t2") == "short");
}

TEST_CASE("CompressWorkingView: 关掉热区保护后重复照收") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", std::string(2000, 'A'));
    history.push_back(UserText("最后一问"));
    AppendRead(history, "t2", "src/a.cpp", std::string(2000, 'A'));

    agent::StructuralCompressionOptions options;
    options.protect_hot_zone = false;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);
    CHECK(stats.duplicate_groups == 1);
    CHECK(ResultTextAt(view, 5, "t2").find("e1") != std::string::npos);
}

TEST_CASE("CompressWorkingView: 消息条数与 tool 配对永远不破") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", std::string(2000, 'A'));
    AppendRead(history, "t2", "src/b.cpp", std::string(20000, 'B'));
    nlohmann::json cmd;
    cmd["command"] = "run tests";
    history.push_back(AssistantToolUse("t3", "run_command", cmd));
    history.push_back(UserToolResult("t3", "all passed"));
    history.push_back(AssistantText("done"));
    history.push_back(UserText("最后一问"));

    agent::StructuralCompressionOptions options;
    agent::StructuralCompressionStats stats;
    const auto view = agent::CompressWorkingView(history, options, stats);

    REQUIRE(view.size() == history.size());
    // 副作用命令的结果一字不动(不判重也不外置?外置只看长度——超长的
    // 副作用结果同样可外置,因为外置只是换展示、不涉判等;这条命令结果
    // 很短,原样)。
    CHECK(ResultTextAt(view, 6, "t3") == "all passed");
    // 逐消息逐块:tool_use 的 id 与紧随 user 消息里同 id 的 result 仍在。
    for (std::size_t i = 0; i < view.size(); ++i) {
        for (const auto& block : view[i].content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                const auto& call = std::get<api::ToolUseBlock>(block);
                bool paired = false;
                for (const auto& next : view[i + 1].content) {
                    if (std::holds_alternative<api::ToolResultBlock>(next) &&
                        std::get<api::ToolResultBlock>(next).tool_use_id == call.id) {
                        paired = true;
                    }
                }
                CHECK(paired);
            }
        }
    }
}

TEST_CASE("HotZoneStartIndex: 最后一条用户文本输入起热区") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    AppendRead(history, "t1", "src/a.cpp", "x");
    history.push_back(UserText("最后一问"));
    AppendRead(history, "t2", "src/b.cpp", "y");
    CHECK(agent::HotZoneStartIndex(history) == 3);
    CHECK(agent::HotZoneStartIndex({}) == 0);
    CHECK(agent::HotZoneStartIndex({UserToolResult("t", "孤立结果")}) == 0);
}
