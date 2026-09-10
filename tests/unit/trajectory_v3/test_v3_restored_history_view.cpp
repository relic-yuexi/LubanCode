// 轨迹 v3 P3 第一棒:runtime 显示 DTO 投影(HistoryTimeline → 显示 DTO,
// session_switch 接线点 3/5)。规矩:runtime 适配层做翻译,显示层(cli/
// app_server)只吃这份 DTO,不直接碰 reader.hpp 的 C++ 结构。
// 验收:逐消息 in_current_context/replaced_by_derivation/removed_by_compacts
// 标志齐;CompactMarkerView 的 context_tokens_before/after 与 removed/
// retained refs 齐(§4.11:持久字段,不重算);system 根不进显示;工具
// 调用按 actionId 配对;display.hidden 默认不渲染但不报错;坏账给空。
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "api/types.hpp"
#include "runtime/trajectory_history_view.hpp"

using lubancode::runtime::ProjectRestoredHistory;
using lubancode::runtime::RestoredHistoryItem;

namespace {

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

const RestoredHistoryItem* FindItem(const lubancode::runtime::RestoredHistoryView& view,
                                    const std::string& id) {
    for (const auto& item : view.items) {
        if (item.kind == RestoredHistoryItem::Kind::Message && item.message.message_id == id) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("显示投影: compact 全链——被压缩原文保留并带三标志,压缩标记带持久 token 数") {
    const auto view = ProjectRestoredHistory(Fixture("compact_full.jsonl"));
    CHECK(view.session_id == "20260910-083000-V3FIX4");
    REQUIRE_FALSE(view.items.empty());

    // system 根(msg-000001)是上下文根,不进显示 DTO。
    CHECK(FindItem(view, "msg-000001") == nullptr);

    // 被压缩原文(msg-000002/000003):仍在时间线、正文照带,标明已由
    // compact-000001 移出上下文(§4.1"原文保留,标明已由摘要替代")。
    const auto* removed = FindItem(view, "msg-000002");
    REQUIRE(removed != nullptr);
    CHECK_FALSE(removed->message.in_current_context);
    REQUIRE(removed->message.removed_by_compacts.size() == 1);
    CHECK(removed->message.removed_by_compacts[0] == "compact-000001");
    REQUIRE_FALSE(removed->message.message.content.empty());
    const auto* text = std::get_if<lubancode::api::TextBlock>(&removed->message.message.content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text.find("第一轮") != std::string::npos);

    // 压缩标记:插在时间线的 applied 位置(token 数字读持久字段)。
    bool saw_marker = false;
    for (const auto& item : view.items) {
        if (item.kind != RestoredHistoryItem::Kind::Compact) {
            continue;
        }
        saw_marker = true;
        CHECK(item.compact.compact_id == "compact-000001");
        CHECK(item.compact.event_id == "evt-000014");
        CHECK(item.compact.context_tokens_before == 142800);
        CHECK(item.compact.context_tokens_after == 31600);
        REQUIRE(item.compact.removed_message_refs.size() == 2);
        CHECK(item.compact.removed_message_refs[0] == "msg-000002");
        CHECK(item.compact.removed_message_refs[1] == "msg-000003");
        REQUIRE(item.compact.retained_message_refs.size() == 2);
        CHECK(item.compact.retained_message_refs[0] == "msg-000004");
        // 标记在摘要(msg-000008)与压缩后新输入(msg-000009)之间:顺序
        // 不许挪(§4.10"不能把标记插回过去伪造发生顺序")。
    }
    CHECK(saw_marker);

    // compact prompt(msg-000006,hidden)与生效摘要(msg-000008,hidden):
    // hidden 标志如实带(渲染层默认不画,不等于删除)。
    const auto* prompt = FindItem(view, "msg-000006");
    REQUIRE(prompt != nullptr);
    CHECK(prompt->message.hidden);
    const auto* summary = FindItem(view, "msg-000008");
    REQUIRE(summary != nullptr);
    CHECK(summary->message.hidden);
    CHECK(summary->message.in_current_context);

    // 时间线 seq 升序,消息与标记合流不乱序。
    for (std::size_t i = 1; i < view.items.size(); ++i) {
        CHECK(view.items[i - 1].seq < view.items[i].seq);
    }
}

TEST_CASE("显示投影: 工具调用按 actionId 配对,结果走 user 消息携带") {
    const auto view = ProjectRestoredHistory(Fixture("tool_round.jsonl"));

    // assistant(msg-000003)的调用块:provider 号换成 actionId。
    const auto* assistant = FindItem(view, "msg-000003");
    REQUIRE(assistant != nullptr);
    CHECK(assistant->message.message.role == lubancode::api::Role::Assistant);
    const lubancode::api::ToolUseBlock* use = nullptr;
    for (const auto& block : assistant->message.message.content) {
        if (auto* candidate = std::get_if<lubancode::api::ToolUseBlock>(&block)) {
            use = candidate;
        }
    }
    REQUIRE(use != nullptr);
    CHECK(use->id == "action-000001");  // 不是 provider 的 call_prov_9xK
    CHECK(use->name == "read_file");
    CHECK(use->input.value("path", std::string()) == "src/app/main.cpp");

    // tool 消息(msg-000004):user 消息携带 ToolResultBlock,同键配对。
    const auto* tool = FindItem(view, "msg-000004");
    REQUIRE(tool != nullptr);
    CHECK(tool->message.message.role == lubancode::api::Role::User);
    const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&tool->message.message.content[0]);
    REQUIRE(result != nullptr);
    CHECK(result->tool_use_id == "action-000001");
    CHECK(result->content.find("exit_code: 0") != std::string::npos);
    CHECK_FALSE(result->is_error);  // 选用结果是 done

    // 用户原文照带。
    const auto* user = FindItem(view, "msg-000002");
    REQUIRE(user != nullptr);
    const auto* user_text =
        std::get_if<lubancode::api::TextBlock>(&user->message.message.content[0]);
    REQUIRE(user_text != nullptr);
    CHECK(user_text->text.find("main.cpp") != std::string::npos);
}

TEST_CASE("显示投影: 降档派生——原版标 replaced_by_derivation,链上在派生版") {
    const auto view = ProjectRestoredHistory(Fixture("preview_reduction.jsonl"));
    const auto* original = FindItem(view, "msg-000004");
    REQUIRE(original != nullptr);
    CHECK(original->message.replaced_by_derivation);
    CHECK_FALSE(original->message.in_current_context);
    const auto* derived = FindItem(view, "msg-000005");
    REQUIRE(derived != nullptr);
    CHECK(derived->message.in_current_context);
    CHECK_FALSE(derived->message.hidden);
}

TEST_CASE("显示投影: 坏账给空 view,不抛错不冒充") {
    // 非 v3 文件(v2 main.jsonl 形状)与不存在路径:都给空 items。
    const auto bogus = ProjectRestoredHistory(std::filesystem::temp_directory_path() / "no-such-v3.jsonl");
    CHECK(bogus.items.empty());
    CHECK(bogus.session_id.empty());
}
