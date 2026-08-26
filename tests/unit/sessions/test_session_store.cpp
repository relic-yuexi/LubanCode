// sessions/session_store:会话存档的序列化/反序列化/事件回放/成对修补/slug/
// 导出。多数是纯函数不碰磁盘(SessionStore 磁盘薄壳的行为靠集成验证);
// ListSessions 的 cwd 过滤要真扫目录,用临时目录测。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include "agent/compact.hpp"
#include "sessions/session_store.hpp"
#include "api/types.hpp"
#include "cli/slash_commands.hpp"

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

// 消息第一段文本,断言用。
std::string FirstText(const api::Message& m) {
    for (const auto& block : m.content) {
        if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
            return tb->text;
        }
    }
    return std::string();
}

// std::filesystem::path -> UTF-8 字符串(跟 session_store 内部同款)。
std::string PathUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// 序列化往返
// ---------------------------------------------------------------------------

TEST_CASE("消息序列化往返: 纯文本(中文)") {
    const api::Message original = UserText("我的暗号是青龙,记住了。");
    const std::string line = sessions::SerializeSessionMessage(original, "2026-07-17 10:00:00");
    CHECK(line.find('\n') == std::string::npos);  // 一行一条,不许夹换行

    const auto parsed = sessions::DeserializeSessionMessage(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->role == api::Role::User);
    REQUIRE(parsed->content.size() == 1);
    const auto* tb = std::get_if<api::TextBlock>(&parsed->content[0]);
    REQUIRE(tb != nullptr);
    CHECK(tb->text == "我的暗号是青龙,记住了。");
}

TEST_CASE("消息序列化往返: user 带图片") {
    api::Message original;
    original.role = api::Role::User;
    original.content.push_back(api::ImageBlock{"image/webp", "cGl4ZWxz", "截图.webp", 800, 600});

    const auto parsed = sessions::DeserializeSessionMessage(sessions::SerializeSessionMessage(original, "ts"));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->content.size() == 1);
    const auto* image = std::get_if<api::ImageBlock>(&parsed->content[0]);
    REQUIRE(image != nullptr);
    CHECK(image->media_type == "image/webp");
    CHECK(image->data == "cGl4ZWxz");
    CHECK(image->filename == "截图.webp");
    CHECK(image->width == 800);
    CHECK(image->height == 600);
}
TEST_CASE("消息序列化往返: assistant 带 tool_use") {
    api::Message original;
    original.role = api::Role::Assistant;
    original.content.push_back(api::TextBlock{"我看看这个文件。"});
    api::ToolUseBlock use;
    use.id = "toolu_01";
    use.name = "read_file";
    use.input = nlohmann::json{{"path", "C:/测试/a.txt"}};
    original.content.push_back(use);

    const auto parsed = sessions::DeserializeSessionMessage(sessions::SerializeSessionMessage(original, "ts"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->role == api::Role::Assistant);
    REQUIRE(parsed->content.size() == 2);
    const auto* got = std::get_if<api::ToolUseBlock>(&parsed->content[1]);
    REQUIRE(got != nullptr);
    CHECK(got->id == "toolu_01");
    CHECK(got->name == "read_file");
    CHECK(got->input["path"] == "C:/测试/a.txt");
}

TEST_CASE("消息序列化往返: user 带 tool_result(含 is_error)") {
    api::Message original;
    original.role = api::Role::User;
    api::ToolResultBlock result;
    result.tool_use_id = "toolu_01";
    result.content = "第一行\n第二行(中文内容)";
    result.is_error = true;
    original.content.push_back(result);

    const auto parsed = sessions::DeserializeSessionMessage(sessions::SerializeSessionMessage(original, "ts"));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->content.size() == 1);
    const auto* got = std::get_if<api::ToolResultBlock>(&parsed->content[0]);
    REQUIRE(got != nullptr);
    CHECK(got->tool_use_id == "toolu_01");
    CHECK(got->content == "第一行\n第二行(中文内容)");
    CHECK(got->is_error);
}

TEST_CASE("消息序列化往返: assistant 带 thinking(signature 也要往返)") {
    api::Message original;
    original.role = api::Role::Assistant;
    api::ThinkingBlock thinking;
    thinking.text = "让我分析一下这个问题。";
    thinking.signature = "sig_abc123";
    original.content.push_back(thinking);
    original.content.push_back(api::TextBlock{"答案是 42"});

    const auto parsed = sessions::DeserializeSessionMessage(sessions::SerializeSessionMessage(original, "ts"));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->content.size() == 2);
    const auto* got_thinking = std::get_if<api::ThinkingBlock>(&parsed->content[0]);
    REQUIRE(got_thinking != nullptr);
    CHECK(got_thinking->text == "让我分析一下这个问题。");
    CHECK(got_thinking->signature == "sig_abc123");
    const auto* got_text = std::get_if<api::TextBlock>(&parsed->content[1]);
    REQUIRE(got_text != nullptr);
    CHECK(got_text->text == "答案是 42");
}

TEST_CASE("反序列化: 坏行给 nullopt,不抛异常") {
    CHECK_FALSE(sessions::DeserializeSessionMessage("").has_value());
    CHECK_FALSE(sessions::DeserializeSessionMessage("不是 JSON").has_value());
    CHECK_FALSE(sessions::DeserializeSessionMessage("{\"role\":\"盗号的\",\"content\":[]}").has_value());
    CHECK_FALSE(sessions::DeserializeSessionMessage("{\"role\":\"user\"}").has_value());  // 缺 content
}

// ---------------------------------------------------------------------------
// meta
// ---------------------------------------------------------------------------

TEST_CASE("meta 序列化往返") {
    sessions::SessionMeta meta;
    meta.version = 1;
    meta.wire = "anthropic";
    meta.model = "MiniMax-M2";
    meta.cwd = "D:\\工程\\lubancode";
    meta.started_at = "2026-07-17 09:30:00";

    const auto parsed = sessions::ParseSessionMeta(sessions::SerializeSessionMeta(meta));
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    CHECK(parsed->wire == "anthropic");
    CHECK(parsed->model == "MiniMax-M2");
    CHECK(parsed->cwd == "D:\\工程\\lubancode");
    CHECK(parsed->started_at == "2026-07-17 09:30:00");
}

TEST_CASE("meta 解析: 坏行/缺 version 给 nullopt") {
    CHECK_FALSE(sessions::ParseSessionMeta("").has_value());
    CHECK_FALSE(sessions::ParseSessionMeta("垃圾").has_value());
    CHECK_FALSE(sessions::ParseSessionMeta("{\"wire\":\"anthropic\"}").has_value());
    // 消息行不是 meta(没有 version 字段)
    CHECK_FALSE(sessions::ParseSessionMeta("{\"role\":\"user\",\"content\":[],\"ts\":\"x\"}").has_value());
}

// ---------------------------------------------------------------------------
// slug / 会话 id
// ---------------------------------------------------------------------------

TEST_CASE("slug: 中文按码点截前 20 字,不从多字节字符中间掐断") {
    const std::string text = "这是一条很长很长的中文消息用来测试截断是否安全无虞";  // >20 字
    const std::string slug = sessions::MakeSessionSlug(text);
    CHECK(slug == "这是一条很长很长的中文消息用来测试截断是");  // 恰 20 个字
    // 截出来的必须是合法 UTF-8:字节数是 3 的倍数(全汉字)
    CHECK(slug.size() == 20 * 3);
}

TEST_CASE("slug: 空白和文件名危险字符换成 -,连续并一个,首尾剥掉") {
    CHECK(sessions::MakeSessionSlug("fix: a/b\\c?") == "fix-a-b-c");
    CHECK(sessions::MakeSessionSlug("  hello   world  ") == "hello-world");
    CHECK(sessions::MakeSessionSlug("<>:\"|?*") == "untitled");
    CHECK(sessions::MakeSessionSlug("") == "untitled");
}

TEST_CASE("slug: 中英混排,ASCII 字母数字原样留") {
    CHECK(sessions::MakeSessionSlug("修 bug 123") == "修-bug-123");
}

TEST_CASE("会话 id: 时间戳 + slug") {
    CHECK(sessions::MakeSessionId("20260717-093000", "我的暗号是青龙") == "20260717-093000-我的暗号是青龙");
}

TEST_CASE("TruncateUtf8Chars: 不截/截了补省略号") {
    CHECK(sessions::TruncateUtf8Chars("短句", 40) == "短句");
    const std::string cut = sessions::TruncateUtf8Chars("一二三四五", 3);
    CHECK(cut == "一二三…");
}

// ---------------------------------------------------------------------------
// 成对修补
// ---------------------------------------------------------------------------

TEST_CASE("修补: 孤儿 tool_use 补一条 is_error 的 tool_result") {
    std::vector<api::Message> history;
    history.push_back(UserText("列一下文件"));
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    api::ToolUseBlock use;
    use.id = "toolu_断掉";
    use.name = "run_command";
    use.input = nlohmann::json{{"command", "dir"}};
    assistant.content.push_back(use);
    history.push_back(assistant);  // 到此存档中断:没有 tool_result

    const int repaired = sessions::RepairToolPairs(history);
    CHECK(repaired == 1);
    REQUIRE(history.size() == 3);
    CHECK(history[2].role == api::Role::User);
    REQUIRE(history[2].content.size() == 1);
    const auto* patch = std::get_if<api::ToolResultBlock>(&history[2].content[0]);
    REQUIRE(patch != nullptr);
    CHECK(patch->tool_use_id == "toolu_断掉");
    CHECK(patch->is_error);
    CHECK(patch->content == "[会话恢复:结果缺失]");
}

TEST_CASE("修补: 下一条 user 消息已有部分结果,只补缺的那几个,补在开头") {
    std::vector<api::Message> history;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"id_a", "read_file", nlohmann::json::object()});
    assistant.content.push_back(api::ToolUseBlock{"id_b", "search", nlohmann::json::object()});
    history.push_back(assistant);
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::ToolResultBlock{"id_a", "读到了", false});
    history.push_back(user);

    const int repaired = sessions::RepairToolPairs(history);
    CHECK(repaired == 1);
    REQUIRE(history.size() == 2);
    REQUIRE(history[1].content.size() == 2);
    const auto* first = std::get_if<api::ToolResultBlock>(&history[1].content[0]);
    REQUIRE(first != nullptr);
    CHECK(first->tool_use_id == "id_b");  // 补的排最前
    CHECK(first->is_error);
}

TEST_CASE("修补: 成对齐全的历史一动不动") {
    std::vector<api::Message> history;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"id_1", "read_file", nlohmann::json::object()});
    history.push_back(assistant);
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::ToolResultBlock{"id_1", "内容", false});
    history.push_back(user);
    history.push_back(AssistantText("看完了。"));

    CHECK(sessions::RepairToolPairs(history) == 0);
    CHECK(history.size() == 3);
}

TEST_CASE("修补: 孤儿 tool_result(对不上任何 tool_use)删掉") {
    std::vector<api::Message> history;
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::ToolResultBlock{"id_不存在", "幽灵结果", false});
    user.content.push_back(api::TextBlock{"顺带一句话"});
    history.push_back(user);

    sessions::RepairToolPairs(history);
    REQUIRE(history.size() == 1);
    REQUIRE(history[0].content.size() == 1);
    CHECK(std::holds_alternative<api::TextBlock>(history[0].content[0]));
}

// ---------------------------------------------------------------------------
// 整文件解析
// ---------------------------------------------------------------------------

TEST_CASE("ParseSessionFile: meta + 消息 + 坏行跳过 + 孤儿修补") {
    sessions::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = "/tmp";
    meta.started_at = "2026-07-17 08:00:00";

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"id_x", "run_command", nlohmann::json{{"command", "dir"}}});

    std::string content = sessions::SerializeSessionMeta(meta) + "\n";
    content += sessions::SerializeSessionMessage(UserText("你好"), "t1") + "\n";
    content += "这一行是坏的\n";
    content += sessions::SerializeSessionMessage(assistant, "t2") + "\n";  // 孤儿 tool_use

    const auto session = sessions::ParseSessionFile(content);
    REQUIRE(session.has_value());
    CHECK(session->meta.model == "m1");
    CHECK(session->skipped_lines == 1);
    CHECK(session->repaired == 1);
    REQUIRE(session->messages.size() == 3);  // user + assistant + 修补出来的 user
    // 旧格式(无事件行):全量流水就是原始两条(修补只作用于有效态),
    // 没有压缩、没有标题——按现状全量恢复,一个不坏。
    CHECK(session->all_messages.size() == 2);
    CHECK(session->compact_count == 0);
    CHECK(session->compact_positions.empty());
    CHECK(session->title.empty());
}

TEST_CASE("ParseSessionFile: 首行不是 meta 给 nullopt") {
    CHECK_FALSE(sessions::ParseSessionFile("").has_value());
    CHECK_FALSE(sessions::ParseSessionFile("{\"role\":\"user\",\"content\":[]}\n").has_value());
}

TEST_CASE("ParseSessionFile: 兼容 CRLF 行尾") {
    sessions::SessionMeta meta;
    meta.started_at = "x";
    std::string content = sessions::SerializeSessionMeta(meta) + "\r\n";
    content += sessions::SerializeSessionMessage(UserText("青龙"), "t") + "\r\n";
    const auto session = sessions::ParseSessionFile(content);
    REQUIRE(session.has_value());
    REQUIRE(session->messages.size() == 1);
}

// ---------------------------------------------------------------------------
// 导出 Markdown
// ---------------------------------------------------------------------------

TEST_CASE("导出 MD: 用户/助手分节,工具调用折叠成 details,结果截前 N 行") {
    sessions::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = "/tmp";
    meta.started_at = "2026-07-17 08:00:00";

    std::vector<api::Message> messages;
    messages.push_back(UserText("帮我看文件"));
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"好,我看。"});
    assistant.content.push_back(api::ToolUseBlock{"id_1", "read_file", nlohmann::json{{"path", "a.txt"}}});
    messages.push_back(assistant);
    api::Message tool_user;
    tool_user.role = api::Role::User;
    std::string long_result;
    for (int i = 1; i <= 40; ++i) {
        long_result += "第" + std::to_string(i) + "行\n";
    }
    tool_user.content.push_back(api::ToolResultBlock{"id_1", long_result, false});
    messages.push_back(tool_user);
    messages.push_back(AssistantText("看完了,一共 40 行。"));

    const std::string md = sessions::ExportSessionMarkdown(meta, messages, "20260717-080000-帮我看文件");

    CHECK(md.find("# 会话 20260717-080000-帮我看文件") != std::string::npos);
    CHECK(md.find("## 用户") != std::string::npos);
    CHECK(md.find("## 助手") != std::string::npos);
    CHECK(md.find("<details>") != std::string::npos);
    CHECK(md.find("<summary>工具调用: read_file</summary>") != std::string::npos);
    CHECK(md.find("\"path\": \"a.txt\"") != std::string::npos);
    CHECK(md.find("第30行") != std::string::npos);       // 前 30 行在
    CHECK(md.find("第31行") == std::string::npos);       // 第 31 行起截掉
    CHECK(md.find("已省略其余") != std::string::npos);   // 有省略标注
    // 只装着 tool_result 的 user 消息不单开"用户"节:统共两节用户?不,
    // 只有开头那条真用户消息一节。
    std::size_t user_sections = 0;
    for (std::size_t pos = md.find("## 用户"); pos != std::string::npos; pos = md.find("## 用户", pos + 1)) {
        ++user_sections;
    }
    CHECK(user_sections == 1);
}

// ---------------------------------------------------------------------------
// slash 解析(/sessions //resume //export)
// ---------------------------------------------------------------------------

TEST_CASE("ParseSlashCommand: /sessions") {
    const auto parsed = cli::ParseSlashCommand("/sessions");
    CHECK(parsed.command == cli::SlashCommand::Sessions);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /resume 带编号") {
    const auto parsed = cli::ParseSlashCommand("/resume 3");
    CHECK(parsed.command == cli::SlashCommand::Resume);
    CHECK(parsed.args == "3");
}

TEST_CASE("ParseSlashCommand: /resume 带中文 id") {
    const auto parsed = cli::ParseSlashCommand("/Resume 20260717-093000-我的暗号是青龙");
    CHECK(parsed.command == cli::SlashCommand::Resume);
    CHECK(parsed.args == "20260717-093000-我的暗号是青龙");
}

TEST_CASE("ParseSlashCommand: /export 不带参数") {
    const auto parsed = cli::ParseSlashCommand("/export");
    CHECK(parsed.command == cli::SlashCommand::Export);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /export 带路径") {
    const auto parsed = cli::ParseSlashCommand("/export D:/导出/会话.md");
    CHECK(parsed.command == cli::SlashCommand::Export);
    CHECK(parsed.args == "D:/导出/会话.md");
}

// ---------------------------------------------------------------------------
// compact 事件:序列化往返 / MakeCompactEvent / ApplyCompactEvent
// ---------------------------------------------------------------------------

TEST_CASE("compact 事件序列化往返") {
    sessions::CompactEvent event;
    event.archive = UserText("[对话存档,此前内容已压缩] 暗号玄武,任务是修 bug。");
    event.kept_from = 3;

    const std::string line = sessions::SerializeCompactEvent(event, "2026-07-18 10:00:00");
    CHECK(line.find('\n') == std::string::npos);
    // 事件行不是消息行:旧版本的消息反序列化认不得它,当坏行跳过。
    CHECK_FALSE(sessions::DeserializeSessionMessage(line).has_value());

    const auto parsed = sessions::ParseCompactEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->kept_from == 3);
    CHECK(parsed->archive.role == api::Role::User);
    CHECK(FirstText(parsed->archive) == "[对话存档,此前内容已压缩] 暗号玄武,任务是修 bug。");
}

TEST_CASE("compact 事件解析: 坏行给 nullopt") {
    CHECK_FALSE(sessions::ParseCompactEvent("").has_value());
    CHECK_FALSE(sessions::ParseCompactEvent("不是 JSON").has_value());
    CHECK_FALSE(sessions::ParseCompactEvent("{\"type\":\"compact\"}").has_value());  // 缺 archive/kept_from
    CHECK_FALSE(sessions::ParseCompactEvent("{\"type\":\"compact\",\"archive\":5,\"kept_from\":0}").has_value());
    CHECK_FALSE(
        sessions::ParseCompactEvent("{\"type\":\"compact\",\"archive\":{\"role\":\"user\",\"content\":[]},\"kept_from\":-1}")
            .has_value());
    // 消息行不是 compact 事件
    CHECK_FALSE(sessions::ParseCompactEvent(sessions::SerializeSessionMessage(UserText("你好"), "t")).has_value());
    // title 事件不是 compact 事件
    CHECK_FALSE(sessions::ParseCompactEvent(sessions::SerializeTitleEvent("标题", "t")).has_value());
}

TEST_CASE("MakeCompactEvent 跟 BuildCompactedHistory 对得上账") {
    // 老历史 4 条,压缩保留最后一轮(u2 起):新历史 = [并入存档的 u2, a2]。
    std::vector<api::Message> history;
    history.push_back(UserText("u1"));
    history.push_back(AssistantText("a1"));
    history.push_back(UserText("u2"));
    history.push_back(AssistantText("a2"));
    const auto archive = UserText("[对话存档,此前内容已压缩] 要点");
    const auto new_history = agent::BuildCompactedHistory(history, archive, /*hot_zone_tokens=*/1);
    REQUIRE(new_history.size() == 2);

    const auto event = sessions::MakeCompactEvent(history.size(), new_history);
    CHECK(event.kept_from == 3);  // 老历史里 a2 那条起保留
    CHECK(FirstText(event.archive) == FirstText(new_history[0]));

    // 回放老历史 + 事件,得到的正是内存里的新历史。
    const auto replayed = sessions::ApplyCompactEvent(history, event);
    REQUIRE(replayed.size() == new_history.size());
    CHECK(FirstText(replayed[0]) == FirstText(new_history[0]));
    CHECK(FirstText(replayed[1]) == "a2");
}

TEST_CASE("MakeCompactEvent: 新历史为空的防御路径") {
    const auto event = sessions::MakeCompactEvent(5, {});
    CHECK(event.kept_from == 5);  // 全不保留
    CHECK(event.archive.content.empty());
}

TEST_CASE("ApplyCompactEvent: kept_from 越界夹到列表长度,不越界访问") {
    std::vector<api::Message> effective;
    effective.push_back(UserText("m1"));
    effective.push_back(AssistantText("m2"));
    sessions::CompactEvent event;
    event.archive = UserText("[存档]");
    event.kept_from = 999;
    const auto out = sessions::ApplyCompactEvent(effective, event);
    REQUIRE(out.size() == 1);  // 只剩 archive
    CHECK(FirstText(out[0]) == "[存档]");
}

// ---------------------------------------------------------------------------
// compact 事件回放(整文件)
// ---------------------------------------------------------------------------

namespace {

// 拼一份存档:meta 行 + 给定行。
std::string JoinLines(const std::vector<std::string>& lines) {
    sessions::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = "D:/场子";
    meta.started_at = "2026-07-18 09:00:00";
    std::string content = sessions::SerializeSessionMeta(meta) + "\n";
    for (const auto& line : lines) {
        content += line + "\n";
    }
    return content;
}

}  // namespace

TEST_CASE("回放: 单次压缩,恢复的是压缩后的活状态") {
    // 流水:u1 a1 u2 a2 | compact | u3 a3
    std::vector<api::Message> history{UserText("u1"), AssistantText("a1"), UserText("u2"), AssistantText("a2")};
    const auto new_history = agent::BuildCompactedHistory(history, UserText("[对话存档,此前内容已压缩] 玄武"), /*hot_zone_tokens=*/1);
    const auto event = sessions::MakeCompactEvent(history.size(), new_history);

    std::vector<std::string> lines;
    for (const auto& m : history) {
        lines.push_back(sessions::SerializeSessionMessage(m, "t"));
    }
    lines.push_back(sessions::SerializeCompactEvent(event, "t"));
    lines.push_back(sessions::SerializeSessionMessage(UserText("u3"), "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a3"), "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->compact_count == 1);
    REQUIRE(session->compact_positions.size() == 1);
    CHECK(session->compact_positions[0] == 4);  // 事件落在全量第 4 条之前
    CHECK(session->all_messages.size() == 6);   // 全量流水一条不少
    // 有效态 = [并入存档的 u2, a2, u3, a3]
    REQUIRE(session->messages.size() == 4);
    CHECK(FirstText(session->messages[0]).find("[对话存档,此前内容已压缩] 玄武") == 0);
    CHECK(FirstText(session->messages[0]).find("u2") != std::string::npos);
    CHECK(FirstText(session->messages[1]) == "a2");
    CHECK(FirstText(session->messages[2]) == "u3");
    CHECK(FirstText(session->messages[3]) == "a3");
    CHECK(session->skipped_lines == 0);
}

TEST_CASE("回放: 两次压缩,逐次替换,最后一次说了算") {
    // 第一段:u1 a1 u2 a2 → 压缩 1
    std::vector<api::Message> effective{UserText("u1"), AssistantText("a1"), UserText("u2"), AssistantText("a2")};
    std::vector<std::string> lines;
    for (const auto& m : effective) {
        lines.push_back(sessions::SerializeSessionMessage(m, "t"));
    }
    auto compacted1 = agent::BuildCompactedHistory(effective, UserText("[对话存档,此前内容已压缩] 存档一"), /*hot_zone_tokens=*/1);
    lines.push_back(sessions::SerializeCompactEvent(sessions::MakeCompactEvent(effective.size(), compacted1), "t"));
    effective = compacted1;

    // 第二段:续聊 u3 a3,再压缩一次
    effective.push_back(UserText("u3"));
    effective.push_back(AssistantText("a3"));
    lines.push_back(sessions::SerializeSessionMessage(UserText("u3"), "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a3"), "t"));
    auto compacted2 = agent::BuildCompactedHistory(effective, UserText("[对话存档,此前内容已压缩] 存档二"), /*hot_zone_tokens=*/1);
    lines.push_back(sessions::SerializeCompactEvent(sessions::MakeCompactEvent(effective.size(), compacted2), "t"));
    effective = compacted2;

    // 压缩后再来一条普通消息
    effective.push_back(UserText("u4"));
    lines.push_back(sessions::SerializeSessionMessage(UserText("u4"), "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->compact_count == 2);
    CHECK(session->all_messages.size() == 7);  // u1 a1 u2 a2 u3 a3 u4
    REQUIRE(session->compact_positions.size() == 2);
    CHECK(session->compact_positions[0] == 4);
    CHECK(session->compact_positions[1] == 6);
    // 有效态跟内存里逐步演算的完全一致:[并入存档二的 u3, a3, u4]
    REQUIRE(session->messages.size() == effective.size());
    for (std::size_t i = 0; i < effective.size(); ++i) {
        CHECK(FirstText(session->messages[i]) == FirstText(effective[i]));
    }
    CHECK(FirstText(session->messages[0]).find("存档二") != std::string::npos);
    CHECK(FirstText(session->messages[0]).find("存档一") == std::string::npos);
}

TEST_CASE("回放: 坏 compact 事件行跳过,不影响其余") {
    std::vector<std::string> lines;
    lines.push_back(sessions::SerializeSessionMessage(UserText("u1"), "t"));
    lines.push_back("{\"type\":\"compact\",\"kept_from\":0}");            // 缺 archive
    lines.push_back("{\"type\":\"compact\",\"archive\":\"不是对象\",\"kept_from\":1}");
    lines.push_back("{\"type\":\"没见过的事件\",\"x\":1}");
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a1"), "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->compact_count == 0);
    CHECK(session->skipped_lines == 3);
    REQUIRE(session->messages.size() == 2);
    CHECK(FirstText(session->messages[0]) == "u1");
    CHECK(FirstText(session->messages[1]) == "a1");
}

TEST_CASE("回放: 存档里 kept_from 越界,防御成只剩 archive") {
    sessions::CompactEvent event;
    event.archive = UserText("[对话存档,此前内容已压缩] 只剩我");
    event.kept_from = 999;
    std::vector<std::string> lines;
    lines.push_back(sessions::SerializeSessionMessage(UserText("u1"), "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a1"), "t"));
    lines.push_back(sessions::SerializeCompactEvent(event, "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->compact_count == 1);
    REQUIRE(session->messages.size() == 1);
    CHECK(FirstText(session->messages[0]) == "[对话存档,此前内容已压缩] 只剩我");
    CHECK(session->all_messages.size() == 2);  // 全量照旧
}

// ---------------------------------------------------------------------------
// title 事件
// ---------------------------------------------------------------------------

TEST_CASE("title 事件序列化往返") {
    const std::string line = sessions::SerializeTitleEvent("修会话存档三件套", "2026-07-18 10:00:00");
    CHECK(line.find('\n') == std::string::npos);
    CHECK_FALSE(sessions::DeserializeSessionMessage(line).has_value());  // 不是消息行
    const auto title = sessions::ParseTitleEvent(line);
    REQUIRE(title.has_value());
    CHECK(*title == "修会话存档三件套");
}

TEST_CASE("title 事件解析: 坏行给 nullopt") {
    CHECK_FALSE(sessions::ParseTitleEvent("").has_value());
    CHECK_FALSE(sessions::ParseTitleEvent("{\"type\":\"title\"}").has_value());       // 缺 title
    CHECK_FALSE(sessions::ParseTitleEvent("{\"type\":\"title\",\"title\":42}").has_value());
    CHECK_FALSE(sessions::ParseTitleEvent(sessions::SerializeSessionMessage(UserText("x"), "t")).has_value());
}

TEST_CASE("回放: title 追加,最后一条胜") {
    std::vector<std::string> lines;
    lines.push_back(sessions::SerializeSessionMessage(UserText("u1"), "t"));
    lines.push_back(sessions::SerializeTitleEvent("初稿标题", "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a1"), "t"));
    lines.push_back(sessions::SerializeTitleEvent("定稿标题", "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->title == "定稿标题");
    CHECK(session->messages.size() == 2);      // 事件行不算消息
    CHECK(session->all_messages.size() == 2);
    CHECK(session->skipped_lines == 0);
}

TEST_CASE("回放: 没有 title 事件就是空,展示层回退首句摘要") {
    std::vector<std::string> lines;
    lines.push_back(sessions::SerializeSessionMessage(UserText("首句在此"), "t"));
    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->title.empty());
}

// ---------------------------------------------------------------------------
// cwd 事件(0.27.x:meta.cwd 首行写死,会话中途搬目录靠事件行,最后一条胜)
// ---------------------------------------------------------------------------

TEST_CASE("cwd 事件序列化往返") {
    const std::string line = sessions::SerializeCwdEvent("D:/repo/.lubancode/worktrees/fix-1", "t");
    CHECK(line.find('\n') == std::string::npos);
    CHECK_FALSE(sessions::DeserializeSessionMessage(line).has_value());  // 不是消息行
    const auto cwd = sessions::ParseCwdEvent(line);
    REQUIRE(cwd.has_value());
    CHECK(*cwd == "D:/repo/.lubancode/worktrees/fix-1");
    CHECK_FALSE(sessions::ParseCwdEvent("").has_value());
    CHECK_FALSE(sessions::ParseCwdEvent("{\"type\":\"cwd\"}").has_value());  // 缺 cwd
    CHECK_FALSE(sessions::ParseCwdEvent(sessions::SerializeTitleEvent("x", "t")).has_value());
}

TEST_CASE("回放: cwd 事件追加,最后一条覆盖 meta.cwd") {
    std::vector<std::string> lines;
    lines.push_back(sessions::SerializeSessionMessage(UserText("u1"), "t"));
    lines.push_back(sessions::SerializeCwdEvent("D:/repo/.lubancode/worktrees/fix-1", "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a1"), "t"));
    lines.push_back(sessions::SerializeCwdEvent("D:/repo", "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->meta.cwd == "D:/repo");  // meta 首行是 D:/场子,被最后一条 cwd 事件盖掉
    CHECK(session->messages.size() == 2);   // 事件行不算消息
    CHECK(session->skipped_lines == 0);
}

// ---------------------------------------------------------------------------
// queue 事件(取走即消费单路径二:排队消息落会话存档)
// ---------------------------------------------------------------------------

namespace {

std::vector<sessions::ArchivedQueueItem> SampleQueue() {
    std::vector<sessions::ArchivedQueueItem> items;
    sessions::ArchivedQueueItem main_item;
    main_item.id = 1;
    main_item.text = "排在第一的";
    items.push_back(main_item);
    sessions::ArchivedQueueItem agent_item;
    agent_item.id = 2;
    agent_item.subagent = true;
    agent_item.task_id = 3;
    agent_item.text = "给三号的";
    items.push_back(agent_item);
    sessions::ArchivedQueueItem retried;
    retried.id = 3;
    retried.text = "失败回还过的";
    retried.attempts = 2;
    items.push_back(retried);
    return items;
}

}  // namespace

TEST_CASE("queue 事件序列化往返:main/子代理/回还账都在") {
    const std::string line = sessions::SerializeQueueEvent(SampleQueue(), "2026-08-22 10:00:00");
    CHECK(line.find('\n') == std::string::npos);
    CHECK_FALSE(sessions::DeserializeSessionMessage(line).has_value());  // 不是消息行

    const auto parsed = sessions::ParseQueueEvent(line);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 3);
    CHECK((*parsed)[0].id == 1);
    CHECK_FALSE((*parsed)[0].subagent);
    CHECK((*parsed)[0].text == "排在第一的");
    CHECK((*parsed)[0].attempts == 0);
    CHECK((*parsed)[1].subagent);
    CHECK((*parsed)[1].task_id == 3);
    CHECK((*parsed)[1].text == "给三号的");
    CHECK((*parsed)[2].attempts == 2);  // 自动重试到顶的账也往返

    // 空快照也合法(清账后落的"没排队了")。
    const auto empty = sessions::ParseQueueEvent(sessions::SerializeQueueEvent({}, "t"));
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("queue 事件解析: 坏行/坏条目不废整份") {
    CHECK_FALSE(sessions::ParseQueueEvent("").has_value());
    CHECK_FALSE(sessions::ParseQueueEvent("{\"type\":\"queue\"}").has_value());  // 缺 items
    CHECK_FALSE(sessions::ParseQueueEvent("{\"type\":\"queue\",\"items\":42}").has_value());
    CHECK_FALSE(sessions::ParseQueueEvent(sessions::SerializeTitleEvent("x", "t")).has_value());

    // 单条缺 text / 空 text / id=0 / 认不出的目标号:跳过那一条,其余保住。
    const std::string mixed =
        "{\"type\":\"queue\",\"items\":["
        "{\"id\":7,\"target\":\"main\",\"text\":\"好的条目\"},"
        "{\"id\":8,\"target\":\"main\"},"
        "{\"id\":0,\"target\":\"main\",\"text\":\"没 id 的\"},"
        "{\"id\":9,\"target\":\"#abc\",\"text\":\"目标号认不出\"},"
        "{\"id\":10,\"target\":\"#5\",\"text\":\"五号的\"}"
        "],\"ts\":\"t\"}";
    const auto parsed = sessions::ParseQueueEvent(mixed);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
    CHECK((*parsed)[0].text == "好的条目");
    CHECK((*parsed)[1].subagent);
    CHECK((*parsed)[1].task_id == 5);
}

TEST_CASE("回放: queue 事件快照式,最后一条胜;消息账不受牵连") {
    std::vector<std::string> lines;
    lines.push_back(sessions::SerializeSessionMessage(UserText("u1"), "t"));
    lines.push_back(sessions::SerializeQueueEvent(SampleQueue(), "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a1"), "t"));
    // 送走一条之后的快照:只剩给三号的。
    std::vector<sessions::ArchivedQueueItem> after_deliver;
    sessions::ArchivedQueueItem only_agent;
    only_agent.id = 2;
    only_agent.subagent = true;
    only_agent.task_id = 3;
    only_agent.text = "给三号的";
    after_deliver.push_back(only_agent);
    lines.push_back(sessions::SerializeQueueEvent(after_deliver, "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->messages.size() == 2);  // 事件行不算消息
    CHECK(session->skipped_lines == 0);
    REQUIRE(session->queued_messages.size() == 1);  // 最后一条快照说了算
    CHECK(session->queued_messages[0].task_id == 3);

    // 没有 queue 行的老档:空表,老场子照旧全量恢复。
    std::vector<std::string> legacy;
    legacy.push_back(sessions::SerializeSessionMessage(UserText("u1"), "t"));
    const auto legacy_session = sessions::ParseSessionFile(JoinLines(legacy));
    REQUIRE(legacy_session.has_value());
    CHECK(legacy_session->queued_messages.empty());
}

TEST_CASE("queue 事件经真 SessionStore 落盘再读回:排队→退出→resume 接得回") {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
                                      ("lubancode_queue_event_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir, ec);
    sessions::SessionStore store(dir.string());
    REQUIRE(store.Begin(sessions::SessionMeta{}, "sess-queue-test"));
    CHECK(store.AppendMessage(UserText("第一问")));
    CHECK(store.AppendQueueEvent(SampleQueue()));
    store.Reset();  // 模拟 /exit 关档;柄先关,目录才能收

    const auto bytes = sessions::ReadSessionFileBytes(dir.string() + "/sess-queue-test.jsonl");
    REQUIRE(bytes.has_value());
    const auto session = sessions::ParseSessionFile(*bytes);
    REQUIRE(session.has_value());
    REQUIRE(session->queued_messages.size() == 3);
    CHECK(session->queued_messages[0].text == "排在第一的");
    CHECK(session->queued_messages[2].attempts == 2);
    CHECK(session->messages.size() == 1);  // 排队快照不是消息

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 路径归一化 / 中间缩略
// ---------------------------------------------------------------------------

TEST_CASE("NormalizePathForCompare: 斜杠方向、大小写、尾斜杠都归一") {
    CHECK(sessions::NormalizePathForCompare("C:\\Foo\\Bar") == sessions::NormalizePathForCompare("c:/foo/bar/"));
    CHECK(sessions::NormalizePathForCompare("C:/foo/./bar/../bar") == sessions::NormalizePathForCompare("C:\\foo\\bar"));
    CHECK(sessions::NormalizePathForCompare("C:/工程/子目录") == sessions::NormalizePathForCompare("C:\\工程\\子目录\\"));
    CHECK(sessions::NormalizePathForCompare("C:/foo") != sessions::NormalizePathForCompare("C:/foobar"));
    CHECK(sessions::NormalizePathForCompare("").empty());
}

TEST_CASE("AbbreviateUtf8Middle: 不超长原样,超长头尾留、中间省略") {
    CHECK(sessions::AbbreviateUtf8Middle("short", 10) == "short");
    CHECK(sessions::AbbreviateUtf8Middle("abcdefghij", 5) == "ab…ij");
    // 中文按码点算,不从多字节字符中间掐断
    CHECK(sessions::AbbreviateUtf8Middle("D:/一二三四五六七八九十/工程", 9) == "D:/一…十/工程");
    CHECK(sessions::AbbreviateUtf8Middle("一二三四", 4) == "一二三四");
}

// ---------------------------------------------------------------------------
// ListSessions:cwd 过滤(真扫临时目录)
// ---------------------------------------------------------------------------

TEST_CASE("ListSessions: cwd 过滤只出本目录,all 全出且带 cwd/title") {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "lubancode_test_sessions_cwd";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    const std::string dir = PathUtf8(base);

    const auto write_session = [&](const std::string& id, const std::string& cwd, const std::string& text,
                                    const std::string& title) {
        sessions::SessionMeta meta;
        meta.wire = "anthropic";
        meta.model = "m";
        meta.cwd = cwd;
        meta.started_at = "2026-07-18 08:00:00";
        std::ofstream f(base / (id + ".jsonl"), std::ios::binary);
        f << sessions::SerializeSessionMeta(meta) << "\n";
        f << sessions::SerializeSessionMessage(UserText(text), "t") << "\n";
        if (!title.empty()) {
            f << sessions::SerializeTitleEvent(title, "t") << "\n";
        }
    };
    // 混录三个目录的场子;甲目录两场,故意用不同斜杠方向/大小写/尾斜杠。
    // 文件名用 ASCII(narrow 构造 fs::path 走 ANSI 代码页,中文名会抛),
    // 中文覆盖留在 meta.cwd / 消息正文里。
    write_session("20260718-090000-a", "D:\\场子甲", "甲一", "");
    write_session("20260718-090001-b", "D:/场子乙", "乙一", "");
    write_session("20260718-090002-c", "D:\\场子甲\\", "甲二", "甲二的标题");
    write_session("20260718-090003-d", "E:/场子丙", "丙一", "");

    // 过滤:斜杠方向和大小写都跟 meta 里写的不一样,照样对上。
    const auto mine = sessions::ListSessions(dir, 20, "d:/场子甲");
    REQUIRE(mine.size() == 2);
    CHECK(mine[0].id == "20260718-090002-c");  // 倒序,新的在前
    CHECK(mine[0].title == "甲二的标题");       // title 事件进列表
    CHECK(mine[0].message_count == 1);          // 事件行不算消息
    CHECK(mine[1].id == "20260718-090000-a");
    CHECK(mine[1].title.empty());               // 没设标题,展示层回退首句摘要
    CHECK(mine[1].first_user_text == "甲一");

    // 不过滤(/sessions all):四场全出,每条带 cwd。
    const auto all = sessions::ListSessions(dir, 20);
    REQUIRE(all.size() == 4);
    CHECK(all[0].id == "20260718-090003-d");
    CHECK(all[0].cwd == "E:/场子丙");  // 原样保留,展示层自己缩略
    CHECK(all[3].cwd == "D:\\场子甲");

    // 别的目录过滤:只出乙那一场。
    const auto other = sessions::ListSessions(dir, 20, "D:\\场子乙\\");
    REQUIRE(other.size() == 1);
    CHECK(other[0].id == "20260718-090001-b");

    fs::remove_all(base, ec);
}

// ---------------------------------------------------------------------------
// 导出 Markdown:标题大标题 + 压缩标注
// ---------------------------------------------------------------------------

TEST_CASE("导出 MD: title 当大标题,压缩发生点插标注行") {
    sessions::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = "/tmp";
    meta.started_at = "2026-07-18 08:00:00";

    std::vector<api::Message> messages;
    messages.push_back(UserText("第一问"));
    messages.push_back(AssistantText("第一答"));
    messages.push_back(UserText("第二问"));
    messages.push_back(AssistantText("第二答"));

    const std::string md = sessions::ExportSessionMarkdown(meta, messages, "20260718-080000-x",
                                                         /*max_result_lines=*/30, "玄武任务", {2});
    CHECK(md.rfind("# 玄武任务", 0) == 0);                       // 标题当大标题
    CHECK(md.find("- 会话: 20260718-080000-x") != std::string::npos);
    CHECK(md.find("# 会话 20260718-080000-x") == std::string::npos);
    const std::size_t note = md.find("> ⚡ 此处发生过一次上下文压缩");
    REQUIRE(note != std::string::npos);
    CHECK(note > md.find("第一答"));  // 标注在第一答之后
    CHECK(note < md.find("第二问"));  // 在第二问之前

    // 没标题、没压缩:老样子,一行标注都不多。
    const std::string plain = sessions::ExportSessionMarkdown(meta, messages, "20260718-080000-x");
    CHECK(plain.rfind("# 会话 20260718-080000-x", 0) == 0);
    CHECK(plain.find("上下文压缩") == std::string::npos);
}

TEST_CASE("导出 MD: 压缩发生在末尾,标注补在最后") {
    sessions::SessionMeta meta;
    meta.started_at = "x";
    std::vector<api::Message> messages;
    messages.push_back(UserText("唯一一问"));
    const std::string md = sessions::ExportSessionMarkdown(meta, messages, "id", 30, "", {1});
    const std::size_t note = md.find("> ⚡ 此处发生过一次上下文压缩");
    REQUIRE(note != std::string::npos);
    CHECK(note > md.find("唯一一问"));
}

// ---------------------------------------------------------------------------
// slash 解析(/sessions all //title)
// ---------------------------------------------------------------------------

TEST_CASE("ParseSlashCommand: /sessions all") {
    const auto parsed = cli::ParseSlashCommand("/sessions all");
    CHECK(parsed.command == cli::SlashCommand::Sessions);
    CHECK(parsed.args == "all");
}

TEST_CASE("ParseSlashCommand: /title 裸敲与带标题") {
    const auto bare = cli::ParseSlashCommand("/title");
    CHECK(bare.command == cli::SlashCommand::Title);
    CHECK(bare.args.empty());

    const auto with_args = cli::ParseSlashCommand("/Title 我的 存档 标题");
    CHECK(with_args.command == cli::SlashCommand::Title);
    CHECK(with_args.args == "我的 存档 标题");
}

// ---------------------------------------------------------------------------
// compact_v2 事件(0.31.x 第三期):分层压缩的完整记账与回放。
// ---------------------------------------------------------------------------

TEST_CASE("compact_v2: 序列化/解析往返,epoch/manifest/metrics 都在") {
    sessions::CompactV2Event event;
    event.archive = UserText("[对话存档,此前内容已压缩] 正文");
    event.kept_from = 3;
    event.epoch = 2;
    event.manifest = nlohmann::json::parse(R"({"goal":"修好压缩","open_items":["补测试"]})");
    event.metrics = nlohmann::json::parse(R"({"chunks":4,"reduce_passes":1,"trigger":"midturn"})");

    const auto parsed = sessions::ParseCompactV2Event(sessions::SerializeCompactV2Event(event, "t"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->kept_from == 3);
    CHECK(parsed->epoch == 2);
    CHECK(parsed->manifest["goal"] == "修好压缩");
    CHECK(parsed->metrics["chunks"] == 4);
    CHECK(FirstText(parsed->archive) == "[对话存档,此前内容已压缩] 正文");
}

TEST_CASE("compact_v2: type 不对 / 缺 archive、kept_from → nullopt") {
    CHECK_FALSE(sessions::ParseCompactV2Event(sessions::SerializeCompactEvent(sessions::CompactEvent{}, "t")).has_value());
    CHECK_FALSE(sessions::ParseCompactV2Event("{bad json").has_value());
    CHECK_FALSE(sessions::ParseCompactV2Event(R"({"type":"compact_v2","archive":{"role":"user","content":[]}})")
                    .has_value());  // 缺 kept_from
}

TEST_CASE("compact_v2: UpgradeToV2/AsCompactEvent 折返,v1 回放路共用") {
    sessions::CompactEvent v1;
    v1.archive = UserText("[存档]");
    v1.kept_from = 2;
    const auto v2 = sessions::UpgradeToV2(v1, /*epoch=*/5, nlohmann::json::object(), nlohmann::json::object());
    CHECK(v2.epoch == 5);
    const auto back = sessions::AsCompactEvent(v2);
    CHECK(back.kept_from == 2);
    CHECK(FirstText(back.archive) == "[存档]");
}

TEST_CASE("回放: v1 与 v2 混排,活状态逐次替换,epoch/manifest 记账") {
    // 流水:u1 a1 | compact(v1) | u2 a2 | compact_v2 | u3 a3
    std::vector<api::Message> first{UserText("u1"), AssistantText("a1")};
    auto compacted1 = agent::BuildCompactedHistory(first, UserText("[对话存档,此前内容已压缩] 存档一"),
                                                   /*hot_zone_tokens=*/1);
    sessions::CompactEvent v1_event = sessions::MakeCompactEvent(first.size(), compacted1);

    std::vector<api::Message> effective = compacted1;
    effective.push_back(UserText("u2"));
    effective.push_back(AssistantText("a2"));
    auto compacted2 = agent::BuildCompactedHistory(effective, UserText("[对话存档,此前内容已压缩] 存档二"),
                                                   /*hot_zone_tokens=*/1);
    const auto v2_event =
        sessions::UpgradeToV2(sessions::MakeCompactEvent(effective.size(), compacted2), /*epoch=*/2,
                           nlohmann::json::parse(R"({"goal":"二期目标","open_items":["收尾"]})"),
                           nlohmann::json::parse(R"({"chunks":3,"hierarchical":true})"));

    std::vector<std::string> lines;
    for (const auto& m : first) {
        lines.push_back(sessions::SerializeSessionMessage(m, "t"));
    }
    lines.push_back(sessions::SerializeCompactEvent(v1_event, "t"));
    for (std::size_t i = compacted1.size(); i < effective.size(); ++i) {
        lines.push_back(sessions::SerializeSessionMessage(effective[i], "t"));
    }
    lines.push_back(sessions::SerializeCompactV2Event(v2_event, "t"));
    lines.push_back(sessions::SerializeSessionMessage(UserText("u3"), "t"));
    lines.push_back(sessions::SerializeSessionMessage(AssistantText("a3"), "t"));

    const auto session = sessions::ParseSessionFile(JoinLines(lines));
    REQUIRE(session.has_value());
    CHECK(session->compact_count == 2);       // v1 + v2 都算
    CHECK(session->compact_epoch == 2);       // v2 记的序号
    CHECK(session->last_compact_manifest["goal"] == "二期目标");
    REQUIRE(session->compact_positions.size() == 2);
    CHECK(session->all_messages.size() == 6); // 全量流水:u1 a1 u2 a2 u3 a3 一条不少
    // 有效态 = [并入存档二的 u2, a2, u3, a3](两次压缩逐次替换,最后说了算)。
    REQUIRE(session->messages.size() == 4);
    CHECK(FirstText(session->messages[0]).find("存档二") != std::string::npos);
    CHECK(FirstText(session->messages[2]) == "u3");
}

// ---------------------------------------------------------------------------
// ExtractPromptHistory(0.30.x Ctrl+R 反向搜索):只读事件账的用户提问行。
// ---------------------------------------------------------------------------
TEST_CASE("ExtractPromptHistory:只收用户纯文本提问,事件行/工具回执/slash 不进") {
    using sessions::SerializeSessionMessage;
    const api::Message user_text = api::Message{api::Role::User, {api::TextBlock{"怎么配代理"}}};
    const api::Message user_slash = api::Message{api::Role::User, {api::TextBlock{"/model gpt"}}};
    const api::Message user_tool_result =
        api::Message{api::Role::User, {api::ToolResultBlock{"tool-1", "命令输出", false}}};
    const api::Message assistant_text =
        api::Message{api::Role::Assistant, {api::TextBlock{"这样配:……"}}};
    const std::string jsonl =
        SerializeSessionMeta(sessions::SessionMeta{1, "anthropic", "m", "d", "t"}) + "\n" +
        SerializeSessionMessage(user_text, "2026-08-16 10:00:00") + "\n" +
        sessions::SerializeTitleEvent("标题", "2026-08-16 10:00:01") + "\n" +
        SerializeSessionMessage(user_slash, "2026-08-16 10:01:00") + "\n" +
        SerializeSessionMessage(assistant_text, "2026-08-16 10:02:00") + "\n" +
        SerializeSessionMessage(user_tool_result, "2026-08-16 10:03:00") + "\n" + "{bad json line" +
        "\n";
    const auto records = sessions::ExtractPromptHistory(jsonl);
    REQUIRE(records.size() == 1);
    CHECK(records[0].text == "怎么配代理");
    CHECK(records[0].ts == "2026-08-16 10:00:00");

    // 空串/纯空白提问不进;多行提问整段收(拼 '\n'),首尾空白剥掉。
    const api::Message multiline = api::Message{api::Role::User, {api::TextBlock{"第一行\n第二行\n\n  "}}};
    const api::Message blank = api::Message{api::Role::User, {api::TextBlock{"   "}}};
    const std::string jsonl2 =
        SerializeSessionMessage(multiline, "ts1") + "\n" + SerializeSessionMessage(blank, "ts2") + "\n";
    const auto records2 = sessions::ExtractPromptHistory(jsonl2);
    REQUIRE(records2.size() == 1);
    CHECK(records2[0].text == "第一行\n第二行");

    // loop 单:scheduled tick 的宿主前缀消息与 goal synthetic 不进
    // (Ctrl+R 满屏重复句的防线)。
    const api::Message loop_tick =
        api::Message{api::Role::User, {api::TextBlock{"[定时循环 tick]\ntask_id: loop-3\n..."}}};
    const api::Message goal_turn = api::Message{api::Role::User, {api::TextBlock{"[goal goal-3 r1 iteration 2]\n目标:..."}}};
    const std::string jsonl3 =
        SerializeSessionMessage(loop_tick, "ts3") + "\n" + SerializeSessionMessage(goal_turn, "ts4") + "\n";
    CHECK(sessions::ExtractPromptHistory(jsonl3).empty());
}
