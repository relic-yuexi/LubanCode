# This graph has no CLI executable, CLI resources or host test dependency.
# Combined builds reuse exactly the same source files and per-file CTests.
if(NOT LUBANCODE_BUILD_SDK)
  message(FATAL_ERROR "LubanCoreTests requires LUBANCODE_BUILD_SDK=ON")
endif()
set(_lubancore_tests_root "${CMAKE_SOURCE_DIR}/tests")
# This private child-process fixture has only standard/platform dependencies,
# is never installed, and cannot enter the SDK library's dependency closure.
add_executable(lubancore_sdk_search_probe
  "${_lubancore_tests_root}/support/sdk_search_probe.cpp")
target_compile_features(lubancore_sdk_search_probe PRIVATE cxx_std_23)
set_target_properties(lubancore_sdk_search_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
  set_property(TARGET lubancore_sdk_search_probe PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
file(GLOB LUBANCORE_FOCUSED_TEST_SOURCES CONFIGURE_DEPENDS
  "${_lubancore_tests_root}/integration/sdk/test_*.cpp")
list(SORT LUBANCORE_FOCUSED_TEST_SOURCES)
if(NOT LUBANCORE_FOCUSED_TEST_SOURCES)
  message(FATAL_ERROR "SDK build requires registered integration/sdk tests")
endif()
list(APPEND LUBANCORE_FOCUSED_TEST_SOURCES
  "${_lubancore_tests_root}/unit/packages/test_package_manifest.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_coordinator.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_start_transaction.cpp"
  "${_lubancore_tests_root}/unit/trajectory/test_session_recovery_view.cpp"
  "${_lubancore_tests_root}/unit/platform/test_atomic_write.cpp"
  "${_lubancore_tests_root}/unit/tools/test_lua_protected.cpp"
  "${_lubancore_tests_root}/unit/trajectory_v3/test_v3_result_store.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_session_resources.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_session_execution.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_execution_owner.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_subagent_terminal_receipt.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_child_foreground_integration.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_child_parent_observation.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_child_history_adoption.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_scoped_turn_bindings.cpp")
set(_lubancore_tests_exclude)
if(LUBANCODE_BUILD_CLI)
  set(_lubancore_tests_exclude EXCLUDE_FROM_ALL)
endif()
add_executable(lubancore_sdk_tests ${_lubancore_tests_exclude}
  "${_lubancore_tests_root}/support/main.cpp"
  "${_lubancore_tests_root}/support/fake_http_server.cpp"
  # The result-component fixtures call the production reader without exporting
  # its private symbols from the SDK DLL. Session and installed-host cases call
  # the SDK's hidden reader through the public Session API.
  "${CMAKE_SOURCE_DIR}/src/sdk/results.cpp"
  # The private pending implementation is compiled into the reference fixture;
  # its symbols stay hidden in the SDK DLL. Public Session cases use the DLL.
  "${CMAKE_SOURCE_DIR}/src/sdk/approval.cpp"
  # The CAS fixture uses the real SDK Memory opening/report module privately.
  "${CMAKE_SOURCE_DIR}/src/sdk/memory.cpp"
  # Real private Action host adapters exercise native sink receipts without
  # exporting new private symbols or adding a public writer-fault option.
  "${CMAKE_SOURCE_DIR}/src/sdk/action_dispatch.cpp"
  # The public-only child acceptance source is also built after relocation. It
  # belongs to these fixture executables, never the SDK library closure.
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/subagents.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/lua.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/actions.cpp"
  ${LUBANCORE_FOCUSED_TEST_SOURCES})
target_link_libraries(lubancore_sdk_tests PRIVATE
  lubancode_runtime lubancore_sdk doctest::doctest)
target_include_directories(lubancore_sdk_tests PRIVATE "${_lubancore_tests_root}/support")
target_compile_definitions(lubancore_sdk_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${_lubancore_tests_root}/fixtures"
  LUBANCORE_TEST_SEARCH_PROBE="$<TARGET_FILE:lubancore_sdk_search_probe>")
add_dependencies(lubancore_sdk_tests lubancore_sdk_search_probe)
target_compile_features(lubancore_sdk_tests PRIVATE cxx_std_23)
target_precompile_headers(lubancore_sdk_tests PRIVATE "${_lubancore_tests_root}/support/pch.hpp")
set_source_files_properties("${_lubancore_tests_root}/support/main.cpp"
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# MSVC appends Release/Debug, beside the SDK DLL and its app-local dependencies.
set_target_properties(lubancore_sdk_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
  target_link_libraries(lubancore_sdk_tests PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(lubancore_sdk_tests PRIVATE /MP /FS)
endif()
if(TARGET lubancode_tests)
  add_dependencies(lubancode_tests lubancore_sdk_tests)
  target_compile_definitions(lubancode_tests PRIVATE
    LUBANCORE_TEST_SEARCH_PROBE="$<TARGET_FILE:lubancore_sdk_search_probe>")
endif()
foreach(sdk_source IN LISTS LUBANCORE_FOCUSED_TEST_SOURCES)
  get_filename_component(sdk_basename "${sdk_source}" NAME)
  if(NOT sdk_basename MATCHES "^test_([A-Za-z0-9_]+)\\.cpp$")
    message(FATAL_ERROR "Invalid SDK test filename: ${sdk_basename}")
  endif()
  set(sdk_stem "${CMAKE_MATCH_1}")
  set(sdk_test "sdk.focused.${sdk_stem}")
  add_test(NAME "${sdk_test}"
    COMMAND lubancore_sdk_tests "--source-file=*${sdk_basename}")
  set_tests_properties("${sdk_test}" PROPERTIES
    LABELS "sdk-focused" TIMEOUT 300
    ENVIRONMENT "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0")
  if(sdk_basename STREQUAL "test_atomic_write.cpp")
    set(sdk_original_test "unit.platform.atomic_write")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "platform-atomic-write")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "platform-atomic-write")
    endif()
  elseif(sdk_basename STREQUAL "test_package_manifest.cpp")
    set(sdk_original_test "unit.packages.package_manifest")
  elseif(sdk_basename STREQUAL "test_session_recovery_view.cpp")
    set(sdk_original_test "unit.trajectory.session_recovery_view")
  elseif(sdk_basename STREQUAL "test_v3_result_store.cpp")
    set(sdk_original_test "unit.trajectory_v3.v3_result_store")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "trajectory-v3-result-store")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "trajectory-v3-result-store")
    endif()
  elseif(sdk_basename STREQUAL "test_lua_protected.cpp")
    set(sdk_original_test "unit.tools.lua_protected")
  elseif(sdk_basename STREQUAL "test_tool_job_coordinator.cpp" OR
         sdk_basename STREQUAL "test_tool_job_start_transaction.cpp")
    set(sdk_original_test "unit.tools.${sdk_stem}")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "tools-${sdk_stem}")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "tools-${sdk_stem}")
    endif()
  elseif(sdk_basename STREQUAL "test_session_resources.cpp" OR
         sdk_basename STREQUAL "test_session_execution.cpp" OR
         sdk_basename STREQUAL "test_execution_owner.cpp" OR
         sdk_basename STREQUAL "test_subagent_terminal_receipt.cpp" OR
         sdk_basename STREQUAL "test_child_foreground_integration.cpp" OR
         sdk_basename STREQUAL "test_child_parent_observation.cpp" OR
         sdk_basename STREQUAL "test_child_history_adoption.cpp" OR
         sdk_basename STREQUAL "test_scoped_turn_bindings.cpp")
    set(sdk_original_test "unit.runtime.${sdk_stem}")
  else()
    set(sdk_original_test "integration.sdk.${sdk_stem}")
  endif()
  if(TEST "${sdk_original_test}")
    get_test_property("${sdk_original_test}" ENVIRONMENT sdk_test_environment)
    if(NOT sdk_test_environment STREQUAL "NOTFOUND")
      set_tests_properties("${sdk_test}" PROPERTIES ENVIRONMENT "${sdk_test_environment}")
    endif()
  endif()
endforeach()
