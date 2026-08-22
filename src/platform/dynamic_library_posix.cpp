// dynamic_library.hpp 的 POSIX 实现(Linux/macOS):dlopen/dlsym/dlclose。
// dlopen 认绝对路径与带 soname 的相对查找;这里统一喂绝对路径(canonical
// 化由调用方做——按 canonical path 幂等记账本就是调用方的规矩)。
#include "platform/dynamic_library.hpp"

#ifndef _WIN32

#include <dlfcn.h>

#include "platform/paths.hpp"

namespace lubancode::platform {

ModuleHandle OpenModule(const std::filesystem::path& path, ModuleError& error) {
    // RTLD_NOW:符号现在就解析,缺依赖当场报,不留到调用时炸。
    // RTLD_LOCAL:插件符号不漏进全局命名空间,两只插件同名的内部符号
    // 不互踩(单子「原生插件跨平台布局」)。
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* raw = dlerror();
        error.system_code = 0;
        error.message = std::string("dlopen 失败: ") + (raw != nullptr ? raw : "(没有 dlerror 文本)") +
                        ": " + PathToUtf8(path);
    }
    return handle;
}

void* FindSymbol(ModuleHandle module, const char* name, ModuleError& error) {
    if (module == nullptr) {
        error.message = "FindSymbol: 模块句柄是空的";
        return nullptr;
    }
    dlerror();  // 清旧错;dlsym 对空值符号也返回 NULL,靠 dlerror 分辨。
    void* symbol = dlsym(module, name);
    const char* raw = dlerror();
    if (raw != nullptr) {
        error.system_code = 0;
        error.message = std::string("dlsym 找不到 ") + name + ": " + raw;
        return nullptr;
    }
    return symbol;
}

bool CloseModule(ModuleHandle module) {
    if (module == nullptr) {
        return true;
    }
    // dlclose 返回非 0 = 还有引用没放完(别的代码也 dlopen 了同一库);
    // 这不是卸载失败,符号层面已经不可再解析了,如实报 true。
    dlclose(module);
    return true;
}

const char* DynamicLibraryExtension() {
#if defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

}  // namespace lubancode::platform

#endif  // !_WIN32
