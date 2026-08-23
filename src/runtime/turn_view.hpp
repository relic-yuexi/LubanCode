// TurnView(终端回合视觉收束单):一轮问答的中立视图模型。
//
// 单子"新的显示模型"一节:不能继续给 TranscriptItem 加几个 bool 糊过去,
// 立中立模型——Turn/ModelStep/ToolBatch/TurnItemView 四层,外加整轮的
// metrics 与 turn 级活动态。Runtime/领域层产真值:不带 ANSI、不截终端宽、
// 不写"● Done"一类画面词(守门测试 test_app_boundary_gate 钉着,这只头
// 只认标准库与 nlohmann/json,零 cli/app 依赖)。
//
// 身份:turn/step/batch/item 的 id 全部走 IdAuthority 的号,与 event.hpp
// 的三层身份同一本发号账,不另起炉灶。step_id 复用 turn-<n> 发号局的批次
// 号段(batch 也一样),前端凭 id 对账,不靠次序猜。
//
// 谁产谁吃:SessionRuntime/装配层按事件流攒出 TurnView;TerminalTurnRenderer
// (cli 侧)把 TurnView 投影成 frame;Web/Tauri 可另投 DOM——同一份账,
// 两家不各抄一遍。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/turn_item.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 一轮的中立视图
// ---------------------------------------------------------------------------

// 条目种类(view 层):user(用户输入)/thinking/text/tool/warning/error。
// 与 ItemKind(事件层)同词,多出 User/Error 两枚视图态;Tool/SubTool 的
// 区分落在 parent_item_id(空 = main 工具,非空 = 挂在父条目下的内层)。
enum class TurnItemViewKind { User, Thinking, Text, Tool, Warning, Error };

// 条目状态:与 TurnItemStatus 同一套词,另加 Skipped——ESC 打断时"同批
// 尚未开跑"的那几枚(单子"工具批次"节:正在跑的记 Interrupted,同批
// 尚未开跑的记 Skipped,不能只给历史补合成 tool_result、屏上却少了那几枚)。
enum class TurnItemViewState { Pending, Running, Succeeded, Failed, Declined, Cancelled, Interrupted, Skipped };

// 一枚条目的视图条目:身份 + 归属 + 状态 + 标题/摘要的事实底子。
// title/summary 不拼画面文案——工具标题的"run_command(cmd)"、摘要的
// "Done · 退出码 0"都是终端投影(cli::BuildToolTitle 一族),这里只存
// 领域真值(kind/tool_name/input/result_text),前端自己措辞。
struct TurnItemView {
    std::string item_id;       // IdAuthority 发的 item-<n>(或既有条目的 id)
    std::string tool_use_id;   // 模型调用 id(工具条目;可空)
    std::string parent_item_id;  // 父条目(SubTool 挂 AgentItem、PTC stub 挂 PTC 条目;空 = 顶层)
    std::string step_id;       // 属于哪个 model step(空 = 不归 step 管,如 user/footer)
    std::string batch_id;      // 属于哪个 tool batch(空 = 不在批次里)
    std::uint64_t seq = 0;     // 条目在轮内的原序(事件到达序,保序用)
    TurnItemViewKind kind = TurnItemViewKind::Tool;
    TurnItemViewState status = TurnItemViewState::Running;
    std::string tool_name;     // 原始工具名(非工具条目可空/用 kind 名)
    nlohmann::json input = nlohmann::json::object();  // 结构化入参(真值)
    std::string result_text;   // 结果原文(终态才有)
    bool result_is_error = false;
    std::int64_t started_at_ms = 0;  // Unix epoch 毫秒
    std::int64_t ended_at_ms = 0;    // 0 = 还没终态
    std::optional<DiffTable> diff;   // edit_file/write_file 的中立行表(直吃现成)
};

// 一次模型响应 = 一个 model step。thinking/text/tool 条目都归它;下一次
// 模型请求回来另起一枚(单子:两批之间要留一口气,不画满宽横线)。
struct ModelStepView {
    std::string step_id;
    int index = 0;             // 0 起,轮内第几拍
    std::vector<std::string> item_ids;  // 本步条目(原序)
    bool finished = false;
};

// 工具批次:同一条 assistant message 里的连续 ToolUseBlock。执行仍串行
// (单子"不做"节:不偷偷并行);画面上一批多枚时先全登记 Pending,再按
// 次序 Running -> 终态。
struct ToolBatchView {
    std::string batch_id;
    std::string step_id;
    std::vector<std::string> ordered_item_ids;  // 批内次序(模型给的顺序)
    bool finished = false;
};

// 整轮收账:wall_duration 是起点(用户输入交账)到终点(footer 落笔)的
// 墙上耗时;approval_wait 单记。token 四项与 request_count 给详细态
// (Ctrl+O //usage),紧凑态只写总耗时(单子"统计降噪"节)。
struct TurnMetrics {
    std::int64_t wall_duration_ms = 0;    // 整轮墙钟(模型+工具+审批+重试)
    std::int64_t approval_wait_ms = 0;    // 审批等待单记(详细态可写)
    int request_count = 0;                // 本轮模型请求数
    int tool_count = 0;                   // 本轮工具调用数
    int failed_tool_count = 0;            // 失败(含拒绝/拦下)的工具数
    std::int64_t input_tokens = 0;        // 含 cache_read/creation(完整输入)
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t reasoning_tokens = 0;
};

// 一轮的完整视图:user 条目 + steps(内含 batches/items)+ 终态与 metrics。
struct TurnView {
    std::string turn_id;
    bool finished = false;                 // footer 是否已落(turn.completed)
    TurnItemViewState status = TurnItemViewState::Running;
    std::int64_t started_at_ms = 0;
    std::int64_t ended_at_ms = 0;
    std::string user_item_id;              // 用户输入条目(空 = 没建,如 slash 轮)
    std::vector<ModelStepView> steps;
    std::vector<ToolBatchView> batches;
    std::vector<TurnItemView> items;       // 轮内全部条目(含 user/footer 前的一切)
    TurnMetrics metrics;
};

// ---------------------------------------------------------------------------
// turn 级活动态(Working 活动条的真值)
// ---------------------------------------------------------------------------

// 活动条的生命周期认整个 turn,不认单次 HTTP/model request(单子第六节:
// 现有 SpinnerBackend 每次模型请求新起、首个 stream event 一到便停,一轮
// 里"模型 -> 工具 -> 模型"计时会消失重来)。Runtime/Session 持有,
// BottomChrome 只订阅快照并画。
enum class TurnActivityPhase {
    Idle,             // 没有 turn 在跑(终态落账后回这里)
    WaitingModel,     // 请求已发,等首字节
    Thinking,         // 思考增量在流
    RunningTool,      // 工具执行中
    WaitingApproval,  // 审批/ask_user 菜单占屏
    Stopping,         // cancel 已置,等终态落账
};

struct TurnActivityState {
    std::string turn_id;              // 空 = 没有 turn 在跑
    TurnActivityPhase phase = TurnActivityPhase::Idle;
    std::int64_t started_at_ms = 0;   // turn.started 那一刻(epoch ms;steady 钟由前端自持)
    std::int64_t approval_wait_started_at_ms = 0;  // 0 = 当前不在等审批
    std::int64_t approval_wait_accumulated_ms = 0; // 历次审批等待累计(结束一段就并进来)
    bool interrupt_requested = false; // ESC 已置 cancel,终态未落

    bool active() const { return !turn_id.empty() && phase != TurnActivityPhase::Idle; }
};

// 枚举 <-> 稳定字符串(线上表示是字符串;实现见 runtime_contract.cpp)。
std::string ToString(TurnItemViewKind kind);
std::string ToString(TurnItemViewState status);
std::string ToString(TurnActivityPhase phase);
bool ParseTurnItemViewKind(const std::string& s, TurnItemViewKind& out);
bool ParseTurnItemViewState(const std::string& s, TurnItemViewState& out);
bool ParseTurnActivityPhase(const std::string& s, TurnActivityPhase& out);

}  // namespace lubancode::runtime

// 画面文案(Worked for / Stopped after / Failed after、耗时人话、footer 横线)
// 是终端投影,住 cli/format_utils 与 cli/divider——runtime 只产毫秒与状态,
// 不写画面词(单子"Runtime/领域层产真值"的边界)。
