cmake_minimum_required(VERSION 3.21)

project(lubancode VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# 编译选项:统一 UTF-8 源码/执行字符集,打开常用告警
# ---------------------------------------------------------------------------
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)

  # 全静态链接,exe 自己就能跑,不用到处放 DLL、操心 PATH(见下方
  # BUILD_SHARED_LIBS OFF)。cpr 在 BUILD_SHARED_LIBS OFF 时,会在它自己的
  # CMakeLists.txt 里把运行时库也改成静态的(MultiThreaded[Debug]),这里
  # 顶层项目得跟着定成一样的,不然 lubancode/lubancode_core 用的是默认的
  # 动态运行时(MultiThreaded[Debug]DLL),跟 cpr/curl 的静态运行时对不上,
  # 链接时会报 LNK2038(RuntimeLibrary 不匹配)。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 依赖获取:优先使用 vcpkg(manifest 模式,检测 VCPKG_ROOT / vcpkg 工具链已生效),
# 否则回退到 CMake FetchContent 直接拉取 cpr 与 nlohmann-json 源码构建。
# ---------------------------------------------------------------------------
set(LUBANCODE_USING_VCPKG OFF)
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
  set(LUBANCODE_USING_VCPKG ON)
elseif(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file")
  set(LUBANCODE_USING_VCPKG ON)
endif()

if(LUBANCODE_USING_VCPKG)
  message(STATUS "lubancode: 使用 vcpkg 提供依赖 (VCPKG_ROOT / toolchain 已检测到)")
  find_package(cpr CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
else()
  message(STATUS "lubancode: 未检测到 vcpkg,改用 FetchContent 拉取依赖源码构建")
  include(FetchContent)

  # 全部编成静态库:exe 自己就能跑,不用把 cpr.dll / libcurl-*.dll 之类的
  # 运行时依赖丢得到处都是、还得操心 PATH。
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  # 注:当前网络环境下 github.com 主站(git clone / release 下载)连接常被重置,
  # 但 codeload.github.com 的归档包下载是通的,所以这里统一改成 URL 方式拉 tarball,
  # 不用 GIT_REPOSITORY。

  # cpr 自带的 libcurl 拉取开关:未找到系统 curl 时自行下载构建
  set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
  set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CPR_ENABLE_SSL ON CACHE BOOL "" FORCE)
  # curl 默认会再拉一份 zlib-ng(走 github.com git clone,同样连不上),
  # M0 骨架不需要压缩支持,直接关掉,避免这层依赖。
  set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
  # 这台机器上 MSYS2/mingw64 的 include 目录里装着 nghttp2、libidn2、libpsl、
  # libssh2 的开发头文件,curl 的 find_package 会把它们找出来当"系统库"用,
  # 但那是给 MinGW/GCC 用的头文件,MSVC 编译器读不懂,会直接语法报错。
  # M0 骨架只需要基本的 HTTP/HTTPS,这几个可选特性用不上,统一关掉、
  # 并且禁止 find_package 再去找它们。
  set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(CMAKE_DISABLE_FIND_PACKAGE_NGHTTP2 ON CACHE BOOL "" FORCE)
  set(CMAKE_DISABLE_FIND_PACKAGE_Libidn2 ON CACHE BOOL "" FORCE)
  set(CMAKE_DISABLE_FIND_PACKAGE_Libpsl ON CACHE BOOL "" FORCE)
  set(CMAKE_DISABLE_FIND_PACKAGE_Libssh2 ON CACHE BOOL "" FORCE)

  FetchContent_Declare(
    cpr
    URL https://codeload.github.com/libcpr/cpr/tar.gz/refs/tags/1.11.2
  )

  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")
  FetchContent_Declare(
    nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
  )

  # cpr 内部会自己 FetchContent 一份 curl 源码,默认地址是
  # github.com/curl/curl/releases/download/...,同样连不上。
  # 这里提前用 codeload.github.com 把 curl 源码下载解压好,通过
  # FETCHCONTENT_SOURCE_DIR_CURL 让 cpr 直接复用这份源码,不必自己再下载一次。
  set(_lubancode_curl_tag "curl-8_10_1")
  set(_lubancode_vendor_dir "${CMAKE_BINARY_DIR}/_vendor")
  set(_lubancode_curl_src_dir "${_lubancode_vendor_dir}/curl-${_lubancode_curl_tag}")
  if(NOT EXISTS "${_lubancode_curl_src_dir}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${_lubancode_vendor_dir}")
    message(STATUS "lubancode: 下载 curl 源码(${_lubancode_curl_tag})...")
    file(DOWNLOAD
      "https://codeload.github.com/curl/curl/tar.gz/refs/tags/${_lubancode_curl_tag}"
      "${_lubancode_vendor_dir}/curl.tar.gz"
      STATUS _lubancode_curl_dl_status
      TLS_VERIFY ON)
    list(GET _lubancode_curl_dl_status 0 _lubancode_curl_dl_code)
    if(NOT _lubancode_curl_dl_code EQUAL 0)
      message(FATAL_ERROR "lubancode: curl 源码下载失败: ${_lubancode_curl_dl_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_lubancode_vendor_dir}/curl.tar.gz" DESTINATION "${_lubancode_vendor_dir}")
  endif()
  set(FETCHCONTENT_SOURCE_DIR_CURL "${_lubancode_curl_src_dir}" CACHE PATH "" FORCE)

  FetchContent_MakeAvailable(cpr nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# 核心库:api / config 这些不碰命令行交互的代码,单独编成静态库,
# 好让可执行文件和测试程序都能链接同一份实现,不用测试再编一遍。
# cli/setup_wizard.cpp、cli/slash_commands.cpp 虽然在 cli/ 目录下,但逻辑本身
# 不碰真实 IO(打印/读一行/拉模型列表都是外部注入的函数),跟这里其余文件
# 一样"纯逻辑可单测",所以也放进来;真正碰键盘/控制台的 cli/console_input.cpp
# 留在 lubancode 可执行文件那边,不挪过来。
# ---------------------------------------------------------------------------
add_library(lubancode_core STATIC
  src/api/sse_framing.cpp
  src/api/anthropic/client.cpp
  src/api/anthropic/events.cpp
  src/api/responses/client.cpp
  src/api/responses/events.cpp
  src/api/responses/request.cpp
  src/api/assembler.cpp
  src/api/models.cpp
  src/config/config.cpp
  src/tools/registry.cpp
  src/tools/read_file.cpp
  src/tools/run_command.cpp
  src/tools/write_file.cpp
  src/tools/edit_file.cpp
  src/tools/search.cpp
  src/agent/loop.cpp
  src/agent/context.cpp
  src/cli/setup_wizard.cpp
  src/cli/slash_commands.cpp
)
target_include_directories(lubancode_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lubancode_core PUBLIC cpr::cpr nlohmann_json::nlohmann_json)
target_compile_features(lubancode_core PUBLIC cxx_std_23)

add_executable(lubancode src/main.cpp src/cli/console_input.cpp)
target_link_libraries(lubancode PRIVATE lubancode_core)
target_compile_features(lubancode PRIVATE cxx_std_23)

install(TARGETS lubancode RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# 测试:doctest 单头文件,同样走 FetchContent,走 codeload.github.com 镜像
# (原因同上:github.com 主站连不通,codeload.github.com 的归档包下载是通的)。
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  doctest
  URL https://codeload.github.com/doctest/doctest/tar.gz/refs/tags/v2.4.12
)
FetchContent_MakeAvailable(doctest)

enable_testing()
add_subdirectory(tests)
