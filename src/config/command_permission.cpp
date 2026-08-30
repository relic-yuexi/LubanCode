// 实现说明见 command_permission.hpp。函数体自 config.cpp 原样搬来,行为
// 一字未改(骨架拆解反弹·问题 7 纯搬家)。
#include "config/command_permission.hpp"

namespace lubancode::config {

namespace {

// 命令去掉前导空白后,以某条(非空)前缀打头就算命中。
bool CommandHasPrefix(const std::string& command, const std::vector<std::string>& prefixes) {
    if (prefixes.empty()) {
        return false;
    }
    const std::size_t start = command.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return false;
    }
    const std::string trimmed = command.substr(start);
    for (const std::string& prefix : prefixes) {
        if (!prefix.empty() && trimmed.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

CommandPermission ClassifyCommandByPermissions(const std::string& command,
                                               const std::vector<std::string>& allow_commands,
                                               const std::vector<std::string>& deny_commands) {
    if (CommandHasPrefix(command, deny_commands)) {
        return CommandPermission::Deny;  // deny 压过 allow
    }
    if (CommandHasPrefix(command, allow_commands)) {
        return CommandPermission::Allow;
    }
    return CommandPermission::None;
}

}  // namespace lubancode::config
