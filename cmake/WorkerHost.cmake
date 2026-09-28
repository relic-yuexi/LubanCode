# This host consumes only the SDK's public target and a private JSON codec.
# It must not link runtime/core/app or include their internal source directories.
add_executable(luban-worker src/worker_host/main.cpp src/worker_host/host.cpp)
target_link_libraries(luban-worker PRIVATE LubanCore::Core nlohmann_json::nlohmann_json)
target_compile_features(luban-worker PRIVATE cxx_std_23)
if(APPLE)
  set_target_properties(luban-worker PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
  set_target_properties(luban-worker PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS luban-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT WorkerHost)
install(FILES docs/development/worker-host.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/lubancore COMPONENT WorkerHost)
if(LUBANCODE_BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME worker.process.lifecycle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/worker_host/test_worker_process.py"
    --worker "$<TARGET_FILE:luban-worker>"
    --resource-root "${CMAKE_CURRENT_SOURCE_DIR}/docs/development")
  set_tests_properties(worker.process.lifecycle PROPERTIES LABELS "worker-process" TIMEOUT 300)
endif()
