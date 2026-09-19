// 按代理状态投影单(TUI 主子代理视图彻底隔离 P1):视图身份与每页状态账
// 的纯模型。
//
// §四的拟议接口在本期落地:AgentViewKey(身份)、AgentUiState(每页交互
// 状态,切走保留切回还原)、AgentFrameSnapshot(main/sub 适配成同一份只读
// 显示合同)。住 cli 层的理由:消费方(转录控制器、终端渲染)在 cli,cli
// 不许反引 app;模型只认标准库与 runtime 的中立视图(TurnView),两层都
// 拿得到。
//
// 身份规矩(单子 §四.1):AgentViewKey = {session_generation, task_id}。
// main 也是合法成员——UI 边界把现有 task_id=0 映射进来,不另造号。会话
// 清空、恢复、任务号重用时,旧 generation 的事件不许污染新会话:所有页
// 状态账按 generation 分册,换代即整册作废。
//
// 事实来源(单子 §四.2):原始事件/SessionV3 仍是事实来源,这里的快照只
// 是缓存。快照带三枚修订号(内容/活动/统计)——订阅方先建立水位,再取
// 快照并接续事件;重复序号不重放,有缺口重新取快照。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/turn_view.hpp"

namespace lubancode::cli {

// ---------------------------------------------------------------------------
// 身份
// ---------------------------------------------------------------------------

// 一页会话的稳定身份。task_id=0 即 main(UI 边界映射,不另造 main 专用号);
// 其余按 AgentTool 任务号。session_generation 由会话侧的视图登记簿发号,
// /clear、/resume 换代——换代后旧键查不到旧账,旧 generation 的迟到事件
// 自然作废。
struct AgentViewKey {
    std::uint64_t session_generation = 0;
    int task_id = 0;  // 0 = main

    bool is_main() const { return task_id == 0; }

    // 供 unordered_map 的键:session_generation 与 task_id 各占半。
    std::uint64_t hash_value() const {
        return (session_generation << 32) ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(task_id));
    }
    bool operator==(const AgentViewKey& other) const {
        return session_generation == other.session_generation && task_id == other.task_id;
    }
    bool operator!=(const AgentViewKey& other) const { return !(*this == other); }
};

// UI 边界的便捷映射:查看任务号(0 = main)→ 键。generation 由登记簿侧
// 递进来;没有登记簿的场合(单测/演示)用 0。
inline AgentViewKey AgentViewKeyFor(std::uint64_t session_generation, int viewed_task_id) {
    return AgentViewKey{session_generation, viewed_task_id};
}

struct AgentViewKeyHash {
    std::size_t operator()(const AgentViewKey& key) const {
        return static_cast<std::size_t>(key.hash_value());
    }
};

// ---------------------------------------------------------------------------
// 帧令牌(单子 §五:切页事务与 FrameToken)
// ---------------------------------------------------------------------------

// 一帧的三重身份:这帧画的是哪页(AgentViewKey)、第几代换页(view_epoch,
// 登记簿每换一页 +1)、第几版布局(layout_revision,resize/Ctrl+L/Ctrl+O
// 这类整屏重排各 +1)。规矩:布局在锁外算好,写屏前在统一提交锁内再验
// 令牌——快速 A→B→A 时第一轮 A 的过期绘制(旧 epoch / 旧布局)不得
// 通过。令牌失配丢的只是画面指令,事件与账面照收(登记簿的收账/水位
// 合同不变)。
struct FrameToken {
    AgentViewKey view;
    std::uint64_t view_epoch = 0;
    std::uint64_t layout_revision = 0;

    bool operator==(const FrameToken& other) const {
        return view == other.view && view_epoch == other.view_epoch &&
               layout_revision == other.layout_revision;
    }
    bool operator!=(const FrameToken& other) const { return !(*this == other); }
};

// ---------------------------------------------------------------------------
// 每页交互状态(AgentUiState,单子 §四.2)
// ---------------------------------------------------------------------------

// 切走保留、切回还原的那本账:滚动锚点、是否跟随末尾、展开条目、草稿与
// 光标。数据侧(转录/工具/用量)不在册——那是 AgentViewState/台账的事;
// 这里只存"用户看到哪、敲到哪"。原始事件仍是事实来源,这本账丢了顶多
// 丢滚动位置与草稿,不丢对话。
struct AgentUiState {
    // 滚动:查看帧按"头几行 + 最近 N 行"裁尾,P2 收拢成完整滚动锚后这枚
    // 字段就是锚。0 = 未记(按默认跟随末尾处理)。
    int view_tail_rows = 0;
    bool follow_tail = true;  // 后台到消息不强迫用户跳回页尾(单子 §四.2)

    // 展开档:查看态 Ctrl+O(sub 页用 agent_view_expanded,main 页用最近
    // 条目档)。各页各记,切页不串档。
    bool view_expanded = false;    // sub 页:查看帧思考/正文尾巴的展开档
    bool expand_latest = false;    // main 页:空闲态 Ctrl+O 的最近一条档
    bool expand_all = false;       // main 页:全局紧凑/详细档(Ctrl+O 全展开)
    bool focus_view_active = false;  // main 页:Ctrl+E 聚焦查看是否开着

    // 草稿与光标(单子 §四.2:切走后保留,切回后还原)。busy 监听线程的
    // 排队 composer 在换页时存取;空闲 composer 的草稿绑定在 P3(输入与
    // 生命周期收口)统一收口,这里先把账立好。
    std::string draft_text;
    int draft_cursor_chars = 0;  // 草稿光标(码点数;0 = 行尾语义由加载方定)
};

// 会话级存取册:按 AgentViewKey 分册存 AgentUiState。进程内一只(槽位在
// console_input 装配),两处查看路径(流式监听线程、空闲 composer)共用。
// 线程约定:两处查看路径各自串行(监听线程/主线程),存取本身加小锁,
// 不依赖调用方。
class AgentUiStateStore {
public:
    // 存:整份覆盖(调用方在切走前把自己页的现值交来)。
    void Save(const AgentViewKey& key, AgentUiState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[key] = std::move(state);
    }
    // 取:没记过的页给默认值(跟随末尾、全收起、无草稿)。
    AgentUiState Load(const AgentViewKey& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = states_.find(key);
        return it != states_.end() ? it->second : AgentUiState{};
    }
    // 换代清册(/clear、/resume):旧 generation 的整册作废,任务号重用
    // 也不会串到旧草稿。
    void DropGeneration(std::uint64_t session_generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = states_.begin(); it != states_.end();) {
            if (it->first.session_generation == session_generation) {
                it = states_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 清册(会话收场/单测隔离)。
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.clear();
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return states_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<AgentViewKey, AgentUiState, AgentViewKeyHash> states_;
};

// ---------------------------------------------------------------------------
// 统一快照合同(AgentFrameSnapshot,单子 §四.2)
// ---------------------------------------------------------------------------

// main/sub 适配成同一份只读显示合同,供统一 renderer 消费(P2 收拢写者后
// 由会话级 UI 调度整帧取用;P1 先立合同,main 的活回合重铺已经吃它)。
// 快照不拥有任何锁、不带任何终端概念——纯数据。
//
// 修订号口径(单子 §四.2:稳定 item ID、内容 revision、活动与统计
// revision):
//   - content_revision:转录内容动一笔 +1(新条目、终态、正文/思考增量)。
//     订阅衔接的主水位:重铺后按它判定"哪些事件还没画"。
//   - activity_revision:活动态(运行/等审批/停止中)变动 +1。
//   - stats_revision:用量与统计变动 +1。
struct AgentFrameSnapshot {
    AgentViewKey key;
    std::uint64_t content_revision = 0;
    std::uint64_t activity_revision = 0;
    std::uint64_t stats_revision = 0;

    bool running = false;  // 这页的回合/任务是否在跑
    std::string title;     // 页头身份行用的短标题(main 留空,渲染层自定)

    // 转录投影:稳定 item ID 的条目序列(main 复用 TurnCollector 攒的
    // TurnView;sub 由任务事件适配,见 app 侧适配器)。条目状态、工具
    // 配对、用量都在这份视图里——统一 renderer 只认它。
    std::shared_ptr<const runtime::TurnView> turn;
};

// main 侧适配器:TurnCollector 的活视图 → 快照(纯搬运,不改一笔账)。
// revisions 由调用方(登记簿)递进来——登记簿才是修订号的发号处。
inline AgentFrameSnapshot AgentFrameSnapshotFromTurnView(const AgentViewKey& key,
                                                          std::shared_ptr<const runtime::TurnView> turn,
                                                          std::uint64_t content_revision,
                                                          std::uint64_t activity_revision,
                                                          std::uint64_t stats_revision, bool running) {
    AgentFrameSnapshot snapshot;
    snapshot.key = key;
    snapshot.content_revision = content_revision;
    snapshot.activity_revision = activity_revision;
    snapshot.stats_revision = stats_revision;
    snapshot.running = running;
    snapshot.turn = std::move(turn);
    return snapshot;
}

}  // namespace lubancode::cli
