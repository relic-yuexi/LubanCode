// app 编译边界钉子(TU B):与 test_app_boundary_a.cpp 同 include 一批 app
// 公共头。取各符号地址逼链接器接上(重复定义/缺符号时这只 TU 根本进不了
// 测试程序,失败本身就是测试结果)。比较用 uintptr_t,别拿函数地址与
// nullptr 比——GCC 的 -Waddress 会告"永不为空"。
#include <cstdint>

#include "app/backend_stack.hpp"
#include "app/tool_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/interactive_session.hpp"
#include "app/one_shot.hpp"
#include "app/cli_options.hpp"
#include "app/cli_app.hpp"

namespace {

std::uintptr_t SymbolAddressSum() {
    return reinterpret_cast<std::uintptr_t>(&lubancode::app::BuildBackend) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::BuildBaseToolRegistry) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::MemoryOptionsFromConfig) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::PrintDivider) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::RunTurn) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::HandlePromptCommand) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::ResumeSession) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::ChooseModelId) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::PrintToolsCommand) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::RunInteractiveSession) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::AskOnce) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::RunCli) +
           reinterpret_cast<std::uintptr_t>(&lubancode::app::ParseCliArgs);
}

}  // namespace

int BoundaryTuBProbe() {
    return SymbolAddressSum() == 0 ? 1 : 0;
}
