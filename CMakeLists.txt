cmake_minimum_required(VERSION 3.21)

project(lubancode VERSION 0.26.13 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 先于第三方依赖读取标准测试开关。cpr 的子工程会把 BUILD_TESTING 设成
# OFF 来关自己的测试；封存顶层选择，免得它顺手盖掉 LubanCode 的测试。
include(CTest)
set(LUBANCODE_BUILD_TESTING "${BUILD_TESTING}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# 编译选项:统一 UTF-8 源码/执行字符集,打开常用告警
# ---------------------------------------------------------------------------
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
  # vendored doctest 2.4.12 给 std::tuple 做特化,新 MSVC 的 N5014 合规检查
  # (C5285)会告警;第三方头改不了,就地压掉。自家代码不该犯这条,压了不亏。
  add_compile_options(/wd5285)

  # 全静态链接,exe 自己就能跑,不用到处放 DLL、操心 PATH(见下方
  # BUILD_SHARED_LIBS OFF)。cpr 在 BUILD_SHARED_LIBS OFF 时,会在它自己的
  # CMakeLists.txt 里把运行时库也改成静态的(MultiThreaded[Debug]),这里
  # 顶层项目得跟着定成一样的,不然 lubancode/lubancode_core 用的是默认的
  # 动态运行时(MultiThreaded[Debug]DLL),跟 cpr/curl 的静态运行时对不上,
  # 链接时会报 LNK2038(RuntimeLibrary 不匹配)。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------------------
# 依赖下载源开关:本机网络 github.com 主站(git clone / release 下载)连接
# 常被重置,但 codeload.github.com 的归档包下载是通的,所以本机默认走
# codeload 镜像;CI(GitHub Actions,网络直通 github.com)配置时传
# -DLUBANCODE_USE_CODELOAD=OFF 走官方地址,两边各走各的。
# ---------------------------------------------------------------------------
option(LUBANCODE_USE_CODELOAD
       "用 codeload.github.com 镜像拉 FetchContent 依赖(github 主站不通的网络环境用)" ON)
# _lubancode_tarball(<out_var> <owner/repo> <tag>):按开关拼 tarball 地址。
function(_lubancode_tarball out_var repo tag)
  if(LUBANCODE_USE_CODELOAD)
    set(${out_var} "https://codeload.github.com/${repo}/tar.gz/refs/tags/${tag}" PARENT_SCOPE)
  else()
    set(${out_var} "https://github.com/${repo}/archive/refs/tags/${tag}.tar.gz" PARENT_SCOPE)
  endif()
endfunction()

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

  # 注:统一 URL 方式拉 tarball(不用 GIT_REPOSITORY),地址由顶部的
  # LUBANCODE_USE_CODELOAD 开关决定走 codeload 镜像还是 github 官方。

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

  _lubancode_tarball(_lubancode_cpr_url "libcpr/cpr" "1.11.2")
  FetchContent_Declare(
    cpr
    URL ${_lubancode_cpr_url}
  )

  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")
  _lubancode_tarball(_lubancode_json_url "nlohmann/json" "v3.11.3")
  FetchContent_Declare(
    nlohmann_json
    URL ${_lubancode_json_url}
  )

  # cpr 内部会自己 FetchContent 一份 curl 源码,默认地址是
  # github.com/curl/curl/releases/download/...(本机网络连不上)。
  # 这里提前把 curl 源码 tarball 下载解压好(地址按 LUBANCODE_USE_CODELOAD
  # 决定),通过 FETCHCONTENT_SOURCE_DIR_CURL 让 cpr 直接复用这份源码,
  # 不必自己再下载一次。
  set(_lubancode_curl_tag "curl-8_10_1")
  set(_lubancode_vendor_dir "${CMAKE_BINARY_DIR}/_vendor")
  set(_lubancode_curl_src_dir "${_lubancode_vendor_dir}/curl-${_lubancode_curl_tag}")
  if(NOT EXISTS "${_lubancode_curl_src_dir}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${_lubancode_vendor_dir}")
    message(STATUS "lubancode: 下载 curl 源码(${_lubancode_curl_tag})...")
    _lubancode_tarball(_lubancode_curl_url "curl/curl" "${_lubancode_curl_tag}")
    file(DOWNLOAD
      "${_lubancode_curl_url}"
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
# yaml-cpp(项目记忆 schema 3 的 front matter):与 lua 同一待遇,不管走没
# 走 vcpkg 都 FetchContent 拉源码自建,免得两条构建路各认一份。规格不许
# 手搓半吊子 YAML 解析器,front matter 读写全交给它。
# ---------------------------------------------------------------------------
_lubancode_tarball(_lubancode_yamlcpp_url "jbeder/yaml-cpp" "0.8.0")
# yaml-cpp 0.8.0 的 cmake_minimum_required 还停在 3.4,新 CMake 已删 <3.5
# 兼容;官方给的政策出口就是这项。顶层自要求 3.21,不受影响。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  yaml-cpp
  URL ${_lubancode_yamlcpp_url}
)
FetchContent_MakeAvailable(yaml-cpp)
if(MSVC)
  # 别人家的 C++,顶层 /W4 会刷一屏没意义的告警,单独关掉(同 lua)。
  target_compile_options(yaml-cpp PRIVATE /w)
endif()

# ---------------------------------------------------------------------------
# Lua 5.4(M7 Lua 插件用):不管走没走 vcpkg,统一 FetchContent 拉源码
# (跟下面 doctest 一个待遇)。lua 官方仓库没有 CMakeLists,这里自己包一层:
# GLOB 根目录的 *.c,排除 lua.c(独立解释器入口)/onelua.c(单文件合并编译
# 用)/ltests.c(官方内部测试桩),编成静态库。地址同样走 codeload.github.com
# (github.com 主站连不通,原因见上)。
# ---------------------------------------------------------------------------
include(FetchContent)
enable_language(C)
_lubancode_tarball(_lubancode_lua_url "lua/lua" "v5.4.7")
FetchContent_Declare(
  lua
  URL ${_lubancode_lua_url}
)
FetchContent_MakeAvailable(lua)  # 没有 CMakeLists.txt,只下载解压,不 add_subdirectory
file(GLOB _lubancode_lua_sources "${lua_SOURCE_DIR}/*.c")
list(FILTER _lubancode_lua_sources EXCLUDE REGEX "(lua|onelua|ltests)\\.c$")
add_library(lubancode_lua STATIC ${_lubancode_lua_sources})
target_include_directories(lubancode_lua PUBLIC ${lua_SOURCE_DIR})
if(MSVC)
  # lua 是别人家的 C 代码,顶层的 /W4 会刷一屏没意义的告警,单独关掉。
  target_compile_options(lubancode_lua PRIVATE /w)
endif()

# ---------------------------------------------------------------------------
# 提示词模块编译期嵌入(0.19.x):src/prompts/{core,features,platforms}/*.md
# 构建期由 cmake/embed_prompts.cmake 生成 <build>/generated/embedded_prompts.hpp
# (每个模块一个 constexpr 常量),prompt_assembler.cpp 引用它做运行时条件
# 拼装。选 add_custom_command(构建期)而不是 configure 期 file(READ):
# .md 改一字,时间戳就能触发重生成 + 重编,不必重新 configure;新增/删除
# .md 靠 GLOB 的 CONFIGURE_DEPENDS 自动重新 configure 补上依赖清单。
# ---------------------------------------------------------------------------
set(_lubancode_prompts_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/prompts")
set(_lubancode_embedded_prompts "${CMAKE_BINARY_DIR}/generated/embedded_prompts.hpp")
file(GLOB _lubancode_prompt_modules CONFIGURE_DEPENDS
  "${_lubancode_prompts_dir}/core/*.md"
  "${_lubancode_prompts_dir}/features/*.md"
  "${_lubancode_prompts_dir}/platforms/*.md")
add_custom_command(
  OUTPUT "${_lubancode_embedded_prompts}"
  COMMAND ${CMAKE_COMMAND}
          -DPROMPTS_DIR=${_lubancode_prompts_dir}
          -DOUTPUT=${_lubancode_embedded_prompts}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_prompts.cmake
  DEPENDS ${_lubancode_prompt_modules} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_prompts.cmake
  COMMENT "lubancode: 生成 embedded_prompts.hpp(提示词模块编译期嵌入)"
  VERBATIM)

# 常见 provider / 模型目录也随 exe 带一份快照。运行时可从 GitHub 更新，
# 断网或缓存损坏时仍回退这份构建时内容。
set(_lubancode_provider_catalog "${CMAKE_CURRENT_SOURCE_DIR}/catalog/providers.json")
set(_lubancode_embedded_provider_catalog "${CMAKE_BINARY_DIR}/generated/embedded_provider_catalog.hpp")
add_custom_command(
  OUTPUT "${_lubancode_embedded_provider_catalog}"
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${_lubancode_provider_catalog}
          -DOUTPUT=${_lubancode_embedded_provider_catalog}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_provider_catalog.cmake
  DEPENDS ${_lubancode_provider_catalog} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_provider_catalog.cmake
  COMMENT "lubancode: 嵌入 provider 目录快照"
  VERBATIM)

# ---------------------------------------------------------------------------
# 核心库:api / config 这些不碰命令行交互的代码,单独编成静态库,
# 好让可执行文件和测试程序都能链接同一份实现,不用测试再编一遍。
# cli/setup_wizard.cpp、cli/slash_commands.cpp、cli/line_editor.cpp 虽然在
# cli/ 目录下,但逻辑本身不碰真实 IO(打印/读一行/拉模型列表都是外部注入的
# 函数,line_editor 的 LineEditorCore 吃抽象 KeyEvent、吐 RenderState,不认
# Win32),跟这里其余文件一样"纯逻辑可单测",所以也放进来;真正碰键盘/
# 控制台的 cli/console_input.cpp 留在 lubancode 可执行文件那边,不挪过来。
# ---------------------------------------------------------------------------
# 平台抽象层(v0.20.x 跨平台单):接口在 src/platform/*.hpp,实现按平台
# 二选一。Windows 实现是从原调用点搬家的 Win32 代码;POSIX 实现语义对齐,
# 验证状态见各文件头注释。
if(WIN32)
  set(_lubancode_platform_sources
    src/platform/paths_win.cpp
    src/platform/process_win.cpp
    src/platform/console_win.cpp
    src/platform/peer_transport_win.cpp
    src/platform/clipboard_win.cpp)
else()
  set(_lubancode_platform_sources
    src/platform/paths_posix.cpp
    src/platform/process_posix.cpp
    src/platform/console_posix.cpp
    src/platform/peer_transport_posix.cpp
    src/platform/clipboard_posix.cpp)
endif()

add_library(lubancode_core STATIC
  ${_lubancode_platform_sources}
  src/platform/terminal_batch.cpp
  src/platform/text_encoding.cpp
  src/platform/json_safe.cpp
  src/api/sse_framing.cpp
  src/api/anthropic/client.cpp
  src/api/anthropic/events.cpp
  src/api/chat/client.cpp
  src/api/chat/events.cpp
  src/api/chat/request.cpp
  src/api/responses/client.cpp
  src/api/responses/events.cpp
  src/api/responses/request.cpp
  src/api/assembler.cpp
  src/api/models.cpp
  src/config/config.cpp
  src/config/model_catalog.cpp
  src/config/provider_catalog.cpp
  src/config/update_checker.cpp
  src/config/prompt_files.cpp
  src/config/project_instructions.cpp
  src/config/skill_store.cpp
  src/tools/registry.cpp
  src/tools/read_file.cpp
  src/tools/run_command.cpp
  src/tools/background_tasks.cpp
  src/tools/background_output.cpp
  src/tools/command_safety.cpp
  src/tools/schema_check.cpp
  src/tools/isolation.cpp
  src/tools/worktree_tool.cpp
  src/tools/write_file.cpp
  src/tools/edit_file.cpp
  src/tools/ask_user.cpp
  src/tools/search.cpp
  src/tools/agent_tool.cpp
  src/tools/agent_message_tool.cpp
  src/tools/hooks.cpp
  src/hooks/hash.cpp
  src/hooks/protocol.cpp
  src/hooks/trust.cpp
  src/hooks/loader.cpp
  src/hooks/dispatcher.cpp
  src/hooks/detached.cpp
  src/tools/skill_loader.cpp
  src/tools/skill_tool.cpp
  src/tools/todo_tool.cpp
  src/tools/tool_search.cpp
  src/tools/context_tools.cpp
  src/tools/plugin_loader.cpp
  src/tools/lua_tool.cpp
  src/tools/web_fetch.cpp
  src/tools/web_search.cpp
  src/memory/project_memory.cpp
  src/memory/frontmatter.cpp
  src/memory/memory_tool.cpp
  src/mcp/transport.cpp
  src/mcp/client.cpp
  src/mcp/mcp_tool.cpp
  src/lsp/transport.cpp
  src/lsp/client.cpp
  src/lsp/manager.cpp
  src/tools/lsp_tool.cpp
  src/tools/list_sessions_tool.cpp
  src/tools/send_session_message_tool.cpp
  src/agent/loop.cpp
  src/agent/peer_registry.cpp
  src/agent/peer_mailbox.cpp
  src/agent/peer_session.cpp
  src/ptc/protocol.cpp
  src/ptc/stub_generator.cpp
  src/ptc/bootstrap_py.cpp
  src/ptc/runner.cpp
  src/ptc/profile.cpp
  src/ptc/ptc_tool.cpp
  src/ptc/bench.cpp
  src/agent/prompt_assembler.cpp
  "${_lubancode_embedded_prompts}"
  "${_lubancode_embedded_provider_catalog}"
  src/agent/context.cpp
  src/agent/context_events.cpp
  src/agent/artifact_store.cpp
  src/agent/model_router.cpp
  src/agent/prefix.cpp
  src/agent/compact.cpp
  src/agent/context_budget.cpp
  src/agent/microcompact.cpp
  src/agent/session_store.cpp
  src/agent/workflow_recorder.cpp
  src/agent/skill_drafter.cpp
  # 显示系统剥离单第一步:runtime 合同(纯类型 + 序列化,零 cli/app 依赖)。
  src/runtime/runtime_contract.cpp
  src/cli/i18n.cpp
  src/cli/image_input.cpp
  src/cli/setup_wizard.cpp
  src/cli/provider_wizard.cpp
  src/cli/provider_switch.cpp
  src/cli/wizard_panel.cpp
  src/cli/slash_commands.cpp
  src/cli/worktree.cpp
  src/cli/theme.cpp
  src/cli/choice_menu.cpp
  src/cli/line_editor.cpp
  src/cli/queue_model.cpp
  src/cli/terminal_frame.cpp
  src/cli/context_tracker.cpp
  src/cli/divider.cpp
  src/cli/format_utils.cpp
  src/cli/todo_render.cpp
  src/cli/transcript.cpp
  src/cli/agent_panel.cpp
  src/cli/bottom_chrome.cpp
  src/cli/keymap.cpp
  src/cli/terminal_caps.cpp
  src/cli/history_search.cpp
  src/cli/mention_menu.cpp
  src/cli/markdown.cpp
  src/cli/latex_math.cpp
  src/cli/diff.cpp
  src/cli/diff_syntax.cpp
)
target_include_directories(lubancode_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include   # M7:C ABI 插件对外头文件 luban_plugin.h
  ${CMAKE_BINARY_DIR}/generated)        # 0.19.x:embedded_prompts.hpp(提示词嵌入)
target_link_libraries(lubancode_core PUBLIC cpr::cpr nlohmann_json::nlohmann_json)
if(WIN32)
  # WIC(Windows Imaging Component):剪贴板位图 CF_DIB -> PNG 转码(Alt+V 直贴)。
  target_link_libraries(lubancode_core PRIVATE windowscodecs)
endif()
target_link_libraries(lubancode_core PRIVATE lubancode_lua)  # M7:Lua 插件运行时
target_link_libraries(lubancode_core PRIVATE yaml-cpp::yaml-cpp)  # 项目记忆 front matter(schema 3)
target_compile_features(lubancode_core PUBLIC cxx_std_23)
target_precompile_headers(lubancode_core PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.hpp")
if(MSVC)
  target_compile_options(lubancode_core PRIVATE /MP)
endif()

# app 编排层由主程序与测试共链。此前这些实现各编一遍，Windows 冷构建
# 为同一批大 TU 付两回账；收成静态库后只编一次，main 只留平台入口。
add_library(lubancode_app STATIC
  src/app/backend_stack.cpp
  src/app/memory_extract.cpp
  src/app/model_router.cpp
  src/app/session_title.cpp
  src/app/runtime_profile.cpp
  src/app/tool_runtime.cpp
  src/app/turn_runner.cpp
  src/app/hook_runtime.cpp
  src/app/commands/prompt_commands.cpp
  src/app/commands/session_commands.cpp
  src/app/commands/settings_commands.cpp
  src/app/commands/workspace_commands.cpp
  src/app/commands/hook_commands.cpp
  src/app/commands/peer_commands.cpp
  src/app/commands/doctor_commands.cpp
  src/app/interactive_session.cpp
  src/app/one_shot.cpp
  src/app/cli_options.cpp
  src/app/cli_app.cpp
  src/cli/console_input.cpp
  src/cli/spinner.cpp
  src/cli/record_command.cpp)
target_link_libraries(lubancode_app PUBLIC lubancode_core)
target_compile_features(lubancode_app PUBLIC cxx_std_23)
target_precompile_headers(lubancode_app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.hpp")
if(MSVC)
  target_compile_options(lubancode_app PRIVATE /MP)
endif()

add_executable(lubancode src/main.cpp)
target_link_libraries(lubancode PRIVATE lubancode_app)
target_compile_features(lubancode PRIVATE cxx_std_23)

install(TARGETS lubancode RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/skills/" DESTINATION share/lubancode/skills)

# 官方技能不再编进 C++。开发构建每次把源码树 skills/ 镜到可执行文件旁；
# 只改 Markdown 再跑 cmake --build 就会同步，不触发 C++ 重编。发行包也沿用
# 同一目录形状，便携运行直接读 <exe-dir>/skills。
file(GLOB_RECURSE _lubancode_official_skill_files CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/skills/*")
add_custom_target(lubancode_official_skills ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:lubancode>/skills"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/skills"
          "$<TARGET_FILE_DIR:lubancode>/skills"
  DEPENDS ${_lubancode_official_skill_files}
  COMMENT "lubancode: 同步官方 skills 到程序目录"
  VERBATIM)
add_dependencies(lubancode_official_skills lubancode)

# ---------------------------------------------------------------------------
# 测试:doctest 单头文件,同样走 FetchContent,走 codeload.github.com 镜像
# (原因同上:github.com 主站连不通,codeload.github.com 的归档包下载是通的)。
# ---------------------------------------------------------------------------
set(BUILD_TESTING "${LUBANCODE_BUILD_TESTING}" CACHE BOOL "Build LubanCode tests" FORCE)
if(LUBANCODE_BUILD_TESTING)
  include(FetchContent)
  _lubancode_tarball(_lubancode_doctest_url "doctest/doctest" "v2.4.12")
  FetchContent_Declare(
    doctest
    URL ${_lubancode_doctest_url}
  )
  FetchContent_MakeAvailable(doctest)
  add_subdirectory(tests)
endif()
