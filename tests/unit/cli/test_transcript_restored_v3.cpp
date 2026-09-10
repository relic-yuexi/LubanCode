// 轨迹 v3 P3 第一棒:FormatRestoredHistory 的 v3 入口(吃 runtime 的
// RestoredHistoryView)。验收(设计单 §1.3/§4.10/§4.11):
//   - 旧 user/assistant/tool 渲染成背景块/Markdown/配对卡;
//   - 压缩分界线插在时间线原序位置,带"前 ~N → 后 ~M tokens"
//     (N/M 取 CompactMarkerView 持久字段,不重算);
//   - 被压缩原文(removed_by_compacts)照常显示——"已压缩"说历史不说删除;
//   - display.hidden 默认不渲染,不报错;
//   - 纯读:渲染结果只是文本,不碰 live 条目账(与 TranscriptUiController
//     的 items_ 无涉——本函数即一次性铺滚动缓冲的字符串)。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "runtime/trajectory_history_view.hpp"

using lubancode::cli::BuiltinTheme;
using lubancode::cli::FormatRestoredHistory;
using lubancode::runtime::RestoredCompactView;
using lubancode::runtime::RestoredHistoryItem;
using lubancode::runtime::RestoredHistoryView;
using lubancode::runtime::RestoredMessageView;

namespace {

RestoredHistoryItem UserMessage(const std::string& id, const std::string& text) {
    RestoredHistoryItem item;
    item.kind = RestoredHistoryItem::Kind::Message;
    item.message.message_id = id;
    item.message.message.role = lubancode::api::Role::User;
    item.message.message.content.push_back(lubancode::api::TextBlock{text});
    return item;
}

RestoredHistoryItem AssistantText(const std::string& id, const std::string& text) {
    RestoredHistoryItem item;
    item.kind = RestoredHistoryItem::Kind::Message;
    item.message.message_id = id;
    item.message.message.role = lubancode::api::Role::Assistant;
    item.message.message.content.push_back(lubancode::api::TextBlock{text});
    return item;
}

RestoredHistoryItem CompactMarker(std::uint64_t before, std::uint64_t after) {
    RestoredHistoryItem item;
    item.kind = RestoredHistoryItem::Kind::Compact;
    item.compact.compact_id = "compact-000001";
    item.compact.context_tokens_before = before;
    item.compact.context_tokens_after = after;
    return item;
}

}  // namespace

TEST_CASE("v3 重放: user 块/assistant Markdown/工具配对照铺,压缩线带持久 token 数") {
    RestoredHistoryView view;
    view.session_id = "20260910-083000-V3FIX3";

    RestoredHistoryItem removed = UserMessage("msg-000002", "第一问:聊聊 parser 的设计。");
    removed.message.in_current_context = false;
    removed.message.removed_by_compacts = {"compact-000001"};
    view.items.push_back(std::move(removed));
    view.items.push_back(AssistantText("msg-000003", "parser 分三层:**词法**、**语法**、**语义**。"));
    view.items.push_back(CompactMarker(142800, 31600));
    view.items.push_back(UserMessage("msg-000009", "压缩之后接着问。"));

    // assistant 带 tool_use,后面 user 消息带同键 tool_result(actionId 配对)。
    RestoredHistoryItem assistant_tool = AssistantText("msg-000010", "我再读一个文件。");
    assistant_tool.message.message.role = lubancode::api::Role::Assistant;
    lubancode::api::ToolUseBlock use;
    use.id = "action-000001";
    use.name = "read_file";
    use.input = nlohmann::json{{"path", "a.txt"}};
    assistant_tool.message.message.content.push_back(std::move(use));
    view.items.push_back(std::move(assistant_tool));

    RestoredHistoryItem tool_result;
    tool_result.kind = RestoredHistoryItem::Kind::Message;
    tool_result.message.message_id = "msg-000011";
    tool_result.message.message.role = lubancode::api::Role::User;
    tool_result.message.message.content.push_back(
        lubancode::api::ToolResultBlock{"action-000001", "第一行\n第二行\n第三行", false});
    view.items.push_back(std::move(tool_result));

    const std::string out = FormatRestoredHistory(view, BuiltinTheme("plain"), 120);

    // 旧 user/assistant/tool 都渲染(§1.3"前面的 user、assistant、tool 仍能
    // 向上滚动查看")。
    CHECK(out.find("> 第一问") != std::string::npos);
    CHECK(out.find("● 助手") != std::string::npos);
    CHECK(out.find("词法") != std::string::npos);
    CHECK(out.find("> 压缩之后接着问") != std::string::npos);
    CHECK(out.find("read_file(a.txt)") != std::string::npos);
    CHECK(out.find("第一行") != std::string::npos);
    CHECK(out.find("另有 2 行") != std::string::npos);

    // 压缩分界线:数字读持久字段,带千位逗号(§4.11 示例口径)。
    const std::size_t line = out.find("上下文压缩");
    CHECK(line != std::string::npos);
    CHECK(out.find("前 ~142,800") != std::string::npos);
    CHECK(out.find("后 ~31,600") != std::string::npos);

    // 被压缩原文仍显示:removed_by_compacts 只标状态,不折叠正文。
    CHECK(out.find("第一问:聊聊 parser 的设计") != std::string::npos);

    // 压缩线在原文之后、压缩后新输入之前(时间线原序)。
    const std::size_t removed_pos = out.find("第一问:聊聊 parser 的设计");
    const std::size_t marker_pos = out.find("上下文压缩");
    const std::size_t after_pos = out.find("压缩之后接着问");
    CHECK(removed_pos < marker_pos);
    CHECK(marker_pos < after_pos);
}

TEST_CASE("v3 重放: hidden 默认不渲染不报错;缺 token 数退回无数字文案") {
    RestoredHistoryView view;
    view.items.push_back(UserMessage("msg-000002", "正文还在。"));

    // display.hidden 的 compact prompt:默认不画(§4.28)。
    RestoredHistoryItem hidden_prompt = UserMessage("msg-000006", "内部压缩指令不该露面");
    hidden_prompt.message.hidden = true;
    view.items.push_back(std::move(hidden_prompt));

    // 持久字段缺失(0/0):退回不带数字的压缩文案,不编造。
    view.items.push_back(CompactMarker(0, 0));
    view.items.push_back(AssistantText("msg-000009", "收尾。"));

    const std::string out = FormatRestoredHistory(view, BuiltinTheme("plain"), 100);
    CHECK(out.find("内部压缩指令不该露面") == std::string::npos);  // hidden 不渲染
    CHECK(out.find("正文还在") != std::string::npos);
    CHECK(out.find("收尾") != std::string::npos);
    CHECK(out.find("此处发生过一次上下文压缩") != std::string::npos);
    CHECK(out.find("~0") == std::string::npos);  // 不补 0、不重算
}

TEST_CASE("v2 入口回归: 同一颗 formatter 照旧吃 api::Message 序列") {
    std::vector<lubancode::api::Message> messages;
    messages.push_back({lubancode::api::Role::User, {lubancode::api::TextBlock{"旧问题"}}});
    lubancode::api::Message assistant;
    assistant.role = lubancode::api::Role::Assistant;
    assistant.content.push_back(lubancode::api::TextBlock{"旧回答"});
    messages.push_back(std::move(assistant));

    const std::string out = FormatRestoredHistory(messages, BuiltinTheme("plain"), 100);
    CHECK(out.find("> 旧问题") != std::string::npos);
    CHECK(out.find("● 助手") != std::string::npos);
    CHECK(out.find("旧回答") != std::string::npos);
    CHECK(out.find("上下文压缩") == std::string::npos);  // v2 路不带压缩线(不强求)
}
