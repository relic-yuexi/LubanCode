// dynamic_library.hpp 的 Windows 实现:LoadLibraryW/GetProcAddress/
// FreeLibrary(从 tools/plugin_loader.cpp 的原路搬来,包成平台接口)。
#include "platform/dynamic_library.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "platform/paths.hpp"

namespace lubancode::platform {

ModuleHandle OpenModule(const std::filesystem::path& path, ModuleError& error) {
    // 坏 DLL(不是合法 PE)默认可能弹系统错误对话框,先把线程错误模式
    // 调成静默,加载完再还原——坏文件只该换来一行警告,不该卡个弹窗。
    DWORD old_error_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_error_mode);
    const HMODULE module = LoadLibraryW(path.c_str());
    const DWORD load_error = module == nullptr ? GetLastError() : 0;
    SetThreadErrorMode(old_error_mode, nullptr);
    if (module == nullptr) {
        error.system_code = static_cast<int>(load_error);
        // 常见码给人话,其余给码号(126 = 找不到模块,127 = 找不到入口)。
        std::string hint;
        if (load_error == ERROR_MOD_NOT_FOUND || load_error == ERROR_DLL_NOT_FOUND) {
            hint = "(模块或其依赖 DLL 找不到)";
        } else if (load_error == ERROR_BAD_EXE_FORMAT) {
            hint = "(不是合法的 PE 库,或架构不合)";
        }
        error.message = "LoadLibrary 失败(错误码 " + std::to_string(load_error) + ")" + hint + ": " +
                        PathToUtf8(path);
        return nullptr;
    }
    return module;
}

void* FindSymbol(ModuleHandle module, const char* name, ModuleError& error) {
    if (module == nullptr) {
        error.message = "FindSymbol: 模块句柄是空的";
        return nullptr;
    }
    const auto proc = GetProcAddress(static_cast<HMODULE>(module), name);
    if (proc == nullptr) {
        error.system_code = static_cast<int>(GetLastError());
        error.message = "GetProcAddress 找不到 " + std::string(name) +
                        "(错误码 " + std::to_string(error.system_code) + ")";
    }
    return reinterpret_cast<void*>(proc);
}

bool CloseModule(ModuleHandle module) {
    if (module == nullptr) {
        return true;
    }
    return FreeLibrary(static_cast<HMODULE>(module)) != FALSE;
}

const char* DynamicLibraryExtension() { return ".dll"; }

}  // namespace lubancode::platform
#endif  // _WIN32
