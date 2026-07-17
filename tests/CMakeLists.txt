add_executable(lubancode_tests
  main.cpp
  test_sse_framing.cpp
  test_anthropic_events.cpp
)
target_link_libraries(lubancode_tests PRIVATE lubancode_core doctest::doctest)
target_compile_features(lubancode_tests PRIVATE cxx_std_23)

add_test(NAME lubancode_tests COMMAND lubancode_tests)
