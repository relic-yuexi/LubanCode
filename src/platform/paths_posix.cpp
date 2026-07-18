// POSIX 实现:环境变量 + 主目录。路径即 UTF-8 直通,不需要 Windows 那套
// 宽窄转换。语义对齐 paths_win.cpp。
//
// 验证状态:WSL Ubuntu 26.04(g++ 15.2)真机编译、单测通过(config/主目录
// 相关用例全绿);macOS 未经真机验证,待 CI 亮灯。
#include "platform/paths.hpp"

#include <cstdlib>

namespace lubancode::platform {

std::optional<std::string> GetEnvVar(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<std::string> HomeDir() {
    return GetEnvVar("HOME");
}

}  // namespace lubancode::platform
