// 排队消息的会话层账本(0.28.x"排队消息在工具边界送达并可 Shift+左键编辑")。
//
// 旧貌:待发队列是一只住在 TurnInputListener 线程里的 vector<string>,整轮
// Run() 结束才 TakeQueuedLines() 搬给交互循环,下一圈再当全新用户轮次逐条
// 发送。新貌:队列升到会话层,变成一只线程安全的 SteeringQueue——
//   - 每条消息有稳定 id、目标(main 会话 / 某只子代理任务)、送达策略与状态;
//   - listener 只提交编辑动作(落队/取回/替换/取消/删除),不拥有最终数据;
//   - main AgentLoop 在轮次边界按 MainSession 目标取件(规格第三步),子代理
//     目标由会话泵转投 AgentTool 的任务 inbox(SendTaskMessage 那套);
//   - UI(流式 footer 与空闲 composer)每次重画现拉轻量快照,不长期持锁。
//
// 纯逻辑、不认终端(单测直接钉,tests/test_queue_model.cpp);线程安全靠
// 内部一把互斥,所有公开方法都能跨线程调。
//
// 编辑事务带版本号(规格"数据与线程"节):BeginEdit 取出条目正文与版本,
// 冻结该条(edit_open)——投递泵见到冻结条目跳过,不会"一边送旧文、一边
// 显示已保存"。CommitEdit 只在版本未动时原位替换(保 id、目标、排队次序);
// 版本被动过(比如别的路已改写)返回 Conflict,调用方提示,不静默覆盖。
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "platform/console.hpp"  // KeyInput::Kind(取回键判定)

namespace lubancode::cli {

// -----------------------------------------------------------------------
// 取回键的判定(纯函数,单测钉)
// -----------------------------------------------------------------------

// Shift+Left/Ctrl+Left 这一下该不该触发"取回排队消息"。规矩(规格"进入
// 编辑态"一节):composer 为空、当前不在队列编辑态、队列里还有条目,三者
// 齐了才取。正文非空时 Shift+Left 保持 composer 既有的光标语义,不抢输入
// ——这条也顺带堵死 IME 组合期:组合中的半个词会先以正文形式落在 composer
// 里,正文空是取回的硬前提,组合没提交就永远取不走队列。
bool ShouldRecallQueuedMessage(bool composer_empty, bool editing, std::size_t queue_size);

// 这枚 platform 按键算不算"取回键"。Shift+Left 恒认;Ctrl+Left 是"终端
// 不报 Shift 修饰"时的备用键,可经环境变量 LUBANCODE_QUEUE_RECALL_FALLBACK
// (none/off/0/false)关掉,屏上提示按实际能力显示。
bool IsQueueRecallKey(platform::KeyInput::Kind kind);
// 备用键此刻开没开(环境变量现读,测试可直接改环境钉行为)。
bool QueueRecallFallbackEnabled();
// 屏上提示的取回键写法("按实际能力显示"):Windows 键事件恒带修饰键状态,
// 只写主键;POSIX 可能不报 Shift 修饰,备用键开着就连备用键一起写。
std::string QueueRecallHint();

// -----------------------------------------------------------------------
// 数据
// -----------------------------------------------------------------------

using QueueId = std::uint64_t;  // 0 = 无效 id

// 队列消息的收件目标(规格"队列按目标分账"节)。值语义,可比较。
struct MessageTarget {
    enum class Kind { MainSession, Subagent };
    Kind kind = Kind::MainSession;
    int task_id = 0;  // Subagent 时有效:AgentTool 任务号

    static MessageTarget Main() { return MessageTarget{}; }
    static MessageTarget Agent(int task_id) { return MessageTarget{Kind::Subagent, task_id}; }
    bool is_main() const { return kind == Kind::MainSession; }
    // 面板行/队列行显出的目标短名:"main" / "#3"(纯文本,不带 i18n——
    // 队列行的目标标签要短,两语言共这一份机器样短名)。
    std::string short_label() const;
};
bool operator==(const MessageTarget& a, const MessageTarget& b);
bool operator!=(const MessageTarget& a, const MessageTarget& b);

// 送达策略。默认等下一个工具边界;Esc"打断并立即送"会把整队列翻成
// Immediate(实际投递仍只发生在安全点:打断收场后的会话泵)。
enum class DeliveryMode { AfterNextToolBoundary, Immediate };

// 活队列里的条目状态。已送达的条目直接出队(不留在活队列里);TargetGone
// (子代理先结束,明确拒收、不改投 main)与 Failed(投递出错)留在原位、
// 标注原因,等用户取回改写或删除——不吞掉。
enum class QueueItemState { Queued, TargetGone, Failed };

struct QueuedMessage {
    QueueId id = 0;
    MessageTarget target;
    std::string text;
    DeliveryMode delivery = DeliveryMode::AfterNextToolBoundary;
    QueueItemState state = QueueItemState::Queued;
    std::string note;         // TargetGone/Failed 的原因说明(展示用)
    std::uint64_t version = 1;  // 编辑事务版本:每次成功改写 +1
    bool edit_open = false;   // 编辑事务开着:投递冻结(见文件头注释)
};

// -----------------------------------------------------------------------
// 会话层队列(线程安全)
// -----------------------------------------------------------------------

class SteeringQueue {
public:
    // 编辑事务的凭据。BeginEdit 交出一份;Commit/Cancel/Delete 只认凭据里
    // 的版本,版本对不上就是 Conflict——"编辑期间恰逢边界送达"要么冻结、
    // 要么提交失败提示,绝不一边送旧文一边显示已保存。
    struct EditHandle {
        QueueId id = 0;
        std::uint64_t version = 0;
        MessageTarget target;
        std::string text;  // 取出那一刻的原文(调用方装进编辑器)
    };
    enum class CommitStatus { Ok, Conflict, NotFound };

    // ---- 落队 / 查询 ----
    // 新消息入队(排队顺序 = 落队顺序)。空文本拒收,返回 0。
    QueueId Enqueue(MessageTarget target, std::string text);
    // 轻量快照(锁内拷一份)。UI 每帧现拉,不长期持锁。
    std::vector<QueuedMessage> Snapshot() const;
    bool empty() const;
    std::size_t size() const;
    // 可取回编辑的条目数(Queued/TargetGone/Failed 都算;投递冻结中的不算
    // ——它已经有一只编辑事务开着)。
    std::size_t editable_size() const;

    // ---- 投递 ----
    // 取走该目标的可投递条目(状态 Queued、非编辑冻结),按落队顺序;取走
    // 即出队(视为已送达,不再留在活队列)。跨目标互不影响。
    std::vector<QueuedMessage> TakeDeliverable(MessageTarget target);
    // 只取队头一条(会话泵"一条一条自动发送"用;一次边界多条的批量注入
    // 走 TakeDeliverable)。没有可投递的给 nullopt。
    std::optional<QueuedMessage> TakeFirstDeliverable(MessageTarget target);
    // 只问有没有,不动账(判"有没有可等的事"用)。
    bool HasDeliverable(MessageTarget target) const;

    // ---- 编辑事务 ----
    // 取最新一条可编辑的(TargetGone/Failed 也给取:用户要改目标或删掉)。
    std::optional<EditHandle> BeginEditLatest();
    std::optional<EditHandle> BeginEdit(QueueId id);
    // 原位替换:保 id、目标、排队次序;版本对得上才成。
    CommitStatus CommitEdit(const EditHandle& handle, std::string new_text);
    // 放弃修改:条目按原文留队,解冻。
    CommitStatus CancelEdit(const EditHandle& handle);
    // 删掉凭据所指条目(编辑事务里的 Del;两段确认在调用方)。
    CommitStatus DeleteMessage(const EditHandle& handle);
    // 按 id 直接删(会话泵把子代理目标转投任务 inbox 成功后清账用)。
    bool Remove(QueueId id);

    // ---- 标注 / 状态 ----
    void MarkTargetGone(QueueId id, std::string note);  // 子代理终态拒收
    void MarkFailed(QueueId id, std::string note);      // 投递出错,留原位标错

    // Esc"打断并立即送":翻状态旗(标题跟着变"正在打断并送达"),投递仍由
    // 会话泵在安全点执行。全部送空后 ClearImmediateDelivery。
    void RequestImmediateDelivery();
    bool immediate_delivery_requested() const;
    void ClearImmediateDelivery();

    // 会话收场(/clear、退出):未送明细一次交出并清空——"明确丢弃、不静默
    // 消失"的最低限,打印处置由调用方负责。
    std::vector<QueuedMessage> TakeAllForDisposal();

private:
    mutable std::mutex mutex_;
    std::vector<QueuedMessage> items_;
    QueueId next_id_ = 1;
    bool immediate_ = false;
};

// 进程内唯一的交互会话实例。一次进程只有一场交互会话,账本挂在这里,
// console_input(footer/空闲 composer)与 app 层(投递泵)都从这儿取;
// /clear 与会话析构时 TakeAllForDisposal 清账。单测自建局部 SteeringQueue,
// 不碰这份全局。
SteeringQueue& SessionSteeringQueue();

// -----------------------------------------------------------------------
// 显示(纯函数)
// -----------------------------------------------------------------------

// 标题模式(规格"标题说清送达时机与操作"节:标题随状态变,不挂假话)。
enum class QueueTitleMode {
    Boundary,    // 正在等下一个工具边界(流式期间的缺省)
    EndOfTurn,   // 没有工具边界可等(空闲 composer 视角:本轮收尾后送出)
    Immediate,   // Esc 已按下,正在打断并送达
    Editing,     // 有一条正被取回编辑
};

struct QueueViewOptions {
    std::size_t visible_cap = 3;         // 逐条摆的上限,超出加"另有 N 条"
    QueueTitleMode title_mode = QueueTitleMode::Boundary;
    std::string key_hint;                // 取回键提示("Shift+←" 等;空串 = 不带键提示段)
};

// 队列区成行:空队列没有行(连标题都不画);非空时标题一行 + 条目行
// (超上限先加一行"另有 N 条",围着正在编辑的条目开窗)。条目行带目标
// 短名(子代理才带 "[#3] ")、编辑中/目标已结束/发送失败标记,正文只摆
// 首行(全文取回编辑器里看)。i18n 成对。
std::vector<std::string> BuildSteeringQueueRows(const std::vector<QueuedMessage>& items,
                                                const QueueViewOptions& options);

}  // namespace lubancode::cli
