// ChannelWorkPump 实现(QQ 接入单 Q2)。装配合同见头文件。
#include "runtime/channel_work_pump.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/turn_ingress.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/index.hpp"

namespace lubancode::runtime {

namespace {

std::int64_t DefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// source_ref 定式 "ingress:<channel>:<account>:<sid>" 的拆解(结算反查)。
bool ParseSourceRef(const std::string& source_ref, std::string* channel_id,
                    std::string* account_id, std::int64_t* sid) {
    std::istringstream stream(source_ref);
    std::string prefix;
    if (!std::getline(stream, prefix, ':') || prefix != "ingress") {
        return false;
    }
    if (!std::getline(stream, *channel_id, ':') || channel_id->empty()) {
        return false;
    }
    if (!std::getline(stream, *account_id, ':') || account_id->empty()) {
        return false;
    }
    std::string sid_text;
    if (!std::getline(stream, sid_text) || sid_text.empty()) {
        return false;
    }
    try {
        *sid = std::stoll(sid_text);
    } catch (const std::exception&) {
        return false;
    }
    return *sid > 0;
}

// TurnIngress 的首枚文本块(渠道正文投影走 MakeChannelTurnIngress 同一
// 份冻结投影:媒体给占位说明,不发明第二套)。
std::string PromptFromIngress(const TurnIngress& ingress) {
    for (const auto& block : ingress.message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            return text->text;
        }
    }
    return std::string();
}

}  // namespace

std::string ChannelWorkPump::MakeChannelOperationId(const std::string& channel_id,
                                                    const std::string& account_id,
                                                    std::int64_t ingress_sid) {
    // 渠道域 + 账号 + ingress 身份(§六第五项)。同信重发(spool 重投 →
    // ingress 去重命中原 sid)同键;异正文撞同键只可能来自账损坏,交
    // SessionService 的 operation_conflict 如实报。
    return "chan:" + channel_id + ":" + account_id + ":in:" + std::to_string(ingress_sid);
}

ChannelWorkPump::ChannelWorkPump() = default;

ChannelWorkPump::~ChannelWorkPump() = default;

ChannelWorkPump::OpenResult ChannelWorkPump::Open(ChannelWorkPump* out, api::Backend& backend,
                                                  tools::ToolRegistry& registry,
                                                  Options options) {
    OpenResult result;
    if (out == nullptr) {
        result.error = "channel work pump 装配:out 为空";
        return result;
    }
    if (options.manager == nullptr) {
        result.error = "channel work pump 装配:manager 缺";
        return result;
    }
    if (options.outbox == nullptr) {
        result.error = "channel work pump 装配:outbox 缺(共享 automation 泵的单写者账)";
        return result;
    }
    if (!options.now_ms) {
        options.now_ms = [] { return DefaultNowMs(); };
    }
    out->backend_ = &backend;
    out->registry_ = &registry;
    out->options_ = std::move(options);
    out->retry_at_.clear();
    out->books_.clear();
    out->suppress_delivery_ = false;

    HeadlessExecutor::Options executor_options;
    executor_options.workspaces_root = out->options_.workspaces_root;
    executor_options.workspace_root = out->options_.workspace_identity.identity_root;
    executor_options.cwd_utf8 = out->options_.cwd_utf8;
    executor_options.lubancode_version = out->options_.lubancode_version;
    executor_options.wire_name = out->options_.wire_name;
    executor_options.model = out->options_.model;
    // 回复原件落 outbox 账下的 replies/(选择原件与段原件同一目录)。
    executor_options.replies_dir = out->options_.outbox->replies_dir();
    executor_options.tools = out->options_.tools;
    executor_options.hook_dispatcher = out->options_.hook_dispatcher;
    executor_options.max_steps_per_turn = out->options_.max_steps_per_turn;
    executor_options.max_wall_secs = out->options_.max_wall_secs;
    executor_options.max_total_tokens = out->options_.max_total_tokens;
    executor_options.fault_injection = out->options_.fault_injection;
    executor_options.max_live_channel_sessions = out->options_.max_live_channel_sessions;
    // 映射记账(§六第四项):开场(含 resume-as-new)后落 sessions.jsonl。
    ChannelWorkPump* pump = out;
    const std::string workspace_key = out->options_.workspace_identity.workspace_key;
    executor_options.on_session_mapped =
        [pump, workspace_key](const std::string& session_key, const std::string& session_id) {
            // session_key 自带账号域("channel:<ch>:<acct>:..."),账随键走。
            AccountBooks* books = pump->BooksForSessionKey(session_key);
            if (books == nullptr) {
                return;
            }
            (void)books->session_map.Map(session_key, workspace_key, session_id,
                                         pump->options_.now_ms());
        };
    out->executor_.emplace(backend, registry, std::move(executor_options));
    result.ok = true;
    return result;
}

std::string ChannelWorkPump::session_id_for(const std::string& channel_id,
                                            const std::string& account_id,
                                            const std::string& conversation_id) const {
    channel::ChannelConversation conversation;
    conversation.kind = channel::ConversationKind::Direct;
    conversation.id = conversation_id;
    const std::string session_key = channel::MakeChannelSessionKey(
        channel_id, account_id, conversation, /*sender_id=*/std::string(),
        channel::GroupSessionScope::Group);
    AccountBooks* books = const_cast<ChannelWorkPump*>(this)->BooksFor(channel_id, account_id);
    if (books == nullptr) {
        return std::string();
    }
    const auto found =
        books->session_map.Find(session_key, options_.workspace_identity.workspace_key);
    return found.has_value() ? *found : std::string();
}

ChannelWorkPump::AccountBooks* ChannelWorkPump::BooksFor(const std::string& channel_id,
                                                         const std::string& account_id) {
    const std::lock_guard<std::mutex> lock(books_mutex_);
    const std::string key = channel_id + "/" + account_id;
    const auto found = books_.find(key);
    if (found != books_.end()) {
        return found->second.get();
    }
    auto books = std::make_unique<AccountBooks>();
    const std::filesystem::path account_dir = options_.channels_state_root / channel_id / account_id;
    (void)channel::ChannelSessionMap::Open(&books->session_map, account_dir / "sessions.jsonl");
    (void)channel::ChannelWorkLedger::Open(&books->work_ledger, account_dir / "work.jsonl");
    // 开不了账(broken)也入册:记账调用如实失败,恢复裁决按绑定缺失
    // needs_review——不静默丢账,也不拦整只泵。
    AccountBooks* raw = books.get();
    books_.emplace(key, std::move(books));
    return raw;
}

// session_key = "channel:<ch>:<acct>:..." 的账号段拆解(映射记账回调用)。
ChannelWorkPump::AccountBooks* ChannelWorkPump::BooksForSessionKey(
    const std::string& session_key) {
    std::istringstream stream(session_key);
    std::string prefix;
    std::string channel_id;
    std::string account_id;
    if (!std::getline(stream, prefix, ':') || prefix != "channel") {
        return nullptr;
    }
    if (!std::getline(stream, channel_id, ':') || channel_id.empty()) {
        return nullptr;
    }
    if (!std::getline(stream, account_id, ':') || account_id.empty()) {
        return nullptr;
    }
    return BooksFor(channel_id, account_id);
}

bool ChannelWorkPump::TickOnce(std::int64_t now_ms) {
    if (closed_.load()) {
        return false;
    }
    // 1) 桥泵(字节往返 + send 超时裁决;与装配层 PumpAll 重叠无害——
    //    Drain 空、Flush 空)。
    for (const auto& snapshot : options_.manager->Snapshots()) {
        options_.manager->Pump(snapshot.channel_id, snapshot.account_id);
    }
    // 2) 回执结算 → outbox 推进 → 源结算。
    if (!ApplyDeliveryOutcomes(now_ms)) {
        return false;
    }
    ReconcileDeliveredSources(now_ms);
    // 3) 恢复扫描(Running 件的跨账裁决;不盲重跑)。
    if (!SweepRecovery(now_ms)) {
        return false;
    }
    // 4) 至多一轮新执行(公平:与 automation 泵各一;账号间轮转)。
    if (accepting_.load()) {
        if (!RunOneChannelTurn(now_ms)) {
            return false;
        }
    }
    // 5) 渠道投递驱动(新入箱的段同 tick 出发;故障注入窗 3 例外)。
    if (suppress_delivery_) {
        suppress_delivery_ = false;  // 本 tick 死在入箱后发送前:恢复路接管
    } else if (!DriveChannelDeliveries(now_ms)) {
        return false;
    }
    if (options_.outbox->broken()) {
        return false;  // 投递账 broken:停泵(写盘失败停止推进)
    }
    return true;
}

void ChannelWorkPump::StopAccepting() {
    accepting_.store(false);  // 不再取新活;在飞投递/恢复照走
}

bool ChannelWorkPump::Close(int /*grace_ms*/) {
    // 同步泵:无在飞 turn(TickOnce 已收口);渠道活场封口,writer 析构关。
    closed_.store(true);
    if (executor_.has_value()) {
        executor_->CloseChannelSessions("gateway_channel_shutdown");
    }
    return true;
}

bool ChannelWorkPump::ApplyDeliveryOutcomes(std::int64_t now_ms) {
    for (const auto& snapshot : options_.manager->Snapshots()) {
        for (const auto& outcome : options_.manager->DrainChannelDeliveryOutcomes(
                 snapshot.channel_id, snapshot.account_id)) {
            const auto item = options_.outbox->Find(outcome.client_delivery_id);
            if (!item.has_value()) {
                continue;  // 账上没有(理论不可达):丢弃留诊断
            }
            switch (outcome.status) {
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Accepted:
                    // QQ 已接受(provider_message_id 记账;幂等)。
                    (void)options_.outbox->MarkSent(outcome.client_delivery_id,
                                                    outcome.provider_message_id, now_ms);
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::RateLimited:
                    // 限频:退避后同载荷重试(同 delivery_id → 同 msg_seq);
                    // 重试帽尽 → failed。
                    if (item->attempts >= static_cast<std::int64_t>(options_.max_send_attempts)) {
                        (void)options_.outbox->MarkChannelFailed(outcome.client_delivery_id,
                                                                "rate_limited", now_ms);
                    } else {
                        retry_at_[outcome.client_delivery_id] =
                            now_ms + options_.send_retry_backoff_ms;
                    }
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Rejected:
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::AuthFailed:
                    // 平台明确拒绝/令牌失效:终态失败,不自动重试,不重跑 Agent。
                    (void)options_.outbox->MarkChannelFailed(
                        outcome.client_delivery_id,
                        outcome.error_code.empty() ? "platform_reject" : outcome.error_code,
                        now_ms);
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Unknown:
                    // 超时 = delivery_unknown(§七:停自动重发,不虚 exactly-once)。
                    (void)options_.outbox->MarkOutcomeUnknown(outcome.client_delivery_id, now_ms);
                    break;
            }
        }
    }
    return !options_.outbox->broken();
}

std::optional<bool> ChannelWorkPump::SourceDeliveryVerdict(const std::string& source_ref) const {
    // 源的全部段:全 sent → true;有终态失败/未知 → false;未齐 → nullopt。
    bool any = false;
    bool all_sent = true;
    for (const auto& item : options_.outbox->ListItems()) {
        if (item.source_ref != source_ref) {
            continue;
        }
        any = true;
        if (item.state == "sent" || item.state == "delivered") {
            continue;
        }
        if (item.state == "failed" || item.state == "delivery_unknown" || item.state == "flagged") {
            all_sent = false;
        } else {
            return std::nullopt;  // 还有段在途/待发
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return all_sent;
}

void ChannelWorkPump::ReconcileDeliveredSources(std::int64_t /*now_ms*/) {
    // 终态项的源结算(幂等:已结算的 Transition 报错忽略)。崩溃窗口
    // "投递终态已落、ingress 未结算"在这里补;发送失败不把已成功的业务
    // 执行改成执行失败(ingress 走 DeliveryFailed 旁路,Replied 事实保留)。
    std::set<std::string> sources;
    for (const auto& item : options_.outbox->ListItems()) {
        if (item.delivery_target == "local:file" || item.source_ref.empty()) {
            continue;
        }
        const auto verdict = SourceDeliveryVerdict(item.source_ref);
        if (!verdict.has_value()) {
            continue;
        }
        sources.insert(item.source_ref);
        std::string channel_id;
        std::string account_id;
        std::int64_t sid = 0;
        if (!ParseSourceRef(item.source_ref, &channel_id, &account_id, &sid)) {
            continue;
        }
        const std::string reason =
            *verdict ? std::string("delivered") : std::string("delivery_failed");
        (void)options_.manager->SettleIngressDelivered(channel_id, account_id, sid, *verdict,
                                                       reason);
    }
}

bool ChannelWorkPump::SweepRecovery(std::int64_t now_ms) {
    for (const auto& snapshot : options_.manager->Snapshots()) {
        for (const auto& view : options_.manager->ListRunningIngress(snapshot.channel_id,
                                                                    snapshot.account_id)) {
            if (in_flight_sids_.count(snapshot.channel_id + "/" + snapshot.account_id + "/" +
                                      std::to_string(view.sid)) > 0) {
                continue;  // 本进程在跑的轮(多线程泵的门)
            }
            if (!RecoverOne(snapshot.channel_id, snapshot.account_id, view, now_ms)) {
                // 账写不进(ingress/dead letter 落不了盘):停泵。
                return false;
            }
        }
    }
    return true;
}

bool ChannelWorkPump::RecoverOne(const std::string& channel_id, const std::string& account_id,
                                 const channel::ChannelManager::IngressRunningView& view,
                                 std::int64_t now_ms) {
    AccountBooks* books = BooksFor(channel_id, account_id);
    const auto dead_letter = [this, &channel_id, &account_id,
                              sid = view.sid](const std::string& reason) {
        return options_.manager->DeadLetterIngress(channel_id, account_id, sid, reason);
    };
    const auto bound = books->work_ledger.FindBound(view.sid);
    if (!bound.has_value() || bound->session_id.empty() || bound->turn_id.empty()) {
        // claim 后崩(绑定行没落):needs_review,不猜"没开过场"。
        return !dead_letter("needs_review:claimed_without_binding").has_value();
    }
    // resolver:workspace 房门按 key 反查,不拼目录名(§11.4)。
    const auto room = workspace::index::ResolveDirByWorkspaceKey(
        options_.workspaces_root, options_.workspace_identity.workspace_key);
    if (!room.has_value()) {
        return !dead_letter("needs_review:workspace_room_unresolved").has_value();
    }
    const auto stream =
        trajectory::v3::FindV3SessionStream(*room / "sessions" / bound->session_id);
    if (!stream.has_value()) {
        return !dead_letter("needs_review:v3_stream_not_found").has_value();
    }
    const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
    if (!ledger.has_value()) {
        return !dead_letter("needs_review:v3_stream_unreadable: " + ledger.error()).has_value();
    }
    // 冻结策略重算同一 selectionId(纯函数,不调模型)。
    const ReplySelectionPlan plan = PlanReplySelection(*ledger, bound->turn_id);
    if (!plan.ok) {
        // 无 assistant:生成没完成(claim 后崩在执行中)。不盲重跑。
        return !dead_letter("needs_review:generation_incomplete: " + plan.error).has_value();
    }
    const std::string target_str =
        gateway::MakeChannelDeliveryTarget(channel_id, account_id, view.event.conversation.id);
    if (!SelectionAlreadyCommitted(*ledger, plan.selection_id)) {
        // 窗口:生成结束、selection 未提交——补齐(续原卷写事实,不开新轮)。
        auto writer = trajectory::v3::V3Writer::Continue(*stream);
        if (!writer.has_value()) {
            return !dead_letter("needs_review:v3_writer_continue_failed: " + writer.error())
                        .has_value();
        }
        const auto commit = CommitReplySelection(&*writer, options_.outbox->replies_dir(), plan,
                                                 bound->session_id, target_str);
        if (!commit.committed) {
            return !dead_letter("needs_review:selection_commit_failed: " + commit.error)
                        .has_value();
        }
    }
    // selection 已在(或刚补齐):补 outbox 投影(同 deliveryId 幂等)。
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = channel_id;
    target.account_id = account_id;
    target.conversation_id = view.event.conversation.id;
    target.reply_to_message_id = view.event.message_id;
    target.source_ref =
        "ingress:" + channel_id + ":" + account_id + ":" + std::to_string(view.sid);
    const auto enqueued = options_.outbox->EnqueueChannel(
        plan.selection_id, plan.text, bound->session_id, bound->turn_id, target, now_ms);
    if (!enqueued.accepted && !enqueued.duplicate) {
        return false;  // 入箱失败(账 broken):停泵——执行事实保留,不冒充成功
    }
    // 执行侧结算:Running → Replied(投递态另算)。已在 Replied(幂等重扫)
    // 报"非法迁移",按已结算放过;写盘失败停泵。
    const auto replied = options_.manager->SettleIngressReplied(channel_id, account_id, view.sid);
    if (replied.has_value() && replied->find("非法迁移") == std::string::npos) {
        return false;
    }
    return true;
}

bool ChannelWorkPump::RunOneChannelTurn(std::int64_t now_ms) {
    // 并发帽先查(取件前;claim 了不跑会把件搁死在 Running)。
    if (options_.max_active_channel_turns > 0 &&
        active_turns_ >= options_.max_active_channel_turns) {
        return true;
    }
    const std::vector<channel::ChannelManager::AccountSnapshot> snapshots =
        options_.manager->Snapshots();
    if (snapshots.empty()) {
        return true;
    }
    // 账号间轮转公平(FairnessCounter 是 goal/loop 的账,渠道族不掺;
    // 与 automation 的公平靠 CompositeGatewayPump 各 tick 一轮)。
    for (std::size_t step = 0; step < snapshots.size(); ++step) {
        const std::size_t index = (last_account_index_ + step) % snapshots.size();
        const auto& snapshot = snapshots[index];
        if (!options_.manager->HasPendingWork(snapshot.channel_id, snapshot.account_id)) {
            continue;
        }
        auto work = options_.manager->TakeNextWork(snapshot.channel_id, snapshot.account_id);
        if (!work.has_value()) {
            continue;
        }
        last_account_index_ = (index + 1) % snapshots.size();
        const std::string in_flight_key = snapshot.channel_id + "/" + snapshot.account_id + "/" +
                                          std::to_string(work->sid);
        in_flight_sids_.insert(in_flight_key);
        ++active_turns_;
        const bool ok = ProcessWorkItem(snapshot.channel_id, snapshot.account_id, *work, now_ms);
        --active_turns_;
        in_flight_sids_.erase(in_flight_key);
        return ok;
    }
    return true;
}

bool ChannelWorkPump::ProcessWorkItem(const std::string& channel_id,
                                      const std::string& account_id,
                                      const channel::ChannelManager::WorkItem& work,
                                      std::int64_t now_ms) {
    AccountBooks* books = BooksFor(channel_id, account_id);
    // 正文投影:渠道事件的冻结投影(媒体占位说明,同一份 MakeChannelTurnIngress)。
    const TurnIngress ingress = MakeChannelTurnIngress(
        work.event, work.route.provenance, work.route.session_key,
        work.route.memory.user_memory || work.route.memory.project_memory,
        &work.route.tools);

    HeadlessExecutor::ChannelTurnRequest request;
    request.session_key = work.route.session_key;
    const auto stored = books->session_map.Find(work.route.session_key,
                                                options_.workspace_identity.workspace_key);
    if (stored.has_value()) {
        request.stored_session_id = *stored;
    }
    request.prompt = PromptFromIngress(ingress);
    request.binding.work_id = MakeChannelOperationId(channel_id, account_id, work.sid);
    request.binding.source_kind = "channel";
    request.binding.source_id = channel_id + ":" + account_id;
    request.binding.owner_epoch = owner_epoch_;
    request.binding.attempt = 1;
    request.per_turn_tools = &work.route.tools;
    request.delivery_target =
        gateway::MakeChannelDeliveryTarget(channel_id, account_id, work.conversation_id);
    request.binding_extra = nlohmann::json::object({
        {"channelId", channel_id},
        {"accountId", account_id},
        {"ingressSid", work.sid},
        {"sessionKey", work.route.session_key},
        {"conversationId", work.conversation_id},
        {"senderId", work.sender_id},
        {"provenance", work.route.provenance.ToJson()},
    });

    const std::int64_t sid = work.sid;
    const std::string bound_session_key = work.route.session_key;
    std::atomic<bool> cancel_flag{false};
    const auto result = executor_->ExecuteChannelTurn(
        request,
        [this, books, sid, bound_session_key, now_ms](const std::string& session_id,
                                                      const std::string& turn_id) {
            // 领域绑定先于 V3 work.bound(恢复器优先走领域行定位原场)。
            (void)books->work_ledger.Bind(sid, bound_session_key, session_id, turn_id, now_ms);
        },
        &cancel_flag);
    if (!result.ok) {
        if (result.error_code == "gateway.fault_injected") {
            return true;  // 不结算:恢复路接管(模拟进程死在半路)
        }
        // 执行失败(模型错/取消/受理拒):如实退场,不盲重跑。
        (void)options_.manager->DeadLetterIngress(channel_id, account_id, sid,
                                                  "turn_failed: " + result.error_code + ": " +
                                                      result.error);
        return true;
    }
    // reply selection 已提交 → outbox 投影(拆段入箱,deliveryId 定式幂等)。
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = channel_id;
    target.account_id = account_id;
    target.conversation_id = work.conversation_id;
    target.reply_to_message_id = work.event.message_id;
    target.source_ref = "ingress:" + channel_id + ":" + account_id + ":" + std::to_string(sid);
    const auto enqueued = options_.outbox->EnqueueChannel(
        result.selection_id, result.reply_text, result.session_id, result.turn_id, target,
        now_ms);
    // 执行侧结算:Running → Replied(投递态另算;发送失败不把执行改失败)。
    (void)options_.manager->SettleIngressReplied(channel_id, account_id, sid);
    if (!enqueued.accepted && !enqueued.duplicate) {
        return false;  // outbox 账写不进:停泵
    }
    // 故障注入窗 3:入 outbox 后、发送前——恢复器应从已入箱项续投,不重跑。
    if (options_.fault_after_enqueue) {
        if (const std::string fault = options_.fault_after_enqueue(); !fault.empty()) {
            suppress_delivery_ = true;
            return true;
        }
    }
    return true;
}

bool ChannelWorkPump::DriveChannelDeliveries(std::int64_t now_ms) {
    // 段序:同 selection 按 ordinal 依次,每 tick 每组只驱动最靠前的未终
    // 态段(前段未 sent 不发后段);源里有段终态失败 → 后段就地失败收档。
    std::vector<gateway::ReplyOutboxItem> items = options_.outbox->PendingChannelItems();
    std::sort(items.begin(), items.end(), [](const gateway::ReplyOutboxItem& a,
                                             const gateway::ReplyOutboxItem& b) {
        if (a.selection_id != b.selection_id) {
            return a.selection_id < b.selection_id;
        }
        return a.ordinal < b.ordinal;
    });
    std::set<std::string> touched_selections;  // 本 tick 已驱动/已跳过的组
    for (const auto& item : items) {
        if (touched_selections.count(item.selection_id) > 0) {
            continue;  // 前面的段还没 sent:后段等
        }
        touched_selections.insert(item.selection_id);
        // 源级失败传染:有段终态失败/未知 → 余下待发段就地失败收档。
        const auto verdict = SourceDeliveryVerdict(item.source_ref);
        if (verdict.has_value() && !*verdict) {
            if (item.state == "pending") {
                (void)options_.outbox->MarkChannelFailed(item.delivery_id,
                                                         "skipped_after_segment_failure", now_ms);
            }
            continue;
        }
        // 节流:限频退避未到不发。
        const auto throttled = retry_at_.find(item.delivery_id);
        if (throttled != retry_at_.end() && now_ms < throttled->second) {
            continue;
        }
        // 在途裁决:账上 sending 而桥上无在途 send = 发出请求丢了(崩窗/
        // 回执丢)→ 重驱动(同 delivery_id → 同 msg_seq,平台去重兜底);
        // 有在途 → 等回执或超时裁决。
        if (item.state == "sending" &&
            options_.manager->HasPendingSend(item.target_channel_id, item.target_account_id,
                                             item.delivery_id)) {
            continue;
        }
        // 重试帽:attempts 由 item.attempt 行推进,帽尽即终态失败。
        if (item.attempts >= static_cast<std::int64_t>(options_.max_send_attempts)) {
            (void)options_.outbox->MarkChannelFailed(item.delivery_id, "rate_limited", now_ms);
            continue;
        }
        std::string text;
        if (!options_.outbox->LoadChannelItemText(item.delivery_id, &text)) {
            (void)options_.outbox->MarkChannelFailed(item.delivery_id, "artifact_missing",
                                                     now_ms);
            continue;
        }
        // 发出前记尝试(§七:先账后网络)。
        if (!options_.outbox->RecordAttempt(item.delivery_id, now_ms)) {
            continue;
        }
        channel::ChannelManager::ChannelSendRequest send;
        send.conversation_id = item.target_conversation_id;
        send.text = text;
        send.reply_to_message_id = item.target_reply_to_message_id;
        send.client_delivery_id = item.delivery_id;
        const auto error = options_.manager->SendReply(item.target_channel_id,
                                                       item.target_account_id, send);
        if (error.has_value()) {
            // 受理失败(账号不在 Running 等):退避后再驱动,attempts 照涨。
            retry_at_[item.delivery_id] = now_ms + options_.send_retry_backoff_ms;
        }
    }
    return !options_.outbox->broken();
}

}  // namespace lubancode::runtime
