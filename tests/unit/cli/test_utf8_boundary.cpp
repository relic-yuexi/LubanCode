// 工具输出非法 UTF-8 的信任边界测试(read_file 编码处置、RunOneTool 规范化、
// JSON 序列化窄边界)。病根与规格见 todos/工具输出非法UTF8导致会话退出.todo:
// read_file 曾把 GBK 字节原样拼进 Result.content,recorder/会话落盘的
// nlohmann::json dump 抛 type_error.316 穿透主循环,整场会话被顶层 catch
// 掐死。这里把那条路逐段钉死:文件入口不放进坏字节,公共边界统一清洗,
// 漏网坏串在序列化窄边界转成可回传错误或清洗落盘,进程不退出。
//
// fixture 全部现造(GBK 双字节、残缺三字节、孤立续字节、NUL、边界横跨),
// 不拿任何真实 worktree 当测试依赖。

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "agent/workflow_recorder.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/request.hpp"
#include "api/responses/request.hpp"
#include "api/types.hpp"
#include "platform/json_safe.hpp"
#include "platform/text_encoding.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;
using lubancode::platform::DumpJsonSanitized;
using lubancode::platform::FindInvalidUtf8Field;
using lubancode::platform::IsValidUtf8;
using lubancode::platform::SanitizeExternalText;

namespace {

// 用完即删的临时文件,内容按字节原样写(连 NUL、GBK 字节一起)。
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_utf8_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".bin");
        std::ofstream file(path_, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::string Utf8Path() const {
        const std::u8string u8 = path_.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

private:
    std::filesystem::path path_;
};

tools::Tool::Result RunReadFile(const std::string& path) {
    lubancode::tools::ReadFileTool tool;
    nlohmann::json input;
    input["path"] = path;
    return tool.execute(input);
}

// 按脚本吐事件的假后端(与 test_loop.cpp 同款),不碰网络。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

// 固定吐一份(可以是坏字节的)结果的假工具。
class BadOutputTool : public tools::Tool {
public:
    explicit BadOutputTool(std::string content) : content_(std::move(content)) {}

    std::string name() const override { return "bad_output"; }
    std::string description() const override { return "fake tool with dirty output"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }

    tools::Tool::Result execute(const nlohmann::json&) override { return {content_, false}; }

private:
    std::string content_;
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// read_file:编码处置的明规矩
// ---------------------------------------------------------------------------

TEST_CASE("read_file: 合法 UTF-8 中文逐字节保真") {
    TempFile file("第一行 汉字\nsecond line\n第三行\n");
    const auto result = RunReadFile(file.Utf8Path());
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content == "     1\t第一行 汉字\n     2\tsecond line\n     3\t第三行\n");
    CHECK(IsValidUtf8(result.content));
}

TEST_CASE("read_file: UTF-8 BOM 剥掉,不混进正文") {
    TempFile file("\xEF\xBB\xBF"
                  "第一行\nsecond\n");
    const auto result = RunReadFile(file.Utf8Path());
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("\xEF\xBB\xBF") == std::string::npos);
    CHECK(result.content == "     1\t第一行\n     2\tsecond\n");
}

TEST_CASE("read_file: GBK 文件返回明确错误,坏字节不进结果") {
    // "你好" 的 GBK 编码。
    TempFile file("开头\n\xC4\xE3\xBA\xC3\n结尾\n");
    const auto result = RunReadFile(file.Utf8Path());
    REQUIRE(result.is_error);
    CHECK(IsValidUtf8(result.content));                       // 错误文本本身必须合法
    CHECK(result.content.find("UTF-8") != std::string::npos); // 错误里明说编码问题
    CHECK_NOTHROW(nlohmann::json(result.content).dump());     // 错误文本直接 dump 不抛
}

TEST_CASE("read_file: 残缺三字节/孤立续字节/随机坏字节都明确报错") {
    const std::string cases[] = {
        "abc\xE4\xBD",        // "你" 的前两个字节,残缺三字节序列
        "x\x80y\n",           // 孤立续字节
        "\xFF\xFE\x01\n",     // 随机坏字节
        "\xE4\xBD\xA0\xE4",   // 合法"你"后跟残缺尾巴
    };
    for (const std::string& content : cases) {
        CAPTURE(content);
        TempFile file(content + "\n");
        const auto result = RunReadFile(file.Utf8Path());
        CHECK(result.is_error);
        CHECK(IsValidUtf8(result.content));
    }
}

TEST_CASE("read_file: 合法 UTF-8 里夹坏字节(混合编码)报错") {
    TempFile file("汉字夹着\xC4\xE3坏字节\n");
    const auto result = RunReadFile(file.Utf8Path());
    REQUIRE(result.is_error);
    CHECK(IsValidUtf8(result.content));
    CHECK(result.content.find("第 1 行") != std::string::npos);  // 指明坏字节在哪一行
}

TEST_CASE("read_file: 夹 NUL 的二进制文件拒绝") {
    TempFile file(std::string("abc\0def\n", 8));
    const auto result = RunReadFile(file.Utf8Path());
    REQUIRE(result.is_error);
    CHECK(result.content.find("NUL") != std::string::npos);
    CHECK(IsValidUtf8(result.content));
}

TEST_CASE("read_file: 坏序列与中文横跨 1024/4096/65536 字节边界都被发现") {
    for (const std::size_t boundary : {std::size_t{1024}, std::size_t{4096}, std::size_t{65536}}) {
        CAPTURE(boundary);
        // 输出的行号前缀占 7 字节("     1\t");让坏字节正好骑在输出字节流
        // 的边界上。原 bug 的报错恰好是 "invalid UTF-8 byte at index 1024"。
        std::string dirty(boundary - 8, 'a');
        dirty += "\xC4\xE3";  // GBK 双字节横跨边界
        TempFile file(dirty + "\n");
        const auto result = RunReadFile(file.Utf8Path());
        CHECK(result.is_error);

        // 同样位置换成合法中文:照常读出,逐字节保真。
        std::string clean(boundary - 8, 'a');
        clean += "汉";  // 三字节汉字横跨边界
        TempFile ok_file(clean + "\n");
        const auto ok_result = RunReadFile(ok_file.Utf8Path());
        REQUIRE_FALSE(ok_result.is_error);
        CHECK(ok_result.content.find("汉") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 公共边界的规范化
// ---------------------------------------------------------------------------

TEST_CASE("SanitizeExternalText: 混合内容只换坏字节,合法中文保真") {
    const std::string mixed = "汉字夹着\xC4\xE3坏字节";
    REQUIRE_FALSE(IsValidUtf8(mixed));
    const std::string cleaned = SanitizeExternalText(mixed);
    CHECK(IsValidUtf8(cleaned));
    CHECK(cleaned.find("汉字夹着") != std::string::npos);
    CHECK(cleaned.find("坏字节") != std::string::npos);
    CHECK(cleaned.find("\xC4\xE3") == std::string::npos);
}

TEST_CASE("SanitizeExternalText: 大内容里 65535 处的坏字节也清洗得到") {
    std::string big(65535, 'x');
    big += "\xC4\xE3";
    big += "end";
    const std::string cleaned = SanitizeExternalText(big);
    CHECK(IsValidUtf8(cleaned));
    CHECK(cleaned.find("end") != std::string::npos);
}

TEST_CASE("RunOneTool 信任边界: on_tool_done 与下一轮请求拿同一份已合法内容") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_1", "bad_output"),
        TextOnlyScript("收工"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<BadOutputTool>("好开头 " + std::string("\xC4\xE3\xBA\xC3") + " 尾巴"));

    agent::Agent loop(backend, registry, "test-model", "system prompt");

    std::vector<std::string> order;
    std::string done_content;
    agent::Callbacks callbacks;
    callbacks.on_post_tool_hook = [&order](const std::string&, const std::string&, const nlohmann::json&,
                                           const tools::Tool::Result& result) {
        // hooks 框架第三步起的次序:PostToolUse 在工具结果已清洗成合法
        // UTF-8 之后触发(规格"PostToolUse 在工具结果已清洗、已结构化后
        // 触发")——post hook 看到的必须是干净内容。
        order.push_back(IsValidUtf8(result.content) ? "post:clean" : "post:raw");
    };
    callbacks.on_tool_done = [&order, &done_content](const std::string&, const std::string&,
                                                     const tools::Tool::Result& result) {
        order.push_back("done");
        done_content = result.content;
    };

    const auto outcome = loop.Run("跑一趟", callbacks);
    REQUIRE(outcome.has_value());

    // 回调次序:post hook 先于 on_tool_done;两者拿到的都已合法(hooks
    // 框架把清洗挪到了 post hook 之前)。
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "post:clean");
    CHECK(order[1] == "done");
    CHECK(IsValidUtf8(done_content));
    CHECK(done_content.find("好开头") != std::string::npos);
    CHECK(done_content.find("\xC4\xE3") == std::string::npos);

    // 下一轮请求历史里的 tool_result 与 on_tool_done 收到的一字不差。
    REQUIRE(backend.captured_requests.size() == 2);
    const auto& second = backend.captured_requests[1];
    std::optional<std::string> request_result_content;
    for (const auto& message : second.messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                request_result_content = result->content;
            }
        }
    }
    REQUIRE(request_result_content.has_value());
    CHECK(*request_result_content == done_content);
}

// ---------------------------------------------------------------------------
// JSON 序列化窄边界
// ---------------------------------------------------------------------------

TEST_CASE("清洗后的内容能进三种 wire 请求 JSON 并 dump") {
    const std::string cleaned = SanitizeExternalText("汉字\xC4\xE3 tail");
    REQUIRE(IsValidUtf8(cleaned));

    api::Request request;
    request.model = "test-model";
    request.max_tokens = 64;
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::ToolResultBlock{"toolu_1", cleaned, false});
    request.messages.push_back(user);

    CHECK_NOTHROW(api::chat::BuildRequestJson(request).dump());
    CHECK_NOTHROW(api::responses::BuildRequestJson(request, false, {}).dump());
    CHECK_NOTHROW(api::anthropic::BuildRequestJson(request).dump());
}

TEST_CASE("FindInvalidUtf8Field: 坏串埋在哪儿都能定位到字段路径") {
    nlohmann::json body;
    body["messages"][2]["content"][0]["content"] = "好\xC4\xE3";
    const auto field = FindInvalidUtf8Field(body);
    REQUIRE(field.has_value());
    CHECK(*field == "messages[2].content[0].content");

    body["messages"][2]["content"][0]["content"] = "干净了";
    CHECK_FALSE(FindInvalidUtf8Field(body).has_value());
}

TEST_CASE("DumpJsonSanitized: 坏串树兜底后每行可重新解析") {
    nlohmann::json j;
    j["seq"] = 1;
    j["data"]["tool"] = "read_file";
    j["data"]["output"] = "开头\xC4\xE3结尾";
    bool changed = false;
    const std::string line = DumpJsonSanitized(j, &changed);
    CHECK(changed);
    const auto parsed = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed["data"]["output"].get<std::string>().find("开头") != std::string::npos);
    // 干净树原样直通,不动手脚。
    CHECK(DumpJsonSanitized(nlohmann::json{{"a", "汉"}}) == "{\"a\":\"汉\"}");
}

TEST_CASE("SerializeSessionMessage: 坏串兜底,JSONL 行可重新解析,/resume 不吃坏行") {
    // 配一对 tool_use/tool_result,不然 RepairToolPairs 会把孤儿 tool_result
    // 当残档删掉。
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    api::ToolUseBlock use;
    use.id = "toolu_1";
    use.name = "read_file";
    use.input = nlohmann::json::object({{"path", "a.txt"}});
    assistant.content.push_back(std::move(use));

    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::ToolResultBlock{"toolu_1", std::string("结果\xC4\xE3"), false});

    const std::string assistant_line = agent::SerializeSessionMessage(assistant, "2026-08-14 00:00:00");
    const std::string line = agent::SerializeSessionMessage(message, "2026-08-14 00:00:01");
    CHECK(agent::DeserializeSessionMessage(line).has_value());
    // meta 行拼上,整文件解析(真实 /resume 路径)。
    const std::string file_content =
        agent::SerializeSessionMeta(agent::SessionMeta{}) + "\n" + assistant_line + "\n" + line + "\n";
    const auto session = agent::ParseSessionFile(file_content);
    REQUIRE(session.has_value());
    REQUIRE(session->messages.size() == 2);
    REQUIRE(session->messages[1].content.size() == 1);
    const auto* result = std::get_if<api::ToolResultBlock>(&session->messages[1].content[0]);
    REQUIRE(result != nullptr);
    CHECK(IsValidUtf8(result->content));
    CHECK(result->content.find("结果") != std::string::npos);
    CHECK(session->skipped_lines == 0);
}

TEST_CASE("SerializeRecordEvent: 录制器吃到坏输出,事件行仍是合法 JSON") {
    agent::RecordEvent event;
    event.seq = 7;
    event.ts = "2026-08-14 00:00:00";
    event.source = "model";
    event.type = agent::kEventToolResult;
    event.data["tool"] = "read_file";
    event.data["summary"] = std::string("首行\xC4\xE3摘要");
    const std::string line = agent::SerializeRecordEvent(event);
    const auto parsed = agent::ParseRecordEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->seq == 7);
    CHECK(IsValidUtf8(parsed->data["summary"].get<std::string>()));
}
