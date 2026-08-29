// ContextManager 的钉子测试(骨架拆解批四·病六拆出的那只手):
//   - 双历史不变式:持久账(history_)只收真输入,请求账(request_history_)
//     另收每轮动态上下文;两账的公共前缀逐条相等、条数只差动态块;
//   - cache epoch 账:追加律判定的断因点名(model_changed /
//     history_compacted / hard_trim),epoch 只增不减;
//   - sticky 工作视图:真裁过一次后钉住,后续只在尾部追加。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/context.hpp"
#include "agent/context_manager.hpp"
#include "api/types.hpp"

namespace {

using namespace lubancode;

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Request MakeRequest(const std::vector<api::Message>& messages, const std::string& model = "m") {
    api::Request request;
    request.model = model;
    request.system = "sys";
    request.messages = messages;
    return request;
}

api::Message WithContextBlock(api::Message message, const std::string& context) {
    message.content.push_back(api::TextBlock{context});
    return message;
}

}  // namespace

TEST_CASE("双历史不变式:动态上下文只进请求账,持久账只见真输入") {
    agent::ContextManager context;
    REQUIRE(context.durable_history().empty());
    REQUIRE(context.request_history().empty());
    CHECK(context.cache_epoch() == 1);

    api::Message user = UserMessage("第一问");
    context.PushUserTurn(user, WithContextBlock(user, "[记忆召回] 动态上下文"));
    context.PushMessage(AssistantMessage("第一答"));

    REQUIRE(context.durable_history().size() == 2);
    REQUIRE(context.request_history().size() == 2);
    // 持久账的第一条只有一个文本块(真输入);请求账的第一条多一块动态上下文。
    CHECK(context.durable_history().front().content.size() == 1);
    CHECK(context.request_history().front().content.size() == 2);
    // 两账从第二条起逐条相等(比较文本块内容)。
    const auto& durable_text = std::get<api::TextBlock>(context.durable_history()[1].content.front()).text;
    const auto& request_text = std::get<api::TextBlock>(context.request_history()[1].content.front()).text;
    CHECK(durable_text == request_text);

    // 来信注入:末条是 assistant 的边界,来信新起一条 user(两账同注)。
    context.InjectIncoming(UserMessage("[字条] 跟进"));
    CHECK(context.durable_history().size() == 3);
    CHECK(context.request_history().size() == 3);
    // 再来一封:末条已是 user,文本块追加进末条,不起连排 user。
    context.InjectIncoming(UserMessage("[字条] 再跟进"));
    CHECK(context.durable_history().size() == 3);
    CHECK(context.request_history().size() == 3);
    CHECK(context.durable_history().back().content.size() == 2);
    CHECK(context.request_history().back().content.size() == 2);
}

TEST_CASE("epoch 账:首份无从比较,追加不破,改 model 断一次点名 model_changed") {
    agent::ContextManager context;
    context.PushUserTurn(UserMessage("问"), UserMessage("问"));

    // 首份请求:没有上一份,天然算追加。
    auto first = context.AccountRequest(MakeRequest(context.request_history()));
    CHECK(first.append_only);
    CHECK_FALSE(first.had_previous);
    CHECK(context.cache_epoch() == 1);

    // 追加一条 assistant 再发:追加律成立,epoch 不动。
    context.PushMessage(AssistantMessage("答"));
    auto second = context.AccountRequest(MakeRequest(context.request_history()));
    CHECK(second.append_only);
    CHECK(second.had_previous);
    CHECK(second.appended_messages == 1);
    CHECK(context.cache_epoch() == 1);

    // 换 model:断一次,断因点名,epoch +1。
    auto broken = context.AccountRequest(MakeRequest(context.request_history(), "other-model"));
    CHECK_FALSE(broken.append_only);
    CHECK(broken.break_reason == "model_changed");
    CHECK(context.cache_epoch() == 2);
}

TEST_CASE("epoch 账:ReplaceHistory 开新 epoch,断因点名 history_compacted") {
    agent::ContextManager context;
    context.PushUserTurn(UserMessage("问"), UserMessage("问"));
    (void)context.AccountRequest(MakeRequest(context.request_history()));

    std::vector<api::Message> compacted;
    compacted.push_back(UserMessage("[对话存档] 摘要"));
    context.ReplaceHistory(compacted);

    CHECK(context.durable_history().size() == 1);
    CHECK(context.request_history().size() == 1);
    CHECK(context.cache_epoch() == 2);
    // 新 epoch 的第一份请求:指纹已清,无从比较;但 compact 的断因已记,
    // 下一份请求若再断,优先用显式因。
    auto after = context.AccountRequest(MakeRequest(context.request_history()));
    CHECK(after.append_only);
    CHECK_FALSE(after.had_previous);
}

TEST_CASE("epoch 账:hard trim 的显式因优先于指纹反推") {
    agent::ContextManager context;
    context.PushUserTurn(UserMessage("问"), UserMessage("问"));
    (void)context.AccountRequest(MakeRequest(context.request_history()));

    // 模拟 hard trim 真丢了东西:loop 先记账,视图里少了一条(追改旧消息)。
    context.NotePendingEpochBreak("hard_trim");
    std::vector<api::Message> trimmed = context.durable_history();
    auto forced = context.AccountRequest(MakeRequest({}));
    CHECK_FALSE(forced.append_only);
    CHECK(forced.break_reason == "hard_trim");  // 显式因压过 old_message_changed
    CHECK(context.cache_epoch() == 2);
    (void)trimmed;
}

TEST_CASE("sticky 工作视图:真裁一次后钉住,后续只在尾部追加") {
    agent::ContextManager context;
    // 五轮长历史,每轮塞 400 字节,把 max_context_chars 压到 1000 触发轮级裁剪。
    for (int i = 0; i < 5; ++i) {
        api::Message user = UserMessage("第" + std::to_string(i) + "问 " + std::string(400, 'x'));
        context.PushUserTurn(user, user);
        context.PushMessage(AssistantMessage("答 " + std::string(100, 'y')));
    }

    const agent::ContextViewBudget budget{2400};
    auto first = context.BuildWorkingView(budget);
    CHECK(first.trim.trimmed_turns);  // 真动手裁了:中间整轮被丢

    // 再来一轮:追加进钉住的视图,不再重算"第一轮 + 最近 N 轮"。
    api::Message more = UserMessage("第六问 " + std::string(200, 'z'));
    context.PushUserTurn(more, more);
    auto second = context.BuildWorkingView(budget);
    CHECK_FALSE(second.trim.trimmed_turns);  // 余量内:这轮没再真裁
    // 钉住的视图尾部带上了新消息。
    bool saw_new = false;
    for (const auto& message : second.messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block);
                text != nullptr && text->text.find("第六问") != std::string::npos) {
                saw_new = true;
            }
        }
    }
    CHECK(saw_new);
}

// ---------------------------------------------------------------------------
// 压力 dry-run 视图(P1-1 口径统一):触发线、/context 与压缩前后账都该
// 拿"结构压缩后会真发出去的那本"估,不拿未压缩全量估——真机 189k 的估账
// 对 47k 的真实请求,就是两把尺分家的账。
// ---------------------------------------------------------------------------

namespace {

api::Message AssistantToolUse(const std::string& id, const std::string& name, const nlohmann::json& input) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::ToolUseBlock{id, name, input});
    return message;
}

api::Message UserToolResult(const std::string& tool_use_id, const std::string& content) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::ToolResultBlock{tool_use_id, content, false});
    return message;
}

}  // namespace

TEST_CASE("BuildPressureDryRunView: 重复工具结果被收敛,估算显著低于全量,且不污染决策台账") {
    agent::ContextManager context;
    // 一轮里同一文件读三遍:冷区(最后一条用户输入之前)的重复只读结果在
    // 工作视图里只留一份正文,后来者换引用。
    const std::string file_body = "同一份文件正文," + std::string(2000, 'f');
    context.PushUserTurn(UserMessage("读一下这个文件"), UserMessage("读一下这个文件"));
    const nlohmann::json input = {{"path", "src/app.cpp"}};
    for (int i = 0; i < 3; ++i) {
        context.PushMessage(AssistantToolUse("use_" + std::to_string(i), "read_file", input));
        context.PushMessage(UserToolResult("use_" + std::to_string(i), file_body));
    }
    context.PushMessage(AssistantMessage("读完了,说点结论。"));

    const std::size_t full_tokens = agent::EstimateHistoryTokens(context.request_history());
    const std::size_t pressure_tokens = agent::EstimateHistoryTokens(context.BuildPressureDryRunView());
    // 三份重复收敛成一份正文 + 两条引用:估算至少省掉一份以上的量。
    CHECK(pressure_tokens < full_tokens * 3 / 4);

    // dry-run 不落决策台账:正式 BuildWorkingView 照常从头定形,视图与
    // dry-run 同形状(估算可复核)。
    CHECK(context.result_view_memo().decisions.empty());
    agent::ContextViewBudget budget{10000000};
    const auto working = context.BuildWorkingView(budget);
    CHECK(agent::EstimateHistoryTokens(working.messages) == pressure_tokens);
    CHECK_FALSE(context.result_view_memo().decisions.empty());
}

TEST_CASE("BuildPressureDryRunView: 关掉结构压缩时原样返回请求账") {
    agent::ContextManager context;
    context.set_structural_compression_enabled(false);
    context.PushUserTurn(UserMessage("问"), UserMessage("问"));
    context.PushMessage(AssistantMessage("答"));
    const auto view = context.BuildPressureDryRunView();
    REQUIRE(view.size() == 2);
    CHECK(std::get<api::TextBlock>(view[0].content[0]).text == "问");
}

// ---- 问题 9:每请求缓存诊断账(稳定前缀/指纹/wire 公共前缀) -----------------

TEST_CASE("诊断账: 稳定前缀条数与合成指纹随追加律走,常态 wire 字节不可得") {
    agent::ContextManager context;
    context.PushUserTurn(UserMessage("第一问"), UserMessage("第一问"));
    context.PushMessage(AssistantMessage("第一答"));

    // 首份请求:没有上一份,稳定前缀 0 条、指纹空;wire 公共前缀不可得。
    auto first = context.AccountRequest(MakeRequest(context.request_history()));
    CHECK_FALSE(first.had_previous);
    CHECK(first.stable_prefix_messages == 0);
    CHECK(first.prefix_hash.empty());
    CHECK(first.total_messages == 2);
    CHECK(first.wire_common_prefix_bytes == -1);  // 没传 wire dump:不冒充 0
    CHECK(first.cache_epoch == 1);

    // 追加一条再发:稳定前缀 = 上一份的 2 条,指纹非空且 16 hex,epoch 不动。
    context.PushMessage(UserMessage("第二问"));
    auto second = context.AccountRequest(MakeRequest(context.request_history()));
    CHECK(second.had_previous);
    CHECK(second.stable_prefix_messages == 2);
    CHECK(second.total_messages == 3);
    CHECK(second.prefix_hash.size() == 16);
    CHECK(second.cache_epoch == 1);
    CHECK(second.wire_common_prefix_bytes == -1);

    // 追改旧消息:追加律破,稳定前缀缩到分岔处,epoch +1 并点名。
    std::vector<api::Message> rewritten = context.request_history();
    std::get<api::TextBlock>(rewritten[0].content[0]).text = "改口";
    auto broken = context.AccountRequest(MakeRequest(rewritten));
    CHECK_FALSE(broken.append_only);
    CHECK(broken.break_reason == "old_message_changed");
    CHECK(broken.stable_prefix_messages == 0);
    CHECK(broken.prefix_hash.empty());
    CHECK(broken.cache_epoch == 2);
}

TEST_CASE("诊断账: wire dump 只在递进来才算,首记不可得、次记可比;换史清 dump") {
    agent::ContextManager context;
    context.PushUserTurn(UserMessage("问"), UserMessage("问"));

    const std::string body_a = "AAAA-prefix-body.AAAA";
    const std::string body_b = "AAAA-prefix-body.BBBB";
    // 第一次(诊断模式刚开):没有上一份 dump,不可得,但这份已留存。
    auto first = context.AccountRequest(MakeRequest(context.request_history()), &body_a);
    CHECK(first.wire_common_prefix_bytes == -1);
    // 第二次:与留存的 body_a 量公共前缀("AAAA-prefix-body." 17 字节)。
    auto second = context.AccountRequest(MakeRequest(context.request_history()), &body_b);
    CHECK(second.wire_common_prefix_bytes == 17);
    // 常态(不传 dump):不做全序列化,字段回 -1,留存的 dump 不动。
    auto third = context.AccountRequest(MakeRequest(context.request_history()));
    CHECK(third.wire_common_prefix_bytes == -1);
    // 换史(新 epoch):留存的 dump 清掉,下一份又从"不可得"起步。
    context.ReplaceHistory({UserMessage("[存档] 摘要")});
    auto after = context.AccountRequest(MakeRequest(context.request_history()), &body_b);
    CHECK(after.wire_common_prefix_bytes == -1);
}
