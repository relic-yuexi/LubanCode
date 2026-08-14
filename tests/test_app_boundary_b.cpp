// app 编译边界钉子(TU B):与 test_app_boundary_a.cpp 同 include 一批 app
// 公共头。返回 0 = 探针符号都接上了;链接失败(重复定义/缺符号)时这只
// TU 根本进不了测试程序,失败本身就是测试结果。
#include "app/backend_stack.hpp"
#include "app/tool_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"

int BoundaryTuBProbe() {
    return (&lubancode::app::BuildBackend == nullptr ||
            &lubancode::app::BuildBaseToolRegistry == nullptr ||
            &lubancode::app::MemoryOptionsFromConfig == nullptr ||
            &lubancode::app::PrintDivider == nullptr ||
            &lubancode::app::RunTurn == nullptr ||
            &lubancode::app::HandlePromptCommand == nullptr ||
            &lubancode::app::ResumeSession == nullptr ||
            &lubancode::app::HandleModelCommand == nullptr ||
            &lubancode::app::PrintToolsCommand == nullptr)
               ? 1
               : 0;
}
