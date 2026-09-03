// 终端接线收尾单:子代理面板 presenter。原先 BuildAgentPanelEntries/
// BuildAgentTaskTranscriptLines 与三只状态词函数(OutcomeReasonText/
// AgentActivityWord/AgentStateWord,合计约 290 行)住在 interactive_session
// 大类里,按病灶二拆出:面板条目与查看态视口的数据行在这拼,渲染组件
// (AgentPanelEntry 的布局、FormatTranscriptItem、RenderMarkdown)全是
// 已拆好的 cli 组件;大类只持一只本类实例并在钩子里转发。
//
// 台账缓存(revision/tasks):0.28.x 起 TaskSummaries 是轻量全量(不截
// 8 只),按 TaskRevision 缓存,只有台账动了才重拉——缓存跟着本实例走
// (会话级),不进全局。

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cli/transcript.hpp"  // SessionBlock(查看态会话块模型)
#include "tools/task_ledger.hpp"  // AgentTaskSummary/AgentTaskEvent(台账条目)

namespace lubancode::cli {
struct Theme;
struct AgentPanelEntry;
}
namespace lubancode::tools {
class AgentTool;
struct AgentTaskSummary;
}

namespace lubancode::app {

// 事件账 -> 查看根视角的会话块(主/Subagent 面板同构渲染单):把
// AgentTaskEvent 逐枚折成 SessionBlock,渲染交给 cli::RenderSessionBlocks
// 与 FormatTranscriptItems——presenter 不再逐枚直调单卡 formatter 后裸追加,
// 也不再自行决定块间空行。当前查看代理自己的工具一律投 Tool(相对根节点
// 的层级,不按"是不是 Main"写死)。公开给单测直接钉投影与配对规矩。
std::vector<lubancode::cli::SessionBlock> BuildAgentTaskBlocks(
    const std::vector<lubancode::tools::AgentTaskEvent>& events, const lubancode::cli::Theme& theme);

class AgentPanelPresenter {
public:
    explicit AgentPanelPresenter(const lubancode::cli::Theme& theme);

    // 后台子代理面板:轻量全量列表(空闲与流式两处 painter 一个格式)。
    // agent_tool 为空 = 没装 agent 工具,空表。
    std::vector<lubancode::cli::AgentPanelEntry> Entries(lubancode::tools::AgentTool* agent_tool);

    // 查看态的会话视口行:子代理与 main 同款会话——消息账(TaskEvents,
    // 按时间追加)逐事件铺开。agent_view_expanded 是查看态 Ctrl+O 的档位
    //(流式思考尾巴展开/收起),由会话侧的 transcript 控制器递进来。
    std::vector<std::string> TaskTranscriptLines(lubancode::tools::AgentTool* agent_tool, int task_id, int width,
                                                 bool agent_view_expanded);

private:
    const lubancode::cli::Theme& theme_;
    // TaskRevision 缓存:0.28.x 的轻量全量(不截 8 只),台账没动不重拉。
    std::uint64_t cached_revision_ = 0;
    std::vector<lubancode::tools::AgentTaskSummary> cached_tasks_;
};

}  // namespace lubancode::app
