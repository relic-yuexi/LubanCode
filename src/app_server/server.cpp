// server.hpp 的实现:装配方法表、thread 账、turn/start 的整回合驱动。
#include "app_server/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>
#include <vector>

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

// usage 报告 -> 事件字段(五项原样,缺失字段前端自己看)。
nlohmann::json UsageToJson(const api::Usage& usage) {
    return nlohmann::json{{"inputTokens", usage.input_tokens},
                          {"outputTokens", usage.output_tokens},
                          {"cacheReadTokens", usage.cache_read_tokens},
                          {"cacheCreationTokens", usage.cache_creation_tokens},
                          {"outputReasoningTokens", usage.output_reasoning_tokens}};
}

// 一回合聚合的 usage(各请求求和)。
api::Usage SumUsage(const std::vector<api::UsageReport>& reports) {
    api::Usage total;
    for (const api::UsageReport& report : reports) {
        total.input_tokens += report.usage.input_tokens;
        total.output_tokens += report.usage.output_tokens;
        total.cache_read_tokens += report.usage.cache_read_tokens;
        total.cache_creation_tokens += report.usage.cache_creation_tokens;
        total.output_reasoning_tokens += report.usage.output_reasoning_tokens;
    }
    return total;
}

}  // namespace

std::string PlatformId() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

Server::Server(ServerOptions options, BackendFactory backend_factory, RegistryFactory registry_factory)
    : options_(std::move(options)), backend_factory_(std::move(backend_factory)),
      registry_factory_(std::move(registry_factory)), sessions_dir_(options_.sessions_dir) {
    dispatcher_ = std::make_shared<Dispatcher>();
    dispatcher_->SetInitializeResultFactory(
        [this]() { return MakeInitializeResult(options_.lubancode_version, PlatformId()); });
    RegisterMethods();
}

void Server::RegisterMethods() {
    // thread/start
    dispatcher_->RegisterMethod(
        kMethodThreadStart, [this](const IncomingRequest& request, DispatchContext& context)
                           -> std::optional<nlohmann::json> {
            std::string error_code;
            // 参数表在 handler 内查(纯函数,错误码稳定)。
            const ParamsCheck base = CheckThreadStartParams(request.params);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            const nlohmann::json result = HandleThreadStart(request.params, error_code);
            if (!error_code.empty()) {
                return MakeError(request.id, kErrInternalError, "thread/start 失败: " + error_code);
            }
            // thread/started 事件在响应之前发:前端先见事件后见响应,顺眼
            // 也顺逻辑(threadId 是事件给出来的身份)。
            context.emit_event(kEventThreadStarted,
                               MakeThreadStartedParams(result.value("threadId", std::string()),
                                                        result.value("cwd", std::string())),
                               false);
            return MakeResult(request.id, result);
        });

    // thread/list
    dispatcher_->RegisterMethod(
        kMethodThreadList, [this](const IncomingRequest& request, DispatchContext&)
                             -> std::optional<nlohmann::json> { return MakeResult(request.id, HandleThreadList()); });

    // thread/stop
    dispatcher_->RegisterMethod(
        kMethodThreadStop, [this](const IncomingRequest& request, DispatchContext& context)
                           -> std::optional<nlohmann::json> {
            std::string thread_id;
            const ParamsCheck base = CheckThreadStopParams(request.params, thread_id);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json result = HandleThreadStop(thread_id, error_code);
            if (!error_code.empty()) {
                return MakeError(request.id, kErrInvalidParams, "thread/stop 失败: " + error_code);
            }
            context.emit_event(kEventThreadStopped, MakeThreadStoppedParams(thread_id), false);
            return MakeResult(request.id, result);
        });

    // turn/start
    dispatcher_->RegisterMethod(
        kMethodTurnStart, [this](const IncomingRequest& request, DispatchContext& context)
                          -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::string text;
            const ParamsCheck base = CheckTurnStartParams(request.params, thread_id, text);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json completed = HandleTurnStart(thread_id, text, error_code);
            if (!error_code.empty()) {
                if (error_code == "already_running") {
                    return MakeError(request.id, kErrTurnAlreadyRunning,
                                     "该 thread 已有回合在跑: " + thread_id);
                }
                return MakeError(request.id, kErrInvalidParams, "turn/start 失败: " + error_code);
            }
            return MakeResult(request.id, completed);
        });
}

std::size_t Server::active_thread_count() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    return threads_.size();
}

nlohmann::json Server::HandleThreadStart(const nlohmann::json& params, std::string& out_error_code) {
    out_error_code.clear();
    auto record = std::make_shared<ThreadRecord>();
    record->cwd = params.value("cwd", std::string());
    if (record->cwd.empty()) {
        record->cwd = options_.cwd;
    }

    // 会话账:复用 SessionStore,不另立第二本账。首句摘要用一句占位的
    // 协议话——thread/start 阶段还没有用户文本(单子的存档恢复线会把
    // 真首句补进来);MakeSessionId 的 slug 拿它生成文件名。
    record->thread_id = agent::MakeSessionId(agent::NowIdTimestamp(), "app-server-thread");
    if (!sessions_dir_.empty()) {
        record->store = std::make_unique<agent::SessionStore>(sessions_dir_);
        agent::SessionMeta meta;
        meta.wire = "app-server";
        meta.model = std::string(); // 骨架期没有模型名可写(假 backend)
        meta.cwd = record->cwd;
        meta.started_at = agent::NowTimestamp();
        if (!record->store->Begin(meta, record->thread_id)) {
            // 落盘失败不拦协议:thread 照开,只打 stderr。
            Diagnose("会话建档失败,本场不落盘: " + sessions_dir_);
            record->store.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads_[record->thread_id] = record;
    }
    Diagnose("thread 已建: " + record->thread_id);
    return nlohmann::json{{"threadId", record->thread_id}, {"cwd", record->cwd}};
}

nlohmann::json Server::HandleThreadList() {
    // 列举走现成的 ListSessions(会话目录真扫);目录没配就只报活着的。
    std::vector<nlohmann::json> entries;
    if (!sessions_dir_.empty()) {
        for (const agent::SessionListEntry& entry : agent::ListSessions(sessions_dir_, 100)) {
            entries.push_back(nlohmann::json{{"threadId", entry.id},
                                             {"startedAt", entry.started_at},
                                             {"cwd", entry.cwd},
                                             {"title", entry.title},
                                             {"firstUserText", entry.first_user_text},
                                             {"messageCount", entry.message_count}});
        }
    }
    return MakeThreadListResult(entries);
}

nlohmann::json Server::HandleThreadStop(const std::string& thread_id, std::string& out_error_code) {
    out_error_code.clear();
    std::shared_ptr<ThreadRecord> record;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        const auto it = threads_.find(thread_id);
        if (it == threads_.end()) {
            out_error_code = "不认识的 threadId: " + thread_id;
            return nlohmann::json();
        }
        record = it->second;
        threads_.erase(it);
    }
    if (record->turn_running.load()) {
        // 在跑回合的打断是 Broker 那条线的活;骨架期同步驱动,到不了这里。
        Diagnose("thread 停场时有回合在跑(骨架期不并发,防御): " + thread_id);
    }
    if (record->store != nullptr) {
        record->store->Reset(); // 文件留在磁盘上,只是句柄收掉
    }
    Diagnose("thread 已停: " + thread_id);
    return MakeThreadStoppedResult();
}

nlohmann::json Server::HandleTurnStart(const std::string& thread_id, const std::string& text,
                                       std::string& out_error_code) {
    out_error_code.clear();

    std::shared_ptr<ThreadRecord> record;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        const auto it = threads_.find(thread_id);
        if (it == threads_.end()) {
            out_error_code = "不认识的 threadId: " + thread_id;
            return nlohmann::json();
        }
        record = it->second;
    }

    // 同一 thread 同拍两轮:协议明拒(单子验收:规矩写死并有测试)。
    bool expected = false;
    if (!record->turn_running.compare_exchange_strong(expected, true)) {
        out_error_code = "already_running";
        return nlohmann::json();
    }

    const std::string turn_id = "turn-" + std::to_string(++turn_counter_);
    record->turn_id = turn_id;

    connection_->EmitEvent(kEventTurnStarted, MakeTurnStartedParams(thread_id, turn_id));
    if (record->store != nullptr) {
        api::Message user_message;
        user_message.role = api::Role::User;
        user_message.content.push_back(api::TextBlock{text});
        record->store->AppendMessage(user_message);
    }

    // 事件账:三条 item 各自的 id(回合内单调:0,1,2...)。seq 由出站
    // 顺序天然保住(单写者:本线程),显式序号字段是 schema 冻结时的
    // 待定项(见 protocol.hpp 的 kEventQueueOverflow 注释),骨架期靠
    // 发送次序。
    nlohmann::json completed_params;
    {
        // 装配:假 backend + 空注册表(骨架期整回合就是模型文本流;工具
        // 审批与真工具链是 Broker/审批线的事)。
        std::unique_ptr<api::Backend> backend = backend_factory_();
        std::unique_ptr<tools::ToolRegistry> registry =
            registry_factory_ ? registry_factory_() : std::make_unique<tools::ToolRegistry>();

        agent::AgentRuntimeProfile profile;
        profile.max_steps_per_turn = 32; // 骨架期防跑飞;真配置线接进来再换
        agent::AgentLoop loop(*backend, *registry, std::move(profile), std::string("lubancode app-server"));

        agent::Callbacks callbacks;
        std::string text_item_id;
        callbacks.on_text_delta = [&](const std::string& delta) {
            if (text_item_id.empty()) {
                text_item_id = "item-0";
                connection_->EmitEvent(kEventItemStarted,
                                       MakeItemStartedParams(thread_id, turn_id, text_item_id, kItemTypeText,
                                                             nlohmann::json::object()));
            }
            connection_->EmitEvent(kEventItemDelta,
                                   MakeItemDeltaParams(thread_id, turn_id, text_item_id, delta));
        };
        std::string thinking_item_id;
        callbacks.on_thinking_delta = [&](const std::string& delta) {
            if (thinking_item_id.empty()) {
                thinking_item_id = "item-t";
                connection_->EmitEvent(kEventItemStarted,
                                       MakeItemStartedParams(thread_id, turn_id, thinking_item_id,
                                                             kItemTypeThinking, nlohmann::json::object()));
            }
            connection_->EmitEvent(kEventItemDelta,
                                   MakeItemDeltaParams(thread_id, turn_id, thinking_item_id, delta));
        };
        std::vector<api::UsageReport> usage_reports;
        callbacks.on_usage = [&](const api::UsageReport& report) { usage_reports.push_back(report); };
        // 工具审批:协议位留着(item/started + permission/request 反向请求),
        // 执行链等 Broker;骨架期没有工具可调(注册表空/假工具免确认),
        // 这里返回 false = 拒绝一切需确认工具,不许偷偷执行(单子底线)。
        callbacks.on_tool_confirm = [](const std::string&, const nlohmann::json&) { return false; };

        const std::expected<agent::RunOutcome, std::string> outcome = loop.Run(text, callbacks);
        if (!text_item_id.empty()) {
            connection_->EmitEvent(
                kEventItemCompleted,
                MakeItemCompletedParams(thread_id, turn_id, text_item_id, nlohmann::json::object()));
        }
        if (!thinking_item_id.empty()) {
            connection_->EmitEvent(
                kEventItemCompleted,
                MakeItemCompletedParams(thread_id, turn_id, thinking_item_id, nlohmann::json::object()));
        }

        std::string status;
        std::string error_message;
        if (!outcome.has_value()) {
            status = std::string(kTurnStatusError);
            error_message = outcome.error();
        } else if (outcome->cancelled) {
            status = std::string(kTurnStatusCancelled);
        } else {
            status = std::string(kTurnStatusSuccess);
        }
        const int steps_used = outcome.has_value() ? outcome->steps_used : 0;
        completed_params = MakeTurnCompletedParams(thread_id, turn_id, status, error_message,
                                                   UsageToJson(SumUsage(usage_reports)), steps_used);

        // 存档:助手回合整段落一条(事件流是协议的,存档是会话账的)。
        if (record->store != nullptr && outcome.has_value()) {
            for (const api::Message& message : loop.history()) {
                // 历史里首条 user(本轮输入)已写过;只补 assistant 的尾巴。
                if (message.role == api::Role::Assistant) {
                    record->store->AppendMessage(message);
                }
            }
        }
    }

    connection_->EmitEvent(kEventTurnCompleted, completed_params);
    record->turn_running.store(false);
    return completed_params;
}

int Server::Run() {
    connection_ = std::make_unique<StdioConnection>(
        dispatcher_,
        [](const std::string& line) {
            if (!WriteProtocolLine(line)) {
                Diagnose("stdout 写失败(断管),收线");
            }
        },
        []() { return ReadStdinChunk(); }, options_.outbox_capacity);
    return connection_->Run();
}

// 单测直驱用的连接装配:HandleTurnStart 要经 connection_ 发事件,测试里
// 先用假读写器把连接立起来,再直调 handler。
void Server::AttachForTest(std::unique_ptr<StdioConnection> connection) {
    connection_ = std::move(connection);
}

}  // namespace lubancode::app_server
