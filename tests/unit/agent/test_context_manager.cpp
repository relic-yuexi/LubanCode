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
