// agent/session_store:会话存档的序列化/反序列化/成对修补/slug/导出,
// 全是纯函数,不碰磁盘(SessionStore 磁盘薄壳的行为靠集成验证,不在这儿测)。

#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "agent/session_store.hpp"
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

}  // namespace

// ---------------------------------------------------------------------------
// 序列化往返
// ---------------------------------------------------------------------------

TEST_CASE("消息序列化往返: 纯文本(中文)") {
    const api::Message original = UserText("我的暗号是青龙,记住了。");
    const std::string line = agent::SerializeSessionMessage(original, "2026-07-17 10:00:00");
    CHECK(line.find('\n') == std::string::npos);  // 一行一条,不许夹换行

    const auto parsed = agent::DeserializeSessionMessage(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->role == api::Role::User);
    REQUIRE(parsed->content.size() == 1);
    const auto* tb = std::get_if<api::TextBlock>(&parsed->content[0]);
    REQUIRE(tb != nullptr);
    CHECK(tb->text == "我的暗号是青龙,记住了。");
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

    const auto parsed = agent::DeserializeSessionMessage(agent::SerializeSessionMessage(original, "ts"));
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

    const auto parsed = agent::DeserializeSessionMessage(agent::SerializeSessionMessage(original, "ts"));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->content.size() == 1);
    const auto* got = std::get_if<api::ToolResultBlock>(&parsed->content[0]);
    REQUIRE(got != nullptr);
    CHECK(got->tool_use_id == "toolu_01");
    CHECK(got->content == "第一行\n第二行(中文内容)");
    CHECK(got->is_error);
}

TEST_CASE("反序列化: 坏行给 nullopt,不抛异常") {
    CHECK_FALSE(agent::DeserializeSessionMessage("").has_value());
    CHECK_FALSE(agent::DeserializeSessionMessage("不是 JSON").has_value());
    CHECK_FALSE(agent::DeserializeSessionMessage("{\"role\":\"盗号的\",\"content\":[]}").has_value());
    CHECK_FALSE(agent::DeserializeSessionMessage("{\"role\":\"user\"}").has_value());  // 缺 content
}

// ---------------------------------------------------------------------------
// meta
// ---------------------------------------------------------------------------

TEST_CASE("meta 序列化往返") {
    agent::SessionMeta meta;
    meta.version = 1;
    meta.wire = "anthropic";
    meta.model = "MiniMax-M2";
    meta.cwd = "D:\\工程\\lubancode";
    meta.started_at = "2026-07-17 09:30:00";

    const auto parsed = agent::ParseSessionMeta(agent::SerializeSessionMeta(meta));
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    CHECK(parsed->wire == "anthropic");
    CHECK(parsed->model == "MiniMax-M2");
    CHECK(parsed->cwd == "D:\\工程\\lubancode");
    CHECK(parsed->started_at == "2026-07-17 09:30:00");
}

TEST_CASE("meta 解析: 坏行/缺 version 给 nullopt") {
    CHECK_FALSE(agent::ParseSessionMeta("").has_value());
    CHECK_FALSE(agent::ParseSessionMeta("垃圾").has_value());
    CHECK_FALSE(agent::ParseSessionMeta("{\"wire\":\"anthropic\"}").has_value());
    // 消息行不是 meta(没有 version 字段)
    CHECK_FALSE(agent::ParseSessionMeta("{\"role\":\"user\",\"content\":[],\"ts\":\"x\"}").has_value());
}

// ---------------------------------------------------------------------------
// slug / 会话 id
// ---------------------------------------------------------------------------

TEST_CASE("slug: 中文按码点截前 20 字,不从多字节字符中间掐断") {
    const std::string text = "这是一条很长很长的中文消息用来测试截断是否安全无虞";  // >20 字
    const std::string slug = agent::MakeSessionSlug(text);
    CHECK(slug == "这是一条很长很长的中文消息用来测试截断是");  // 恰 20 个字
    // 截出来的必须是合法 UTF-8:字节数是 3 的倍数(全汉字)
    CHECK(slug.size() == 20 * 3);
}

TEST_CASE("slug: 空白和文件名危险字符换成 -,连续并一个,首尾剥掉") {
    CHECK(agent::MakeSessionSlug("fix: a/b\\c?") == "fix-a-b-c");
    CHECK(agent::MakeSessionSlug("  hello   world  ") == "hello-world");
    CHECK(agent::MakeSessionSlug("<>:\"|?*") == "untitled");
    CHECK(agent::MakeSessionSlug("") == "untitled");
}

TEST_CASE("slug: 中英混排,ASCII 字母数字原样留") {
    CHECK(agent::MakeSessionSlug("修 bug 123") == "修-bug-123");
}

TEST_CASE("会话 id: 时间戳 + slug") {
    CHECK(agent::MakeSessionId("20260717-093000", "我的暗号是青龙") == "20260717-093000-我的暗号是青龙");
}

TEST_CASE("TruncateUtf8Chars: 不截/截了补省略号") {
    CHECK(agent::TruncateUtf8Chars("短句", 40) == "短句");
    const std::string cut = agent::TruncateUtf8Chars("一二三四五", 3);
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

    const int repaired = agent::RepairToolPairs(history);
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

    const int repaired = agent::RepairToolPairs(history);
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

    CHECK(agent::RepairToolPairs(history) == 0);
    CHECK(history.size() == 3);
}

TEST_CASE("修补: 孤儿 tool_result(对不上任何 tool_use)删掉") {
    std::vector<api::Message> history;
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::ToolResultBlock{"id_不存在", "幽灵结果", false});
    user.content.push_back(api::TextBlock{"顺带一句话"});
    history.push_back(user);

    agent::RepairToolPairs(history);
    REQUIRE(history.size() == 1);
    REQUIRE(history[0].content.size() == 1);
    CHECK(std::holds_alternative<api::TextBlock>(history[0].content[0]));
}

// ---------------------------------------------------------------------------
// 整文件解析
// ---------------------------------------------------------------------------

TEST_CASE("ParseSessionFile: meta + 消息 + 坏行跳过 + 孤儿修补") {
    agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = "/tmp";
    meta.started_at = "2026-07-17 08:00:00";

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"id_x", "run_command", nlohmann::json{{"command", "dir"}}});

    std::string content = agent::SerializeSessionMeta(meta) + "\n";
    content += agent::SerializeSessionMessage(UserText("你好"), "t1") + "\n";
    content += "这一行是坏的\n";
    content += agent::SerializeSessionMessage(assistant, "t2") + "\n";  // 孤儿 tool_use

    const auto session = agent::ParseSessionFile(content);
    REQUIRE(session.has_value());
    CHECK(session->meta.model == "m1");
    CHECK(session->skipped_lines == 1);
    CHECK(session->repaired == 1);
    REQUIRE(session->messages.size() == 3);  // user + assistant + 修补出来的 user
}

TEST_CASE("ParseSessionFile: 首行不是 meta 给 nullopt") {
    CHECK_FALSE(agent::ParseSessionFile("").has_value());
    CHECK_FALSE(agent::ParseSessionFile("{\"role\":\"user\",\"content\":[]}\n").has_value());
}

TEST_CASE("ParseSessionFile: 兼容 CRLF 行尾") {
    agent::SessionMeta meta;
    meta.started_at = "x";
    std::string content = agent::SerializeSessionMeta(meta) + "\r\n";
    content += agent::SerializeSessionMessage(UserText("青龙"), "t") + "\r\n";
    const auto session = agent::ParseSessionFile(content);
    REQUIRE(session.has_value());
    REQUIRE(session->messages.size() == 1);
}

// ---------------------------------------------------------------------------
// 导出 Markdown
// ---------------------------------------------------------------------------

TEST_CASE("导出 MD: 用户/助手分节,工具调用折叠成 details,结果截前 N 行") {
    agent::SessionMeta meta;
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

    const std::string md = agent::ExportSessionMarkdown(meta, messages, "20260717-080000-帮我看文件");

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
