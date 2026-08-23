// memory 回合抽取的纯函数单测:任务分型、转写压缩、JSON 解析、提示词拼装。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app/memory_extract.hpp"

using namespace lubancode;

namespace {

api::Message UserText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

std::vector<api::Message> ToolRound(const std::string& tool_name, const std::string& result,
                                    bool is_error = false) {
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    api::ToolUseBlock use;
    use.name = tool_name;
    use.input = nlohmann::json{{"path", "src/main.cpp"}};
    assistant.content.push_back(use);
    api::Message result_message;
    result_message.role = api::Role::User;
    api::ToolResultBlock block;
    block.tool_use_id = "t1";
    block.content = result;
    block.is_error = is_error;
    result_message.content.push_back(block);
    return {assistant, result_message};
}

}  // namespace

TEST_CASE("ClassifyTaskType: 分型命中各自的侧重") {
    CHECK(app::ClassifyTaskType("帮我装一下依赖,用 conda 建环境", {}) == "config");
    CHECK(app::ClassifyTaskType("编译不过,看看构建哪里坏了", {}) == "config");
    CHECK(app::ClassifyTaskType("把 README 文档补一段说明", {}) == "docs");
    CHECK(app::ClassifyTaskType("看看 AgentLoop 在哪,梳理一下流程", {}) == "research");
    CHECK(app::ClassifyTaskType("这个报错为什么出现", {}) == "research");
    CHECK(app::ClassifyTaskType("修复这个 bug,把判断改掉", {}) == "code");
    CHECK(app::ClassifyTaskType("今天天气不错", {}) == "other");

    // 工具名也算证据:纯"看看"配 write_file 仍偏 code。
    CHECK(app::ClassifyTaskType("看看这里", {"write_file", "edit_file"}) == "code");
    CHECK(app::ClassifyTaskType("看看这里", {"read_file", "search"}) == "research");
}

TEST_CASE("BuildTurnTranscript: 正文收全,工具摘要截断,总量有上限") {
    std::vector<api::Message> messages;
    messages.push_back(UserText("请修一下这个崩溃"));
    for (auto& message : ToolRound("read_file", std::string(2000, 'x'))) {
        messages.push_back(std::move(message));
    }
    messages.push_back(AssistantText("修好了。"));

    const std::string transcript = app::BuildTurnTranscript(messages, 24 * 1024);
    CHECK(transcript.find("[用户] 请修一下这个崩溃") != std::string::npos);
    CHECK(transcript.find("[工具调用] read_file(") != std::string::npos);
    CHECK(transcript.find("[工具结果] ") != std::string::npos);
    CHECK(transcript.find("[助手] 修好了。") != std::string::npos);
    // 工具结果只留开头一小段,不整包送抽取。
    CHECK(transcript.size() < 1200);

    // 总量上限:超限时截住,不无限长。
    const std::string big(100 * 1024, 'x');
    std::vector<api::Message> huge;
    huge.push_back(UserText(big));
    const std::string bounded = app::BuildTurnTranscript(huge, 2 * 1024);
    CHECK(bounded.size() <= 2 * 1024 + 100);

    // 空输入给空串。
    CHECK(app::BuildTurnTranscript({}, 1024).empty());
}

TEST_CASE("ParseExtractionJson: 严格字段与容错围栏") {
    const std::string good = R"({"task_type":"code","summary":"修了崩溃","retrieval_terms":["crash","崩 布局"],)"
                             R"("candidates":[{"kind":"fact","title":"崩溃根因","summary":"根因是 X",)"
                             R"("content":"崩溃来自 Y 的空指针。","keywords":["crash"],"paths":["src/y.cpp"],)"
                             R"("confidence":"verified"},{"kind":"bogus","title":"x","content":"y"}]})";
    const auto parsed = app::ParseExtractionJson(good);
    REQUIRE(parsed.has_value());
    CHECK(parsed->task_type == "code");
    CHECK(parsed->summary == "修了崩溃");
    REQUIRE(parsed->retrieval_terms.size() == 2);
    REQUIRE(parsed->candidates.size() == 1);  // bogus kind 的整条丢弃
    CHECK(parsed->candidates[0].title == "崩溃根因");
    CHECK(parsed->candidates[0].confidence == "verified");

    // 模型裹了 ```json 围栏也能解析。
    const auto fenced = app::ParseExtractionJson("```json\n" + good + "\n```");
    REQUIRE(fenced.has_value());
    CHECK(fenced->candidates.size() == 1);

    // 认不出的 task_type 落 other;candidates 超过 3 条只取前 3。
    std::string many = R"({"task_type":"weird","candidates":[)"
                       R"({"kind":"fact","title":"1","content":"c"},)"
                       R"({"kind":"fact","title":"2","content":"c"},)"
                       R"({"kind":"fact","title":"3","content":"c"},)"
                       R"({"kind":"fact","title":"4","content":"c"}]})";
    const auto capped = app::ParseExtractionJson(many);
    REQUIRE(capped.has_value());
    CHECK(capped->task_type == "other");
    CHECK(capped->candidates.size() == 3);

    // 前后带解释文字:取首个 { 到末个 }。
    const auto chatty = app::ParseExtractionJson("好的,以下是总结:\n" + good + "\n以上。");
    REQUIRE(chatty.has_value());

    CHECK_FALSE(app::ParseExtractionJson("没有任何 JSON").has_value());
    CHECK_FALSE(app::ParseExtractionJson("{broken").has_value());
}

TEST_CASE("BuildExtractionSystemPrompt: 基础契约 + 分型侧重") {
    const std::string code_prompt = app::BuildExtractionSystemPrompt("", "code");
    CHECK(code_prompt.find("候选只收四类") != std::string::npos);
    CHECK(code_prompt.find("feedback") != std::string::npos);
    CHECK(code_prompt.find("失败经验") != std::string::npos);

    const std::string config_prompt = app::BuildExtractionSystemPrompt("", "config");
    CHECK(config_prompt.find("包管理器") != std::string::npos);

    // 认不出的分型落 other 模块;提示词非空。
    CHECK_FALSE(app::BuildExtractionSystemPrompt("", "nonsense").empty());
    CHECK(app::BuildExtractionSystemPrompt("", "other").find("不属于") != std::string::npos);
}
