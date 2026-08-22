// 平台抽象:动态库加载(plugins 单第 5 步:抽 native loader)。
//
// 三平台共用一个形状:
//   Windows: LoadLibraryW / GetProcAddress / FreeLibrary(.dll)
//   Linux:   dlopen / dlsym / dlclose(.so)
//   macOS:   dlopen / dlsym / dlclose(.dylib)
//
// 语义要点:
//   - OpenModule 按路径幂等由调用方(NativeModuleRegistry)记账,平台层
//     不做引用计数去重(Windows LoadLibrary 本身有引用计数,POSIX dlopen
//     也是——同一句柄两次 open 会各拿一份引用,close 两次才真卸;跨平台
//     行为一致靠上层只 open 一次)。
//   - 坏库不弹窗:Windows 侧先调线程错误模式(LoadLibrary 对非 PE 文件
//     默认可能弹系统错误框);POSIX 侧 dlopen 天然静默。
//   - 错误信息人话:Windows GetLastError 码、POSIX dlerror 串原样带回。
#pragma once

#include <filesystem>
#include <string>

namespace lubancode::platform {

// 一枚已加载模块的句柄。void* 存活免得这个头文件拖平台头。
using ModuleHandle = void*;

// 模块加载失败/找不到符号的人话错误。
struct ModuleError {
    int system_code = 0;  // Windows GetLastError;POSIX 无码填 0
    std::string message;  // 人话(含 dlerror/错误码译文)
};

// 加载一枚动态库。成功返回非空句柄;失败返回错误(路径打不开/不是合法
// 库/依赖缺失各有各的话)。load_with_flags:POSIX 可传 RTLD_* 组合
// (0 = 默认 RTLD_NOW|RTLD_LOCAL 由这里补);Windows 忽略。
// 静默模式:Windows 下不弹系统错误框(坏 PE 只该换来一行警告)。
ModuleHandle OpenModule(const std::filesystem::path& path, ModuleError& error);

// 按名字找符号。找不到返回 nullptr(error 填人话)。
void* FindSymbol(ModuleHandle module, const char* name, ModuleError& error);

// 卸载。句柄空是空操作。返回 false = 卸载失败(POSIX dlclose 可能因
// 别处还持引用而"没真卸",这不是错误;Windows FreeLibrary 失败才是)。
bool CloseModule(ModuleHandle module);

// 当前平台认的动态库扩展名(带点,小写):".dll" / ".so" / ".dylib"。
const char* DynamicLibraryExtension();

}  // namespace lubancode::platform
