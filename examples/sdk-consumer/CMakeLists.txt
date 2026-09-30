cmake_minimum_required(VERSION 3.24)
project(lubancore_installed_consumer LANGUAGES CXX)

# This project must remain buildable after copying just this directory elsewhere.
# No add_subdirectory of LubanCode and no source/build-tree include directories.
find_package(LubanCore CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(lubancore_consumer main.cpp)
target_compile_features(lubancore_consumer PRIVATE cxx_std_23)
target_link_libraries(lubancore_consumer PRIVATE LubanCore::Core Threads::Threads)
if(MSVC)
  target_compile_options(lubancore_consumer PRIVATE /utf-8 /W4)
endif()
if(WIN32)
  add_custom_command(TARGET lubancore_consumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:LubanCore::Core>" "$<TARGET_FILE_DIR:lubancore_consumer>"
    VERBATIM)
endif()

enable_testing()
add_test(NAME sdk.consumer.smoke
  COMMAND lubancore_consumer smoke "${CMAKE_CURRENT_BINARY_DIR}/state-smoke")
add_test(NAME sdk.consumer.isolation
  COMMAND lubancore_consumer isolation "${CMAKE_CURRENT_BINARY_DIR}/state-isolation")
add_test(NAME sdk.consumer.seed
  COMMAND lubancore_consumer seed "${CMAKE_CURRENT_BINARY_DIR}/state-restart")
add_test(NAME sdk.consumer.resume
  COMMAND lubancore_consumer resume "${CMAKE_CURRENT_BINARY_DIR}/state-restart")
add_test(NAME sdk.consumer.recovery_seed
  COMMAND lubancore_consumer recovery-seed "${CMAKE_CURRENT_BINARY_DIR}/state-recovery")
add_test(NAME sdk.consumer.recovery_resume
  COMMAND lubancore_consumer recovery-resume "${CMAKE_CURRENT_BINARY_DIR}/state-recovery")
set_tests_properties(sdk.consumer.seed PROPERTIES FIXTURES_SETUP sdk_restart)
set_tests_properties(sdk.consumer.resume PROPERTIES FIXTURES_REQUIRED sdk_restart)
set_tests_properties(sdk.consumer.recovery_seed PROPERTIES FIXTURES_SETUP sdk_recovery)
set_tests_properties(sdk.consumer.recovery_resume PROPERTIES FIXTURES_REQUIRED sdk_recovery)
set_tests_properties(sdk.consumer.smoke sdk.consumer.isolation sdk.consumer.seed sdk.consumer.resume
  sdk.consumer.recovery_seed sdk.consumer.recovery_resume
  PROPERTIES TIMEOUT 120)
