add_executable(lubancode_tests
  main.cpp
  test_sse_framing.cpp
  test_anthropic_events.cpp
  test_assembler.cpp
  test_tools.cpp
  test_loop.cpp
  test_responses_request.cpp
  test_responses_events.cpp
  test_write_edit.cpp
  test_search.cpp
  test_context.cpp
  test_config.cpp
  test_models.cpp
  test_setup_wizard.cpp
  test_slash_commands.cpp
  test_theme.cpp
  test_prompts.cpp
  test_line_editor.cpp
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
)
target_link_libraries(lubancode_tests PRIVATE lubancode_core doctest::doctest)
target_compile_features(lubancode_tests PRIVATE cxx_std_23)

# M8:真实 Python 夹具(mcp_test_server.py)测试要知道 tests/fixtures/ 的绝对
# 路径——ctest 跑的时候工作目录不一定是 tests/ 源码目录,不能瞎猜相对路径。
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME lubancode_tests COMMAND lubancode_tests)
