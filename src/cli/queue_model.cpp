// 会话层排队队列(SteeringQueue)与队列区成行(BuildSteeringQueueRows)。
// 设计说明见 queue_model.hpp 文件头;键位/编辑事务/窗口化的规则全在这里,
// 不认终端、不认线程,单测钉在 tests/unit/agent/test_queue_model.cpp。

#include "cli/queue_model.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8 等纯函数(标题不走宽度,这里其实只借声明)
#include "platform/paths.hpp"   // GetEnvVar

namespace lubancode::cli {

// -----------------------------------------------------------------------
// 取回键判定
// -----------------------------------------------------------------------

bool ShouldRecallQueuedMessage(bool composer_empty, bool editing, std::size_t queue_size) {
    return composer_empty && !editing && queue_size > 0;
}

bool QueueRecallFallbackEnabled() {
    const auto value = lubancode::platform::GetEnvVar("LUBANCODE_QUEUE_RECALL_FALLBACK");
    if (!value.has_value()) {
        return true;  // 默认开:终端不报 Shift 修饰时至少有 Ctrl+Left 可用
    }
    std::string normalized = *value;
    for (char& c : normalized) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return !(normalized == "none" || normalized == "off" || normalized == "0" || normalized == "false");
}

bool IsQueueRecallKey(platform::KeyInput::Kind kind) {
    using K = platform::KeyInput::Kind;
    if (kind == K::ShiftLeft) {
        return true;
    }
    return kind == K::CtrlLeft && QueueRecallFallbackEnabled();
}

// 屏上提示的取回键写法(规格:按实际能力显示)。Windows 的键事件恒带修饰
// 键状态,Shift+← 稳;POSIX 有终端不报 Shift 修饰,备用键 Ctrl+← 一并写上
// ——备用键被环境变量关掉时只写主键。
std::string QueueRecallHint() {
    const bool with_fallback =
#ifdef _WIN32
        false;  // VK_LEFT+SHIFT_PRESSED 恒可判,不必推备用键
#else
        QueueRecallFallbackEnabled();
#endif
    return tr(with_fallback ? "queue.key_hint_fallback" : "queue.key_hint");
}

// -----------------------------------------------------------------------
// MessageTarget
// -----------------------------------------------------------------------

bool operator==(const MessageTarget& a, const MessageTarget& b) {
    return a.kind == b.kind && (a.kind != MessageTarget::Kind::Subagent || a.task_id == b.task_id);
}

bool operator!=(const MessageTarget& a, const MessageTarget& b) { return !(a == b); }

std::string MessageTarget::short_label() const {
    if (kind == Kind::Subagent) {
        return "#" + std::to_string(task_id);
    }
    return "main";
}

// -----------------------------------------------------------------------
// SteeringQueue
// -----------------------------------------------------------------------

QueueId SteeringQueue::Enqueue(MessageTarget target, std::string text) {
    if (text.empty()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const QueueId id = next_id_++;
    QueuedMessage item;
    item.id = id;
    item.target = target;
    item.text = std::move(text);
    item.delivery = immediate_ ? DeliveryMode::Immediate : DeliveryMode::AfterNextToolBoundary;
    items_.push_back(std::move(item));
    return id;
}

bool SteeringQueue::RestoreFromArchive(std::vector<QueuedMessage> items) {
    if (items.empty()) {
        return true;  // 存档里没有排队账,空手而归不算失败
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!items_.empty()) {
        return false;  // 本场已有自己的账,不拿旧档盖
    }
    QueueId max_id = 0;
    for (auto& item : items) {
        max_id = std::max(max_id, item.id);
        // 回还过的条目(attempts>0)照旧不自动重发:存档记的是"等用户处置"
        // 的状态,resume 不该偷偷把死循环闸拆了。
        items_.push_back(std::move(item));
    }
    next_id_ = max_id + 1;  // 后续新条目不与恢复的 id 撞号
    return true;
}

std::vector<QueuedMessage> SteeringQueue::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

bool SteeringQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
}

std::size_t SteeringQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

std::size_t SteeringQueue::editable_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(items_.begin(), items_.end(), [](const QueuedMessage& item) { return !item.edit_open; }));
}

std::vector<QueuedMessage> SteeringQueue::TakeDeliverable(MessageTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QueuedMessage> out;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->target == target && it->state == QueueItemState::Queued && !it->edit_open) {
            out.push_back(std::move(*it));
            it = items_.erase(it);
            continue;
        }
        ++it;
    }
    return out;
}

std::optional<QueuedMessage> SteeringQueue::TakeFirstDeliverable(MessageTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->target == target && it->state == QueueItemState::Queued && !it->edit_open) {
            std::optional<QueuedMessage> out = std::move(*it);
            items_.erase(it);
            return out;
        }
    }
    return std::nullopt;
}

void SteeringQueue::ReturnToFront(QueuedMessage item) {
    std::lock_guard<std::mutex> lock(mutex_);
    item.delivery_attempts += 1;
    // 状态归位:自动发送失败不是条目本身的病(TargetGone/Failed 才是),
    // 还回来的还是健康的 Queued,只是带上了"已试过一次"的账。
    item.state = QueueItemState::Queued;
    item.edit_open = false;
    item.delivery = immediate_ ? DeliveryMode::Immediate : DeliveryMode::AfterNextToolBoundary;
    items_.insert(items_.begin(), std::move(item));
}

std::optional<QueuedMessage> SteeringQueue::TakeFirstAutoSendable(MessageTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->target != target || it->state != QueueItemState::Queued || it->edit_open) {
            continue;
        }
        if (it->delivery_attempts >= kMaxAutoSendAttempts) {
            continue;  // 已试过一次、失败还回来的:不自动重发,留队等人
        }
        std::optional<QueuedMessage> out = std::move(*it);
        items_.erase(it);
        return out;
    }
    return std::nullopt;
}

bool SteeringQueue::HasDeliverable(MessageTarget target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(items_.begin(), items_.end(), [&](const QueuedMessage& item) {
        return item.target == target && item.state == QueueItemState::Queued && !item.edit_open;
    });
}

bool SteeringQueue::HasAnyDeliverable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(items_.begin(), items_.end(),
                       [](const QueuedMessage& item) {
                           return item.state == QueueItemState::Queued && !item.edit_open;
                       });
}

std::optional<SteeringQueue::EditHandle> SteeringQueue::BeginEditLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (!it->edit_open) {
            it->edit_open = true;
            EditHandle handle;
            handle.id = it->id;
            handle.version = it->version;
            handle.target = it->target;
            handle.text = it->text;
            return handle;
        }
    }
    return std::nullopt;
}

std::optional<SteeringQueue::EditHandle> SteeringQueue::BeginEdit(QueueId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.id == id && !item.edit_open) {
            item.edit_open = true;
            EditHandle handle;
            handle.id = item.id;
            handle.version = item.version;
            handle.target = item.target;
            handle.text = item.text;
            return handle;
        }
    }
    return std::nullopt;
}

SteeringQueue::CommitStatus SteeringQueue::CommitEdit(const EditHandle& handle, std::string new_text) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.id != handle.id) {
            continue;
        }
        // 版本对不上 = 取出之后账被人动过(别的编辑事务已改写)。规格钉死:
        // 不可一边送旧文一边显示已保存——提交失败,调用方提示。
        if (!item.edit_open || item.version != handle.version) {
            return CommitStatus::Conflict;
        }
        item.text = std::move(new_text);
        ++item.version;
        item.edit_open = false;
        // 用户亲手改写过的就是一条新话:自动重试的旧账翻篇,终态标注一并
        // 作废(取走即消费单——不然失败/目标已结束的条目改完也永远不投
        // 递,死在队里)。目标本身没改(改目标是另一码事,面板处置键的事)。
        item.delivery_attempts = 0;
        item.state = QueueItemState::Queued;
        item.note.clear();
        return CommitStatus::Ok;
    }
    return CommitStatus::NotFound;
}

SteeringQueue::CommitStatus SteeringQueue::CancelEdit(const EditHandle& handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.id != handle.id) {
            continue;
        }
        if (!item.edit_open) {
            return CommitStatus::Conflict;
        }
        item.edit_open = false;  // 原文从未被动过,解冻即还原
        return CommitStatus::Ok;
    }
    return CommitStatus::NotFound;
}

SteeringQueue::CommitStatus SteeringQueue::DeleteMessage(const EditHandle& handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id != handle.id) {
            continue;
        }
        if (!it->edit_open) {
            return CommitStatus::Conflict;
        }
        items_.erase(it);
        return CommitStatus::Ok;
    }
    return CommitStatus::NotFound;
}

bool SteeringQueue::Remove(QueueId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id == id) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

void SteeringQueue::MarkTargetGone(QueueId id, std::string note) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.id == id) {
            item.state = QueueItemState::TargetGone;
            item.note = std::move(note);
            return;
        }
    }
}

void SteeringQueue::MarkFailed(QueueId id, std::string note) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.id == id) {
            item.state = QueueItemState::Failed;
            item.note = std::move(note);
            return;
        }
    }
}

void SteeringQueue::RequestImmediateDelivery() {
    std::lock_guard<std::mutex> lock(mutex_);
    immediate_ = true;
    for (auto& item : items_) {
        if (item.state == QueueItemState::Queued) {
            item.delivery = DeliveryMode::Immediate;
        }
    }
}

bool SteeringQueue::immediate_delivery_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return immediate_;
}

void SteeringQueue::ClearImmediateDelivery() {
    std::lock_guard<std::mutex> lock(mutex_);
    immediate_ = false;
}

std::vector<QueuedMessage> SteeringQueue::TakeAllForDisposal() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QueuedMessage> out = std::move(items_);
    items_.clear();
    immediate_ = false;
    return out;
}

SteeringQueue& SessionSteeringQueue() {
    static SteeringQueue queue;
    return queue;
}

// -----------------------------------------------------------------------
// 显示成行
// -----------------------------------------------------------------------

namespace {

// 正文首行:截到第一个换行;多行正文尾巴补一枚省略号(全文在取回编辑器里
// 看,队列区只摆首行——可折叠,不丢数据)。
std::string FirstLineOf(const std::string& text) {
    const std::size_t cut = text.find('\n');
    std::string line = text.substr(0, cut);
    for (char& c : line) {
        if (c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    if (cut != std::string::npos) {
        line += "\xe2\x80\xa6";  // "…"
    }
    return line;
}

}  // namespace

std::vector<std::string> BuildSteeringQueueRows(const std::vector<QueuedMessage>& items,
                                                const QueueViewOptions& options) {
    std::vector<std::string> rows;
    if (items.empty()) {
        return rows;  // 规格明确:没队列连标题都不画
    }
    const std::string key_hint = options.key_hint.empty() ? QueueRecallHint() : options.key_hint;
    switch (options.title_mode) {
        case QueueTitleMode::Boundary:
            rows.push_back(trf("queue.title.boundary", key_hint));
            break;
        case QueueTitleMode::EndOfTurn:
            rows.push_back(trf("queue.title.end_of_turn", key_hint));
            break;
        case QueueTitleMode::Immediate:
            rows.push_back(tr("queue.title.immediate"));
            break;
        case QueueTitleMode::Editing:
            rows.push_back(tr("queue.title.editing"));
            break;
    }

    const std::size_t count = items.size();
    const std::size_t cap = options.visible_cap == 0 ? count : options.visible_cap;
    std::size_t first = 0;
    std::size_t visible = count;
    if (count > cap) {
        visible = cap;
        // 超上限:围着正在编辑的条目开窗(没在编辑就摆最新的 cap 条)。
        std::size_t edit_index = count;  // "无"
        for (std::size_t i = 0; i < count; ++i) {
            if (items[i].edit_open) {
                edit_index = i;
                break;
            }
        }
        if (edit_index < count) {
            first = edit_index > cap / 2 ? edit_index - cap / 2 : 0;
            if (first + visible > count) {
                first = count - visible;
            }
        } else {
            first = count - visible;
        }
        rows.push_back(trf("input.queue_more", count - visible));
    }

    for (std::size_t i = first; i < first + visible; ++i) {
        const QueuedMessage& item = items[i];
        std::string prefix;
        switch (item.state) {
            case QueueItemState::TargetGone:
                prefix += tr("queue.mark.target_gone");
                break;
            case QueueItemState::Failed:
                prefix += tr("queue.mark.failed");
                break;
            case QueueItemState::Queued:
                break;
        }
        if (!item.target.is_main()) {
            prefix += trf("queue.mark.target", item.target.task_id);
        }
        if (item.edit_open) {
            prefix += tr("queue.mark.editing");
        }
        rows.push_back("  \xE2\x86\xB3 " + prefix + FirstLineOf(item.text));  // "  ↳ "
    }
    return rows;
}

// ---------------------------------------------------------------------------
// 清账告知成行(路径三)
// ---------------------------------------------------------------------------

namespace {

// 首条预览行:剥前导空白、截到第一个换行(全文用户自己排的,认得)。
std::string DisposalPreviewRow(const QueuedMessage& item) {
    std::string first_line = item.text;
    first_line.erase(0, first_line.find_first_not_of("\r\n\t "));
    const std::size_t cut = first_line.find('\n');
    if (cut != std::string::npos) {
        first_line.resize(cut);
    }
    return trf("queue.disposal_preview", item.target.short_label(), first_line);
}

}  // namespace

std::vector<std::string> BuildQueueDisposalRows(const std::vector<QueuedMessage>& discarded) {
    std::vector<std::string> rows;
    if (discarded.empty()) {
        return rows;  // 没倒掉东西就不吭声,与旧"空队列零输出"一致
    }
    rows.push_back(trf("queue.disposal_head", discarded.size()));
    rows.push_back(DisposalPreviewRow(discarded.front()));
    return rows;
}

std::vector<std::string> BuildQueueArchiveRows(const std::vector<QueuedMessage>& queued) {
    std::vector<std::string> rows;
    if (queued.empty()) {
        return rows;
    }
    rows.push_back(trf("queue.archive_head", queued.size()));
    rows.push_back(DisposalPreviewRow(queued.front()));
    return rows;
}

}  // namespace lubancode::cli
