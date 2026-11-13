cmake_minimum_required(VERSION 3.24)
project(lubancore_rag_example LANGUAGES CXX)

# Copy this directory plus sdk-consumer/agentic_rag.cpp into a standalone folder.
# This project never reaches back into the repository or build tree for sources.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/agentic_rag.cpp")
  message(FATAL_ERROR "Copy examples/sdk-consumer/agentic_rag.cpp beside this CMakeLists.txt first")
endif()
find_package(LubanCore CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_executable(lubancore_rag main.cpp agentic_rag.cpp)
target_compile_features(lubancore_rag PRIVATE cxx_std_23)
target_link_libraries(lubancore_rag PRIVATE LubanCore::Core Threads::Threads)
if(MSVC)
  target_compile_options(lubancore_rag PRIVATE /utf-8 /W4)
endif()
if(WIN32)
  add_custom_command(TARGET lubancore_rag POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:LubanCore::Core>" "$<TARGET_FILE_DIR:lubancore_rag>"
    VERBATIM)
endif()
