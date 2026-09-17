// ChannelWorkPump 实现(QQ 接入单 Q2)。装配合同见头文件。
#include "runtime/channel_work_pump.hpp"
#include "runtime/channel_file_delivery.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/turn_ingress.hpp"
#include "channel/channel_commands.hpp"  // Q7 菜单/面板命令分派
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "runtime/channel_session_host.hpp"  // ChannelToolDenialText(Q6 拒绝文案)
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "trajectory/v3/writer.hpp"
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

// ---- P0 刀二:执行失败的用户提示(脱敏映射) ---------------------------------

TurnFailureNotice MakeTurnFailureNotice(const std::string& error_code,
                                        const std::string& error_detail) {
    // 分型只认稳定错误码与关键词;error_detail 不进文案(内部报错可能带
    // 路径/密钥,一个字都不发到 QQ)。
    const bool unestimated =
        error_detail.find("unestimated_media_or_reasoning") != std::string::npos;
    const bool capacity =
        error_detail.find("context.adapter_input_exceeds_capacity") != std::string::npos ||
        error_detail.find("上下文预检未通过") != std::string::npos;
    const bool cancelled = error_detail.find("cancelled") != std::string::npos ||
                           error_detail.find("取消") != std::string::npos;
    if (error_code == "gateway.turn_failed" && unestimated) {
        return {"E-CTX1",
                "这条消息没能处理完:对话里有按当前策略无法计入预算的内容(如图片或加密思考),"
                "本轮已拦下、没有发给模型。请发一条纯文字消息;反复出现请把编号 E-CTX1 告诉维护者。"};
    }
    if (error_code == "gateway.turn_failed" && capacity) {
        return {"E-CTX2",
                "这条消息没能处理完:对话太长,超出了模型的上下文窗口。请开一个新会话再发,或缩短内容。"};
    }
    if (cancelled) {
        return {"E-CNL1", "这一轮被中断了,消息没有处理完。请重新发送。"};
    }
    if (error_code == "gateway.turn_failed") {
        return {"E-NET1",
                "这条消息没能处理完:调用模型失败(网络或服务端错误)。请稍后重发;"
                "反复出现请把编号 E-NET1 告诉维护者。"};
    }
    if (error_code == "gateway.launch_failed" || error_code == "gateway.requires_v3") {
        return {"E-SES1",
                "这条消息没能处理:会话服务没能启动。请稍后重发;反复出现请把编号 E-SES1 告诉维护者。"};
    }
    if (error_code == "gateway.input_rejected") {
        return {"E-INT1", "这条消息没能被受理。请重新发送;反复出现请把编号 E-INT1 告诉维护者。"};
    }
    if (error_code.rfind("selection.", 0) == 0 || error_code == "gateway.reply_unavailable") {
        return {"E-RPL1",
                "回复已生成,但整理投递时出了问题。请重发一条消息;反复出现请把编号 E-RPL1 告诉维护者。"};
    }
    if (error_code.rfind("needs_review", 0) == 0 || error_detail.rfind("needs_review", 0) == 0) {
        return {"E-RVW1",
                "处理这条消息时进程中断,已挂起待人工核对,不会自动补跑。请重发一条新消息。"};
    }
    return {"E-OTH1", "这条消息没能处理完。请重发;反复出现请把编号 E-OTH1 告诉维护者。"};
}

std::string ChannelWorkPump::MakeChannelOperationId(const std::string& channel_id,
                                                    const std::string& account_id,
                                                    std::int64_t ingress_sid) {
    // 渠道域 + 账号 + ingress 身份(§六第五项)。同信重发(spool 重投 →
    // ingress 去重命中原 sid)同键;异正文撞同键只可能来自账损坏,交
    // SessionService 的 operation_conflict 如实报。
    return "chan:" + channel_id + ":" + account_id + ":in:" + std::to_string(ingress_sid);
}

std::string ChannelWorkPump::MakeChannelJobSessionKey(const std::string& channel_id,
                                                      const std::string& account_id,
                                                      const std::string& job_id) {
    // Q5:任务自己的隔离场(§11.2 "不自动带上 QQ 全部聊天史")。账号段与
    // 聊天会话同构(映射账/work ledger 随账号走),kind=job 不与
    // direct/group 档撞名。
    return "channel:" + channel_id + ":" + account_id + ":job:" + job_id;
}

ChannelWorkPump::ChannelWorkPump() = default;

ChannelWorkPump::~ChannelWorkPump() {
    // 异步 turn 线程兜底收口(Close 没跑过也不悬线程):打断在飞审批
    // 等待,join 后再拆成员。
    ShutDownTurnWorkers();
}

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
    executor_options.workspace_identity = out->options_.workspace_identity;  // W2:整份递(与 automation 同尺)
    executor_options.cwd_utf8 = out->options_.cwd_utf8;
    executor_options.lubancode_version = out->options_.lubancode_version;
    executor_options.wire_name = out->options_.wire_name;
    executor_options.model = out->options_.model;
    executor_options.skills_prompt = out->options_.skills_prompt;
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
    // Q4 媒体仓:缺省落 workspace 身份根下 channel-media/(不在渠道状态根
    // ——Q0 工具护单盖整棵渠道状态树,模型 read_file 读不到;workspace 侧
    // 受控读取口 = 既有 read_file 的路径纪律)。开不了账不拦泵:附件行
    // 如实报不可用,正文路照走。
    std::filesystem::path media_root = out->options_.media_root;
    if (media_root.empty()) {
        media_root = out->options_.workspace_identity.identity_root / "channel-media";
    }
    ChannelMediaService media_service;
    if (ChannelMediaService::Open(&media_service, media_root)) {
        out->media_service_.emplace(std::move(media_service));
    }
    // Q6(§12.2 第九行):等按钮的线程不能是唯一收按钮线程——起专用工作
    // 线程跑渠道 turn,tick 线程只推进事件/投递/恢复。0 = 同步(旧测试
    // 装配零变化)。审批窗是人工节奏,turn 阻塞在工作线程,tick 不堵:
    // QQ 心跳、新来信、控制命令照常运转。
    for (std::size_t i = 0; i < out->options_.channel_turn_workers; ++i) {
        out->turn_workers_.emplace_back([out]() { out->TurnWorkerLoop(); });
    }
    result.ok = true;
    return result;
}

std::string ChannelWorkPump::IngestAttachmentsPrompt(
    const channel::ChannelManager::WorkItem& work, std::vector<api::ImageBlock>* images) {
    if (!media_service_.has_value()) {
        return std::string();
    }
    const auto receipts = media_service_->Ingest(
        work.event, work.sid, options_.media_download, options_.media_limits,
        options_.now_ms());
    std::string prompt;
    for (const auto& receipt : receipts) {
        if (!prompt.empty()) {
            prompt += "\n";
        }
        prompt += receipt.prompt_line;
        if (receipt.ready && receipt.mime_type.rfind("image/", 0) == 0 && !options_.accepts_images) {
            prompt += "\n[当前模型目录声明只支持文本，图片已存档但未送入视觉输入。请换支持图片的模型。]";
            continue;
        }
        if (images != nullptr) {
            if (auto image = LoadChannelImage(receipt)) {
                images->push_back(std::move(*image));
                prompt += "\n[已附加图片视觉输入；能否识图取决于当前模型。]";
            } else if (receipt.ready && receipt.mime_type.rfind("image/", 0) == 0) {
                prompt += "\n[图片未接入视觉输入：格式、尺寸或原件校验未通过。不要声称看过图片。]";
            }
        }
    }
    return prompt;
}

std::optional<gateway::DurableReplyOutbox::ChannelAttachment>
ChannelWorkPump::ReplyFileAttachment(const std::string& selection_id,
                                     const std::string& reply_text) const {
    // 产物附件合同(Q4 §十,最保守路):任务结果文件 = 本轮 reply selection
    // 的冻结正文原件(replies/<selectionId>.txt——本地族 out/<id>.txt 的
    // 渠道对应物)。短回复(单段装得下)即正文,不带文件;拆段 > 1 时末段
    // 附带完整正文原件,手机上不用连刷多屏。点名发文件
    // (channel_deliver_artifact)归后续批。
    if (gateway::SplitReplySegments(reply_text, gateway::kChannelSegmentBytes).size() <= 1) {
        return std::nullopt;
    }
    const std::filesystem::path artifact =
        options_.outbox->replies_dir() / (selection_id + ".txt");
    std::error_code ec;
    if (!std::filesystem::exists(artifact, ec) || ec) {
        return std::nullopt;  // 原件不在(理论不可达——selection 已提交);不带
    }
    gateway::DurableReplyOutbox::ChannelAttachment attachment;
    attachment.local_path = platform::PathToUtf8(artifact);
    attachment.file_name = selection_id + ".txt";
    attachment.mime_type = "text/plain";
    attachment.size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(artifact, ec));
    if (ec) {
        return std::nullopt;
    }
    return attachment;
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
        books->session_map.Find(channel::ChannelSessionMap::SlotKey(session_key,
            books->session_map.ActiveSlot(session_key, options_.workspace_identity.workspace_key)),
            options_.workspace_identity.workspace_key);
    return found.has_value() ? *found : std::string();
}

std::string ChannelWorkPump::job_session_id_for(const std::string& channel_id,
                                                const std::string& account_id,
                                                const std::string& job_id) const {
    AccountBooks* books = const_cast<ChannelWorkPump*>(this)->BooksFor(channel_id, account_id);
    if (books == nullptr) {
        return std::string();
    }
    const auto found = books->session_map.Find(
        MakeChannelJobSessionKey(channel_id, account_id, job_id),
        options_.workspace_identity.workspace_key);
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
    // 2.6) Q6 审批面:互动回调排水(裁决→回应平台)+ 审批卡投递。放在
    // 回执结算后(卡片投递结果先知)、新执行前(已批的 turn 快点续跑)。
    if (!DriveApprovalFlow(now_ms)) {
        return false;
    }
    // 2.5) Q1b 配对提示入箱(投递走第 5 步的既有渠道投递驱动)。
    PumpPairingNotices(now_ms);
    // 3) 恢复扫描(Running 件的跨账裁决;不盲重跑)。
    if (!SweepRecovery(now_ms)) {
        return false;
    }
    // 3.5) Q5 渠道任务恢复:claimed 未结算的跨账裁决(automation 泵只管
    // 本地任务,渠道任务的恢复/认领/执行都在本泵)。
    if (!SweepChannelJobRecovery(now_ms)) {
        return false;
    }
    // 4) 至多一轮新执行(公平:与 automation 泵各一;账号间轮转)。
    if (accepting_.load()) {
        if (!RunOneChannelTurn(now_ms)) {
            return false;
        }
        // 4.5) Q5:至多一枚渠道任务执行(周期拍点由 automation 泵的
        // SweepSchedule 生成——同一本账,两只泵各认各的,不抢)。
        std::string job_error;
        if (!RunOneChannelJob(now_ms, &job_error)) {
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
    // 异步泵(Q6):打断在飞审批等待(cancel 旗 → WaitApproval 悬空收口,
    // 按拒绝收场),队列里未跑的件退回 ingress 账(重启恢复面接管,不盲
    // 跑)。
    closed_.store(true);
    ShutDownTurnWorkers();
    if (executor_.has_value()) {
        executor_->CloseChannelSessions("gateway_channel_shutdown");
    }
    return true;
}

void ChannelWorkPump::ShutDownTurnWorkers() {
    if (turn_workers_.empty() && turn_jobs_.empty()) {
        return;
    }
    turn_workers_stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
        // 未跑的件退回:claim 了不跑会把件搁死在 Running——按"宿主收口"
        // 明退,重启恢复面按需 review,不盲重跑。
        for (const TurnJob& job : turn_jobs_) {
            (void)options_.manager->DeadLetterIngress(job.channel_id, job.account_id,
                                                      job.work.sid, "gateway_shutdown");
        }
        turn_jobs_.clear();
        // 在飞的打断:cancel 旗 → 审批等待悬空收口 + 模型流取消链。
        for (const auto& cancel : inflight_cancels_) {
            cancel->store(true);
        }
    }
    turn_jobs_wake_.notify_all();
    for (std::thread& worker : turn_workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    turn_workers_.clear();
}

void ChannelWorkPump::TurnWorkerLoop() {
    while (true) {
        std::optional<TurnJob> job;
        {
            std::unique_lock<std::mutex> lock(turn_jobs_mutex_);
            turn_jobs_wake_.wait(lock, [this]() {
                return !turn_jobs_.empty() || turn_workers_stop_.load();
            });
            if (turn_jobs_.empty()) {
                if (turn_workers_stop_.load()) {
                    return;
                }
                continue;
            }
            job = std::move(turn_jobs_.front());
            turn_jobs_.pop_front();
        }
        const bool ok =
            ProcessWorkItem(job->channel_id, job->account_id, job->work, job->now_ms,
                            job->turn_key, job->cancel.get());
        {
            std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
            --active_turns_;
            in_flight_sids_.erase(job->turn_key);
            for (auto it = inflight_cancels_.begin(); it != inflight_cancels_.end(); ++it) {
                if (it->get() == job->cancel.get()) {
                    inflight_cancels_.erase(it);
                    break;
                }
            }
        }
        (void)ok;  // 失败已在 ProcessWorkItem 内结算(DeadLetter/停泵诊断)
    }
}

bool ChannelWorkPump::ApplyDeliveryOutcomes(std::int64_t now_ms) {
    for (const auto& snapshot : options_.manager->Snapshots()) {
        for (const auto& outcome : options_.manager->DrainChannelDeliveryOutcomes(
                 snapshot.channel_id, snapshot.account_id)) {
            const auto item = options_.outbox->Find(outcome.client_delivery_id);
            if (!item.has_value()) {
                // Q6:审批卡片的回执不在 reply outbox 账上(交互 outbox 是
                // 泵内队列)——按 delivery_id 对账;对得上就消化这笔回执。
                if (SettleApprovalCardOutcome(outcome.client_delivery_id, outcome.status,
                                              outcome.error_code, now_ms)) {
                    continue;
                }
                continue;  // 账上没有(理论不可达):丢弃留诊断
            }
            switch (outcome.status) {
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Accepted:
                    // QQ 已接受(provider_message_id 记账;幂等)。
                    (void)options_.outbox->MarkSent(outcome.client_delivery_id,
                                                    outcome.provider_message_id, now_ms);
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::RateLimited:
                    if (item->source_ref.rfind("chanjob:", 0) == 0) {
                        // Q5 渠道任务段:主动消息额度受限(§11.3 40034100
                        // 一族)——不硬发不谎报:挂起等互动,下一封来信进
                        // 回复窗后锚定补投。attempt 帽不烧(政策性等待,
                        // 不是发送失败)。补投会换锚(主动→被动):旧发送
                        // 身份先裁决(A06——平台已拒,账里留迹),新身份
                        // 待新锚分配,不偷换同一 delivery。
                        (void)options_.outbox->RetireChannelSendIdentity(
                            outcome.client_delivery_id, "active_quota_rejected_await_interaction",
                            now_ms);
                        await_interaction_.insert(outcome.client_delivery_id);
                        break;
                    }
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
                    if (item->source_ref.rfind("chanjob:", 0) == 0 &&
                        outcome.error_code == "reply_window_expired") {
                        // Q5 渠道任务段:回复窗口过期——不硬发(不拿陈旧
                        // msg_id 冒充被动回复)、不谎报(不记成功/终态失败),
                        // 挂起待下次互动补投(§11.3)。普通聊天回复维持 Q2
                        // 规矩:窗口过期不转主动消息,终态。
                        // A06 改锚补投的裁决:过期锚上的旧尝试先裁决(平台
                        // 已明确拒绝该锚),新锚进窗后分配可追溯的新身份。
                        (void)options_.outbox->RetireChannelSendIdentity(
                            outcome.client_delivery_id, "reply_window_expired_reanchor",
                            now_ms);
                        await_interaction_.insert(outcome.client_delivery_id);
                        break;
                    }
                    (void)options_.outbox->MarkChannelFailed(
                        outcome.client_delivery_id,
                        outcome.error_code.empty() ? "platform_reject" : outcome.error_code,
                        now_ms);
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::AuthFailed:
                    // 令牌失效:终态失败,不自动重试,不重跑 Agent。
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

// Q1b 配对提示排水:manager 的 PendingPairing 提示入 outbox 渠道段——走
// Q2 既有链路(EnqueueChannel 拆段/幂等 deliveryId + DriveChannelDeliveries
// 投递/回执结算),不旁路。selection_id 由 code 派生(同 code 重入同
// deliveryId,幂等);来源审计用独立前缀,不吃 ingress 结算路。
void ChannelWorkPump::PumpPairingNotices(std::int64_t now_ms) {
    for (const auto& snapshot : options_.manager->Snapshots()) {
        for (const auto& notice : options_.manager->DrainPendingPairingNotices(
                 snapshot.channel_id, snapshot.account_id)) {
            gateway::DurableReplyOutbox::ChannelTarget target;
            target.channel_id = snapshot.channel_id;
            target.account_id = snapshot.account_id;
            target.conversation_id = notice.conversation_id;
            target.reply_to_message_id = notice.reply_to_message_id;
            target.source_ref = "pairing-notice:" + snapshot.channel_id + ":" +
                                snapshot.account_id + ":" + notice.sender_id;
            const std::string selection_id = "pairing:" + snapshot.channel_id + ":" +
                                             snapshot.account_id + ":code:" + notice.code;
            const auto enqueued = options_.outbox->EnqueueChannel(
                selection_id, notice.text, /*session_id=*/std::string(),
                /*turn_id=*/std::string(), target, now_ms);
            (void)enqueued;  // 幂等重入/账 broken 都不拦泵:broken 由统一闸停
        }
    }
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
            bool in_flight = false;
            {
                std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
                in_flight = in_flight_sids_.count(snapshot.channel_id + "/" +
                                                  snapshot.account_id + "/" +
                                                  std::to_string(view.sid)) > 0;
            }
            if (in_flight) {
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
    const auto dead_letter = [this, &channel_id, &account_id, &view,
                              now_ms](const std::string& reason) {
        const auto error =
            options_.manager->DeadLetterIngress(channel_id, account_id, view.sid, reason);
        if (!error.has_value()) {
            // P0 刀二:恢复裁决的死信同样给用户提示(幂等:同 sid 同
            // deliveryId,重复恢复不刷屏)。执行失败事实不因提示改写。
            EnqueueTurnFailureNotice(channel_id, account_id, view.sid, view.event,
                                     "needs_review", reason, now_ms);
        }
        return error;
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
    // Q4 产物附件:与执行路同一纯函数(同正文同段数同附件,幂等补投影)。
    auto attachment = StagedChannelFile(options_.outbox->replies_dir() / "files",
        MakeChannelOperationId(channel_id, account_id, view.sid));
    if (!attachment) attachment = ReplyFileAttachment(plan.selection_id, plan.text);
    const auto enqueued = options_.outbox->EnqueueChannel(
        plan.selection_id, plan.text, bound->session_id, bound->turn_id, target, now_ms,
        attachment.has_value() ? &*attachment : nullptr);
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
    {
        std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
        if (options_.max_active_channel_turns > 0 &&
            active_turns_ >= options_.max_active_channel_turns) {
            return true;
        }
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
        const std::string turn_key = snapshot.channel_id + "/" + snapshot.account_id + "/" +
                                     std::to_string(work->sid);
        {
            std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
            in_flight_sids_.insert(turn_key);
            ++active_turns_;
        }
        // Q6:异步模式把执行递工作线程(审批等待阻塞在那边,tick 不堵);
        // 同步模式(旧装配/多数测试)现场跑,行为不变。
        if (!turn_workers_.empty()) {
            TurnJob job;
            job.channel_id = snapshot.channel_id;
            job.account_id = snapshot.account_id;
            job.turn_key = turn_key;
            job.work = std::move(*work);
            job.now_ms = now_ms;
            job.cancel = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
                inflight_cancels_.push_back(job.cancel);
                turn_jobs_.push_back(std::move(job));
            }
            turn_jobs_wake_.notify_one();
            return true;
        }
        std::atomic<bool> cancel_flag{false};
        const bool ok = ProcessWorkItem(snapshot.channel_id, snapshot.account_id, *work, now_ms,
                                        turn_key, &cancel_flag);
        {
            std::lock_guard<std::mutex> lock(turn_jobs_mutex_);
            --active_turns_;
            in_flight_sids_.erase(turn_key);
        }
        return ok;
    }
    return true;
}

bool ChannelWorkPump::ProcessWorkItem(const std::string& channel_id,
                                      const std::string& account_id,
                                      const channel::ChannelManager::WorkItem& work,
                                      std::int64_t now_ms, const std::string& turn_key,
                                      const std::atomic<bool>* cancel) {
    AccountBooks* books = BooksFor(channel_id, account_id);
    // Q5 补投锚:该会话最近一封被受理的来信(被动回复窗判定的原料)。
    // Q6 异步 turn 起,写在工作线程、读在 tick(FreshInboundAnchor)——
    // 过 recent_inbound_mutex_。
    {
        std::lock_guard<std::mutex> lock(recent_inbound_mutex_);
        recent_inbound_[channel_id + "/" + account_id + "/" + work.conversation_id] =
            RecentInbound{work.event.message_id, work.event.received_at_ms};
    }
    // Q5 聊天侧任务工具的渠道上下文:本轮存续期间冻结(模型不可伪造),
    // 轮结束即清——终端路/自动任务路没有 Scope,工具必拒。
    std::optional<ChannelAutomationBridge::TurnScope> automation_scope;
    if (options_.automation_bridge != nullptr) {
        ChannelAutomationBridge::TurnContext context;
        context.channel_id = channel_id;
        context.account_id = account_id;
        context.conversation_id = work.conversation_id;
        context.conversation_kind = work.event.conversation.kind;
        context.sender_id = work.sender_id;
        context.message_id = work.event.message_id;
        context.received_at_ms = work.event.received_at_ms;
        automation_scope.emplace(*options_.automation_bridge, context);
    }
    // 正文投影:渠道事件的冻结投影(媒体占位说明,同一份 MakeChannelTurnIngress)。
    const TurnIngress ingress = MakeChannelTurnIngress(
        work.event, work.route.provenance, work.route.session_key,
        work.route.memory.user_memory || work.route.memory.project_memory,
        &work.route.tools);

    // Q7 菜单/面板回调分派:命中命令表的输入(菜单 send_message / 面板
    // command 填入、用户发送后的文本)走宿主侧动作——控制命令零模型直答,
    // 预设输入换正文照常过闸进模型。菜单不扩权:require_tools 逐名过本轮
    // 冻结策略,名单外就地拒;不命中命令表的输入零变化。
    std::optional<std::string> preset_prompt;
    if (const auto command =
            channel::MatchChannelCommand(work.commands, PromptFromIngress(ingress))) {
        if (command->action != "prompt") {
            std::string reply;
            if (command->action == "help") {
                reply = channel::MakeChannelHelpText(work.commands);
            } else if (command->action == "unknown") {
                reply = "暂不支持这个命令。输入 /help 查看可用命令。";
            } else if (command->action == "menu_help") {
                reply = "菜单须由本机配置启用。在全局 config.json 的 QQ 账号段加入：\n"
                        "\"menu\": {\"publish\": true, \"preset\": \"assistant\"}\n"
                        "重启 Gateway 后会尝试发布帮助、新会话、会话列表、提醒、文件说明、功能状态。"
                        "发布失败或远端菜单冲突请查看 Gateway 诊断；本条说明不代表已发布。";
            } else if (command->action == "session") {
                const auto& base = work.route.session_key;
                const auto& workspace = options_.workspace_identity.workspace_key;
                const auto active = books->session_map.ActiveSlot(base, workspace);
                const auto slots = books->session_map.Slots(base, workspace);
                const auto& args = command->prompt;
                if (args.empty() || args == "list") {
                    reply = "会话列表（仅当前 QQ 会话、当前工作区）：\n";
                    for (const auto& slot : slots) {
                        reply += (slot == active ? "* " : "  ") + slot + "\n";
                    }
                    reply += "输入 /session switch <编号> 切换；/new 开新会话。";
                } else if (args == "current") {
                    reply = "当前会话：" + active;
                } else if (args == "new") {
                    // 同一来信重入仍选同一编号，不重复开场。首次发言时才建 V3。
                    const auto slot = "s-" + std::to_string(work.sid);
                    if (!books->session_map.SelectSlot(base, workspace, slot, true, now_ms)) return false;
                    reply = "已切到新会话 " + slot + "。下一条消息从空上下文开始；历史和提醒照留。";
                } else if (args.rfind("switch ", 0) == 0) {
                    const auto slot = args.substr(7);
                    if (std::find(slots.begin(), slots.end(), slot) == slots.end()) {
                        reply = "找不到这个会话。输入 /session 查看本聊天可切换的编号。";
                    } else {
                        if (!books->session_map.SelectSlot(base, workspace, slot, false, now_ms)) return false;
                        reply = "已切到会话 " + slot + "。";
                    }
                } else {
                    reply = "用法：/session [list|current|new|switch <编号>]";
                }
            } else if (command->action == "capabilities") {
                reply = "当前工作目录：" + options_.cwd_utf8 + "\n工具状态：\n";
                for (const auto& name : {"run_command", "web_search", "web_fetch", "skill", "send_file"}) {
                    reply += std::string(name) + "：";
                    const auto* tool = registry_->Find(name);
                    if (tool == nullptr) reply += "未装配";
                    else if (!work.route.tools.Allows(name)) reply += "渠道策略未放行";
                    else if (tool->needs_confirm() && !work.route.tools.ExplicitlyAllows(name))
                        reply += "须审批（未配置审批带时会拒绝）";
                    else reply += "可用";
                    reply += "\n";
                }
                if (registry_->Find("web_search") == nullptr)
                    reply += "联网搜索须配置全局 search.provider 与 search.api_key，再放行 web_search。\n";
                reply += options_.accepts_images ? "图片：已接视觉输入，需模型支持。\n" : "图片：当前模型只支持文本。\n";
                reply += options_.skills_summary;
            } else if (command->action == "file_help") {
                reply = channel::MakeChannelFileHelpText();
            } else {  // list_reminders(automation 域不在 → 稳定说明,不装死)
                if (options_.automation_bridge == nullptr ||
                    options_.automation_bridge->store() == nullptr) {
                    reply = channel::MakeMenuCommandUnavailableText();
                } else {
                    const auto outcome = options_.automation_bridge->ListReminders();
                    reply = outcome.ok
                                ? channel::FormatReminderListText(outcome.payload, now_ms)
                                : channel::MakeMenuCommandUnavailableText();
                }
            }
            return ReplyMenuCommand(channel_id, account_id, work, reply, now_ms);
        }
        std::vector<std::string> missing_tools;
        for (const auto& tool : command->require_tools) {
            if (!work.route.tools.Allows(tool)) {
                missing_tools.push_back(tool);
            }
        }
        if (!missing_tools.empty()) {
            return ReplyMenuCommand(channel_id, account_id, work,
                                    channel::MakeMenuCommandDeniedText(missing_tools), now_ms);
        }
        preset_prompt = command->prompt;  // 换输入,走完整模型轮(逐轮闸照旧)
    }

    HeadlessExecutor::ChannelTurnRequest request;
    request.session_key = channel::ChannelSessionMap::SlotKey(work.route.session_key,
        books->session_map.ActiveSlot(work.route.session_key, options_.workspace_identity.workspace_key));
    const auto stored = books->session_map.Find(request.session_key,
                                                options_.workspace_identity.workspace_key);
    if (stored.has_value()) {
        request.stored_session_id = *stored;
    }
    request.prompt = preset_prompt.has_value() ? *preset_prompt : PromptFromIngress(ingress);
    // Q4 附件接纳(准入已过、执行前):下载落仓 + 有界预览行并进 prompt。
    // 失败附件给稳定说明,不假装读过文件;不拦正文轮。
    if (const std::string media_prompt = IngestAttachmentsPrompt(work, &request.images); !media_prompt.empty()) {
        request.prompt += "\n" + media_prompt;
    }
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
    // Q6 远端审批:needs_confirm 工具在 tools.approve 带内时,确认经
    // ChannelInteractionBroker 发 QQ 按钮卡片问用户(上下文全部本轮冻结,
    // 模型不可伪造)。空 broker = 无审批路(裁定里带外工具 fail closed,
    // 行为与 Q0 一致)。
    if (options_.interaction_broker != nullptr) {
        const std::string frozen_turn_key = turn_key;
        const std::string frozen_channel = channel_id;
        const std::string frozen_account = account_id;
        const std::string frozen_conversation = work.conversation_id;
        const std::string frozen_session_key = work.route.session_key;
        const std::string frozen_sender = work.sender_id;
        const std::string frozen_message_id = work.event.message_id;
        const std::int64_t frozen_received_at = work.event.received_at_ms;
        const channel::ToolRoutePolicy frozen_tools = work.route.tools;
        ChannelWorkPump* pump = this;
        request.on_tool_confirm =
            [pump, frozen_turn_key, frozen_channel, frozen_account, frozen_conversation,
             frozen_session_key, frozen_sender, frozen_message_id, frozen_received_at,
             frozen_tools, cancel](const std::string& tool_use_id, const std::string& name,
                                   const nlohmann::json& input) {
                return pump->DecideChannelToolApproval(
                    frozen_turn_key, frozen_tools, tool_use_id, name, input, frozen_channel,
                    frozen_account, frozen_conversation, frozen_session_key, frozen_sender,
                    frozen_message_id, frozen_received_at, cancel);
            };
    }

    const std::int64_t sid = work.sid;
    const std::string bound_session_key = work.route.session_key;
    std::atomic<bool> local_cancel{false};
    const std::atomic<bool>* effective_cancel = cancel != nullptr ? cancel : &local_cancel;
    ChannelFileDeliveryScope file_scope(platform::Utf8ToPath(options_.cwd_utf8),
        options_.outbox->replies_dir() / "files", request.binding.work_id);
    const auto result = executor_->ExecuteChannelTurn(
        request,
        [this, books, sid, bound_session_key, now_ms](const std::string& session_id,
                                                      const std::string& turn_id) {
            // 领域绑定先于 V3 work.bound(恢复器优先走领域行定位原场)。
            (void)books->work_ledger.Bind(sid, bound_session_key, session_id, turn_id, now_ms);
        },
        effective_cancel);
    // Q6:turn 收场(writer 空闲窗口)把审批流水落进该场 V3——requested/
    // resolved 都是事实行,同 session 单飞保证此刻无并发写。
    WriteApprovalFactsToV3(result.session_id, result.turn_id, turn_key);
    if (!result.ok) {
        if (result.error_code == "gateway.fault_injected") {
            return true;  // 不结算:恢复路接管(模拟进程死在半路)
        }
        // 执行失败(模型错/取消/受理拒):如实退场,不盲重跑。
        const auto dead_letter = options_.manager->DeadLetterIngress(
            channel_id, account_id, sid, "turn_failed: " + result.error_code + ": " + result.error);
        if (!dead_letter.has_value()) {
            // P0 刀二:死信已落,补一条用户提示(幂等;送达不改写执行失败
            // 事实)。死信写失败(账问题)时不发提示——件还在 Running,恢复
            // 扫描重裁决时会再走到这里。
            EnqueueTurnFailureNotice(channel_id, account_id, sid, work.event, result.error_code,
                                      result.error, now_ms);
        }
        return true;
    }
    // reply selection 已提交 → outbox 投影(拆段入箱,deliveryId 定式幂等)。
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = channel_id;
    target.account_id = account_id;
    target.conversation_id = work.conversation_id;
    target.reply_to_message_id = work.event.message_id;
    target.source_ref = "ingress:" + channel_id + ":" + account_id + ":" + std::to_string(sid);
    // Q4 产物附件:长文(拆段 > 1)末段附带任务结果文件(冻结正文原件)。
    auto attachment = StagedChannelFile(options_.outbox->replies_dir() / "files", request.binding.work_id);
    if (!attachment) attachment = ReplyFileAttachment(result.selection_id, result.reply_text);
    const auto enqueued = options_.outbox->EnqueueChannel(
        result.selection_id, result.reply_text, result.session_id, result.turn_id, target,
        now_ms, attachment.has_value() ? &*attachment : nullptr);
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

bool ChannelWorkPump::ReplyMenuCommand(const std::string& channel_id,
                                       const std::string& account_id,
                                       const channel::ChannelManager::WorkItem& work,
                                       const std::string& reply_text, std::int64_t now_ms) {
    // 直答路径:零模型。回复沿 outbox 渠道段(selection 幂等),source_ref
    // 用 ingress 定式——ReconcileDeliveredSources 照常把这件 ingress 推到
    // delivered/delivery_failed(与正文路同一条投递结算链)。
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = channel_id;
    target.account_id = account_id;
    target.conversation_id = work.conversation_id;
    target.reply_to_message_id = work.event.message_id;
    target.source_ref = "ingress:" + channel_id + ":" + account_id + ":" +
                        std::to_string(work.sid);
    const std::string selection_id =
        "menucmd:" + channel_id + ":" + account_id + ":" + std::to_string(work.sid);
    const auto enqueued =
        options_.outbox->EnqueueChannel(selection_id, reply_text, /*session_id=*/std::string(),
                                        /*turn_id=*/std::string(), target, now_ms);
    // 执行侧结算:Running → Replied(投递态另算;与正文路同款幂等容忍)。
    (void)options_.manager->SettleIngressReplied(channel_id, account_id, work.sid);
    if (!enqueued.accepted && !enqueued.duplicate) {
        return false;  // outbox 账写不进:停泵
    }
    return true;
}

void ChannelWorkPump::EnqueueTurnFailureNotice(
    const std::string& channel_id, const std::string& account_id, std::int64_t sid,
    const channel::ChannelInboundEvent& event, const std::string& error_code,
    const std::string& error_detail, std::int64_t now_ms) {
    // 独立前缀:不吃 ingress 结算路(ReconcileDeliveredSources 解不出
    // ingress 定式即跳过)——提示送达与否都不推 ingress 状态,执行失败
    // 事实只在 dead letter。selection_id 定式按 sid 幂等:重复恢复/重扫
    // 不刷屏(同 deliveryId,EnqueueChannel 回 duplicate)。
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = channel_id;
    target.account_id = account_id;
    target.conversation_id = event.conversation.id;
    target.reply_to_message_id = event.message_id;  // 被动锚(触发来信在窗内)
    target.source_ref = "turnfail:" + channel_id + ":" + account_id + ":" + std::to_string(sid);
    const TurnFailureNotice notice = MakeTurnFailureNotice(error_code, error_detail);
    const std::string text =
        "[未回复说明] " + notice.text + "(编号 " + notice.short_code + ")";
    const std::string selection_id = "turnfail:" + channel_id + ":" + account_id + ":" +
                                     std::to_string(sid);
    const auto enqueued = options_.outbox->EnqueueChannel(
        selection_id, text, /*session_id=*/std::string(), /*turn_id=*/std::string(), target,
        now_ms);
    // 幂等重入/duplicate 不拦泵;账 broken 由统一闸(TickOnce 末尾)停。
    // 提示发不出去(网络/权限/过期窗)时,执行失败在 dead letter、提示
    // 投递失败在 outbox 段状态,两层都可本地查(channel status 链视图)。
    (void)enqueued;
}

// ---------------------------------------------------------------------------
// Q6:QQ 按钮批准一次工具调用(todo §十二)——裁决、卡片、回调、账
// ---------------------------------------------------------------------------

namespace {

// 脱敏摘要(§12.2 第五行:卡片展示经脱敏的操作摘要):工具名 + 入参的
// 字符串字段抽样(每段截 80 字节,至多 3 段),零密钥零全文参数——
// command/path/file_name 这类"人眼能判断"的字段优先,其余字段只报名。
std::string MakeApprovalSummary(const std::string& name, const nlohmann::json& input) {
    std::string summary = name;
    if (input.is_object()) {
        for (const auto* key : {"command", "path", "file_path", "file_name", "url", "query",
                                "pattern", "script"}) {
            if (summary.size() > 160) {
                break;
            }
            if (input.contains(key) && input.at(key).is_string()) {
                std::string value = input.at(key).get<std::string>();
                if (value.size() > 80) {
                    value.resize(80);
                    value += "…";
                }
                summary += " " + std::string(key) + "=" + value;
            }
        }
        if (input.size() > 0 && summary == name) {
            summary += " (参数 " + std::to_string(input.size()) + " 项)";
        }
    }
    return summary;
}

// 审批卡键盘(官方消息按钮页:action.type=1 回调按钮;permission.type=0
// 指定用户——配对 sender 才能按,宿主身份复核是最终防线;click_limit
// 已废弃不填;unsupport_tips 客户端不支持时提示)。
nlohmann::json MakeApprovalKeyboard(const std::string& token, const std::string& operator_id) {
    // 显式逐层构造(深嵌套 initializer list 在 clang 下解析不稳,不赌推导)。
    const auto make_action = [&token, &operator_id](bool accept) {
        nlohmann::json permission = nlohmann::json::object();
        permission["type"] = 0;
        permission["specify_user_ids"] = nlohmann::json::array({operator_id});
        nlohmann::json action = nlohmann::json::object();
        action["type"] = 1;
        action["permission"] = std::move(permission);
        action["data"] = EncodeApprovalButtonData(token, accept);
        action["unsupport_tips"] = "请升级手机QQ后使用按钮审批";
        return action;
    };
    nlohmann::json approve = nlohmann::json::object();
    approve["id"] = "approve";
    approve["render_data"] = {{"label", "允许这次"}, {"visited_label", "已允许"}, {"style", 1}};
    approve["action"] = make_action(true);
    nlohmann::json decline = nlohmann::json::object();
    decline["id"] = "decline";
    decline["render_data"] = {{"label", "拒绝"}, {"visited_label", "已拒绝"}, {"style", 0}};
    decline["action"] = make_action(false);

    nlohmann::json buttons = nlohmann::json::array();
    buttons.push_back(std::move(approve));
    buttons.push_back(std::move(decline));
    nlohmann::json rows = nlohmann::json::array();
    rows.push_back(nlohmann::json{{"buttons", std::move(buttons)}});
    nlohmann::json keyboard = nlohmann::json::object();
    keyboard["content"] = {{"rows", std::move(rows)}};
    return keyboard;
}

}  // namespace

HeadlessExecutor::Options::ToolConfirmDecision ChannelWorkPump::DecideChannelToolApproval(
    const std::string& turn_key, const channel::ToolRoutePolicy& tools,
    const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
    const std::string& channel_id, const std::string& account_id,
    const std::string& conversation_id, const std::string& session_key,
    const std::string& sender_id, const std::string& message_id, std::int64_t received_at_ms,
    const std::atomic<bool>* cancel) {
    using Decision = HeadlessExecutor::Options::ToolConfirmDecision;
    const std::int64_t now = options_.now_ms();
    // 1) 显式预授权(Q0 语义原样):某层 tools.allow 列名且不在 deny——
    //    不问,直接放行。审批政策不改写既有授权。
    if (tools.ExplicitlyAllows(name)) {
        return Decision{true, std::string()};
    }
    // 2) hard deny / 暴露面外:按钮不能覆盖 deny(§12.2 第三行);文案沿
    //    渠道拒绝的既有口径。
    if (!tools.Allows(name)) {
        Decision decision;
        decision.denial_text = ChannelToolDenialText(name);
        return decision;
    }
    // 3) 审批带:approve 带内才可申请远端审批;带外照旧 fail closed。
    if (!tools.Approvable(name) || options_.interaction_broker == nullptr) {
        Decision decision;
        decision.denial_text = ChannelToolDenialText(name);
        return decision;
    }
    // 4) 发卡等按钮:上下文全部宿主冻结;参数 hash 钉住"批的是什么"
    //    (参数变化=新请求新 token,自然重新申请)。
    ChannelApprovalContext context;
    context.channel_id = channel_id;
    context.account_id = account_id;
    context.conversation_id = conversation_id;
    context.session_key = session_key;
    context.turn_key = turn_key;
    context.tool_use_id = tool_use_id;
    context.tool_name = name;
    context.operator_id = sender_id;
    context.message_id = message_id;
    context.summary = MakeApprovalSummary(name, input);
    context.args_sha256 = platform::Sha256Hex(input.is_null() ? std::string("{}") : input.dump());
    context.deadline_ms = now + options_.approval_timeout_ms;
    context.timeout_ms = options_.approval_timeout_ms;
    context.received_at_ms = received_at_ms;
    ChannelWorkPump* pump = this;
    std::string token_out;  // on_requested 同步先于 AskApproval 返回,这里拿得到
    const auto future = options_.interaction_broker->AskApproval(
        context, [&token_out, pump](const ChannelInteractionBroker::RequestedFact& fact) {
            token_out = fact.token;
            pump->EnqueueApprovalCard(fact);
        });
    if (cancel != nullptr) {
        future->WatchInterrupt(cancel);
    }
    const auto response = future->WaitApproval();
    if (!response.has_value()) {
        // 悬空收口:超时/取消/卡片失败——等价拒绝,文案照"没人可答"写,
        // 不冒充用户拒绝(§12.2 第十行)。收口事实分账:cancel 打断走
        // Cancel;其余(超时)记 Timeout;卡片失败已由投递路 CancelByToken
        // 摘表,这里的二次收口是 no-op。
        Decision decision;
        if (cancel != nullptr && cancel->load()) {
            options_.interaction_broker->CancelByToken(token_out, "cancelled");
            decision.denial_text = "本轮被取消,审批悬空收口,未执行工具 " + name + "。";
        } else {
            options_.interaction_broker->NoteTimeout(token_out);
            decision.denial_text =
                "审批窗内没人答复(超时或审批卡投递失败),按拒绝收口,未执行工具 " +
                name + "。";
        }
        return decision;
    }
    if (response->decision == runtime::InteractionDecision::Accept ||
        response->decision == runtime::InteractionDecision::AcceptForSession) {
        return Decision{true, std::string()};
    }
    Decision decision;
    decision.denial_text = "用户在 QQ 上拒绝了工具 " + name + " 的执行请求,本次未执行。";
    return decision;
}

void ChannelWorkPump::EnqueueApprovalCard(
    const ChannelInteractionBroker::RequestedFact& fact) {
    // 交互 outbox(§12.2 第十一行):与已完成 Agent 回复分开、有期限;卡片
    // 发送失败不重试突破审批 TTL(见 DriveApprovalFlow)。delivery_id 定式
    // 稳定:同卡重试同 id(平台 msg_seq 稳定),幂等。
    ApprovalCard card;
    card.token = fact.token;
    card.delivery_id = "appr-card-" + fact.token_hash.substr(0, 16);
    card.channel_id = fact.context.channel_id;
    card.account_id = fact.context.account_id;
    card.conversation_id = fact.context.conversation_id;
    card.reply_to_message_id = fact.context.message_id;  // 被动锚(触发来信在窗内)
    card.markdown = "**工具审批请求**\n" + fact.context.summary +
                    "\n允许则执行这一次;拒绝或超时都不执行。";
    card.keyboard = MakeApprovalKeyboard(fact.token, fact.context.operator_id);
    card.deadline_ms = fact.context.deadline_ms;
    {
        std::lock_guard<std::mutex> lock(approval_cards_mutex_);
        // 同 token 不重复入队(防御:AskApproval 一次一枚)。
        for (const ApprovalCard& existing : approval_cards_) {
            if (existing.token == card.token) {
                return;
            }
        }
        approval_cards_.push_back(std::move(card));
    }
}

bool ChannelWorkPump::DriveApprovalFlow(std::int64_t now_ms) {
    if (options_.interaction_broker == nullptr) {
        return true;
    }
    // 1) 互动回调排水:裁决 → 回应平台(code=0 处理成功/3 重复/4 没权限)。
    //    code=0 只表示回调处理成功,不表示工具执行成功(官方口径)。
    for (const auto& snapshot : options_.manager->Snapshots()) {
        for (const channel::ChannelManager::ChannelInteraction& interaction :
             options_.manager->DrainChannelInteractions(snapshot.channel_id,
                                                        snapshot.account_id)) {
            // 只认消息按钮回调(type=11);其余互动类型(菜单/授权)不归
            // 审批裁决——回"操作失败"让客户端收口,不冒充处理成功。
            if (interaction.type != 11) {
                (void)options_.manager->AckInteraction(snapshot.channel_id, snapshot.account_id,
                                                       interaction.interaction_id,
                                                       /*code=操作失败*/ 1);
                continue;
            }
            std::string token;
            bool accept = false;
            if (!DecodeApprovalButtonData(interaction.button_data, &token, &accept)) {
                // 不是宿主发的审批按钮(菜单指令类):如实回失败,不猜。
                (void)options_.manager->AckInteraction(snapshot.channel_id, snapshot.account_id,
                                                       interaction.interaction_id, 1);
                continue;
            }
            const auto resolution = options_.interaction_broker->ResolveByToken(
                token, accept, interaction.operator_id, interaction.interaction_id);
            int ack_code = 4;  // 默认"没有权限"——未知 token 不泄露存在性
            switch (resolution) {
                case ChannelInteractionBroker::Resolution::Applied:
                    ack_code = 0;  // 成功
                    break;
                case ChannelInteractionBroker::Resolution::Duplicate:
                    ack_code = 3;  // 重复操作(幂等:只返回已处理)
                    break;
                case ChannelInteractionBroker::Resolution::NotAuthorized:
                    ack_code = 4;  // 他人代按:没有权限
                    break;
                case ChannelInteractionBroker::Resolution::Stale:
                    ack_code = 4;  // 未知/过期/重启后旧卡:没有权限
                    break;
            }
            (void)options_.manager->AckInteraction(snapshot.channel_id, snapshot.account_id,
                                                   interaction.interaction_id, ack_code);
        }
    }
    // 2) 卡片驱动:到点的重发(窗内)、过期未投递的取消(fail closed——
    //    没有卡片就没有按钮,等待只会超时,早收口让用户重发指令)。
    std::vector<ApprovalCard> send_now;
    std::vector<std::string> cancel_tokens;
    {
        std::lock_guard<std::mutex> lock(approval_cards_mutex_);
        for (ApprovalCard& card : approval_cards_) {
            if (card.done || card.inflight) {
                continue;
            }
            if (now_ms >= card.deadline_ms) {
                // 审批窗已过:等 WaitApproval 自己超时收口(默认拒绝),
                // 卡片不再投递。
                card.done = true;
                continue;
            }
            if (now_ms < card.retry_at_ms) {
                continue;
            }
            card.inflight = true;
            send_now.push_back(card);
        }
    }
    for (const ApprovalCard& card : send_now) {
        // 发送身份(A05):与正文段/提示/附件共用 outbox 持久分配器——
        // 同一来信下所有回复各占一号,不因撞号把去重回执错挂。重试(窗内
        // 退避)幂等回旧身份;锚不变,无冲突路径。
        std::uint32_t card_msg_seq = 0;
        std::string card_anchor = card.reply_to_message_id;
        {
            const std::string payload_sha =
                platform::Sha256Hex(card.markdown + card.keyboard.dump());
            const auto assigned = options_.outbox->AssignChannelSendIdentity(
                card.delivery_id, card.account_id, card.reply_to_message_id, payload_sha,
                now_ms);
            if (!assigned.ok) {
                // 账写不进:卡片这轮不投(退避重试),不冒充已问过用户。
                std::lock_guard<std::mutex> lock(approval_cards_mutex_);
                for (ApprovalCard& entry : approval_cards_) {
                    if (entry.delivery_id == card.delivery_id) {
                        entry.inflight = false;
                        entry.retry_at_ms = now_ms + options_.send_retry_backoff_ms;
                    }
                }
                continue;
            }
            card_anchor = assigned.identity.anchor_msg_id;
            card_msg_seq = assigned.identity.msg_seq;
        }
        channel::ChannelManager::ChannelSendRequest send;
        send.conversation_id = card.conversation_id;
        send.text = card.markdown;
        send.reply_to_message_id = card_anchor;
        send.client_delivery_id = card.delivery_id;
        send.msg_seq = card_msg_seq;
        send.keyboard = card.keyboard;
        const auto error =
            options_.manager->SendReply(card.channel_id, card.account_id, send);
        if (error.has_value()) {
            // 受理失败(账号不在 Running/transport 缺):重试或取消按窗判。
            const bool in_window = now_ms + options_.send_retry_backoff_ms < card.deadline_ms;
            std::lock_guard<std::mutex> lock(approval_cards_mutex_);
            for (ApprovalCard& entry : approval_cards_) {
                if (entry.delivery_id != card.delivery_id) {
                    continue;
                }
                entry.inflight = false;
                if (in_window) {
                    entry.retry_at_ms = now_ms + options_.send_retry_backoff_ms;
                } else {
                    entry.done = true;
                    cancel_tokens.push_back(entry.token);
                }
            }
        }
    }
    for (const std::string& token : cancel_tokens) {
        options_.interaction_broker->CancelByToken(token, "card_failed");
    }
    // 3) 已终态卡片的清理(投递成功后按钮可能在窗内任何时候被按——
    //    done 的卡片保留到窗末才清,token 裁决权在 broker)。
    {
        std::lock_guard<std::mutex> lock(approval_cards_mutex_);
        for (auto it = approval_cards_.begin(); it != approval_cards_.end();) {
            if (it->done && now_ms >= it->deadline_ms) {
                it = approval_cards_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

bool ChannelWorkPump::SettleApprovalCardOutcome(
    const std::string& client_delivery_id,
    channel::ChannelManager::ChannelDeliveryOutcome::Status status, const std::string& error_code,
    std::int64_t now_ms) {
    std::optional<ApprovalCard> matched;
    bool is_card = false;
    {
        std::lock_guard<std::mutex> lock(approval_cards_mutex_);
        for (ApprovalCard& card : approval_cards_) {
            if (card.delivery_id != client_delivery_id || !card.inflight) {
                continue;
            }
            is_card = true;
            card.inflight = false;
            switch (status) {
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Accepted:
                    // 已投递:按钮等用户按;审批不动(裁决归回调)。
                    card.done = true;
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::RateLimited:
                    // 窗内退避重试,不突破审批 TTL。
                    if (now_ms + options_.send_retry_backoff_ms < card.deadline_ms) {
                        card.retry_at_ms = now_ms + options_.send_retry_backoff_ms;
                    } else {
                        card.done = true;
                        matched = card;
                    }
                    break;
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Rejected:
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::AuthFailed:
                case channel::ChannelManager::ChannelDeliveryOutcome::Status::Unknown:
                    // 明确拒绝/令牌失效/delivery_unknown:按钮可能没到用户
                    // 手上——取消等待(fail closed),用户重发指令再来。
                    card.done = true;
                    matched = card;
                    break;
            }
            break;
        }
    }
    if (matched.has_value()) {
        options_.interaction_broker->CancelByToken(matched->token, "card_failed");
    }
    (void)error_code;
    return is_card;
}

void ChannelWorkPump::WriteApprovalFactsToV3(const std::string& session_id,
                                             const std::string& turn_id,
                                             const std::string& turn_key) {
    if (options_.interaction_broker == nullptr || session_id.empty()) {
        return;
    }
    std::vector<ChannelInteractionBroker::RequestedFact> requested;
    std::vector<ChannelInteractionBroker::ResolvedFact> resolved;
    options_.interaction_broker->TakeFactsForTurn(turn_key, &requested, &resolved);
    if (requested.empty() && resolved.empty()) {
        return;
    }
    // 场流定位(RecoverOne 同款路:workspace 房门按 key 反查)。落不进
    // 账不拦 turn 收场——执行事实优先,审批审计丢失留诊断(stderr)。
    const auto room = workspace::index::ResolveDirByWorkspaceKey(options_.workspaces_root,
                                                                 options_.workspace_identity.workspace_key);
    if (!room.has_value()) {
        return;
    }
    const auto stream = trajectory::v3::FindV3SessionStream(*room / "sessions" / session_id);
    if (!stream.has_value()) {
        return;
    }
    auto writer = trajectory::v3::V3Writer::Continue(*stream);
    if (!writer.has_value()) {
        return;
    }
    for (const auto& fact : requested) {
        trajectory::v3::EventDraft draft;
        draft.kind = trajectory::v3::EventKindV3::ChannelApprovalRequested;
        draft.turn_id = turn_id;
        draft.payload = nlohmann::json::object({
            {"tokenHash", fact.token_hash},
            {"tool", fact.context.tool_name},
            {"argsSha256", fact.context.args_sha256},
            {"channelId", fact.context.channel_id},
            {"accountId", fact.context.account_id},
            {"conversationId", fact.context.conversation_id},
            {"operatorId", fact.context.operator_id},
            {"deadlineMs", fact.context.deadline_ms},
        });
        (void)writer->AppendEvent(std::move(draft), trajectory::v3::Durability::PowerLoss);
    }
    for (const auto& fact : resolved) {
        const char* decision = "cancelled";
        switch (fact.outcome) {
            case ChannelInteractionBroker::Outcome::Approved:
                decision = "approved";
                break;
            case ChannelInteractionBroker::Outcome::Declined:
                decision = "declined";
                break;
            case ChannelInteractionBroker::Outcome::Timeout:
                decision = "timeout";
                break;
            case ChannelInteractionBroker::Outcome::Cancelled:
                decision = "cancelled";
                break;
            case ChannelInteractionBroker::Outcome::Pending:
                break;
        }
        trajectory::v3::EventDraft draft;
        draft.kind = trajectory::v3::EventKindV3::ChannelApprovalResolved;
        draft.turn_id = turn_id;
        draft.payload = nlohmann::json::object({
            {"tokenHash", fact.token_hash},
            {"decision", decision},
            {"by", fact.by},
        });
        if (!fact.interaction_id.empty()) {
            draft.payload["interactionId"] = fact.interaction_id;
        }
        (void)writer->AppendEvent(std::move(draft), trajectory::v3::Durability::PowerLoss);
    }
}

// ---------------------------------------------------------------------------
// Q5:渠道身份建的定时任务——认领/执行/恢复/补投(todo §十一)
// ---------------------------------------------------------------------------

bool ChannelWorkPump::SweepChannelJobRecovery(std::int64_t now_ms) {
    if (options_.automation_store == nullptr) {
        return true;
    }
    for (const auto& occurrence : options_.automation_store->OpenOccurrences()) {
        const auto job = options_.automation_store->FindJob(occurrence.job_id);
        if (!job.has_value() || !job->ChannelBacked()) {
            continue;  // 本地任务归 automation 泵的恢复面
        }
        if (!RecoverChannelJobOccurrence(occurrence, now_ms)) {
            return false;  // 账写不进:停泵(与 ingress 恢复同一条纪律)
        }
    }
    return !options_.automation_store->broken();
}

bool ChannelWorkPump::RecoverChannelJobOccurrence(const gateway::AutomationOccurrence& occurrence,
                                                  std::int64_t now_ms) {
    gateway::AutomationStore* store = options_.automation_store;
    const auto job = store->FindJob(occurrence.job_id);
    if (!job.has_value()) {
        return store->SettleOccurrence(occurrence.occurrence_id, "failed", "job_missing", now_ms);
    }
    // 绑定行不在:claim 后崩,无开轮事实——重派同一 occurrence(attempt+1);
    // 任务已取消 → cancelled;attempt 帽到顶 → needs_review(§八同款裁决)。
    if (occurrence.session_id.empty() || occurrence.turn_id.empty()) {
        if (job->state == gateway::AutomationJobState::Cancelled) {
            return store->SettleOccurrence(occurrence.occurrence_id, "cancelled",
                                           "job_cancelled_before_redispatch", now_ms);
        }
        if (store->RedispatchOccurrence(occurrence.occurrence_id, "claimed_without_binding",
                                        now_ms)) {
            return true;
        }
        if (store->broken()) {
            return false;
        }
        return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                       "redispatch_exhausted(attempt 帽到顶)", now_ms);
    }
    // 绑定行在:按 V3 账裁决(补 selection / 补 outbox 投影,不调模型)。
    const auto room = workspace::index::ResolveDirByWorkspaceKey(
        options_.workspaces_root, options_.workspace_identity.workspace_key);
    if (!room.has_value()) {
        return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                       "workspace_room_unresolved", now_ms);
    }
    const auto stream =
        trajectory::v3::FindV3SessionStream(*room / "sessions" / occurrence.session_id);
    if (!stream.has_value()) {
        return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                       "v3_stream_not_found", now_ms);
    }
    const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
    if (!ledger.has_value()) {
        return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                       "v3_stream_unreadable: " + ledger.error(), now_ms);
    }
    const ReplySelectionPlan plan = PlanReplySelection(*ledger, occurrence.turn_id);
    if (!plan.ok) {
        return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                       "generation_incomplete: " + plan.error, now_ms);
    }
    const std::string target_str = gateway::MakeChannelDeliveryTarget(
        job->delivery_channel, job->delivery_account, job->delivery_conversation);
    if (!SelectionAlreadyCommitted(*ledger, plan.selection_id)) {
        auto writer = trajectory::v3::V3Writer::Continue(*stream);
        if (!writer.has_value()) {
            return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                           "v3_writer_continue_failed: " + writer.error(), now_ms);
        }
        const auto commit = CommitReplySelection(&*writer, options_.outbox->replies_dir(), plan,
                                                 occurrence.session_id, target_str);
        if (!commit.committed) {
            return store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                           commit.error_code + ": " + commit.error, now_ms);
        }
    }
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = job->delivery_channel;
    target.account_id = job->delivery_account;
    target.conversation_id = job->delivery_conversation;
    // 锚不冻结:发送时按"最近来信是否在窗内"现取(主动/被动分型)。
    target.reply_to_message_id = "";
    target.source_ref = "chanjob:" + job->delivery_channel + ":" + job->delivery_account + ":" +
                        occurrence.job_id + ":" + occurrence.occurrence_id;
    const auto enqueued = options_.outbox->EnqueueChannel(
        plan.selection_id, plan.text, occurrence.session_id, occurrence.turn_id, target, now_ms);
    if (!enqueued.accepted && !enqueued.duplicate) {
        // 入箱失败(账 broken):执行事实保留,occurrence 停审,泵停。
        (void)store->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                      "outbox_enqueue_failed", now_ms);
        return false;
    }
    return store->SettleOccurrence(occurrence.occurrence_id, "succeeded",
                                   "recovered_enqueued_to_channel", now_ms);
}

bool ChannelWorkPump::RunOneChannelJob(std::int64_t now_ms, std::string* error) {
    if (options_.automation_store == nullptr) {
        return true;
    }
    // 并发帽先查(claim 了不跑会把件搁死在 Claimed)。
    if (options_.max_active_channel_turns > 0 &&
        active_turns_ >= options_.max_active_channel_turns) {
        return true;
    }
    const auto claimed = options_.automation_store->ClaimDue(
        owner_epoch_, now_ms, gateway::AutomationStore::ClaimScope::ChannelBackedOnly);
    if (!claimed.has_value()) {
        if (options_.automation_store->broken()) {
            *error = "automation.append_failed: 渠道任务 claim 落不了盘";
            return false;
        }
        return true;  // 没到点的渠道任务
    }
    ++active_turns_;
    struct DecOnReturn {
        ChannelWorkPump* pump;
        ~DecOnReturn() { --pump->active_turns_; }
    } active_guard{this};
    gateway::AutomationStore* store = options_.automation_store;
    const std::string occurrence_id = claimed->occurrence_id;
    const auto job = store->FindJob(claimed->job_id);
    if (!job.has_value()) {
        (void)store->SettleOccurrence(occurrence_id, "failed", "job_missing", now_ms);
        return true;
    }
    // 执行前重验创建者准入(§11.2 末行):配对/allowlist/渠道策略现核,
    // 不过 → cancelled(任务保留,准入恢复后未来拍照跑,不删不暂停)。
    channel::ChannelConversation conversation;
    conversation.kind = channel::ConversationKind::Direct;
    conversation.id = job->delivery_conversation;
    const auto route = options_.manager->ProbeRoute(
        job->owner_channel, job->owner_account, conversation, job->owner_sender, now_ms);
    if (route.status != channel::RouteDecision::Status::Admitted) {
        (void)store->SettleOccurrence(occurrence_id, "cancelled",
                                      "creator_not_admitted:" + route.reason, now_ms);
        return true;
    }
    // 隔离任务场:同 job 同场(任务自己的上下文跨拍续),不进聊天会话
    // (§11.2 不自动带聊天史);重启经映射账 resume-as-new。
    const std::string session_key =
        MakeChannelJobSessionKey(job->delivery_channel, job->delivery_account, job->job_id);
    AccountBooks* books = BooksFor(job->delivery_channel, job->delivery_account);
    HeadlessExecutor::ChannelTurnRequest request;
    request.session_key = session_key;
    if (books != nullptr) {
        const auto stored = books->session_map.Find(session_key,
                                                    options_.workspace_identity.workspace_key);
        if (stored.has_value()) {
            request.stored_session_id = *stored;
        }
    }
    request.prompt = job->prompt;
    request.binding.work_id = occurrence_id;
    request.binding.source_kind = "automation";
    request.binding.source_id = job->job_id;
    request.binding.owner_epoch = owner_epoch_;
    request.binding.attempt = claimed->attempt;
    request.per_turn_tools = &route.tools;
    request.delivery_target =
        gateway::MakeChannelDeliveryTarget(job->delivery_channel, job->delivery_account,
                                           job->delivery_conversation);
    request.binding_extra = nlohmann::json::object({
        {"jobId", job->job_id},
        {"occurrenceId", occurrence_id},
        {"channelId", job->delivery_channel},
        {"accountId", job->delivery_account},
        {"conversationId", job->delivery_conversation},
        {"senderId", job->owner_sender},
    });

    const std::string bind_occurrence_id = occurrence_id;
    std::atomic<bool> cancel_flag{claimed->cancel_requested};
    const auto result = executor_->ExecuteChannelTurn(
        request,
        [store, bind_occurrence_id, now_ms](const std::string& session_id,
                                            const std::string& turn_id) {
            // 领域绑定先于 V3 gateway.work.bound(恢复器优先走领域行)。
            (void)store->BindOccurrence(bind_occurrence_id, session_id, turn_id, now_ms);
        },
        &cancel_flag);
    if (!result.ok) {
        if (result.error_code == "gateway.fault_injected") {
            return true;  // 不结算:恢复路接管(模拟进程死在半路)
        }
        const bool cancelled = claimed->cancel_requested ||
                               result.error.find("cancelled") != std::string::npos;
        const std::string outcome = cancelled ? "cancelled" : "failed";
        return store->SettleOccurrence(occurrence_id, outcome,
                                       "turn_failed: " + result.error_code + ": " + result.error,
                                       now_ms);
    }
    // 结果投回创建会话(§11.3:绑定的账号/目标,不附陈旧 msg_id——锚在
    // 发送时按互动窗现取)。执行与投递分账:occurrence 落账 succeeded 是
    // "执行完成且已交投递账",投递态在 outbox(可查,不谎报)。
    gateway::DurableReplyOutbox::ChannelTarget target;
    target.channel_id = job->delivery_channel;
    target.account_id = job->delivery_account;
    target.conversation_id = job->delivery_conversation;
    target.reply_to_message_id = "";
    target.source_ref = "chanjob:" + job->delivery_channel + ":" + job->delivery_account + ":" +
                        job->job_id + ":" + occurrence_id;
    const auto enqueued = options_.outbox->EnqueueChannel(
        result.selection_id, result.reply_text, result.session_id, result.turn_id, target,
        now_ms);
    if (!enqueued.accepted && !enqueued.duplicate) {
        (void)store->SettleOccurrence(occurrence_id, "needs_review", "outbox_enqueue_failed",
                                      now_ms);
        *error = "outbox.append_failed: 渠道任务结果入不了箱";
        return false;  // outbox 账写不进:停泵
    }
    return store->SettleOccurrence(occurrence_id, "succeeded", "enqueued_to_channel_outbox",
                                   now_ms);
}

std::optional<std::string> ChannelWorkPump::FreshInboundAnchor(
    const gateway::ReplyOutboxItem& item, std::int64_t now_ms) const {
    std::optional<RecentInbound> recent;
    {
        const std::lock_guard<std::mutex> lock(recent_inbound_mutex_);
        const auto found = recent_inbound_.find(item.target_channel_id + "/" +
                                                item.target_account_id + "/" +
                                                item.target_conversation_id);
        if (found != recent_inbound_.end()) {
            recent = found->second;
        }
    }
    if (!recent.has_value()) {
        return std::nullopt;
    }
    if (now_ms - recent->received_at_ms > options_.passive_reply_window_ms ||
        now_ms < recent->received_at_ms) {
        return std::nullopt;  // 窗外(或时钟倒拨):不带陈旧锚,走主动消息
    }
    return recent->message_id;
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
        // 发送身份裁决(A05/A06):live 身份在账上 = 已冻结——结果未知的
        // 重试、重启后的重投恒复用同一 (锚, msg_seq),不再选锚。没有
        // live 身份才首次选锚并持久分配(账行 PowerLoss 先落,再上网络)。
        const auto live_identity =
            options_.outbox->FindLiveChannelSendIdentity(item.delivery_id);
        std::string send_anchor;
        std::uint32_t send_msg_seq = 0;
        if (live_identity.has_value()) {
            send_anchor = live_identity->anchor_msg_id;
            send_msg_seq = live_identity->msg_seq;
        } else {
            // 身份未冻结才选锚(A06):直聊段用入箱锚;chanjob 按"该会话
            // 最近一来信是否在窗内"现取,窗外 = 主动消息(不带陈旧锚)。
            send_anchor = item.target_reply_to_message_id;
            const bool is_chanjob = item.source_ref.rfind("chanjob:", 0) == 0;
            if (is_chanjob) {
                const auto fresh = FreshInboundAnchor(item, now_ms);
                if (!fresh.has_value()) {
                    if (await_interaction_.count(item.delivery_id) > 0) {
                        continue;  // 挂起等互动:没新来信不硬发
                    }
                    send_anchor.clear();  // 主动消息(不带陈旧锚)
                } else {
                    send_anchor = *fresh;
                    await_interaction_.erase(item.delivery_id);  // 补投解锁
                }
            }
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
        if (!live_identity.has_value()) {
            // 首次网络发送前持久冻结发送身份(A05):同 (账号,锚) 跨回复
            // 共用发号器——配对提示/任务回执/审批卡/正文分段/附件不撞号;
            // 重启后发号从账重放,不归零。
            const auto assigned = options_.outbox->AssignChannelSendIdentity(
                item.delivery_id, item.target_account_id, send_anchor, item.reply_sha256,
                now_ms);
            if (!assigned.ok) {
                if (assigned.error_code == "identity_anchor_conflict") {
                    // live 身份锚不一致(挂起补投的旧被动身份未裁决):先
                    // 裁决旧尝试(账里留迹),下一 tick 再分配新身份——
                    // 不偷换同一 delivery 的在途身份。
                    (void)options_.outbox->RetireChannelSendIdentity(
                        item.delivery_id, "anchor_superseded_for_reanchor", now_ms);
                } else if (assigned.error_code == "identity_payload_mismatch") {
                    (void)options_.outbox->MarkChannelFailed(
                        item.delivery_id, "identity_payload_mismatch", now_ms);
                } else {
                    return false;  // 账写不进:停泵(outbox 统一闸接管)
                }
                continue;
            }
            send_anchor = assigned.identity.anchor_msg_id;
            send_msg_seq = assigned.identity.msg_seq;
        }
        // 发出前记尝试(§七:先账后网络)。
        if (!options_.outbox->RecordAttempt(item.delivery_id, now_ms)) {
            continue;
        }
        channel::ChannelManager::ChannelSendRequest send;
        send.conversation_id = item.target_conversation_id;
        send.text = text;
        send.reply_to_message_id = send_anchor;
        send.client_delivery_id = item.delivery_id;
        send.msg_seq = send_msg_seq;
        // Q4:带附件的段(末段)把冻结引用递给渠道(适配器读原件上传)。
        if (!item.attachment_local_path.empty()) {
            channel::ChannelManager::OutboundAttachment attachment;
            attachment.local_path = item.attachment_local_path;
            attachment.file_name = item.attachment_file_name;
            attachment.mime_type = item.attachment_mime_type;
            attachment.size_bytes = item.attachment_size_bytes;
            send.attachment = std::move(attachment);
        }
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
