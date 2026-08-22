// connection.hpp 的实现:读分帧、逐行路由、写出站队列。
#include "app_server/connection.hpp"

#include <utility>
#include <vector>

#include "runtime/id_authority.hpp"

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

}  // namespace

StdioConnection::StdioConnection(std::shared_ptr<Dispatcher> dispatcher, LineWriter writer,
                                  ChunkReader reader, std::size_t outbox_capacity)
    : dispatcher_(std::move(dispatcher)), writer_(std::move(writer)), reader_(std::move(reader)),
      outbox_(outbox_capacity) {}

void StdioConnection::EmitEvent(std::string_view method, const nlohmann::json& params) {
    // 事件显式 seq(阶段 3 冻结):进程内单调号,连接层统一盖。params
    // 是拷贝进(不污染调用方),盖完再序列化。
    nlohmann::json stamped = params;
    if (stamped.is_object()) {
        stamped[kSeqField] = runtime::ProcessIdAuthority().NextSeq();
    }
    if (!outbox_.Push(SerializeMessage(MakeEvent(method, stamped)), EventMustKeep(method))) {
        // 丢事件了(可丢事件撞满且合并救不下):补一条 queue/overflow
        // 通报(它自己是 must_keep)。params 从被丢事件的 params 里借
        // threadId/turnId,drop/coalesce 账用 outbox 的累计值。
        const std::string thread_id = stamped.is_object() ? stamped.value("threadId", std::string())
                                                          : std::string();
        const std::string turn_id =
            stamped.is_object() ? stamped.value("turnId", std::string()) : std::string();
        nlohmann::json overflow_params;
        overflow_params[kSeqField] = runtime::ProcessIdAuthority().NextSeq();
        overflow_params["threadId"] = thread_id;
        overflow_params["turnId"] = turn_id;
        overflow_params["dropped"] = outbox_.dropped();
        overflow_params["coalesced"] = outbox_.coalesced();
        outbox_.Push(SerializeMessage(MakeEvent(kEventQueueOverflow, overflow_params)),
                     EventMustKeep(kEventQueueOverflow));
        Diagnose("出站队列溢出:累计丢 " + std::to_string(outbox_.dropped()) + " 条,合并救下 " +
                 std::to_string(outbox_.coalesced()) + " 条");
    }
}

void StdioConnection::ProcessLine(const std::string& line) {
    // 空行:既是坏报文又是无害的心跳噪音,回 parse error 太吵,静默跳过
    // (一行必须是一条完整消息,空行什么都不是)。
    if (line.empty()) {
        return;
    }

    EnvelopeError error;
    const std::optional<IncomingMessage> message = ParseIncoming(line, error);
    if (!message.has_value()) {
        // 坏报文:稳定错误码,不崩。id 捞得出就回,捞不出回 null。
        const nlohmann::json response = error.has_id
                                             ? MakeError(error.id, error.code, error.message)
                                             : MakeErrorForUnparseable(error.code, error.message);
        writer_(SerializeMessage(response));
        return;
    }

    DispatchContext context;
    context.emit_event = [this](std::string_view method, const nlohmann::json& params, bool must_keep) {
        if (must_keep == EventMustKeep(method)) {
            // 分型一致:走统一出口(seq 盖章 + 溢出通报)。
            EmitEvent(method, params);
            return;
        }
        // 调用方给了与分型表不同的保全等级:尊重调用方(比如装配层
        // 刻意放行的自定义事件),但 seq 照盖。
        nlohmann::json stamped = params;
        if (stamped.is_object()) {
            stamped[kSeqField] = runtime::ProcessIdAuthority().NextSeq();
        }
        outbox_.Push(SerializeMessage(MakeEvent(method, stamped)), must_keep);
    };
    // 反向请求响应的落点:装配层给了就用(审批/ask_user 悬起件的配对口)。
    if (resolve_interaction_) {
        context.resolve_interaction = resolve_interaction_;
    }

    DispatchOutcome outcome;
    switch (message->kind) {
        case IncomingMessage::Kind::Request:
            outcome = dispatcher_->HandleRequest(message->request, context);
            break;
        case IncomingMessage::Kind::Notification:
            outcome = dispatcher_->HandleNotification(message->notification, context);
            break;
        case IncomingMessage::Kind::Response:
            outcome = dispatcher_->HandleResponse(message->response);
            break;
    }

    for (const std::string& outbound_line : outcome.outbound) {
        writer_(outbound_line);
    }
    if (outcome.close_connection) {
        closed_.store(true);
    }
}

int StdioConnection::Run() {
    while (!closed_.load()) {
        const std::string chunk = reader_();
        if (chunk.empty()) {
            // EOF / 断管:收口。在跑回合的打断与终态冲刷是后续接真执行
            // 线后的事;骨架期队列里剩的直接刷出去。
            break;
        }
        for (const std::string& line : framer_.Feed(chunk)) {
            ProcessLine(line);
            if (closed_.load()) {
                break;
            }
        }
        if (framer_.overflowed()) {
            // 超长行:尽力回一条 parse error,然后退线——协议已不可信。
            Diagnose("入站单行超过上限,退线");
            writer_(SerializeMessage(MakeErrorForUnparseable(kErrParseError, "单行超过长度上限")));
            closed_.store(true);
            break;
        }
    }

    // 收线:把出站队列里剩的刷完(终态不悄悄丢)。
    while (const std::optional<std::string> line = outbox_.Pop()) {
        writer_(*line);
    }
    closed_.store(true);
    return 0;
}

bool WriteProtocolLine(const std::string& line) {
    if (std::fputs(line.c_str(), stdout) == EOF && !line.empty()) {
        return false;
    }
    if (std::fputc('\n', stdout) == EOF) {
        return false;
    }
    if (std::fflush(stdout) == EOF) {
        return false;
    }
    return true;
}

std::string ReadStdinChunk() {
    char buffer[65536];
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer), stdin);
    if (got == 0) {
        return std::string();
    }
    return std::string(buffer, got);
}

}  // namespace lubancode::app_server
