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
  "${_lubancore_tests_root}/unit/api/test_usage_numeric_allocations.cpp"
  "${_lubancore_tests_root}/unit/packages/test_package_manifest.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_coordinator.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_start_transaction.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_hold_recovery.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_owned_registration.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_owned_adoption.cpp"
  "${_lubancore_tests_root}/unit/tools/test_tool_job_post_live_invocation.cpp"
  "${_lubancore_tests_root}/unit/tools/test_run_command_execution_limits.cpp"
  "${_lubancore_tests_root}/unit/trajectory/test_session_recovery_view.cpp"
  "${_lubancore_tests_root}/unit/trajectory/test_journal_native_receipts.cpp"
  "${_lubancore_tests_root}/unit/trajectory/test_managed_session_ownership.cpp"
  "${_lubancore_tests_root}/unit/trajectory/test_managed_session_reservation.cpp"
  "${_lubancore_tests_root}/unit/memory/test_memory_project_commit_handoff.cpp"
  "${_lubancore_tests_root}/unit/trajectory_v3/test_v3_journal_receipts.cpp"
  "${_lubancore_tests_root}/unit/trajectory_v3/test_v3_journal_owner.cpp"
  "${_lubancore_tests_root}/unit/platform/test_atomic_write.cpp"
  "${_lubancore_tests_root}/unit/tools/test_lua_protected.cpp"
  "${_lubancore_tests_root}/unit/trajectory_v3/test_v3_result_store.cpp"
  "${_lubancore_tests_root}/unit/trajectory_v3/test_v3_result_immutable_publication.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_session_resources.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_session_execution.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_execution_owner.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_subagent_terminal_receipt.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_child_foreground_integration.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_child_parent_observation.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_child_history_adoption.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_scoped_turn_bindings.cpp")
list(APPEND LUBANCORE_FOCUSED_TEST_SOURCES
  "${_lubancore_tests_root}/unit/runtime/test_owned_job_admission.cpp"
  "${_lubancore_tests_root}/unit/runtime/test_middleware_native_receipts.cpp"
  "${_lubancore_tests_root}/unit/hooks/test_middleware_dispatch_cause.cpp"
  "${_lubancore_tests_root}/unit/hooks/test_middleware_job_post_contract.cpp")
if(NOT LUBANCORE_WITH_LUA)
  list(FILTER LUBANCORE_FOCUSED_TEST_SOURCES EXCLUDE REGEX "/test_(lubancore_lua|lua_protected)\\.cpp$")
endif()
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
  # The internal turn-binding fixture calls the same producer/strict reader;
  # it grants no extra shared-library ABI or alternate execution stack.
  "${CMAKE_SOURCE_DIR}/src/sdk/operation_ledger.cpp"
  # The same private Job producer is used by reference fixtures, not exported.
  "${CMAKE_SOURCE_DIR}/src/sdk/job_operations.cpp"
  "${CMAKE_SOURCE_DIR}/src/sdk/adapters.cpp"
  "${CMAKE_SOURCE_DIR}/src/sdk/command_jobs.cpp"
  "${CMAKE_SOURCE_DIR}/src/sdk/command_jobs_opening.cpp"
  # The private storage guards exercise the real adapter without exporting it
  # from the shared SDK. Public Session fixtures still call the shared library.
  "${CMAKE_SOURCE_DIR}/src/sdk/named_results.cpp"
  # The public-only child acceptance source is also built after relocation. It
  # belongs to these fixture executables, never the SDK library closure.
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/subagents.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/lua_build_profile.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/actions.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/todo_write.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/agentic_rag.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/web_fetch.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/web_search.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/command_jobs.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/named_results.cpp"
  "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/journal_owner.cpp"
  ${LUBANCORE_FOCUSED_TEST_SOURCES})
if(LUBANCORE_WITH_LUA)
  target_sources(lubancore_sdk_tests PRIVATE "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/lua.cpp")
endif()
target_link_libraries(lubancore_sdk_tests PRIVATE
  lubancode_runtime lubancore_sdk doctest::doctest)
target_include_directories(lubancore_sdk_tests PRIVATE "${_lubancore_tests_root}/support")
target_compile_definitions(lubancore_sdk_tests PRIVATE
  LUBANCORE_CONSUMER_WITH_LUA=$<BOOL:${LUBANCORE_WITH_LUA}>
  LUBANCORE_TEST_JOB_POST_SDK=1
  LUBANCORE_TEST_USAGE_NUMERIC_FAULT_PROBE="$<TARGET_FILE:lubancore_usage_numeric_fault_probe>"
  LUBANCODE_TEST_FIXTURES_DIR="${_lubancore_tests_root}/fixtures"
  LUBANCORE_TEST_SEARCH_PROBE="$<TARGET_FILE:lubancore_sdk_search_probe>"
  LUBANCORE_TEST_COMMAND_LIMITS_PROBE="$<TARGET_FILE:lubancore_command_limits_probe>")
add_dependencies(lubancore_sdk_tests lubancore_sdk_search_probe lubancore_command_limits_probe)
add_dependencies(lubancore_sdk_tests lubancore_usage_numeric_fault_probe)
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
  if(LUBANCODE_ASAN_TEST_PROFILE)
    add_dependencies(lubancode_tests lubancore_sdk_search_probe)
  else()
    add_dependencies(lubancode_tests lubancore_sdk_tests)
  endif()
  target_compile_definitions(lubancode_tests PRIVATE
    LUBANCORE_TEST_JOB_POST_SDK=1
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
  if(sdk_basename STREQUAL "test_lubancore_web_fetch.cpp" OR sdk_basename STREQUAL "test_lubancore_web_search.cpp")
    set_property(TEST "${sdk_test}" APPEND PROPERTY ENVIRONMENT "NO_PROXY=127.0.0.1" "no_proxy=127.0.0.1")
  endif()
  # SDK-only and combined builds keep this fixed-duration shell fixture isolated
  # from other CTest processes, while its own four concurrent contexts still run.
  if(WIN32 AND sdk_basename STREQUAL "test_run_command_execution_limits.cpp")
    set_tests_properties("${sdk_test}" PROPERTIES RUN_SERIAL TRUE)
  endif()
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
  elseif(sdk_basename STREQUAL "test_memory_project_commit_handoff.cpp")
    set(sdk_original_test "unit.memory.memory_project_commit_handoff")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "memory-project-handoff")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "memory-project-handoff")
    endif()
  elseif(sdk_basename STREQUAL "test_managed_session_ownership.cpp" OR
         sdk_basename STREQUAL "test_managed_session_reservation.cpp")
    set(sdk_original_test "unit.trajectory.${sdk_stem}")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "managed-session-opening")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "managed-session-opening")
    endif()
  elseif(sdk_basename STREQUAL "test_v3_result_store.cpp")
    set(sdk_original_test "unit.trajectory_v3.v3_result_store")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "trajectory-v3-result-store")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "trajectory-v3-result-store")
    endif()
  elseif(sdk_basename STREQUAL "test_v3_result_immutable_publication.cpp")
    set(sdk_original_test "unit.trajectory_v3.v3_result_immutable_publication")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "platform-atomic-write")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "platform-atomic-write")
    endif()
  elseif(sdk_basename STREQUAL "test_lubancore_named_result_guards.cpp")
    set(sdk_original_test "integration.sdk.lubancore_named_result_guards")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "platform-atomic-write")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "platform-atomic-write")
    endif()
  elseif(sdk_basename STREQUAL "test_v3_journal_receipts.cpp")
    set(sdk_original_test "unit.trajectory_v3.v3_journal_receipts")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "trajectory-v3-journal-receipts")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "trajectory-v3-journal-receipts")
    endif()
  elseif(sdk_basename STREQUAL "test_lubancore_owned_job_deadline.cpp")
    set(sdk_original_test "integration.sdk.lubancore_owned_job_deadline")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "sdk-owned-job-deadline")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "sdk-owned-job-deadline")
    endif()
  elseif(sdk_basename STREQUAL "test_lubancore_job_operations.cpp")
    set(sdk_original_test "integration.sdk.lubancore_job_operations")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "sdk-job-operations")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "sdk-job-operations")
    endif()
  elseif(sdk_basename STREQUAL "test_lubancore_package_inventory.cpp")
    set(sdk_original_test "integration.sdk.lubancore_package_inventory")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "sdk-package-inventory")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "sdk-package-inventory")
    endif()
  elseif(sdk_basename STREQUAL "test_lubancore_lua_build_profile.cpp")
    set(sdk_original_test "integration.sdk.lubancore_lua_build_profile")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "sdk-lua-build-profile")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "sdk-lua-build-profile")
    endif()
  elseif(sdk_basename STREQUAL "test_lua_protected.cpp")
    set(sdk_original_test "unit.tools.lua_protected")
  elseif(sdk_basename STREQUAL "test_tool_job_coordinator.cpp" OR
         sdk_basename STREQUAL "test_tool_job_start_transaction.cpp" OR
         sdk_basename STREQUAL "test_tool_job_hold_recovery.cpp" OR
         sdk_basename STREQUAL "test_tool_job_owned_registration.cpp" OR
         sdk_basename STREQUAL "test_tool_job_owned_adoption.cpp" OR
         sdk_basename STREQUAL "test_tool_job_post_live_invocation.cpp" OR
         sdk_basename STREQUAL "test_run_command_execution_limits.cpp")
    set(sdk_original_test "unit.tools.${sdk_stem}")
    set_tests_properties("${sdk_test}" PROPERTIES RESOURCE_LOCK "tools-${sdk_stem}")
    if(TEST "${sdk_original_test}")
      set_tests_properties("${sdk_original_test}" PROPERTIES RESOURCE_LOCK "tools-${sdk_stem}")
    endif()
  elseif(sdk_basename STREQUAL "test_middleware_dispatch_cause.cpp" OR
         sdk_basename STREQUAL "test_middleware_job_post_contract.cpp")
    set(sdk_original_test "unit.hooks.${sdk_stem}")
  elseif(sdk_basename STREQUAL "test_session_resources.cpp" OR
         sdk_basename STREQUAL "test_session_execution.cpp" OR
         sdk_basename STREQUAL "test_execution_owner.cpp" OR
         sdk_basename STREQUAL "test_subagent_terminal_receipt.cpp" OR
         sdk_basename STREQUAL "test_child_foreground_integration.cpp" OR
         sdk_basename STREQUAL "test_child_parent_observation.cpp" OR
         sdk_basename STREQUAL "test_child_history_adoption.cpp" OR
         sdk_basename STREQUAL "test_owned_job_admission.cpp" OR
         sdk_basename STREQUAL "test_middleware_native_receipts.cpp" OR
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
