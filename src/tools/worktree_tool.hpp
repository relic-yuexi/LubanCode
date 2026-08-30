// 内置 "worktree" 工具(模型侧,0.27.x):让主代理自己会进 worktree——
// enter 建房或进已有房(整场会话 chdir 搬进去)、status 问在不在房里、
// list 列房、exit keep|remove 出房。与用户的 /worktree 命令共用同一个
// cli::WorktreeSession 实例(互斥:一边 active 另一边回 AlreadyActive),
// 账只有一本。
//
// 两处硬安全线不归确认档管(对齐 Claude Code:权限规则压不住这一问):
//   1. 进 .lubancode/worktrees 之外的已有房:必须用户点头。工具自带一个
//      独立的 confirm 回调(交互入口注入真终端问话;管道/单发模式没有
//      人可问,一律拒),不走 needs_confirm/三档确认。
//   2. exit remove 遇脏房:必须用户点头,yolo 也不豁免。同上回调。
//
// enter/exit 动了进程 cwd,成功后调 on_session_moved(交互入口用它重拼
// 系统提示、同步子代理 cwd——跟 /worktree 同一条 sync 路)。

#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/worktree.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class WorktreeTool : public Tool {
public:
    // 问一句是非题。返回 nullopt = 没人可问(管道/单发)或用户打断;
    // true = 点头;false = 拒绝。交互入口注入,不设则两道硬确认一律拒。
    using ConfirmHandler = std::function<std::optional<bool>(const std::string& question_utf8)>;

    // on_session_moved:enter/exit 成功搬了 cwd 之后回调一次(可为空)。
    WorktreeTool(lubancode::cli::WorktreeSession& session, ConfirmHandler confirm,
                 std::function<void()> on_session_moved);

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;

private:
    Result HandleEnter(const nlohmann::json& input);
    Result HandleExit(const nlohmann::json& input);

    lubancode::cli::WorktreeSession& session_;
    ConfirmHandler confirm_;
    std::function<void()> on_session_moved_;
};

}  // namespace lubancode::tools
