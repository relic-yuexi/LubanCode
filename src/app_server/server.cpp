// server.hpp 的实现:装配方法表、thread 账、turn/start 的整回合驱动、
// 审批/ask_user 反向请求与 turn/interrupt 的接线(阶段 2)。
//
// 线程模型(阶段 2 起):
//   - 读线程(Run 所在线程):逐行读入站,请求法子大部分在读线程跑;
//   - 回合工作线程:turn/start 受理后把整回合挪进工作线程跑(handler
//     立即回 turnId,不堵读线程——审批悬停期间事件泵照常活,前端可以
//     随时发 interrupt/答复审批);
//   - 写线程:从 outbox 逐条 PopWait、写 stdout。
// 同一 thread 同拍两轮仍协议明拒(turn_running 的 CAS)。
#include "app_server/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include "tools/ask_user.hpp"
#include "tools/registry.hpp"

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

std::shared_ptr<ThreadRecord> Server::FindThread(const std::string& thread_id) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const auto it = threads_.find(thread_id);
    return it == threads_.end() ? nullptr : it->second;
}

Server::~Server() {
    // 析构兜底:在跑的回合收口(与 Shutdown 同路),不能让 joinable 的
    // 工作线程把进程掀了(std::thread 析构时 joinable = terminate)。
    Shutdown();
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
        kMethodTurnStart, [this](const IncomingRequest& request, DispatchContext&)
                          -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::string text;
            const ParamsCheck base = CheckTurnStartParams(request.params, thread_id, text);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json accepted = AcceptTurnStart(thread_id, text, error_code);
            if (!error_code.empty()) {
                if (error_code == "already_running") {
                    return MakeError(request.id, kErrTurnAlreadyRunning,
                                     "该 thread 已有回合在跑: " + thread_id);
                }
                return MakeError(request.id, kErrInvalidParams, "turn/start 失败: " + error_code);
            }
            // 立即回 {threadId, turnId}:整回合在工作线程跑,终态走
            // turn/completed 事件(handler 不等回合结束——审批悬停期间
            // 读线程还得收前端的答复与 interrupt)。
            return MakeResult(request.id, accepted);
        });

    // turn/interrupt
    dispatcher_->RegisterMethod(
        kMethodTurnInterrupt, [this](const IncomingRequest& request, DispatchContext&)
                              -> std::optional<nlohmann::json> {
            std::string thread_id;
            std::string turn_id;
            const ParamsCheck base = CheckTurnInterruptParams(request.params, thread_id, turn_id);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::string error_code;
            const nlohmann::json result = HandleTurnInterrupt(thread_id, turn_id, error_code);
            if (!error_code.empty()) {
                if (error_code == "stale") {
                    // 迟到的打断:回合不在跑(收口了/没这回合),不追旧账。
                    return MakeError(request.id, kErrStaleRequestId,
                                     "回合不在跑,打断不受理: " + thread_id + " " + turn_id);
                }
                return MakeError(request.id, kErrInvalidParams, "turn/interrupt 失败: " + error_code);
            }
            return MakeResult(request.id, result);
        });
}

std::size_t Server::active_thread_count() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    return threads_.size();
}

nlohmann::json Server::HandleThreadStart(const nlohmann::json& params, std::string& out_error_code) {
    out_error_code.clear();
    auto record = std::make_shared<ThreadRecord>(std::string());
    record->cwd = params.value("cwd", std::string());
    if (record->cwd.empty()) {
        record->cwd = options_.cwd;
    }

    // 会话账:复用 SessionStore,不另立第二本账。首句摘要用一句占位的
    // 协议话——thread/start 阶段还没有用户文本(单子的存档恢复线会把
    // 真首句补进来);MakeSessionId 的 slug 拿它生成文件名。
    record->thread_id = agent::MakeSessionId(agent::NowIdTimestamp(), "app-server-thread");
    record->interactions = std::make_unique<InteractionLedger>(record->thread_id);
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
        // 在跑回合按打断收口:置旗、悬起件全清(审批悬停立即醒,按
        // "thread 关闭"的悬空收口处理),等回合工作线程收尾(硬时限内),
        // 等不到就分离——store 的柄不能在它还写着时收。
        record->interrupt_requested.store(true);
        record->interactions->CancelPending();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options_.interrupt_hard_deadline_ms);
        while (!record->turn_finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (record->turn_finished.load() && record->turn_worker.joinable()) {
            record->turn_worker.join();
        } else if (record->turn_worker.joinable()) {
            Diagnose("thread 停场时回合未在硬时限内收口,分离工作线程: " + thread_id);
            record->turn_worker.detach();
        }
    }
    if (record->store != nullptr) {
        record->store->Reset(); // 文件留在磁盘上,只是句柄收掉
    }
    Diagnose("thread 已停: " + thread_id);
    return MakeThreadStoppedResult();
}

nlohmann::json Server::AcceptTurnStart(const std::string& thread_id, const std::string& text,
                                       std::string& out_error_code) {
    out_error_code.clear();

    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr) {
        out_error_code = "不认识的 threadId: " + thread_id;
        return nlohmann::json();
    }

    // 同一 thread 同拍两轮:协议明拒(单子验收:规矩写死并有测试)。
    bool expected = false;
    if (!record->turn_running.compare_exchange_strong(expected, true)) {
        out_error_code = "already_running";
        return nlohmann::json();
    }

    const std::string turn_id = "turn-" + std::to_string(++turn_counter_);
    record->turn_id = turn_id;
    record->interrupted_turn.clear();
    record->interrupt_requested.store(false);
    record->turn_finished.store(false);
    if (record->turn_worker.joinable()) {
        record->turn_worker.join(); // 上一轮的尾巴(正常已收,防御)
    }
    record->turn_worker = std::thread([this, record, thread_id, turn_id, text] {
        RunTurnToCompletion(record, thread_id, turn_id, text);
    });
    return nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}};
}

nlohmann::json Server::HandleTurnStart(const std::string& thread_id, const std::string& text,
                                       std::string& out_error_code) {
    // 兼容直驱(单测与阶段 1 的口径):受理 + 等工作线程收尾,返回
    // turn/completed 的 params。协议路径(读线程)只调 AcceptTurnStart,
    // 立即回 turnId——这里给同步消费方留一条等完的路。
    const nlohmann::json accepted = AcceptTurnStart(thread_id, text, out_error_code);
    if (!out_error_code.empty()) {
        return nlohmann::json();
    }
    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr || !record->turn_worker.joinable()) {
        out_error_code = "回合工作线程没能立起";
        return nlohmann::json();
    }
    // interrupt 的硬时限:置旗后 loop.Run 最多在流式/工具边界处收口;真有
    // 卡死不退的(长命令/卡死的外部进程),等满 interrupt_hard_deadline_ms
    // 就不再等——回合标记收口失败,join 分离(detach),读线程不被拖死。
    // 这里是同步口径的等法;协议路径的兜底在连接收线(断管/exit)时由
    // 析构路径兜。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.interrupt_hard_deadline_ms);
    while (!record->turn_finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (record->turn_finished.load()) {
        record->turn_worker.join();
        return record->last_completed;
    }
    // 硬时限已到回合还没收口:不 join(卡死工作线程不该拖死调用方),
    // detach 让它自生自灭,终态事件由它(假如还活着)自己补发。
    Diagnose("回合未在硬时限内收口,分离工作线程: " + thread_id + " " + record->turn_id);
    record->turn_worker.detach();
    record->turn_running.store(false);
    out_error_code = "hard_deadline";
    return nlohmann::json();
}

// 整回合驱动:在工作线程跑。审批/ask_user 悬停期间读线程照常收前端的
// 答复(HandleInteractionResponse)与打断(HandleTurnInterrupt);悬停的
// future 靠 promise 被读线程唤醒,事件泵不堵。
void Server::RunTurnToCompletion(const std::shared_ptr<ThreadRecord>& record, const std::string& thread_id,
                                 const std::string& turn_id, const std::string& text) {
    connection_->EmitEvent(kEventTurnStarted, MakeTurnStartedParams(thread_id, turn_id));
    if (record->store != nullptr) {
        api::Message user_message;
        user_message.role = api::Role::User;
        user_message.content.push_back(api::TextBlock{text});
        record->store->AppendMessage(user_message);
    }

    // 事件账:条目 id 单调(见下方 next_item_seq);TODO(seq)回合内
    // 计数凑合,runtime P4 的统一 seq 分配器落地后改挂 thread 级序号。
    nlohmann::json completed_params;
    {
        // 装配:假 backend + 注册表(骨架期工具链由测试注入假工具;生产
        // 装配走 cli_app 的 registry_factory)。
        std::unique_ptr<api::Backend> backend = backend_factory_();
        std::unique_ptr<tools::ToolRegistry> registry =
            registry_factory_ ? registry_factory_() : std::make_unique<tools::ToolRegistry>();

        agent::AgentRuntimeProfile profile;
        profile.max_steps_per_turn = 32; // 骨架期防跑飞;真配置线接进来再换
        agent::AgentLoop loop(*backend, *registry, std::move(profile), std::string("lubancode app-server"));

        agent::Callbacks callbacks;
        // 条目 id:回合内单调递增。
        std::uint64_t next_item_seq = 0;
        std::string text_item_id;
        callbacks.on_text_delta = [&](const std::string& delta) {
            if (text_item_id.empty()) {
                text_item_id = "item-" + std::to_string(next_item_seq++);
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
                thinking_item_id = "item-" + std::to_string(next_item_seq++);
                connection_->EmitEvent(kEventItemStarted,
                                       MakeItemStartedParams(thread_id, turn_id, thinking_item_id,
                                                             kItemTypeThinking, nlohmann::json::object()));
            }
            connection_->EmitEvent(kEventItemDelta,
                                   MakeItemDeltaParams(thread_id, turn_id, thinking_item_id, delta));
        };
        std::vector<api::UsageReport> usage_reports;
        callbacks.on_usage = [&](const api::UsageReport& report) { usage_reports.push_back(report); };

        // ---- 审批接线(阶段 2 核心) ----
        // needs_confirm 的工具:先查会话级放行账(acceptForSession 记过
        // 的免问),没放行就发 permission/request 反向请求,悬停等前端
        // 答复。悬停期间:
        //   - 读线程 HandleInteractionResponse 把答复 resolve 进 promise;
        //   - turn/interrupt 置旗 + CancelPending,future 按 cancel 醒;
        //   - 超时(选项给了时限)按"没人可答"悬空收口,不冒充用户拒绝。
        callbacks.on_tool_confirm_async =
            [this, record, turn_id](const agent::ApprovalRequest& request)
            -> std::shared_ptr<agent::InteractionFuture> {
                // 会话级放行:免问直接放。
                if (record->interactions->IsSessionAllowed(request.tool_name)) {
                    agent::ApprovalResponse accepted;
                    accepted.decision = agent::ApprovalDecision::Accept;
                    return std::make_shared<ReadyApprovalFuture>(std::move(accepted));
                }
                const runtime::ApprovalRequest mirrored = MirrorApprovalRequest(request);
                auto future = std::static_pointer_cast<PendingFuture>(record->interactions->AskApproval(
                    mirrored, turn_id,
                    [this](std::string_view method, const nlohmann::json& params) {
                        // must_keep:审批丢了客户端不知道要答。
                        connection_->outbox().Push(SerializeMessage(MakeEvent(method, params)),
                                                   EventMustKeep(method));
                    }));
                if (options_.approval_timeout_ms > 0) {
                    // 限时悬停:超时按悬空收口,悬停不偷跑。
                    future->SetTimeout(std::chrono::milliseconds(options_.approval_timeout_ms));
                }
                // 打断旗轮询贴在 future 上:CancelPending 收 promise 唤醒
                // Wait;旗先置位而 promise 未收(窗口极窄)由 future 的
                // 轮询兜底。
                future->WatchInterrupt(&record->interrupt_requested);
                return future;
            };
        // 悬空收口的拒绝文案:写明真因,不冒充用户拒绝(P2 工人接线
        // 注意 3)。interrupt 旗置位 = 打断;否则是超时/断线。
        callbacks.on_tool_denial_text = [&record](const std::string& name) {
            if (record->interrupt_requested.load()) {
                return "回合被 turn/interrupt 打断," + name + " 的审批按取消收口,未执行。";
            }
            return "审批悬停超时(前端未在时限内答复)," + name + " 按取消收口,未执行。";
        };
        // 同步回落路不设:app-server 一律走 async(没有终端可问)。

        // ---- ask_user 接线(user/ask 反向请求,同一套悬起机制) ----
        // 工具表的 ask_user 工具(装配时注入的)经 SetHandler 换成悬起
        // 版:每题发一枚 user/ask,等前端 answers。
        if (tools::Tool* raw_ask = registry->Find("ask_user"); raw_ask != nullptr) {
            if (auto* ask_tool = dynamic_cast<tools::AskUserTool*>(raw_ask); ask_tool != nullptr) {
                ask_tool->SetHandler([this, record, turn_id](const tools::AskUserQuestion& question)
                                         -> std::expected<std::vector<std::string>, std::string> {
                    runtime::QuestionRequest request;
                    request.header = question.header;
                    request.question = question.question;
                    request.multi_select = question.multi_select;
                    for (const tools::AskUserOption& option : question.options) {
                        request.options.push_back(runtime::QuestionOption{option.label, option.description});
                    }
                    auto future = record->interactions->AskQuestion(
                        request, turn_id, /*question_index=*/0,
                        [this](std::string_view method, const nlohmann::json& params) {
                            connection_->outbox().Push(SerializeMessage(MakeEvent(method, params)),
                                                       EventMustKeep(method));
                        });
                    if (options_.approval_timeout_ms > 0) {
                        future->SetTimeout(std::chrono::milliseconds(options_.approval_timeout_ms));
                    }
                    future->WatchInterrupt(&record->interrupt_requested);
                    const std::optional<runtime::QuestionResponse> answer = future->WaitQuestion();
                    if (!answer.has_value()) {
                        // 悬空收口:打断/超时/断线。真因文案。
                        if (record->interrupt_requested.load()) {
                            return std::unexpected("回合被 turn/interrupt 打断,提问按取消收口");
                        }
                        return std::unexpected("提问悬停超时(前端未在时限内答复),按取消收口");
                    }
                    if (!answer->error.empty()) {
                        return std::unexpected(answer->error);
                    }
                    if (answer->answers.empty()) {
                        return std::unexpected("用户没有选择答案");
                    }
                    return answer->answers;
                });
            }
        }

        // ---- 跑 ----
        const std::expected<agent::RunOutcome, std::string> outcome =
            loop.Run(text, callbacks, &record->interrupt_requested);
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

        // acceptForSession 的放行记账在答复侧(HandleInteractionResponse)
        // 落,这里只收终态。
        std::string status;
        std::string error_message;
        if (!outcome.has_value()) {
            status = std::string(kTurnStatusError);
            error_message = outcome.error();
        } else if (record->interrupt_requested.load() && outcome->cancelled) {
            // 打断收场:interrupted 终态(单子口径:turn/interrupt 发
            // interrupted,ESC 语义的 cancelled 留给内部收线)。
            status = std::string(kTurnStatusInterrupted);
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

    // 回合收口:清掉这一轮残留的悬起请求(理论到不了这——审批都是同步
    // Wait 的;防御:中断路径上 future 撤了 promise 没收的,这里兜底)。
    record->interactions->CancelPending();
    connection_->EmitEvent(kEventTurnCompleted, completed_params);
    record->last_completed = completed_params;
    record->turn_finished.store(true);
    record->turn_running.store(false);
}

nlohmann::json Server::HandleTurnInterrupt(const std::string& thread_id, const std::string& turn_id,
                                           std::string& out_error_code) {
    out_error_code.clear();
    const std::shared_ptr<ThreadRecord> record = FindThread(thread_id);
    if (record == nullptr) {
        out_error_code = "不认识的 threadId: " + thread_id;
        return nlohmann::json();
    }
    if (!record->turn_running.load()) {
        out_error_code = "stale"; // 回合不在跑:迟到打断,不受理
        return nlohmann::json();
    }
    if (!turn_id.empty() && turn_id != record->turn_id) {
        out_error_code = "stale"; // 点名了别的回合:旧账不追
        return nlohmann::json();
    }
    record->interrupt_requested.store(true);
    record->interrupted_turn = record->turn_id;
    // 审批悬停立即醒:pending 全按 cancel 收口(文案由 denial_text 写
    // "被 turn/interrupt 打断")。回合驱动在流式/工具边界看旗收场,终态
    // turn/completed(status=interrupted)照发。
    record->interactions->CancelPending();
    Diagnose("turn/interrupt 受理: " + thread_id + " " + record->turn_id);
    return nlohmann::json{{"threadId", thread_id}, {"turnId", record->turn_id}, {"accepted", true}};
}

InteractionResolution Server::HandleInteractionResponse(const IncomingResponse& response) {
    // requestId -> 哪场 thread:先扫各 thread 的悬起件(骨架期 thread 数
    // 少,线性可忍;TODO(seq/runtime P4)统一 id 后按 id 前缀直路由)。
    std::vector<std::shared_ptr<ThreadRecord>> records;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (const auto& [id, record] : threads_) {
            records.push_back(record);
        }
    }
    InteractionResolution result;
    result.error_code = runtime::kStaleRequestId;
    result.error_message = "没有在飞的请求配得上这条响应";
    for (const std::shared_ptr<ThreadRecord>& record : records) {
        if (record->interactions->pending_count() == 0) {
            continue;
        }
        // 只有一场 thread 认领这条响应(其它场报 stale);认领成功即收。
        const InteractionResolution one = record->interactions->HandleIncomingResponse(response);
        if (one.ok) {
            return one;
        }
    }
    return result;
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
    connection_->SetInteractionResolver([this](const IncomingResponse& response) -> std::string {
        const InteractionResolution result = HandleInteractionResponse(response);
        if (result.ok) {
            return std::string();
        }
        return result.error_code.empty() ? std::string(runtime::kStaleRequestId) : result.error_code;
    });
    const int code = connection_->Run();
    Shutdown();
    return code;
}

void Server::Shutdown() {
    // 在跑的回合一律按打断收口:置旗、悬起全清(审批悬停立即醒),等
    // 收尾(硬时限内),等不到就分离——绝不许 join 卡死把退场路堵死。
    std::vector<std::shared_ptr<ThreadRecord>> records;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (const auto& [id, record] : threads_) {
            records.push_back(record);
        }
    }
    for (const std::shared_ptr<ThreadRecord>& record : records) {
        if (record->turn_worker.joinable()) {
            record->interrupt_requested.store(true);
            record->interactions->CancelPending();
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(options_.interrupt_hard_deadline_ms);
            while (!record->turn_finished.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (record->turn_finished.load()) {
                record->turn_worker.join();
            } else {
                Diagnose("收线时回合未在硬时限内收口,分离工作线程: " + record->thread_id);
                record->turn_worker.detach();
            }
        }
        if (record->store != nullptr) {
            record->store->Reset();
        }
    }
}

// 单测直驱用的连接装配:HandleTurnStart 要经 connection_ 发事件,测试里
// 先用假读写器把连接立起来,再直调 handler。resolver 一并接上,整线
// 直驱(Feed 响应信封)与生产同路。
void Server::AttachForTest(std::unique_ptr<StdioConnection> connection) {
    connection->SetInteractionResolver([this](const IncomingResponse& response) -> std::string {
        const InteractionResolution result = HandleInteractionResponse(response);
        if (result.ok) {
            return std::string();
        }
        return result.error_code.empty() ? std::string(runtime::kStaleRequestId) : result.error_code;
    });
    connection_ = std::move(connection);
}

}  // namespace lubancode::app_server
