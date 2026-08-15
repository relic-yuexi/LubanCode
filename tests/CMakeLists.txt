add_executable(lubancode_tests
  main.cpp
  test_sse_framing.cpp
  test_anthropic_events.cpp
  test_assembler.cpp
  test_tools.cpp
  test_background_tasks.cpp
  test_command_safety.cpp
  test_loop.cpp
  test_responses_request.cpp
  test_responses_events.cpp
  test_chat_request.cpp
  test_chat_events.cpp
  test_write_edit.cpp
  test_ask_user.cpp
  test_search.cpp
  test_context.cpp
  test_context_events.cpp
  test_config.cpp
  test_model_catalog.cpp
  test_provider_catalog.cpp
  test_update_checker.cpp
  test_models.cpp
  test_setup_wizard.cpp
  test_provider_wizard.cpp
  test_slash_commands.cpp
  test_worktree.cpp
  test_worktree_tool.cpp
  test_isolation.cpp
  test_agent_isolation.cpp
  test_theme.cpp
  test_prompts.cpp
  test_prompt_assembler.cpp
  test_line_editor.cpp
  test_queue_model.cpp
  # 0.28.x 排队消息取回键:CSI 参数表(纯函数)、取回判定、备用键开关。
  test_queue_keys.cpp
  test_peer_mailbox.cpp
  test_peer_registry.cpp
  test_peer_loop.cpp
  test_peer_transport.cpp
  test_peer_tools.cpp
  test_terminal_frame.cpp
  test_choice_menu.cpp
  test_compact.cpp
  test_context_tracker.cpp
  test_anthropic_request.cpp
  test_agent_tool.cpp
  # agent_message 窄工具(主模型给运行中子代理传增量):入参/四类失败、
  # 运行名册、假 main backend 集成(inbox 收到 + 来源标签)。
  test_agent_message_tool.cpp
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
  test_agent_panel.cpp
  test_agent_inbox.cpp
  test_markdown.cpp
  test_latex_math.cpp
  test_diff.cpp
  test_diff_syntax.cpp
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
  test_workflow_recorder.cpp
  test_skill_drafter.cpp
  test_backend_stack.cpp
  test_tool_runtime.cpp
  test_status_refresh.cpp
  # app 编译边界钉子:两只 TU 同 include app 公共头,专查 ODR 与漏符号;
  # 相应实现由共享的 lubancode_app 静态库提供。
  test_app_boundary_a.cpp
  test_app_boundary_b.cpp
  # InteractiveSession 真构造/EOF 退场/真析构的集成钉子。
  test_interactive_session.cpp
  # ParseCliArgs 参数矩阵(纯函数)。
  test_cli_options.cpp
  # 领域命令 handler 的状态账(/clear /title /send /peerperm)。
  test_app_commands.cpp
  # memory 回合抽取(分型/转写/JSON 解析,纯函数)。
  test_memory_extract.cpp
  # memory 检索评测尺子(固定语料,Recall/Precision/注入字节/stale 拦截率)。
  test_memory_retrieval.cpp
  # app 与终端实现由 lubancode_app 提供，不在测试目标里重复编译。
  # 回归 BuildCallbacks::on_usage 接线(状态行局部发布)仍走真实终端实现。
  test_repaint_coord.cpp
  test_utf8_boundary.cpp
)
target_link_libraries(lubancode_tests PRIVATE lubancode_app doctest::doctest)
target_compile_features(lubancode_tests PRIVATE cxx_std_23)
target_precompile_headers(lubancode_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/pch.hpp")
# main.cpp 须先定义 DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN，再独自包含 doctest；
# 若强塞测试 PCH，定义落在包含之后，入口便不会生成。
set_source_files_properties(main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if(MSVC)
  target_compile_options(lubancode_tests PRIVATE /MP /FS)
endif()

# M11:test_network_timeout.cpp 手写了个原始 socket 假服务器(模拟连不上/
# 半路断流),要链 Winsock。
if(WIN32)
  target_link_libraries(lubancode_tests PRIVATE ws2_32)
endif()

# M8:真实 Python 夹具(mcp_test_server.py)测试要知道 tests/fixtures/ 的绝对
# 路径——ctest 跑的时候工作目录不一定是 tests/ 源码目录,不能瞎猜相对路径。
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  LUBANCODE_TEST_OFFICIAL_SKILLS_DIR="${CMAKE_SOURCE_DIR}/skills")

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

# 多行字符公式的视觉小样：直连正式渲染器，不进 ctest。
add_executable(latex_box_experiment EXCLUDE_FROM_ALL latex_box_experiment.cpp)
target_link_libraries(latex_box_experiment PRIVATE lubancode_core)
target_compile_features(latex_box_experiment PRIVATE cxx_std_23)

# 0.17.0:集成验证用的刮屏驱动器(AllocConsole + WriteConsoleInput +
# ReadConsoleOutput,真控制台专用),不进 ctest——要真钥匙、要弹控制台
# 窗口,只在集成验证时手动跑:
#   screen_driver <lubancode.exe> <子进程工作目录> <报告文件> [--composer-only]
if(WIN32)
  add_executable(screen_driver EXCLUDE_FROM_ALL screen_driver.cpp)
  target_compile_features(screen_driver PRIVATE cxx_std_23)

  # markdown(0.18.x):markdown 重画专用的刮屏驱动器,同样不进 ctest,
  # 集成验证时手动跑:markdown_screen_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(markdown_screen_driver EXCLUDE_FROM_ALL markdown_screen_driver.cpp)
  target_compile_features(markdown_screen_driver PRIVATE cxx_std_23)

  # bracketed paste:整段多行内容折成一枚占位符，提交时才展开。
  add_executable(paste_screen_driver EXCLUDE_FROM_ALL paste_screen_driver.cpp)
  target_compile_features(paste_screen_driver PRIVATE cxx_std_23)

  # 流式脚注框化(0.22.x):验流式期间正文下方那个跟 composer 视觉一致的框
  # (上横线/输入行/下横线/状态行)、排队回显、ESC 打断、可视区域自然滚屏时
  # 框还贴得住,同样不进 ctest,集成验证时手动跑:
  #   stream_footer_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(stream_footer_driver EXCLUDE_FROM_ALL stream_footer_driver.cpp)
  target_compile_features(stream_footer_driver PRIVATE cxx_std_23)

  # /clear、provider 切换清屏、ask_user、diff 残色 + 子代理内层工具展示
  # (重复行/紧凑折叠)专用刮屏驱动器,同样不进 ctest,集成验证时手动跑:
  #   fold_dup_clear_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(fold_dup_clear_driver EXCLUDE_FROM_ALL fold_dup_clear_driver.cpp)
  target_compile_features(fold_dup_clear_driver PRIVATE cxx_std_23)

  # 子代理面板移到输入框上方(0.28.x):验面板在上横线之上、输入框/状态行
  # 钉底、焦点/查看态短标题/两段确认。数据走 LUBANCODE_AGENT_PANEL_DEMO
  # 假代理钩子,不碰网络:
  #   agent_panel_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(agent_panel_driver EXCLUDE_FROM_ALL agent_panel_driver.cpp)
  target_compile_features(agent_panel_driver PRIVATE cxx_std_23)

  # 真前台任务 + 真流式监听(本单新增):进程内起假 anthropic SSE 服务,真
  # lubancode.exe 连上去跑一条带 title 的前台 agent 调用,子代理卡在真工具
  # (ping)里数秒,流式期间逐帧断言代理面板/详情/Esc 分层。工作目录建议给
  # 一个空临时目录(避开项目记忆 identity):
  #   agent_stream_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(agent_stream_driver EXCLUDE_FROM_ALL agent_stream_driver.cpp)
  target_compile_features(agent_stream_driver PRIVATE cxx_std_23)
  target_link_libraries(agent_stream_driver PRIVATE ws2_32)
endif()
