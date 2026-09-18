// memory 回合抽取的纯函数单测:任务分型、转写压缩、JSON 解析、提示词拼装。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/memory_extract.hpp"
#include "platform/text_encoding.hpp"

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
    use.id = "t1";
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
    CHECK(bounded.size() <= 2 * 1024);

    // 空输入给空串。
    CHECK(app::BuildTurnTranscript({}, 1024).empty());
}

TEST_CASE("BuildTurnTranscript: tool floods cannot crowd out final conclusions or copy write payloads") {
    std::vector<api::Message> messages{UserText("以后按这个结论处理。")};
    for (int i = 0; i < 100; ++i) {
        messages.push_back(AssistantText("中间进度不要送给抽取模型"));
        auto round = ToolRound("read_file", std::string(5000, 'x'));
        auto& use = std::get<api::ToolUseBlock>(round[0].content[0]);
        use.input["path"] = "src/step-" + std::to_string(i) + ".cpp";
        use.input["content"] = "RAW_FILE_PAYLOAD_MUST_NOT_LEAK";
        for (auto& message : round) messages.push_back(std::move(message));
    }
    for (auto& message : ToolRound("run_command", "confirmed failure", true)) {
        messages.push_back(std::move(message));
    }
    messages.push_back(AssistantText("最终结论：worker 句柄提前释放。"));
    const auto transcript = app::BuildTurnTranscript(messages, 8 * 1024);
    CHECK(transcript.size() <= 8 * 1024);
    CHECK(transcript.find("最终结论：worker 句柄提前释放。") != std::string::npos);
    CHECK(transcript.find("src/step-99.cpp") != std::string::npos);
    CHECK(transcript.find("src/step-0.cpp") == std::string::npos);
    CHECK(transcript.find("中间进度") == std::string::npos);
    CHECK(transcript.find("RAW_FILE_PAYLOAD") == std::string::npos);
    CHECK(transcript.find("[工具结果,失败] confirmed failure") != std::string::npos);
    for (std::size_t cap = 0; cap < 80; ++cap) {
        const auto tiny = app::BuildTurnTranscript(messages, cap);
        CHECK(tiny.size() <= cap);
        CHECK(platform::IsValidUtf8(tiny));
    }
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

    // 认不出的 task_type 落 other;candidates 超过 3 条只取前 3。(新字段
    // 合同下 summary 必填,这枚夹具补上。)
    std::string many = R"({"task_type":"weird","summary":"s","candidates":[)"
                       R"({"kind":"fact","title":"1","content":"c"},)"
                       R"({"kind":"fact","title":"2","content":"c"},)"
                       R"({"kind":"fact","title":"3","content":"c"},)"
                       R"({"kind":"fact","title":"4","content":"c"}]})";
    const auto capped = app::ParseExtractionJson(many);
    REQUIRE(capped.has_value());
    CHECK(capped->task_type == "other");
    CHECK(capped->candidates.size() == 3);

    // 前后带解释文字:首个 { 起配对扫描,首尾纯文字说明无歧义放行。
    const auto chatty = app::ParseExtractionJson("好的,以下是总结:\n" + good + "\n以上。");
    REQUIRE(chatty.has_value());

    CHECK_FALSE(app::ParseExtractionJson("没有任何 JSON").has_value());
    CHECK_FALSE(app::ParseExtractionJson("{broken").has_value());
}

TEST_CASE("ParseExtractionJson: occurred_at 材料里有才留,不像日期落空") {
    // 材料里明确给出的日期照收。
    const std::string with_date = R"({"task_type":"other","summary":"s","candidates":[)"
                                  R"({"kind":"fact","title":"上线日","content":"5月8日上线。",)"
                                  R"("occurred_at":"2023-05-08","confidence":"verified"}]})";
    const auto dated = app::ParseExtractionJson(with_date);
    REQUIRE(dated.has_value());
    REQUIRE(dated->candidates.size() == 1);
    CHECK(dated->candidates[0].occurred_at == "2023-05-08");

    // 模型推算/口语时间:形状不像日期,清洗成空——不拦整条候选,也不造假。
    const std::string sloppy = R"({"task_type":"other","summary":"s","candidates":[)"
                               R"({"kind":"fact","title":"上线日","content":"上周上线。",)"
                               R"("occurred_at":"上周三","confidence":"verified"}]})";
    const auto cleaned = app::ParseExtractionJson(sloppy);
    REQUIRE(cleaned.has_value());
    REQUIRE(cleaned->candidates.size() == 1);
    CHECK(cleaned->candidates[0].occurred_at.empty());

    // 没给字段 = 空。
    const std::string none = R"({"task_type":"other","summary":"s","candidates":[)"
                             R"({"kind":"fact","title":"上线日","content":"上线了。",)"
                             R"("confidence":"verified"}]})";
    const auto undated = app::ParseExtractionJson(none);
    REQUIRE(undated.has_value());
    CHECK(undated->candidates[0].occurred_at.empty());
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

// ---------------------------------------------------------------------------
// 回合记忆抽取 JSON 收口修复单(P0-A/P0-B/P1-A):结构化错误、解析收口、
// 字段合同、结束原因分类。八行场景表的纯函数层全在这。
// ---------------------------------------------------------------------------

TEST_CASE("错误码枚举名钉死 + 稳定码:结构化新码与旧文案路各记各账") {
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::SyntaxInvalid)) ==
          "syntax_invalid");
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::Utf8Invalid)) == "utf8_invalid");
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::SchemaInvalid)) ==
          "schema_invalid");
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::OutputTruncated)) ==
          "output_truncated");
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::EmptyOutput)) == "empty_output");
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::TransportFailed)) ==
          "transport_failed");
    CHECK(std::string(app::ExtractionErrorCodeName(app::ExtractionErrorCode::RouteMiss)) == "route_miss");

    // 结构化版:六类 + route_miss。
    CHECK(app::StableExtractErrorCode(app::ExtractionError{.code = app::ExtractionErrorCode::Utf8Invalid}) ==
          "utf8_invalid");
    CHECK(app::StableExtractErrorCode(app::ExtractionError{.code = app::ExtractionErrorCode::RouteMiss}) ==
          "route_miss");

    // 旧文案版保留:固定前缀折旧码(parse_failed 是 syntax/utf8/schema 三类
    // 的旧统称),旧账离线对账继续可用。
    CHECK(app::StableExtractErrorCode("cheap 路由找不到 provider \"kimi\"") == "route_miss");
    CHECK(app::StableExtractErrorCode("抽取输出为空") == "empty_output");
    CHECK(app::StableExtractErrorCode("抽取输出不是合法 JSON: ...") == "parse_failed");
    CHECK(app::StableExtractErrorCode("网络炸了") == "other");
}

TEST_CASE("ParseExtractionJson: 本单事故形态——未转义双引号,语法错定位") {
    // summary 里混入未转义双引号:字符串提前结束,后续汉字落到字符串外。
    const std::string incident =
        R"({"task_type":"code","summary":"用户问"问题"","candidates":[]})";
    const auto broken = app::ParseExtractionJson(incident);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == app::ExtractionErrorCode::SyntaxInvalid);
    // 定位号:错误字节偏移落在原文内(截取偏移换算回原文偏移)。
    CHECK(broken.error().error_offset != app::kExtractionNoOffset);
    CHECK(broken.error().error_offset < incident.size());
    // 输入是合法 UTF-8:这不是编码病,是合法中文落在 JSON 字符串外。
    CHECK(broken.error().utf8_valid);
    // 终端短文案:不带库异常原文(last read 可能含半个多字节字符)。
    CHECK(broken.error().message.find("last read") == std::string::npos);
    CHECK(broken.error().message.find("json.exception") == std::string::npos);
    // 稳定码:结构化路记 syntax_invalid,旧文案路记 parse_failed(同范畴)。
    CHECK(app::StableExtractErrorCode(broken.error()) == "syntax_invalid");
    CHECK(app::StableExtractErrorCode(broken.error().message) == "parse_failed");
}

TEST_CASE("ParseExtractionJson: 合法内容完整保留(转义引号/中文引号/emoji/路径/换行)") {
    const std::string text =
        R"({"task_type":"code","summary":"用户说\"回退链\"没配好:D:\\repo\\src\\app\\router.cpp 报错\n修好了","retrieval_terms":["回退链😀","router"],"candidates":[]})";
    const auto parsed = app::ParseExtractionJson(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->summary == "用户说\"回退链\"没配好:D:\\repo\\src\\app\\router.cpp 报错\n修好了");
    REQUIRE(parsed->retrieval_terms.size() == 2);
    CHECK(parsed->retrieval_terms[0] == "回退链😀");

    // 中文引号与全角标点不是 JSON 语法字符,原样保留;候选字段完整带出。
    const std::string with_candidate =
        R"({"task_type":"docs","summary":"改了“安装”一节","candidates":[)"
        R"({"kind":"preference","title":"文档用中文引号“”","summary":"一行","content":"正文含“引号”。",)"
        R"("confidence":"user-stated"}]})";
    const auto carried = app::ParseExtractionJson(with_candidate);
    REQUIRE(carried.has_value());
    REQUIRE(carried->candidates.size() == 1);
    CHECK(carried->candidates[0].title == "文档用中文引号“”");
    CHECK(carried->candidates[0].confidence == "user-stated");
}

TEST_CASE("ParseExtractionJson: 漏逗号与全角标点落语法位置——syntax_invalid") {
    // 漏逗号。
    const auto missing_comma =
        app::ParseExtractionJson(R"({"task_type":"code" "summary":"s"})");
    REQUIRE_FALSE(missing_comma.has_value());
    CHECK(missing_comma.error().code == app::ExtractionErrorCode::SyntaxInvalid);

    // 全角冒号/全角逗号落在语法位置:语法错,不是字段错。
    const auto full_width =
        app::ParseExtractionJson("{\n\"task_type\"：\"code\"，\"summary\":\"s\"\n}");
    REQUIRE_FALSE(full_width.has_value());
    CHECK(full_width.error().code == app::ExtractionErrorCode::SyntaxInvalid);
    CHECK(full_width.error().error_offset != app::kExtractionNoOffset);
}

TEST_CASE("ParseExtractionJson: 残缺 UTF-8 与合法 UTF-8 语法错分类不同") {
    // 真残缺 UTF-8:半截三字节序列(E4 B8 尾字节)。
    const std::string broken_utf8 = "{\"task_type\":\"code\",\"summary\":\"半截\xE4\xB8";
    const auto utf8_broken = app::ParseExtractionJson(broken_utf8);
    REQUIRE_FALSE(utf8_broken.has_value());
    CHECK(utf8_broken.error().code == app::ExtractionErrorCode::Utf8Invalid);
    CHECK_FALSE(utf8_broken.error().utf8_valid);
    CHECK(utf8_broken.error().error_offset != app::kExtractionNoOffset);
    CHECK(utf8_broken.error().error_offset < broken_utf8.size());
    CHECK(app::StableExtractErrorCode(utf8_broken.error()) == "utf8_invalid");

    // 合法 UTF-8 的语法错(事故原文形态):syntax_invalid,不是编码病。
    const auto syntax =
        app::ParseExtractionJson(R"({"task_type":"code","summary":"用户问"问题""})");
    REQUIRE_FALSE(syntax.has_value());
    CHECK(syntax.error().code == app::ExtractionErrorCode::SyntaxInvalid);
    CHECK(syntax.error().utf8_valid);
}

TEST_CASE("ParseExtractionJson: 字段合同——缺必填/类型错返回字段路径,不抛出") {
    // {} :缺必填 task_type(旧法靠默认值掩过去,新合同拒绝)。
    const auto empty = app::ParseExtractionJson("{}");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == app::ExtractionErrorCode::SchemaInvalid);
    CHECK(empty.error().field_path == "task_type");

    // 缺 summary。
    const auto no_summary = app::ParseExtractionJson(R"({"task_type":"code"})");
    REQUIRE_FALSE(no_summary.has_value());
    CHECK(no_summary.error().field_path == "summary");

    // summary null/数字/数组/对象:显式判型,不再 value() 抛 type_error.302。
    const std::vector<std::string> wrong_summaries = {
        R"({"task_type":"code","summary":null})",
        R"({"task_type":"code","summary":42})",
        R"({"task_type":"code","summary":["a"]})",
        R"({"task_type":"code","summary":{"a":1}})",
    };
    for (const std::string& bad_summary : wrong_summaries) {
        const auto wrong = app::ParseExtractionJson(bad_summary);
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == app::ExtractionErrorCode::SchemaInvalid);
        CHECK(wrong.error().field_path == "summary");
    }

    // 顶层数组:parse 得动,按"顶层必须 object"的合同拒。
    const auto top_array =
        app::ParseExtractionJson(R"([{"task_type":"code","summary":"s"}])");
    REQUIRE_FALSE(top_array.has_value());
    CHECK(top_array.error().code == app::ExtractionErrorCode::SchemaInvalid);

    // candidates 字段类型错(对象顶数组):拒整次,带路径。
    const auto candidates_object = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":{"kind":"fact"}})");
    REQUIRE_FALSE(candidates_object.has_value());
    CHECK(candidates_object.error().field_path == "candidates");

    // 候选条目非 object:结构类型错拒整次。
    const auto item_not_object = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":["fact"]})");
    REQUIRE_FALSE(item_not_object.has_value());
    CHECK(item_not_object.error().code == app::ExtractionErrorCode::SchemaInvalid);
    CHECK(item_not_object.error().field_path == "candidates[0]");

    // 候选字段类型错(title 数字、kind 数字、keywords 非数组):拒整次带路径。
    const auto title_number = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":[{"kind":"fact","title":7,"content":"c"}]})");
    REQUIRE_FALSE(title_number.has_value());
    CHECK(title_number.error().field_path == "candidates[0].title");

    const auto kind_number = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":[{"kind":1,"title":"t","content":"c"}]})");
    REQUIRE_FALSE(kind_number.has_value());
    CHECK(kind_number.error().field_path == "candidates[0].kind");

    const auto keywords_object = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":[{"kind":"fact","title":"t","content":"c","keywords":{"k":"v"}}]})");
    REQUIRE_FALSE(keywords_object.has_value());
    CHECK(keywords_object.error().field_path == "candidates[0].keywords");

    // 无效业务候选:kind 枚举外、title/content 空——跳过该条,不拒整次。
    const auto skipping = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":[)"
        R"({"kind":"bogus","title":"t","content":"c"},)"
        R"({"kind":"fact","title":"","content":"c"},)"
        R"({"kind":"fact","title":"好候选","content":"正文"}]})");
    REQUIRE(skipping.has_value());
    REQUIRE(skipping->candidates.size() == 1);
    CHECK(skipping->candidates[0].title == "好候选");

    // confidence 枚举外值清洗成 inferred,不冒充高置信。
    const auto confidence = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":[{"kind":"fact","title":"t","content":"c","confidence":"high"}]})");
    REQUIRE(confidence.has_value());
    REQUIRE(confidence->candidates.size() == 1);
    CHECK(confidence->candidates[0].confidence == "inferred");

    // 候选正文预算:超 8 KiB 的 content 整条跳过,不截断不拒整次。
    const std::string huge_content(app::kMaxCandidateContentBytes + 1, 'x');
    const std::string over_budget_json =
        std::string(R"({"task_type":"code","summary":"s","candidates":[)") +
        std::string(R"({"kind":"fact","title":"t","content":")") + huge_content + "\"}]}";
    const auto over_budget = app::ParseExtractionJson(over_budget_json);
    REQUIRE(over_budget.has_value());
    CHECK(over_budget->candidates.empty());
}

TEST_CASE("ParseExtractionJson: 包装规则——围栏/说明/花括号/多对象/半截各归其位") {
    const std::string good = R"({"task_type":"code","summary":"修了崩溃","retrieval_terms":[],"candidates":[]})";

    // 字符串内花括号 + 前后说明:配对扫描跳过字符串内的 {,取到真正的 }。
    const auto braces_in_string = app::ParseExtractionJson(
        "总结如下:\n" +
        std::string(R"({"task_type":"code","summary":"配置模板是 {braces} 对","candidates":[]})") + "\n以上。");
    REQUIRE(braces_in_string.has_value());
    CHECK(braces_in_string->summary == "配置模板是 {braces} 对");

    // 多对象:带前导说明时,配对之后还有结构字符,拒绝碰运气。
    const auto two_objects =
        app::ParseExtractionJson("总结:\n" + good + "\n" + good);
    REQUIRE_FALSE(two_objects.has_value());
    CHECK(two_objects.error().code == app::ExtractionErrorCode::SyntaxInvalid);

    // 首字符就是 '{' 的多对象(纯 JSON 形态):parse 整段失败,同样拒。
    const auto back_to_back = app::ParseExtractionJson(good + "\n" + good);
    REQUIRE_FALSE(back_to_back.has_value());
    CHECK(back_to_back.error().code == app::ExtractionErrorCode::SyntaxInvalid);

    const auto object_then_array = app::ParseExtractionJson("总结:\n" + good + "\n[1,2]");
    REQUIRE_FALSE(object_then_array.has_value());

    // 结构化前导(数组在前):拒绝。
    const auto lead_array = app::ParseExtractionJson("[备注] " + good);
    REQUIRE_FALSE(lead_array.has_value());
    CHECK(lead_array.error().code == app::ExtractionErrorCode::SyntaxInvalid);

    // 外层截断但内层已有 '}':不误收半截对象。
    const auto truncated = app::ParseExtractionJson(
        R"({"task_type":"code","summary":"s","candidates":[{"kind":"fact"})");
    REQUIRE_FALSE(truncated.has_value());
    CHECK(truncated.error().code == app::ExtractionErrorCode::SyntaxInvalid);

    // 嵌套/多围栏:歧义,拒绝。
    const auto nested_fence =
        app::ParseExtractionJson("```json\n" + good + "\n```\n```json\n" + good + "\n```");
    REQUIRE_FALSE(nested_fence.has_value());

    // 前后说明的尾巴是纯文字(无结构字符):无歧义,兼容放行(既有行为)。
    const auto chatty_tail = app::ParseExtractionJson("好的:\n" + good + "\n以上。");
    REQUIRE(chatty_tail.has_value());
}

TEST_CASE("ParseExtractionJson: 诊断文案自身是合法 UTF-8") {
    // 坏 UTF-8 的诊断输出:错误消息含偏移数字与固定中文,不得再引入坏字节。
    // (字面量在 hex 转义后断开——"\x80def" 会被贪婪吃成一个超长转义。)
    const std::string broken_utf8 = "abc\xFF\x80"
                                    "def";
    const auto result = app::ParseExtractionJson(broken_utf8);
    REQUIRE_FALSE(result.has_value());
    const std::string message = result.error().message;
    CHECK(platform::IsValidUtf8(message));
    CHECK(platform::IsValidUtf8(app::StableExtractErrorCode(result.error())));
}

TEST_CASE("FinishMemoryExtraction: 结束原因与失败的分类(六类收口)") {
    // 传输失败:发送失败/流内错 → transport_failed,错误消息消毒后可直出。
    agent::SampleResult failed;
    failed.ok = false;
    failed.error.message = "连接被重置\xE4\xB8";
    failed.text = "半截";
    failed.provider_response_id = "resp-1";
    failed.stop_reason = "max_tokens";  // 传输失败优先于结束原因分类
    const auto transport = app::FinishMemoryExtraction(failed);
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error().code == app::ExtractionErrorCode::TransportFailed);
    CHECK(platform::IsValidUtf8(transport.error().message));
    CHECK(transport.error().request_id == "resp-1");
    CHECK(transport.error().body_bytes == 6);  // "半截" UTF-8 6 字节
    CHECK(app::StableExtractErrorCode(transport.error()) == "transport_failed");

    // 本地超时预算到点(取消误报 ESC 单 Bug 1):采样层带回 Cancelled +
    // local_deadline 稳定码 → deadline_timeout 单独分类,文案不带按键
    // 指控,半截 usage/正文照带回,稳定码与 transport_failed 分账。
    agent::SampleResult deadline;
    deadline.ok = false;
    deadline.error.kind = api::ErrorKind::Cancelled;
    deadline.error.message = "采样超过 45 秒,被本地超时预算停止";
    deadline.error.api_code = "local_deadline";
    deadline.usage.input_tokens = 900;
    deadline.usage_reported = true;
    deadline.text = "半截总结";
    const auto timed_out = app::FinishMemoryExtraction(deadline);
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(timed_out.error().code == app::ExtractionErrorCode::DeadlineTimeout);
    CHECK(app::StableExtractErrorCode(timed_out.error()) == "deadline_timeout");
    CHECK(timed_out.error().message.find("超时") != std::string::npos);
    CHECK(timed_out.error().message.find("ESC") == std::string::npos);
    // 同是 Cancelled 但没有 local_deadline 码的(来源未知/用户取消):照走
    // transport_failed,不借 deadline 的名。
    agent::SampleResult plain_cancel = deadline;
    plain_cancel.error.api_code.clear();
    const auto unknown = app::FinishMemoryExtraction(plain_cancel);
    CHECK(unknown.error().code == app::ExtractionErrorCode::TransportFailed);

    // 空正文:empty_output。
    agent::SampleResult empty;
    empty.ok = true;
    empty.text = "";
    const auto no_text = app::FinishMemoryExtraction(empty);
    REQUIRE_FALSE(no_text.has_value());
    CHECK(no_text.error().code == app::ExtractionErrorCode::EmptyOutput);
    CHECK(app::StableExtractErrorCode(no_text.error()) == "empty_output");

    // 已知截断结束原因:output_truncated,即便剩余文本碰巧是合法 JSON。
    agent::SampleResult truncated;
    truncated.ok = true;
    truncated.text = R"({"task_type":"code","summary":"碰巧完整","candidates":[]})";
    truncated.stop_reason = "max_tokens";
    truncated.provider_response_id = "resp-2";
    const auto cut = app::FinishMemoryExtraction(truncated);
    REQUIRE_FALSE(cut.has_value());
    CHECK(cut.error().code == app::ExtractionErrorCode::OutputTruncated);
    CHECK(cut.error().stop_reason == "max_tokens");
    CHECK(cut.error().body_bytes == truncated.text.size());
    CHECK(app::StableExtractErrorCode(cut.error()) == "output_truncated");
    CHECK(cut.error().message.find("未报告 usage") != std::string::npos);
    truncated.usage_reported = true;
    truncated.usage.input_tokens = 812;
    truncated.usage.output_tokens = 1500;
    const auto billed_cut = app::FinishMemoryExtraction(truncated);
    REQUIRE_FALSE(billed_cut.has_value());
    CHECK(billed_cut.error().message.find("input=812, output=1500") != std::string::npos);
    CHECK(billed_cut.error().message.find("不自动重试") != std::string::npos);

    // openai 的 length、大写 MAX_TOKENS 同样归截断。
    truncated.stop_reason = "length";
    CHECK(app::FinishMemoryExtraction(truncated).error().code ==
          app::ExtractionErrorCode::OutputTruncated);
    truncated.stop_reason = "MAX_TOKENS";
    CHECK(app::FinishMemoryExtraction(truncated).error().code ==
          app::ExtractionErrorCode::OutputTruncated);

    // 结束原因未知/缺失:不据此判死,照走解析,诊断单列。
    truncated.stop_reason = "weird_reason";
    truncated.text = R"({"task_type":"code","summary":"好的","candidates":[]})";
    const auto odd_reason = app::FinishMemoryExtraction(truncated);
    REQUIRE(odd_reason.has_value());
    truncated.stop_reason = "";  // provider 没报
    truncated.text = R"({"task_type":"code","summary":"半截语法错")";
    const auto no_reason = app::FinishMemoryExtraction(truncated);
    REQUIRE_FALSE(no_reason.has_value());
    CHECK(no_reason.error().code == app::ExtractionErrorCode::SyntaxInvalid);
    CHECK(no_reason.error().stop_reason.empty());

    // 半截流(无收尾事件):文本非空、stop_reason 空——同上,走解析分类。
    agent::SampleResult schema_flagged;
    schema_flagged.ok = true;
    schema_flagged.text = "不是 JSON";
    schema_flagged.stop_reason = "end_turn";
    schema_flagged.schema_ok = false;
    schema_flagged.schema_error = "采样正文不是合法 JSON";
    const auto not_json = app::FinishMemoryExtraction(schema_flagged);
    REQUIRE_FALSE(not_json.has_value());
    CHECK(not_json.error().code == app::ExtractionErrorCode::SyntaxInvalid);
    // SampleModel 复检账并进诊断(P1-A 消费 schema_ok)。
    CHECK(not_json.error().schema_check_error == "采样正文不是合法 JSON");

    // 成功:结束原因已知非截断,解析通过。
    agent::SampleResult fine;
    fine.ok = true;
    fine.text = R"({"task_type":"code","summary":"成了","candidates":[]})";
    fine.stop_reason = "end_turn";
    const auto parsed = app::FinishMemoryExtraction(fine);
    REQUIRE(parsed.has_value());
    CHECK(parsed->summary == "成了");
}

TEST_CASE("MemoryExtractionOutputSchema: 字段合同与解析器同一把尺子") {
    const nlohmann::json& schema = app::MemoryExtractionOutputSchema();
    CHECK(schema.at("type") == "object");
    CHECK(schema.at("required") == nlohmann::json::array({"task_type", "summary"}));
    // 顶层声明的四字段与解析合同一致。
    for (const char* key : {"task_type", "summary", "retrieval_terms", "candidates"}) {
        CHECK(schema.at("properties").contains(key));
    }
    // 调用两次同一份(静态单例)。
    CHECK(&app::MemoryExtractionOutputSchema() == &schema);
}
