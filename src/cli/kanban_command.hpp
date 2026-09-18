// Kanban 看板单:`lubancode kanban [--no-open] [--output <路径>]` 执行体。
//
// 只读快照:扫全量 workspace 的会话索引(QueryWorkspaceSessions,
// all_workspaces + include_archived)与各房 workspace.json(ScanRooms),
// 渲染成一份自包含 HTML 落盘,默认顺手开浏览器。不重放 Journal、不起
// 服务、不写任何业务账——唯一写入是那份 HTML 产物。
#pragma once

#include <string>

namespace lubancode::cli {

struct KanbanCommandArgs {
    bool no_open = false;   // --no-open:只打印产物路径,不开浏览器
    std::string output;     // --output <路径>;空 = <状态根>/kanban/kanban.html
};

// 退出码:0 成功;1 状态根不可用/索引读不动/产物写不进。
int RunKanbanCommand(const KanbanCommandArgs& args);

}  // namespace lubancode::cli
