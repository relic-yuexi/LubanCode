cmake_minimum_required(VERSION 3.25)
project(LubanCoreCommandJobsProbe LANGUAGES CXX)

# A private CI executable, outside both SDK producer and installed consumer.
# No SDK dependency, package lookup, install rule or product build target.
set(CMAKE_SUPPRESS_REGENERATION ON)
add_executable(lubancore_command_limits_probe tests/support/command_limits_probe.cpp)
target_compile_features(lubancore_command_limits_probe PRIVATE cxx_std_23)
set_target_properties(lubancore_command_limits_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
  target_compile_options(lubancore_command_limits_probe PRIVATE /utf-8 /permissive-)
  set_property(TARGET lubancore_command_limits_probe PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
