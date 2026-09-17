// LuaHook 单 P1-D 的 hook 子命令执行体:`lubancode hook validate/test`
// (校验与试跑,引擎在 runtime/hook_package_check)与 `lubancode hook init`
// (官方 scaffold)。这里只递材料、只打印、只落盘;退出码 0/1/2 与报告
// 四档分账的口径见 runtime/hook_package_check.hpp。
#pragma once

#include "app/cli_options.hpp"

namespace lubancode::app {

// validate/test 共用:verb 决定跑不跑 fixtures fake 档(--json 吐机器面)。
int RunHookCheckCommand(const HookCliArgs& args);

// hook init <名字> [--dir <父目录>]:落官方 scaffold(可跑样例 + fixtures;
// 生成后 `lubancode hook test <目录>` 应全绿)。已存在的目录不覆盖。
int RunHookInitCommand(const HookCliArgs& args);

}  // namespace lubancode::app
