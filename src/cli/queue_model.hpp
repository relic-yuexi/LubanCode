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
// 纯逻辑、不认终端(单测直接钉,tests/unit/agent/test_queue_model.cpp);线程安全靠
// 内部一把互斥,所有公开方法都能跨线程调。
//
// 编辑事务带版本号(规格"数据与线程"节):BeginEdit 取出条目正文与版本,
// 冻结该条(edit_open)——投递泵见到冻结条目跳过,不会"一边送旧文、一边
// 显示已保存"。CommitEdit 只在版本未动时原位替换(保 id、目标、排队次序);
// 版本被动过(比如别的路已改写)返回 Conflict,调用方提示,不静默覆盖。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cli/slash_commands.hpp"  // SlashCommand(忙碌排队白名单的判料)
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
    // slash 身份不落字段:忙碌期排队的完整斜杠命令,身份每次从正文现折
    // (IsQueuedSlashText,判法与 ProcessLine 同一颗 ParseSlashCommand)。
    // 好处是永远不漂——Shift+← 取回改写、存档落盘、resume 灌回,身份天然
    // 跟着正文走;要真存一枚旗子,编辑改写/存档恢复处处都得同步,漏一处
    // 就是"字段说普通文、正文是命令"的两本账(问题二:忙碌期排队的
    // /context 被当成普通消息送模型)。
    // 自动发送失败回还账(取走即消费单):会话泵把这条拿去自动发送、那轮
    // 却以请求失败收场时,ReturnToFront 原样还回队首并翻开这位。已试过一次
    // 的条目不再自动重发(防死循环):会话泵见它跳过,留队等用户手动处置
    // (Shift+← 取回改写再排,或删掉)。用户自己手动重排不算——那是新条目。
    int delivery_attempts = 0;
};

// -----------------------------------------------------------------------
// 会话层队列(线程安全)
// -----------------------------------------------------------------------

// 状态可见变化的观察口(P0-4 轨迹接线,§5.5 control.queue.item.*):
//   Enqueued   新条目落队(监听线程在忙,cli 层够不着账本——从这广播);
//   UserRemoved 用户删除(编辑事务里的 Del;终态安全,进 cancelled)。
// 取走类操作(TakeDeliverable 一族)与 Remove 故意不广播:投递成败在
// 调用方手上,dequeued 事件由调用方在落锤点(注入请求成功/转投 inbox
// 成功之后)直接进账本——取走又退还(ReturnToFront)的窗口里发 dequeued
// 会造出"账已终态、队里还躺着"的两本账。
enum class QueueChangeKind { Enqueued, UserRemoved };
using QueueChangeObserver = std::function<void(QueueChangeKind kind, const QueuedMessage& item)>;

class SteeringQueue {
public:
    // 自动发送的重试上限(取走即消费单):同一条消息最多自动送 kMax-
    // AutoSendAttempts 次(首发 + 失败回还后的那一次重试),再失败留队列
    // 等用户手动处置——自动重发无穷尽就是死循环。
    static constexpr int kMaxAutoSendAttempts = 2;

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
    // P0-4 轨迹观察口:enqueue/user_removed 两类终态安全的变化从这广播
    //(锁外回调,观察器不得回调本队列)。置空摘除。
    void SetChangeObserver(QueueChangeObserver observer);
    // resume 重建队列用(会话存档 queue 事件行):带着原 id/正文/目标/尝试
    // 次数整条放回,保留存档里的排队次序。只在会话起头(队列还空着)整批
    // 灌;id 撞了或队列非空就不收——运行中的队列只归运行中的账本管。
    bool RestoreFromArchive(std::vector<QueuedMessage> items);
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
    // 问题二(忙碌期排队的 slash 被当普通消息送模型):slash 条目(IsQueued-
    // SlashText)在这里一律让路——不随工具边界进模型,留在队列等轮末会话泵
    // 经 ProcessLine 本地执行。普通文字照旧按落队顺序取走。
    std::vector<QueuedMessage> TakeDeliverable(MessageTarget target);
    // 只取队头一条(一次边界只送一条的场合;批量注入走 TakeDeliverable)。
    // 没有可投递的给 nullopt。slash 让路规矩与 TakeDeliverable 同款。
    std::optional<QueuedMessage> TakeFirstDeliverable(MessageTarget target);
    // 出路二的失败退还(取走即消费单):TakeFirstDeliverable 拿去自动发送
    // 的那条,若那轮以请求失败收场,从这里塞回队首(attempts +1),原 id、
    // 原状态都保住。队列在取走与还回之间又进了新条目也不碍事——塞在最前,
    // 重发时它还是头一条。
    void ReturnToFront(QueuedMessage item);
    // 会话泵的防死循环闸:队头这条还该不该自动发(状态健康、没冻、没超
    // 自动重试上限)。attempts 满了的那条跳过,泵往后找——找不着就轮空,
    // 队列留给用户。
    std::optional<QueuedMessage> TakeFirstAutoSendable(MessageTarget target);
    // 只问有没有,不动账(判"有没有可等的事"用)。
    bool HasDeliverable(MessageTarget target) const;
    // 任意目标还有没有可投递的(Esc"打断并立即送"要不要翻旗用)。
    bool HasAnyDeliverable() const;

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
    QueueChangeObserver observer_;  // 锁外拷出再调,防死锁
};

// 进程内唯一的交互会话实例。一次进程只有一场交互会话,账本挂在这里,
// console_input(footer/空闲 composer)与 app 层(投递泵)都从这儿取;
// /clear 与会话析构时 TakeAllForDisposal 清账。单测自建局部 SteeringQueue,
// 不碰这份全局。
SteeringQueue& SessionSteeringQueue();

// P0-4 轨迹账的条目 id("q-<n>"):排队消息在 control.queue.item.* 事件里
// 的稳定身份(item_id 与 input_id 同用这一枚——排队消息本身就是输入身份)。
inline std::string QueueItemId(QueueId id) { return "q-" + std::to_string(id); }

// ---------------------------------------------------------------------------
// 忙碌期排队的 slash 命令(问题二:排队的 /context 被包成 [用户排队消息]
// 直接送模型,轮末 ProcessLine 再也看不见它)
//
// 三层规矩:
//   1. 身份保留:完整斜杠命令入队即认 slash,身份由正文自带(见
//      QueuedMessage 的注释),不降成普通文本;
//   2. 工具边界让路:TakeDeliverable 见 slash 跳过——不注入模型,留在
//      队列;轮末会话泵(TakeFirstAutoSendable → ProcessLine)本地执行
//      (打开 /context 的本地面板那类)。普通排队文字照旧在工具边界作
//      steering 送模型;
//   3. 提交门:不支持忙碌排队的命令提交时就明说拒绝入队(QueueText-
//      AdmittedDuringBusy),不悄悄降级。
// ---------------------------------------------------------------------------

// 这行排队正文是不是完整斜杠命令。判法与 ProcessLine(interactive_
// session.cpp:501-514)同源:ParseSlashCommand 认它不是 NotSlash 就是
// (含 Unknown——轮末分派器自会打"不认得"/查 workflow alias,不是发
// 模型,队列层不多嘴)。以 `/` 起头的整段都算:命令词认不认得都先按
// slash 的归宿走,与空闲时敲同一串字的归宿一致(不过忙碌提交门会把
// Unknown 拒在入队之前,见下)。
bool IsQueuedSlashText(const std::string& text);

// 忙碌期 slash 排队白名单(口径审过全部 51 案后定):
//   放行 = 「轮末本地执行无害」——只读本地面板/清单类(help/context/
//   todos/skills/mcp/lsp/plugins/tools/background/trace/config/package/
//   evolve/agents/sessions:只打印,不动状态)与会话内维护类(compact/
//   think/title/copy:效果即时可见、可逆,不换会话、不删档、不写项目
//   文件、不发网络请求)。
//   拒绝 = 其余全部,按病根分三类——
//   - 菜单/向导类(model/provider/language/keymap/record/peers/send/
//     peerperm 等):轮末自动弹出 ReadLine 选择器,用户不在场;
//   - 换场/毁档类(clear/exit/archive/delete/resume/worktree/init/
//     update/export/prompt/soul/skill/plugin/memory/hooks/doctor 等):
//     一条排队的旧命令悄悄改掉会话去向或写盘,用户早忘了排过它;
//   - 起工作/发模型类(plan/goal/loop/workflow/image 等):那是排一轮
//     活,不是本地命令,想续话排普通文字即可。Unknown(含 workflow
//     alias)一律不排:不认得的命令不进队列。
// 默认从严:新命令不进这张表就先拒,审明白再放。
bool SlashCommandQueueableDuringBusy(SlashCommand command);

// 忙碌期提交门:这行正文可不可排进队列(拦截点在流式 footer 的 Enter
// 与排队条目编辑的 CommitEdit)。普通文字恒放行;slash 须同时满足
// 「目标是 main」(本地命令没有子代理可执行,递给代理只会变成喂它的
// 一行字)与「命令在白名单内」。拒绝时调用方明说原因,正文留在
// composer 供当场改写。
bool QueueTextAdmittedDuringBusy(const std::string& text, MessageTarget target);

// -----------------------------------------------------------------------
// 清账告知(取走即消费单路径三:淡字换醒目)
// -----------------------------------------------------------------------

// /clear 与退场倒队列时的成行:标题一行(条数 + 首条预览截一行),醒目色
// 由调用方包(theme 那层不进纯逻辑)。空清账给空表,一行不打。
std::vector<std::string> BuildQueueDisposalRows(const std::vector<QueuedMessage>& discarded);

// 退场(不清账)时的成行:条数 + 首条预览 + "随存档带走,resume 接得回"。
// 空队列给空表。
std::vector<std::string> BuildQueueArchiveRows(const std::vector<QueuedMessage>& queued);

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
