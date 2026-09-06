// 上下文保护(token 轴):统一 token 估算尺、字节计量(EstimateHistoryBytes
// 只剩计量用途)、单条巨肥工具结果的保命索(ShrinkOversizedToolResults)。
// 旧的按字节整轮删裁(TrimHistory)已随字节轴拆除,相关册子一并退场。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/context.hpp"
#include "agent/runtime_profile.hpp"  // kFallbackContextWindowTokens:窗口未知兜底
#include "api/types.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8:截断回归的判据

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

api::Message AssistantToolUse(const std::string& id, const std::string& name) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::ToolUseBlock{id, name, nlohmann::json::object()});
    return m;
}

api::Message UserToolResult(const std::string& tool_use_id, const std::string& content) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::ToolResultBlock{tool_use_id, content, false});
    return m;
}

}  // namespace

TEST_CASE("EstimateHistoryBytes: 累加所有文本/工具入参/工具结果的字节数") {
    std::vector<api::Message> history;
    history.push_back(UserText("12345"));
    history.push_back(AssistantText("abcde"));
    CHECK(agent::EstimateHistoryBytes(history) == 10);
}

// ---------------------------------------------------------------------------
// 0.31.x 分层压缩第一期:统一 token 口径
// ---------------------------------------------------------------------------

TEST_CASE("EstimateUtf8Tokens: 统一口径,ASCII 4 字符 1 token,非 ASCII 1.5 token/字") {
    // 纯 ASCII:8 个字符 2 token。
    CHECK(agent::EstimateUtf8Tokens("abcdefgh") == 2);
    // 纯中文(UTF-8 每字 3 字节):4 个码点 6 token(1.5/字)。
    CHECK(agent::EstimateUtf8Tokens("你好世界") == 6);
    // 混排:ASCII 4 个 = 1 token,中文 2 个 = 3 token,合计 4。
    CHECK(agent::EstimateUtf8Tokens("abcd你好") == 4);
    // 空串 0。
    CHECK(agent::EstimateUtf8Tokens("") == 0);
}

TEST_CASE("EstimateHistoryTokens: 逐块累加,统一口径不回退到字节/3") {
    std::vector<api::Message> history;
    history.push_back(UserText("12345678"));  // 8 ASCII = 2 token
    history.push_back(AssistantText("你好"));  // 2 码点 = 3 token
    CHECK(agent::EstimateHistoryTokens(history) == 5);
}

// ---------------------------------------------------------------------------
// 保命索:单条巨肥工具结果的尾部截断(token 轴口径)
// ---------------------------------------------------------------------------

TEST_CASE("ShrinkOversizedToolResults: 线内的原样放行,一条不动") {
    std::vector<api::Message> history;
    history.push_back(UserText("用户输入"));
    history.push_back(AssistantToolUse("tool_a", "some_tool"));
    history.push_back(UserToolResult("tool_a", std::string(1000, 'x')));
    history.push_back(AssistantText("回复"));

    agent::TrimReport report;
    const auto out = agent::ShrinkOversizedToolResults(history, /*window_tokens=*/100000, 1.0, &report);

    REQUIRE(out.size() == history.size());
    CHECK(std::get<api::ToolResultBlock>(out[2].content[0]).content == std::string(1000, 'x'));
    CHECK_FALSE(report.truncated_results);
}

TEST_CASE("ShrinkOversizedToolResults: 单条超窗口 25% 即截尾,落回线内并打标注,配对不破") {
    std::vector<api::Message> history;
    history.push_back(UserText("用户输入"));
    history.push_back(AssistantToolUse("tool_big", "some_tool"));
    // 120000 个 ASCII = 30000 token;窗口 100000 的 25% 线是 25000,超线。
    history.push_back(UserToolResult("tool_big", std::string(120000, 'x')));
    history.push_back(AssistantText("回复"));

    agent::TrimReport report;
    const auto out = agent::ShrinkOversizedToolResults(history, /*window_tokens=*/100000, 1.0, &report);

    // 消息条数不变,tool_use/tool_result 配对原样。
    REQUIRE(out.size() == history.size());
    REQUIRE(std::holds_alternative<api::ToolResultBlock>(out[2].content[0]));
    const auto& tool_result = std::get<api::ToolResultBlock>(out[2].content[0]);
    CHECK(tool_result.tool_use_id == "tool_big");
    CHECK(tool_result.content.size() < 120000);
    CHECK(tool_result.content.find("[内容过长已截断]") != std::string::npos);
    // 截完落回 25% 线内(ASCII 4 字符 1 token)。
    CHECK(agent::EstimateUtf8Tokens(tool_result.content) <= 100000 * 25 / 100);
    CHECK(report.truncated_results);
}

TEST_CASE("ShrinkOversizedToolResults: 25% 判线的边界——恰好在线上不动,越一线就截") {
    // 窗口 100000,线 = 25000 token。恰好 100000 个 ASCII = 25000 token:不截。
    {
        std::vector<api::Message> history;
        history.push_back(UserToolResult("t", std::string(100000, 'x')));
        agent::TrimReport report;
        const auto out = agent::ShrinkOversizedToolResults(history, 100000, 1.0, &report);
        CHECK(std::get<api::ToolResultBlock>(out[0].content[0]).content.size() == 100000);
        CHECK_FALSE(report.truncated_results);
    }
    // 104000 个 ASCII = 26000 token:越线 1000 即截,截完精确落回线上
    //(标注自身的 token 已在预算里内扣)。
    {
        std::vector<api::Message> history;
        history.push_back(UserToolResult("t", std::string(104000, 'x')));
        agent::TrimReport report;
        const auto out = agent::ShrinkOversizedToolResults(history, 100000, 1.0, &report);
        const auto& result = std::get<api::ToolResultBlock>(out[0].content[0]);
        CHECK(result.content.size() < 104000);
        CHECK(agent::EstimateUtf8Tokens(result.content) <= 25000);
        CHECK(report.truncated_results);
    }
}

TEST_CASE("ShrinkOversizedToolResults: 校准系数进判定——同一份内容,系数放大就越线") {
    // 60000 ASCII = 15000 token;窗口 100000 的线是 25000。默认尺线内。
    std::vector<api::Message> history;
    history.push_back(UserToolResult("t", std::string(60000, 'x')));

    agent::TrimReport calm;
    const auto out_calm = agent::ShrinkOversizedToolResults(history, 100000, 1.0, &calm);
    CHECK_FALSE(calm.truncated_results);

    // 校准系数 2.0(真机实测虚估一倍):估算翻倍到 30000,越线即截。
    agent::TrimReport hot;
    const auto out_hot = agent::ShrinkOversizedToolResults(history, 100000, 2.0, &hot);
    CHECK(hot.truncated_results);
    CHECK(std::get<api::ToolResultBlock>(out_hot[0].content[0]).content.size() <
          std::get<api::ToolResultBlock>(out_calm[0].content[0]).content.size());
}

TEST_CASE("ShrinkOversizedToolResults: 窗口未知(0)落 128k 兜底,不裸奔") {
    // 600000 ASCII = 150000 token:128k 兜底窗口的 25% 线是 32000,必超。
    std::vector<api::Message> history;
    history.push_back(UserToolResult("t", std::string(600000, 'x')));

    agent::TrimReport report;
    const auto out = agent::ShrinkOversizedToolResults(history, /*window_tokens=*/0, 1.0, &report);
    const auto& result = std::get<api::ToolResultBlock>(out[0].content[0]);
    CHECK(report.truncated_results);
    // 截到 128k 兜底窗口的线内,不是清一色裁到底。
    CHECK(agent::EstimateUtf8Tokens(result.content) <= agent::kFallbackContextWindowTokens * 25 / 100);
}

TEST_CASE("ShrinkOversizedToolResults: 截断有下限,不会把 tool_result 截成空壳") {
    std::vector<api::Message> history;
    history.push_back(AssistantToolUse("tool_a", "t"));
    history.push_back(UserToolResult("tool_a", std::string(5000, 'x')));

    // 窗口小到线内预算为零:每条至少保底 1KB,不至于清空。
    const auto out = agent::ShrinkOversizedToolResults(history, /*window_tokens=*/100, 1.0, nullptr);
    REQUIRE(out.size() == history.size());
    const auto& tool_result = std::get<api::ToolResultBlock>(out[1].content[0]);
    CHECK(tool_result.content.size() >= 1024);
    CHECK(tool_result.content.find('x') != std::string::npos);
}

// 回归:截短按字节裸砍,刀口落进三字节汉字的腰上,合法 UTF-8 也被截成
// 非法——请求体 dump 当场 type_error.316,会话每回合必挂(真机实锤:
// messages[6].content[0].content 的 0xA6 悬空续字节)。
TEST_CASE("ShrinkOversizedToolResults: 中文超长 tool_result 截断后仍是合法 UTF-8") {
    std::vector<api::Message> history;
    history.push_back(AssistantToolUse("tool_cn", "t"));
    // 三字节汉字铺满:刀口无论落在哪儿都在多字节序列里。
    std::string big;
    for (int i = 0; i < 20000; ++i) {
        big += "码";
    }
    history.push_back(UserToolResult("tool_cn", big));

    const auto out = agent::ShrinkOversizedToolResults(history, /*window_tokens=*/10000, 1.0, nullptr);
    REQUIRE(out.size() == history.size());
    const auto& tool_result = std::get<api::ToolResultBlock>(out[1].content[0]);
    CHECK(tool_result.content.size() < big.size());
    CHECK(tool_result.content.find("[内容过长已截断]") != std::string::npos);
    CHECK(platform::IsValidUtf8(tool_result.content));  // 不劈半个字
}

TEST_CASE("ShrinkOversizedToolResults: 确定性——同一份历史两次截出一副形状") {
    // 追加律的地基:截断只看结果自身的 token 账,不看全量 overage;同一份
    // 输入每请求截出同一副输出,旧消息不随历史增长被追改(旧字节轴按全量
    // overage 截会滑窗,已拆)。
    const std::string big(120000, 'x');
    std::vector<api::Message> history;
    history.push_back(UserToolResult("t1", big));
    history.push_back(UserToolResult("t2", big));

    const auto first = agent::ShrinkOversizedToolResults(history, 100000, 1.0, nullptr);
    const auto second = agent::ShrinkOversizedToolResults(history, 100000, 1.0, nullptr);
    CHECK(std::get<api::ToolResultBlock>(first[0].content[0]).content ==
          std::get<api::ToolResultBlock>(second[0].content[0]).content);
    // 两条巨肥结果各自独立截断:第二条不会被"第一条已吃掉预算"牵连。
    CHECK(std::get<api::ToolResultBlock>(first[1].content[0]).content.size() ==
          std::get<api::ToolResultBlock>(first[0].content[0]).content.size());
}
