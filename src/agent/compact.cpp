#include "agent/compact.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include "api/assembler.hpp"

namespace lubancode::agent {

namespace {

// 跟 agent/context.cpp 里的同名私有 helper 语义一模一样(角色是 user、
// 且至少带一个 TextBlock 才算"一轮的开头"),但那边是匿名命名空间里的
// 私有函数、没导出,agent/context.hpp/.cpp 又不许改动,所以这里原样再写
// 一份,不去碰那个文件。
bool IsUserTurnStart(const api::Message& message) {
    if (message.role != api::Role::User) {
        return false;
    }
    for (const auto& block : message.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<api::Message> BuildCompactedHistory(const std::vector<api::Message>& history,
                                                  const api::Message& archive) {
    std::vector<api::Message> new_history;

    std::size_t start = history.size();
    for (std::size_t i = history.size(); i-- > 0;) {
        if (IsUserTurnStart(history[i])) {
            start = i;
            break;
        }
    }

    if (start >= history.size()) {
        // 没有可保留的用户轮,存档只能自己单独成一条。
        new_history.push_back(archive);
        return new_history;
    }

    // 存档正文并入保留轮的第一条 user 消息开头,不单独成一条消息——独立的
    // 存档 user 消息紧跟保留轮的 user 输入,就是相邻两条 user,违反 Anthropic
    // 的角色交替要求(标准端点 400;MiniMax 宽容,才一直没暴露)。
    std::string archive_text;
    for (const auto& block : archive.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            archive_text += std::get<api::TextBlock>(block).text;
        }
    }

    api::Message merged = history[start];
    bool merged_into_text = false;
    for (auto& block : merged.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            auto& text_block = std::get<api::TextBlock>(block);
            text_block.text = archive_text + "\n\n" + text_block.text;
            merged_into_text = true;
            break;
        }
    }
    if (!merged_into_text) {
        // IsUserTurnStart 保证有 TextBlock,这里纯防御。
        merged.content.push_back(api::TextBlock{archive_text});
    }
    new_history.push_back(std::move(merged));
    new_history.insert(new_history.end(), history.begin() + static_cast<std::ptrdiff_t>(start) + 1, history.end());

    return new_history;
}

std::expected<api::Message, api::Error> Compact(api::Backend& backend, const std::string& model,
                                                   const std::vector<api::Message>& history,
                                                   const std::string& focus) {
    std::string instruction =
        "以上是到目前为止的对话历史。请把它压缩成一份存档:保留任务目标、关键决策、涉及的文件路径、"
        "代码要点、尚未完成的事项;闲聊和过程细节(工具调用的中间试错、无关寒暄)可以舍弃。直接给出"
        "压缩后的正文,不要加任何解释性的前后缀。";
    if (!focus.empty()) {
        instruction += "重点保留:" + focus;
    }

    api::Request request;
    request.model = model;
    request.system = instruction;
    request.messages = history;

    // 踩过的坑:history 最后一条常常是 assistant 消息——/compact 一般紧跟在
    // 模型刚回复完之后触发,历史里还没来得及追加下一条用户输入。这时原样把
    // 整段历史塞进 messages 发出去,Anthropic/Responses 两边的 API 都会把它
    // 当成"续写最后这条 assistant 消息"(prefill continuation)处理,模型压根
    // 不理会 system 里"请总结"的指令,只顺着最后一条内容随手接几个字——实测
    // 真出现过压缩正文只有孤零零一个"2"字的情况(最后一条 assistant 消息原本
    // 就是"2"),此前记住的事实全丢了。补一条 user 消息收尾,让模型明确进入
    // "该我说话、按 system 指令办"的状态;这条消息本身不必带实质内容
    // (指令已经在 system 里了),只是个"请开始"的信号。history 最后一条
    // 若本来就是 user 角色(比如工具结果),模型下一轮天然该轮到它开口,
    // 不需要再补。
    if (!request.messages.empty() && request.messages.back().role == api::Role::Assistant) {
        api::Message trailer;
        trailer.role = api::Role::User;
        trailer.content.push_back(api::TextBlock{"请开始压缩,直接给出存档正文,不要重复原对话内容。"});
        request.messages.push_back(trailer);
    }
    request.max_tokens = 4096;

    api::MessageAssembler assembler;
    bool stream_error = false;
    std::string stream_error_message;

    const auto send_result = backend.send_stream(request, [&](const api::StreamEvent& event) {
        assembler.Feed(event);
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, api::StreamError>) {
                    stream_error = true;
                    stream_error_message = e.message;
                }
            },
            event);
    });

    if (!send_result.has_value()) {
        return std::unexpected(send_result.error());
    }
    if (stream_error) {
        return std::unexpected(api::Error{api::ErrorKind::Api, stream_error_message, 0});
    }

    const api::Message summary_message = assembler.BuildMessage();
    std::string summary_text;
    for (const auto& block : summary_message.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            summary_text += std::get<api::TextBlock>(block).text;
        }
    }

    api::Message archive;
    archive.role = api::Role::User;
    archive.content.push_back(api::TextBlock{"[对话存档,此前内容已压缩] " + summary_text});
    return archive;
}

}  // namespace lubancode::agent
