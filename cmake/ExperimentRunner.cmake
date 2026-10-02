include(GNUInstallDirs)
find_package(Threads REQUIRED)
if(WIN32)
  set(_runner_native src/job_runner/process_win.cpp src/platform/paths_win.cpp)
else()
  set(_runner_native src/job_runner/process_posix.cpp src/platform/paths_posix.cpp)
endif()
add_library(luban_job_runner_client STATIC
  src/job_runner/client.cpp src/job_runner/common.cpp ${_runner_native}
  src/platform/atomic_write.cpp src/platform/secure_file.cpp src/platform/sha256.cpp)
target_include_directories(luban_job_runner_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(luban_job_runner_client PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_compile_features(luban_job_runner_client PUBLIC cxx_std_23)
if(WIN32)
  target_link_libraries(luban_job_runner_client PRIVATE advapi32 bcrypt)
endif()
add_executable(luban-runner src/job_runner/main.cpp src/job_runner/server.cpp)
target_link_libraries(luban-runner PRIVATE luban_job_runner_client)
install(TARGETS luban-runner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ExperimentRunner)
if(LUBANCODE_BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME runner.process.lifecycle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/runner/test_runner_process.py"
      --runner "$<TARGET_FILE:luban-runner>"
      --report "${CMAKE_BINARY_DIR}/test-evidence/runner-process.json")
  set_tests_properties(runner.process.lifecycle PROPERTIES LABELS "runner-process" TIMEOUT 600)
endif()
