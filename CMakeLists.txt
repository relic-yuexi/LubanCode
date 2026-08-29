cmake_minimum_required(VERSION 3.21)

# src/app/version.hpp 是版本号唯一出处。CMake 读取它来填 PROJECT_VERSION，
# 免得构建元数据与程序 --version 各挂一块旧牌子。
file(READ "${CMAKE_CURRENT_LIST_DIR}/src/app/version.hpp" _lubancode_version_header)
string(REGEX MATCH
  "kVersion[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
  _lubancode_version_match
  "${_lubancode_version_header}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "无法从 src/app/version.hpp 读取 LubanCode SemVer")
endif()
set(LUBANCODE_VERSION "${CMAKE_MATCH_1}")

project(lubancode VERSION "${LUBANCODE_VERSION}" LANGUAGES CXX)

unset(_lubancode_version_header)
unset(_lubancode_version_match)

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
  "${_lubancode_prompts_dir}/platforms/*.md"
  "${_lubancode_prompts_dir}/modes/*.md")
add_custom_command(
  OUTPUT "${_lubancode_embedded_prompts}"
  COMMAND ${CMAKE_COMMAND}
          -DPROMPTS_DIR=${_lubancode_prompts_dir}
          -DOUTPUT=${_lubancode_embedded_prompts}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_prompts.cmake
  DEPENDS ${_lubancode_prompt_modules} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_prompts.cmake
  COMMENT "lubancode: 生成 embedded_prompts.hpp(提示词模块编译期嵌入)"
  VERBATIM)

# 工具文案(模型可见:工具描述 + schema 参数说明)按语言分档嵌入:
# src/prompts/tools/<语言码>/<工具名>.md 构建期由 cmake/embed_tool_text.cmake
# 生成 <build>/generated/embedded_tool_text.hpp({语言, 工具, 键, 正文} 总表),
# src/tools/tool_text.cpp 首次用时装载成全局查表,回退链 en → zh-CN → C++
# 兜底原文案。与提示词模块同一套构建期生成的路子。
set(_lubancode_tool_text_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/prompts/tools")
set(_lubancode_embedded_tool_text "${CMAKE_BINARY_DIR}/generated/embedded_tool_text.hpp")
file(GLOB_RECURSE _lubancode_tool_text_modules CONFIGURE_DEPENDS
  "${_lubancode_tool_text_dir}/*/*.md")
add_custom_command(
  OUTPUT "${_lubancode_embedded_tool_text}"
  COMMAND ${CMAKE_COMMAND}
          -DTOOLS_DIR=${_lubancode_tool_text_dir}
          -DOUTPUT=${_lubancode_embedded_tool_text}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_tool_text.cmake
  DEPENDS ${_lubancode_tool_text_modules} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_tool_text.cmake
  COMMENT "lubancode: 生成 embedded_tool_text.hpp(工具文案按语言分档嵌入)"
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
    src/platform/network_proxy_win.cpp
    src/platform/process_win.cpp
    src/platform/dynamic_library_win.cpp
    src/platform/console_win.cpp
    src/platform/peer_transport_win.cpp
    src/platform/clipboard_win.cpp)
else()
  set(_lubancode_platform_sources
    src/platform/paths_posix.cpp
    src/platform/network_proxy_posix.cpp
    src/platform/process_posix.cpp
    src/platform/dynamic_library_posix.cpp
    src/platform/console_posix.cpp
    src/platform/peer_transport_posix.cpp
    src/platform/clipboard_posix.cpp)
endif()

# ---------------------------------------------------------------------------
# 显示系统剥离单第八步:target 按层拆开,依赖单向(单子"CMake 拆 target"原文)。
#   lubancode_engine   api + agent + tools + config + memory + hooks + mcp +
#                      lsp + ptc + platform + i18n/theme/worktree 三个零流叶子
#   lubancode_runtime  合同(Event/Command/Sink/Broker)+ TurnRuntime/SessionRuntime/
#                      CommandService/TurnItem(事件编排,不认终端)
#   lubancode_core     engine + runtime + cli 纯逻辑(旧名兼容层,老消费方
#                      [tests/deepseek_e2e 等]不断链)
#   lubancode_app      装配层 + commands + app_server + 终端交互
# 依赖方向:runtime -> engine,core -> runtime -> engine,app -> core。反向
# include(engine/runtime 里 include cli 终端件/app)由 tests/gate 层的守门
# 测试(test_app_boundary_gate.cpp)在 CI 里钉死。
# ---------------------------------------------------------------------------
add_library(lubancode_engine STATIC
  ${_lubancode_platform_sources}
  src/platform/console_common.cpp
  src/platform/network_proxy.cpp
  src/platform/terminal_batch.cpp
  src/platform/text_encoding.cpp
  src/platform/json_safe.cpp
  # 统一 Package 封装单阶段 4:目录内容指纹的共用底座(Plugin 与 Package
  # 同一把尺,两把 v1 材料各自冻结)。
  src/platform/dir_fingerprint.cpp
  src/api/sse_framing.cpp
  src/api/http_stream_transport.cpp  # 批六:四家 wire client 共用的 cpr 传输骨架
  src/api/anthropic/client.cpp
  src/api/anthropic/events.cpp
  src/api/chat/client.cpp
  src/api/chat/events.cpp
  src/api/chat/request.cpp
  src/api/responses/client.cpp
  src/api/responses/events.cpp
  src/api/responses/request.cpp
  src/api/gemini/client.cpp
  src/api/gemini/events.cpp
  src/api/gemini/request.cpp
  src/api/assembler.cpp
  src/api/models.cpp
  src/api/types.cpp
  src/config/config.cpp
  src/config/model_catalog.cpp
  src/config/provider_catalog.cpp
  src/config/update_checker.cpp
  src/config/prompt_files.cpp
  src/config/project_instructions.cpp
  src/config/skill_store.cpp
  src/config/plugin_trust.cpp
  src/tools/registry.cpp
  src/tools/tool_content.cpp  # MCP 富结果:中立内容块/投影/序列化
  src/tools/tool_text.cpp
  src/tools/read_file.cpp
  src/tools/run_command.cpp
  src/tools/background_tasks.cpp
  src/tools/shell_info.cpp
  src/tools/background_output.cpp
  src/tools/command_safety.cpp
  src/tools/schema_check.cpp
  src/tools/isolation.cpp
  src/tools/worktree_tool.cpp
  src/tools/write_file.cpp
  src/tools/edit_file.cpp
  # 逐枚追踪单第四期:条件式撤销的执行侧(undo_file_edit)。
  src/tools/undo_file_edit.cpp
  # 持久目标单:goal 执行轮的检查点工具(schema 校验/证据白名单/状态门槛)。
  src/tools/goal_checkpoint_tool.cpp
  # loop 单:scheduled tick 里模型声明完成的窄口(scope 校验/两档动作)。
  src/tools/loop_control_tool.cpp
  src/tools/ask_user.cpp
  src/tools/search.cpp
  # 观察边界(真机实测 P2-5):search/read_file 默认不吞子代理日志与
  # .evidence 运行时证据,显式点名才放行并告知体积。
  src/tools/observation_filter.cpp
  src/tools/agent_tool.cpp
  # 骨架拆解批三·病十四:AgentTool 六职拆分出的台账/调度/隔间三件。
  src/tools/task_ledger.cpp
  src/tools/subagent_scheduler.cpp
  src/tools/subagent_isolation.cpp
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
  src/mcp/rich_result.cpp  # MCP 富结果:tools/call 全块型解析与 artifact 落盘
  src/mcp/mcp_tool.cpp
  src/lsp/transport.cpp
  src/lsp/client.cpp
  src/lsp/manager.cpp
  src/tools/lsp_tool.cpp
  src/tools/list_sessions_tool.cpp
  src/tools/send_session_message_tool.cpp
  src/agent/loop.cpp
  # 骨架拆解批四·病六/病十二:Agent 自立门户(身份+皮+接线+历史句柄),
  # 上下文管理拆进 ContextManager(双历史/前缀账/sticky/压缩台账/artifact)。
  src/agent/agent.cpp
  src/agent/context_manager.cpp
  # 骨架拆解批三·harness 合流:主回合与子代理共用的续投外环/取消链/
  # 收场分型/Stop 续跑环。
  src/agent/turn_harness.cpp
  # 骨架拆解批七·病十五:peer 域/会话域/技能域物件迁出 agent/(目录归位,
  # target 不动——engine 的 tool_trace/artifact_store 仍引它们的纯函数)。
  src/peers/peer_registry.cpp
  src/peers/peer_mailbox.cpp
  src/peers/peer_session.cpp
  src/ptc/protocol.cpp
  src/ptc/stub_generator.cpp
  src/ptc/bootstrap_py.cpp
  src/ptc/runner.cpp
  src/ptc/profile.cpp
  src/ptc/ptc_tool.cpp
  src/ptc/bench.cpp
  src/agent/prompt_assembler.cpp
  "${_lubancode_embedded_prompts}"
  "${_lubancode_embedded_tool_text}"
  "${_lubancode_embedded_provider_catalog}"
  src/agent/context.cpp
  src/agent/context_events.cpp
  src/agent/artifact_store.cpp
  # ccmoon 真机巡检单 P0:模型输出图片(Responses image_generation_call)
  # 的解码/验身/原子落盘。
  src/agent/model_image_store.cpp
  # 工具结果图片回喂单:请求出门前把工具图从 artifact 落盘重灌 base64,
  # 四家 wire 据此上原生图块(护栏:魔数/字节/边长帽)。
  src/agent/tool_result_images.cpp
  src/agent/model_router.cpp
  # 自定义 Agent 单阶段 1:Agent Definition 严格解析(yaml-cpp)与三层 Catalog。
  src/agent/agent_definition.cpp
  src/agent/agent_catalog.cpp
  # 自定义 Agent 单阶段 3:AgentProfileResolver——父上下文 + Definition ->
  # Resolved AgentProfile 的唯一合并权威(AgentTool 派发与 Workflow 绑定共用)。
  src/agent/agent_profile_resolver.cpp
  # 骨架拆解批一·病四:SampleModel 原语(六处单发采样的公共路)。
  src/agent/sample_model.cpp
  src/agent/prefix.cpp
  src/agent/compact.cpp
  src/agent/context_budget.cpp
  src/agent/microcompact.cpp
  src/sessions/session_store.cpp
  # 持久目标单:goal 事件行(goal_v1 族)的 serialize/parse 纯函数。
  src/sessions/goal_session.cpp
  # 逐枚追踪单:工具生命周期栅栏事件、恢复账本(纯函数)。
  src/agent/tool_trace.cpp
  src/sessions/session_catalog.cpp
  # 会话管理器单第四五步:SessionLifecycle 搬与删的唯一收口(纯磁盘薄壳)。
  src/sessions/session_lifecycle.cpp
  src/skills/workflow_recorder.cpp
  src/skills/skill_drafter.cpp
  # engine 引用的四个 cli 叶子(零标准流、零终端依赖,tools/config 都吃):
  # i18n(字符串表)、theme(配色值)、worktree(git 房务原语)、line_editor
  # (纯逻辑编辑核,agent_tool 吃 DisplayWidthUtf8)。它们留在 src/cli/
  # 目录,但编进 engine——单子守门名单(cin/cout/cerr/ReadLine/ChoiceMenu/
  # TranscriptPainter)不含这四个文件,守门测试同钉。
  src/cli/i18n.cpp
  src/cli/theme.cpp
  src/cli/worktree.cpp
  src/cli/line_editor.cpp
)
target_include_directories(lubancode_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(lubancode_engine PUBLIC cpr::cpr nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(lubancode_engine PRIVATE advapi32 windowscodecs)
endif()
target_link_libraries(lubancode_engine PRIVATE lubancode_lua)
# yaml-cpp 走 PUBLIC:engine 的头(memory/front matter)把 yaml 头递给
# 消费方,拆 target 后 core/workflow 那层也直接吃,PRIVATE 会断 include 路径。
target_link_libraries(lubancode_engine PUBLIC yaml-cpp::yaml-cpp)
target_compile_features(lubancode_engine PUBLIC cxx_std_23)
target_precompile_headers(lubancode_engine PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.hpp")
if(MSVC)
  target_compile_options(lubancode_engine PRIVATE /MP)
endif()

# runtime:事件合同 + 会话/轮次/命令编排。不认终端(守门见
# test_app_boundary_gate.cpp),链 engine(agent/config/tools 的内核件)。
add_library(lubancode_runtime STATIC
  # 显示系统剥离单第一步:runtime 合同(纯类型 + 序列化,零 cli/app 依赖)。
  src/runtime/runtime_contract.cpp
  # 会话管理器单第六步:会话查询/搬删的 runtime 侧命令服务(合同消费方)。
  src/runtime/session_command_service.cpp
  # 第三步:TurnRuntime 纯逻辑核(权限/hooks/usage/取消),cli/app 零依赖。
  src/runtime/turn_runtime.cpp
  # 第五步:TurnItem 领域条目 + 中立 diff 行表(diff 计算移出 ToolDisplay)。
  src/runtime/turn_item.cpp
  # 终端回合视觉收束单:TurnView 中立视图模型(turn/step/batch/item 四层
  # + metrics + turn 活动态),零 cli 依赖,枚举序列化同钉。
  src/runtime/turn_view.cpp
  # 第六步:SessionRuntime(会话存档账/权限账/thread 身份/事件接线)。
  src/runtime/session_runtime.cpp
  # 第七步:CommandService(typed API:/model、/resume、审批回答)。
  src/runtime/command_service.cpp
  # plugins 本地 Tool 运行时(冻结合同 + process v1):manifest 强校验、
  # 进程协议帧、无 shell 进程执行、工具装配层。
  src/runtime/plugin_contract.cpp
  src/runtime/plugin_process.cpp
  src/runtime/plugin_tool.cpp
  # plugins 第 4 步:内嵌 Lua 进 RuntimeManager(三道软墙)。
  src/runtime/plugin_lua.cpp
  # 逐枚追踪单:canonical 工具事件的装配层分线器(持久栅栏/UI 投影/录制)。
  src/runtime/tool_trace_hub.cpp
  # /goal 持久目标单:领域类型/状态机转换表/objective 计数/序列化。
  src/runtime/goal_types.cpp
  # /goal 持久目标单:目标状态机唯一写口(命令面/预算闸/防空转/硬门槛/回放)。
  src/runtime/goal_coordinator.cpp
  # /goal 持久目标单:独立无工具的终点判定模型调用(strict schema/一次 repair)。
  src/runtime/goal_evaluator.cpp
  # /goal 持久目标单:每轮喂执行模型的动态 GoalContext(纯函数)。
  src/runtime/goal_context.cpp
  # /goal 持久目标单:compact 守恒的 goal snapshot(拼装/校验/hash)。
  src/runtime/goal_compact.cpp
  # /goal 持久目标单:ToolTraceHub 事件 -> GoalEvidence 的采证适配器(纯函数)。
  src/runtime/goal_evidence.cpp
  # /loop 会话定时循环单:领域类型/状态机转换表/interval 解析/错过合并账。
  src/runtime/loop_types.cpp
  # /loop 会话定时循环单:内存 scheduler(timer 只发 wake/泵取件/退避/expiry)。
  src/runtime/loop_scheduler.cpp
  # /loop 会话定时循环单:空闲唤醒多路总口(替单枚 SetIdleWakeHook)。
  src/runtime/idle_wake.cpp
  # loop 单:goal 单立的会话工作分流合同在这里实装(公平泵 PumpNextWork)。
  src/runtime/session_work_scheduler.cpp
  # loop 单遗留(骨架):scheduled prompt -> 安全动作的 catalog 与 resolver。
  src/runtime/scheduled_action.cpp
  # Plan 模式(只读研究硬闸单):CollaborationMode/ModePolicy/PlanDocument
  # 合同与纯函数(ModePolicy 硬闸、Plan shell 分类、<proposed_plan> 扫描)。
  src/runtime/plan_mode.cpp
  # 骨架拆解批五先行半批:预算闸(三尺 + 停因分型 + 连撞计数)——三外壳
  # (loop/goal/workflow)各声明自己的尺,机制只此一份。
  src/runtime/budget_gate.cpp
  # 骨架拆解批五先行半批:重试退避(固定/指数/阶梯 + 上限 + 抖动档 +
  # 可取消等待)——loop 的 5s/15s 与 workflow 的节点级指数同走这枚。
  src/runtime/retry_backoff.cpp
  # 骨架拆解批五乙(病十六后半):统一回放接口——信封/次序/恢复入口
  # 一份;loop/goal/workflow 三家回放各接各的域编解码与折叠口。
  src/runtime/replay.cpp
)
target_include_directories(lubancode_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(lubancode_runtime PUBLIC lubancode_engine)
target_compile_features(lubancode_runtime PUBLIC cxx_std_23)
target_precompile_headers(lubancode_runtime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.hpp")
if(MSVC)
  target_compile_options(lubancode_runtime PRIVATE /MP)
endif()

# core:旧名兼容层 = engine + runtime + cli 纯逻辑(不碰键盘/控制台的那半)。
# cli/setup_wizard.cpp、cli/slash_commands.cpp 等在 cli/ 目录下但逻辑本身
# 不碰真实 IO(打印/读一行都是注入函数),与从前同待遇放这层。
add_library(lubancode_core STATIC
  src/cli/image_input.cpp
  src/cli/setup_wizard.cpp
  src/cli/provider_wizard.cpp
  src/cli/provider_switch.cpp
  src/cli/wizard_panel.cpp
  src/cli/slash_commands.cpp
  src/cli/choice_menu.cpp
  src/cli/queue_model.cpp
  src/cli/terminal_frame.cpp
  # 终端接线收尾单:输出端口(stdout/stderr 的唯一改道口,进程级一只)。
  src/cli/terminal_port.cpp
  # 终端接线收尾单:transcript 控制器(导航/查看态/条目账,自大类搬出)。
  src/cli/transcript_controller.cpp
  src/cli/context_tracker.cpp
  src/cli/divider.cpp
  src/cli/format_utils.cpp
  src/cli/todo_render.cpp
  src/cli/transcript.cpp
  # 终端回合视觉收束单:TerminalTurnRenderer(TurnView -> 行组,纯函数;
  # 条目措辞复用 FormatTranscriptItem,这里只管轮的骨架与 footer)。
  src/cli/turn_renderer.cpp
  src/cli/agent_panel.cpp
  # 终端接线收尾单:面板接线宿主(provider/actions 实例化,静态槽退役)。
  src/cli/agent_panel_host.cpp
  src/cli/bottom_chrome.cpp
  src/cli/keymap.cpp
  src/cli/terminal_caps.cpp
  src/cli/history_search.cpp
  src/cli/session_picker.cpp
  src/cli/mention_menu.cpp
  src/cli/markdown.cpp
  src/cli/latex_math.cpp
  src/cli/diff.cpp
  src/cli/diff_syntax.cpp
  # Workflows 自然语言编排单:定义/解析/校验/目录/图渲染,纯逻辑。
  src/workflow/definition.cpp
  src/workflow/parser.cpp
  src/workflow/catalog.cpp
  src/workflow/validator.cpp
  src/workflow/graph_view.cpp
  src/workflow/store.cpp
  src/workflow/journal.cpp
  src/workflow/runtime.cpp
  src/workflow/host_executors.cpp
  src/workflow/compiler.cpp
  src/workflow/planner.cpp
  src/workflow/frontend.cpp
  # 统一 Package 封装单阶段 1:根清单严格解析、SemVer/兼容范围、四层扫描、
  # 稳定盘点与内容哈希(纯逻辑,与 workflow 平级)。
  src/package/semver.cpp
  src/package/manifest.cpp
  src/package/inventory.cpp
  # 统一 Package 封装单阶段 2:六类组件的原生 parser 接线(ComponentSource-
  # Root)、mcp.yaml 解析、短名/全名引用解析、MountPlan(只产计划不执行)。
  src/package/component.cpp
  src/package/catalog.cpp
  # 统一 Package 封装单阶段 3:会话钉快照(PackageMount)与四类内容组件
  # (Skill/Workflow/Agent/Prompt Profile)的挂载材料折算。
  src/package/mounting.cpp
  # 统一 Package 封装单阶段 4:Package 信任门(PackageTrustStore + 审批
  # 材料与账务;指纹底座在 platform/dir_fingerprint)。
  src/package/trust.cpp
  # 统一 Package 封装单阶段 5:Plugin 与 MCP 挂载事务——mcp.yaml 折
  # McpServerConfig(${package_dir}/${package_data}/${env:NAME} 结构化展开)、
  # 插件探针、暂存 runtime、失败回滚、成功发布(整包成整包败的执行版)。
  src/package/code_mounting.cpp
  # 统一 Package 封装单阶段 6:启停账(PackageStateStore——包外
  # package-state.json,停用的包挂载一律跳过,连内容组件都不挂)。
  src/package/state.cpp
  # 自进化闭环阶段 1:EvolutionObservation、五路只读 adapter、指纹归一、
  # 采集器与只追加观察账(纯逻辑,复用 skills/journal 的既有脱敏)。
  src/evolution/observation.cpp
  src/evolution/adapters.cpp
  src/evolution/collector.cpp
  src/evolution/observation_store.cpp
  # 自进化闭环阶段 2:录制 -> 最小 content-only 候选的起草器、候选态
  # (evolution/approval/状态机序列化 + 候选仓只读盘点)与状态机唯一写口
  # EvolutionCoordinator(整包哈希照 package 阶段 1 盘点算法复算)。
  src/evolution/drafter.cpp
  src/evolution/candidate.cpp
  src/evolution/coordinator.cpp
  # 自进化闭环阶段 3:评测与基线——eval-plan 解析、确定性验收检查
  # (file_exists/json_parses/command 一类)、静态门(doctor 复用 + 密钥/
  # 绝对路径扫描)、指标账七项、账面汇总与确定性判词。只产结果不落盘,
  # 落账与状态迁移仍走 EvolutionCoordinator。
  src/evolution/eval.cpp
  # 自进化闭环阶段 4:VersionStore——staging 复制与原子落位、channels
  # 指针账(active/canary)、install-log 只追加事件账与 PackageSnapshot
  # (路径+哈希,装配折算)。store 机械归它;迁状态的笔仍在 Coordinator。
  src/evolution/promoter.cpp
)
target_include_directories(lubancode_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include   # M7:C ABI 插件对外头文件 luban_plugin.h
  ${CMAKE_BINARY_DIR}/generated)        # 0.19.x:embedded_prompts.hpp(提示词嵌入)
# core = engine + runtime + cli 纯逻辑:引擎与运行时的传递依赖从这两层
# PUBLIC 递过来(cpr/json/lua/yaml 在 engine 上,这里不再重复挂)。
target_link_libraries(lubancode_core PUBLIC lubancode_runtime)
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
  # 终端接线收尾单:子代理面板 presenter(条目列表/查看态视口,自大类搬出)。
  src/app/agent_panel_presenter.cpp
  src/app/session_title.cpp
  src/app/runtime_profile.cpp
  src/app/tool_runtime.cpp
  src/app/turn_runner.cpp
  # 骨架拆解批二余款:终端画屏的事件流 sink(Callbacks 老路拔除后,显示
  # 出水全走 TurnEventAdapter,这只在 sink 侧翻画面)。
  src/app/terminal_turn_sink.cpp
  # 终端画面隔网先行批:流式事件与终端之间的 UI 队列 + 过渡消费侧。
  src/app/ui_event_pump.cpp
  src/app/hook_runtime.cpp
  src/app_server/protocol.hpp
  src/app_server/schema.cpp
  src/app_server/framing.cpp
  src/app_server/outbox.cpp
  src/app_server/dispatcher.cpp
  src/app_server/connection.cpp
  src/app_server/interaction.cpp
  src/app_server/server.cpp
  # 可见调试阶段 3:browser/* 方法面——sidecar 进程管理、RPC 配对、
  # 事件转发、审批与取消、截图 artifact。
  src/app_server/browser_service.cpp
  src/app/commands/prompt_commands.cpp
  src/app/commands/session_commands.cpp
  src/app/commands/settings_commands.cpp
  src/app/commands/workspace_commands.cpp
  src/app/commands/hook_commands.cpp
  src/app/commands/peer_commands.cpp
  src/app/commands/doctor_commands.cpp
  # 自定义 Agent 单阶段 1:/agents 与 /agent doctor 的只读骨架。
  src/app/commands/agent_commands.cpp
  # /goal 持久目标单:终端 slash 的排版/gate/二次确认文案。
  src/app/commands/goal_commands.cpp
  # /loop 会话定时循环单:slash 业务面(列表/状态/暂停/补拍的终端行)。
  src/app/commands/loop_commands.cpp
  src/app/commands/workflow_commands.cpp
  # 统一 Package 封装单阶段 1:/package list|show|doctor 只读面。
  src/app/commands/package_commands.cpp
  # background 管理面单:/background show|logs|stop 的用户命令面。
  src/app/commands/background_commands.cpp
  # 自进化闭环阶段 1:/evolve status|list|show 只读面。
  src/app/commands/evolve_commands.cpp
  # 终端接线收尾单:/memory 命令 presenter(自大类搬出)。
  src/app/commands/memory_commands.cpp
  # 终端接线收尾单:/trace 命令 presenter(自大类搬出)。
  src/app/commands/trace_commands.cpp
  # 终端接线收尾单:/model 命令 presenter(自大类搬出)。
  src/app/commands/model_commands.cpp
  # 会话终章:命令分派注册制(47 案 switch 换注册表,路由与门)。
  src/app/commands/command_registry.cpp
  # 会话终章:子系统接线器(goal/loop/plan/peer/录制,状态+装配+泵+存档恢复)。
  src/app/wirings/goal_session_wiring.cpp
  src/app/wirings/loop_session_wiring.cpp
  src/app/wirings/plan_session_wiring.cpp
  src/app/wirings/peer_session_wiring.cpp
  src/app/wirings/record_session_wiring.cpp
  # 会话终章:组合根装配件(控制器构造函数的装配段外迁,cli_app 调用)。
  src/app/session_stack.cpp
  # 会话终章:控制器的装配半边(构造/析构/接线/材料包;运行半边在
  # interactive_session.cpp,类声明共享 interactive_session_controller.hpp)。
  src/app/interactive_session_wiring.cpp
  # 会话终章:@ 提及支件(索引缓存 + 提交前校验/账单,自大类搬出)。
  src/app/mention_support.cpp
  src/app/interactive_session.cpp
  src/app/one_shot.cpp
  src/app/cli_options.cpp
  src/app/plugin_scaffold.cpp
  src/app/cli_app.cpp
  src/cli/console_input.cpp
  src/cli/session_picker_panel.cpp
  src/cli/spinner.cpp
  # 骨架拆解批二:SpinnerBackend 自 backend_stack 挪来(UI 件归显示层)。
  src/cli/spinner_backend.cpp
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
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/" DESTINATION share/lubancode/docs)

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

# 官方 docs 与 skills 同版同行。开发构建把两棵资源树都镜到程序旁；便携包、
# Windows 安装和 POSIX share/lubancode 安装由安装脚本保持同一相对布局，
# lubancode-config 因而能从技能目录稳定找到 ../../docs。
file(GLOB_RECURSE _lubancode_official_doc_files CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/*")
add_custom_target(lubancode_official_docs ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:lubancode>/docs"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/docs"
          "$<TARGET_FILE_DIR:lubancode>/docs"
  DEPENDS ${_lubancode_official_doc_files}
  COMMENT "lubancode: 同步官方 docs 到程序目录"
  VERBATIM)
add_dependencies(lubancode_official_docs lubancode)

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
