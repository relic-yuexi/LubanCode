// 工具确认的终端半边(骨架拆解反弹·问题 1):菜单渲染、diff 预览、
// [y/a/N] 读行与"允许并记住"的持久化,全从 app/turn_runner.cpp 搬来——
// 判断半边(ConfirmToolUse:档位裁定 + 钩子表态 + 拼单)不碰终端,真终端
// IO 归这只口,与流式正文同走 cli 层的出水路。
//
// 搬家规矩(整改单原文):行为一字不变——文案、菜单项、默认高亮、锁与
// StreamFooterSuspendScope 的作用域、反馈行的次序,逐字节照旧;改的只是
// 代码住在哪个抽屉。
#pragma once

#include <functional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "cli/theme.hpp"

namespace lubancode::cli {

struct ToolDisplay;

// 确认问话的单子:判断半边(turn_runner 的 ConfirmToolUse)拼好递进来。
// 要不要问已经裁定完,这里只管"怎么问、答了怎么落画面"。
struct ToolConfirmRequest {
    std::string tool_use_id;                 // 条目路由(OnConfirmRequest/ShowDiffPreview 用)
    std::string name;                        // 工具名(write_file/run_command/...)
    nlohmann::json input = nlohmann::json::object();  // 本次入参(详情/diff 的料)
    bool auto_confirm = false;               // --yes/管道档:持久化追问跳过的旗
    std::set<std::string>* always_allowed = nullptr;  // 会话级"总允许"账(选 a 落这)
    const Theme* theme = nullptr;            // 配色(plain 主题下着色为空串)
    ToolDisplay* display = nullptr;          // 工具条目画板(条目态/diff)
    // 审批悬起旁听(loop 单遗留):真要问用户前 asked(true),答完 answered
    //(allowed)——装配层拿它推 scheduler 的 WaitingPermission 账。
    std::function<void(bool asked, bool allowed)> approval_observer;
};

// 自动放行路的留存 diff(--yes/yolo/auto 档的文件工具、PreToolUse/Permission
// Request 钩子 allow):先算统一 diff 存进条目,工具完成后在原锚点一次画
// 出,不铺马上擦掉的临时预览。write_file/edit_file 之外的工具直接返回;
// 管道模式内部自己短路,输出照旧是稳定纯文本。
void ShowAutomaticToolDiff(ToolDisplay& display, const std::string& tool_use_id, const std::string& name,
                           const nlohmann::json& input);

// 真要问用户(判断半边已裁定要问,钩子也没替用户表态):铺参数详情或
// 留存 diff、起三档菜单(真控制台)或 [y/a/N] 读一行(管道)、答完收画面。
// "本会话总允许"落 always_allowed 账;用户再答"写进 settings.local.json"
// 后调 OnToolAllowedPersist。返回最终 allowed。
bool AskToolConfirm(const ToolConfirmRequest& request);

// "允许并记住"的持久化命令(显式拆出,整改单问题 1 第 3 条):把工具名
// 写进项目 settings.local.json,顺带保证 .gitignore 挡住它,反馈行随后
// 打印。由终端半边收到用户的"记住"选择后调用——判断/装配文件不再直接
// 写配置。
void OnToolAllowedPersist(const std::string& tool_name);

}  // namespace lubancode::cli
