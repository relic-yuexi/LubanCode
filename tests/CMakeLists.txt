add_executable(lubancode_tests
  main.cpp
  test_sse_framing.cpp
  test_anthropic_events.cpp
  test_assembler.cpp
  test_tools.cpp
  test_command_safety.cpp
  test_loop.cpp
  test_responses_request.cpp
  test_responses_events.cpp
  test_write_edit.cpp
  test_ask_user.cpp
  test_search.cpp
  test_context.cpp
  test_config.cpp
  test_model_catalog.cpp
  test_models.cpp
  test_setup_wizard.cpp
  test_provider_wizard.cpp
  test_slash_commands.cpp
  test_worktree.cpp
  test_theme.cpp
  test_prompts.cpp
  test_prompt_assembler.cpp
  test_line_editor.cpp
  test_choice_menu.cpp
  test_compact.cpp
  test_context_tracker.cpp
  test_anthropic_request.cpp
  test_agent_tool.cpp
  test_hooks.cpp
  test_skills.cpp
  test_mcp_transport.cpp
  test_mcp_config.cpp
  test_mcp_client.cpp
  test_mcp_tool.cpp
  test_lsp_framing.cpp
  test_lsp_config.cpp
  test_lsp_client.cpp
  test_lsp_tool.cpp
  test_todo_tool.cpp
  test_tool_search.cpp
  test_todo_render.cpp
  test_divider.cpp
  test_format_utils.cpp
  test_transcript.cpp
  test_agent_status.cpp
  test_markdown.cpp
  test_latex_math.cpp
  test_diff.cpp
  test_plugins.cpp
  test_web_fetch.cpp
  test_web_search.cpp
  test_session_store.cpp
  test_prompt_files.cpp
  test_project_instructions.cpp
  test_project_memory.cpp
  test_i18n.cpp
  test_image_input.cpp
  test_network_timeout.cpp
  test_skill_store.cpp
)
target_link_libraries(lubancode_tests PRIVATE lubancode_core doctest::doctest)
target_compile_features(lubancode_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(lubancode_tests PRIVATE /FS)
endif()

# M11:test_network_timeout.cpp 手写了个原始 socket 假服务器(模拟连不上/
# 半路断流),要链 Winsock。
if(WIN32)
  target_link_libraries(lubancode_tests PRIVATE ws2_32)
endif()

# M8:真实 Python 夹具(mcp_test_server.py)测试要知道 tests/fixtures/ 的绝对
# 路径——ctest 跑的时候工作目录不一定是 tests/ 源码目录,不能瞎猜相对路径。
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# M7:真 DLL 加载测试的靶子——示例插件 hello_plugin(直接编 examples/ 里那
# 份源码,顺带验证示例真的能编)+ api_version 填错的 bad_version_plugin。
# 两个 DLL 落在同一个输出目录,目录路径喂给测试(LUBANCODE_TEST_PLUGIN_DIR),
# PluginHost::LoadDirectory 真扫真载。
if(WIN32)
  add_library(hello_plugin_fixture SHARED
    ${CMAKE_SOURCE_DIR}/examples/plugins/hello_plugin/hello_plugin.c)
  target_include_directories(hello_plugin_fixture PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(hello_plugin_fixture PROPERTIES OUTPUT_NAME hello_plugin PREFIX "")

  add_library(bad_version_plugin SHARED fixtures/bad_version_plugin.c)
  target_include_directories(bad_version_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(bad_version_plugin PROPERTIES PREFIX "")

  add_dependencies(lubancode_tests hello_plugin_fixture bad_version_plugin)
  target_compile_definitions(lubancode_tests PRIVATE
    LUBANCODE_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:hello_plugin_fixture>")
endif()

add_test(NAME lubancode_tests COMMAND lubancode_tests)

# 0.17.0:集成验证用的刮屏驱动器(AllocConsole + WriteConsoleInput +
# ReadConsoleOutput,真控制台专用),不进 ctest——要真钥匙、要弹控制台
# 窗口,只在集成验证时手动跑:
#   screen_driver <lubancode.exe> <子进程工作目录> <报告文件>
if(WIN32)
  add_executable(screen_driver screen_driver.cpp)
  target_compile_features(screen_driver PRIVATE cxx_std_23)

  # markdown(0.18.x):markdown 重画专用的刮屏驱动器,同样不进 ctest,
  # 集成验证时手动跑:markdown_screen_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(markdown_screen_driver markdown_screen_driver.cpp)
  target_compile_features(markdown_screen_driver PRIVATE cxx_std_23)

  # bracketed paste:整段多行内容折成一枚占位符，提交时才展开。
  add_executable(paste_screen_driver paste_screen_driver.cpp)
  target_compile_features(paste_screen_driver PRIVATE cxx_std_23)

  # 流式脚注框化(0.22.x):验流式期间正文下方那个跟 composer 视觉一致的框
  # (上横线/输入行/下横线/状态行)、排队回显、ESC 打断、可视区域自然滚屏时
  # 框还贴得住,同样不进 ctest,集成验证时手动跑:
  #   stream_footer_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(stream_footer_driver stream_footer_driver.cpp)
  target_compile_features(stream_footer_driver PRIVATE cxx_std_23)

  # /clear、provider 切换清屏、ask_user、diff 残色 + 子代理内层工具展示
  # (重复行/紧凑折叠)专用刮屏驱动器,同样不进 ctest,集成验证时手动跑:
  #   fold_dup_clear_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(fold_dup_clear_driver fold_dup_clear_driver.cpp)
  target_compile_features(fold_dup_clear_driver PRIVATE cxx_std_23)
endif()
