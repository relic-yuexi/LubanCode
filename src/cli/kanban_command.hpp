// Kanban 看板单:`lubancode kanban [--no-open] [--output <路径>]` 执行体。
//
// 只读快照:扫全量 workspace 的会话索引(QueryWorkspaceSessions,
// all_workspaces + include_archived)与各房 workspace.json(ScanRooms),
// 渲染成一份自包含 HTML 落盘,默认顺手开浏览器。不重放 Journal、不起
// 服务、不写任何业务账——唯一写入是那份 HTML 产物。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cli/theme.hpp"

namespace lubancode::cli {

struct KanbanCommandArgs {
    bool no_open = false;   // --no-open:只打印产物路径,不开浏览器
    std::string output;     // --output <路径>;空 = <状态根>/kanban/kanban.html
};

// TUI 排版批 7:产物提示的 frame 渲染(纯函数,形状册直调)。既有两句
// ("看板已生成: <路径>" / "N 个项目,M 场会话")原样进键值对框,标题用
// 命令名 kanban(schema 标识符,不添文案)。
std::vector<std::string> RenderKanbanNotice(const std::string& output_path,
                                            std::size_t project_count, std::size_t session_count,
                                            const Theme& theme, int width);

// 退出码:0 成功;1 状态根不可用/索引读不动/产物写不进。
int RunKanbanCommand(const KanbanCommandArgs& args);

}  // namespace lubancode::cli
