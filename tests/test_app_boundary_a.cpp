// app 编译边界钉子(TU A):app 头文件必须"只声明、不定义"——两只独立
// translation unit 同时 include 同一批公共头还要能链接,专钉两件事:
//   1. 头里若残留非 inline 函数体,两 TU 各落一份定义,链接期撞重复符号;
//   2. 实现真落进了 .cpp(符号存在),漏搬/漏链接在这里冒"无法解析的外部
//      符号"。逐组拆分时随 commit 扩充 include 与探针清单。
#include <doctest/doctest.h>

#include "app/backend_stack.hpp"
#include "app/tool_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/interactive_session.hpp"

// TU B 的探针(见 test_app_boundary_b.cpp):它引用同一批符号,链接器
// 必须把 TU B 的引用也接上,两 TU 谁也跑不掉。
int BoundaryTuBProbe();

TEST_CASE("app 编译边界:两 TU 同 include 公共头可链接且符号存在") {
    CHECK(&lubancode::app::BuildBackend != nullptr);
    CHECK(&lubancode::app::BuildBaseToolRegistry != nullptr);
    CHECK(&lubancode::app::MemoryOptionsFromConfig != nullptr);
    CHECK(&lubancode::app::PrintDivider != nullptr);
    CHECK(&lubancode::app::RunTurn != nullptr);
    CHECK(&lubancode::app::HandlePromptCommand != nullptr);
    CHECK(&lubancode::app::ResumeSession != nullptr);
    CHECK(&lubancode::app::HandleModelCommand != nullptr);
    CHECK(&lubancode::app::PrintToolsCommand != nullptr);
    CHECK(&lubancode::app::InteractiveLoop != nullptr);
    CHECK(BoundaryTuBProbe() == 0);
}
