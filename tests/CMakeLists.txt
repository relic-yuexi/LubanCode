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
)
target_link_libraries(lubancode_tests PRIVATE lubancode_core doctest::doctest)
target_compile_features(lubancode_tests PRIVATE cxx_std_23)

add_test(NAME lubancode_tests COMMAND lubancode_tests)
