// 历史裁剪(agent/context.hpp):不超限不动、超限裁中间、tool_use/tool_result
// 永远成对、占位消息正确插入。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/context.hpp"
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

// 造一轮"user 文本 -> assistant tool_use -> user tool_result -> assistant 文本"
// 的完整对话,塞进 out 里。
void AppendFullTurn(std::vector<api::Message>& out, const std::string& turn_tag, std::size_t padding_chars) {
    out.push_back(UserText("用户输入 " + turn_tag + std::string(padding_chars, 'u')));
    out.push_back(AssistantToolUse("tool_" + turn_tag, "some_tool"));
    out.push_back(UserToolResult("tool_" + turn_tag, "工具结果 " + turn_tag + std::string(padding_chars, 'r')));
    out.push_back(AssistantText("助手回复 " + turn_tag + std::string(padding_chars, 'a')));
}

// 造一轮带两步工具往返的用户轮(user turn):user -> asst(tool) ->
// user(result) -> asst(tool) -> user(result) -> asst(正文)。TrimHistory
// 按"用户消息"切段,一轮不管里头几步,要么整段留、要么整段裁——测试用
// 它钉这层语义,不因 step 与 turn 分家而把一轮从中间劈开。
void AppendMultiStepTurn(std::vector<api::Message>& out, const std::string& turn_tag, std::size_t padding_chars) {
    out.push_back(UserText("用户输入 " + turn_tag + std::string(padding_chars, 'u')));
    out.push_back(AssistantToolUse("tool_a_" + turn_tag, "some_tool"));
    out.push_back(UserToolResult("tool_a_" + turn_tag, "第一步结果 " + turn_tag + std::string(padding_chars, 'r')));
    out.push_back(AssistantToolUse("tool_b_" + turn_tag, "some_tool"));
    out.push_back(UserToolResult("tool_b_" + turn_tag, "第二步结果 " + turn_tag + std::string(padding_chars, 'r')));
    out.push_back(AssistantText("助手回复 " + turn_tag + std::string(padding_chars, 'a')));
}

}  // namespace

TEST_CASE("EstimateChars: 累加所有文本/工具入参/工具结果的字节数") {
    std::vector<api::Message> history;
    history.push_back(UserText("12345"));
    history.push_back(AssistantText("abcde"));
    CHECK(agent::EstimateHistoryBytes(history) == 10);
}

TEST_CASE("TrimHistory: 不超限时原样返回,一条不动") {
    std::vector<api::Message> history;
    history.push_back(UserText("你好"));
    history.push_back(AssistantText("你也好"));

    const auto trimmed = agent::TrimHistory(history, /*max_chars=*/100000, /*keep_recent_turns=*/3);

    REQUIRE(trimmed.size() == history.size());
    CHECK(std::get<api::TextBlock>(trimmed[0].content[0]).text == "你好");
    CHECK(std::get<api::TextBlock>(trimmed[1].content[0]).text == "你也好");
}

TEST_CASE("TrimHistory: 超限时保留最早一轮和最近 N 轮,中间整轮替换成占位消息") {
    std::vector<api::Message> history;
    // 第一轮(要保留)
    AppendFullTurn(history, "turn0", 50);
    // 中间 5 轮(要被裁掉)
    for (int i = 1; i <= 5; ++i) {
        AppendFullTurn(history, "mid" + std::to_string(i), 50);
    }
    // 最近 3 轮(要保留)
    AppendFullTurn(history, "recent1", 50);
    AppendFullTurn(history, "recent2", 50);
    AppendFullTurn(history, "recent3", 50);

    const std::size_t full_chars = agent::EstimateHistoryBytes(history);
    REQUIRE(full_chars > 0);

    // 阈值设得比全量小,但比"第一轮 + 最近三轮"大,逼着中间 5 轮被裁掉。
    const auto trimmed = agent::TrimHistory(history, /*max_chars=*/full_chars / 2, /*keep_recent_turns=*/3);

    CHECK(trimmed.size() < history.size());

    // 第一轮还在(最早的 user 消息内容保留)。
    REQUIRE_FALSE(trimmed.empty());
    CHECK(trimmed[0].role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(trimmed[0].content[0]));
    CHECK(std::get<api::TextBlock>(trimmed[0].content[0]).text.find("turn0") != std::string::npos);

    // 裁剪说明并入了保留区间第一条 user 消息的开头(不再单独成一条消息——
    // 那样会跟后面的 user 输入连成相邻两条 user,违反角色交替)。
    bool found_notice = false;
    for (const auto& message : trimmed) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::TextBlock>(block)) {
                const std::string& text = std::get<api::TextBlock>(block).text;
                if (text.rfind("[早前对话已裁剪]", 0) == 0) {
                    found_notice = true;
                    // 并入:同一条消息里还带着 recent1 的原始输入。
                    CHECK(text.find("recent1") != std::string::npos);
                    CHECK(message.role == api::Role::User);
                }
            }
        }
    }
    CHECK(found_notice);

    // 裁剪后不允许出现相邻两条"带用户文本输入"的 user 消息(角色交替)。
    for (std::size_t i = 0; i + 1 < trimmed.size(); ++i) {
        const bool both_user = trimmed[i].role == api::Role::User && trimmed[i + 1].role == api::Role::User;
        CHECK_FALSE(both_user);
    }

    // 最近三轮的文本都还在。
    bool has_recent1 = false;
    bool has_recent2 = false;
    bool has_recent3 = false;
    bool has_mid = false;
    for (const auto& message : trimmed) {
        for (const auto& block : message.content) {
            std::string text;
            if (std::holds_alternative<api::TextBlock>(block)) {
                text = std::get<api::TextBlock>(block).text;
            } else if (std::holds_alternative<api::ToolResultBlock>(block)) {
                text = std::get<api::ToolResultBlock>(block).content;
            }
            if (text.find("recent1") != std::string::npos) has_recent1 = true;
            if (text.find("recent2") != std::string::npos) has_recent2 = true;
            if (text.find("recent3") != std::string::npos) has_recent3 = true;
            if (text.find("mid") != std::string::npos) has_mid = true;
        }
    }
    CHECK(has_recent1);
    CHECK(has_recent2);
    CHECK(has_recent3);
    CHECK_FALSE(has_mid);  // 中间几轮应该已经被裁掉了
}

TEST_CASE("TrimHistory: tool_use 与 tool_result 永远成对,不会被从中间截断") {
    std::vector<api::Message> history;
    for (int i = 0; i < 8; ++i) {
        AppendFullTurn(history, "t" + std::to_string(i), 200);
    }

    const std::size_t full_chars = agent::EstimateHistoryBytes(history);
    const auto trimmed = agent::TrimHistory(history, full_chars / 3, /*keep_recent_turns=*/2);

    // 逐条检查:如果一条 assistant 消息里有 ToolUseBlock,紧跟着下一条
    // 消息里必须能找到与之 id 对应的 ToolResultBlock(说明配对完整,
    // 没有被从中间截断导致落单)。
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
        for (const auto& block : trimmed[i].content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                const auto& tool_use = std::get<api::ToolUseBlock>(block);
                REQUIRE(i + 1 < trimmed.size());
                bool found_pair = false;
                for (const auto& next_block : trimmed[i + 1].content) {
                    if (std::holds_alternative<api::ToolResultBlock>(next_block)) {
                        if (std::get<api::ToolResultBlock>(next_block).tool_use_id == tool_use.id) {
                            found_pair = true;
                        }
                    }
                }
                CHECK(found_pair);
            }
        }
    }
}

TEST_CASE("TrimHistory: 按 user turn 切段,一轮内多步工具往返回整段留或整段裁") {
    // 一轮 = 一条用户输入到收口;一轮里可以有好几步(每次模型请求一步)。
    // TrimHistory 的"轮"是用户轮,不是模型步:被裁的轮六条消息一起消失,
    // 保留的轮六条消息一条不少,绝不把一轮从两步中间劈开。
    std::vector<api::Message> history;
    AppendMultiStepTurn(history, "turn0", 50);  // 第一轮(保留)
    for (int i = 1; i <= 4; ++i) {
        AppendMultiStepTurn(history, "mid" + std::to_string(i), 50);  // 中间轮(裁掉)
    }
    AppendMultiStepTurn(history, "recent1", 50);  // 最近一轮(保留)

    const std::size_t full_chars = agent::EstimateHistoryBytes(history);
    const auto trimmed = agent::TrimHistory(history, /*max_chars=*/full_chars / 3, /*keep_recent_turns=*/1);

    CHECK(trimmed.size() < history.size());

    // 保留的两轮各自完整:开头还是那条 user 文本;被保留轮的 tool_use 与
    // tool_result 配对一条不缺(两步共四块,全在)。
    REQUIRE_FALSE(trimmed.empty());
    CHECK(trimmed[0].role == api::Role::User);

    std::size_t kept_pairs = 0;
    for (std::size_t i = 0; i + 1 < trimmed.size(); ++i) {
        for (const auto& block : trimmed[i].content) {
            if (!std::holds_alternative<api::ToolUseBlock>(block)) {
                continue;
            }
            const auto& tool_use = std::get<api::ToolUseBlock>(block);
            for (const auto& next_block : trimmed[i + 1].content) {
                if (std::holds_alternative<api::ToolResultBlock>(next_block) &&
                    std::get<api::ToolResultBlock>(next_block).tool_use_id == tool_use.id) {
                    ++kept_pairs;
                }
            }
        }
    }
    // 两轮 x 每轮两步 = 四对,中间被裁的轮不许留下落单的半截。
    CHECK(kept_pairs == 4);

    // 中间轮确实整段没了:它们的工具 id 不再出现。
    for (const auto& message : trimmed) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                const std::string& id = std::get<api::ToolUseBlock>(block).id;
                CHECK(id.find("mid") == std::string::npos);
            }
        }
    }
}

TEST_CASE("TrimHistory: 轮数不够裁(第一轮和最近 N 轮已覆盖全部)时原样返回") {
    std::vector<api::Message> history;
    AppendFullTurn(history, "only1", 500000);
    AppendFullTurn(history, "only2", 500000);

    const std::size_t full_chars = agent::EstimateHistoryBytes(history);
    // 阈值远小于全量,强制触发"超限"分支,但只有 2 轮、keep_recent_turns=3
    // 时第一轮和"最近 3 轮"必然覆盖/重叠了全部历史,裁不动。
    const auto trimmed = agent::TrimHistory(history, full_chars / 10, /*keep_recent_turns=*/3);

    REQUIRE(trimmed.size() == history.size());
}

TEST_CASE("TrimHistory: 空历史直接返回空") {
    std::vector<api::Message> history;
    const auto trimmed = agent::TrimHistory(history, 100, 3);
    CHECK(trimmed.empty());
}

TEST_CASE("MaxContextCharsFromEnv: 没设置环境变量时返回默认值") {
    // 测试环境里一般不会设这个变量;这里只验证函数在没设置时不崩、给出合理默认值。
    const std::size_t value = agent::MaxContextCharsFromEnv();
    CHECK(value > 0);
}

TEST_CASE("TrimHistory: 轮数不够裁但单条 tool_result 超大时,内容尾部截断并标注,配对不破") {
    std::vector<api::Message> history;
    history.push_back(UserText("用户输入"));
    history.push_back(AssistantToolUse("tool_big", "some_tool"));
    history.push_back(UserToolResult("tool_big", std::string(50000, 'x')));
    history.push_back(AssistantText("回复"));

    // 只有一轮,按轮裁不动;阈值远小于 tool_result 的体积,逼出截断兜底。
    const auto trimmed = agent::TrimHistory(history, /*max_chars=*/10000, /*keep_recent_turns=*/3);

    // 消息条数不变,tool_use/tool_result 配对原样。
    REQUIRE(trimmed.size() == history.size());
    REQUIRE(std::holds_alternative<api::ToolResultBlock>(trimmed[2].content[0]));
    const auto& tool_result = std::get<api::ToolResultBlock>(trimmed[2].content[0]);
    CHECK(tool_result.tool_use_id == "tool_big");
    CHECK(tool_result.content.size() < 50000);
    CHECK(tool_result.content.find("[内容过长已截断]") != std::string::npos);

    // 截断后总量确实落回了阈值以内。
    CHECK(agent::EstimateHistoryBytes(trimmed) <= 10000);
}

TEST_CASE("TrimHistory: 截断有下限,不会把 tool_result 截成空壳") {
    std::vector<api::Message> history;
    history.push_back(UserText("输入"));
    history.push_back(AssistantToolUse("tool_a", "t"));
    history.push_back(UserToolResult("tool_a", std::string(5000, 'x')));

    // 阈值小到不可能满足,但每条 tool_result 至少保底 1KB,不至于清空。
    const auto trimmed = agent::TrimHistory(history, /*max_chars=*/10, /*keep_recent_turns=*/3);

    REQUIRE(trimmed.size() == history.size());
    const auto& tool_result = std::get<api::ToolResultBlock>(trimmed[2].content[0]);
    CHECK(tool_result.content.size() >= 1024);
    CHECK(tool_result.content.find('x') != std::string::npos);
}

// ---------------------------------------------------------------------------
// 0.31.x 分层压缩第一期:统一 token 口径 + 硬裁剪报告
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

TEST_CASE("TrimHistory: TrimReport 报告丢轮与截结果,不丢时全默认假") {
    // 不超限:一条不动,报告全假。
    {
        std::vector<api::Message> history;
        history.push_back(UserText("你好"));
        agent::TrimReport report;
        const auto trimmed = agent::TrimHistory(history, 100000, 3, &report);
        CHECK(trimmed.size() == history.size());
        CHECK_FALSE(report.trimmed_turns);
        CHECK_FALSE(report.truncated_results);
        CHECK(report.dropped_messages == 0);
    }
    // 超限丢轮:报告真、条数对得上(丢的 = 原长 - 新长,占位并进了保留首条)。
    {
        std::vector<api::Message> history;
        AppendFullTurn(history, "turn0", 50);
        for (int i = 1; i <= 5; ++i) {
            AppendFullTurn(history, "mid" + std::to_string(i), 50);
        }
        AppendFullTurn(history, "recent1", 50);
        const std::size_t full = agent::EstimateHistoryBytes(history);
        agent::TrimReport report;
        const auto trimmed = agent::TrimHistory(history, full / 2, 1, &report);
        CHECK(report.trimmed_turns);
        CHECK(report.dropped_messages > 0);
        CHECK(report.dropped_messages == history.size() - trimmed.size());
    }
    // 超大工具结果截尾:truncated_results 真,轮没丢。
    {
        std::vector<api::Message> history;
        history.push_back(UserText("输入"));
        history.push_back(AssistantToolUse("tool_big", "t"));
        history.push_back(UserToolResult("tool_big", std::string(50000, 'x')));
        agent::TrimReport report;
        const auto trimmed = agent::TrimHistory(history, 10000, 3, &report);
        CHECK(report.truncated_results);
        CHECK_FALSE(report.trimmed_turns);
        CHECK(trimmed.size() == history.size());
    }
}
