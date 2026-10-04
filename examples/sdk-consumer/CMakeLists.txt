cmake_minimum_required(VERSION 3.24)
project(lubancore_installed_consumer LANGUAGES CXX)

# This project must remain buildable after copying just this directory elsewhere.
# No add_subdirectory of LubanCode and no source/build-tree include directories.
find_package(LubanCore CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(lubancore_consumer main.cpp authorization.cpp builtin_search.cpp builtin_memory.cpp builtin_memory_save.cpp subagents.cpp packages.cpp package_inventory.cpp lua.cpp actions.cpp event_sink.cpp memory_blobs.cpp)
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
add_test(NAME sdk.consumer.authorization
  COMMAND lubancore_consumer authorization "${CMAKE_CURRENT_BINARY_DIR}/state-authorization")
add_test(NAME sdk.consumer.packages
  COMMAND lubancore_consumer packages "${CMAKE_CURRENT_BINARY_DIR}/state-packages")
add_test(NAME sdk.consumer.package_inventory
  COMMAND lubancore_consumer package-inventory "${CMAKE_CURRENT_BINARY_DIR}/state-package-inventory")
add_test(NAME sdk.consumer.lua
  COMMAND lubancore_consumer lua "${CMAKE_CURRENT_BINARY_DIR}/state-lua")
add_test(NAME sdk.consumer.lua_seed
  COMMAND lubancore_consumer lua-seed "${CMAKE_CURRENT_BINARY_DIR}/state-lua-restart")
add_test(NAME sdk.consumer.lua_resume
  COMMAND lubancore_consumer lua-resume "${CMAKE_CURRENT_BINARY_DIR}/state-lua-restart")
set_tests_properties(sdk.consumer.lua_seed PROPERTIES FIXTURES_SETUP sdk_lua_restart)
set_tests_properties(sdk.consumer.lua_resume PROPERTIES FIXTURES_REQUIRED sdk_lua_restart)
set_tests_properties(sdk.consumer.lua sdk.consumer.lua_seed sdk.consumer.lua_resume PROPERTIES TIMEOUT 120)
set(LUBANCORE_CONSUMER_RESOURCE_ROOT "" CACHE PATH "Explicit relocated installed SDK resource root for real search acceptance")
if(NOT IS_ABSOLUTE "${LUBANCORE_CONSUMER_RESOURCE_ROOT}")
  message(FATAL_ERROR "Set LUBANCORE_CONSUMER_RESOURCE_ROOT to the absolute relocated share/lubancore directory")
endif()
add_test(NAME sdk.consumer.builtin_search
  COMMAND lubancore_consumer builtin-search "${CMAKE_CURRENT_BINARY_DIR}/state-builtin-search" "${LUBANCORE_CONSUMER_RESOURCE_ROOT}")
add_test(NAME sdk.consumer.smoke
  COMMAND lubancore_consumer smoke "${CMAKE_CURRENT_BINARY_DIR}/state-smoke")
add_test(NAME sdk.consumer.isolation
  COMMAND lubancore_consumer isolation "${CMAKE_CURRENT_BINARY_DIR}/state-isolation")
add_test(NAME sdk.consumer.extensions
  COMMAND lubancore_consumer extensions "${CMAKE_CURRENT_BINARY_DIR}/state-extensions")
add_test(NAME sdk.consumer.actions
  COMMAND lubancore_consumer actions "${CMAKE_CURRENT_BINARY_DIR}/state-actions")
add_test(NAME sdk.consumer.event_sink
  COMMAND lubancore_consumer event-sink "${CMAKE_CURRENT_BINARY_DIR}/state-event-sink")
add_test(NAME sdk.consumer.memory_blobs
  COMMAND lubancore_consumer memory-blobs "${CMAKE_CURRENT_BINARY_DIR}/state-memory-blobs")
add_test(NAME sdk.consumer.results
  COMMAND lubancore_consumer results "${CMAKE_CURRENT_BINARY_DIR}/state-results")
add_test(NAME sdk.consumer.skills_seed
  COMMAND lubancore_consumer skills-seed "${CMAKE_CURRENT_BINARY_DIR}/state-skills")
add_test(NAME sdk.consumer.skills_resume
  COMMAND lubancore_consumer skills-resume "${CMAKE_CURRENT_BINARY_DIR}/state-skills")
add_test(NAME sdk.consumer.memory_seed
  COMMAND lubancore_consumer memory-seed "${CMAKE_CURRENT_BINARY_DIR}/state-memory")
add_test(NAME sdk.consumer.memory_resume
  COMMAND lubancore_consumer memory-resume "${CMAKE_CURRENT_BINARY_DIR}/state-memory")
add_test(NAME sdk.consumer.memory_save_seed
  COMMAND lubancore_consumer memory-save-seed "${CMAKE_CURRENT_BINARY_DIR}/state-memory-save")
add_test(NAME sdk.consumer.memory_save_resume
  COMMAND lubancore_consumer memory-save-resume "${CMAKE_CURRENT_BINARY_DIR}/state-memory-save")
add_test(NAME sdk.consumer.subagents
  COMMAND lubancore_consumer subagents "${CMAKE_CURRENT_BINARY_DIR}/state-subagents")
add_test(NAME sdk.consumer.subagent_seed
  COMMAND lubancore_consumer subagent-seed "${CMAKE_CURRENT_BINARY_DIR}/state-subagent-restart")
add_test(NAME sdk.consumer.subagent_resume
  COMMAND lubancore_consumer subagent-resume "${CMAKE_CURRENT_BINARY_DIR}/state-subagent-restart")
add_test(NAME sdk.consumer.result_seed
  COMMAND lubancore_consumer result-seed "${CMAKE_CURRENT_BINARY_DIR}/state-result-restart")
add_test(NAME sdk.consumer.result_resume
  COMMAND lubancore_consumer result-resume "${CMAKE_CURRENT_BINARY_DIR}/state-result-restart")
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
set_tests_properties(sdk.consumer.result_seed PROPERTIES FIXTURES_SETUP sdk_result_restart)
set_tests_properties(sdk.consumer.result_resume PROPERTIES FIXTURES_REQUIRED sdk_result_restart)
set_tests_properties(sdk.consumer.skills_seed PROPERTIES FIXTURES_SETUP sdk_skills_restart)
set_tests_properties(sdk.consumer.skills_resume PROPERTIES FIXTURES_REQUIRED sdk_skills_restart)
set_tests_properties(sdk.consumer.memory_seed PROPERTIES FIXTURES_SETUP sdk_memory_restart)
set_tests_properties(sdk.consumer.memory_resume PROPERTIES FIXTURES_REQUIRED sdk_memory_restart)
set_tests_properties(sdk.consumer.memory_save_seed PROPERTIES FIXTURES_SETUP sdk_memory_save_restart)
set_tests_properties(sdk.consumer.memory_save_resume PROPERTIES FIXTURES_REQUIRED sdk_memory_save_restart)
set_tests_properties(sdk.consumer.subagent_seed PROPERTIES FIXTURES_SETUP sdk_child_restart)
set_tests_properties(sdk.consumer.subagent_resume PROPERTIES FIXTURES_REQUIRED sdk_child_restart)
set_tests_properties(sdk.consumer.authorization sdk.consumer.packages sdk.consumer.package_inventory sdk.consumer.builtin_search sdk.consumer.smoke sdk.consumer.isolation sdk.consumer.extensions sdk.consumer.actions sdk.consumer.event_sink sdk.consumer.memory_blobs sdk.consumer.results sdk.consumer.seed sdk.consumer.resume
  sdk.consumer.recovery_seed sdk.consumer.recovery_resume sdk.consumer.result_seed sdk.consumer.result_resume
  sdk.consumer.skills_seed sdk.consumer.skills_resume sdk.consumer.memory_seed sdk.consumer.memory_resume
  sdk.consumer.memory_save_seed sdk.consumer.memory_save_resume
  sdk.consumer.subagents sdk.consumer.subagent_seed sdk.consumer.subagent_resume PROPERTIES TIMEOUT 120)
