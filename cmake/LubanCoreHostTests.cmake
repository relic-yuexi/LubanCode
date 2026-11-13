# Host parity is a separate graph. Never introduce AppServer into SDK-only tests.
if(NOT LUBANCODE_BUILD_CLI OR NOT LUBANCODE_BUILD_SDK)
  message(FATAL_ERROR "LubanCoreHostTests requires both CLI and SDK")
endif()
set(_lubancore_host_root "${CMAKE_SOURCE_DIR}/tests")
set(_lubancore_host_test_entries
  "integration/app_server/test_app_server_session_parity.cpp|integration.app_server.app_server_session_parity"
  "unit/app_server/test_session_assembly.cpp|unit.app_server.session_assembly"
  "unit/app_server/test_plugin_assembly.cpp|unit.app_server.plugin_assembly"
  "unit/app/test_mcp_host_policy.cpp|unit.app.mcp_host_policy"
  "unit/runtime/test_tool_runtime.cpp|unit.runtime.tool_runtime"
  "integration/plugins/test_tool_runtime_deferral.cpp|integration.plugins.tool_runtime_deferral"
  "unit/app/test_turn_runner_scoped_bindings.cpp|unit.app.turn_runner_scoped_bindings")
set(_lubancore_host_sources)
foreach(entry IN LISTS _lubancore_host_test_entries)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 source)
  list(APPEND _lubancore_host_sources "${_lubancore_host_root}/${source}")
endforeach()
add_executable(lubancore_host_tests EXCLUDE_FROM_ALL
  "${_lubancore_host_root}/support/main.cpp"
  "${_lubancore_host_root}/support/fake_http_server.cpp"
  ${_lubancore_host_sources})
target_link_libraries(lubancore_host_tests PRIVATE lubancode_app doctest::doctest)
target_include_directories(lubancore_host_tests PRIVATE "${_lubancore_host_root}/support")
target_compile_definitions(lubancore_host_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${_lubancore_host_root}/fixtures")
target_compile_features(lubancore_host_tests PRIVATE cxx_std_23)
target_precompile_headers(lubancore_host_tests PRIVATE "${_lubancore_host_root}/support/pch.hpp")
set_source_files_properties("${_lubancore_host_root}/support/main.cpp"
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_target_properties(lubancore_host_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
  target_link_libraries(lubancore_host_tests PRIVATE ws2_32)
  # FetchContent's static fallback has no runtime DLLs. A generated loop also
  # preserves spaces in vcpkg paths without issuing a copy command with no input.
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sdk-host-runtime-$<CONFIG>.cmake"
    CONTENT [=[
set(_runtime_dlls [==[$<TARGET_RUNTIME_DLLS:lubancore_host_tests>]==])
foreach(_runtime_dll IN LISTS _runtime_dlls)
  file(COPY "${_runtime_dll}" DESTINATION [==[$<TARGET_FILE_DIR:lubancore_host_tests>]==])
endforeach()
]=])
  add_custom_command(TARGET lubancore_host_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/sdk-host-runtime-$<CONFIG>.cmake"
    VERBATIM)
endif()
if(MSVC)
  target_compile_options(lubancore_host_tests PRIVATE /MP /FS)
endif()
if(NOT LUBANCODE_ASAN_TEST_PROFILE)
  add_dependencies(lubancode_tests lubancore_host_tests)
endif()
foreach(entry IN LISTS _lubancore_host_test_entries)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 source)
  list(GET parts 1 original_test)
  get_filename_component(basename "${source}" NAME)
  string(REGEX REPLACE "^test_(.*)\\.cpp$" "\\1" stem "${basename}")
  set(host_test "sdk.host.${stem}")
  add_test(NAME "${host_test}" COMMAND lubancore_host_tests "--source-file=*${basename}")
  set_tests_properties("${host_test}" PROPERTIES LABELS "sdk-host-parity" TIMEOUT 300
    ENVIRONMENT "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0"
    RESOURCE_LOCK "sdk-host-${stem}")
  if(TEST "${original_test}")
    set_tests_properties("${original_test}" PROPERTIES RESOURCE_LOCK "sdk-host-${stem}")
  endif()
endforeach()
