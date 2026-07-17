# hello_plugin - lubancode C ABI 插件示例的独立小工程。
#
#   cmake -S . -B build
#   cmake --build build --config Release
#
# 编出来的 hello_plugin.dll 拷进 <主目录>/.lubancode/plugins/ 即挂载。
# 插件用自己的 CRT 分配/释放内存(free_result 由插件自己实现),所以
# 运行时库跟宿主对不对得上无所谓,不用特意静态链接。
cmake_minimum_required(VERSION 3.21)

project(hello_plugin LANGUAGES C)

add_library(hello_plugin SHARED hello_plugin.c)

# 对外头文件在仓库根的 include/ 下;把这份示例整个拷出去单独用的话,
# 把 luban_plugin.h 一起拷走,改成指向你放它的地方即可。
target_include_directories(hello_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

# 产物名固定 hello_plugin.dll(不带 lib 前缀),工具名前缀就取这个文件名。
set_target_properties(hello_plugin PROPERTIES PREFIX "")
