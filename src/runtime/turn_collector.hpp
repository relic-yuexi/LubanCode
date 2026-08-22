// TurnCollector(终端回合视觉收束单):把一轮的事件攒成 TurnView。
//
// 落地次序第 2/3 步的 runtime 半边:AgentLoop 的 step/batch 边界回调与
// item 生命周期回调喂进来,这边只记账——发号、归组、算 metrics,不碰
// 画面(零 cli/app 依赖,守门测试钉着)。终端的 TerminalTurnRenderer 拿
// 攒好的 TurnView 排版;app-server/Web/Tauri 拿同一份。
//
// 与 TurnEventAdapter 的分工:那只把回调翻成 ServerEvent 流(事件出口,
// 给远端前端);这只把同样的回调攒成内存里的 TurnView(视图模型,给要
// "整轮重画"的终端:Crtl+L、resume、查看态回来都要一份完整账)。两家吃
// 同一批 id(都从 IdAuthority 发),不各猜批次。

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_item.hpp"
#include "runtime/turn_view.hpp"

namespace lubancode::runtime {

// 一轮一只。线程约定与 AgentLoop 回调一致:全部在 Run() 所在线程被调,
// 不加锁(装配层若要跨线程读,自己在外层加)。
class TurnCollector {
public:
    TurnCollector(IdAuthority& ids, std::string turn_id)
        : ids_(ids), turn_id_(std::move(turn_id)) {}

    TurnCollector(const TurnCollector&) = delete;
    TurnCollector& operator=(const TurnCollector&) = delete;

    const std::string& turn_id() const { return turn_id_; }
    const TurnView& view() const { return view_; }
    TurnView take_view() { return std::move(view_); }

    // 轮起:user 条目落账(用户输入提交后、交给 turn runtime 那一刻)。
    // 返回 user 条目的 item_id。slash/本地校验失败没发模型的轮不调它。
    std::string StartTurn(const std::string& user_text, std::int64_t started_at_ms) {
        view_ = TurnView{};
        view_.turn_id = turn_id_;
        view_.started_at_ms = started_at_ms;
        view_.status = TurnItemViewState::Running;
        if (!user_text.empty()) {
            TurnItemView user;
            user.item_id = ids_.NextItemId();
            user.seq = next_seq_++;
            user.kind = TurnItemViewKind::User;
            user.status = TurnItemViewState::Succeeded;
            user.started_at_ms = started_at_ms;
            user.ended_at_ms = started_at_ms;
            user.result_text = user_text;
            view_.user_item_id = user.item_id;
            view_.items.push_back(std::move(user));
        }
        return view_.user_item_id;
    }

    // step 边界(接 on_model_step_started):新起一枚 ModelStepView;此后
    // 的 thinking/text/tool 条目都归它,直到下一枚 step。
    void OnModelStepStarted(int step_index) {
        current_step_id_ = "step-" + std::to_string(step_index);
        ModelStepView step;
        step.step_id = current_step_id_;
        step.index = step_index;
        view_.steps.push_back(std::move(step));
    }

    // batch 边界(接 on_tool_batch_started):登记批次;ordered_tool_use_ids
    // 里的工具条目还没 start——先把"占位条目"立起来(Pending 态),单子
    // 要求:一批多枚时先全登记,再逐枚 Running -> 终态,用户一眼能看出
    // "模型这拍打算跑三件"。batch_id 复用发号局。
    void OnToolBatchStarted(int step_index, int batch_index, const std::vector<std::string>& ordered_tool_use_ids) {
        const std::string batch_id = "batch-" + std::to_string(batch_index);
        current_batch_id_ = batch_id;
        ToolBatchView batch;
        batch.batch_id = batch_id;
        batch.step_id = current_step_id_.empty() ? "step-" + std::to_string(step_index) : current_step_id_;
        for (const std::string& tool_use_id : ordered_tool_use_ids) {
            TurnItemView item;
            item.item_id = ids_.NextItemId();
            item.tool_use_id = tool_use_id;
            item.parent_item_id = parent_stack_.empty() ? std::string() : parent_stack_.back();
            item.step_id = batch.step_id;
            item.batch_id = batch_id;
            item.seq = next_seq_++;
            item.kind = TurnItemViewKind::Tool;
            item.status = TurnItemViewState::Pending;
            item.started_at_ms = NowMs();
            const std::string item_id = item.item_id;
            batch.ordered_item_ids.push_back(item_id);
            if (!view_.steps.empty()) {
                view_.steps.back().item_ids.push_back(item_id);
            }
            view_.items.push_back(std::move(item));
            tool_use_to_item_[tool_use_id] = item_id;
        }
        view_.batches.push_back(std::move(batch));
        view_.metrics.tool_count = static_cast<int>(tool_use_to_item_.size());
    }

    // batch 收口(接 on_tool_batch_finished):打断时,同批还没跑到终态的
    // 条目记 Skipped(单子:不能只给历史补合成 tool_result,屏上却少了那
    // 几枚);正在跑的记 Interrupted 由 OnToolDone 的 interrupted 路覆盖。
    void OnToolBatchFinished(int /*batch_index*/, bool interrupted) {
        if (!current_batch_id_.empty()) {
            for (ToolBatchView& batch : view_.batches) {
                if (batch.batch_id != current_batch_id_) {
                    continue;
                }
                batch.finished = true;
                if (interrupted) {
                    for (const std::string& item_id : batch.ordered_item_ids) {
                        TurnItemView* item = FindItem(item_id);
                        if (item != nullptr && item->status == TurnItemViewState::Pending) {
                            item->status = TurnItemViewState::Skipped;
                            item->ended_at_ms = NowMs();
                        }
                    }
                }
            }
        }
        current_batch_id_.clear();
    }

    // ---- 条目生命周期 -------------------------------------------------------

    // 工具真正开跑(接 on_tool_start/on_builtin_tool_start):把 batch 登记
    // 的 Pending 条目点亮成 Running,补 tool_name/input。迟到的 start(id
    // 没在批次里,比如子代理内层工具)现场立条目。
    void OnToolStarted(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input) {
        TurnItemView* item = FindByToolUse(tool_use_id);
        if (item == nullptr) {
            TurnItemView fresh;
            fresh.item_id = ids_.NextItemId();
            fresh.tool_use_id = tool_use_id;
            fresh.parent_item_id = parent_stack_.empty() ? std::string() : parent_stack_.back();
            fresh.step_id = current_step_id_;
            fresh.batch_id = current_batch_id_;
            fresh.seq = next_seq_++;
            fresh.kind = TurnItemViewKind::Tool;
            fresh.started_at_ms = NowMs();
            fresh.tool_name = name;
            fresh.input = input;
            const std::string item_id = fresh.item_id;
            if (!view_.steps.empty()) {
                view_.steps.back().item_ids.push_back(item_id);
            }
            view_.items.push_back(std::move(fresh));
            tool_use_to_item_[tool_use_id] = item_id;
            ++view_.metrics.tool_count;
            return;
        }
        item->status = TurnItemViewState::Running;
        item->started_at_ms = NowMs();
        item->tool_name = name;
        item->input = input;
    }

    // 工具终态(接 on_tool_done/on_builtin_tool_done)。interrupted 为真
    // 表示这枚是"ESC 后没真跑、补的合成结果"——按 Skipped 记,不冒充跑过
    // 又失败。outcome 缺省按 result_is_error 分。
    void OnToolFinished(const std::string& tool_use_id, const std::string& result_text, bool is_error,
                        std::optional<TurnItemViewState> forced = std::nullopt) {
        TurnItemView* item = FindByToolUse(tool_use_id);
        if (item == nullptr) {
            return;  // 迟到/陌生终态:丢弃不误伤(与 ToolDisplay 同规矩)
        }
        item->result_text = runtime::TruncateUtf8Bytes(result_text, kTurnItemOutputCapBytes);
        item->result_is_error = is_error;
        item->ended_at_ms = NowMs();
        if (forced.has_value()) {
            item->status = *forced;
        } else if (is_error) {
            item->status = TurnItemViewState::Failed;
            ++view_.metrics.failed_tool_count;
        } else {
            item->status = TurnItemViewState::Succeeded;
        }
        tool_use_to_item_.erase(tool_use_id);
    }

    // ESC 打断时"正在跑"的那枚:Interrupted(Pending 的那批走 Skipped,
    // 由 OnToolBatchFinished 覆盖)。
    void MarkRunningInterrupted() {
        for (TurnItemView& item : view_.items) {
            if (item.status == TurnItemViewState::Running) {
                item.status = TurnItemViewState::Interrupted;
                item.ended_at_ms = NowMs();
            }
        }
    }

    // 正文/思考增量:Text/Thinking 条目按 step 归账;一次响应先思考后正文
    // 再工具,原序靠 seq 保住。is_new_block 为真时另起一枚条目(调用方在
    // 思考->正文的切换点判断),否则续灌当前那枚。
    void OnTextDelta(const std::string& text, bool thinking) {
        std::string& current_id = thinking ? thinking_item_id_ : text_item_id_;
        if (current_id.empty()) {
            TurnItemView item;
            item.item_id = ids_.NextItemId();
            item.step_id = current_step_id_;
            item.seq = next_seq_++;
            item.kind = thinking ? TurnItemViewKind::Thinking : TurnItemViewKind::Text;
            item.status = TurnItemViewState::Running;
            item.started_at_ms = NowMs();
            item.result_text = text;
            const std::string item_id = item.item_id;
            if (!view_.steps.empty()) {
                view_.steps.back().item_ids.push_back(item_id);
            }
            view_.items.push_back(std::move(item));
            current_id = item_id;
            return;
        }
        for (TurnItemView& item : view_.items) {
            if (item.item_id == current_id) {
                item.result_text += text;
                if (item.result_text.size() > kTurnItemOutputCapBytes) {
                    item.result_text = runtime::TruncateUtf8Bytes(item.result_text, kTurnItemOutputCapBytes);
                }
                return;
            }
        }
    }

    // 正文/思考块收口(下一枚 item 开始时收上一枚,与 TurnEventAdapter
    // 同规矩):"顺利说完"算 Succeeded。
    void CloseTextItems() {
        for (TurnItemView& item : view_.items) {
            if ((item.kind == TurnItemViewKind::Text || item.kind == TurnItemViewKind::Thinking) &&
                item.status == TurnItemViewState::Running) {
                item.status = TurnItemViewState::Succeeded;
                item.ended_at_ms = NowMs();
            }
        }
        text_item_id_.clear();
        thinking_item_id_.clear();
    }

    // 父子栈(agent 工具开跑时压入、终态弹出):子代理内层工具沿现有
    // SubTool 规则挂 AgentItem 下,不与 main 的 ToolBatch 混排。
    void PushParent(const std::string& item_id) { parent_stack_.push_back(item_id); }
    void PopParent() {
        if (!parent_stack_.empty()) {
            parent_stack_.pop_back();
        }
    }

    // usage 记账(request_count 与 token 四项;单子:统计降噪——footer
    // 紧凑态只写总耗时,token 只进详细态)。
    void OnUsage(const api::UsageReport& report) {
        ++view_.metrics.request_count;
        view_.metrics.input_tokens += report.usage.input_tokens;
        view_.metrics.cache_read_tokens += report.usage.cache_read_tokens;
        view_.metrics.cache_creation_tokens += report.usage.cache_creation_tokens;
        view_.metrics.output_tokens += report.usage.output_tokens;
        view_.metrics.reasoning_tokens += report.usage.output_reasoning_tokens;
    }

    // 轮收口:终态唯一。wall_duration 由调用方按 steady 钟算好递进来
    //(起点是用户输入交账那一刻,不是 collector 构造那一刻);approval_wait
    // 由 TurnActivityState 的累计账递进来。
    void FinishTurn(TurnItemViewState status, std::int64_t wall_duration_ms, std::int64_t approval_wait_ms) {
        CloseTextItems();
        view_.status = status;
        view_.finished = true;
        view_.ended_at_ms = NowMs();
        view_.metrics.wall_duration_ms = wall_duration_ms;
        view_.metrics.approval_wait_ms = approval_wait_ms;
    }

    TurnItemView* FindItem(const std::string& item_id) {
        for (TurnItemView& item : view_.items) {
            if (item.item_id == item_id) {
                return &item;
            }
        }
        return nullptr;
    }

private:
    TurnItemView* FindByToolUse(const std::string& tool_use_id) {
        const auto it = tool_use_to_item_.find(tool_use_id);
        if (it == tool_use_to_item_.end()) {
            return nullptr;
        }
        return FindItem(it->second);
    }

    static std::int64_t NowMs() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    IdAuthority& ids_;
    std::string turn_id_;
    TurnView view_;
    std::uint64_t next_seq_ = 0;
    std::string current_step_id_;
    std::string current_batch_id_;
    std::string text_item_id_;
    std::string thinking_item_id_;
    std::vector<std::string> parent_stack_;
    std::unordered_map<std::string, std::string> tool_use_to_item_;
};

}  // namespace lubancode::runtime
