# hello_plugin - lubancode C ABI 插件示例的独立小工程(ABI v2,三平台)。
#
#   cmake -S . -B build
#   cmake --build build --config Release
#
# 三平台产物:
#   Windows:  hello_plugin.dll
#   Linux:    libhello_plugin.so
#   macOS:    libhello_plugin.dylib
# 拷进 <主目录>/.lubancode/plugins/ 即挂载(工具名前缀取 v2 manifest 的
# plugin_id 字段,即 hello_plugin)。
#
# 插件声明 CAP_HOST_ALLOCATOR,结果 buffer 从宿主堆拿/还(luban_plugin.h
# 的 buffer 契约),所以运行时库跟宿主对不对得上无所谓,不用特意静态链接。
cmake_minimum_required(VERSION 3.21)

project(hello_plugin LANGUAGES C)

add_library(hello_plugin SHARED hello_plugin.c)

# 对外头文件在仓库根的 include/ 下;把这份示例整个拷出去单独用的话,
# 把 luban_plugin.h 一起拷走,改成指向你放它的地方即可。
target_include_directories(hello_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

# Windows:产物名固定 hello_plugin.dll(不带 lib 前缀),POSIX 两平台按各
# 自惯例(lib 前缀 + .so/.dylib),宿主按平台扩展名扫描,都认。
if(WIN32)
  set_target_properties(hello_plugin PROPERTIES PREFIX "")
endif()
